# Runtime Behavior

## 1) Task Model
- `input_task` (higher priority): scans keys/encoder/touch, sends HID reports, updates LEDs
- `display_task`: refreshes OLED clock every 200ms
- Runtime `MACROPAD` info logs are briefly gated during startup while TinyUSB CDC enumerates, then fallback to normal output.
- Startup flow is non-blocking: boot does not wait for CDC connection before initializing subsystems.
- HID transport is mode-based:
  - `USB` mode: TinyUSB `CDC + HID`
  - `BLE` mode: TinyUSB `CDC only` + BLE HID

## 2) Key Handling
- GPIO input is debounced (`DEBOUNCE_MS`).
- Keyboard actions update and send keyboard report state.
- Consumer actions send one-shot usage on key press.

## 3) Encoder Handling
- Rotation uses PCNT detent conversion (`ENCODER_DETENT_PULSES`).
- CW/CCW actions are layer-specific consumer usages.
- Button supports multi-tap layer control:
  - 1 tap: delayed single action
  - 2 taps: layer 1
  - 3 taps: layer 2 (or provisioning cancel when Wi-Fi captive portal is active)
  - 4+ taps: layer 3
- Keyboard mode switch:
  - `keyboard.mode.switch_tap_count` taps (default `5`) toggles `USB <-> BLE`
  - mode switch is persisted then applied by controlled reboot
- BLE pairing shortcut:
  - 7 taps starts BLE pairing window (BLE mode only)
- OTA verify override:
  - when OTA is awaiting confirmation, normal multi-tap actions are suspended
  - required tap count is `ota.confirm_tap_count` (default `3`)
  - successful confirm finalizes firmware (cancels rollback)
- Optional Home Assistant control shortcut:
  - configurable tap count (`home_assistant.control.tap_count`)
  - triggers one service call (`/api/services/<domain>/<service>`)

## 4) Touch Slider Handling
- Swipe direction:
  - `R->L` => `left_usage`
  - `L->R` => `right_usage`
- Uses strength, timing, side sequence, and filtered balance checks.
- Hold-repeat can be enabled per side per layer.

## 5) LED Feedback
- LED 0: USB mounted indicator
- LED 1: HID ready indicator
- LED 2: active layer color
- Key LEDs: per-layer dim/active scales
- Brightness groups:
  - indicator LEDs use `MACRO_LED_INDICATOR_BRIGHTNESS`
  - key LEDs use `MACRO_LED_KEY_BRIGHTNESS`
- LED anti-flicker behavior:
  - refresh is change-driven (no full strip refresh every scan loop)
  - USB/HID indicator states are debounced before being rendered
- LED inactivity off:
  - all RGB LEDs are forced off after `MACRO_LED_OFF_TIMEOUT_SEC` of no user input
  - any new key/encoder/touch activity restores normal RGB output

## 6) Clock and Sync Indicator
- OLED shows `HH:MM:SS`.
- Sync marker behavior:
  - synced: top-right indicator
  - unsynced: bottom bar indicator
- When `home_assistant.display.enabled=true` and state is available:
  - OLED shows clock with an additional status line (example: `HA: ON`)
  - state is polled from `/api/states/<entity_id>`

## 7) Wi-Fi Provisioning Fallback
- Boot STA connect strategy:
  - use menuconfig SSID/password when provided
  - otherwise use previously stored Wi-Fi credentials
  - if connect fails/times out, start captive portal provisioning (when enabled)
- Captive portal runtime:
  - starts SoftAP + DNS catch-all + HTTP setup UI
  - user selects SSID/password in web UI and submits connect request
  - credentials are stored in Wi-Fi flash/NVS and reused on future boots
  - OLED switches from clock scene to provisioning status scene
  - EC11 triple-tap cancels provisioning immediately

## 8) Buzzer Feedback
- Buzzer playback is non-blocking and queue-driven.
- Default event hooks:
  - startup Mario intro notes (first phrase)
  - key-press click
  - layer-switch beeps N times for layer N
  - optional encoder-step tone
- Event and tone parameters are configured via `buzzer.*` in `config/keymap_config.yaml` (generated to `MACRO_BUZZER_*`).
- Event melodies are RTTTL strings; playback is parsed and queued at runtime.
- Startup melody is streamed from RTTTL so long boot tunes are not capped by queue depth.
- Encoder-step tones are rate-limited and coalesced to avoid post-spin tail playback.
- Optional encoder multi-tap can toggle buzzer enable/disable with configurable on/off tones.

## 9) OLED Screen Protection
- Universal pixel shifting:
  - All rendered clock content (including static markers) is shifted together.
  - Shift is randomized in the configured range (default `+/-2 px`) every configured interval (default 60s).
- Inactivity handling:
  - Screen dims after `MACRO_OLED_DIM_TIMEOUT_SEC`.
  - Screen turns off after `MACRO_OLED_OFF_TIMEOUT_SEC`.
  - Any user input activity restores screen and normal brightness.
- Hourly inversion:
  - Display inversion state toggles each hour to distribute pixel wear.
  - Inversion timing starts only after SNTP/local time is considered valid, so pre-sync boot time does not trigger a false inversion.
- Brightness:
  - Default brightness is configurable (`MACRO_OLED_DEFAULT_BRIGHTNESS_PERCENT`, default 70%).

For complete OLED behavior details and tuning guidance:
- [OLED Display](OLED-Display)

For buzzer behavior details and tuning guidance:
- [Buzzer Feedback](Buzzer-Feedback)

## 10) Home Assistant Bridge (Optional)
- Runtime events can be published asynchronously to HA REST event bus.
- Publisher is queue-based and non-blocking for input/HID loops.
- Configured through `home_assistant.*` in `config/keymap_config.yaml`.
- Sensitive transport fields are set in menuconfig:
  - `CONFIG_MACROPAD_HA_BASE_URL`
  - `CONFIG_MACROPAD_HA_BEARER_TOKEN`
- Current event families:
  - `layer_switch`
  - `key_event`
  - `encoder_step`
  - `touch_swipe`
- Optional runtime additions:
  - one polled display entity for OLED status
  - one direct service-control action bound to encoder multi-tap

## 11) Local Web Service
- Lifecycle is automatic and non-blocking:
  - starts when STA is connected and captive portal is inactive
  - stops when captive portal is active or STA disconnects
- Read-only API exports runtime telemetry (`/api/v1/health`, `/api/v1/state`).
- Read-only API also exports buffered runtime logs (`/api/v1/system/logs?limit=N`).
  - Before SNTP sync: log line prefixes use boot-relative milliseconds.
  - After SNTP sync: log line prefixes use real local wall-clock time.
- Optional write routes are gated by config (`web_service.control_enabled`).
- Input loop continuously feeds layer/key/encoder/touch state into module cache.
- OTA control/state routes are exposed under `/api/v1/system/ota` and in `/api/v1/state`.
- Keyboard mode/BLE routes are exposed:
  - `/api/v1/system/keyboard_mode`
  - `/api/v1/system/ble/pair`
  - `/api/v1/system/ble/clear_bond`
- `/api/v1/state` includes mode/transport fields (`keyboard_mode`, BLE pairing/link status).

Details and route reference:
- [Web Service](Web-Service)

## 12) OTA Verification Flow
- OTA update is started by web API (`POST /api/v1/system/ota`).
- OTA download runs incrementally and reports progress to logs/OLED/REST.
- Download success reboots into new image.
- New image enters `PENDING_VERIFY` state (rollback enabled).
- Device runs auto self-check for `ota.self_check_duration_ms`.
- If self-check retries are exhausted, device stays in OTA confirm-wait with warning status.
- OLED displays confirmation prompt.
- If EC11 tap-count confirmation is received in time:
  - `esp_ota_mark_app_valid_cancel_rollback()` is called.
  - temporary "OTA confirmed" banner is shown, then OLED returns to normal scene automatically.
- If timeout expires (`ota.confirm_timeout_sec > 0`):
  - `esp_ota_mark_app_invalid_rollback_and_reboot()` is called.

## 13) BLE Pairing Runtime
- BLE mode pairing uses passkey security and bonding.
- If no bond exists in BLE mode, pairing window starts automatically.
- Manual pairing window can be opened via web API.
- Single-bond policy is enforced (new bond replaces old bond).
- Tap precedence order:
  - OTA confirm
  - keyboard mode switch
  - BLE pairing shortcut
  - Home Assistant control
  - buzzer toggle
  - normal layer/single-tap behavior
