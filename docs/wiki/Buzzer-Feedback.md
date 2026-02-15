# Buzzer Feedback

## 1) Overview
- Module: `main/buzzer.c`
- API header: `main/buzzer.h`
- Hardware target: passive buzzer on `MACRO_BUZZER_GPIO` (default `GPIO_NUM_21`)
- Driver: LEDC PWM

The buzzer uses a non-blocking queue, so sound feedback does not stall input handling.

## 2) Runtime Integration
- `app_main()`:
  - `buzzer_init()`
  - `buzzer_play_startup()`
- `input_task`:
  - key press -> `buzzer_play_keypress()`
  - layer switch -> `buzzer_play_layer_switch()`
  - encoder step -> `buzzer_play_encoder_step()` (optional, config-gated)
  - periodic service -> `buzzer_update(now)`

## 3) Configuration (`main/keymap_config.h`)
- Core:
  - `MACRO_BUZZER_ENABLED`
  - `MACRO_BUZZER_GPIO`
  - `MACRO_BUZZER_DUTY_PERCENT`
  - `MACRO_BUZZER_QUEUE_SIZE`
- Startup:
  - `MACRO_BUZZER_STARTUP_ENABLED`
  - `MACRO_BUZZER_STARTUP_FREQ1_HZ`
  - `MACRO_BUZZER_STARTUP_FREQ2_HZ`
  - `MACRO_BUZZER_STARTUP_TONE_MS`
  - `MACRO_BUZZER_STARTUP_GAP_MS`
- Key press:
  - `MACRO_BUZZER_KEYPRESS_ENABLED`
  - `MACRO_BUZZER_KEYPRESS_FREQ_HZ`
  - `MACRO_BUZZER_KEYPRESS_MS`
- Layer switch:
  - `MACRO_BUZZER_LAYER_SWITCH_ENABLED`
  - `MACRO_BUZZER_LAYER_BASE_FREQ_HZ`
  - `MACRO_BUZZER_LAYER_STEP_HZ`
  - `MACRO_BUZZER_LAYER_MS`
- Encoder step:
  - `MACRO_BUZZER_ENCODER_STEP_ENABLED`
  - `MACRO_BUZZER_ENCODER_CW_FREQ_HZ`
  - `MACRO_BUZZER_ENCODER_CCW_FREQ_HZ`
  - `MACRO_BUZZER_ENCODER_MS`

## 4) API Surface
- `buzzer_init()`
- `buzzer_update(TickType_t now)`
- `buzzer_stop()`
- `buzzer_play_tone()`
- `buzzer_play_tone_ex()`
- event helpers:
  - `buzzer_play_startup()`
  - `buzzer_play_keypress()`
  - `buzzer_play_layer_switch()`
  - `buzzer_play_encoder_step()`

## 5) Tuning Guidance
- Too loud/harsh:
  - lower `MACRO_BUZZER_DUTY_PERCENT`
  - shorten event durations (`*_MS`)
- Sounds overlap under rapid input:
  - reduce enabled event types
  - increase `MACRO_BUZZER_QUEUE_SIZE` cautiously
- Want silent firmware:
  - set `MACRO_BUZZER_ENABLED` to `false`

## 6) Validation Checklist
1. Boot device and verify startup chirp (if enabled).
2. Press keys and verify click feedback.
3. Switch layers via encoder multi-tap and verify per-layer tone change.
4. Rotate encoder and verify optional step tone behavior.
5. Confirm no input lag while buzzer is active.
