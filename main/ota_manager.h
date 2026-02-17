#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"

#include "esp_err.h"

typedef enum {
    OTA_MANAGER_STATE_DISABLED = 0,
    OTA_MANAGER_STATE_READY,
    OTA_MANAGER_STATE_DOWNLOADING,
    OTA_MANAGER_STATE_DOWNLOAD_FAILED,
    OTA_MANAGER_STATE_REBOOTING,
    OTA_MANAGER_STATE_SELF_CHECK_RUNNING,
    OTA_MANAGER_STATE_WAITING_CONFIRM,
    OTA_MANAGER_STATE_CONFIRMED,
    OTA_MANAGER_STATE_ROLLBACK_REBOOTING,
} ota_manager_state_t;

typedef struct {
    bool enabled;
    bool pending_verify;
    ota_manager_state_t state;
    uint8_t confirm_tap_count;
    uint32_t self_check_duration_ms;
    uint32_t self_check_elapsed_ms;
    uint32_t confirm_timeout_ms;
    uint32_t confirm_remaining_ms;
    uint32_t self_check_free_heap_bytes;
    char current_url[192];
    char last_error[96];
} ota_manager_status_t;

esp_err_t ota_manager_init(void);
void ota_manager_poll(TickType_t now);

esp_err_t ota_manager_start_update(const char *url);
bool ota_manager_handle_encoder_taps(uint8_t taps);

void ota_manager_get_status(ota_manager_status_t *out_status);
const char *ota_manager_state_name(ota_manager_state_t state);

bool ota_manager_get_oled_lines(char *line0,
                                size_t line0_size,
                                char *line1,
                                size_t line1_size,
                                char *line2,
                                size_t line2_size,
                                char *line3,
                                size_t line3_size);
