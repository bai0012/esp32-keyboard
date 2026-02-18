# Web Service

## 1) Purpose
The web service module provides a local, versioned REST API foundation for future:
- dashboard/web UI integration
- local automation hooks
- remote control expansion (with optional auth in future revisions)

Current focus is stable scaffolding and extension points, not full feature UI.

## 2) Module Files
- `main/web_service.c`
- `main/web_service.h`

Integrated by:
- `main/main.c` (event feed + lifecycle polling + control callback registration)

## 3) Runtime Lifecycle
The service is started/stopped automatically by `web_service_poll()`:
- Start condition:
  - STA connected (`wifi_portal_is_connected()==true`)
  - captive portal not active (`wifi_portal_is_active()==false`)
- Stop condition:
  - STA disconnected, or
  - captive portal active

This avoids conflict with provisioning HTTP flow and keeps behavior deterministic.

## 4) API Routes (Current)
Base prefix: `/api/v1`

### Read-only routes
- `GET /api/v1/health`
  - Returns service health/lifecycle info.
- `GET /api/v1/state`
  - Returns cached runtime state:
    - active layer
    - buzzer enabled state
    - idle age
    - latest key/encoder/swipe telemetry
    - nested OTA state object
    - keyboard mode and BLE status:
      - `keyboard_mode`, `mode_switch_pending`, `mode_switch_target`
      - `usb_mounted`, `usb_hid_ready`
      - `ble_connected`, `ble_bonded`, `ble_pairing_active`, `ble_pairing_remaining_ms`, `ble_peer_addr`
- `GET /api/v1/system/keyboard_mode`
  - Returns focused keyboard-mode/BLE status payload.
- `GET /api/v1/system/logs`
  - Returns recent runtime logs collected in a RAM ring buffer.
  - Query string:
    - `limit` optional (`1..80`, default `40`)
  - Response fields:
    - `time_synced`: `false` before SNTP time is valid, `true` after sync
    - `entries[]`: `{id,line}` records in chronological order
  - Timestamp behavior in each line:
    - before sync: boot-relative (`+[ms]`)
    - after sync: real local time (`YYYY-MM-DD HH:MM:SS`)
- `GET /api/v1/system/ota`
  - Returns OTA manager state/status snapshot.

### Optional control routes
Control routes are available only when:
- `web_service.control_enabled: true`
- control callbacks are registered by main app

Routes:
- `POST /api/v1/control/layer`
  - body: `{"layer":2}` (1-based layer index)
- `POST /api/v1/control/buzzer`
  - body: `{"enabled":true}`
- `POST /api/v1/control/consumer`
  - body: `{"usage":233}`
- `POST /api/v1/system/ota`
  - body (optional): `{"url":"https://host/path/fw.bin"}` or `{"url":"http://host/path/fw.bin"}`
  - if URL is omitted, menuconfig default URL is used
  - OTA route also requires control enable
- `POST /api/v1/system/keyboard_mode`
  - body: `{"mode":"usb"}` or `{"mode":"ble"}`
  - persists target mode and schedules controlled reboot apply
- `POST /api/v1/system/ble/pair`
  - body (optional): `{"timeout_sec":120}`
  - opens BLE pairing window in BLE mode
- `POST /api/v1/system/ble/clear_bond`
  - clears existing BLE bond information

If control is disabled, routes return `403`.

## 5) Configuration (YAML)
Section: `web_service` in `config/keymap_config.yaml`

- `enabled`
- `port`
- `max_uri_handlers`
- `stack_size`
- `recv_timeout_sec`
- `send_timeout_sec`
- `cors_enabled`
- `control_enabled`

These values are generated into `main/keymap_config.h` as `MACRO_WEB_SERVICE_*`.
If `max_uri_handlers` is configured too low, runtime now auto-adjusts it to a safe minimum and logs a warning.

## 6) Extension Interface
`web_service.h` exposes a control callback interface:
- `set_layer`
- `set_buzzer`
- `send_consumer`
- `set_keyboard_mode`
- `start_ble_pairing`
- `clear_ble_bond`

This keeps the web module decoupled from input/HID logic and supports future custom actions.

## 7) OTA Integration
- Web service is the runtime trigger interface for OTA downloads.
- OTA status is surfaced in both:
  - `GET /api/v1/system/ota`
  - `GET /api/v1/state` (`ota` object)
- OTA status now includes transfer progress fields:
  - `download_total_bytes`
  - `download_read_bytes`
  - `download_elapsed_ms`
  - `download_percent`
- Post-update confirmation itself is intentionally local-only (EC11 multi-tap), not web-confirmed.

## 8) Security Notes
Current implementation is LAN-local foundation with optional write routes.

Auth mechanisms are configured in `menuconfig` (not YAML):
- `MACROPAD_WEB_API_KEY`
  - request header: `X-API-Key: <value>`
  - disabled when blank
- `MACROPAD_WEB_BASIC_AUTH_USER`
- `MACROPAD_WEB_BASIC_AUTH_PASSWORD`
  - request header: `Authorization: Basic <base64(user:pass)>`
  - Basic Auth is enabled only when both fields are non-empty

Auth policy:
- if both mechanisms are disabled, routes are open
- if one mechanism is enabled, that mechanism is required
- if both are enabled, either one is accepted

For production-grade control exposure in future revisions, add:
- authentication (token/session)
- route-level authorization
- rate limiting / brute-force protection
- optional HTTPS/reverse-proxy termination

## 9) Validation Checklist
1. Device connects to Wi-Fi STA.
2. Confirm `GET /api/v1/health` responds on configured port.
3. Confirm `GET /api/v1/state` updates after key/encoder/touch activity.
4. Confirm `GET /api/v1/system/logs?limit=20` returns recent logs with expected timestamp mode.
5. If `control_enabled=true`, verify:
   - layer switch via `POST /control/layer`
   - buzzer on/off via `POST /control/buzzer`
6. Enter captive portal mode and confirm web service stops automatically.
7. Trigger OTA with `POST /api/v1/system/ota` and verify state transitions in `/state`.
