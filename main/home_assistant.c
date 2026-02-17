#include "home_assistant.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_crt_bundle.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "keymap_config.h"
#include "sdkconfig.h"

#define TAG "HOME_ASSISTANT"

#define HA_TASK_STACK 6144
#define HA_TASK_PRIO 3
#define HA_URL_MAX 256
#define HA_AUTH_MAX 448
#define HA_EVENT_SUFFIX_MAX 48
#define HA_EVENT_TYPE_MAX 96
#define HA_JSON_MAX 320
#define HA_KEY_NAME_MAX 32
#define HA_ENTITY_ID_MAX 96
#define HA_CONTROL_DOMAIN_MAX 32
#define HA_CONTROL_SERVICE_MAX 32
#define HA_DISPLAY_LINE_MAX 96
#define HA_DISPLAY_NAME_MAX 64
#define HA_DISPLAY_STATE_MAX 64
#define HA_HTTP_BODY_MAX 896

typedef enum {
    HA_EVT_LAYER_SWITCH = 0,
    HA_EVT_KEY_EVENT,
    HA_EVT_ENCODER_STEP,
    HA_EVT_TOUCH_SWIPE,
    HA_EVT_CUSTOM_JSON,
    HA_EVT_SERVICE_CALL,
} ha_event_kind_t;

typedef struct {
    ha_event_kind_t kind;
    uint8_t retry_count;
    union {
        struct {
            uint8_t layer_index;
        } layer_switch;
        struct {
            uint8_t layer_index;
            uint8_t key_index;
            bool pressed;
            uint16_t usage;
            char key_name[HA_KEY_NAME_MAX];
        } key_event;
        struct {
            uint8_t layer_index;
            int32_t steps;
            uint16_t usage;
        } encoder_step;
        struct {
            uint8_t layer_index;
            bool left_to_right;
            uint16_t usage;
        } touch_swipe;
        struct {
            char event_suffix[HA_EVENT_SUFFIX_MAX];
            char json_payload[HA_JSON_MAX];
        } custom_json;
        struct {
            char domain[HA_CONTROL_DOMAIN_MAX];
            char service[HA_CONTROL_SERVICE_MAX];
            char entity_id[HA_ENTITY_ID_MAX];
        } service_call;
    } data;
} ha_event_t;

static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static SemaphoreHandle_t s_display_lock;
static bool s_runtime_enabled;
static bool s_display_runtime_enabled;
static bool s_control_runtime_enabled;
static uint32_t s_last_drop_log_ms;
static uint32_t s_last_display_error_log_ms;
static TickType_t s_display_next_poll_tick;
static char s_base_url[HA_URL_MAX];
static char s_auth_header[HA_AUTH_MAX];
static char s_device_name_escaped[HA_KEY_NAME_MAX];

static char s_display_line[HA_DISPLAY_LINE_MAX];
static uint32_t s_display_updated_ms;
static bool s_display_ready;

static inline uint32_t now_ms(void)
{
    return (uint32_t)xTaskGetTickCount() * portTICK_PERIOD_MS;
}

static void json_escape_copy(char *dst, size_t dst_size, const char *src)
{
    size_t w = 0;
    if (dst_size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    for (size_t i = 0; src[i] != '\0'; ++i) {
        const char c = src[i];
        if (w + 2 >= dst_size) {
            break;
        }
        if (c == '"' || c == '\\') {
            dst[w++] = '\\';
            dst[w++] = c;
        } else if ((unsigned char)c < 0x20) {
            dst[w++] = '_';
        } else {
            dst[w++] = c;
        }
    }
    dst[w] = '\0';
}

static void normalize_base_url(void)
{
    strlcpy(s_base_url, CONFIG_MACROPAD_HA_BASE_URL, sizeof(s_base_url));
    size_t n = strlen(s_base_url);
    while (n > 0 && s_base_url[n - 1] == '/') {
        s_base_url[n - 1] = '\0';
        --n;
    }
}

static void build_event_type(char *out, size_t out_size, const char *suffix)
{
    if (MACRO_HA_EVENT_PREFIX[0] != '\0') {
        (void)snprintf(out, out_size, "%s_%s", MACRO_HA_EVENT_PREFIX, suffix);
        return;
    }
    strlcpy(out, suffix, out_size);
}

static esp_http_client_handle_t ha_http_client_init(const char *url, esp_http_client_method_t method)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .method = method,
        .timeout_ms = MACRO_HA_REQUEST_TIMEOUT_MS,
    };
    if (strncmp(url, "https://", 8) == 0) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return NULL;
    }

    if (s_auth_header[0] != '\0') {
        (void)esp_http_client_set_header(client, "Authorization", s_auth_header);
    }
    return client;
}

static esp_err_t post_event_json(const char *event_suffix, const char *json_payload)
{
    char event_type[HA_EVENT_TYPE_MAX];
    char url[HA_URL_MAX + HA_EVENT_TYPE_MAX + 32];
    build_event_type(event_type, sizeof(event_type), event_suffix);

    const int n = snprintf(url, sizeof(url), "%s/api/events/%s", s_base_url, event_type);
    if (n <= 0 || (size_t)n >= sizeof(url)) {
        ESP_LOGE(TAG, "Event URL too long for suffix=%s", event_suffix);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_http_client_handle_t client = ha_http_client_init(url, HTTP_METHOD_POST);
    if (client == NULL) {
        return ESP_FAIL;
    }

    (void)esp_http_client_set_header(client, "Content-Type", "application/json");
    (void)esp_http_client_set_post_field(client, json_payload, strlen(json_payload));

    esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "POST failed event=%s err=%s", event_type, esp_err_to_name(err));
        return err;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "POST rejected event=%s http=%d", event_type, status);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t post_service_json(const char *domain, const char *service, const char *json_payload)
{
    char url[HA_URL_MAX + HA_CONTROL_DOMAIN_MAX + HA_CONTROL_SERVICE_MAX + 32];
    const int n = snprintf(url, sizeof(url), "%s/api/services/%s/%s", s_base_url, domain, service);
    if (n <= 0 || (size_t)n >= sizeof(url)) {
        ESP_LOGE(TAG, "Service URL too long domain=%s service=%s", domain, service);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_http_client_handle_t client = ha_http_client_init(url, HTTP_METHOD_POST);
    if (client == NULL) {
        return ESP_FAIL;
    }

    (void)esp_http_client_set_header(client, "Content-Type", "application/json");
    (void)esp_http_client_set_post_field(client, json_payload, strlen(json_payload));

    esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Service call failed %s/%s err=%s", domain, service, esp_err_to_name(err));
        return err;
    }
    if (status < 200 || status >= 300) {
        ESP_LOGW(TAG, "Service call rejected %s/%s http=%d", domain, service, status);
        return ESP_FAIL;
    }

    return ESP_OK;
}

static esp_err_t http_get_body(const char *url, char *out, size_t out_size)
{
    if (out == NULL || out_size < 2U) {
        return ESP_ERR_INVALID_ARG;
    }
    out[0] = '\0';

    esp_http_client_handle_t client = ha_http_client_init(url, HTTP_METHOD_GET);
    if (client == NULL) {
        return ESP_FAIL;
    }

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    (void)esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);

    size_t used = 0;
    while (used < (out_size - 1U)) {
        const int rd = esp_http_client_read(client, out + used, (int)(out_size - 1U - used));
        if (rd < 0) {
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            return ESP_FAIL;
        }
        if (rd == 0) {
            break;
        }
        used += (size_t)rd;
    }
    out[used] = '\0';

    // Response larger than our buffer.
    if (esp_http_client_is_chunked_response(client) && used == (out_size - 1U)) {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_ERR_INVALID_SIZE;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    if (status < 200 || status >= 300) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool json_extract_string_field(const char *json, const char *field, char *out, size_t out_size)
{
    if (json == NULL || field == NULL || out == NULL || out_size == 0U) {
        return false;
    }
    out[0] = '\0';

    char pattern[48];
    const int pattern_len = snprintf(pattern, sizeof(pattern), "\"%s\":\"", field);
    if (pattern_len <= 0 || (size_t)pattern_len >= sizeof(pattern)) {
        return false;
    }

    const char *p = strstr(json, pattern);
    if (p == NULL) {
        return false;
    }
    p += pattern_len;

    size_t w = 0;
    while (*p != '\0' && w < (out_size - 1U)) {
        if (*p == '"') {
            break;
        }
        if (*p == '\\') {
            ++p;
            if (*p == '\0') {
                break;
            }
            switch (*p) {
            case '"':
            case '\\':
            case '/':
                out[w++] = *p;
                ++p;
                continue;
            case 'b':
                out[w++] = ' ';
                ++p;
                continue;
            case 'f':
            case 'n':
            case 'r':
            case 't':
                out[w++] = ' ';
                ++p;
                continue;
            case 'u':
                // Skip \uXXXX sequence and substitute '?'
                if (w < (out_size - 1U)) {
                    out[w++] = '?';
                }
                ++p;
                for (int i = 0; i < 4 && *p != '\0'; ++i, ++p) {
                }
                continue;
            default:
                out[w++] = *p;
                ++p;
                continue;
            }
        }

        out[w++] = *p;
        ++p;
    }
    out[w] = '\0';
    return w > 0U;
}

static void build_display_line(const char *name, const char *state, char *out, size_t out_size)
{
    const char *label = MACRO_HA_DISPLAY_LABEL;
    if (label[0] == '\0') {
        label = (name != NULL && name[0] != '\0') ? name : "HA";
    }
    (void)snprintf(out, out_size, "%s: %s", label, state);
}

static esp_err_t refresh_display_state(void)
{
    if (!s_display_runtime_enabled) {
        return ESP_ERR_INVALID_STATE;
    }

    char url[HA_URL_MAX + HA_ENTITY_ID_MAX + 24];
    const int n = snprintf(url, sizeof(url), "%s/api/states/%s", s_base_url, MACRO_HA_DISPLAY_ENTITY_ID);
    if (n <= 0 || (size_t)n >= sizeof(url)) {
        return ESP_ERR_INVALID_SIZE;
    }

    char body[HA_HTTP_BODY_MAX];
    ESP_RETURN_ON_ERROR(http_get_body(url, body, sizeof(body)), TAG, "state GET failed");

    char state[HA_DISPLAY_STATE_MAX];
    if (!json_extract_string_field(body, "state", state, sizeof(state))) {
        return ESP_FAIL;
    }

    char friendly_name[HA_DISPLAY_NAME_MAX];
    (void)json_extract_string_field(body, "friendly_name", friendly_name, sizeof(friendly_name));

    char line[HA_DISPLAY_LINE_MAX];
    build_display_line(friendly_name, state, line, sizeof(line));

    if (s_display_lock != NULL && xSemaphoreTake(s_display_lock, pdMS_TO_TICKS(5)) == pdTRUE) {
        strlcpy(s_display_line, line, sizeof(s_display_line));
        s_display_updated_ms = now_ms();
        s_display_ready = true;
        xSemaphoreGive(s_display_lock);
    }
    return ESP_OK;
}

static bool build_event_payload(const ha_event_t *event, char *event_suffix, size_t event_suffix_size, char *json, size_t json_size)
{
    char key_name_escaped[HA_KEY_NAME_MAX];

    switch (event->kind) {
    case HA_EVT_LAYER_SWITCH:
        strlcpy(event_suffix, "layer_switch", event_suffix_size);
        (void)snprintf(json,
                       json_size,
                       "{\"device\":\"%s\",\"layer_index\":%u,\"layer\":%u}",
                       s_device_name_escaped,
                       (unsigned)event->data.layer_switch.layer_index,
                       (unsigned)event->data.layer_switch.layer_index + 1U);
        return true;
    case HA_EVT_KEY_EVENT:
        strlcpy(event_suffix, "key_event", event_suffix_size);
        json_escape_copy(key_name_escaped, sizeof(key_name_escaped), event->data.key_event.key_name);
        (void)snprintf(json,
                       json_size,
                       "{\"device\":\"%s\",\"layer_index\":%u,\"layer\":%u,\"key_index\":%u,\"key\":%u,\"pressed\":%s,\"usage\":%u,\"name\":\"%s\"}",
                       s_device_name_escaped,
                       (unsigned)event->data.key_event.layer_index,
                       (unsigned)event->data.key_event.layer_index + 1U,
                       (unsigned)event->data.key_event.key_index,
                       (unsigned)event->data.key_event.key_index + 1U,
                       event->data.key_event.pressed ? "true" : "false",
                       (unsigned)event->data.key_event.usage,
                       key_name_escaped);
        return true;
    case HA_EVT_ENCODER_STEP:
        strlcpy(event_suffix, "encoder_step", event_suffix_size);
        (void)snprintf(json,
                       json_size,
                       "{\"device\":\"%s\",\"layer_index\":%u,\"layer\":%u,\"steps\":%ld,\"usage\":%u}",
                       s_device_name_escaped,
                       (unsigned)event->data.encoder_step.layer_index,
                       (unsigned)event->data.encoder_step.layer_index + 1U,
                       (long)event->data.encoder_step.steps,
                       (unsigned)event->data.encoder_step.usage);
        return true;
    case HA_EVT_TOUCH_SWIPE:
        strlcpy(event_suffix, "touch_swipe", event_suffix_size);
        (void)snprintf(json,
                       json_size,
                       "{\"device\":\"%s\",\"layer_index\":%u,\"layer\":%u,\"direction\":\"%s\",\"usage\":%u}",
                       s_device_name_escaped,
                       (unsigned)event->data.touch_swipe.layer_index,
                       (unsigned)event->data.touch_swipe.layer_index + 1U,
                       event->data.touch_swipe.left_to_right ? "L_to_R" : "R_to_L",
                       (unsigned)event->data.touch_swipe.usage);
        return true;
    case HA_EVT_CUSTOM_JSON:
        strlcpy(event_suffix, event->data.custom_json.event_suffix, event_suffix_size);
        strlcpy(json, event->data.custom_json.json_payload, json_size);
        return true;
    default:
        return false;
    }
}

static esp_err_t process_event(const ha_event_t *event)
{
    if (event->kind == HA_EVT_SERVICE_CALL) {
        char payload[HA_JSON_MAX];
        (void)snprintf(payload, sizeof(payload), "{\"entity_id\":\"%s\"}", event->data.service_call.entity_id);
        return post_service_json(event->data.service_call.domain, event->data.service_call.service, payload);
    }

    char event_suffix[HA_EVENT_SUFFIX_MAX] = {0};
    char json[HA_JSON_MAX] = {0};
    if (!build_event_payload(event, event_suffix, sizeof(event_suffix), json, sizeof(json))) {
        return ESP_ERR_INVALID_ARG;
    }
    return post_event_json(event_suffix, json);
}

static void queue_event(const ha_event_t *event)
{
    if (!s_runtime_enabled || s_queue == NULL) {
        return;
    }
    if (xQueueSend(s_queue, event, 0) == pdTRUE) {
        return;
    }

    const uint32_t t = now_ms();
    if ((t - s_last_drop_log_ms) >= 1000U) {
        s_last_drop_log_ms = t;
        ESP_LOGW(TAG, "Event queue full; dropping events");
    }
}

static void ha_poll_display_if_due(TickType_t now)
{
    if (!s_display_runtime_enabled) {
        return;
    }
    if (now < s_display_next_poll_tick) {
        return;
    }

    const int poll_ms = (MACRO_HA_DISPLAY_POLL_INTERVAL_MS < 500) ? 500 : MACRO_HA_DISPLAY_POLL_INTERVAL_MS;
    s_display_next_poll_tick = now + pdMS_TO_TICKS((uint32_t)poll_ms);

    const esp_err_t err = refresh_display_state();
    if (err != ESP_OK) {
        const uint32_t t = now_ms();
        if ((t - s_last_display_error_log_ms) >= 3000U) {
            s_last_display_error_log_ms = t;
            ESP_LOGW(TAG, "State poll failed: %s", esp_err_to_name(err));
        }
    }
}

static void ha_worker_task(void *arg)
{
    (void)arg;

    const TickType_t idle_wait_ticks = pdMS_TO_TICKS(MACRO_HA_WORKER_INTERVAL_MS);
    s_display_next_poll_tick = xTaskGetTickCount();

    while (1) {
        ha_event_t event = {0};
        if (xQueueReceive(s_queue, &event, idle_wait_ticks) == pdTRUE) {
            const esp_err_t err = process_event(&event);
            if (err != ESP_OK && event.retry_count < MACRO_HA_MAX_RETRY) {
                event.retry_count++;
                if (xQueueSend(s_queue, &event, pdMS_TO_TICKS(1)) != pdTRUE) {
                    ESP_LOGW(TAG, "Retry enqueue failed; event dropped");
                }
            }
        }

        ha_poll_display_if_due(xTaskGetTickCount());
    }
}

esp_err_t home_assistant_init(void)
{
    if (!MACRO_HA_ENABLED) {
        s_runtime_enabled = false;
        return ESP_OK;
    }

    if (MACRO_HA_QUEUE_SIZE <= 0) {
        ESP_LOGW(TAG, "Disabled: invalid queue_size=%d", (int)MACRO_HA_QUEUE_SIZE);
        s_runtime_enabled = false;
        return ESP_OK;
    }
    if (strlen(CONFIG_MACROPAD_HA_BASE_URL) == 0) {
        ESP_LOGW(TAG, "Disabled: empty CONFIG_MACROPAD_HA_BASE_URL");
        s_runtime_enabled = false;
        return ESP_OK;
    }

    normalize_base_url();
    json_escape_copy(s_device_name_escaped, sizeof(s_device_name_escaped), MACRO_HA_DEVICE_NAME);
    if (strlen(CONFIG_MACROPAD_HA_BEARER_TOKEN) > 0U) {
        (void)snprintf(s_auth_header, sizeof(s_auth_header), "Bearer %s", CONFIG_MACROPAD_HA_BEARER_TOKEN);
    } else {
        s_auth_header[0] = '\0';
    }

    s_queue = xQueueCreate((UBaseType_t)MACRO_HA_QUEUE_SIZE, sizeof(ha_event_t));
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_display_lock = xSemaphoreCreateMutex();
    if (s_display_lock == NULL) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_display_runtime_enabled =
        MACRO_HA_DISPLAY_ENABLED &&
        (strlen(MACRO_HA_DISPLAY_ENTITY_ID) > 0U);

    s_control_runtime_enabled =
        MACRO_HA_CONTROL_ENABLED &&
        (strlen(MACRO_HA_CONTROL_DOMAIN) > 0U) &&
        (strlen(MACRO_HA_CONTROL_SERVICE) > 0U) &&
        (strlen(MACRO_HA_CONTROL_ENTITY_ID) > 0U) &&
        (MACRO_HA_CONTROL_TAP_COUNT > 0);

    if (MACRO_HA_CONTROL_ENABLED && !s_control_runtime_enabled) {
        ESP_LOGW(TAG, "Control disabled: invalid home_assistant.control config");
    }

    if (xTaskCreate(ha_worker_task, "ha_worker", HA_TASK_STACK, NULL, HA_TASK_PRIO, &s_task) != pdPASS) {
        vSemaphoreDelete(s_display_lock);
        s_display_lock = NULL;
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_runtime_enabled = true;
    ESP_LOGI(TAG,
             "ready url=%s queue=%d timeout=%dms retries=%d display=%d control=%d",
             s_base_url,
             (int)MACRO_HA_QUEUE_SIZE,
             (int)MACRO_HA_REQUEST_TIMEOUT_MS,
             (int)MACRO_HA_MAX_RETRY,
             s_display_runtime_enabled ? 1 : 0,
             s_control_runtime_enabled ? 1 : 0);
    return ESP_OK;
}

bool home_assistant_is_enabled(void)
{
    return s_runtime_enabled;
}

bool home_assistant_get_display_text(char *out, size_t out_size, uint32_t *age_ms)
{
    if (out == NULL || out_size == 0U || !s_runtime_enabled || !s_display_runtime_enabled || s_display_lock == NULL) {
        return false;
    }

    bool ok = false;
    out[0] = '\0';
    if (xSemaphoreTake(s_display_lock, pdMS_TO_TICKS(5)) == pdTRUE) {
        if (s_display_ready) {
            strlcpy(out, s_display_line, out_size);
            if (age_ms != NULL) {
                *age_ms = now_ms() - s_display_updated_ms;
            }
            ok = true;
        }
        xSemaphoreGive(s_display_lock);
    }
    return ok;
}

esp_err_t home_assistant_trigger_default_control(void)
{
    if (!s_runtime_enabled || !s_control_runtime_enabled) {
        return ESP_ERR_INVALID_STATE;
    }

    ha_event_t event = {0};
    event.kind = HA_EVT_SERVICE_CALL;
    strlcpy(event.data.service_call.domain, MACRO_HA_CONTROL_DOMAIN, sizeof(event.data.service_call.domain));
    strlcpy(event.data.service_call.service, MACRO_HA_CONTROL_SERVICE, sizeof(event.data.service_call.service));
    strlcpy(event.data.service_call.entity_id, MACRO_HA_CONTROL_ENTITY_ID, sizeof(event.data.service_call.entity_id));
    queue_event(&event);
    return ESP_OK;
}

void home_assistant_notify_layer_switch(uint8_t layer_index)
{
    if (!MACRO_HA_PUBLISH_LAYER_SWITCH) {
        return;
    }

    ha_event_t event = {0};
    event.kind = HA_EVT_LAYER_SWITCH;
    event.data.layer_switch.layer_index = layer_index;
    queue_event(&event);
}

void home_assistant_notify_key_event(uint8_t layer_index,
                                     uint8_t key_index,
                                     bool pressed,
                                     uint16_t usage,
                                     const char *key_name)
{
    if (!MACRO_HA_PUBLISH_KEY_EVENT) {
        return;
    }

    ha_event_t event = {0};
    event.kind = HA_EVT_KEY_EVENT;
    event.data.key_event.layer_index = layer_index;
    event.data.key_event.key_index = key_index;
    event.data.key_event.pressed = pressed;
    event.data.key_event.usage = usage;
    strlcpy(event.data.key_event.key_name, (key_name != NULL) ? key_name : "", sizeof(event.data.key_event.key_name));
    queue_event(&event);
}

void home_assistant_notify_encoder_step(uint8_t layer_index, int32_t steps, uint16_t usage)
{
    if (!MACRO_HA_PUBLISH_ENCODER_STEP) {
        return;
    }

    ha_event_t event = {0};
    event.kind = HA_EVT_ENCODER_STEP;
    event.data.encoder_step.layer_index = layer_index;
    event.data.encoder_step.steps = steps;
    event.data.encoder_step.usage = usage;
    queue_event(&event);
}

void home_assistant_notify_touch_swipe(uint8_t layer_index, bool left_to_right, uint16_t usage)
{
    if (!MACRO_HA_PUBLISH_TOUCH_SWIPE) {
        return;
    }

    ha_event_t event = {0};
    event.kind = HA_EVT_TOUCH_SWIPE;
    event.data.touch_swipe.layer_index = layer_index;
    event.data.touch_swipe.left_to_right = left_to_right;
    event.data.touch_swipe.usage = usage;
    queue_event(&event);
}

esp_err_t home_assistant_queue_custom_event(const char *event_suffix, const char *json_payload)
{
    if (!s_runtime_enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    if (event_suffix == NULL || event_suffix[0] == '\0' || json_payload == NULL || json_payload[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    ha_event_t event = {0};
    event.kind = HA_EVT_CUSTOM_JSON;
    strlcpy(event.data.custom_json.event_suffix, event_suffix, sizeof(event.data.custom_json.event_suffix));
    strlcpy(event.data.custom_json.json_payload, json_payload, sizeof(event.data.custom_json.json_payload));
    queue_event(&event);
    return ESP_OK;
}
