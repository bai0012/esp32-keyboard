# ESP32-S3 MacroPad

Firmware for a custom ESP32-S3 MacroPad with:
- 12 mechanical keys
- EC11 rotary encoder with push button
- 2-channel touch slider
- 15x SK6812 RGB LEDs
- 128x64 I2C OLED display
- Passive buzzer feedback (LEDC PWM)
- USB HID (keyboard + consumer control)
- Optional Wi-Fi SNTP time sync

Hardware reference is documented in `hardware_info.md`.

## Features
- 3 key layers defined in `config/keymap_config.yaml`
- Per-layer encoder mappings (single tap, CW, CCW)
- Per-layer touch-slide mappings
- Touch slide direction detection (`R->L` / `L->R`)
- Optional touch hold-repeat (used for volume on layer 2 by default)
- RGB layer/status feedback
  - software anti-flicker update path (change-driven LED refresh + USB status debounce)
- OLED subsystem with clock scene, framebuffer primitives, and UTF-8 text entry points
- Pluggable glyph-font interface for future multilingual rendering (including Chinese/CJK glyph packs)
- Build-time OLED animation asset pipeline for boot/menu scenes (`assets/animations`)
- Passive buzzer feedback:
  - startup melody via RTTTL
  - key-press click
  - layer-switch beeps N times for layer N
  - optional encoder-step tone
  - optional encoder multi-tap toggle for buzzer enable/disable, with configurable on/off tones
  - all event sounds configurable via RTTTL strings in `config/keymap_config.yaml`
- OLED burn-in protection:
  - random pixel shift (default every 60s, +/-2 px)
  - inactivity auto-dim and auto-off
  - hourly full-screen inversion toggle (starts after SNTP time is valid)
  - configurable default brightness (default 70%)
- System tuning profile:
  - 8MB flash target
  - dual OTA app slots
  - 1MB `cfgstore` data partition reserved for future configuration storage
  - 240MHz default CPU frequency
  - performance-oriented compiler optimization

## Repository Layout
- `main/main.c`: app orchestration, input scan loop, task startup
- `main/macropad_hid.c`: TinyUSB descriptors and HID report sending
- `main/touch_slider.c`: touch gesture state machine and hold-repeat
- `main/oled.c`: OLED core driver, framebuffer primitives, UTF-8 text path, and clock scene renderer
- `main/buzzer.c`: passive buzzer tone queue and event helpers
- `assets/animations/`: source images + manifest for OLED animations
- `config/keymap_config.yaml`: editable source-of-truth config (keys/encoder/touch/OLED/LED/buzzer)
- `tools/generate_keymap_header.py`: YAML -> `main/keymap_config.h` generator
- `tools/generate_oled_animation_header.py`: animation assets -> `main/oled_animation_assets.h` generator
- `main/keymap_config.h`: auto-generated C config header (do not edit manually)
- `main/Kconfig.projbuild`: Wi-Fi, NTP, timezone config entries
- `partitions_8mb_ota.csv`: custom 8MB partition layout (2 OTA + cfgstore)

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

Check firmware size:

```powershell
idf.py size
```

## Flash
```powershell
idf.py -p <PORT> flash monitor
```

## Configuration
### 1) Key/encoder/touch behavior
Edit `config/keymap_config.yaml`:
- key maps (`keymap_layers`)
- encoder maps (`encoder.layers`)
- touch maps and tunables (`touch.*`)
- LED brightness + layer color scales (`led.*`)
- buzzer behavior + RTTTL melodies (`buzzer.*`)
- OLED protection and I2C speed (`oled.*`)

Then rebuild. `main/keymap_config.h` is generated automatically from YAML.

Manual generation (optional):
```powershell
python tools/generate_keymap_header.py --in config/keymap_config.yaml --out main/keymap_config.h
```

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
- Buzzer:
  - startup/key/layer/encoder feedback behavior is driven by `buzzer.*` in YAML
  - event melodies use RTTTL strings (`name:d=,o=,b=:notes`)
  - tone playback is non-blocking and queued
  - startup RTTTL is streamed incrementally so long boot melodies are not truncated by queue depth
  - encoder-step beeps are throttled/coalesced to avoid long tail playback after very fast spins
  - optional encoder multi-tap can toggle buzzer state (`buzzer.encoder_toggle.*`)
- OLED protection:
  - Pixel shift applies to all rendered content.
  - Any user input activity restores normal brightness and screen-on state.

## OLED Display Details
### 1) Display content
- Clock format (current scene): `HH:MM:SS`
- Sync indicator:
  - synced: top-right marker
  - unsynced: bottom marker
- Rendering module is generalized for future text/bitmap/animation scenes (`main/oled.c`).
- Boot animation frames are loaded from generated assets (`main/oled_animation_assets.h`) at startup.

### 4) Animation assets
- Edit `assets/animations/manifest.yaml` to define animation frame order/timing.
- Put source frames in `assets/animations/<animation_name>/`.
- Supported source formats: `.pbm` (native, no extra deps), plus `.png`, `.bmp`, `.jpg`, `.jpeg` when Pillow is installed.
- Build will auto-generate `main/oled_animation_assets.h`.

### 2) Burn-in protection
- Universal pixel shift:
  - shifts all rendered content together
  - interval controlled by `MACRO_OLED_SHIFT_INTERVAL_SEC`
  - range controlled by `MACRO_OLED_SHIFT_RANGE_PX`
- Inactivity policy:
  - dim after `MACRO_OLED_DIM_TIMEOUT_SEC`
  - off after `MACRO_OLED_OFF_TIMEOUT_SEC`
  - wake immediately on any key/encoder/touch activity
- Hourly inversion:
  - inversion toggles hourly after time is valid/synced
  - avoids false inversion caused by pre-sync placeholder time

### 3) Brightness tuning
- Default brightness: `MACRO_OLED_DEFAULT_BRIGHTNESS_PERCENT` (default 70%)
- Dimmed brightness: `MACRO_OLED_DIM_BRIGHTNESS_PERCENT`

## Developer Docs
- Wiki home: `docs/wiki/Home.md`
- Wiki sidebar/navigation: `docs/wiki/_Sidebar.md`
- OLED deep-dive: `docs/wiki/OLED-Display.md`
- Buzzer deep-dive: `docs/wiki/Buzzer-Feedback.md`

## Documentation Policy (Required)
For every new feature or behavior change, update docs in the same change set:
- Update `README.md` for user-facing behavior/setup impact
- Update relevant pages in `docs/wiki/` for architecture/logic/config impact
- Update inline comments only where they clarify non-obvious logic

A feature is not complete until documentation is updated.
