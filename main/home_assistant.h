#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t home_assistant_init(void);
bool home_assistant_is_enabled(void);
bool home_assistant_get_display_text(char *out, size_t out_size, uint32_t *age_ms);
esp_err_t home_assistant_trigger_default_control(void);

void home_assistant_notify_layer_switch(uint8_t layer_index);
void home_assistant_notify_key_event(uint8_t layer_index,
                                     uint8_t key_index,
                                     bool pressed,
                                     uint16_t usage,
                                     const char *key_name);
void home_assistant_notify_encoder_step(uint8_t layer_index, int32_t steps, uint16_t usage);
void home_assistant_notify_touch_swipe(uint8_t layer_index, bool left_to_right, uint16_t usage);

/* Queue a pre-serialized JSON object payload to HA event bus (future extension API). */
esp_err_t home_assistant_queue_custom_event(const char *event_suffix, const char *json_payload);
