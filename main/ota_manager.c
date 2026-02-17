#include "ota_manager.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "freertos/semphr.h"
#include "freertos/task.h"

#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"

#include "keymap_config.h"
#include "sdkconfig.h"

#define TAG "OTA_MANAGER"

#define OTA_URL_MAX 192
#define OTA_ERROR_MAX 96
#define OTA_TASK_STACK 8192

typedef struct {
    bool initialized;
    SemaphoreHandle_t lock;
    TaskHandle_t worker_task;
    ota_manager_state_t state;
    bool pending_verify;
    TickType_t self_check_start_tick;
    TickType_t confirm_start_tick;
    TickType_t confirm_deadline_tick;
    uint32_t self_check_free_heap_bytes;
    char current_url[OTA_URL_MAX];
    char last_error[OTA_ERROR_MAX];
} ota_manager_context_t;

static ota_manager_context_t s_ota = {0};

static inline void ota_lock(void)
{
    if (s_ota.lock != NULL) {
        (void)xSemaphoreTake(s_ota.lock, portMAX_DELAY);
    }
}

static inline void ota_unlock(void)
{
    if (s_ota.lock != NULL) {
        (void)xSemaphoreGive(s_ota.lock);
    }
}

static void ota_set_error_locked(const char *error_text)
{
    if (error_text != NULL) {
        strlcpy(s_ota.last_error, error_text, sizeof(s_ota.last_error));
    } else {
        s_ota.last_error[0] = '\0';
    }
}

static void ota_set_error_name_locked(esp_err_t err)
{
    strlcpy(s_ota.last_error, esp_err_to_name(err), sizeof(s_ota.last_error));
}

static bool ota_url_is_supported(const char *url)
{
    if (url == NULL) {
        return false;
    }
    return strncmp(url, "https://", 8) == 0;
}

const char *ota_manager_state_name(ota_manager_state_t state)
{
    switch (state) {
    case OTA_MANAGER_STATE_DISABLED:
        return "disabled";
    case OTA_MANAGER_STATE_READY:
        return "ready";
    case OTA_MANAGER_STATE_DOWNLOADING:
        return "downloading";
    case OTA_MANAGER_STATE_DOWNLOAD_FAILED:
        return "download_failed";
    case OTA_MANAGER_STATE_REBOOTING:
        return "rebooting";
    case OTA_MANAGER_STATE_SELF_CHECK_RUNNING:
        return "self_check";
    case OTA_MANAGER_STATE_WAITING_CONFIRM:
        return "wait_confirm";
    case OTA_MANAGER_STATE_CONFIRMED:
        return "confirmed";
    case OTA_MANAGER_STATE_ROLLBACK_REBOOTING:
        return "rollback";
    default:
        return "unknown";
    }
}

static void ota_worker_task(void *arg)
{
    char url[OTA_URL_MAX] = {0};
    (void)arg;

    ota_lock();
    strlcpy(url, s_ota.current_url, sizeof(url));
    ota_unlock();

    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = CONFIG_MACROPAD_OTA_HTTP_TIMEOUT_MS,
        .keep_alive_enable = true,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    ESP_LOGI(TAG, "Starting OTA from: %s", url);
    const esp_err_t err = esp_https_ota(&ota_cfg);
    if (err == ESP_OK) {
        ota_lock();
        s_ota.state = OTA_MANAGER_STATE_REBOOTING;
        ota_set_error_locked(NULL);
        ota_unlock();
        ESP_LOGI(TAG, "OTA downloaded successfully, rebooting to new firmware");
        vTaskDelay(pdMS_TO_TICKS(250));
        esp_restart();
    }

    ota_lock();
    s_ota.state = OTA_MANAGER_STATE_DOWNLOAD_FAILED;
    ota_set_error_name_locked(err);
    s_ota.worker_task = NULL;
    ota_unlock();
    ESP_LOGE(TAG, "OTA download failed: %s", esp_err_to_name(err));
    vTaskDelete(NULL);
}

static void ota_enter_wait_confirm(TickType_t now)
{
    s_ota.state = OTA_MANAGER_STATE_WAITING_CONFIRM;
    s_ota.confirm_start_tick = now;
    if (MACRO_OTA_CONFIRM_TIMEOUT_SEC > 0) {
        s_ota.confirm_deadline_tick = now + pdMS_TO_TICKS((uint32_t)MACRO_OTA_CONFIRM_TIMEOUT_SEC * 1000U);
    } else {
        s_ota.confirm_deadline_tick = 0;
    }
}

static bool ota_run_self_check(uint32_t *free_heap_out)
{
    const uint32_t free_heap = (uint32_t)esp_get_free_heap_size();
    const UBaseType_t task_count = uxTaskGetNumberOfTasks();
    if (free_heap_out != NULL) {
        *free_heap_out = free_heap;
    }

    if (free_heap < (uint32_t)MACRO_OTA_SELF_CHECK_MIN_HEAP_BYTES) {
        ESP_LOGE(TAG,
                 "Self-check failed: free heap=%" PRIu32 " < min=%u",
                 free_heap,
                 (unsigned)MACRO_OTA_SELF_CHECK_MIN_HEAP_BYTES);
        return false;
    }
    if (task_count < 3U) {
        ESP_LOGE(TAG, "Self-check failed: task_count=%u < 3", (unsigned)task_count);
        return false;
    }

    ESP_LOGI(TAG,
             "Self-check passed: free_heap=%" PRIu32 " task_count=%u",
             free_heap,
             (unsigned)task_count);
    return true;
}

esp_err_t ota_manager_init(void)
{
    if (s_ota.initialized) {
        return ESP_OK;
    }

    memset(&s_ota, 0, sizeof(s_ota));
    s_ota.lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_ota.lock != NULL, ESP_ERR_NO_MEM, TAG, "mutex alloc failed");
    s_ota.initialized = true;

    if (!MACRO_OTA_ENABLED) {
        s_ota.state = OTA_MANAGER_STATE_DISABLED;
        ESP_LOGI(TAG, "disabled by config");
        return ESP_OK;
    }

    s_ota.state = OTA_MANAGER_STATE_READY;
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running != NULL) {
        esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
        if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
            state == ESP_OTA_IMG_PENDING_VERIFY) {
            const TickType_t now = xTaskGetTickCount();
            s_ota.pending_verify = true;
            s_ota.state = OTA_MANAGER_STATE_SELF_CHECK_RUNNING;
            s_ota.self_check_start_tick = now;
            ota_set_error_locked(NULL);
            ESP_LOGW(TAG, "Running OTA image is pending verify; starting self-check");
        }
    }

    ESP_LOGI(TAG,
             "ready enabled=%d confirm_taps=%u timeout=%us default_url=%s",
             MACRO_OTA_ENABLED,
             (unsigned)MACRO_OTA_CONFIRM_TAP_COUNT,
             (unsigned)MACRO_OTA_CONFIRM_TIMEOUT_SEC,
             CONFIG_MACROPAD_OTA_DEFAULT_URL);
    return ESP_OK;
}

void ota_manager_poll(TickType_t now)
{
    if (!s_ota.initialized || !MACRO_OTA_ENABLED) {
        return;
    }

    ota_lock();
    if (s_ota.state == OTA_MANAGER_STATE_SELF_CHECK_RUNNING) {
        const TickType_t elapsed = now - s_ota.self_check_start_tick;
        if (elapsed >= pdMS_TO_TICKS((uint32_t)MACRO_OTA_SELF_CHECK_DURATION_MS)) {
            uint32_t free_heap = 0;
            if (ota_run_self_check(&free_heap)) {
                s_ota.self_check_free_heap_bytes = free_heap;
                ota_enter_wait_confirm(now);
                ESP_LOGW(TAG,
                         "Self-check complete; press EC11 %u times to confirm OTA",
                         (unsigned)MACRO_OTA_CONFIRM_TAP_COUNT);
            } else {
                s_ota.state = OTA_MANAGER_STATE_ROLLBACK_REBOOTING;
                ota_set_error_locked("self-check failed");
                ota_unlock();
                ESP_LOGE(TAG, "Self-check failed; rolling back");
                if (esp_ota_mark_app_invalid_rollback_and_reboot() != ESP_OK) {
                    ESP_LOGE(TAG, "Rollback API failed; forcing reboot");
                    esp_restart();
                }
                return;
            }
        }
    } else if (s_ota.state == OTA_MANAGER_STATE_WAITING_CONFIRM &&
               s_ota.confirm_deadline_tick != 0 &&
               now >= s_ota.confirm_deadline_tick) {
        s_ota.state = OTA_MANAGER_STATE_ROLLBACK_REBOOTING;
        ota_set_error_locked("confirm timeout");
        ota_unlock();
        ESP_LOGE(TAG, "OTA confirmation timeout; rolling back");
        if (esp_ota_mark_app_invalid_rollback_and_reboot() != ESP_OK) {
            ESP_LOGE(TAG, "Rollback API failed; forcing reboot");
            esp_restart();
        }
        return;
    }
    ota_unlock();
}

esp_err_t ota_manager_start_update(const char *url)
{
    if (!s_ota.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!MACRO_OTA_ENABLED) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    const char *chosen_url = url;
    if (chosen_url == NULL || chosen_url[0] == '\0') {
        chosen_url = CONFIG_MACROPAD_OTA_DEFAULT_URL;
    }
    if (chosen_url == NULL || chosen_url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ota_url_is_supported(chosen_url)) {
        ESP_LOGE(TAG, "Only HTTPS OTA URLs are supported");
        return ESP_ERR_NOT_SUPPORTED;
    }

    ota_lock();
    if (s_ota.worker_task != NULL || s_ota.state == OTA_MANAGER_STATE_DOWNLOADING) {
        ota_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ota.state == OTA_MANAGER_STATE_WAITING_CONFIRM ||
        s_ota.state == OTA_MANAGER_STATE_SELF_CHECK_RUNNING) {
        ota_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    strlcpy(s_ota.current_url, chosen_url, sizeof(s_ota.current_url));
    s_ota.state = OTA_MANAGER_STATE_DOWNLOADING;
    ota_set_error_locked(NULL);

    if (xTaskCreate(ota_worker_task, "ota_worker", OTA_TASK_STACK, NULL, 5, &s_ota.worker_task) != pdPASS) {
        s_ota.worker_task = NULL;
        s_ota.state = OTA_MANAGER_STATE_DOWNLOAD_FAILED;
        ota_set_error_locked("task create failed");
        ota_unlock();
        return ESP_ERR_NO_MEM;
    }
    ota_unlock();
    return ESP_OK;
}

bool ota_manager_handle_encoder_taps(uint8_t taps)
{
    bool consumed = false;

    if (!s_ota.initialized || !MACRO_OTA_ENABLED) {
        return false;
    }

    ota_lock();
    if (s_ota.state == OTA_MANAGER_STATE_WAITING_CONFIRM) {
        consumed = true;
        if (taps == (uint8_t)MACRO_OTA_CONFIRM_TAP_COUNT) {
            const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
            if (err == ESP_OK) {
                s_ota.state = OTA_MANAGER_STATE_CONFIRMED;
                s_ota.pending_verify = false;
                ota_set_error_locked(NULL);
                ESP_LOGI(TAG, "OTA image confirmed by EC11 tap x%u", (unsigned)taps);
            } else {
                s_ota.state = OTA_MANAGER_STATE_ROLLBACK_REBOOTING;
                ota_set_error_name_locked(err);
                ota_unlock();
                ESP_LOGE(TAG, "OTA confirm failed: %s; rebooting for rollback", esp_err_to_name(err));
                esp_restart();
                return true;
            }
        } else {
            ESP_LOGW(TAG,
                     "Awaiting OTA confirm: received tap x%u, expected x%u",
                     (unsigned)taps,
                     (unsigned)MACRO_OTA_CONFIRM_TAP_COUNT);
        }
    }
    ota_unlock();
    return consumed;
}

void ota_manager_get_status(ota_manager_status_t *out_status)
{
    if (out_status == NULL) {
        return;
    }

    memset(out_status, 0, sizeof(*out_status));
    out_status->enabled = MACRO_OTA_ENABLED;
    out_status->confirm_tap_count = (uint8_t)MACRO_OTA_CONFIRM_TAP_COUNT;
    out_status->self_check_duration_ms = (uint32_t)MACRO_OTA_SELF_CHECK_DURATION_MS;
    out_status->confirm_timeout_ms = (uint32_t)MACRO_OTA_CONFIRM_TIMEOUT_SEC * 1000U;

    if (!s_ota.initialized) {
        out_status->state = OTA_MANAGER_STATE_DISABLED;
        return;
    }

    const TickType_t now = xTaskGetTickCount();
    ota_lock();
    out_status->state = s_ota.state;
    out_status->pending_verify = s_ota.pending_verify;
    out_status->self_check_free_heap_bytes = s_ota.self_check_free_heap_bytes;
    strlcpy(out_status->current_url, s_ota.current_url, sizeof(out_status->current_url));
    strlcpy(out_status->last_error, s_ota.last_error, sizeof(out_status->last_error));

    if (s_ota.state == OTA_MANAGER_STATE_SELF_CHECK_RUNNING) {
        out_status->self_check_elapsed_ms = (uint32_t)pdTICKS_TO_MS(now - s_ota.self_check_start_tick);
    }
    if (s_ota.state == OTA_MANAGER_STATE_WAITING_CONFIRM) {
        out_status->confirm_remaining_ms =
            (s_ota.confirm_deadline_tick == 0 || now >= s_ota.confirm_deadline_tick)
                ? 0U
                : (uint32_t)pdTICKS_TO_MS(s_ota.confirm_deadline_tick - now);
    }
    ota_unlock();
}

bool ota_manager_get_oled_lines(char *line0,
                                size_t line0_size,
                                char *line1,
                                size_t line1_size,
                                char *line2,
                                size_t line2_size,
                                char *line3,
                                size_t line3_size)
{
    ota_manager_status_t status = {0};
    ota_manager_get_status(&status);

    if (line0_size > 0U) {
        line0[0] = '\0';
    }
    if (line1_size > 0U) {
        line1[0] = '\0';
    }
    if (line2_size > 0U) {
        line2[0] = '\0';
    }
    if (line3_size > 0U) {
        line3[0] = '\0';
    }

    if (!status.enabled) {
        return false;
    }

    switch (status.state) {
    case OTA_MANAGER_STATE_SELF_CHECK_RUNNING:
        (void)snprintf(line0, line0_size, "OTA Self-check");
        (void)snprintf(line1, line1_size, "Running...");
        (void)snprintf(line2,
                       line2_size,
                       "%lus/%lus",
                       (unsigned long)(status.self_check_elapsed_ms / 1000U),
                       (unsigned long)(status.self_check_duration_ms / 1000U));
        (void)snprintf(line3, line3_size, "Please wait");
        return true;
    case OTA_MANAGER_STATE_WAITING_CONFIRM:
        (void)snprintf(line0, line0_size, "OTA verify done");
        (void)snprintf(line1, line1_size, "Press EC11 x%u", (unsigned)status.confirm_tap_count);
        (void)snprintf(line2, line2_size, "to confirm");
        if (status.confirm_timeout_ms > 0U) {
            (void)snprintf(line3,
                           line3_size,
                           "Timeout %lus",
                           (unsigned long)(status.confirm_remaining_ms / 1000U));
        } else {
            (void)snprintf(line3, line3_size, "No timeout");
        }
        return true;
    case OTA_MANAGER_STATE_DOWNLOADING:
        (void)snprintf(line0, line0_size, "OTA updating");
        (void)snprintf(line1, line1_size, "Downloading...");
        (void)snprintf(line2, line2_size, "Please wait");
        if (status.current_url[0] != '\0') {
            (void)snprintf(line3, line3_size, "URL set");
        }
        return true;
    case OTA_MANAGER_STATE_DOWNLOAD_FAILED:
        (void)snprintf(line0, line0_size, "OTA failed");
        (void)snprintf(line1, line1_size, "%s", status.last_error[0] ? status.last_error : "unknown");
        (void)snprintf(line2, line2_size, "Retry from API");
        return true;
    case OTA_MANAGER_STATE_REBOOTING:
        (void)snprintf(line0, line0_size, "OTA done");
        (void)snprintf(line1, line1_size, "Rebooting...");
        return true;
    case OTA_MANAGER_STATE_CONFIRMED:
        (void)snprintf(line0, line0_size, "OTA confirmed");
        (void)snprintf(line1, line1_size, "Rollback canceled");
        return true;
    case OTA_MANAGER_STATE_ROLLBACK_REBOOTING:
        (void)snprintf(line0, line0_size, "OTA rollback");
        (void)snprintf(line1, line1_size, "Rebooting...");
        return true;
    default:
        break;
    }

    return false;
}

