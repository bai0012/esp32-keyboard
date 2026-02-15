# Architecture

## 1) High-Level Design
- `app_main()` initializes platform services and feature modules.
- Two FreeRTOS tasks run continuously:
  - `input_task`: input scan and action dispatch
  - `display_task`: OLED clock render

## 2) Module Boundaries
- `main/main.c`
  - Module orchestration
  - GPIO and PCNT setup
  - Key debounce and action routing
  - Layer switching and LED feedback
  - OLED protection policy control (shift/dim/off/invert timing)
  - Wi-Fi + SNTP init
- `main/macropad_hid.c`
  - TinyUSB descriptor and callback setup
  - Keyboard report send
  - Consumer report send
- `main/touch_slider.c`
  - Touch baseline and idle-noise compensation
  - Swipe direction detection
  - Hold-repeat trigger scheduler
- `main/oled.c`
  - I2C OLED init and command path
  - Framebuffer primitives
  - UTF-8 text draw path with pluggable font callback
  - Centered animation-frame render API
  - 7-segment style clock render scene (`oled_render_clock`)
- `assets/animations/*` + `tools/generate_oled_animation_header.py`
  - Build-time conversion of image frames into packed monochrome bitmaps
  - Generated header: `main/oled_animation_assets.h`
- `main/buzzer.c`
  - Passive buzzer (LEDC PWM) initialization
  - Non-blocking tone queue
  - Event-tone helper APIs (startup/key/layer/encoder)

OLED subsystem deep-dive:
- [OLED Display](OLED-Display)

## 3) Data/Control Flow
1. Input signals are sampled in `input_task`.
2. Key/encoder/touch events are mapped from `config/keymap_config.yaml` (compiled as `main/keymap_config.h`).
3. HID reports are sent by `macropad_hid` APIs.
4. LEDs and OLED are updated for runtime status feedback.

## 4) Build Composition
- `main/CMakeLists.txt` registers:
  - `main.c`
  - `buzzer.c`
  - `macropad_hid.c`
  - `touch_slider.c`
  - `oled.c`
