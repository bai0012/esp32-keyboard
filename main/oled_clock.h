#pragma once

#include <time.h>

#include "esp_err.h"

esp_err_t oled_clock_init(void);
esp_err_t oled_clock_render(const struct tm *timeinfo);
