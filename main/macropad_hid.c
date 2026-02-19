#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"

#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_console.h"
#include "tinyusb_default_config.h"
#include "tusb.h"
#include "class/hid/hid.h"
#include "class/hid/hid_device.h"

#include "keymap_config.h"

#include "macropad_hid.h"

#define TAG "MACROPAD_USB"

#define KEY_COUNT MACRO_KEY_COUNT
#define HID_REPORT_RETRY_MS 50

enum {
    REPORT_ID_KEYBOARD = 1,
    REPORT_ID_CONSUMER = 2,
};

#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_OUT   0x02
#define EPNUM_CDC_IN    0x82
#define EPNUM_HID_IN    0x83
#define TUSB_DESC_TOTAL_LEN_CDC_HID (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_DESC_LEN)
#define TUSB_DESC_TOTAL_LEN_CDC_ONLY (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

static const uint8_t s_hid_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(REPORT_ID_KEYBOARD)),
    TUD_HID_REPORT_DESC_CONSUMER(HID_REPORT_ID(REPORT_ID_CONSUMER)),
};

static const uint8_t s_configuration_descriptor_cdc_hid[] = {
    TUD_CONFIG_DESCRIPTOR(1, 3, 0, TUSB_DESC_TOTAL_LEN_CDC_HID, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_CDC_DESCRIPTOR(0, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
    TUD_HID_DESCRIPTOR(2, 5, false, sizeof(s_hid_report_descriptor), EPNUM_HID_IN, 16, 5),
};

static const uint8_t s_configuration_descriptor_cdc_only[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, TUSB_DESC_TOTAL_LEN_CDC_ONLY, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_CDC_DESCRIPTOR(0, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
};

static const tusb_desc_device_t s_device_descriptor = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x303A,
    .idProduct = 0x4011,
    .bcdDevice = 0x0101,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

static const char *s_string_descriptor[] = {
    (char[]){0x09, 0x04},
    "Espressif",
    "ESP32 MacroPad",
    "123456",
    "MacroPad CDC",
    "MacroPad HID",
};

static bool s_hid_enabled = true;

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    (void)instance;
    return s_hid_report_descriptor;
}

uint16_t tud_hid_get_report_cb(uint8_t instance,
                               uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer,
                               uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance,
                           uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer,
                           uint16_t bufsize)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)bufsize;
}

static inline bool hid_enabled_and_ready(void)
{
    return s_hid_enabled && tud_mounted() && tud_hid_ready();
}

static bool hid_send_report_retry(uint8_t report_id,
                                  const void *report,
                                  uint16_t report_len,
                                  TickType_t timeout_ticks)
{
    const TickType_t start = xTaskGetTickCount();
    while ((xTaskGetTickCount() - start) < timeout_ticks) {
        if (hid_enabled_and_ready() && tud_hid_report(report_id, report, report_len)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return false;
}

static inline const macro_action_config_t *active_key_cfg(size_t idx, uint8_t active_layer)
{
    return &g_macro_keymap_layers[active_layer][idx];
}

esp_err_t macropad_usb_init_mode(bool enable_hid_keyboard)
{
    esp_err_t err = ESP_OK;
    tinyusb_config_t tusb_cfg = TINYUSB_DEFAULT_CONFIG();
    tusb_cfg.descriptor.device = &s_device_descriptor;
    tusb_cfg.descriptor.full_speed_config = enable_hid_keyboard
        ? s_configuration_descriptor_cdc_hid
        : s_configuration_descriptor_cdc_only;
    tusb_cfg.descriptor.string = s_string_descriptor;
    tusb_cfg.descriptor.string_count = sizeof(s_string_descriptor) / sizeof(s_string_descriptor[0]);
#if (TUD_OPT_HIGH_SPEED)
    tusb_cfg.descriptor.high_speed_config = enable_hid_keyboard
        ? s_configuration_descriptor_cdc_hid
        : s_configuration_descriptor_cdc_only;
#endif
    err = tinyusb_driver_install(&tusb_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "tinyusb_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }
    if (err == ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "TinyUSB driver already installed, continuing");
    }
    s_hid_enabled = enable_hid_keyboard;

#if CONFIG_TINYUSB_CDC_ENABLED
    tinyusb_config_cdcacm_t acm_cfg = {
        .cdc_port = TINYUSB_CDC_ACM_0,
    };
    err = tinyusb_cdcacm_init(&acm_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "tinyusb_cdcacm_init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = tinyusb_console_init(TINYUSB_CDC_ACM_0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "tinyusb_console_init failed: %s", esp_err_to_name(err));
        return err;
    }
#endif

    ESP_LOGI(TAG,
             "TinyUSB started (CDC%s), console redirected to CDC",
             s_hid_enabled ? " + HID" : " only");
    return ESP_OK;
}

esp_err_t macropad_usb_init(void)
{
    return macropad_usb_init_mode(true);
}

void macropad_send_consumer_report(uint16_t usage)
{
    if (usage == 0) {
        return;
    }
    if (!hid_enabled_and_ready()) {
        ESP_LOGW(TAG,
                 "Skip consumer report 0x%X, HID not ready/enabled (enabled=%d mounted=%d ready=%d)",
                 usage,
                 s_hid_enabled,
                 tud_mounted(),
                 tud_hid_ready());
        return;
    }

    const TickType_t timeout_ticks = pdMS_TO_TICKS(HID_REPORT_RETRY_MS);
    if (!hid_send_report_retry(REPORT_ID_CONSUMER, &usage, sizeof(usage), timeout_ticks)) {
        ESP_LOGW(TAG, "Consumer press report timeout usage=0x%X", usage);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(12));

    const uint16_t release = 0;
    if (!hid_send_report_retry(REPORT_ID_CONSUMER, &release, sizeof(release), timeout_ticks)) {
        ESP_LOGW(TAG, "Consumer release report timeout");
    }
}

void macropad_send_keyboard_report(const bool *key_pressed, uint8_t active_layer)
{
    if (key_pressed == NULL) {
        return;
    }

    if (!hid_enabled_and_ready()) {
        ESP_LOGW(TAG,
                 "Skip keyboard report, HID not ready/enabled (enabled=%d mounted=%d ready=%d)",
                 s_hid_enabled,
                 tud_mounted(),
                 tud_hid_ready());
        return;
    }

    uint8_t keycodes[6] = {0};
    size_t report_index = 0;

    for (size_t i = 0; i < KEY_COUNT && report_index < 6; ++i) {
        const macro_action_config_t *cfg = active_key_cfg(i, active_layer);
        if (key_pressed[i] && cfg->type == MACRO_ACTION_KEYBOARD) {
            keycodes[report_index++] = (uint8_t)cfg->usage;
        }
    }

    const TickType_t timeout_ticks = pdMS_TO_TICKS(HID_REPORT_RETRY_MS);
    const TickType_t start = xTaskGetTickCount();
    bool sent = false;
    while ((xTaskGetTickCount() - start) < timeout_ticks) {
        if (hid_enabled_and_ready() && tud_hid_keyboard_report(REPORT_ID_KEYBOARD, 0, keycodes)) {
            sent = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (!sent) {
        ESP_LOGW(TAG, "Keyboard report timeout");
    }
}

bool macropad_usb_hid_enabled(void)
{
    return s_hid_enabled;
}

bool macropad_usb_mounted(void)
{
    return tud_mounted();
}

bool macropad_usb_hid_ready(void)
{
    return s_hid_enabled ? tud_hid_ready() : false;
}

bool macropad_usb_cdc_connected(void)
{
    return tud_cdc_connected();
}
