#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t macropad_usb_init(void);
void macropad_send_consumer_report(uint16_t usage);
void macropad_send_keyboard_report(const bool *key_pressed, uint8_t active_layer);
