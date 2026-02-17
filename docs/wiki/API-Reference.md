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

### `esp_err_t oled_render_animation_frame_centered(const oled_animation_t *anim, uint16_t frame_index, int8_t shift_x, int8_t shift_y);`
- Renders one animation frame centered on panel using packed bitmap assets.
- Validates frame index/pointers and returns error on invalid metadata.

### Scene helper

### `esp_err_t oled_render_clock(const struct tm *timeinfo, int8_t shift_x, int8_t shift_y);`
- Current default scene renderer used by `display_task` (`HH:MM:SS`).

### `esp_err_t oled_render_clock_with_status(const struct tm *timeinfo, const char *status_text, int8_t shift_x, int8_t shift_y);`
- Renders clock plus one compact status line (used for Home Assistant state display).

### `esp_err_t oled_render_text_lines(const char *line0, const char *line1, const char *line2, const char *line3, int8_t shift_x, int8_t shift_y);`
- Renders a generic 4-line tiny-font scene.
- Used by captive-portal provisioning status UI on OLED.

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

## 5) Home Assistant Module (`main/home_assistant.h`)

### `esp_err_t home_assistant_init(void);`
- Initializes Home Assistant integration worker/queue from generated config macros.
- Safe when disabled: returns `ESP_OK` and leaves module inactive.

### `bool home_assistant_is_enabled(void);`
- Returns runtime-enabled state of Home Assistant bridge.

### `bool home_assistant_get_display_text(char *out, size_t out_size, uint32_t *age_ms);`
- Returns cached Home Assistant display line from worker polling.
- `age_ms` is optional and reports freshness of cached state text.

### `esp_err_t home_assistant_trigger_default_control(void);`
- Queues one configured Home Assistant service call action.
- Intended for runtime shortcuts (for example encoder multi-tap).

### `void home_assistant_notify_layer_switch(uint8_t layer_index);`
- Queues a layer-switch event when enabled by config.

### `void home_assistant_notify_key_event(uint8_t layer_index, uint8_t key_index, bool pressed, uint16_t usage, const char *key_name);`
- Queues key press/release event metadata for asynchronous publish.

### `void home_assistant_notify_encoder_step(uint8_t layer_index, int32_t steps, uint16_t usage);`
- Queues encoder step event metadata.

### `void home_assistant_notify_touch_swipe(uint8_t layer_index, bool left_to_right, uint16_t usage);`
- Queues touch swipe event metadata.

### `esp_err_t home_assistant_queue_custom_event(const char *event_suffix, const char *json_payload);`
- Extension API for future features to publish custom JSON payloads to HA event bus.

## 6) Wi-Fi Portal Module (`main/wifi_portal.h`)

### `esp_err_t wifi_portal_init(void);`
- Initializes Wi-Fi stack integration for STA + optional captive portal flow.

### `esp_err_t wifi_portal_start(void);`
- Starts boot Wi-Fi connect attempt and triggers fallback captive portal when needed.

### `void wifi_portal_poll(void);`
- Periodic service call for timeout/stop/cancel state transitions.

### `bool wifi_portal_is_active(void);`
- Returns whether captive portal provisioning is currently active.

### `bool wifi_portal_is_connected(void);`
- Returns current STA connectivity state tracked by the module.

### `esp_err_t wifi_portal_cancel(void);`
- Requests immediate cancellation of active provisioning flow.

### `bool wifi_portal_get_oled_lines(...);`
- Exports short provisioning status lines for OLED rendering.
- Returns `false` when provisioning scene is not active.

## 7) Web Service Module (`main/web_service.h`)

### `esp_err_t web_service_init(void);`
- Initializes web-service module state/mutex.
- Does not start HTTP server immediately.

### `esp_err_t web_service_register_control(const web_service_control_if_t *iface);`
- Registers optional control callbacks used by write routes.
- Control routes stay blocked when `web_service.control_enabled=false`.

### `void web_service_poll(void);`
- Lifecycle service entry point.
- Starts server when STA is connected and captive portal is inactive.
- Stops server when captive portal is active or STA is disconnected.

### `bool web_service_is_running(void);`
- Returns current HTTP server running state.

### `void web_service_mark_user_activity(void);`
- Updates internal activity timestamp used by `/api/v1/state`.

### `void web_service_set_active_layer(uint8_t layer_index);`
- Updates exported active layer cache.

### `void web_service_record_key_event(...)`
### `void web_service_record_encoder_step(...)`
### `void web_service_record_touch_swipe(...)`
- Updates cached runtime telemetry consumed by REST state endpoint.

### REST routes (implemented)
- `GET /api/v1/health`
  - health + lifecycle status.
- `GET /api/v1/state`
  - active layer, buzzer state, idle age, latest key/encoder/swipe telemetry.
- `POST /api/v1/control/layer` with `{"layer":2}`
- `POST /api/v1/control/buzzer` with `{"enabled":true}`
- `POST /api/v1/control/consumer` with `{"usage":233}`
  - control routes require `web_service.control_enabled=true`.
