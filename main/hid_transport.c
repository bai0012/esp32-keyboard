#include "hid_transport.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_system.h"

#include "hid_ble_backend.h"
#include "hid_usb_backend.h"
#include "keyboard_mode_store.h"
#include "keymap_config.h"
#include "sdkconfig.h"

#define TAG "HID_TRANSPORT"

typedef struct {
    bool initialized;
    hid_mode_t mode;
    bool mode_switch_pending;
    hid_mode_t mode_switch_target;
    TickType_t mode_switch_reboot_tick;
} hid_transport_ctx_t;

static hid_transport_ctx_t s_ctx = {0};

static hid_mode_t default_mode(void)
{
    return MACRO_KEYBOARD_DEFAULT_MODE_BLE ? HID_MODE_BLE : HID_MODE_USB;
}

static bool ble_feature_enabled(void)
{
    return MACRO_BLUETOOTH_ENABLED;
}

esp_err_t hid_transport_init(void)
{
    if (s_ctx.initialized) {
        return ESP_OK;
    }

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.mode = default_mode();
    ESP_LOGI(TAG, "keyboard mode default=%s", s_ctx.mode == HID_MODE_USB ? "usb" : "ble");

    if (MACRO_KEYBOARD_MODE_PERSIST) {
        keyboard_mode_t stored = KEYBOARD_MODE_USB;
        bool stored_valid = false;
        const esp_err_t load_err = keyboard_mode_store_load(&stored, &stored_valid);
        if (load_err == ESP_OK && stored_valid) {
            s_ctx.mode = (stored == KEYBOARD_MODE_BLE) ? HID_MODE_BLE : HID_MODE_USB;
            ESP_LOGI(TAG, "keyboard mode loaded from NVS=%s", s_ctx.mode == HID_MODE_USB ? "usb" : "ble");
        } else if (load_err != ESP_OK) {
            ESP_LOGW(TAG, "keyboard_mode_store_load failed: %s", esp_err_to_name(load_err));
        } else {
            ESP_LOGI(TAG, "keyboard mode NVS empty; using default");
        }
    }

    if (s_ctx.mode == HID_MODE_BLE && !ble_feature_enabled()) {
        ESP_LOGW(TAG, "BLE mode requested but bluetooth.enabled=false, fallback to USB");
        s_ctx.mode = HID_MODE_USB;
    }

    bool ble_ready = false;
    if (s_ctx.mode == HID_MODE_BLE && ble_feature_enabled()) {
        const esp_err_t ble_err =
            hid_ble_backend_init(CONFIG_MACROPAD_BLE_DEVICE_NAME, CONFIG_MACROPAD_BLE_PASSKEY);
        if (ble_err == ESP_OK) {
            ble_ready = true;
        } else {
            ESP_LOGE(TAG,
                     "BLE init failed in BLE mode: %s; falling back to USB mode",
                     esp_err_to_name(ble_err));
            s_ctx.mode = HID_MODE_USB;
            if (MACRO_KEYBOARD_MODE_PERSIST) {
                (void)keyboard_mode_store_save(KEYBOARD_MODE_USB);
            }
        }
    }

    ESP_RETURN_ON_ERROR(hid_usb_backend_init(s_ctx.mode == HID_MODE_USB), TAG, "usb backend init failed");

    if (ble_ready) {
        hid_ble_backend_status_t ble_status = {0};
        hid_ble_backend_get_status(&ble_status);
        if (!ble_status.bonded) {
            const uint32_t timeout_ms = (uint32_t)MACRO_BLUETOOTH_PAIRING_WINDOW_SEC * 1000U;
            (void)hid_ble_backend_start_pairing_window(timeout_ms);
        }
    }

    s_ctx.initialized = true;
    ESP_LOGI(TAG, "ready mode=%s", s_ctx.mode == HID_MODE_USB ? "usb" : "ble");
    return ESP_OK;
}

void hid_transport_poll(TickType_t now)
{
    if (!s_ctx.initialized) {
        return;
    }

    if (s_ctx.mode == HID_MODE_BLE && ble_feature_enabled()) {
        hid_ble_backend_poll(now);
    }

    if (s_ctx.mode_switch_pending && now >= s_ctx.mode_switch_reboot_tick) {
        ESP_LOGI(TAG,
                 "Applying keyboard mode switch: %s -> %s",
                 s_ctx.mode == HID_MODE_USB ? "usb" : "ble",
                 s_ctx.mode_switch_target == HID_MODE_USB ? "usb" : "ble");
        esp_restart();
    }
}

hid_mode_t hid_transport_get_mode(void)
{
    return s_ctx.mode;
}

bool hid_transport_is_link_ready(void)
{
    if (!s_ctx.initialized) {
        return false;
    }
    if (s_ctx.mode == HID_MODE_USB) {
        return hid_usb_backend_hid_ready();
    }
    if (ble_feature_enabled()) {
        return hid_ble_backend_is_ready();
    }
    return false;
}

bool hid_transport_cdc_connected(void)
{
    return hid_usb_backend_cdc_connected();
}

void hid_transport_send_keyboard_report(const bool *key_pressed, uint8_t active_layer)
{
    if (!s_ctx.initialized) {
        return;
    }

    if (s_ctx.mode == HID_MODE_USB) {
        hid_usb_backend_send_keyboard_report(key_pressed, active_layer);
        return;
    }
    if (ble_feature_enabled()) {
        (void)hid_ble_backend_send_keyboard_report(key_pressed, active_layer);
    }
}

void hid_transport_send_consumer_report(uint16_t usage)
{
    if (!s_ctx.initialized) {
        return;
    }

    if (s_ctx.mode == HID_MODE_USB) {
        hid_usb_backend_send_consumer_report(usage);
        return;
    }
    if (ble_feature_enabled()) {
        (void)hid_ble_backend_send_consumer_report(usage);
    }
}

esp_err_t hid_transport_request_mode_switch(hid_mode_t target)
{
    if (!s_ctx.initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (target != HID_MODE_USB && target != HID_MODE_BLE) {
        return ESP_ERR_INVALID_ARG;
    }
    if (target == HID_MODE_BLE && !ble_feature_enabled()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (target == s_ctx.mode) {
        return ESP_OK;
    }

    if (MACRO_KEYBOARD_MODE_PERSIST) {
        const keyboard_mode_t stored = (target == HID_MODE_BLE) ? KEYBOARD_MODE_BLE : KEYBOARD_MODE_USB;
        ESP_RETURN_ON_ERROR(keyboard_mode_store_save(stored), TAG, "save keyboard mode failed");
    }

    s_ctx.mode_switch_pending = true;
    s_ctx.mode_switch_target = target;
    s_ctx.mode_switch_reboot_tick = xTaskGetTickCount() + pdMS_TO_TICKS(MACRO_KEYBOARD_MODE_SWITCH_REBOOT_DELAY_MS);
    ESP_LOGI(TAG, "keyboard mode switch requested target=%s", target == HID_MODE_USB ? "usb" : "ble");
    return ESP_OK;
}

esp_err_t hid_transport_start_pairing_window(uint32_t timeout_ms)
{
    if (!s_ctx.initialized || s_ctx.mode != HID_MODE_BLE || !ble_feature_enabled()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (MACRO_BLUETOOTH_CLEAR_BOND_ON_NEW_PAIRING) {
        (void)hid_ble_backend_clear_bond();
    }
    return hid_ble_backend_start_pairing_window(timeout_ms);
}

esp_err_t hid_transport_clear_bond(void)
{
    if (!s_ctx.initialized || s_ctx.mode != HID_MODE_BLE || !ble_feature_enabled()) {
        return ESP_ERR_INVALID_STATE;
    }
    return hid_ble_backend_clear_bond();
}

bool hid_transport_get_status(hid_transport_status_t *out_status)
{
    if (out_status == NULL) {
        return false;
    }
    memset(out_status, 0, sizeof(*out_status));
    if (!s_ctx.initialized) {
        return false;
    }

    out_status->initialized = s_ctx.initialized;
    out_status->mode = s_ctx.mode;
    out_status->mode_switch_pending = s_ctx.mode_switch_pending;
    out_status->mode_switch_target = s_ctx.mode_switch_target;
    out_status->ble_enabled = ble_feature_enabled();

    out_status->usb_mounted = hid_usb_backend_mounted();
    out_status->usb_hid_ready = hid_usb_backend_hid_ready();
    out_status->cdc_connected = hid_usb_backend_cdc_connected();

    if (ble_feature_enabled()) {
        hid_ble_backend_status_t ble = {0};
        hid_ble_backend_get_status(&ble);
        out_status->ble_initialized = ble.initialized;
        out_status->ble_connected = ble.connected;
        out_status->ble_advertising = ble.advertising;
        out_status->ble_bonded = ble.bonded;
        out_status->ble_pairing_window_active = ble.pairing_window_active;
        out_status->ble_pairing_remaining_ms = ble.pairing_remaining_ms;
        out_status->ble_passkey = ble.passkey;
        strlcpy(out_status->ble_peer_addr, ble.peer_addr, sizeof(out_status->ble_peer_addr));
    }
    return true;
}

bool hid_transport_get_oled_lines(char *line0,
                                  size_t line0_size,
                                  char *line1,
                                  size_t line1_size,
                                  char *line2,
                                  size_t line2_size,
                                  char *line3,
                                  size_t line3_size)
{
    hid_transport_status_t st = {0};
    if (!hid_transport_get_status(&st)) {
        return false;
    }

    if (line0_size > 0) {
        line0[0] = '\0';
    }
    if (line1_size > 0) {
        line1[0] = '\0';
    }
    if (line2_size > 0) {
        line2[0] = '\0';
    }
    if (line3_size > 0) {
        line3[0] = '\0';
    }

    if (st.mode_switch_pending) {
        (void)snprintf(line0, line0_size, "Keyboard mode");
        (void)snprintf(line1,
                       line1_size,
                       "Switching to %s",
                       st.mode_switch_target == HID_MODE_BLE ? "BLE" : "USB");
        (void)snprintf(line2, line2_size, "Rebooting...");
        return true;
    }

    if (st.mode != HID_MODE_BLE) {
        return false;
    }

    (void)snprintf(line0, line0_size, "Keyboard: BLE");
    if (st.ble_connected) {
        (void)snprintf(line1, line1_size, "Connected");
    } else if (st.ble_advertising) {
        (void)snprintf(line1, line1_size, "Advertising");
    } else {
        (void)snprintf(line1, line1_size, "Idle");
    }

    if (st.ble_pairing_window_active) {
        (void)snprintf(line2, line2_size, "Passkey %06" PRIu32, st.ble_passkey);
        if (st.ble_pairing_remaining_ms > 0) {
            (void)snprintf(line3,
                           line3_size,
                           "Pair %lus",
                           (unsigned long)(st.ble_pairing_remaining_ms / 1000U));
        } else {
            (void)snprintf(line3, line3_size, "Pairing open");
        }
        return true;
    }

    if (st.ble_bonded && st.ble_peer_addr[0] != '\0') {
        (void)snprintf(line2, line2_size, "Bonded");
        (void)snprintf(line3, line3_size, "%s", st.ble_peer_addr);
        return true;
    }

    (void)snprintf(line2, line2_size, "No bond");
    return true;
}
