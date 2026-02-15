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

## 4) Buzzer Module (`main/buzzer.h`)

### `esp_err_t buzzer_init(void);`
- Initializes LEDC PWM for passive buzzer output.

### `void buzzer_update(TickType_t now);`
- Processes tone queue state machine (non-blocking).
- Call periodically from runtime loop.

### `void buzzer_stop(void);`
- Stops output and clears queued tones.

### `esp_err_t buzzer_play_tone(uint16_t frequency_hz, uint16_t duration_ms);`
- Queues a tone.

### `esp_err_t buzzer_play_tone_ex(uint16_t frequency_hz, uint16_t duration_ms, uint16_t silence_ms);`
- Queues a tone with a post-tone silence gap.

### `esp_err_t buzzer_play_rtttl(const char *rtttl);`
- Parses RTTTL and queues the resulting melody notes.
- Returns parse/queue errors for invalid strings or full queue.

### `void buzzer_play_startup(void);`
### `void buzzer_play_keypress(void);`
### `void buzzer_play_layer_switch(uint8_t layer_index);`
### `void buzzer_play_encoder_step(int8_t direction);`
- Convenience event helpers that map runtime events to configured tones.

Behavior/tuning reference:
- [Buzzer Feedback](Buzzer-Feedback)
