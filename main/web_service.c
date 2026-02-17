#include "web_service.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "mbedtls/base64.h"

#include "buzzer.h"
#include "keymap_config.h"
#include "sdkconfig.h"
#include "wifi_portal.h"

#define TAG "WEB_SERVICE"

#define WEB_SERVICE_BODY_MAX 192
#define WEB_SERVICE_JSON_BUF 640
#define WEB_SERVICE_RETRY_MS 2000
#define WEB_SERVICE_HEADER_MAX 256
#define WEB_SERVICE_BASIC_EXPECTED_MAX 320

typedef struct {
    bool valid;
    uint8_t key_index;
    bool pressed;
    uint16_t usage;
    TickType_t tick;
    char name[32];
} web_service_key_event_t;

typedef struct {
    bool valid;
    int32_t steps;
    uint16_t usage;
    TickType_t tick;
} web_service_encoder_event_t;

typedef struct {
    bool valid;
    uint8_t layer_index;
    bool left_to_right;
    uint16_t usage;
    TickType_t tick;
} web_service_swipe_event_t;

typedef struct {
    bool initialized;
    bool running;
    httpd_handle_t server;
    SemaphoreHandle_t lock;
    TickType_t boot_tick;
    TickType_t last_activity_tick;
    TickType_t next_start_retry_tick;
    uint8_t active_layer;
    web_service_key_event_t last_key;
    web_service_encoder_event_t last_encoder;
    web_service_swipe_event_t last_swipe;
    web_service_control_if_t control;
    bool control_registered;
    bool auth_api_key_enabled;
    bool auth_basic_enabled;
    char api_key[WEB_SERVICE_HEADER_MAX];
    char basic_auth_expected[WEB_SERVICE_BASIC_EXPECTED_MAX];
} web_service_state_t;

static web_service_state_t s_ws = {0};

static inline void web_service_lock(void)
{
    if (s_ws.lock != NULL) {
        (void)xSemaphoreTake(s_ws.lock, portMAX_DELAY);
    }
}

static inline void web_service_unlock(void)
{
    if (s_ws.lock != NULL) {
        (void)xSemaphoreGive(s_ws.lock);
    }
}

static void json_escape_copy(char *dst, size_t dst_size, const char *src)
{
    size_t w = 0;
    if (dst == NULL || dst_size == 0U) {
        return;
    }
    dst[0] = '\0';
    if (src == NULL) {
        return;
    }

    for (size_t i = 0; src[i] != '\0'; ++i) {
        const unsigned char c = (unsigned char)src[i];
        const char *rep = NULL;
        switch (c) {
        case '\\':
            rep = "\\\\";
            break;
        case '"':
            rep = "\\\"";
            break;
        case '\b':
            rep = "\\b";
            break;
        case '\f':
            rep = "\\f";
            break;
        case '\n':
            rep = "\\n";
            break;
        case '\r':
            rep = "\\r";
            break;
        case '\t':
            rep = "\\t";
            break;
        default:
            break;
        }

        if (rep != NULL) {
            const size_t rep_len = strlen(rep);
            if ((w + rep_len) >= dst_size) {
                break;
            }
            memcpy(dst + w, rep, rep_len);
            w += rep_len;
            continue;
        }

        if (c < 0x20U) {
            if ((w + 1U) >= dst_size) {
                break;
            }
            dst[w++] = '?';
            continue;
        }

        if ((w + 1U) >= dst_size) {
            break;
        }
        dst[w++] = (char)c;
    }
    dst[w] = '\0';
}

static bool parse_json_int(const char *json, const char *key, int *out)
{
    if (json == NULL || key == NULL || out == NULL) {
        return false;
    }

    char token[48] = {0};
    (void)snprintf(token, sizeof(token), "\"%s\"", key);
    const char *p = strstr(json, token);
    if (p == NULL) {
        return false;
    }
    p = strchr(p, ':');
    if (p == NULL) {
        return false;
    }
    ++p;
    while (*p != '\0' && isspace((int)(unsigned char)*p)) {
        ++p;
    }

    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p) {
        return false;
    }
    *out = (int)v;
    return true;
}

static bool parse_json_bool(const char *json, const char *key, bool *out)
{
    if (json == NULL || key == NULL || out == NULL) {
        return false;
    }

    char token[48] = {0};
    (void)snprintf(token, sizeof(token), "\"%s\"", key);
    const char *p = strstr(json, token);
    if (p == NULL) {
        return false;
    }
    p = strchr(p, ':');
    if (p == NULL) {
        return false;
    }
    ++p;
    while (*p != '\0' && isspace((int)(unsigned char)*p)) {
        ++p;
    }

    if (strncmp(p, "true", 4) == 0) {
        *out = true;
        return true;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out = false;
        return true;
    }
    if (*p == '1') {
        *out = true;
        return true;
    }
    if (*p == '0') {
        *out = false;
        return true;
    }
    return false;
}

static esp_err_t http_send_json(httpd_req_t *req, const char *status, const char *json)
{
    if (req == NULL || status == NULL || json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (MACRO_WEB_SERVICE_CORS_ENABLED) {
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type,Authorization,X-API-Key");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET,POST,OPTIONS");
    }
    return httpd_resp_sendstr(req, json);
}

static esp_err_t http_send_options_ok(httpd_req_t *req)
{
    if (req == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    httpd_resp_set_status(req, "204 No Content");
    if (MACRO_WEB_SERVICE_CORS_ENABLED) {
        httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type,Authorization,X-API-Key");
        httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "GET,POST,OPTIONS");
    }
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t http_read_body(httpd_req_t *req, char *buf, size_t buf_size)
{
    if (req == NULL || buf == NULL || buf_size == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (req->content_len <= 0) {
        buf[0] = '\0';
        return ESP_OK;
    }
    if ((size_t)req->content_len >= buf_size) {
        return ESP_ERR_INVALID_SIZE;
    }

    int total = 0;
    while (total < req->content_len) {
        const int received = httpd_req_recv(req, buf + total, req->content_len - total);
        if (received <= 0) {
            return ESP_FAIL;
        }
        total += received;
    }
    buf[total] = '\0';
    return ESP_OK;
}

static bool http_get_header_copy(httpd_req_t *req, const char *name, char *out, size_t out_size)
{
    if (req == NULL || name == NULL || out == NULL || out_size == 0U) {
        return false;
    }
    out[0] = '\0';

    const size_t len = httpd_req_get_hdr_value_len(req, name);
    if (len == 0U || len >= out_size) {
        return false;
    }
    return httpd_req_get_hdr_value_str(req, name, out, out_size) == ESP_OK;
}

static bool web_auth_api_key_match(httpd_req_t *req)
{
    char header_value[WEB_SERVICE_HEADER_MAX] = {0};
    if (!http_get_header_copy(req, "X-API-Key", header_value, sizeof(header_value))) {
        return false;
    }
    return strcmp(header_value, s_ws.api_key) == 0;
}

static bool web_auth_basic_match(httpd_req_t *req)
{
    char auth_header[WEB_SERVICE_HEADER_MAX] = {0};
    if (!http_get_header_copy(req, "Authorization", auth_header, sizeof(auth_header))) {
        return false;
    }
    return strcmp(auth_header, s_ws.basic_auth_expected) == 0;
}

static esp_err_t web_auth_guard(httpd_req_t *req)
{
    bool api_key_enabled = false;
    bool basic_enabled = false;
    web_service_lock();
    api_key_enabled = s_ws.auth_api_key_enabled;
    basic_enabled = s_ws.auth_basic_enabled;
    web_service_unlock();

    if (!api_key_enabled && !basic_enabled) {
        return ESP_OK;
    }

    bool authorized = false;
    if (api_key_enabled && web_auth_api_key_match(req)) {
        authorized = true;
    }
    if (basic_enabled && web_auth_basic_match(req)) {
        authorized = true;
    }
    if (authorized) {
        return ESP_OK;
    }

    if (basic_enabled) {
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ESP32 MacroPad\"");
    }
    return http_send_json(req, "401 Unauthorized", "{\"ok\":false,\"error\":\"unauthorized\"}");
}

static esp_err_t health_get_handler(httpd_req_t *req)
{
    esp_err_t auth = web_auth_guard(req);
    if (auth != ESP_OK) {
        return auth;
    }

    char json[WEB_SERVICE_JSON_BUF] = {0};
    const uint32_t uptime_ms = (uint32_t)(pdTICKS_TO_MS(xTaskGetTickCount() - s_ws.boot_tick));
    const bool wifi_connected = wifi_portal_is_connected();
    const bool portal_active = wifi_portal_is_active();
    const int n = snprintf(json,
                           sizeof(json),
                           "{\"ok\":true,\"service\":\"macropad-web\",\"uptime_ms\":%" PRIu32
                           ",\"wifi_connected\":%s,\"portal_active\":%s,"
                           "\"control_enabled\":%s,\"running\":%s}",
                           uptime_ms,
                           wifi_connected ? "true" : "false",
                           portal_active ? "true" : "false",
                           MACRO_WEB_SERVICE_CONTROL_ENABLED ? "true" : "false",
                           s_ws.running ? "true" : "false");
    if (n <= 0 || (size_t)n >= sizeof(json)) {
        return http_send_json(req, "500 Internal Server Error", "{\"ok\":false,\"error\":\"encode\"}");
    }
    return http_send_json(req, "200 OK", json);
}

static esp_err_t options_handler(httpd_req_t *req)
{
    return http_send_options_ok(req);
}

static esp_err_t state_get_handler(httpd_req_t *req)
{
    esp_err_t auth = web_auth_guard(req);
    if (auth != ESP_OK) {
        return auth;
    }

    char json[WEB_SERVICE_JSON_BUF] = {0};
    char key_name_json[96] = {0};

    web_service_lock();
    const uint8_t active_layer = s_ws.active_layer;
    const TickType_t now = xTaskGetTickCount();
    const TickType_t activity_tick = s_ws.last_activity_tick;
    const web_service_key_event_t key_event = s_ws.last_key;
    const web_service_encoder_event_t encoder_event = s_ws.last_encoder;
    const web_service_swipe_event_t swipe_event = s_ws.last_swipe;
    web_service_unlock();

    json_escape_copy(key_name_json, sizeof(key_name_json), key_event.name);
    const uint32_t idle_ms = (uint32_t)pdTICKS_TO_MS(now - activity_tick);
    const uint32_t key_age_ms = key_event.valid ? (uint32_t)pdTICKS_TO_MS(now - key_event.tick) : 0U;
    const uint32_t encoder_age_ms = encoder_event.valid ? (uint32_t)pdTICKS_TO_MS(now - encoder_event.tick) : 0U;
    const uint32_t swipe_age_ms = swipe_event.valid ? (uint32_t)pdTICKS_TO_MS(now - swipe_event.tick) : 0U;

    const int n = snprintf(
        json,
        sizeof(json),
        "{\"ok\":true,"
        "\"layer_index\":%u,\"layer\":%u,"
        "\"buzzer_enabled\":%s,"
        "\"idle_ms\":%" PRIu32 ","
        "\"last_key\":{\"valid\":%s,\"index\":%u,\"pressed\":%s,\"usage\":%u,\"name\":\"%s\",\"age_ms\":%" PRIu32 "},"
        "\"last_encoder\":{\"valid\":%s,\"steps\":%" PRId32 ",\"usage\":%u,\"age_ms\":%" PRIu32 "},"
        "\"last_swipe\":{\"valid\":%s,\"layer_index\":%u,\"left_to_right\":%s,\"usage\":%u,\"age_ms\":%" PRIu32 "}}",
        (unsigned)active_layer,
        (unsigned)active_layer + 1U,
        buzzer_is_enabled() ? "true" : "false",
        idle_ms,
        key_event.valid ? "true" : "false",
        (unsigned)key_event.key_index,
        key_event.pressed ? "true" : "false",
        (unsigned)key_event.usage,
        key_name_json,
        key_age_ms,
        encoder_event.valid ? "true" : "false",
        encoder_event.steps,
        (unsigned)encoder_event.usage,
        encoder_age_ms,
        swipe_event.valid ? "true" : "false",
        (unsigned)swipe_event.layer_index,
        swipe_event.left_to_right ? "true" : "false",
        (unsigned)swipe_event.usage,
        swipe_age_ms);
    if (n <= 0 || (size_t)n >= sizeof(json)) {
        return http_send_json(req, "500 Internal Server Error", "{\"ok\":false,\"error\":\"encode\"}");
    }
    return http_send_json(req, "200 OK", json);
}

static esp_err_t ensure_control_ready(httpd_req_t *req)
{
    if (!MACRO_WEB_SERVICE_CONTROL_ENABLED) {
        return http_send_json(req, "403 Forbidden",
                              "{\"ok\":false,\"error\":\"control disabled\"}");
    }

    web_service_lock();
    const bool registered = s_ws.control_registered;
    web_service_unlock();
    if (!registered) {
        return http_send_json(req, "503 Service Unavailable",
                              "{\"ok\":false,\"error\":\"control interface missing\"}");
    }
    return ESP_OK;
}

static esp_err_t control_layer_post_handler(httpd_req_t *req)
{
    esp_err_t auth = web_auth_guard(req);
    if (auth != ESP_OK) {
        return auth;
    }

    esp_err_t guard = ensure_control_ready(req);
    if (guard != ESP_OK) {
        return guard;
    }

    char body[WEB_SERVICE_BODY_MAX] = {0};
    if (http_read_body(req, body, sizeof(body)) != ESP_OK) {
        return http_send_json(req, "400 Bad Request",
                              "{\"ok\":false,\"error\":\"invalid body\"}");
    }

    int layer_value = 0;
    if (!parse_json_int(body, "layer", &layer_value)) {
        return http_send_json(req, "400 Bad Request",
                              "{\"ok\":false,\"error\":\"missing layer\"}");
    }
    if (layer_value < 1 || layer_value > MACRO_LAYER_COUNT) {
        return http_send_json(req, "400 Bad Request",
                              "{\"ok\":false,\"error\":\"layer out of range\"}");
    }

    web_service_lock();
    const web_service_set_layer_cb_t set_layer = s_ws.control.set_layer;
    web_service_unlock();
    if (set_layer == NULL) {
        return http_send_json(req, "503 Service Unavailable",
                              "{\"ok\":false,\"error\":\"layer callback missing\"}");
    }

    const uint8_t layer_index = (uint8_t)(layer_value - 1);
    const esp_err_t err = set_layer(layer_index);
    if (err != ESP_OK) {
        return http_send_json(req, "500 Internal Server Error",
                              "{\"ok\":false,\"error\":\"layer apply failed\"}");
    }

    web_service_set_active_layer(layer_index);
    web_service_mark_user_activity();
    return http_send_json(req, "200 OK", "{\"ok\":true}");
}

static esp_err_t control_buzzer_post_handler(httpd_req_t *req)
{
    esp_err_t auth = web_auth_guard(req);
    if (auth != ESP_OK) {
        return auth;
    }

    esp_err_t guard = ensure_control_ready(req);
    if (guard != ESP_OK) {
        return guard;
    }

    char body[WEB_SERVICE_BODY_MAX] = {0};
    if (http_read_body(req, body, sizeof(body)) != ESP_OK) {
        return http_send_json(req, "400 Bad Request",
                              "{\"ok\":false,\"error\":\"invalid body\"}");
    }

    bool enabled = false;
    if (!parse_json_bool(body, "enabled", &enabled)) {
        return http_send_json(req, "400 Bad Request",
                              "{\"ok\":false,\"error\":\"missing enabled\"}");
    }

    web_service_lock();
    const web_service_set_buzzer_cb_t set_buzzer = s_ws.control.set_buzzer;
    web_service_unlock();
    if (set_buzzer == NULL) {
        return http_send_json(req, "503 Service Unavailable",
                              "{\"ok\":false,\"error\":\"buzzer callback missing\"}");
    }

    const esp_err_t err = set_buzzer(enabled);
    if (err != ESP_OK) {
        return http_send_json(req, "500 Internal Server Error",
                              "{\"ok\":false,\"error\":\"buzzer apply failed\"}");
    }

    web_service_mark_user_activity();
    return http_send_json(req, "200 OK", "{\"ok\":true}");
}

static esp_err_t control_consumer_post_handler(httpd_req_t *req)
{
    esp_err_t auth = web_auth_guard(req);
    if (auth != ESP_OK) {
        return auth;
    }

    esp_err_t guard = ensure_control_ready(req);
    if (guard != ESP_OK) {
        return guard;
    }

    char body[WEB_SERVICE_BODY_MAX] = {0};
    if (http_read_body(req, body, sizeof(body)) != ESP_OK) {
        return http_send_json(req, "400 Bad Request",
                              "{\"ok\":false,\"error\":\"invalid body\"}");
    }

    int usage_value = 0;
    if (!parse_json_int(body, "usage", &usage_value)) {
        return http_send_json(req, "400 Bad Request",
                              "{\"ok\":false,\"error\":\"missing usage\"}");
    }
    if (usage_value < 0 || usage_value > 0xFFFF) {
        return http_send_json(req, "400 Bad Request",
                              "{\"ok\":false,\"error\":\"usage out of range\"}");
    }

    web_service_lock();
    const web_service_send_consumer_cb_t send_consumer = s_ws.control.send_consumer;
    web_service_unlock();
    if (send_consumer == NULL) {
        return http_send_json(req, "503 Service Unavailable",
                              "{\"ok\":false,\"error\":\"consumer callback missing\"}");
    }

    const esp_err_t err = send_consumer((uint16_t)usage_value);
    if (err != ESP_OK) {
        return http_send_json(req, "500 Internal Server Error",
                              "{\"ok\":false,\"error\":\"consumer send failed\"}");
    }

    web_service_mark_user_activity();
    return http_send_json(req, "200 OK", "{\"ok\":true}");
}

static esp_err_t register_routes(httpd_handle_t server)
{
    const httpd_uri_t health_get = {
        .uri = "/api/v1/health",
        .method = HTTP_GET,
        .handler = health_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t state_get = {
        .uri = "/api/v1/state",
        .method = HTTP_GET,
        .handler = state_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t layer_post = {
        .uri = "/api/v1/control/layer",
        .method = HTTP_POST,
        .handler = control_layer_post_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t buzzer_post = {
        .uri = "/api/v1/control/buzzer",
        .method = HTTP_POST,
        .handler = control_buzzer_post_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t consumer_post = {
        .uri = "/api/v1/control/consumer",
        .method = HTTP_POST,
        .handler = control_consumer_post_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t health_options = {
        .uri = "/api/v1/health",
        .method = HTTP_OPTIONS,
        .handler = options_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t state_options = {
        .uri = "/api/v1/state",
        .method = HTTP_OPTIONS,
        .handler = options_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t layer_options = {
        .uri = "/api/v1/control/layer",
        .method = HTTP_OPTIONS,
        .handler = options_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t buzzer_options = {
        .uri = "/api/v1/control/buzzer",
        .method = HTTP_OPTIONS,
        .handler = options_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t consumer_options = {
        .uri = "/api/v1/control/consumer",
        .method = HTTP_OPTIONS,
        .handler = options_handler,
        .user_ctx = NULL,
    };

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &health_get), TAG, "register /health failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &state_get), TAG, "register /state failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &layer_post), TAG, "register /control/layer failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &buzzer_post), TAG, "register /control/buzzer failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &consumer_post), TAG, "register /control/consumer failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &health_options), TAG, "register /health options failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &state_options), TAG, "register /state options failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &layer_options), TAG, "register /control/layer options failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &buzzer_options), TAG, "register /control/buzzer options failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &consumer_options), TAG, "register /control/consumer options failed");
    return ESP_OK;
}

static esp_err_t web_service_start_internal(void)
{
    if (!s_ws.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!MACRO_WEB_SERVICE_ENABLED) {
        return ESP_OK;
    }
    if (s_ws.running) {
        return ESP_OK;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = (uint16_t)MACRO_WEB_SERVICE_PORT;
    cfg.max_uri_handlers = MACRO_WEB_SERVICE_MAX_URI_HANDLERS;
    cfg.stack_size = MACRO_WEB_SERVICE_STACK_SIZE;
    cfg.recv_wait_timeout = MACRO_WEB_SERVICE_RECV_TIMEOUT_SEC;
    cfg.send_wait_timeout = MACRO_WEB_SERVICE_SEND_TIMEOUT_SEC;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        return err;
    }

    err = register_routes(server);
    if (err != ESP_OK) {
        (void)httpd_stop(server);
        return err;
    }

    web_service_lock();
    s_ws.server = server;
    s_ws.running = true;
    web_service_unlock();

    ESP_LOGI(TAG, "started on port %u (control=%d)", (unsigned)cfg.server_port, MACRO_WEB_SERVICE_CONTROL_ENABLED);
    return ESP_OK;
}

static esp_err_t web_service_stop_internal(void)
{
    if (!s_ws.running) {
        return ESP_OK;
    }

    web_service_lock();
    httpd_handle_t server = s_ws.server;
    s_ws.server = NULL;
    s_ws.running = false;
    web_service_unlock();

    if (server != NULL) {
        ESP_RETURN_ON_ERROR(httpd_stop(server), TAG, "httpd_stop failed");
    }

    ESP_LOGI(TAG, "stopped");
    return ESP_OK;
}

static void web_service_init_auth_config(void)
{
    s_ws.auth_api_key_enabled = false;
    s_ws.auth_basic_enabled = false;
    s_ws.api_key[0] = '\0';
    s_ws.basic_auth_expected[0] = '\0';

    if (strlen(CONFIG_MACROPAD_WEB_API_KEY) > 0U) {
        strlcpy(s_ws.api_key, CONFIG_MACROPAD_WEB_API_KEY, sizeof(s_ws.api_key));
        s_ws.auth_api_key_enabled = true;
    }

    const bool has_basic_user = strlen(CONFIG_MACROPAD_WEB_BASIC_AUTH_USER) > 0U;
    const bool has_basic_pass = strlen(CONFIG_MACROPAD_WEB_BASIC_AUTH_PASSWORD) > 0U;
    if (has_basic_user && has_basic_pass) {
        char plain[WEB_SERVICE_HEADER_MAX] = {0};
        const int n = snprintf(plain,
                               sizeof(plain),
                               "%s:%s",
                               CONFIG_MACROPAD_WEB_BASIC_AUTH_USER,
                               CONFIG_MACROPAD_WEB_BASIC_AUTH_PASSWORD);
        if (n <= 0 || (size_t)n >= sizeof(plain)) {
            ESP_LOGW(TAG, "Basic Auth disabled: credential pair too long");
            return;
        }

        unsigned char encoded[WEB_SERVICE_BASIC_EXPECTED_MAX] = {0};
        size_t encoded_len = 0;
        const int rc = mbedtls_base64_encode(encoded,
                                             sizeof(encoded) - 1U,
                                             &encoded_len,
                                             (const unsigned char *)plain,
                                             strlen(plain));
        if (rc != 0 || encoded_len == 0U || encoded_len >= (sizeof(encoded) - 1U)) {
            ESP_LOGW(TAG, "Basic Auth disabled: base64 encode failed (%d)", rc);
            return;
        }

        encoded[encoded_len] = '\0';
        const int m = snprintf(s_ws.basic_auth_expected,
                               sizeof(s_ws.basic_auth_expected),
                               "Basic %s",
                               (const char *)encoded);
        if (m <= 0 || (size_t)m >= sizeof(s_ws.basic_auth_expected)) {
            ESP_LOGW(TAG, "Basic Auth disabled: header too long");
            return;
        }
        s_ws.auth_basic_enabled = true;
    } else if (has_basic_user || has_basic_pass) {
        ESP_LOGW(TAG, "Basic Auth disabled: both username and password must be set");
    }
}

esp_err_t web_service_init(void)
{
    if (s_ws.initialized) {
        return ESP_OK;
    }

    memset(&s_ws, 0, sizeof(s_ws));
    s_ws.lock = xSemaphoreCreateMutex();
    if (s_ws.lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_ws.boot_tick = xTaskGetTickCount();
    s_ws.last_activity_tick = s_ws.boot_tick;
    s_ws.next_start_retry_tick = s_ws.boot_tick;
    s_ws.initialized = true;
    s_ws.active_layer = 0U;
    web_service_init_auth_config();

    ESP_LOGI(TAG,
             "ready enabled=%d port=%u control=%d api_key=%d basic=%d",
             MACRO_WEB_SERVICE_ENABLED,
             (unsigned)MACRO_WEB_SERVICE_PORT,
             MACRO_WEB_SERVICE_CONTROL_ENABLED,
             s_ws.auth_api_key_enabled,
             s_ws.auth_basic_enabled);
    return ESP_OK;
}

esp_err_t web_service_register_control(const web_service_control_if_t *iface)
{
    if (!s_ws.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (iface == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    web_service_lock();
    s_ws.control = *iface;
    s_ws.control_registered = true;
    web_service_unlock();
    return ESP_OK;
}

void web_service_poll(void)
{
    if (!s_ws.initialized || !MACRO_WEB_SERVICE_ENABLED) {
        return;
    }

    const TickType_t now = xTaskGetTickCount();
    const bool should_run = wifi_portal_is_connected() && !wifi_portal_is_active();
    if (should_run) {
        if (!s_ws.running && now >= s_ws.next_start_retry_tick) {
            esp_err_t err = web_service_start_internal();
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "start failed: %s", esp_err_to_name(err));
                s_ws.next_start_retry_tick = now + pdMS_TO_TICKS(WEB_SERVICE_RETRY_MS);
            }
        }
    } else if (s_ws.running) {
        esp_err_t err = web_service_stop_internal();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "stop failed: %s", esp_err_to_name(err));
        }
    }
}

bool web_service_is_running(void)
{
    return s_ws.running;
}

void web_service_mark_user_activity(void)
{
    if (!s_ws.initialized) {
        return;
    }
    web_service_lock();
    s_ws.last_activity_tick = xTaskGetTickCount();
    web_service_unlock();
}

void web_service_set_active_layer(uint8_t layer_index)
{
    if (!s_ws.initialized) {
        return;
    }
    web_service_lock();
    s_ws.active_layer = layer_index;
    web_service_unlock();
}

void web_service_record_key_event(uint8_t key_index, bool pressed, uint16_t usage, const char *key_name)
{
    if (!s_ws.initialized) {
        return;
    }

    web_service_lock();
    s_ws.last_key.valid = true;
    s_ws.last_key.key_index = key_index;
    s_ws.last_key.pressed = pressed;
    s_ws.last_key.usage = usage;
    s_ws.last_key.tick = xTaskGetTickCount();
    if (key_name != NULL) {
        strlcpy(s_ws.last_key.name, key_name, sizeof(s_ws.last_key.name));
    } else {
        s_ws.last_key.name[0] = '\0';
    }
    web_service_unlock();
}

void web_service_record_encoder_step(int32_t steps, uint16_t usage)
{
    if (!s_ws.initialized) {
        return;
    }
    web_service_lock();
    s_ws.last_encoder.valid = true;
    s_ws.last_encoder.steps = steps;
    s_ws.last_encoder.usage = usage;
    s_ws.last_encoder.tick = xTaskGetTickCount();
    web_service_unlock();
}

void web_service_record_touch_swipe(uint8_t layer_index, bool left_to_right, uint16_t usage)
{
    if (!s_ws.initialized) {
        return;
    }
    web_service_lock();
    s_ws.last_swipe.valid = true;
    s_ws.last_swipe.layer_index = layer_index;
    s_ws.last_swipe.left_to_right = left_to_right;
    s_ws.last_swipe.usage = usage;
    s_ws.last_swipe.tick = xTaskGetTickCount();
    web_service_unlock();
}
