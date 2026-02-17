#include "home_assistant.h"

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "keymap_config.h"
#include "sdkconfig.h"

#define TAG "HOME_ASSISTANT"

#define HA_TASK_STACK 4608
#define HA_TASK_PRIO 3
#define HA_URL_MAX 256
#define HA_AUTH_MAX 448
#define HA_EVENT_SUFFIX_MAX 48
#define HA_EVENT_TYPE_MAX 96
#define HA_JSON_MAX 320
#define HA_KEY_NAME_MAX 32

typedef enum {
    HA_EVT_LAYER_SWITCH = 0,
    HA_EVT_KEY_EVENT,
    HA_EVT_ENCODER_STEP,
    HA_EVT_TOUCH_SWIPE,
    HA_EVT_CUSTOM_JSON,
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
    } data;
} ha_event_t;

static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static bool s_runtime_enabled;
static uint32_t s_last_drop_log_ms;
static char s_base_url[HA_URL_MAX];
static char s_auth_header[HA_AUTH_MAX];
static char s_device_name_escaped[HA_KEY_NAME_MAX];

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

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = MACRO_HA_REQUEST_TIMEOUT_MS,
    };
    if (strncmp(url, "https://", 8) == 0) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        return ESP_FAIL;
    }

    (void)esp_http_client_set_header(client, "Content-Type", "application/json");
    if (s_auth_header[0] != '\0') {
        (void)esp_http_client_set_header(client, "Authorization", s_auth_header);
    }
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

static void ha_worker_task(void *arg)
{
    (void)arg;

    const TickType_t idle_wait_ticks = pdMS_TO_TICKS(MACRO_HA_WORKER_INTERVAL_MS);

    while (1) {
        ha_event_t event = {0};
        if (xQueueReceive(s_queue, &event, idle_wait_ticks) != pdTRUE) {
            continue;
        }

        char event_suffix[HA_EVENT_SUFFIX_MAX] = {0};
        char json[HA_JSON_MAX] = {0};
        if (!build_event_payload(&event, event_suffix, sizeof(event_suffix), json, sizeof(json))) {
            continue;
        }

        const esp_err_t err = post_event_json(event_suffix, json);
        if (err == ESP_OK) {
            continue;
        }

        if (event.retry_count >= MACRO_HA_MAX_RETRY) {
            continue;
        }

        event.retry_count++;
        if (xQueueSend(s_queue, &event, pdMS_TO_TICKS(1)) != pdTRUE) {
            ESP_LOGW(TAG, "Retry enqueue failed; event dropped");
        }
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

    if (xTaskCreate(ha_worker_task, "ha_worker", HA_TASK_STACK, NULL, HA_TASK_PRIO, &s_task) != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_runtime_enabled = true;
    ESP_LOGI(TAG,
             "ready url=%s queue=%d timeout=%dms retries=%d",
             s_base_url,
             (int)MACRO_HA_QUEUE_SIZE,
             (int)MACRO_HA_REQUEST_TIMEOUT_MS,
             (int)MACRO_HA_MAX_RETRY);
    return ESP_OK;
}

bool home_assistant_is_enabled(void)
{
    return s_runtime_enabled;
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
