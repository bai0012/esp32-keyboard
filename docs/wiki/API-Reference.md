# API Reference

## 1) HID Module (`main/macropad_hid.h`)

### `esp_err_t macropad_usb_init(void);`
- Initializes TinyUSB with custom keyboard + consumer descriptors.
- Also initializes CDC console when enabled by config.

### `void macropad_send_consumer_report(uint16_t usage);`
- Sends consumer press + release report sequence.
- No-op when usage is `0` or HID is not ready.

### `void macropad_send_keyboard_report(const bool *key_pressed, uint8_t active_layer);`
- Builds keyboard keycode array from current key state/layer.
- Sends one keyboard report.

## 2) Touch Module (`main/touch_slider.h`)

### `esp_err_t touch_slider_init(void);`
- Initializes touch peripheral and captures startup baselines.

### `void touch_slider_update(TickType_t now, uint8_t active_layer, touch_consumer_send_fn send_consumer);`
- Performs one touch processing iteration.
- Calls `send_consumer` when a gesture or hold-repeat action fires.

## 3) OLED Module (`main/oled_clock.h`)

### `esp_err_t oled_clock_init(void);`
- Initializes I2C bus/device and OLED controller.

### `esp_err_t oled_clock_render(const struct tm *timeinfo, int8_t shift_x, int8_t shift_y);`
- Renders the current time using caller-provided pixel-shift offsets.

### `esp_err_t oled_clock_set_brightness_percent(uint8_t percent);`
- Sets OLED contrast as a brightness percentage (`0..100`).

### `esp_err_t oled_clock_set_display_enabled(bool enabled);`
- Powers OLED panel on/off (`on` = wake, `off` = screen off).

### `esp_err_t oled_clock_set_inverted(bool inverted);`
- Toggles OLED display inversion mode (`normal`/`inverse`).

Behavior/tuning reference:
- [OLED Display](OLED-Display)
