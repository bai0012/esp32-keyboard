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

## 6) Extension Interface
`web_service.h` exposes a control callback interface:
- `set_layer`
- `set_buzzer`
- `send_consumer`

This keeps the web module decoupled from input/HID logic and supports future custom actions.

## 7) Security Notes
Current implementation is LAN-local foundation with optional write routes.

For production-grade control exposure in future revisions, add:
- authentication (token/session)
- route-level authorization
- rate limiting / brute-force protection
- optional HTTPS/reverse-proxy termination

## 8) Validation Checklist
1. Device connects to Wi-Fi STA.
2. Confirm `GET /api/v1/health` responds on configured port.
3. Confirm `GET /api/v1/state` updates after key/encoder/touch activity.
4. If `control_enabled=true`, verify:
   - layer switch via `POST /control/layer`
   - buzzer on/off via `POST /control/buzzer`
5. Enter captive portal mode and confirm web service stops automatically.
