# ESP32-S3 MacroPad Wiki

This wiki is organized as clear, task-oriented pages for development and maintenance.

## Project Summary
- Firmware target: `ESP32-S3`
- Input devices: 12 keys, EC11 encoder button/rotation, 2-channel touch slider
- Output devices: USB HID, 15x SK6812 LEDs, 128x64 OLED display, passive buzzer
- Optional network: Wi-Fi SNTP time sync
- Wi-Fi fallback: captive portal provisioning (SoftAP + web UI + credential persistence)
- Optional integration bridge: Home Assistant REST event bus
- OLED protection: pixel shift, inactivity dim/off, hourly inversion
- OLED animation pipeline: build-time conversion from `assets/animations/*` to generated C assets

## Wiki Pages
1. [Architecture](Architecture)
2. [Build and Flash](Build-and-Flash)
3. [Configuration](Configuration)
4. [Runtime Behavior](Runtime-Behavior)
5. [OLED Display](OLED-Display)
6. [Wi-Fi Provisioning](Wi-Fi-Provisioning)
7. [Buzzer Feedback](Buzzer-Feedback)
8. [Touch Slider Algorithm](Touch-Slider-Algorithm)
9. [API Reference](API-Reference)
10. [Home Assistant Integration](Home-Assistant-Integration)
11. [Development Workflow](Development-Workflow)
12. [Troubleshooting](Troubleshooting)
13. [Documentation Policy](Documentation-Policy)

## Core Source Map
- `main/main.c`: startup, task orchestration, input loop, LEDs, Wi-Fi/SNTP
- `main/macropad_hid.c`: TinyUSB/HID descriptors and report sending
- `main/touch_slider.c`: touch gesture state machine + hold-repeat
- `main/oled.c`: OLED driver + framebuffer + text/bitmap primitives + clock scene renderer
- `main/buzzer.c`: non-blocking passive buzzer tone playback
- `main/home_assistant.c`: queue-based Home Assistant REST event publisher
- `main/wifi_portal.c`: STA boot connect + captive portal provisioning fallback
- `config/keymap_config.yaml`: source-of-truth layers, mappings, and tuning constants
- `main/keymap_config.h`: generated config header used by firmware build
