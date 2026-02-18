#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"

#include "esp_err.h"

typedef struct {
    bool initialized;
    bool connected;
    bool advertising;
    bool bonded;
    bool pairing_window_active;
    uint32_t pairing_remaining_ms;
    uint32_t passkey;
    char peer_addr[18];
} hid_ble_backend_status_t;

esp_err_t hid_ble_backend_init(const char *device_name, uint32_t passkey);
void hid_ble_backend_poll(TickType_t now);
esp_err_t hid_ble_backend_send_keyboard_report(const bool *key_pressed, uint8_t active_layer);
esp_err_t hid_ble_backend_send_consumer_report(uint16_t usage);
esp_err_t hid_ble_backend_start_pairing_window(uint32_t timeout_ms);
esp_err_t hid_ble_backend_clear_bond(void);
bool hid_ble_backend_is_ready(void);
void hid_ble_backend_get_status(hid_ble_backend_status_t *out_status);
const char *hid_ble_backend_last_init_step(void);
esp_err_t hid_ble_backend_last_init_error(void);
