#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "hid_transport.h"

typedef esp_err_t (*web_service_set_layer_cb_t)(uint8_t layer_index);
typedef esp_err_t (*web_service_set_buzzer_cb_t)(bool enabled);
typedef esp_err_t (*web_service_send_consumer_cb_t)(uint16_t usage);
typedef esp_err_t (*web_service_set_keyboard_mode_cb_t)(hid_mode_t mode);
typedef esp_err_t (*web_service_ble_pair_cb_t)(uint32_t timeout_sec);
typedef esp_err_t (*web_service_ble_clear_bond_cb_t)(void);

typedef struct {
    web_service_set_layer_cb_t set_layer;
    web_service_set_buzzer_cb_t set_buzzer;
    web_service_send_consumer_cb_t send_consumer;
    web_service_set_keyboard_mode_cb_t set_keyboard_mode;
    web_service_ble_pair_cb_t start_ble_pairing;
    web_service_ble_clear_bond_cb_t clear_ble_bond;
} web_service_control_if_t;

esp_err_t web_service_init(void);
esp_err_t web_service_register_control(const web_service_control_if_t *iface);
void web_service_poll(void);
bool web_service_is_running(void);

void web_service_mark_user_activity(void);
void web_service_set_active_layer(uint8_t layer_index);
void web_service_record_key_event(uint8_t key_index, bool pressed, uint16_t usage, const char *key_name);
void web_service_record_encoder_step(int32_t steps, uint16_t usage);
void web_service_record_touch_swipe(uint8_t layer_index, bool left_to_right, uint16_t usage);
