# Buzzer Feedback

## 1) Overview
- Module: `main/buzzer.c`
- API header: `main/buzzer.h`
- Hardware target: passive buzzer on `MACRO_BUZZER_GPIO` (default `GPIO_NUM_21`)
- Driver: LEDC PWM

The buzzer uses a non-blocking queue, so sound feedback does not stall input handling.
Startup melody is streamed incrementally from RTTTL, so long boot songs are not truncated by queue depth.

## 2) Runtime Integration
- `app_main()`:
  - `buzzer_init()`
  - `buzzer_play_startup()`
- `input_task`:
  - key press -> `buzzer_play_keypress()`
  - layer switch -> `buzzer_play_layer_switch()` (beeps N times for layer N)
  - encoder step -> `buzzer_play_encoder_step()` (optional, config-gated)
  - periodic service -> `buzzer_update(now)`

## 3) Configuration (`config/keymap_config.yaml`)
- Core:
  - `MACRO_BUZZER_ENABLED`
  - `MACRO_BUZZER_GPIO`
  - `MACRO_BUZZER_DUTY_PERCENT`
  - `MACRO_BUZZER_QUEUE_SIZE`
  - `MACRO_BUZZER_RTTTL_NOTE_GAP_MS`
- Startup:
  - `MACRO_BUZZER_STARTUP_ENABLED`
  - `MACRO_BUZZER_RTTTL_STARTUP`
- Key press:
  - `MACRO_BUZZER_KEYPRESS_ENABLED`
  - `MACRO_BUZZER_RTTTL_KEYPRESS`
- Layer switch:
  - `MACRO_BUZZER_LAYER_SWITCH_ENABLED`
  - `MACRO_BUZZER_RTTTL_LAYER1`
  - `MACRO_BUZZER_RTTTL_LAYER2`
  - `MACRO_BUZZER_RTTTL_LAYER3`
- Encoder step:
  - `MACRO_BUZZER_ENCODER_STEP_ENABLED`
  - `MACRO_BUZZER_RTTTL_ENCODER_CW`
  - `MACRO_BUZZER_RTTTL_ENCODER_CCW`
  - `MACRO_BUZZER_ENCODER_MIN_INTERVAL_MS`
- Encoder toggle shortcut:
  - `MACRO_BUZZER_ENCODER_TOGGLE_ENABLED`
  - `MACRO_BUZZER_ENCODER_TOGGLE_TAP_COUNT`
  - `MACRO_BUZZER_RTTTL_TOGGLE_ON`
  - `MACRO_BUZZER_RTTTL_TOGGLE_OFF`

## 4) RTTTL Support
- Event sounds are defined as RTTTL strings in YAML config (`buzzer.*`).
- Supported syntax:
  - `name:d=<default_duration>,o=<default_octave>,b=<bpm>:<notes>`
  - notes: `c d e f g a b p`, optional `#`, optional dots `.`, optional octave digit
  - `p` means pause/rest
- Example:
  - `MACRO_BUZZER_RTTTL_LAYER2 "l2:d=16,o=6,b=180:g,g"`

## 5) API Surface
- `buzzer_init()`
- `buzzer_update(TickType_t now)`
- `buzzer_stop()`
- `buzzer_play_tone()`
- `buzzer_play_tone_ex()`
- `buzzer_play_rtttl()`
- event helpers:
  - `buzzer_play_startup()`
  - `buzzer_play_keypress()`
  - `buzzer_play_layer_switch()`
  - `buzzer_play_encoder_step()`

## 6) Tuning Guidance
- Too loud/harsh:
  - lower `MACRO_BUZZER_DUTY_PERCENT`
  - lower tempo (`b=`) or shorten notes in RTTTL strings
- Sounds overlap under rapid input:
  - reduce enabled event types
  - increase `MACRO_BUZZER_QUEUE_SIZE` cautiously
  - simplify RTTTL phrases
- Encoder rotated very fast but sound keeps trailing:
  - increase `MACRO_BUZZER_ENCODER_MIN_INTERVAL_MS`
  - shorten encoder RTTTL note duration
- Need a runtime mute/unmute shortcut:
  - enable encoder toggle in YAML (`buzzer.encoder_toggle.enabled`)
  - set its tap count to avoid conflicts with layer taps (`2/3/4` are already used)
- Startup melody stops too early:
  - startup playback now streams by design
  - if still truncated, validate RTTTL syntax (header + note tokens)
- Want silent firmware:
  - set `MACRO_BUZZER_ENABLED` to `false`

## 7) Validation Checklist
1. Boot device and verify startup RTTTL phrase.
2. Press keys and verify click feedback.
3. Switch layers via encoder multi-tap and verify layer N beeps exactly N times.
4. Rotate encoder and verify optional step tone behavior.
5. Confirm no input lag while buzzer is active.
