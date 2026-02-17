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
  - Home Assistant module event hooks
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
  - Clock + compact status scene (`oled_render_clock_with_status`)
- `assets/animations/*` + `tools/generate_oled_animation_header.py`
  - Build-time conversion of image frames into packed monochrome bitmaps
  - Generated header: `main/oled_animation_assets.h`
- `main/buzzer.c`
  - Passive buzzer (LEDC PWM) initialization
  - Non-blocking tone queue
  - Event-tone helper APIs (startup/key/layer/encoder)
- `main/home_assistant.c`
  - Queue-based non-blocking worker
  - Home Assistant REST event bus transport (`/api/events/<event_type>`)
  - Home Assistant state polling (`/api/states/<entity_id>`) for OLED
  - Home Assistant service control (`/api/services/<domain>/<service>`)
  - Retry + timeout handling for unstable network/server conditions

OLED subsystem deep-dive:
- [OLED Display](OLED-Display)

## 3) Data/Control Flow
1. Input signals are sampled in `input_task`.
2. Key/encoder/touch events are mapped from `config/keymap_config.yaml` (compiled as `main/keymap_config.h`).
3. HID reports are sent by `macropad_hid` APIs.
4. LEDs/OLED are updated for runtime status feedback.
5. Optional Home Assistant events are queued and published asynchronously.
6. Optional Home Assistant state is polled and cached for display task rendering.
7. Optional Home Assistant service actions are queued from runtime shortcuts.

## 4) Build Composition
- `main/CMakeLists.txt` registers:
  - `main.c`
  - `buzzer.c`
  - `macropad_hid.c`
  - `touch_slider.c`
  - `oled.c`
  - `home_assistant.c`
