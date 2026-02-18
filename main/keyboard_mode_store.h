#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    KEYBOARD_MODE_USB = 0,
    KEYBOARD_MODE_BLE = 1,
} keyboard_mode_t;

esp_err_t keyboard_mode_store_load(keyboard_mode_t *out_mode, bool *out_valid);
esp_err_t keyboard_mode_store_save(keyboard_mode_t mode);
