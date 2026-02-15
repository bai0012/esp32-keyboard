# ESP32-S3 MacroPad Wiki

This wiki is organized as clear, task-oriented pages for development and maintenance.

## Project Summary
- Firmware target: `ESP32-S3`
- Input devices: 12 keys, EC11 encoder button/rotation, 2-channel touch slider
- Output devices: USB HID, 15x SK6812 LEDs, 128x64 OLED clock
- Optional network: Wi-Fi SNTP time sync
- OLED protection: pixel shift, inactivity dim/off, hourly inversion

## Wiki Pages
1. [Architecture](Architecture.md)
2. [Build and Flash](Build-and-Flash.md)
3. [Configuration](Configuration.md)
4. [Runtime Behavior](Runtime-Behavior.md)
5. [Touch Slider Algorithm](Touch-Slider-Algorithm.md)
6. [API Reference](API-Reference.md)
7. [Development Workflow](Development-Workflow.md)
8. [Troubleshooting](Troubleshooting.md)
9. [Documentation Policy](Documentation-Policy.md)

## Core Source Map
- `main/main.c`: startup, task orchestration, input loop, LEDs, Wi-Fi/SNTP
- `main/macropad_hid.c`: TinyUSB/HID descriptors and report sending
- `main/touch_slider.c`: touch gesture state machine + hold-repeat
- `main/oled_clock.c`: OLED framebuffer + clock rendering
- `main/keymap_config.h`: layers, mappings, and all tuning constants
