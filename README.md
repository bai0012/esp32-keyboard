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
- Captive portal provisioning fallback (AP + web UI + credential persistence)
- Optional Home Assistant REST event bridge
- Local web service foundation (future web UI / local API integration)
- OTA update manager with rollback-safe post-update verification

Hardware reference is documented in `hardware_info.md`.

## Features
- 3 key layers defined in `config/keymap_config.yaml`
- Per-layer encoder mappings (single tap, CW, CCW)
- Per-layer touch-slide mappings
- Touch slide direction detection (`R->L` / `L->R`)
- Optional touch hold-repeat (used for volume on layer 2 by default)
- RGB layer/status feedback
  - software anti-flicker update path (change-driven LED refresh + USB status debounce)
  - inactivity auto-off timeout for all RGB LEDs
- Runtime `MACROPAD` logs are gated until TinyUSB CDC is connected (helps with COM re-enumeration after flashing)
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
- Home Assistant integration foundation:
  - queue-based non-blocking publish worker
  - REST event bus publishing (`/api/events/<event_type>`)
  - optional state polling (`/api/states/<entity_id>`) for OLED status line
  - optional direct service control (`/api/services/<domain>/<service>`)
  - configurable event families (layer/key/encoder/touch), timeout, and retry
- Local web service foundation:
  - versioned REST base (`/api/v1/*`)
  - runtime state endpoints for health and input/layer telemetry
  - optional control endpoints (layer/buzzer/consumer/system/ota) gated by config
  - auto lifecycle: starts only when STA is connected and captive portal is inactive
- OTA update workflow:
  - API-triggered firmware download (`/api/v1/system/ota`)
  - rollback-aware first boot (`PENDING_VERIFY`)
  - automatic post-update self-check
  - EC11 multi-tap confirmation (default: 3 taps) before finalizing update
  - configurable confirmation timeout with automatic rollback on timeout
- Wi-Fi provisioning fallback:
  - AP + captive portal web UI when credentials are missing or STA boot connect fails
  - persisted STA credentials (stored in Wi-Fi flash/NVS)
  - configurable AP/auth/timeout/retry/scan limits in YAML
  - provisioning status rendered on OLED
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
- `main/home_assistant.c`: Home Assistant event queue + REST publisher
- `main/wifi_portal.c`: Wi-Fi STA boot connect + captive portal provisioning fallback
- `main/web_service.c`: local REST web service module and control interface
- `main/ota_manager.c`: OTA download/verification state machine and rollback confirm flow
- `assets/animations/`: source images + manifest for OLED animations
- `config/keymap_config.yaml`: editable source-of-truth config (keys/encoder/touch/OLED/LED/buzzer/home_assistant/wifi_portal/web_service)
- `config/keymap_config.yaml`: editable source-of-truth config (keys/encoder/touch/OLED/LED/buzzer/home_assistant/wifi_portal/web_service/ota)
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

Note:
- On some hosts, bootloader and app enumerate on different COM ports.
- Firmware runtime `MACROPAD` logs are gated briefly while CDC comes up, then auto-fallback to normal logging.
- Startup is not blocked waiting for CDC; input/output init and startup tone begin immediately.

## Configuration
### 1) Key/encoder/touch behavior
Edit `config/keymap_config.yaml`:
- key maps (`keymap_layers`)
- encoder maps (`encoder.layers`)
- touch maps and tunables (`touch.*`)
- LED brightness + layer color scales (`led.*`)
  - includes `led.off_timeout_sec` to auto-off all RGB LEDs on inactivity
- buzzer behavior + RTTTL melodies (`buzzer.*`)
- OLED protection and I2C speed (`oled.*`)
- Home Assistant runtime publish behavior (`home_assistant.*`)
- Captive portal behavior (`wifi_portal.*`)
- Local web service behavior (`web_service.*`)
- OTA verification behavior (`ota.*`)

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
- `MACROPAD_HA_BASE_URL` (Home Assistant base URL)
- `MACROPAD_HA_BEARER_TOKEN` (Home Assistant token)
- `MACROPAD_WEB_API_KEY` (local web service API key)
- `MACROPAD_WEB_BASIC_AUTH_USER` (local web service basic-auth username)
- `MACROPAD_WEB_BASIC_AUTH_PASSWORD` (local web service basic-auth password)
- `MACROPAD_OTA_DEFAULT_URL` (optional default OTA URL used by API-triggered OTA when request URL is omitted)
- `MACROPAD_OTA_HTTP_TIMEOUT_MS` (OTA HTTP timeout)

Connection behavior:
- if menuconfig SSID/password are set, firmware tries STA at boot
- if that boot attempt fails, firmware automatically tries previously stored Wi-Fi credentials (if available)
- if menuconfig SSID is empty, firmware tries previously stored Wi-Fi credentials first
- if no credentials are available, or all boot attempts fail, captive portal provisioning starts (when `wifi_portal.enabled=true`)
- credentials submitted from captive portal are stored in Wi-Fi flash storage (NVS) and reused next boot

Security note:
- Keep Home Assistant URL/token in `menuconfig` (sdkconfig), not in `config/keymap_config.yaml`, to avoid leaking secrets to GitHub.
- Keep web-service API key / Basic-Auth credentials in `menuconfig` (sdkconfig), not in YAML.

## Runtime Controls
- Encoder taps:
  - 1 tap: layer-specific consumer action (delayed single-tap resolution)
  - 2 taps: switch to layer 1
  - 3 taps: switch to layer 2 (or cancel Wi-Fi provisioning when captive portal is active)
  - 4+ taps: switch to layer 3
  - OTA verify mode override:
    - while OTA is waiting confirmation, normal tap actions are suspended
    - press EC11 `ota.confirm_tap_count` times (default 3) to confirm and finalize OTA image
- Wi-Fi captive portal:
  - AP name/password/auth/timeout/scan size are configurable in `wifi_portal.*`
  - web UI provides AP scan + SSID/password connect form
  - optional DNS catch-all forces captive-portal behavior
  - OLED shows provisioning state (AP name, selected SSID, status, elapsed time)
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
- Home Assistant:
  - disabled by default and safe to leave unconfigured
  - publishes selected runtime events to Home Assistant event bus
  - can optionally poll one Home Assistant entity state and show it on OLED
  - can optionally trigger one configured Home Assistant service call via encoder multi-tap
  - current event families: `layer_switch`, `key_event`, `encoder_step`, `touch_swipe`
- Web service:
  - read-only runtime endpoints:
    - `GET /api/v1/health`
    - `GET /api/v1/state`
  - optional control endpoints (when `web_service.control_enabled=true`):
    - `POST /api/v1/control/layer` with `{"layer":2}` (1-based layer index)
    - `POST /api/v1/control/buzzer` with `{"enabled":true}`
    - `POST /api/v1/control/consumer` with `{"usage":233}`
    - `POST /api/v1/system/ota` with optional `{"url":"http://host/path/fw.bin"}` or `{"url":"https://host/path/fw.bin"}`
  - OTA status endpoint:
    - `GET /api/v1/system/ota`
    - `/api/v1/state` also includes nested `ota` status object
  - service starts after Wi-Fi STA is connected and stops while captive portal is active
  - optional authentication (configured in menuconfig):
    - API key via `X-API-Key` header (`MACROPAD_WEB_API_KEY`)
    - Basic Auth via `Authorization: Basic ...` (`MACROPAD_WEB_BASIC_AUTH_USER/PASSWORD`)
    - if API key is blank, API-key auth is disabled
    - if username or password is blank, Basic Auth is disabled
    - if both mechanisms are configured, either one is accepted
- OTA post-update verification:
  - new OTA image boots in pending-verify state (rollback enabled)
  - firmware runs self-check for `ota.self_check_duration_ms`
  - OLED shows confirmation prompt
  - EC11 multi-tap confirm finalizes firmware
  - if `ota.confirm_timeout_sec` expires (non-zero), firmware rolls back automatically
  - optional transport/security toggles:
    - `ota.allow_http`: allow plain HTTP OTA URL
    - `ota.skip_cert_verify`: skip HTTPS certificate verification
- OLED protection:
  - Pixel shift applies to all rendered content.
  - Any user input activity restores normal brightness and screen-on state.
- RGB idle-off protection:
  - all RGB LEDs turn off after `led.off_timeout_sec`
  - any user input activity restores RGB lighting

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
- Home Assistant integration: `docs/wiki/Home-Assistant-Integration.md`
- Wi-Fi provisioning/captive portal: `docs/wiki/Wi-Fi-Provisioning.md`
- Web service API foundation: `docs/wiki/Web-Service.md`
- OTA update and verification flow: `docs/wiki/OTA-Update.md`

## Documentation Policy (Required)
For every new feature or behavior change, update docs in the same change set:
- Update `README.md` for user-facing behavior/setup impact
- Update relevant pages in `docs/wiki/` for architecture/logic/config impact
- Update inline comments only where they clarify non-obvious logic

A feature is not complete until documentation is updated.
