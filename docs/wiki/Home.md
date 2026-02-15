# ESP32-S3 MacroPad Wiki

This wiki is organized as clear, task-oriented pages for development and maintenance.

## Project Summary
- Firmware target: `ESP32-S3`
- Input devices: 12 keys, EC11 encoder button/rotation, 2-channel touch slider
- Output devices: USB HID, 15x SK6812 LEDs, 128x64 OLED clock
- Output devices: USB HID, 15x SK6812 LEDs, 128x64 OLED clock, passive buzzer
- Optional network: Wi-Fi SNTP time sync
- OLED protection: pixel shift, inactivity dim/off, hourly inversion

## Wiki Pages
1. [Architecture](Architecture)
2. [Build and Flash](Build-and-Flash)
3. [Configuration](Configuration)
4. [Runtime Behavior](Runtime-Behavior)
5. [OLED Display](OLED-Display)
6. [Buzzer Feedback](Buzzer-Feedback)
7. [Touch Slider Algorithm](Touch-Slider-Algorithm)
8. [API Reference](API-Reference)
9. [Development Workflow](Development-Workflow)
10. [Troubleshooting](Troubleshooting)
11. [Documentation Policy](Documentation-Policy)

## Core Source Map
- `main/main.c`: startup, task orchestration, input loop, LEDs, Wi-Fi/SNTP
- `main/macropad_hid.c`: TinyUSB/HID descriptors and report sending
- `main/touch_slider.c`: touch gesture state machine + hold-repeat
- `main/oled.c`: OLED driver + framebuffer + text/bitmap primitives + clock scene renderer
- `main/buzzer.c`: non-blocking passive buzzer tone playback
- `config/keymap_config.yaml`: source-of-truth layers, mappings, and tuning constants
- `main/keymap_config.h`: generated config header used by firmware build
