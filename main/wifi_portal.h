#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

esp_err_t wifi_portal_init(void);
esp_err_t wifi_portal_start(void);
void wifi_portal_poll(void);

bool wifi_portal_is_active(void);
bool wifi_portal_is_connected(void);
esp_err_t wifi_portal_cancel(void);

bool wifi_portal_get_oled_lines(char *line0,
                                size_t line0_size,
                                char *line1,
                                size_t line1_size,
                                char *line2,
                                size_t line2_size,
                                char *line3,
                                size_t line3_size);
