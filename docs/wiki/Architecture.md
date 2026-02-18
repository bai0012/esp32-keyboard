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
  - SNTP start hook (on IP-acquired event)
  - Home Assistant module event hooks
- `main/hid_transport.c`
  - Runtime HID mode selection (`USB` / `BLE`)
  - Mode-switch persistence + reboot apply
  - Unified keyboard/consumer send API used by app logic
  - BLE status export for OLED/web
- `main/hid_usb_backend.c`
  - USB backend adapter to TinyUSB HID (`main/macropad_hid.c`)
- `main/hid_ble_backend.c`
  - BLE HID backend (ESP HID over BLE)
  - Advertising/pairing window control
  - Passkey security + single-bond handling
- `main/keyboard_mode_store.c`
  - NVS read/write for persisted keyboard mode
- `main/macropad_hid.c`
  - TinyUSB descriptor and callback setup
  - HID enable/disable at descriptor level (`CDC+HID` vs `CDC-only`)
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
- `main/wifi_portal.c`
  - Wi-Fi STA init/connect bootstrap
  - Fallback captive portal provisioning (SoftAP + DNS catch-all + HTTP web UI)
  - Stored credential reuse and persistence via Wi-Fi flash storage
  - Provisioning state API for OLED scene integration
  - Runtime cancel/timeout control path
- `main/web_service.c`
  - Local REST API foundation (`/api/v1/*`)
  - Runtime state cache for layer/key/encoder/touch telemetry
  - Optional control interface callbacks (layer/buzzer/consumer)
  - Lifecycle manager: run only when STA is connected and captive portal is inactive
- `main/ota_manager.c`
  - OTA download trigger and worker (`esp_https_ota`)
  - Post-update pending-verify detection (`esp_ota_get_state_partition`)
  - Automated self-check stage + OLED status export
  - EC11 confirm-gate and timeout-driven rollback path

OLED subsystem deep-dive:
- [OLED Display](OLED-Display)

## 3) Data/Control Flow
1. Input signals are sampled in `input_task`.
2. Key/encoder/touch events are mapped from `config/keymap_config.yaml` (compiled as `main/keymap_config.h`).
3. HID reports are sent by `hid_transport` to selected backend (`USB` or `BLE`).
4. LEDs/OLED are updated for runtime status feedback.
5. Optional Home Assistant events are queued and published asynchronously.
6. Optional Home Assistant state is polled and cached for display task rendering.
7. Optional Home Assistant service actions are queued from runtime shortcuts.
8. Wi-Fi provisioning module manages STA boot connect and captive fallback as needed.
9. Web service module exposes read-only runtime state and optional control routes for future local integrations.
10. OTA manager handles update download and post-update verify/confirm/rollback lifecycle.

## 4) Build Composition
- `main/CMakeLists.txt` registers:
  - `main.c`
  - `buzzer.c`
  - `hid_transport.c`
  - `hid_usb_backend.c`
  - `hid_ble_backend.c`
  - `keyboard_mode_store.c`
  - `macropad_hid.c`
  - `touch_slider.c`
  - `oled.c`
  - `home_assistant.c`
  - `wifi_portal.c`
  - `web_service.c`
  - `ota_manager.c`
