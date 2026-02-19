#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/pulse_cnt.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "led_strip.h"

#include "keymap_config.h"
#include "sdkconfig.h"

#include "buzzer.h"
#include "hid_transport.h"
#include "home_assistant.h"
#include "log_store.h"
#include "oled.h"
#include "oled_animation_assets.h"
#include "ota_manager.h"
#include "touch_slider.h"
#include "wifi_portal.h"
#include "web_service.h"

#define TAG "MACROPAD"

#define KEY_COUNT MACRO_KEY_COUNT
#define DEBOUNCE_MS 20
#define SCAN_INTERVAL_MS 5
#define ENCODER_DETENT_PULSES 2

#define EC11_GPIO_A GPIO_NUM_4
#define EC11_GPIO_B GPIO_NUM_5
#define EC11_GPIO_BUTTON GPIO_NUM_6

#define LED_STRIP_GPIO GPIO_NUM_38
#define LED_STRIP_COUNT 15
#define LED_STATUS_DEBOUNCE_MS 120
#define CDC_LOG_GATE_TIMEOUT_MS 2500
#define BOOT_ANIMATION_MAX_FRAMES 240
#define BOOT_ANIMATION_MAX_TOTAL_MS 8000
#define BOOT_ANIMATION_MIN_FRAME_MS 20
#define BOOT_ANIMATION_MAX_FRAME_MS 1000
#define HA_DISPLAY_STALE_MS 120000U
#define BLE_PAIRING_TAP_COUNT 7

typedef struct {
    bool stable_level;
    bool last_raw;
    TickType_t last_transition_tick;
} debounce_state_t;

static debounce_state_t s_key_db[KEY_COUNT];
static debounce_state_t s_encoder_btn_db;
static bool s_key_pressed[KEY_COUNT];
static uint8_t s_active_layer = 0;
static uint8_t s_encoder_tap_count = 0;
static TickType_t s_encoder_last_tap_tick = 0;
static bool s_encoder_single_pending = false;
static TickType_t s_encoder_single_due_tick = 0;

static pcnt_unit_handle_t s_pcnt_unit;
static led_strip_handle_t s_led_strip;
static uint8_t s_led_last_frame[LED_STRIP_COUNT][3];
static bool s_led_frame_valid = false;
static debounce_state_t s_usb_mounted_db;
static debounce_state_t s_usb_hid_ready_db;

static bool s_sntp_started;
static volatile TickType_t s_last_user_activity_tick = 0;
static TickType_t s_log_gate_start_tick = 0;
static bool s_log_gate_armed = false;

static bool debounce_update(debounce_state_t *state,
                            bool raw_pressed,
                            TickType_t now,
                            TickType_t debounce_ticks);
static void sntp_time_sync_notification_cb(struct timeval *tv);

static inline bool cdc_log_ready(void)
{
    if (hid_transport_cdc_connected()) {
        return true;
    }

    if (!s_log_gate_armed) {
        return false;
    }

    return (xTaskGetTickCount() - s_log_gate_start_tick) >=
           pdMS_TO_TICKS(CDC_LOG_GATE_TIMEOUT_MS);
}

#define APP_LOGI(fmt, ...)            \
    do {                              \
        if (cdc_log_ready()) {        \
            ESP_LOGI(TAG, fmt, ##__VA_ARGS__); \
        }                             \
    } while (0)

static inline void mark_user_activity(TickType_t now)
{
    s_last_user_activity_tick = now;
    web_service_mark_user_activity();
}

static void play_boot_animation(void)
{
    if (g_oled_boot_animation.frame_count == 0U || g_oled_boot_animation.frames == NULL) {
        ESP_LOGW(TAG, "Boot animation missing/empty, skip");
        return;
    }

    const uint16_t frame_count = (g_oled_boot_animation.frame_count > BOOT_ANIMATION_MAX_FRAMES)
        ? BOOT_ANIMATION_MAX_FRAMES
        : g_oled_boot_animation.frame_count;

    uint32_t elapsed_ms = 0U;
    for (uint16_t i = 0; i < frame_count; ++i) {
        esp_err_t err = oled_render_animation_frame_centered(&g_oled_boot_animation, i, 0, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Boot animation frame %u failed: %s", (unsigned)i, esp_err_to_name(err));
            break;
        }

        uint16_t frame_ms = g_oled_boot_animation.frames[i].duration_ms;
        if (frame_ms < BOOT_ANIMATION_MIN_FRAME_MS) {
            frame_ms = BOOT_ANIMATION_MIN_FRAME_MS;
        } else if (frame_ms > BOOT_ANIMATION_MAX_FRAME_MS) {
            frame_ms = BOOT_ANIMATION_MAX_FRAME_MS;
        }

        if ((elapsed_ms + frame_ms) > BOOT_ANIMATION_MAX_TOTAL_MS) {
            ESP_LOGW(TAG, "Boot animation stopped at %u ms safety limit", (unsigned)elapsed_ms);
            break;
        }

        elapsed_ms += frame_ms;
        vTaskDelay(pdMS_TO_TICKS(frame_ms));
    }
}

static int8_t random_shift_px(int8_t range)
{
    if (range <= 0) {
        return 0;
    }
    const uint32_t span = (uint32_t)((range * 2) + 1);
    return (int8_t)((int32_t)(esp_random() % span) - range);
}

static bool is_time_synchronized(const struct tm *timeinfo)
{
    /* SNTP-unsynced localtime is typically 1970; gate hourly inversion until real time is available. */
    return timeinfo->tm_year >= (2020 - 1900);
}

static void send_consumer_report_with_activity(uint16_t usage)
{
    if (usage != 0) {
        mark_user_activity(xTaskGetTickCount());
    }
    hid_transport_send_consumer_report(usage);
}

static void notify_touch_swipe(uint8_t layer_index, bool left_to_right, uint16_t usage)
{
    home_assistant_notify_touch_swipe(layer_index, left_to_right, usage);
    web_service_record_touch_swipe(layer_index, left_to_right, usage);
}

static inline const macro_action_config_t *scan_key_cfg(size_t idx)
{
    return &g_macro_keymap_layers[0][idx];
}

static inline const macro_action_config_t *active_key_cfg(size_t idx)
{
    return &g_macro_keymap_layers[s_active_layer][idx];
}

static bool is_pressed(const macro_action_config_t *cfg)
{
    const int level = gpio_get_level(cfg->gpio);
    return cfg->active_low ? (level == 0) : (level != 0);
}

static void set_active_layer(uint8_t layer)
{
    if (layer >= MACRO_LAYER_COUNT || layer == s_active_layer) {
        return;
    }

    s_active_layer = layer;
    APP_LOGI("Switched to Layer %u", (unsigned)s_active_layer + 1);
    buzzer_play_layer_switch(s_active_layer);
    home_assistant_notify_layer_switch(s_active_layer);
    web_service_set_active_layer(s_active_layer);
    hid_transport_send_keyboard_report(s_key_pressed, s_active_layer);
}

static uint8_t apply_brightness(uint8_t value, uint8_t brightness)
{
    return (uint8_t)((uint16_t)value * brightness / 255U);
}

static inline uint8_t dim_indicator(uint8_t value)
{
    return apply_brightness(value, MACRO_LED_INDICATOR_BRIGHTNESS);
}

static inline uint8_t dim_key(uint8_t value)
{
    return apply_brightness(value, MACRO_LED_KEY_BRIGHTNESS);
}

static esp_err_t update_key_leds(void)
{
    if (s_led_strip == NULL) {
        return ESP_OK;
    }

    const TickType_t now = xTaskGetTickCount();
    const TickType_t status_debounce_ticks = pdMS_TO_TICKS(LED_STATUS_DEBOUNCE_MS);
    const TickType_t led_off_timeout_ticks = pdMS_TO_TICKS((uint32_t)MACRO_LED_OFF_TIMEOUT_SEC * 1000U);
    const bool leds_off_by_idle =
        (led_off_timeout_ticks > 0) &&
        ((now - s_last_user_activity_tick) >= led_off_timeout_ticks);
    hid_transport_status_t hid_status = {0};
    (void)hid_transport_get_status(&hid_status);
    const bool mounted_state = hid_status.usb_mounted;
    const bool link_ready_state = (hid_status.mode == HID_MODE_USB) ? hid_status.usb_hid_ready : hid_status.ble_connected;
    (void)debounce_update(&s_usb_mounted_db, mounted_state, now, status_debounce_ticks);
    (void)debounce_update(&s_usb_hid_ready_db, link_ready_state, now, status_debounce_ticks);

    const macro_rgb_t *layer_color = &g_layer_backlight_color[s_active_layer];
    const uint8_t layer_a_r = layer_color->r;
    const uint8_t layer_a_g = layer_color->g;
    const uint8_t layer_a_b = layer_color->b;
    const uint8_t key_dim_r = (uint8_t)(((uint16_t)layer_color->r * MACRO_LAYER_KEY_DIM_SCALE) / 255U);
    const uint8_t key_dim_g = (uint8_t)(((uint16_t)layer_color->g * MACRO_LAYER_KEY_DIM_SCALE) / 255U);
    const uint8_t key_dim_b = (uint8_t)(((uint16_t)layer_color->b * MACRO_LAYER_KEY_DIM_SCALE) / 255U);
    const uint8_t key_active_r = (uint8_t)(((uint16_t)layer_color->r * MACRO_LAYER_KEY_ACTIVE_SCALE) / 255U);
    const uint8_t key_active_g = (uint8_t)(((uint16_t)layer_color->g * MACRO_LAYER_KEY_ACTIVE_SCALE) / 255U);
    const uint8_t key_active_b = (uint8_t)(((uint16_t)layer_color->b * MACRO_LAYER_KEY_ACTIVE_SCALE) / 255U);
    uint8_t frame[LED_STRIP_COUNT][3] = {0};

    if (!leds_off_by_idle) {
        if (s_usb_mounted_db.stable_level) {
            frame[0][0] = dim_indicator(0);
            frame[0][1] = dim_indicator(40);
            frame[0][2] = dim_indicator(0);
        }
        if (s_usb_hid_ready_db.stable_level) {
            frame[1][0] = dim_indicator(0);
            frame[1][1] = dim_indicator(0);
            frame[1][2] = dim_indicator(40);
        }
        frame[2][0] = dim_indicator(layer_a_r);
        frame[2][1] = dim_indicator(layer_a_g);
        frame[2][2] = dim_indicator(layer_a_b);

        for (size_t i = 0; i < KEY_COUNT; ++i) {
            const macro_action_config_t *cfg = active_key_cfg(i);
            if (cfg->led_index >= LED_STRIP_COUNT) {
                continue;
            }

            frame[cfg->led_index][0] = dim_key(key_dim_r);
            frame[cfg->led_index][1] = dim_key(key_dim_g);
            frame[cfg->led_index][2] = dim_key(key_dim_b);

            if (s_key_pressed[i]) {
                frame[cfg->led_index][0] = dim_key(key_active_r);
                frame[cfg->led_index][1] = dim_key(key_active_g);
                frame[cfg->led_index][2] = dim_key(key_active_b);
            }
        }
    }

    if (s_led_frame_valid && memcmp(frame, s_led_last_frame, sizeof(frame)) == 0) {
        return ESP_OK;
    }

    for (size_t i = 0; i < LED_STRIP_COUNT; ++i) {
        if (!s_led_frame_valid || memcmp(frame[i], s_led_last_frame[i], sizeof(frame[i])) != 0) {
            ESP_RETURN_ON_ERROR(led_strip_set_pixel(s_led_strip,
                                                    i,
                                                    frame[i][0],
                                                    frame[i][1],
                                                    frame[i][2]),
                                TAG, "set led %u failed", (unsigned)i);
        }
    }

    memcpy(s_led_last_frame, frame, sizeof(frame));
    s_led_frame_valid = true;
    return led_strip_refresh(s_led_strip);
}

static bool debounce_update(debounce_state_t *state,
                            bool raw_pressed,
                            TickType_t now,
                            TickType_t debounce_ticks)
{
    if (raw_pressed != state->last_raw) {
        state->last_raw = raw_pressed;
        state->last_transition_tick = now;
    }

    if ((now - state->last_transition_tick) >= debounce_ticks && state->stable_level != state->last_raw) {
        state->stable_level = state->last_raw;
        return true;
    }

    return false;
}

static void sntp_ip_event_handler(void *arg,
                                  esp_event_base_t event_base,
                                  int32_t event_id,
                                  void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        if (!s_sntp_started) {
            APP_LOGI("Starting SNTP with server: %s", CONFIG_MACROPAD_NTP_SERVER);
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, (char *)CONFIG_MACROPAD_NTP_SERVER);
            esp_sntp_set_time_sync_notification_cb(sntp_time_sync_notification_cb);
            esp_sntp_init();
            s_sntp_started = true;
        }
    }
}

static void sntp_time_sync_notification_cb(struct timeval *tv)
{
    (void)tv;
    log_store_mark_time_synced();
    APP_LOGI("SNTP time synchronized");
}

static esp_err_t register_sntp_handler(void)
{
    return esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &sntp_ip_event_handler, NULL);
}

static esp_err_t init_keys(void)
{
    uint64_t pin_mask = 0;
    for (size_t i = 0; i < KEY_COUNT; ++i) {
        pin_mask |= (1ULL << scan_key_cfg(i)->gpio);
    }
    pin_mask |= (1ULL << EC11_GPIO_BUTTON);
    pin_mask |= (1ULL << EC11_GPIO_A);
    pin_mask |= (1ULL << EC11_GPIO_B);

    const gpio_config_t input_cfg = {
        .pin_bit_mask = pin_mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = true,
        .pull_down_en = false,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&input_cfg));

    const TickType_t now = xTaskGetTickCount();
    for (size_t i = 0; i < KEY_COUNT; ++i) {
        const bool pressed = is_pressed(scan_key_cfg(i));
        s_key_db[i].stable_level = pressed;
        s_key_db[i].last_raw = pressed;
        s_key_db[i].last_transition_tick = now;
        s_key_pressed[i] = pressed;
    }

    const int enc_level = gpio_get_level(EC11_GPIO_BUTTON);
    const bool enc_pressed = MACRO_ENCODER_BUTTON_ACTIVE_LOW ? (enc_level == 0) : (enc_level != 0);
    s_encoder_btn_db.stable_level = enc_pressed;
    s_encoder_btn_db.last_raw = enc_pressed;
    s_encoder_btn_db.last_transition_tick = now;

    return ESP_OK;
}

static esp_err_t init_encoder(void)
{
    const pcnt_unit_config_t unit_cfg = {
        .high_limit = 100,
        .low_limit = -100,
    };
    ESP_ERROR_CHECK(pcnt_new_unit(&unit_cfg, &s_pcnt_unit));

    const pcnt_glitch_filter_config_t filter_cfg = {
        .max_glitch_ns = 1000,
    };
    ESP_ERROR_CHECK(pcnt_unit_set_glitch_filter(s_pcnt_unit, &filter_cfg));

    const pcnt_chan_config_t chan_a_cfg = {
        .edge_gpio_num = EC11_GPIO_A,
        .level_gpio_num = EC11_GPIO_B,
    };
    const pcnt_chan_config_t chan_b_cfg = {
        .edge_gpio_num = EC11_GPIO_B,
        .level_gpio_num = EC11_GPIO_A,
    };

    pcnt_channel_handle_t chan_a = NULL;
    pcnt_channel_handle_t chan_b = NULL;
    ESP_ERROR_CHECK(pcnt_new_channel(s_pcnt_unit, &chan_a_cfg, &chan_a));
    ESP_ERROR_CHECK(pcnt_new_channel(s_pcnt_unit, &chan_b_cfg, &chan_b));

    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(chan_a,
                                                  PCNT_CHANNEL_EDGE_ACTION_DECREASE,
                                                  PCNT_CHANNEL_EDGE_ACTION_INCREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(chan_a,
                                                   PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                                   PCNT_CHANNEL_LEVEL_ACTION_INVERSE));

    ESP_ERROR_CHECK(pcnt_channel_set_edge_action(chan_b,
                                                  PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                                  PCNT_CHANNEL_EDGE_ACTION_DECREASE));
    ESP_ERROR_CHECK(pcnt_channel_set_level_action(chan_b,
                                                   PCNT_CHANNEL_LEVEL_ACTION_KEEP,
                                                   PCNT_CHANNEL_LEVEL_ACTION_INVERSE));

    ESP_ERROR_CHECK(pcnt_unit_enable(s_pcnt_unit));
    ESP_ERROR_CHECK(pcnt_unit_clear_count(s_pcnt_unit));
    ESP_ERROR_CHECK(pcnt_unit_start(s_pcnt_unit));

    return ESP_OK;
}

static esp_err_t init_led_strip(void)
{
    const led_strip_config_t strip_cfg = {
        .strip_gpio_num = LED_STRIP_GPIO,
        .max_leds = LED_STRIP_COUNT,
        .led_model = LED_MODEL_SK6812,
        .flags.invert_out = false,
    };
    const led_strip_rmt_config_t rmt_cfg = {
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_led_strip));

    const TickType_t now = xTaskGetTickCount();
    hid_transport_status_t hid_status = {0};
    (void)hid_transport_get_status(&hid_status);
    const bool mounted = hid_status.usb_mounted;
    const bool hid_ready = (hid_status.mode == HID_MODE_USB) ? hid_status.usb_hid_ready : hid_status.ble_connected;
    s_usb_mounted_db.stable_level = mounted;
    s_usb_mounted_db.last_raw = mounted;
    s_usb_mounted_db.last_transition_tick = now;
    s_usb_hid_ready_db.stable_level = hid_ready;
    s_usb_hid_ready_db.last_raw = hid_ready;
    s_usb_hid_ready_db.last_transition_tick = now;
    s_led_frame_valid = false;

    return update_key_leds();
}

static esp_err_t web_control_set_layer(uint8_t layer_index)
{
    if (layer_index >= MACRO_LAYER_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    mark_user_activity(xTaskGetTickCount());
    set_active_layer(layer_index);
    return ESP_OK;
}

static esp_err_t web_control_set_buzzer(bool enabled)
{
    mark_user_activity(xTaskGetTickCount());
    buzzer_set_enabled(enabled);
    return ESP_OK;
}

static esp_err_t web_control_send_consumer(uint16_t usage)
{
    send_consumer_report_with_activity(usage);
    return ESP_OK;
}

static esp_err_t web_control_set_keyboard_mode(hid_mode_t mode)
{
    mark_user_activity(xTaskGetTickCount());
    return hid_transport_request_mode_switch(mode);
}

static esp_err_t web_control_start_ble_pairing(uint32_t timeout_sec)
{
    const uint32_t timeout_ms = (timeout_sec > 0U)
                                    ? (timeout_sec * 1000U)
                                    : ((uint32_t)MACRO_BLUETOOTH_PAIRING_WINDOW_SEC * 1000U);
    mark_user_activity(xTaskGetTickCount());
    return hid_transport_start_pairing_window(timeout_ms);
}

static esp_err_t web_control_clear_ble_bond(void)
{
    mark_user_activity(xTaskGetTickCount());
    return hid_transport_clear_bond();
}

static void input_task(void *arg)
{
    (void)arg;

    const TickType_t debounce_ticks = pdMS_TO_TICKS(DEBOUNCE_MS);
    const TickType_t tap_window_ticks = pdMS_TO_TICKS(MACRO_ENCODER_TAP_WINDOW_MS);
    TickType_t last_heartbeat = xTaskGetTickCount();

    while (1) {
        const TickType_t now = xTaskGetTickCount();
        bool keyboard_state_changed = false;

        for (size_t i = 0; i < KEY_COUNT; ++i) {
            const macro_action_config_t *scan_cfg = scan_key_cfg(i);
            const macro_action_config_t *active_cfg = active_key_cfg(i);
            const bool raw_pressed = is_pressed(scan_cfg);
            if (debounce_update(&s_key_db[i], raw_pressed, now, debounce_ticks)) {
                s_key_pressed[i] = s_key_db[i].stable_level;
                if (s_key_pressed[i]) {
                    mark_user_activity(now);
                    buzzer_play_keypress();
                }

                APP_LOGI("L%u Key[%u:%s] %s (gpio=%d type=%d usage=0x%X)",
                         (unsigned)s_active_layer + 1,
                         (unsigned)i,
                         active_cfg->name,
                         s_key_pressed[i] ? "pressed" : "released",
                         scan_cfg->gpio,
                         (int)active_cfg->type,
                         active_cfg->usage);
                home_assistant_notify_key_event(s_active_layer,
                                                (uint8_t)i,
                                                s_key_pressed[i],
                                                active_cfg->usage,
                                                active_cfg->name);
                web_service_record_key_event((uint8_t)i,
                                             s_key_pressed[i],
                                             active_cfg->usage,
                                             active_cfg->name);

                if (active_cfg->type == MACRO_ACTION_KEYBOARD) {
                    keyboard_state_changed = true;
                } else if (active_cfg->type == MACRO_ACTION_CONSUMER && s_key_pressed[i]) {
                    send_consumer_report_with_activity(active_cfg->usage);
                }
            }
        }

        if (keyboard_state_changed) {
            hid_transport_send_keyboard_report(s_key_pressed, s_active_layer);
        }

        touch_slider_update(now,
                           s_active_layer,
                           send_consumer_report_with_activity,
                           notify_touch_swipe);

        const int enc_level = gpio_get_level(EC11_GPIO_BUTTON);
        const bool enc_btn_raw = MACRO_ENCODER_BUTTON_ACTIVE_LOW ? (enc_level == 0) : (enc_level != 0);
        if (debounce_update(&s_encoder_btn_db, enc_btn_raw, now, debounce_ticks) && s_encoder_btn_db.stable_level) {
            mark_user_activity(now);
            if (s_encoder_single_pending) {
                s_encoder_single_pending = false;
            }
            if (s_encoder_tap_count == 0 || (now - s_encoder_last_tap_tick) > tap_window_ticks) {
                s_encoder_tap_count = 1;
            } else {
                s_encoder_tap_count++;
            }
            s_encoder_last_tap_tick = now;
            APP_LOGI("Encoder tap count=%u", (unsigned)s_encoder_tap_count);
        }

        if (s_encoder_tap_count > 0 && (now - s_encoder_last_tap_tick) > tap_window_ticks) {
            const uint8_t taps = s_encoder_tap_count;
            s_encoder_tap_count = 0;

            if (ota_manager_handle_encoder_taps(taps)) {
                s_encoder_single_pending = false;
                mark_user_activity(now);
            } else if (taps == (uint8_t)MACRO_KEYBOARD_MODE_SWITCH_TAP_COUNT) {
                s_encoder_single_pending = false;
                const hid_mode_t current_mode = hid_transport_get_mode();
                const hid_mode_t target_mode = (current_mode == HID_MODE_USB) ? HID_MODE_BLE : HID_MODE_USB;
                const esp_err_t mode_err = hid_transport_request_mode_switch(target_mode);
                if (mode_err == ESP_OK) {
                    mark_user_activity(now);
                    buzzer_play_keypress();
                    APP_LOGI("Keyboard mode switch requested: %s -> %s",
                             current_mode == HID_MODE_USB ? "USB" : "BLE",
                             target_mode == HID_MODE_USB ? "USB" : "BLE");
                } else {
                    APP_LOGI("Keyboard mode switch request failed: %s", esp_err_to_name(mode_err));
                }
            } else if (taps == (uint8_t)BLE_PAIRING_TAP_COUNT) {
                s_encoder_single_pending = false;
                if (hid_transport_get_mode() != HID_MODE_BLE) {
                    APP_LOGI("BLE pairing tap ignored in USB mode");
                } else {
                    const esp_err_t pair_err =
                        hid_transport_start_pairing_window((uint32_t)MACRO_BLUETOOTH_PAIRING_WINDOW_SEC * 1000U);
                    if (pair_err == ESP_OK) {
                        mark_user_activity(now);
                        buzzer_play_keypress();
                        APP_LOGI("BLE pairing window started via encoder tap x%u", (unsigned)taps);
                    } else {
                        APP_LOGI("BLE pairing start failed: %s", esp_err_to_name(pair_err));
                    }
                }
            } else if (MACRO_HA_CONTROL_ENABLED &&
                taps == (uint8_t)MACRO_HA_CONTROL_TAP_COUNT) {
                s_encoder_single_pending = false;
                const esp_err_t ha_err = home_assistant_trigger_default_control();
                if (ha_err == ESP_OK) {
                    APP_LOGI("HA control queued (domain=%s service=%s entity=%s taps=%u)",
                             MACRO_HA_CONTROL_DOMAIN,
                             MACRO_HA_CONTROL_SERVICE,
                             MACRO_HA_CONTROL_ENTITY_ID,
                             (unsigned)taps);
                } else {
                    APP_LOGI("HA control skipped err=%s", esp_err_to_name(ha_err));
                }
            } else if (MACRO_BUZZER_ENCODER_TOGGLE_ENABLED &&
                taps == (uint8_t)MACRO_BUZZER_ENCODER_TOGGLE_TAP_COUNT) {
                s_encoder_single_pending = false;
                const bool now_enabled = buzzer_toggle_enabled();
                APP_LOGI("Buzzer %s via encoder taps=%u",
                         now_enabled ? "enabled" : "disabled",
                         (unsigned)taps);
            } else if (taps == 1) {
                s_encoder_single_pending = true;
                s_encoder_single_due_tick = now + pdMS_TO_TICKS(MACRO_ENCODER_SINGLE_TAP_DELAY_MS);
                APP_LOGI("Encoder single tap pending (%u ms)", (unsigned)MACRO_ENCODER_SINGLE_TAP_DELAY_MS);
            } else if (taps == 2) {
                s_encoder_single_pending = false;
                set_active_layer(0);
            } else if (taps == 3) {
                s_encoder_single_pending = false;
                if (wifi_portal_is_active()) {
                    const esp_err_t cancel_err = wifi_portal_cancel();
                    APP_LOGI("Wi-Fi portal cancel via tap x3: %s", esp_err_to_name(cancel_err));
                } else {
                    set_active_layer(1);
                }
            } else if (taps >= 4) {
                s_encoder_single_pending = false;
                set_active_layer(2);
            }
        }

        if (s_encoder_single_pending && now >= s_encoder_single_due_tick) {
            s_encoder_single_pending = false;
            const uint16_t usage = g_encoder_layer_config[s_active_layer].button_single_usage;
            APP_LOGI("Encoder single tap (L%u) -> usage=0x%X", (unsigned)s_active_layer + 1, usage);
            send_consumer_report_with_activity(usage);
        }

        int pulse_count = 0;
        ESP_ERROR_CHECK(pcnt_unit_get_count(s_pcnt_unit, &pulse_count));

        const int steps = pulse_count / ENCODER_DETENT_PULSES;
        if (steps != 0) {
            mark_user_activity(now);
            buzzer_play_encoder_step((steps > 0) ? 1 : -1);
            ESP_ERROR_CHECK(pcnt_unit_clear_count(s_pcnt_unit));

            const uint16_t usage = (steps > 0) ?
                g_encoder_layer_config[s_active_layer].cw_usage :
                g_encoder_layer_config[s_active_layer].ccw_usage;

            APP_LOGI("Encoder steps=%d (L%u) usage=0x%X", steps, (unsigned)s_active_layer + 1, usage);
            home_assistant_notify_encoder_step(s_active_layer, steps, usage);
            web_service_record_encoder_step(steps, usage);

            for (int i = 0; i < abs(steps); ++i) {
                send_consumer_report_with_activity(usage);
            }
        }

        esp_err_t led_err = update_key_leds();
        if (led_err != ESP_OK) {
            ESP_LOGE(TAG, "LED update failed: %s", esp_err_to_name(led_err));
        }

        buzzer_update(now);
        hid_transport_poll(now);
        ota_manager_poll(now);
        wifi_portal_poll();
        web_service_poll();

        if ((now - last_heartbeat) >= pdMS_TO_TICKS(2000)) {
            last_heartbeat = now;
            hid_transport_status_t hid_status = {0};
            (void)hid_transport_get_status(&hid_status);
            APP_LOGI("alive mode=%s mounted=%d link_ready=%d ble_init=%d ble_conn=%d ble_adv=%d ble_bond=%d ble_err=%s ble_step=%s k1=%d enc_btn=%d",
                     hid_status.mode == HID_MODE_USB ? "usb" : "ble",
                     hid_status.usb_mounted,
                     hid_transport_is_link_ready(),
                     hid_status.ble_initialized,
                     hid_status.ble_connected,
                     hid_status.ble_advertising,
                     hid_status.ble_bonded,
                     hid_status.ble_init_failed ? esp_err_to_name(hid_status.ble_init_error) : "OK",
                     hid_status.ble_init_step[0] ? hid_status.ble_init_step : "-",
                     gpio_get_level(scan_key_cfg(0)->gpio),
                     gpio_get_level(EC11_GPIO_BUTTON));
        }

        vTaskDelay(pdMS_TO_TICKS(SCAN_INTERVAL_MS));
    }
}

static void display_task(void *arg)
{
    (void)arg;

    const TickType_t dim_timeout_ticks = pdMS_TO_TICKS((uint32_t)MACRO_OLED_DIM_TIMEOUT_SEC * 1000U);
    const TickType_t off_timeout_ticks = pdMS_TO_TICKS((uint32_t)MACRO_OLED_OFF_TIMEOUT_SEC * 1000U);
    const uint8_t normal_brightness = MACRO_OLED_DEFAULT_BRIGHTNESS_PERCENT;
    const uint8_t dim_brightness = MACRO_OLED_DIM_BRIGHTNESS_PERCENT;
    const int8_t shift_range = (int8_t)MACRO_OLED_SHIFT_RANGE_PX;
    const int shift_interval_sec = (MACRO_OLED_SHIFT_INTERVAL_SEC > 0) ? MACRO_OLED_SHIFT_INTERVAL_SEC : 60;

    bool display_enabled = true;
    bool display_dimmed = false;
    bool display_inverted = false;
    int8_t shift_x = 0;
    int8_t shift_y = 0;
    int last_shift_bucket = -1;
    int last_invert_hour = -1;

    while (1) {
        const TickType_t tick_now = xTaskGetTickCount();
        const TickType_t idle_ticks = tick_now - s_last_user_activity_tick;
        const bool should_off = (off_timeout_ticks > 0) && (idle_ticks >= off_timeout_ticks);
        const bool should_dim = !should_off && (dim_timeout_ticks > 0) && (idle_ticks >= dim_timeout_ticks);

        if (should_off != !display_enabled) {
            if (oled_set_display_enabled(!should_off) == ESP_OK) {
                display_enabled = !should_off;
            }
        }

        if (!should_off) {
            const bool want_dim = should_dim;
            if (want_dim != display_dimmed) {
                const uint8_t target = want_dim ? dim_brightness : normal_brightness;
                if (oled_set_brightness_percent(target) == ESP_OK) {
                    display_dimmed = want_dim;
                }
            }
        }

        time_t now = 0;
        struct tm timeinfo = {0};

        time(&now);
        localtime_r(&now, &timeinfo);

        if (is_time_synchronized(&timeinfo)) {
            const int hour_key = (timeinfo.tm_yday * 24) + timeinfo.tm_hour;
            if (last_invert_hour < 0) {
                /* First synced sample: establish baseline without toggling inversion state. */
                last_invert_hour = hour_key;
            } else if (hour_key != last_invert_hour) {
                display_inverted = !display_inverted;
                esp_err_t inv_err = oled_set_inverted(display_inverted);
                if (inv_err != ESP_OK) {
                    ESP_LOGE(TAG, "OLED invert change failed: %s", esp_err_to_name(inv_err));
                }
                last_invert_hour = hour_key;
            }
        } else {
            /* Keep normal polarity before sync so first SNTP correction does not invert unexpectedly. */
            if (display_inverted) {
                if (oled_set_inverted(false) == ESP_OK) {
                    display_inverted = false;
                }
            }
            last_invert_hour = -1;
        }

        const int shift_bucket =
            ((timeinfo.tm_yday * 24 * 3600) + (timeinfo.tm_hour * 3600) + (timeinfo.tm_min * 60) + timeinfo.tm_sec) /
            shift_interval_sec;
        if (shift_bucket != last_shift_bucket) {
            shift_x = random_shift_px(shift_range);
            shift_y = random_shift_px(shift_range);
            last_shift_bucket = shift_bucket;
        }

        if (display_enabled) {
            char ota_l0[48] = {0};
            char ota_l1[48] = {0};
            char ota_l2[48] = {0};
            char ota_l3[48] = {0};
            char portal_l0[48] = {0};
            char portal_l1[48] = {0};
            char portal_l2[48] = {0};
            char portal_l3[48] = {0};
            char ble_l0[48] = {0};
            char ble_l1[48] = {0};
            char ble_l2[48] = {0};
            char ble_l3[48] = {0};
            char ha_line[96] = {0};
            uint32_t ha_age_ms = 0;
            const bool ota_overlay = ota_manager_get_oled_lines(ota_l0,
                                                                sizeof(ota_l0),
                                                                ota_l1,
                                                                sizeof(ota_l1),
                                                                ota_l2,
                                                                sizeof(ota_l2),
                                                                ota_l3,
                                                                sizeof(ota_l3));
            const bool portal_active = wifi_portal_get_oled_lines(portal_l0,
                                                                  sizeof(portal_l0),
                                                                  portal_l1,
                                                                  sizeof(portal_l1),
                                                                  portal_l2,
                                                                  sizeof(portal_l2),
                                                                  portal_l3,
                                                                  sizeof(portal_l3));
            const bool ble_overlay = hid_transport_get_oled_lines(ble_l0,
                                                                  sizeof(ble_l0),
                                                                  ble_l1,
                                                                  sizeof(ble_l1),
                                                                  ble_l2,
                                                                  sizeof(ble_l2),
                                                                  ble_l3,
                                                                  sizeof(ble_l3));
            const bool has_ha_text =
                home_assistant_get_display_text(ha_line, sizeof(ha_line), &ha_age_ms) &&
                (ha_age_ms <= HA_DISPLAY_STALE_MS);
            esp_err_t err = ESP_OK;
            if (ota_overlay) {
                err = oled_render_text_lines(ota_l0, ota_l1, ota_l2, ota_l3, shift_x, shift_y);
            } else if (portal_active) {
                err = oled_render_text_lines(portal_l0, portal_l1, portal_l2, portal_l3, shift_x, shift_y);
            } else if (ble_overlay) {
                err = oled_render_text_lines(ble_l0, ble_l1, ble_l2, ble_l3, shift_x, shift_y);
            } else {
                err = has_ha_text
                          ? oled_render_clock_with_status(&timeinfo, ha_line, shift_x, shift_y)
                          : oled_render_clock(&timeinfo, shift_x, shift_y);
            }
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "OLED render failed: %s", esp_err_to_name(err));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void app_main(void)
{
    setenv("TZ", CONFIG_MACROPAD_TZ, 1);
    tzset();
    s_log_gate_start_tick = xTaskGetTickCount();
    s_log_gate_armed = true;

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(log_store_init());

    s_last_user_activity_tick = xTaskGetTickCount();

    bool oled_ready = false;
    bool wifi_portal_ready = false;
    bool web_ready = false;

    // Bring up USB transport first so CDC is available even if later init fails.
    esp_err_t err = hid_transport_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hid_transport_init failed: %s", esp_err_to_name(err));
    }

    err = init_keys();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init_keys failed: %s", esp_err_to_name(err));
    }
    err = touch_slider_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "touch_slider_init failed: %s", esp_err_to_name(err));
    }
    err = init_encoder();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init_encoder failed: %s", esp_err_to_name(err));
    }
    err = init_led_strip();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init_led_strip failed: %s", esp_err_to_name(err));
    }
    err = buzzer_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "buzzer_init failed: %s", esp_err_to_name(err));
    }
    buzzer_play_startup();
    err = oled_init();
    oled_ready = (err == ESP_OK);
    if (!oled_ready) {
        ESP_LOGE(TAG, "oled_init failed: %s", esp_err_to_name(err));
    }
    err = ota_manager_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_manager_init failed: %s", esp_err_to_name(err));
    }
    if (oled_ready) {
        err = oled_set_brightness_percent(MACRO_OLED_DEFAULT_BRIGHTNESS_PERCENT);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "oled_set_brightness_percent failed: %s", esp_err_to_name(err));
        }
        play_boot_animation();
    }
    err = wifi_portal_init();
    wifi_portal_ready = (err == ESP_OK);
    if (!wifi_portal_ready) {
        ESP_LOGE(TAG, "wifi_portal_init failed: %s", esp_err_to_name(err));
    }
    err = web_service_init();
    web_ready = (err == ESP_OK);
    if (!web_ready) {
        ESP_LOGE(TAG, "web_service_init failed: %s", esp_err_to_name(err));
    }
    if (web_ready) {
        web_service_set_active_layer(s_active_layer);
        const web_service_control_if_t control_if = {
            .set_layer = web_control_set_layer,
            .set_buzzer = web_control_set_buzzer,
            .send_consumer = web_control_send_consumer,
            .set_keyboard_mode = web_control_set_keyboard_mode,
            .start_ble_pairing = web_control_start_ble_pairing,
            .clear_ble_bond = web_control_clear_ble_bond,
        };
        err = web_service_register_control(&control_if);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "web_service_register_control failed: %s", esp_err_to_name(err));
        }
    }
    err = register_sntp_handler();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register_sntp_handler failed: %s", esp_err_to_name(err));
    }
    if (wifi_portal_ready) {
        err = wifi_portal_start();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "wifi_portal_start failed: %s", esp_err_to_name(err));
        }
    }
    {
        const esp_err_t ha_err = home_assistant_init();
        if (ha_err != ESP_OK) {
            ESP_LOGE(TAG, "Home Assistant init failed: %s", esp_err_to_name(ha_err));
        }
    }

    xTaskCreate(display_task, "display_task", 4096, NULL, 4, NULL);
    xTaskCreate(input_task, "input_task", 4096, NULL, 5, NULL);

    APP_LOGI("Macro keyboard started");
    APP_LOGI("Edit mapping in config/keymap_config.yaml");
}
