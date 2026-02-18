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
#define OTA_PROGRESS_LOG_STEP_PERCENT 5U
#define OTA_PROGRESS_LOG_INTERVAL_MS 1000U
#define OTA_PROGRESS_BAR_WIDTH 14U
#define OTA_CONFIRM_BANNER_MS 1500U
#define OTA_SELF_CHECK_HARD_MIN_HEAP_BYTES 24576U
#define OTA_SELF_CHECK_MAX_RETRIES 4U
#define OTA_SELF_CHECK_RETRY_INTERVAL_MS 1500U

typedef struct {
    bool initialized;
    SemaphoreHandle_t lock;
    TaskHandle_t worker_task;
    ota_manager_state_t state;
    bool pending_verify;
    TickType_t self_check_start_tick;
    TickType_t self_check_due_tick;
    uint8_t self_check_retry_count;
    TickType_t confirm_start_tick;
    TickType_t confirm_deadline_tick;
    TickType_t confirm_success_tick;
    TickType_t download_start_tick;
    uint32_t self_check_free_heap_bytes;
    uint32_t download_total_bytes;
    uint32_t download_read_bytes;
    uint8_t download_percent;
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

static void ota_reset_download_progress_locked(void)
{
    s_ota.download_total_bytes = 0U;
    s_ota.download_read_bytes = 0U;
    s_ota.download_percent = 0U;
    s_ota.download_start_tick = 0;
}

static void ota_update_download_progress_locked(uint32_t read_bytes, uint32_t total_bytes, TickType_t now)
{
    s_ota.download_read_bytes = read_bytes;
    s_ota.download_total_bytes = total_bytes;
    if (s_ota.download_start_tick == 0) {
        s_ota.download_start_tick = now;
    }

    if (total_bytes > 0U) {
        uint32_t pct = (read_bytes * 100U) / total_bytes;
        if (pct > 100U) {
            pct = 100U;
        }
        s_ota.download_percent = (uint8_t)pct;
    } else {
        s_ota.download_percent = 0U;
    }
}

static void ota_format_progress_bar(char *out, size_t out_size, uint8_t percent)
{
    if (out == NULL || out_size < 4U) {
        return;
    }
    out[0] = '\0';

    const uint32_t filled = ((uint32_t)percent * OTA_PROGRESS_BAR_WIDTH) / 100U;
    size_t w = 0;
    out[w++] = '[';
    for (uint32_t i = 0; i < OTA_PROGRESS_BAR_WIDTH && (w + 2U) < out_size; ++i) {
        out[w++] = (i < filled) ? '#' : '.';
    }
    if ((w + 1U) < out_size) {
        out[w++] = ']';
    }
    out[w] = '\0';
}

static bool ota_url_is_https(const char *url)
{
    if (url == NULL) {
        return false;
    }
    return strncmp(url, "https://", 8) == 0;
}

static bool ota_url_is_http(const char *url)
{
    if (url == NULL) {
        return false;
    }
    return strncmp(url, "http://", 7) == 0;
}

static bool ota_url_is_supported(const char *url)
{
    if (ota_url_is_https(url)) {
        return true;
    }
    if (MACRO_OTA_ALLOW_HTTP && ota_url_is_http(url)) {
        return true;
    }
    return false;
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
    ota_reset_download_progress_locked();
    ota_unlock();

    const bool is_https = ota_url_is_https(url);
    esp_http_client_config_t http_cfg = {
        .url = url,
        .timeout_ms = CONFIG_MACROPAD_OTA_HTTP_TIMEOUT_MS,
        .keep_alive_enable = true,
    };
    if (is_https && !MACRO_OTA_SKIP_CERT_VERIFY) {
        http_cfg.crt_bundle_attach = esp_crt_bundle_attach;
    } else if (is_https && MACRO_OTA_SKIP_CERT_VERIFY) {
        http_cfg.skip_cert_common_name_check = true;
        ESP_LOGW(TAG, "OTA HTTPS certificate verification is DISABLED by config");
    } else if (ota_url_is_http(url)) {
        ESP_LOGW(TAG, "OTA over plain HTTP is enabled by config (insecure)");
    }
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    ESP_LOGI(TAG, "Starting OTA from: %s", url);
    esp_https_ota_handle_t ota_handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_cfg, &ota_handle);
    if (err != ESP_OK) {
        ota_lock();
        s_ota.state = OTA_MANAGER_STATE_DOWNLOAD_FAILED;
        ota_set_error_name_locked(err);
        s_ota.worker_task = NULL;
        ota_unlock();
        ESP_LOGE(TAG, "OTA begin failed: %s", esp_err_to_name(err));
        vTaskDelete(NULL);
        return;
    }

    uint8_t next_pct_log = OTA_PROGRESS_LOG_STEP_PERCENT;
    TickType_t last_log_tick = 0;
    while ((err = esp_https_ota_perform(ota_handle)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        const int read_int = esp_https_ota_get_image_len_read(ota_handle);
        const int total_int = esp_https_ota_get_image_size(ota_handle);
        const uint32_t read_bytes = (read_int > 0) ? (uint32_t)read_int : 0U;
        const uint32_t total_bytes = (total_int > 0) ? (uint32_t)total_int : 0U;
        const TickType_t now = xTaskGetTickCount();

        ota_lock();
        ota_update_download_progress_locked(read_bytes, total_bytes, now);
        const uint8_t pct = s_ota.download_percent;
        ota_unlock();

        if (total_bytes > 0U) {
            if (pct >= next_pct_log ||
                (last_log_tick == 0 || (now - last_log_tick) >= pdMS_TO_TICKS(OTA_PROGRESS_LOG_INTERVAL_MS))) {
                ESP_LOGI(TAG,
                         "OTA progress: %u%% (%" PRIu32 "/%" PRIu32 " bytes)",
                         (unsigned)pct,
                         read_bytes,
                         total_bytes);
                while (next_pct_log <= pct && next_pct_log < 100U) {
                    next_pct_log = (uint8_t)(next_pct_log + OTA_PROGRESS_LOG_STEP_PERCENT);
                }
                last_log_tick = now;
            }
        } else if (last_log_tick == 0 || (now - last_log_tick) >= pdMS_TO_TICKS(OTA_PROGRESS_LOG_INTERVAL_MS)) {
            ESP_LOGI(TAG, "OTA progress: %" PRIu32 " bytes received", read_bytes);
            last_log_tick = now;
        }
    }

    if (err == ESP_OK && !esp_https_ota_is_complete_data_received(ota_handle)) {
        err = ESP_FAIL;
        ESP_LOGE(TAG, "Complete OTA image was not received");
    }

    if (err == ESP_OK) {
        err = esp_https_ota_finish(ota_handle);
    } else {
        (void)esp_https_ota_abort(ota_handle);
    }

    if (err == ESP_OK) {
        ota_lock();
        s_ota.state = OTA_MANAGER_STATE_REBOOTING;
        s_ota.worker_task = NULL;
        ota_set_error_locked(NULL);
        ota_update_download_progress_locked(s_ota.download_total_bytes,
                                            s_ota.download_total_bytes,
                                            xTaskGetTickCount());
        ota_unlock();
        ESP_LOGI(TAG, "OTA progress: 100%%");
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
    s_ota.self_check_due_tick = 0;
    s_ota.self_check_retry_count = 0;
    s_ota.confirm_start_tick = now;
    s_ota.confirm_success_tick = 0;
    if (MACRO_OTA_CONFIRM_TIMEOUT_SEC > 0) {
        s_ota.confirm_deadline_tick = now + pdMS_TO_TICKS((uint32_t)MACRO_OTA_CONFIRM_TIMEOUT_SEC * 1000U);
    } else {
        s_ota.confirm_deadline_tick = 0;
    }
}

static bool ota_run_self_check(uint32_t *free_heap_out)
{
    const uint32_t free_heap = (uint32_t)esp_get_free_heap_size();
    const uint32_t min_free_heap = (uint32_t)esp_get_minimum_free_heap_size();
    const UBaseType_t task_count = uxTaskGetNumberOfTasks();
    if (free_heap_out != NULL) {
        *free_heap_out = free_heap;
    }

    if (free_heap < OTA_SELF_CHECK_HARD_MIN_HEAP_BYTES) {
        ESP_LOGE(TAG,
                 "Self-check failed: free heap=%" PRIu32 " < hard-min=%u (warn-min=%u, min-ever=%" PRIu32 ")",
                 free_heap,
                 (unsigned)OTA_SELF_CHECK_HARD_MIN_HEAP_BYTES,
                 (unsigned)MACRO_OTA_SELF_CHECK_MIN_HEAP_BYTES,
                 min_free_heap);
        return false;
    }
    if (task_count < 3U) {
        ESP_LOGE(TAG,
                 "Self-check failed: task_count=%u < 3 (free_heap=%" PRIu32 ", min-ever=%" PRIu32 ")",
                 (unsigned)task_count,
                 free_heap,
                 min_free_heap);
        return false;
    }

    if (free_heap < (uint32_t)MACRO_OTA_SELF_CHECK_MIN_HEAP_BYTES) {
        ESP_LOGW(TAG,
                 "Self-check warning: free heap=%" PRIu32 " < warn-min=%u (hard-min=%u, min-ever=%" PRIu32 ")",
                 free_heap,
                 (unsigned)MACRO_OTA_SELF_CHECK_MIN_HEAP_BYTES,
                 (unsigned)OTA_SELF_CHECK_HARD_MIN_HEAP_BYTES,
                 min_free_heap);
    }

    ESP_LOGI(TAG,
             "Self-check passed: free_heap=%" PRIu32 " min_ever=%" PRIu32 " task_count=%u",
             free_heap,
             min_free_heap,
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
    ota_reset_download_progress_locked();
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running != NULL) {
        esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
        const esp_err_t state_err = esp_ota_get_state_partition(running, &state);
        if (state_err == ESP_OK) {
            ESP_LOGI(TAG,
                     "Running OTA image state=%d (0:new 1:pending_verify 2:valid 3:invalid 4:aborted 5:undefined)",
                     (int)state);
        } else {
            ESP_LOGW(TAG, "esp_ota_get_state_partition failed: %s", esp_err_to_name(state_err));
        }
        if (state_err == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
            const TickType_t now = xTaskGetTickCount();
            s_ota.pending_verify = true;
            s_ota.state = OTA_MANAGER_STATE_SELF_CHECK_RUNNING;
            s_ota.self_check_start_tick = now;
            s_ota.self_check_due_tick = now + pdMS_TO_TICKS((uint32_t)MACRO_OTA_SELF_CHECK_DURATION_MS);
            s_ota.self_check_retry_count = 0;
            ota_set_error_locked(NULL);
            ESP_LOGW(TAG, "Running OTA image is pending verify; starting self-check");
        }
    }

    ESP_LOGI(TAG,
             "ready enabled=%d allow_http=%d skip_cert_verify=%d confirm_taps=%u timeout=%us default_url=%s",
             MACRO_OTA_ENABLED,
             MACRO_OTA_ALLOW_HTTP,
             MACRO_OTA_SKIP_CERT_VERIFY,
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
        if (s_ota.self_check_due_tick == 0 || now >= s_ota.self_check_due_tick) {
            uint32_t free_heap = 0;
            if (ota_run_self_check(&free_heap)) {
                s_ota.self_check_free_heap_bytes = free_heap;
                ota_enter_wait_confirm(now);
                ESP_LOGW(TAG,
                         "Self-check complete; press EC11 %u times to confirm OTA",
                         (unsigned)MACRO_OTA_CONFIRM_TAP_COUNT);
            } else {
                if (s_ota.self_check_retry_count < OTA_SELF_CHECK_MAX_RETRIES) {
                    s_ota.self_check_retry_count++;
                    s_ota.self_check_due_tick = now + pdMS_TO_TICKS(OTA_SELF_CHECK_RETRY_INTERVAL_MS);
                    ESP_LOGW(TAG,
                             "Self-check retry %u/%u scheduled in %u ms",
                             (unsigned)s_ota.self_check_retry_count,
                             (unsigned)OTA_SELF_CHECK_MAX_RETRIES,
                             (unsigned)OTA_SELF_CHECK_RETRY_INTERVAL_MS);
                } else {
                    s_ota.state = OTA_MANAGER_STATE_ROLLBACK_REBOOTING;
                    ota_set_error_locked("self-check failed");
                    ota_unlock();
                    ESP_LOGE(TAG, "Self-check failed after retries; rolling back");
                    if (esp_ota_mark_app_invalid_rollback_and_reboot() != ESP_OK) {
                        ESP_LOGE(TAG, "Rollback API failed; forcing reboot");
                        esp_restart();
                    }
                    return;
                }
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
    } else if (s_ota.state == OTA_MANAGER_STATE_CONFIRMED) {
        const TickType_t elapsed = now - s_ota.confirm_success_tick;
        if (elapsed >= pdMS_TO_TICKS(OTA_CONFIRM_BANNER_MS)) {
            s_ota.state = OTA_MANAGER_STATE_READY;
            s_ota.confirm_start_tick = 0;
            s_ota.confirm_deadline_tick = 0;
            s_ota.confirm_success_tick = 0;
            s_ota.current_url[0] = '\0';
            ota_set_error_locked(NULL);
        }
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
        ESP_LOGE(TAG, "Unsupported OTA URL scheme: %s", chosen_url);
        ESP_LOGE(TAG, "Allowed schemes: https://%s", MACRO_OTA_ALLOW_HTTP ? " and http://" : "");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (ota_url_is_https(chosen_url) && MACRO_OTA_SKIP_CERT_VERIFY) {
#if !defined(CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY) || !defined(CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP)
        ESP_LOGE(TAG,
                 "skip_cert_verify requires CONFIG_ESP_TLS_SKIP_SERVER_CERT_VERIFY=y and "
                 "CONFIG_ESP_HTTPS_OTA_ALLOW_HTTP=y");
        return ESP_ERR_NOT_SUPPORTED;
#endif
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
    s_ota.confirm_start_tick = 0;
    s_ota.confirm_deadline_tick = 0;
    s_ota.confirm_success_tick = 0;
    s_ota.self_check_due_tick = 0;
    s_ota.self_check_retry_count = 0;
    ota_reset_download_progress_locked();
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
                s_ota.confirm_success_tick = xTaskGetTickCount();
                s_ota.confirm_deadline_tick = 0;
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
    out_status->download_total_bytes = s_ota.download_total_bytes;
    out_status->download_read_bytes = s_ota.download_read_bytes;
    out_status->download_percent = s_ota.download_percent;
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
    if (s_ota.download_start_tick != 0 && now >= s_ota.download_start_tick) {
        out_status->download_elapsed_ms = (uint32_t)pdTICKS_TO_MS(now - s_ota.download_start_tick);
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
    {
        char bar[24] = {0};
        ota_format_progress_bar(bar, sizeof(bar), status.download_percent);

        (void)snprintf(line0, line0_size, "OTA updating");
        if (status.download_total_bytes > 0U) {
            (void)snprintf(line1, line1_size, "%s %3u%%", bar, (unsigned)status.download_percent);
            (void)snprintf(line2,
                           line2_size,
                           "%" PRIu32 "K/%" PRIu32 "K",
                           status.download_read_bytes / 1024U,
                           status.download_total_bytes / 1024U);
        } else {
            (void)snprintf(line1, line1_size, "Receiving...");
            (void)snprintf(line2, line2_size, "%" PRIu32 "K", status.download_read_bytes / 1024U);
        }

        if (status.download_elapsed_ms > 0U) {
            const uint32_t kbps = (status.download_read_bytes * 1000U) /
                                  status.download_elapsed_ms / 1024U;
            (void)snprintf(line3, line3_size, "%" PRIu32 " KB/s", kbps);
        } else {
            (void)snprintf(line3, line3_size, "Please wait");
        }
        return true;
    }
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
        (void)snprintf(line2, line2_size, "Returning...");
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
