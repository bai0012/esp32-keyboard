#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "driver/pulse_cnt.h"

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "led_strip.h"
#include "tusb.h"

#include "keymap_config.h"
#include "sdkconfig.h"

#include "buzzer.h"
#include "home_assistant.h"
#include "macropad_hid.h"
#include "oled.h"
#include "oled_animation_assets.h"
#include "touch_slider.h"

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

#define WIFI_CONNECTED_BIT BIT0

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

static EventGroupHandle_t s_wifi_event_group;
static bool s_sntp_started;
static volatile TickType_t s_last_user_activity_tick = 0;
static TickType_t s_log_gate_start_tick = 0;
static bool s_log_gate_armed = false;

static bool debounce_update(debounce_state_t *state,
                            bool raw_pressed,
                            TickType_t now,
                            TickType_t debounce_ticks);

static inline bool cdc_log_ready(void)
{
    if (tud_cdc_connected()) {
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
    macropad_send_consumer_report(usage);
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
    macropad_send_keyboard_report(s_key_pressed, s_active_layer);
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
    (void)debounce_update(&s_usb_mounted_db, tud_mounted(), now, status_debounce_ticks);
    (void)debounce_update(&s_usb_hid_ready_db, tud_hid_ready(), now, status_debounce_ticks);

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

static void wifi_event_handler(void *arg,
                               esp_event_base_t event_base,
                               int32_t event_id,
                               void *event_data)
{
    (void)arg;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        esp_wifi_connect();
        return;
    }

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);

        if (!s_sntp_started) {
            APP_LOGI("Starting SNTP with server: %s", CONFIG_MACROPAD_NTP_SERVER);
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, (char *)CONFIG_MACROPAD_NTP_SERVER);
            esp_sntp_init();
            s_sntp_started = true;
        }
    }
}

static esp_err_t init_wifi_and_sntp(void)
{
    if (strlen(CONFIG_MACROPAD_WIFI_SSID) == 0U) {
        ESP_LOGW(TAG, "Wi-Fi SSID empty, SNTP disabled");
        return ESP_OK;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    wifi_config_t wifi_cfg = {0};
    strlcpy((char *)wifi_cfg.sta.ssid, CONFIG_MACROPAD_WIFI_SSID, sizeof(wifi_cfg.sta.ssid));
    strlcpy((char *)wifi_cfg.sta.password, CONFIG_MACROPAD_WIFI_PASSWORD, sizeof(wifi_cfg.sta.password));
    wifi_cfg.sta.pmf_cfg.capable = true;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    APP_LOGI("Wi-Fi started, waiting for IP");
    return ESP_OK;
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
    const bool mounted = tud_mounted();
    const bool hid_ready = tud_hid_ready();
    s_usb_mounted_db.stable_level = mounted;
    s_usb_mounted_db.last_raw = mounted;
    s_usb_mounted_db.last_transition_tick = now;
    s_usb_hid_ready_db.stable_level = hid_ready;
    s_usb_hid_ready_db.last_raw = hid_ready;
    s_usb_hid_ready_db.last_transition_tick = now;
    s_led_frame_valid = false;

    return update_key_leds();
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

                if (active_cfg->type == MACRO_ACTION_KEYBOARD) {
                    keyboard_state_changed = true;
                } else if (active_cfg->type == MACRO_ACTION_CONSUMER && s_key_pressed[i]) {
                    send_consumer_report_with_activity(active_cfg->usage);
                }
            }
        }

        if (keyboard_state_changed) {
            macropad_send_keyboard_report(s_key_pressed, s_active_layer);
        }

        touch_slider_update(now,
                           s_active_layer,
                           send_consumer_report_with_activity,
                           home_assistant_notify_touch_swipe);

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

            if (MACRO_HA_CONTROL_ENABLED &&
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
                set_active_layer(1);
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

            for (int i = 0; i < abs(steps); ++i) {
                send_consumer_report_with_activity(usage);
            }
        }

        esp_err_t led_err = update_key_leds();
        if (led_err != ESP_OK) {
            ESP_LOGE(TAG, "LED update failed: %s", esp_err_to_name(led_err));
        }

        buzzer_update(now);

        if ((now - last_heartbeat) >= pdMS_TO_TICKS(2000)) {
            last_heartbeat = now;
            APP_LOGI("alive mounted=%d hid_ready=%d k1=%d enc_btn=%d",
                     tud_mounted(),
                     tud_hid_ready(),
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
            char ha_line[96] = {0};
            uint32_t ha_age_ms = 0;
            const bool has_ha_text =
                home_assistant_get_display_text(ha_line, sizeof(ha_line), &ha_age_ms) &&
                (ha_age_ms <= HA_DISPLAY_STALE_MS);
            esp_err_t err = has_ha_text
                                ? oled_render_clock_with_status(&timeinfo, ha_line, shift_x, shift_y)
                                : oled_render_clock(&timeinfo, shift_x, shift_y);
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

    s_wifi_event_group = xEventGroupCreate();
    configASSERT(s_wifi_event_group != NULL);
    s_last_user_activity_tick = xTaskGetTickCount();

    ESP_ERROR_CHECK(init_keys());
    ESP_ERROR_CHECK(macropad_usb_init());
    ESP_ERROR_CHECK(touch_slider_init());
    ESP_ERROR_CHECK(init_encoder());
    ESP_ERROR_CHECK(init_led_strip());
    ESP_ERROR_CHECK(buzzer_init());
    buzzer_play_startup();
    ESP_ERROR_CHECK(oled_init());
    ESP_ERROR_CHECK(oled_set_brightness_percent(MACRO_OLED_DEFAULT_BRIGHTNESS_PERCENT));
    play_boot_animation();
    ESP_ERROR_CHECK(init_wifi_and_sntp());
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
