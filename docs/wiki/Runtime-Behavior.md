# Runtime Behavior

## 1) Task Model
- `input_task` (higher priority): scans keys/encoder/touch, sends HID reports, updates LEDs
- `display_task`: refreshes OLED clock every 200ms

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
  - 3 taps: layer 2
  - 4+ taps: layer 3

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

## 6) Clock and Sync Indicator
- OLED shows `HH:MM:SS`.
- Sync marker behavior:
  - synced: top-right indicator
  - unsynced: bottom bar indicator

## 7) Buzzer Feedback
- Buzzer playback is non-blocking and queue-driven.
- Default event hooks:
  - startup Mario intro notes (first phrase)
  - key-press click
  - layer-switch beeps N times for layer N
  - optional encoder-step tone
- Event and tone parameters are configured via `buzzer.*` in `config/keymap_config.yaml` (generated to `MACRO_BUZZER_*`).
- Event melodies are RTTTL strings; playback is parsed and queued at runtime.
- Encoder-step tones are rate-limited and coalesced to avoid post-spin tail playback.

## 8) OLED Screen Protection
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
