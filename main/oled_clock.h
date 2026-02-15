#pragma once

#include <time.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t oled_clock_init(void);
esp_err_t oled_clock_render(const struct tm *timeinfo, int8_t shift_x, int8_t shift_y);
esp_err_t oled_clock_set_brightness_percent(uint8_t percent);
esp_err_t oled_clock_set_display_enabled(bool enabled);
esp_err_t oled_clock_set_inverted(bool inverted);
