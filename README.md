# ESP32-S3 MacroPad

Firmware for a custom ESP32-S3 MacroPad with:
- 12 mechanical keys
- EC11 rotary encoder with push button
- 2-channel touch slider
- 15x SK6812 RGB LEDs
- 128x64 I2C OLED clock
- USB HID (keyboard + consumer control)
- Optional Wi-Fi SNTP time sync

Hardware reference is documented in `hardware_info.md`.

## Features
- 3 key layers defined in `main/keymap_config.h`
- Per-layer encoder mappings (single tap, CW, CCW)
- Per-layer touch-slide mappings
- Touch slide direction detection (`R->L` / `L->R`)
- Optional touch hold-repeat (used for volume on layer 2 by default)
- RGB layer/status feedback
- OLED digital clock with SNTP sync indicator
- OLED burn-in protection:
  - random pixel shift (default every 60s, +/-2 px)
  - inactivity auto-dim and auto-off
  - hourly full-screen inversion toggle (starts after SNTP time is valid)
  - configurable default brightness (default 70%)

## Repository Layout
- `main/main.c`: app orchestration, input scan loop, task startup
- `main/macropad_hid.c`: TinyUSB descriptors and HID report sending
- `main/touch_slider.c`: touch gesture state machine and hold-repeat
- `main/oled_clock.c`: OLED drawing and render pipeline
- `main/keymap_config.h`: key/encoder/touch mappings and tuning constants
- `main/Kconfig.projbuild`: Wi-Fi, NTP, timezone config entries

## Prerequisites
- ESP-IDF `v5.5.x`
- Windows PowerShell environment (current workflow)

Project-specific initialization command:

```powershell
. "C:\Espressif/Initialize-Idf.ps1" -IdfId esp-idf-b29c58f93b4ca0f49cdfc4c3ef43b562
```

## Build
```powershell
idf.py build
```

## Flash
```powershell
idf.py -p <PORT> flash monitor
```

## Configuration
### 1) Key/encoder/touch behavior
Edit `main/keymap_config.h`:
- `g_macro_keymap_layers`
- `g_encoder_layer_config`
- `g_touch_layer_config`
- Touch tuning constants (`MACRO_TOUCH_*`)
- OLED protection constants (`MACRO_OLED_*`)

### 2) Wi-Fi + SNTP + timezone
Set via `idf.py menuconfig` under `MacroPad Configuration`:
- `MACROPAD_WIFI_SSID`
- `MACROPAD_WIFI_PASSWORD`
- `MACROPAD_NTP_SERVER`
- `MACROPAD_TZ`

Leaving SSID empty disables Wi-Fi/SNTP.

## Runtime Controls
- Encoder taps:
  - 1 tap: layer-specific consumer action (delayed single-tap resolution)
  - 2 taps: switch to layer 1
  - 3 taps: switch to layer 2
  - 4+ taps: switch to layer 3
- Touch slider:
  - `R->L` triggers `left_usage`
  - `L->R` triggers `right_usage`
  - Hold-repeat only runs when enabled per-layer in `g_touch_layer_config`
- OLED protection:
  - Pixel shift applies to all rendered content.
  - Any user input activity restores normal brightness and screen-on state.

## Developer Docs
- Wiki home: `docs/wiki/Home.md`
- Wiki sidebar/navigation: `docs/wiki/_Sidebar.md`

## Documentation Policy (Required)
For every new feature or behavior change, update docs in the same change set:
- Update `README.md` for user-facing behavior/setup impact
- Update relevant pages in `docs/wiki/` for architecture/logic/config impact
- Update inline comments only where they clarify non-obvious logic

A feature is not complete until documentation is updated.
