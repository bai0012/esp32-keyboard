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
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "led_strip.h"
#include "tusb.h"

#include "keymap_config.h"
#include "sdkconfig.h"

#include "macropad_hid.h"
#include "oled_clock.h"
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
#define LED_BRIGHTNESS 24

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

static EventGroupHandle_t s_wifi_event_group;
static bool s_sntp_started;

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
    ESP_LOGI(TAG, "Switched to Layer %u", (unsigned)s_active_layer + 1);
    macropad_send_keyboard_report(s_key_pressed, s_active_layer);
}

static uint8_t dim(uint8_t value)
{
    return (uint8_t)((uint16_t)value * LED_BRIGHTNESS / 255U);
}

static esp_err_t update_key_leds(void)
{
    if (s_led_strip == NULL) {
        return ESP_OK;
    }

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

    ESP_RETURN_ON_ERROR(led_strip_clear(s_led_strip), TAG, "led clear failed");

    if (tud_mounted()) {
        ESP_RETURN_ON_ERROR(led_strip_set_pixel(s_led_strip, 0, dim(0), dim(40), dim(0)), TAG, "set led 0 failed");
    }
    if (tud_hid_ready()) {
        ESP_RETURN_ON_ERROR(led_strip_set_pixel(s_led_strip, 1, dim(0), dim(0), dim(40)), TAG, "set led 1 failed");
    }
    ESP_RETURN_ON_ERROR(led_strip_set_pixel(s_led_strip, 2, dim(layer_a_r), dim(layer_a_g), dim(layer_a_b)), TAG, "set led 2 failed");

    for (size_t i = 0; i < KEY_COUNT; ++i) {
        const macro_action_config_t *cfg = active_key_cfg(i);
        if (cfg->led_index >= LED_STRIP_COUNT) {
            continue;
        }

        ESP_RETURN_ON_ERROR(led_strip_set_pixel(s_led_strip,
                                                cfg->led_index,
                                                dim(key_dim_r), dim(key_dim_g), dim(key_dim_b)),
                            TAG, "set key led failed");

        if (s_key_pressed[i]) {
            ESP_RETURN_ON_ERROR(led_strip_set_pixel(s_led_strip,
                                                    cfg->led_index,
                                                    dim(key_active_r), dim(key_active_g), dim(key_active_b)),
                                TAG, "set key led active failed");
        }
    }

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
            ESP_LOGI(TAG, "Starting SNTP with server: %s", CONFIG_MACROPAD_NTP_SERVER);
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

    ESP_LOGI(TAG, "Wi-Fi started, waiting for IP");
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

                ESP_LOGI(TAG, "L%u Key[%u:%s] %s (gpio=%d type=%d usage=0x%X)",
                         (unsigned)s_active_layer + 1,
                         (unsigned)i,
                         active_cfg->name,
                         s_key_pressed[i] ? "pressed" : "released",
                         scan_cfg->gpio,
                         (int)active_cfg->type,
                         active_cfg->usage);

                if (active_cfg->type == MACRO_ACTION_KEYBOARD) {
                    keyboard_state_changed = true;
                } else if (active_cfg->type == MACRO_ACTION_CONSUMER && s_key_pressed[i]) {
                    macropad_send_consumer_report(active_cfg->usage);
                }
            }
        }

        if (keyboard_state_changed) {
            macropad_send_keyboard_report(s_key_pressed, s_active_layer);
        }

        touch_slider_update(now, s_active_layer, macropad_send_consumer_report);

        const int enc_level = gpio_get_level(EC11_GPIO_BUTTON);
        const bool enc_btn_raw = MACRO_ENCODER_BUTTON_ACTIVE_LOW ? (enc_level == 0) : (enc_level != 0);
        if (debounce_update(&s_encoder_btn_db, enc_btn_raw, now, debounce_ticks) && s_encoder_btn_db.stable_level) {
            if (s_encoder_single_pending) {
                s_encoder_single_pending = false;
            }
            if (s_encoder_tap_count == 0 || (now - s_encoder_last_tap_tick) > tap_window_ticks) {
                s_encoder_tap_count = 1;
            } else {
                s_encoder_tap_count++;
            }
            s_encoder_last_tap_tick = now;
            ESP_LOGI(TAG, "Encoder tap count=%u", (unsigned)s_encoder_tap_count);
        }

        if (s_encoder_tap_count > 0 && (now - s_encoder_last_tap_tick) > tap_window_ticks) {
            const uint8_t taps = s_encoder_tap_count;
            s_encoder_tap_count = 0;

            if (taps == 1) {
                s_encoder_single_pending = true;
                s_encoder_single_due_tick = now + pdMS_TO_TICKS(MACRO_ENCODER_SINGLE_TAP_DELAY_MS);
                ESP_LOGI(TAG, "Encoder single tap pending (%u ms)", (unsigned)MACRO_ENCODER_SINGLE_TAP_DELAY_MS);
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
            ESP_LOGI(TAG, "Encoder single tap (L%u) -> usage=0x%X", (unsigned)s_active_layer + 1, usage);
            macropad_send_consumer_report(usage);
        }

        int pulse_count = 0;
        ESP_ERROR_CHECK(pcnt_unit_get_count(s_pcnt_unit, &pulse_count));

        const int steps = pulse_count / ENCODER_DETENT_PULSES;
        if (steps != 0) {
            ESP_ERROR_CHECK(pcnt_unit_clear_count(s_pcnt_unit));

            const uint16_t usage = (steps > 0) ?
                g_encoder_layer_config[s_active_layer].cw_usage :
                g_encoder_layer_config[s_active_layer].ccw_usage;

            ESP_LOGI(TAG, "Encoder steps=%d (L%u) usage=0x%X", steps, (unsigned)s_active_layer + 1, usage);

            for (int i = 0; i < abs(steps); ++i) {
                macropad_send_consumer_report(usage);
            }
        }

        esp_err_t led_err = update_key_leds();
        if (led_err != ESP_OK) {
            ESP_LOGE(TAG, "LED update failed: %s", esp_err_to_name(led_err));
        }

        if ((now - last_heartbeat) >= pdMS_TO_TICKS(2000)) {
            last_heartbeat = now;
            ESP_LOGI(TAG, "alive mounted=%d hid_ready=%d k1=%d enc_btn=%d",
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

    while (1) {
        time_t now = 0;
        struct tm timeinfo = {0};

        time(&now);
        localtime_r(&now, &timeinfo);

        esp_err_t err = oled_clock_render(&timeinfo);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "OLED render failed: %s", esp_err_to_name(err));
        }

        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void app_main(void)
{
    setenv("TZ", CONFIG_MACROPAD_TZ, 1);
    tzset();

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_wifi_event_group = xEventGroupCreate();
    configASSERT(s_wifi_event_group != NULL);

    ESP_ERROR_CHECK(init_keys());
    ESP_ERROR_CHECK(touch_slider_init());
    ESP_ERROR_CHECK(init_encoder());
    ESP_ERROR_CHECK(init_led_strip());
    ESP_ERROR_CHECK(oled_clock_init());
    ESP_ERROR_CHECK(macropad_usb_init());
    ESP_ERROR_CHECK(init_wifi_and_sntp());

    xTaskCreate(display_task, "display_task", 4096, NULL, 4, NULL);
    xTaskCreate(input_task, "input_task", 4096, NULL, 5, NULL);

    ESP_LOGI(TAG, "Macro keyboard started");
    ESP_LOGI(TAG, "Edit mapping in main/keymap_config.h");
}
