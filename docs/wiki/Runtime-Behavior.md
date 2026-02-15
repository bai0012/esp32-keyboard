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

## 6) Clock and Sync Indicator
- OLED shows `HH:MM:SS`.
- Sync marker behavior:
  - synced: top-right indicator
  - unsynced: bottom bar indicator
