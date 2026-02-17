# Configuration

## 1) Source of Truth
- Editable config: `config/keymap_config.yaml`
- Generated header: `main/keymap_config.h` (auto-generated, do not edit directly)
- Generator script: `tools/generate_keymap_header.py`

Build automatically regenerates the header from YAML.

Manual generation:
```powershell
python tools/generate_keymap_header.py --in config/keymap_config.yaml --out main/keymap_config.h
```

## 2) YAML Line-by-Line Reference (with examples)
Each row below is a concrete key path in `config/keymap_config.yaml`.

| Key Path | Example | Meaning |
|---|---|---|
| `schema_version` | `1` | Config schema version for future migration. |
| `counts.key` | `12` | Number of keys per layer. |
| `counts.layer` | `3` | Number of layers for key/encoder/touch maps. |
| `keymap_layers[].name` | `Layer 1 (default)` | Human-readable layer label used in generated comments. |
| `keymap_layers[].keys[].gpio` | `GPIO_NUM_7` | Input GPIO for one key. |
| `keymap_layers[].keys[].active_low` | `true` | `true` means pressed state is logic low. |
| `keymap_layers[].keys[].led_index` | `3` | SK6812 index tied to that key (`0xFF` = no LED). |
| `keymap_layers[].keys[].type` | `MACRO_ACTION_KEYBOARD` | Action type (`MACRO_ACTION_KEYBOARD` or `MACRO_ACTION_CONSUMER`). |
| `keymap_layers[].keys[].usage` | `HID_KEY_A` | HID usage symbol for key/consumer action. |
| `keymap_layers[].keys[].name` | `K1` | Debug/log label for the key. |
| `layer_backlight_colors[].r` | `90` | Layer base red channel (0..255). |
| `layer_backlight_colors[].g` | `90` | Layer base green channel (0..255). |
| `layer_backlight_colors[].b` | `0` | Layer base blue channel (0..255). |
| `led.indicator_brightness` | `16` | Brightness group for LEDs `0/1/2` (USB/HID/layer indicator). |
| `led.key_brightness` | `10` | Brightness group for key backlight LEDs. |
| `led.off_timeout_sec` | `180` | Inactivity timeout before all RGB LEDs are turned off (`0` disables LED auto-off). |
| `led.layer_key_dim_scale` | `45` | Idle scale applied to layer base color. |
| `led.layer_key_active_scale` | `140` | Pressed-key scale applied to layer base color. |
| `encoder.button_active_low` | `true` | Encoder button polarity. |
| `encoder.tap_window_ms` | `350` | Multi-tap grouping window. |
| `encoder.single_tap_delay_ms` | `120` | Delay before dispatching single-tap action. |
| `encoder.layers[].button_single_usage` | `HID_USAGE_CONSUMER_PLAY_PAUSE` | Layer-specific single-tap action. |
| `encoder.layers[].cw_usage` | `HID_USAGE_CONSUMER_VOLUME_INCREMENT` | Clockwise rotation action. |
| `encoder.layers[].ccw_usage` | `HID_USAGE_CONSUMER_VOLUME_DECREMENT` | Counter-clockwise rotation action. |
| `touch.layers[].left_usage` | `HID_USAGE_CONSUMER_SCAN_PREVIOUS_TRACK` | Action fired by `R->L` slide. |
| `touch.layers[].right_usage` | `HID_USAGE_CONSUMER_SCAN_NEXT_TRACK` | Action fired by `L->R` slide. |
| `touch.layers[].left_hold_repeat` | `false` | Enable hold-repeat for `R->L` direction. |
| `touch.layers[].right_hold_repeat` | `false` | Enable hold-repeat for `L->R` direction. |
| `touch.layers[].hold_start_ms` | `220` | Delay before first hold-repeat event. |
| `touch.layers[].hold_repeat_ms` | `110` | Repeat interval while holding edge. |
| `touch.trigger_percent` | `85` | Active threshold as `baseline * percent / 100`. |
| `touch.release_percent` | `92` | Release threshold as `baseline * percent / 100`. |
| `touch.trigger_min_delta` | `3500` | Minimum absolute delta needed to activate touch. |
| `touch.release_min_delta` | `1800` | Minimum absolute delta to remain active. |
| `touch.gesture_window_ms` | `650` | Max time between first and second side for swipe recognition. |
| `touch.min_interval_ms` | `280` | Debounce interval between emitted gestures. |
| `touch.baseline_freeze_total_delta` | `1200` | Freeze baseline when total delta exceeds this value. |
| `touch.baseline_freeze_side_delta` | `600` | Freeze baseline when max-side delta exceeds this value. |
| `touch.contact_min_total_delta` | `1500` | Minimum total delta for valid contact. |
| `touch.contact_min_side_delta` | `700` | Minimum single-side delta for valid contact. |
| `touch.start_side_delta` | `250` | Initial side dominance threshold (`|dR-dL|`). |
| `touch.gesture_travel_delta` | `450` | Minimum filtered travel before allowing swipe fire. |
| `touch.swipe_side_min_delta` | `1500` | Minimum compensated side delta for “side visited”. |
| `touch.swipe_side_relative_percent` | `20` | Crosstalk guard ratio for side-visited detection. |
| `touch.require_both_sides` | `true` | Require both sides visited before swipe emit. |
| `touch.both_sides_hold_ms` | `50` | Hold time after both sides are visited. |
| `touch.side_sequence_min_ms` | `20` | Minimum delay between first-side and second-side activation. |
| `touch.start_dominant_min_ms` | `30` | Start side must remain dominant for this long. |
| `touch.min_swipe_ms` | `100` | Minimum touch-session duration before swipe emit. |
| `touch.direction_dominance_delta` | `650` | Direction confirmation threshold (`|dR-dL|`). |
| `touch.swap_sides` | `false` | Set `true` only if physical slider orientation is mirrored. |
| `touch.debug_log_enable` | `false` | Enable periodic verbose touch debug logs. |
| `touch.debug_log_interval_ms` | `80` | Debug log interval when enabled. |
| `touch.idle_noise_margin` | `120` | Idle-noise compensation margin. |
| `touch.idle_noise_max_delta` | `2400` | Max delta considered as idle noise sampling window. |
| `oled.default_brightness_percent` | `70` | OLED normal brightness at runtime start/wake. |
| `oled.dim_brightness_percent` | `15` | OLED brightness in dim state. |
| `oled.dim_timeout_sec` | `45` | Idle timeout before dimming. |
| `oled.off_timeout_sec` | `180` | Idle timeout before full panel off. |
| `oled.shift_range_px` | `2` | Pixel-shift random radius (`+/-N`). |
| `oled.shift_interval_sec` | `60` | Pixel-shift interval. |
| `oled.i2c_scl_hz` | `800000` | OLED I2C clock speed in Hz. |
| `wifi_portal.enabled` | `true` | Enables captive portal fallback provisioning flow. |
| `wifi_portal.ap_ssid` | `'MacroPad-Setup'` | Provisioning hotspot SSID. |
| `wifi_portal.ap_password` | `'12345678'` | Provisioning hotspot password (`8+` chars for WPA2). |
| `wifi_portal.ap_auth_mode` | `WIFI_AUTH_WPA2_PSK` | AP encryption mode token (`WIFI_AUTH_OPEN`, `WIFI_AUTH_WPA2_PSK`, ...). |
| `wifi_portal.ap_channel` | `1` | Provisioning AP channel. |
| `wifi_portal.ap_max_connections` | `4` | Max stations allowed on provisioning AP. |
| `wifi_portal.timeout_sec` | `300` | Auto-cancel timeout for provisioning (`0` disables timeout). |
| `wifi_portal.sta_connect_timeout_ms` | `12000` | Boot STA connect timeout before fallback portal starts. |
| `wifi_portal.sta_max_retry` | `3` | Max STA retries before initial connect is considered failed. |
| `wifi_portal.scan_max_results` | `20` | Max AP scan entries rendered in provisioning web UI. |
| `wifi_portal.dns_captive_enabled` | `true` | Enables DNS catch-all behavior for captive portal UX. |
| `buzzer.enabled` | `true` | Master buzzer enable. |
| `buzzer.gpio` | `GPIO_NUM_21` | Buzzer output pin. |
| `buzzer.duty_percent` | `28` | PWM duty for passive buzzer loudness. |
| `buzzer.queue_size` | `16` | Non-blocking tone queue capacity. |
| `buzzer.rtttl_note_gap_ms` | `8` | Gap inserted between parsed RTTTL notes. |
| `buzzer.startup.enabled` | `true` | Startup melody enable. |
| `buzzer.startup.rtttl` | `'mario:d=8,o=6,b=100:e,e,p,e,p,c,e,p,g,p,g5'` | Startup RTTTL melody string. |
| `buzzer.keypress.enabled` | `true` | Key-press click enable. |
| `buzzer.keypress.rtttl` | `'key:d=32,o=6,b=180:c'` | Key-press RTTTL melody string. |
| `buzzer.layer_switch.enabled` | `true` | Layer-switch feedback enable. |
| `buzzer.layer_switch.layer1_rtttl` | `'l1:d=16,o=6,b=180:g'` | Layer-1 tone pattern. |
| `buzzer.layer_switch.layer2_rtttl` | `'l2:d=16,o=6,b=180:g,g'` | Layer-2 tone pattern. |
| `buzzer.layer_switch.layer3_rtttl` | `'l3:d=16,o=6,b=180:g,g,g'` | Layer-3 tone pattern. |
| `buzzer.encoder_step.enabled` | `true` | Encoder step tone enable. |
| `buzzer.encoder_step.cw_rtttl` | `'cw:d=32,o=6,b=220:e'` | Clockwise encoder step tone. |
| `buzzer.encoder_step.ccw_rtttl` | `'ccw:d=32,o=6,b=220:d'` | Counter-clockwise encoder step tone. |
| `buzzer.encoder_step.min_interval_ms` | `14` | Minimum interval between queued encoder tones (anti-tail). |
| `buzzer.encoder_toggle.enabled` | `false` | Enable encoder multi-tap buzzer toggle shortcut. |
| `buzzer.encoder_toggle.tap_count` | `5` | Tap count used to toggle buzzer (recommended `5` to avoid layer-tap conflicts). |
| `buzzer.encoder_toggle.on_rtttl` | `'bon:d=32,o=6,b=180:g'` | Sound played when buzzer is turned on. |
| `buzzer.encoder_toggle.off_rtttl` | `'boff:d=32,o=5,b=180:e'` | Sound played before buzzer turns off. |
| `home_assistant.enabled` | `false` | Master switch for Home Assistant event bridge. |
| `home_assistant.device_name` | `'esp32-macropad'` | Device identifier string included in event payloads. |
| `home_assistant.event_prefix` | `'macropad'` | Prefix for HA event names (e.g. `macropad_layer_switch`). |
| `home_assistant.request_timeout_ms` | `1800` | HTTP timeout per event publish. |
| `home_assistant.queue_size` | `24` | Queue depth for pending Home Assistant events. |
| `home_assistant.worker_interval_ms` | `30` | Worker polling interval when queue is idle. |
| `home_assistant.max_retry` | `1` | Retry count for failed publishes. |
| `home_assistant.publish_layer_switch` | `true` | Enable/disable layer-switch event publishing. |
| `home_assistant.publish_key_event` | `false` | Enable/disable key press/release event publishing. |
| `home_assistant.publish_encoder_step` | `false` | Enable/disable encoder step event publishing. |
| `home_assistant.publish_touch_swipe` | `false` | Enable/disable touch swipe event publishing. |
| `home_assistant.display.enabled` | `false` | Enable OLED status line from Home Assistant entity state polling. |
| `home_assistant.display.entity_id` | `'sensor.living_room_temperature'` | Home Assistant entity to poll via `/api/states/<entity_id>`. |
| `home_assistant.display.label` | `'HA'` | Prefix label shown on OLED (`''` uses Home Assistant `friendly_name`). |
| `home_assistant.display.poll_interval_ms` | `3000` | Poll period for state refresh. |
| `home_assistant.control.enabled` | `false` | Enable direct Home Assistant service control shortcut. |
| `home_assistant.control.tap_count` | `6` | Encoder multi-tap count that triggers the configured service call. |
| `home_assistant.control.service_domain` | `'light'` | Home Assistant service domain in `/api/services/<domain>/<service>`. |
| `home_assistant.control.service_name` | `'toggle'` | Home Assistant service name (`toggle` / `turn_on` / `turn_off`). |
| `home_assistant.control.entity_id` | `'light.desk_lamp'` | Target entity sent as `{\"entity_id\":\"...\"}` service payload. |

## 3) Validation Rules
- `counts.layer` must equal:
  - number of `keymap_layers`
  - number of `layer_backlight_colors`
  - number of `encoder.layers`
  - number of `touch.layers`
- `counts.key` must equal each `keymap_layers[].keys` length.
- `usage`, `gpio`, and `type` values must be valid C symbols used by ESP-IDF/TinyUSB headers.

## 4) Related Runtime Config (Menuconfig)
Use `idf.py menuconfig` -> `MacroPad Configuration`:
- `MACROPAD_WIFI_SSID`
- `MACROPAD_WIFI_PASSWORD`
- `MACROPAD_NTP_SERVER`
- `MACROPAD_TZ`
- `MACROPAD_HA_BASE_URL`
- `MACROPAD_HA_BEARER_TOKEN`

If SSID is empty:
- firmware first tries previously stored Wi-Fi credentials
- if none exist (or if boot connect fails), captive portal fallback starts when `wifi_portal.enabled=true`

Security-sensitive Home Assistant values are intentionally stored in `menuconfig`/`sdkconfig` and not in `config/keymap_config.yaml`.

## 5) Related Pages
- [OLED Display](OLED-Display)
- [Buzzer Feedback](Buzzer-Feedback)
- [Touch Slider Algorithm](Touch-Slider-Algorithm)
- [Home Assistant Integration](Home-Assistant-Integration)
- [Wi-Fi Provisioning](Wi-Fi-Provisioning)

## 6) OLED Animation Asset Config
These are file-based assets (not in `keymap_config.yaml`):
- `assets/animations/manifest.yaml`
- `assets/animations/<name>/frame_xxx.png`

`manifest.yaml` example:

```yaml
schema_version: 1
animations:
  boot:
    width: 128
    height: 64
    bit_packed: true
    invert: false
    frame_interval_ms: 110
    frames:
      - boot/frame_000.pbm
      - boot/frame_001.pbm
```

Build-generated output:
- `main/oled_animation_assets.h`
