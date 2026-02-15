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

## 3) OLED Module (`main/oled.h`)

### Core control

### `esp_err_t oled_init(void);`
- Initializes I2C bus/device and OLED controller.

### `esp_err_t oled_set_brightness_percent(uint8_t percent);`
- Sets OLED contrast as a brightness percentage (`0..100`).

### `esp_err_t oled_set_display_enabled(bool enabled);`
- Powers OLED panel on/off (`on` = wake, `off` = screen off).

### `esp_err_t oled_set_inverted(bool inverted);`
- Toggles OLED display inversion mode (`normal`/`inverse`).

### Framebuffer primitives

### `void oled_clear_buffer(void);`
- Clears internal framebuffer.

### `void oled_set_pixel(int x, int y, bool on);`
- Sets a single pixel in framebuffer.

### `void oled_fill_rect(int x, int y, int w, int h, bool on);`
- Fills a rectangle in framebuffer.

### `void oled_draw_bitmap_mono(int x, int y, int w, int h, const uint8_t *bitmap, bool bit_packed);`
- Draws monochrome bitmap data into framebuffer.

### `esp_err_t oled_draw_text_utf8(int x, int y, const char *utf8, const oled_font_t *font);`
- Draws UTF-8 text using caller-supplied glyph callback (`oled_font_t`).
- Enables future multilingual rendering; Chinese/CJK support is provided by adding matching glyph tables/callbacks.

### `esp_err_t oled_present(void);`
- Flushes framebuffer to panel.

### Scene helper

### `esp_err_t oled_render_clock(const struct tm *timeinfo, int8_t shift_x, int8_t shift_y);`
- Current default scene renderer used by `display_task` (`HH:MM:SS`).

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

### `void buzzer_set_enabled(bool enabled);`
- Enables or disables buzzer feedback at runtime.

### `bool buzzer_is_enabled(void);`
- Returns current runtime buzzer-enable state.

### `bool buzzer_toggle_enabled(void);`
- Toggles runtime buzzer state.
- Uses configured on/off RTTTL feedback when available.

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
- Startup helper uses streaming playback so long startup RTTTL is not limited by queue depth.

Behavior/tuning reference:
- [Buzzer Feedback](Buzzer-Feedback)
