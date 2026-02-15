#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

esp_err_t buzzer_init(void);
void buzzer_update(TickType_t now);
void buzzer_stop(void);
void buzzer_set_enabled(bool enabled);
bool buzzer_is_enabled(void);
bool buzzer_toggle_enabled(void);

esp_err_t buzzer_play_tone(uint16_t frequency_hz, uint16_t duration_ms);
esp_err_t buzzer_play_tone_ex(uint16_t frequency_hz, uint16_t duration_ms, uint16_t silence_ms);
esp_err_t buzzer_play_rtttl(const char *rtttl);

void buzzer_play_startup(void);
void buzzer_play_keypress(void);
void buzzer_play_layer_switch(uint8_t layer_index);
void buzzer_play_encoder_step(int8_t direction);
