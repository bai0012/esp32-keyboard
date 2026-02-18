#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t hid_usb_backend_init(bool enable_hid_keyboard);
void hid_usb_backend_send_keyboard_report(const bool *key_pressed, uint8_t active_layer);
void hid_usb_backend_send_consumer_report(uint16_t usage);
bool hid_usb_backend_mounted(void);
bool hid_usb_backend_hid_ready(void);
bool hid_usb_backend_cdc_connected(void);
