#include "hid_usb_backend.h"

#include "macropad_hid.h"

esp_err_t hid_usb_backend_init(bool enable_hid_keyboard)
{
    return macropad_usb_init_mode(enable_hid_keyboard);
}

void hid_usb_backend_send_keyboard_report(const bool *key_pressed, uint8_t active_layer)
{
    macropad_send_keyboard_report(key_pressed, active_layer);
}

void hid_usb_backend_send_consumer_report(uint16_t usage)
{
    macropad_send_consumer_report(usage);
}

bool hid_usb_backend_mounted(void)
{
    return macropad_usb_mounted();
}

bool hid_usb_backend_hid_ready(void)
{
    return macropad_usb_hid_ready();
}

bool hid_usb_backend_cdc_connected(void)
{
    return macropad_usb_cdc_connected();
}
