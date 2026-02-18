#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"

#include "esp_err.h"

typedef enum {
    HID_MODE_USB = 0,
    HID_MODE_BLE = 1,
} hid_mode_t;

typedef struct {
    bool initialized;
    hid_mode_t mode;
    bool mode_switch_pending;
    hid_mode_t mode_switch_target;

    bool usb_mounted;
    bool usb_hid_ready;
    bool cdc_connected;

    bool ble_enabled;
    bool ble_initialized;
    bool ble_connected;
    bool ble_advertising;
    bool ble_bonded;
    bool ble_init_failed;
    esp_err_t ble_init_error;
    bool ble_pairing_window_active;
    uint32_t ble_pairing_remaining_ms;
    uint32_t ble_passkey;
    char ble_peer_addr[18];
} hid_transport_status_t;

esp_err_t hid_transport_init(void);
void hid_transport_poll(TickType_t now);

hid_mode_t hid_transport_get_mode(void);
bool hid_transport_is_link_ready(void);
bool hid_transport_cdc_connected(void);

void hid_transport_send_keyboard_report(const bool *key_pressed, uint8_t active_layer);
void hid_transport_send_consumer_report(uint16_t usage);

esp_err_t hid_transport_request_mode_switch(hid_mode_t target);
esp_err_t hid_transport_start_pairing_window(uint32_t timeout_ms);
esp_err_t hid_transport_clear_bond(void);

bool hid_transport_get_status(hid_transport_status_t *out_status);
bool hid_transport_get_oled_lines(char *line0,
                                  size_t line0_size,
                                  char *line1,
                                  size_t line1_size,
                                  char *line2,
                                  size_t line2_size,
                                  char *line3,
                                  size_t line3_size);
