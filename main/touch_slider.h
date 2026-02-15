#pragma once

#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

typedef void (*touch_consumer_send_fn)(uint16_t usage);

esp_err_t touch_slider_init(void);
void touch_slider_update(TickType_t now, uint8_t active_layer, touch_consumer_send_fn send_consumer);
