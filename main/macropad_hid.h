#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t macropad_usb_init_mode(bool enable_hid_keyboard);
esp_err_t macropad_usb_init(void);
void macropad_send_consumer_report(uint16_t usage);
void macropad_send_keyboard_report(const bool *key_pressed, uint8_t active_layer);
bool macropad_usb_hid_enabled(void);
bool macropad_usb_mounted(void);
bool macropad_usb_hid_ready(void);
bool macropad_usb_cdc_connected(void);
