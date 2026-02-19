#include "wifi_portal.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"

#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "keymap_config.h"
#include "sdkconfig.h"

#define TAG "WIFI_PORTAL"

#define WIFI_STA_CONNECTED_BIT BIT0
#define WIFI_STA_FAILED_BIT BIT1

#define WIFI_PORTAL_DEFAULT_AP_IP "192.168.4.1"
#define WIFI_PORTAL_HTML_BUF 256
#define WIFI_PORTAL_SCAN_BUF 256
#define WIFI_PORTAL_FORM_BUF 512
#define WIFI_PORTAL_DNS_PORT 53

enum {
    WIFI_PORTAL_SCAN_RESULTS =
        (MACRO_WIFI_PORTAL_SCAN_MAX_RESULTS > 0) ? MACRO_WIFI_PORTAL_SCAN_MAX_RESULTS : 1
};

typedef enum {
    PORTAL_STATE_IDLE = 0,
    PORTAL_STATE_STA_CONNECTING,
    PORTAL_STATE_PORTAL_ACTIVE,
    PORTAL_STATE_PORTAL_CONNECTING,
    PORTAL_STATE_CONNECTED,
    PORTAL_STATE_FAILED,
} portal_state_t;

static EventGroupHandle_t s_wifi_evt_group;
static SemaphoreHandle_t s_state_lock;
static esp_netif_t *s_sta_netif;
static esp_netif_t *s_ap_netif;
static httpd_handle_t s_httpd;
static TaskHandle_t s_dns_task;

static bool s_initialized;
static bool s_wifi_started;
static bool s_portal_active;
static bool s_connected;
static bool s_waiting_for_connect;
static bool s_stop_portal_requested;
static bool s_reboot_requested;
static bool s_cancel_requested;
static bool s_timeout_requested;
static bool s_cancelled;
static bool s_timed_out;
static bool s_using_saved_credentials;
static bool s_connect_from_portal;
static uint8_t s_retry_count;
static bool s_boot_connect_in_progress;
static bool s_boot_saved_fallback_pending;
static bool s_boot_saved_fallback_attempted;
static TickType_t s_sta_attempt_start_tick;
static TickType_t s_portal_start_tick;
static TickType_t s_reboot_request_tick;
static portal_state_t s_state;
static wifi_config_t s_boot_saved_cfg;

static char s_ap_ssid[33];
static char s_selected_ssid[33];
static char s_sta_ip[16];

static int s_dns_socket = -1;
static volatile bool s_dns_running = false;

static inline void lock_state(void)
{
    if (s_state_lock != NULL) {
        (void)xSemaphoreTake(s_state_lock, portMAX_DELAY);
    }
}

static inline void unlock_state(void)
{
    if (s_state_lock != NULL) {
        (void)xSemaphoreGive(s_state_lock);
    }
}

static void set_state(portal_state_t state)
{
    lock_state();
    s_state = state;
    unlock_state();
}

static const char *state_text(portal_state_t state)
{
    switch (state) {
    case PORTAL_STATE_IDLE:
        return "idle";
    case PORTAL_STATE_STA_CONNECTING:
        return "sta connect";
    case PORTAL_STATE_PORTAL_ACTIVE:
        return "portal ready";
    case PORTAL_STATE_PORTAL_CONNECTING:
        return "portal connect";
    case PORTAL_STATE_CONNECTED:
        return "connected";
    case PORTAL_STATE_FAILED:
        return "connect fail";
    default:
        return "unknown";
    }
}

static bool has_menuconfig_credentials(void)
{
    return (strlen(CONFIG_MACROPAD_WIFI_SSID) > 0U);
}

static bool get_saved_sta_credentials(wifi_config_t *out_cfg)
{
    if (out_cfg == NULL) {
        return false;
    }
    memset(out_cfg, 0, sizeof(*out_cfg));
    if (esp_wifi_get_config(WIFI_IF_STA, out_cfg) != ESP_OK) {
        return false;
    }
    return out_cfg->sta.ssid[0] != '\0';
}

static bool wifi_cfg_sta_same(const wifi_config_t *a, const wifi_config_t *b)
{
    if (a == NULL || b == NULL) {
        return false;
    }
    return (strncmp((const char *)a->sta.ssid, (const char *)b->sta.ssid, sizeof(a->sta.ssid)) == 0) &&
           (strncmp((const char *)a->sta.password, (const char *)b->sta.password, sizeof(a->sta.password)) == 0);
}

static bool auth_mode_valid_for_password(wifi_auth_mode_t mode, const char *password)
{
    if (mode == WIFI_AUTH_OPEN) {
        return true;
    }
    if (password == NULL) {
        return false;
    }
    return strlen(password) >= 8U;
}

static bool auth_mode_supported_for_softap(wifi_auth_mode_t mode)
{
    switch (mode) {
    case WIFI_AUTH_OPEN:
    case WIFI_AUTH_WPA_PSK:
    case WIFI_AUTH_WPA2_PSK:
    case WIFI_AUTH_WPA_WPA2_PSK:
    case WIFI_AUTH_WPA2_WPA3_PSK:
    case WIFI_AUTH_WPA3_PSK:
    case WIFI_AUTH_WPA3_EXT_PSK:
    case WIFI_AUTH_WPA3_EXT_PSK_MIXED_MODE:
        return true;
    default:
        return false;
    }
}

static wifi_auth_mode_t fallback_softap_auth_mode_for_password(const char *password)
{
    if (password != NULL && strlen(password) >= 8U) {
        return WIFI_AUTH_WPA2_PSK;
    }
    return WIFI_AUTH_OPEN;
}

static wifi_auth_mode_t sanitize_softap_auth_mode(wifi_auth_mode_t configured_mode, char *password, size_t password_size)
{
    wifi_auth_mode_t mode = configured_mode;
    if (!auth_mode_supported_for_softap(mode)) {
        const wifi_auth_mode_t fallback = fallback_softap_auth_mode_for_password(password);
        ESP_LOGW(TAG,
                 "Unsupported softAP authmode=%d, fallback to %s",
                 (int)mode,
                 (fallback == WIFI_AUTH_OPEN) ? "WIFI_AUTH_OPEN" : "WIFI_AUTH_WPA2_PSK");
        mode = fallback;
    }

    if (!auth_mode_valid_for_password(mode, password)) {
        const wifi_auth_mode_t fallback = fallback_softap_auth_mode_for_password(password);
        ESP_LOGW(TAG,
                 "AP auth/password mismatch for authmode=%d, fallback to %s",
                 (int)mode,
                 (fallback == WIFI_AUTH_OPEN) ? "WIFI_AUTH_OPEN" : "WIFI_AUTH_WPA2_PSK");
        mode = fallback;
    }

    if (mode == WIFI_AUTH_OPEN && password != NULL && password_size > 0U) {
        password[0] = '\0';
    }
    return mode;
}

static void store_ap_ip_string(void)
{
    strlcpy(s_sta_ip, WIFI_PORTAL_DEFAULT_AP_IP, sizeof(s_sta_ip));
    if (s_ap_netif == NULL) {
        return;
    }
    esp_netif_ip_info_t ip = {0};
    if (esp_netif_get_ip_info(s_ap_netif, &ip) == ESP_OK) {
        (void)snprintf(s_sta_ip,
                       sizeof(s_sta_ip),
                       "%" PRIu32 ".%" PRIu32 ".%" PRIu32 ".%" PRIu32,
                       (uint32_t)ip.ip.addr & 0xFFU,
                       ((uint32_t)ip.ip.addr >> 8) & 0xFFU,
                       ((uint32_t)ip.ip.addr >> 16) & 0xFFU,
                       ((uint32_t)ip.ip.addr >> 24) & 0xFFU);
    }
}

static void wifi_portal_dns_task(void *arg)
{
    (void)arg;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGW(TAG, "DNS socket create failed");
        s_dns_running = false;
        vTaskDelete(NULL);
        return;
    }
    s_dns_socket = sock;

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(WIFI_PORTAL_DNS_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGW(TAG, "DNS socket bind failed");
        close(sock);
        s_dns_socket = -1;
        s_dns_running = false;
        vTaskDelete(NULL);
        return;
    }

    struct timeval tv = {
        .tv_sec = 1,
        .tv_usec = 0,
    };
    (void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    uint8_t req[512];
    uint8_t rsp[512];

    while (s_dns_running) {
        struct sockaddr_in from = {0};
        socklen_t from_len = sizeof(from);
        const int rx = recvfrom(sock, req, sizeof(req), 0, (struct sockaddr *)&from, &from_len);
        if (rx <= 12) {
            continue;
        }

        int q_end = 12;
        while (q_end < rx && req[q_end] != 0) {
            q_end += req[q_end] + 1;
        }
        if ((q_end + 4) >= rx) {
            continue;
        }
        q_end += 5;

        if ((q_end + 16) >= (int)sizeof(rsp)) {
            continue;
        }

        memset(rsp, 0, sizeof(rsp));
        rsp[0] = req[0];
        rsp[1] = req[1];
        rsp[2] = 0x81;
        rsp[3] = 0x80;
        rsp[4] = req[4];
        rsp[5] = req[5];
        rsp[6] = 0x00;
        rsp[7] = 0x01;

        memcpy(&rsp[12], &req[12], (size_t)(q_end - 12));
        int tx = q_end;

        rsp[tx++] = 0xC0;
        rsp[tx++] = 0x0C;
        rsp[tx++] = 0x00;
        rsp[tx++] = 0x01;
        rsp[tx++] = 0x00;
        rsp[tx++] = 0x01;
        rsp[tx++] = 0x00;
        rsp[tx++] = 0x00;
        rsp[tx++] = 0x00;
        rsp[tx++] = 0x1E;
        rsp[tx++] = 0x00;
        rsp[tx++] = 0x04;

        uint32_t ip_addr = inet_addr(s_sta_ip);
        memcpy(&rsp[tx], &ip_addr, sizeof(ip_addr));
        tx += (int)sizeof(ip_addr);

        (void)sendto(sock, rsp, (size_t)tx, 0, (struct sockaddr *)&from, from_len);
    }

    close(sock);
    s_dns_socket = -1;
    s_dns_running = false;
    vTaskDelete(NULL);
}

static void url_decode(char *dst, size_t dst_size, const char *src)
{
    size_t w = 0;
    if (dst_size == 0U) {
        return;
    }
    for (size_t i = 0; src[i] != '\0' && w + 1U < dst_size; ++i) {
        if (src[i] == '+') {
            dst[w++] = ' ';
            continue;
        }
        if (src[i] == '%' && isxdigit((int)src[i + 1]) && isxdigit((int)src[i + 2])) {
            const char hex[3] = {src[i + 1], src[i + 2], '\0'};
            dst[w++] = (char)strtol(hex, NULL, 16);
            i += 2;
            continue;
        }
        dst[w++] = src[i];
    }
    dst[w] = '\0';
}

static void html_escape_copy(char *dst, size_t dst_size, const char *src)
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
        case '&':
            rep = "&amp;";
            break;
        case '<':
            rep = "&lt;";
            break;
        case '>':
            rep = "&gt;";
            break;
        case '"':
            rep = "&quot;";
            break;
        case '\'':
            rep = "&#39;";
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

static bool form_get_value(const char *form, const char *key, char *out, size_t out_size)
{
    if (form == NULL || key == NULL || out == NULL || out_size == 0U) {
        return false;
    }

    const size_t key_len = strlen(key);
    const char *p = form;
    while (*p != '\0') {
        if ((strncmp(p, key, key_len) == 0) && p[key_len] == '=') {
            p += key_len + 1U;
            const char *end = strchr(p, '&');
            const size_t raw_len = (end != NULL) ? (size_t)(end - p) : strlen(p);
            char tmp[WIFI_PORTAL_FORM_BUF] = {0};
            const size_t copy_len = (raw_len < (sizeof(tmp) - 1U)) ? raw_len : (sizeof(tmp) - 1U);
            memcpy(tmp, p, copy_len);
            tmp[copy_len] = '\0';
            url_decode(out, out_size, tmp);
            return true;
        }
        p = strchr(p, '&');
        if (p == NULL) {
            break;
        }
        ++p;
    }
    return false;
}

static esp_err_t start_sta_connect(wifi_config_t *cfg, bool from_portal)
{
    if (cfg == NULL || cfg->sta.ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    bool started_now = false;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(s_portal_active ? WIFI_MODE_APSTA : WIFI_MODE_STA), TAG, "set mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, cfg), TAG, "set sta config failed");
    if (!s_wifi_started) {
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");
        s_wifi_started = true;
        started_now = true;
    }

    xEventGroupClearBits(s_wifi_evt_group, WIFI_STA_CONNECTED_BIT | WIFI_STA_FAILED_BIT);
    lock_state();
    s_retry_count = 0;
    s_waiting_for_connect = true;
    s_connected = false;
    s_stop_portal_requested = false;
    s_cancelled = false;
    s_timed_out = false;
    s_connect_from_portal = from_portal;
    s_sta_attempt_start_tick = xTaskGetTickCount();
    strlcpy(s_selected_ssid, (const char *)cfg->sta.ssid, sizeof(s_selected_ssid));
    unlock_state();

    set_state(from_portal ? PORTAL_STATE_PORTAL_CONNECTING : PORTAL_STATE_STA_CONNECTING);
    if (!started_now) {
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t stop_dns_server(void)
{
    if (!s_dns_running) {
        return ESP_OK;
    }
    s_dns_running = false;
    if (s_dns_socket >= 0) {
        close(s_dns_socket);
        s_dns_socket = -1;
    }
    return ESP_OK;
}

static esp_err_t stop_http_server(void)
{
    if (s_httpd == NULL) {
        return ESP_OK;
    }
    httpd_handle_t server = s_httpd;
    s_httpd = NULL;
    return httpd_stop(server);
}

static esp_err_t portal_stop_internal(bool cancelled, bool timed_out)
{
    stop_dns_server();
    stop_http_server();

    lock_state();
    s_portal_active = false;
    s_cancel_requested = false;
    s_timeout_requested = false;
    s_cancelled = cancelled;
    s_timed_out = timed_out;
    s_connect_from_portal = false;
    unlock_state();

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set sta mode failed");

    set_state(s_connected ? PORTAL_STATE_CONNECTED : PORTAL_STATE_IDLE);
    return ESP_OK;
}

static esp_err_t portal_try_connect(const char *ssid, const char *password)
{
    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid));
    if (password != NULL) {
        strlcpy((char *)cfg.sta.password, password, sizeof(cfg.sta.password));
    }
    cfg.sta.pmf_cfg.capable = true;

    return start_sta_connect(&cfg, true);
}

static esp_err_t captive_redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static int wifi_scan_to_options(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0U) {
        return 0;
    }
    out[0] = '\0';

    wifi_scan_config_t scan_cfg = {
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };

    if (esp_wifi_scan_start(&scan_cfg, true) != ESP_OK) {
        return 0;
    }

    uint16_t ap_count = (uint16_t)WIFI_PORTAL_SCAN_RESULTS;
    if (ap_count == 0U) {
        return 0;
    }

    wifi_ap_record_t *records = calloc((size_t)WIFI_PORTAL_SCAN_RESULTS, sizeof(wifi_ap_record_t));
    if (records == NULL) {
        return 0;
    }
    if (esp_wifi_scan_get_ap_records(&ap_count, records) != ESP_OK || ap_count == 0U) {
        free(records);
        return 0;
    }

    size_t used = 0;
    for (uint16_t i = 0; i < ap_count; ++i) {
        if (records[i].ssid[0] == '\0') {
            continue;
        }
        char ssid_raw[33] = {0};
        char ssid_html[128] = {0};
        strlcpy(ssid_raw, (const char *)records[i].ssid, sizeof(ssid_raw));
        html_escape_copy(ssid_html, sizeof(ssid_html), ssid_raw);

        char line[WIFI_PORTAL_SCAN_BUF] = {0};
        const int n = snprintf(line,
                               sizeof(line),
                               "<option value=\"%s\">%s (%ddBm)</option>\n",
                               ssid_html,
                               ssid_html,
                               (int)records[i].rssi);
        if (n <= 0 || (size_t)n >= sizeof(line)) {
            continue;
        }
        const size_t len = strlen(line);
        if ((used + len + 1U) >= out_size) {
            break;
        }
        memcpy(out + used, line, len);
        used += len;
        out[used] = '\0';
    }
    free(records);
    return (int)used;
}

static esp_err_t portal_root_handler(httpd_req_t *req)
{
    char *options = calloc(1, 3072);
    char *html = calloc(1, 4096);
    char state_buf[64] = {0};
    char selected_ssid[33] = {0};
    char ap_ssid[33] = {0};
    char ap_ip[16] = {0};
    portal_state_t state_snapshot = PORTAL_STATE_IDLE;

    if (options == NULL || html == NULL) {
        free(options);
        free(html);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
        return ESP_OK;
    }

    lock_state();
    strlcpy(selected_ssid, s_selected_ssid, sizeof(selected_ssid));
    strlcpy(ap_ssid, s_ap_ssid, sizeof(ap_ssid));
    strlcpy(ap_ip, s_sta_ip, sizeof(ap_ip));
    strlcpy(state_buf, state_text(s_state), sizeof(state_buf));
    state_snapshot = s_state;
    unlock_state();

    if (state_snapshot != PORTAL_STATE_PORTAL_CONNECTING &&
        state_snapshot != PORTAL_STATE_STA_CONNECTING) {
        (void)wifi_scan_to_options(options, 3072);
    }

    (void)snprintf(
        html,
        4096,
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>ESP32 MacroPad Wi-Fi Setup</title></head>"
        "<body style=\"font-family:Arial,sans-serif;padding:16px;max-width:640px;margin:auto;\">"
        "<h2>ESP32 MacroPad Wi-Fi Setup</h2>"
        "<p><b>AP:</b> %s &nbsp; <b>IP:</b> %s</p>"
        "<p><b>Status:</b> %s</p>"
        "<p><b>Selected:</b> %s</p>"
        "<form method=\"post\" action=\"/connect\">"
        "<label>Wi-Fi SSID</label><br>"
        "<select name=\"ssid\" style=\"width:100%%;padding:8px;\">%s</select><br><br>"
        "<label>Password</label><br>"
        "<input type=\"password\" name=\"password\" style=\"width:100%%;padding:8px;\"><br><br>"
        "<button type=\"submit\" style=\"padding:10px 16px;\">Connect</button>"
        "</form><br>"
        "<form method=\"get\" action=\"/\">"
        "<button type=\"submit\" style=\"padding:8px 12px;\">Refresh Scan</button>"
        "</form>"
        "<p style=\"color:#666;\">Tip: encoder triple-tap cancels provisioning.</p>"
        "</body></html>",
        ap_ssid,
        ap_ip,
        state_buf,
        (selected_ssid[0] != '\0') ? selected_ssid : "-",
        options);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    free(options);
    free(html);
    return ESP_OK;
}

static esp_err_t portal_connect_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len >= WIFI_PORTAL_FORM_BUF) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid request body");
        return ESP_OK;
    }

    char form[WIFI_PORTAL_FORM_BUF] = {0};
    int rx = 0;
    while (rx < req->content_len) {
        const int r = httpd_req_recv(req, form + rx, req->content_len - rx);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv failed");
            return ESP_OK;
        }
        rx += r;
    }
    form[rx] = '\0';

    char ssid[33] = {0};
    char password[65] = {0};
    (void)form_get_value(form, "ssid", ssid, sizeof(ssid));
    (void)form_get_value(form, "password", password, sizeof(password));

    if (ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID required");
        return ESP_OK;
    }

    const esp_err_t err = portal_try_connect(ssid, password);
    if (err != ESP_OK) {
        char msg[96] = {0};
        (void)snprintf(msg, sizeof(msg), "connect start failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, msg);
        return ESP_OK;
    }

    httpd_resp_set_status(req, "303 See Other");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t start_http_server(void)
{
    if (s_httpd != NULL) {
        return ESP_OK;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 10;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.stack_size = 8192;

    ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &cfg), TAG, "httpd start failed");

    static const httpd_uri_t root_get = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = portal_root_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t connect_post = {
        .uri = "/connect",
        .method = HTTP_POST,
        .handler = portal_connect_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t captive_204 = {
        .uri = "/generate_204",
        .method = HTTP_GET,
        .handler = captive_redirect_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t captive_hotspot = {
        .uri = "/hotspot-detect.html",
        .method = HTTP_GET,
        .handler = captive_redirect_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t captive_ncsi = {
        .uri = "/ncsi.txt",
        .method = HTTP_GET,
        .handler = captive_redirect_handler,
        .user_ctx = NULL,
    };
    static const httpd_uri_t catchall_get = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = captive_redirect_handler,
        .user_ctx = NULL,
    };

    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &root_get), TAG, "register root failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &connect_post), TAG, "register connect failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &captive_204), TAG, "register 204 failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &captive_hotspot), TAG, "register hotspot failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &captive_ncsi), TAG, "register ncsi failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(s_httpd, &catchall_get), TAG, "register catchall failed");
    return ESP_OK;
}

static esp_err_t start_dns_server(void)
{
    if (!MACRO_WIFI_PORTAL_DNS_ENABLED) {
        return ESP_OK;
    }
    if (s_dns_running) {
        return ESP_OK;
    }
    s_dns_running = true;
    if (xTaskCreate(wifi_portal_dns_task, "wifi_portal_dns", 4096, NULL, 3, &s_dns_task) != pdPASS) {
        s_dns_running = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

static esp_err_t portal_start_internal(void)
{
    if (!MACRO_WIFI_PORTAL_ENABLED) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        ESP_RETURN_ON_FALSE(s_ap_netif != NULL, ESP_FAIL, TAG, "create AP netif failed");
    }

    wifi_config_t ap_cfg = {0};
    strlcpy((char *)ap_cfg.ap.ssid, MACRO_WIFI_PORTAL_AP_SSID, sizeof(ap_cfg.ap.ssid));
    strlcpy((char *)ap_cfg.ap.password, MACRO_WIFI_PORTAL_AP_PASSWORD, sizeof(ap_cfg.ap.password));
    ap_cfg.ap.ssid_len = (uint8_t)strlen((const char *)ap_cfg.ap.ssid);
    ap_cfg.ap.max_connection = (uint8_t)MACRO_WIFI_PORTAL_AP_MAX_CONNECTIONS;
    ap_cfg.ap.channel = (uint8_t)MACRO_WIFI_PORTAL_AP_CHANNEL;
    ap_cfg.ap.authmode = sanitize_softap_auth_mode((wifi_auth_mode_t)MACRO_WIFI_PORTAL_AP_AUTH_MODE,
                                                   (char *)ap_cfg.ap.password,
                                                   sizeof(ap_cfg.ap.password));

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_APSTA), TAG, "set APSTA mode failed");
    esp_err_t ap_cfg_err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (ap_cfg_err != ESP_OK) {
        wifi_config_t fallback_cfg = ap_cfg;
        fallback_cfg.ap.authmode = fallback_softap_auth_mode_for_password((const char *)fallback_cfg.ap.password);
        if (fallback_cfg.ap.authmode == WIFI_AUTH_OPEN) {
            fallback_cfg.ap.password[0] = '\0';
        }
        ESP_LOGW(TAG,
                 "set AP config failed for authmode=%d (%s), retrying with %s",
                 (int)ap_cfg.ap.authmode,
                 esp_err_to_name(ap_cfg_err),
                 (fallback_cfg.ap.authmode == WIFI_AUTH_OPEN) ? "WIFI_AUTH_OPEN" : "WIFI_AUTH_WPA2_PSK");
        ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &fallback_cfg), TAG, "set AP fallback config failed");
        ap_cfg = fallback_cfg;
    }

    if (!s_wifi_started) {
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start failed");
        s_wifi_started = true;
    }

    strlcpy(s_ap_ssid, (const char *)ap_cfg.ap.ssid, sizeof(s_ap_ssid));
    store_ap_ip_string();
    ESP_RETURN_ON_ERROR(start_http_server(), TAG, "http server start failed");
    ESP_RETURN_ON_ERROR(start_dns_server(), TAG, "dns server start failed");

    lock_state();
    s_portal_active = true;
    s_portal_start_tick = xTaskGetTickCount();
    s_waiting_for_connect = false;
    s_retry_count = 0;
    s_stop_portal_requested = false;
    s_cancel_requested = false;
    s_timeout_requested = false;
    s_cancelled = false;
    s_timed_out = false;
    unlock_state();

    set_state(PORTAL_STATE_PORTAL_ACTIVE);
    ESP_LOGW(TAG, "Provisioning AP active: ssid=%s ip=%s", s_ap_ssid, s_sta_ip);
    return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_waiting_for_connect) {
            (void)esp_wifi_connect();
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *disc = (const wifi_event_sta_disconnected_t *)event_data;
        xEventGroupClearBits(s_wifi_evt_group, WIFI_STA_CONNECTED_BIT);

        bool waiting_after = false;
        lock_state();
        s_connected = false;
        const bool should_retry = s_waiting_for_connect && (s_retry_count < (uint8_t)MACRO_WIFI_PORTAL_STA_MAX_RETRY);
        if (should_retry) {
            s_retry_count++;
        } else if (s_waiting_for_connect) {
            s_waiting_for_connect = false;
        }
        waiting_after = s_waiting_for_connect;
        unlock_state();

        if (should_retry) {
            ESP_LOGW(TAG,
                     "STA disconnected reason=%d retry=%u/%u",
                     disc ? disc->reason : -1,
                     (unsigned)s_retry_count,
                     (unsigned)MACRO_WIFI_PORTAL_STA_MAX_RETRY);
            (void)esp_wifi_connect();
        } else if (!waiting_after) {
            xEventGroupSetBits(s_wifi_evt_group, WIFI_STA_FAILED_BIT);
            if (s_portal_active) {
                set_state(PORTAL_STATE_PORTAL_ACTIVE);
            } else {
                set_state(PORTAL_STATE_FAILED);
            }
        }
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *ip_event = (const ip_event_got_ip_t *)event_data;
        char ip_buf[16] = {0};
        (void)snprintf(ip_buf,
                       sizeof(ip_buf),
                       IPSTR,
                       IP2STR(&ip_event->ip_info.ip));

        lock_state();
        s_connected = true;
        s_waiting_for_connect = false;
        s_retry_count = 0;
        s_boot_connect_in_progress = false;
        s_boot_saved_fallback_pending = false;
        s_boot_saved_fallback_attempted = false;
        strlcpy(s_sta_ip, ip_buf, sizeof(s_sta_ip));
        if (s_portal_active && s_connect_from_portal) {
            s_reboot_requested = true;
            s_reboot_request_tick = xTaskGetTickCount();
            ESP_LOGW(TAG, "Portal provisioning succeeded; scheduling clean reboot");
        } else if (s_portal_active) {
            s_stop_portal_requested = true;
            ESP_LOGW(TAG, "Portal active but connection not from portal; stopping portal without reboot");
        }
        s_connect_from_portal = false;
        unlock_state();

        xEventGroupSetBits(s_wifi_evt_group, WIFI_STA_CONNECTED_BIT);
        set_state(PORTAL_STATE_CONNECTED);
        ESP_LOGI(TAG, "STA connected ip=%s", ip_buf);
        return;
    }
}

esp_err_t wifi_portal_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    esp_err_t err = esp_netif_init();
    ESP_RETURN_ON_ERROR(err, TAG, "esp_netif_init failed");

    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }

    s_state_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_state_lock != NULL, ESP_ERR_NO_MEM, TAG, "state lock alloc failed");

    s_wifi_evt_group = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_wifi_evt_group != NULL, ESP_ERR_NO_MEM, TAG, "wifi event group alloc failed");

    s_sta_netif = esp_netif_create_default_wifi_sta();
    ESP_RETURN_ON_FALSE(s_sta_netif != NULL, ESP_FAIL, TAG, "create STA netif failed");

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_FLASH), TAG, "wifi storage set failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL),
                        TAG,
                        "wifi event register failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL),
                        TAG,
                        "ip event register failed");

    s_state = PORTAL_STATE_IDLE;
    s_sta_ip[0] = '\0';
    s_initialized = true;
    return ESP_OK;
}

esp_err_t wifi_portal_start(void)
{
    ESP_RETURN_ON_ERROR(wifi_portal_init(), TAG, "wifi portal init failed");

    wifi_config_t menu_cfg = {0};
    wifi_config_t saved_cfg = {0};
    bool has_menu_cfg = false;
    bool has_saved_cfg = false;

    if (has_menuconfig_credentials()) {
        strlcpy((char *)menu_cfg.sta.ssid, CONFIG_MACROPAD_WIFI_SSID, sizeof(menu_cfg.sta.ssid));
        strlcpy((char *)menu_cfg.sta.password, CONFIG_MACROPAD_WIFI_PASSWORD, sizeof(menu_cfg.sta.password));
        menu_cfg.sta.pmf_cfg.capable = true;
        has_menu_cfg = true;
    }
    has_saved_cfg = get_saved_sta_credentials(&saved_cfg);

    lock_state();
    s_boot_connect_in_progress = false;
    s_boot_saved_fallback_pending = false;
    s_boot_saved_fallback_attempted = false;
    s_connect_from_portal = false;
    memset(&s_boot_saved_cfg, 0, sizeof(s_boot_saved_cfg));
    unlock_state();

    if (has_menu_cfg) {
        if (has_saved_cfg && !wifi_cfg_sta_same(&menu_cfg, &saved_cfg)) {
            lock_state();
            s_boot_saved_cfg = saved_cfg;
            s_boot_saved_fallback_pending = true;
            unlock_state();
        }

        ESP_RETURN_ON_ERROR(start_sta_connect(&menu_cfg, false), TAG, "start menuconfig STA connect failed");
        lock_state();
        s_boot_connect_in_progress = true;
        s_using_saved_credentials = false;
        unlock_state();
        ESP_LOGI(TAG,
                 "STA connect started (menuconfig credentials, timeout=%u ms)",
                 (unsigned)MACRO_WIFI_PORTAL_STA_CONNECT_TIMEOUT_MS);
        return ESP_OK;
    }

    if (has_saved_cfg) {
        ESP_RETURN_ON_ERROR(start_sta_connect(&saved_cfg, false), TAG, "start saved STA connect failed");
        lock_state();
        s_boot_connect_in_progress = true;
        s_using_saved_credentials = true;
        unlock_state();
        ESP_LOGI(TAG,
                 "STA connect started (stored credentials, timeout=%u ms)",
                 (unsigned)MACRO_WIFI_PORTAL_STA_CONNECT_TIMEOUT_MS);
        return ESP_OK;
    }

    if (!has_menu_cfg && !has_saved_cfg) {
        ESP_LOGW(TAG, "No STA credentials configured");
        if (MACRO_WIFI_PORTAL_ENABLED) {
            return portal_start_internal();
        }
        return ESP_OK;
    }

    return ESP_OK;
}

void wifi_portal_poll(void)
{
    bool stop_requested = false;
    bool cancel_requested = false;
    bool timeout_requested = false;
    bool reboot_requested = false;
    bool active = false;
    TickType_t start_tick = 0;
    TickType_t reboot_request_tick = 0;
    bool connected = false;
    bool waiting = false;
    bool boot_in_progress = false;
    bool saved_fallback_pending = false;
    bool saved_fallback_attempted = false;
    TickType_t sta_attempt_tick = 0;
    portal_state_t state = PORTAL_STATE_IDLE;
    wifi_config_t saved_cfg = {0};

    lock_state();
    stop_requested = s_stop_portal_requested;
    cancel_requested = s_cancel_requested;
    timeout_requested = s_timeout_requested;
    reboot_requested = s_reboot_requested;
    active = s_portal_active;
    start_tick = s_portal_start_tick;
    reboot_request_tick = s_reboot_request_tick;
    connected = s_connected;
    waiting = s_waiting_for_connect;
    boot_in_progress = s_boot_connect_in_progress;
    saved_fallback_pending = s_boot_saved_fallback_pending;
    saved_fallback_attempted = s_boot_saved_fallback_attempted;
    sta_attempt_tick = s_sta_attempt_start_tick;
    state = s_state;
    saved_cfg = s_boot_saved_cfg;
    unlock_state();

    if (reboot_requested) {
        const TickType_t reboot_delay = pdMS_TO_TICKS(250);
        if ((xTaskGetTickCount() - reboot_request_tick) >= reboot_delay) {
            lock_state();
            s_reboot_requested = false;
            unlock_state();
            ESP_LOGW(TAG, "Provisioning completed; rebooting to apply clean STA runtime");
            esp_restart();
        }
        return;
    }

    if (active) {
        const TickType_t timeout_ticks = pdMS_TO_TICKS((uint32_t)MACRO_WIFI_PORTAL_TIMEOUT_SEC * 1000U);
        if (timeout_ticks > 0U && (xTaskGetTickCount() - start_tick) >= timeout_ticks) {
            timeout_requested = true;
        }
    }

    if (stop_requested) {
        lock_state();
        s_stop_portal_requested = false;
        unlock_state();
        (void)portal_stop_internal(false, false);
        return;
    }

    if (cancel_requested || timeout_requested) {
        lock_state();
        s_cancel_requested = false;
        s_timeout_requested = false;
        unlock_state();
        (void)portal_stop_internal(cancel_requested, timeout_requested);
        return;
    }

    if (!active && boot_in_progress && !connected) {
        const TickType_t sta_timeout_ticks =
            pdMS_TO_TICKS((uint32_t)MACRO_WIFI_PORTAL_STA_CONNECT_TIMEOUT_MS);
        const bool timed_out = (sta_timeout_ticks > 0U) &&
                               ((xTaskGetTickCount() - sta_attempt_tick) >= sta_timeout_ticks);
        const bool failed_now = (!waiting) && (state == PORTAL_STATE_FAILED);
        if (timed_out || failed_now) {
            if (saved_fallback_pending && !saved_fallback_attempted) {
                ESP_LOGW(TAG, "Boot STA connect failed; trying stored credentials");
                lock_state();
                s_boot_saved_fallback_attempted = true;
                s_boot_saved_fallback_pending = false;
                unlock_state();
                if (start_sta_connect(&saved_cfg, false) == ESP_OK) {
                    lock_state();
                    s_using_saved_credentials = true;
                    unlock_state();
                }
                return;
            }

            lock_state();
            s_boot_connect_in_progress = false;
            unlock_state();

            ESP_LOGW(TAG, "Initial STA connect failed/timed out");
            if (MACRO_WIFI_PORTAL_ENABLED) {
                (void)portal_start_internal();
            }
        }
    }
}

bool wifi_portal_is_active(void)
{
    bool active = false;
    lock_state();
    active = s_portal_active;
    unlock_state();
    return active;
}

bool wifi_portal_is_connected(void)
{
    bool connected = false;
    lock_state();
    connected = s_connected;
    unlock_state();
    return connected;
}

esp_err_t wifi_portal_cancel(void)
{
    if (!wifi_portal_is_active()) {
        return ESP_ERR_INVALID_STATE;
    }
    lock_state();
    s_cancel_requested = true;
    unlock_state();
    return ESP_OK;
}

bool wifi_portal_get_oled_lines(char *line0,
                                size_t line0_size,
                                char *line1,
                                size_t line1_size,
                                char *line2,
                                size_t line2_size,
                                char *line3,
                                size_t line3_size)
{
    if (line0 == NULL || line1 == NULL || line2 == NULL || line3 == NULL) {
        return false;
    }
    if (line0_size == 0U || line1_size == 0U || line2_size == 0U || line3_size == 0U) {
        return false;
    }

    bool active = false;
    char ap_ssid[33] = {0};
    char selected[33] = {0};
    char state_buf[24] = {0};
    char ap_ip[16] = {0};
    TickType_t start_tick = 0;

    lock_state();
    active = s_portal_active;
    strlcpy(ap_ssid, s_ap_ssid, sizeof(ap_ssid));
    strlcpy(selected, s_selected_ssid, sizeof(selected));
    strlcpy(state_buf, state_text(s_state), sizeof(state_buf));
    strlcpy(ap_ip, s_sta_ip, sizeof(ap_ip));
    start_tick = s_portal_start_tick;
    unlock_state();

    if (!active) {
        return false;
    }

    const uint32_t elapsed_s = (uint32_t)((xTaskGetTickCount() - start_tick) / configTICK_RATE_HZ);

    (void)snprintf(line0, line0_size, "WIFI SETUP");
    (void)snprintf(line1, line1_size, "AP:%s", (ap_ssid[0] != '\0') ? ap_ssid : "-");
    if (selected[0] != '\0') {
        (void)snprintf(line2, line2_size, "SSID:%s", selected);
    } else {
        (void)snprintf(line2, line2_size, "OPEN:%s", (ap_ip[0] != '\0') ? ap_ip : WIFI_PORTAL_DEFAULT_AP_IP);
    }
    (void)snprintf(line3, line3_size, "%s %lus T3=cancel", state_buf, (unsigned long)elapsed_s);
    return true;
}
