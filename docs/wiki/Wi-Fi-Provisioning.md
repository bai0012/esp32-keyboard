# Wi-Fi Provisioning

## 1) Overview
This firmware supports an ESPHome-style fallback provisioning flow:
- try normal STA connect at boot
- if credentials are missing or boot connect fails, start captive portal
- user connects to AP, selects Wi-Fi in web UI, submits password
- credentials are persisted in Wi-Fi flash storage and reused on next boot

Primary source:
- `main/wifi_portal.c`
- `main/wifi_portal.h`

## 2) Boot Decision Flow
1. If `CONFIG_MACROPAD_WIFI_SSID` is set in `menuconfig`, use it first.
2. If that attempt fails, try stored credentials from Wi-Fi flash storage (if present and different).
3. If menuconfig SSID is empty, try stored credentials directly.
4. If no credentials are available, or boot STA connect attempts fail/times out, start captive portal when `wifi_portal.enabled=true`.

## 3) Captive Portal Runtime
- Starts SoftAP with configured SSID/password/auth mode.
- Starts DNS catch-all (optional) for captive behavior.
- Starts HTTP setup UI:
  - scan nearby SSIDs
  - submit SSID + password
  - trigger STA connect from selected network
- On `IP_EVENT_STA_GOT_IP`, portal auto-stops and firmware returns to STA mode.
- On `IP_EVENT_STA_GOT_IP` from a portal-submitted credential flow, firmware now stops portal services and continues in STA mode without forcing a reboot.
- Normal boot-time STA connect using stored/menuconfig credentials follows the same no-reboot behavior.
- Boot connect is non-blocking for app startup, so boot animation/input tasks are not held by connect timeout waits.

## 4) OLED Behavior During Provisioning
When portal is active, OLED switches from clock scene to status scene:
- line 1: setup title
- line 2: AP name
- line 3: selected SSID (or AP IP hint)
- line 4: state + elapsed seconds + cancel hint

Renderer used:
- `oled_render_text_lines(...)`

## 5) Input Shortcut
- EC11 triple-tap (`tap_count == 3`) cancels active provisioning immediately.
- Outside provisioning mode, triple-tap keeps its original layer-switch meaning.

## 6) Configuration (`config/keymap_config.yaml`)
`wifi_portal.*` options:
- `enabled`
- `ap_ssid`
- `ap_password`
- `ap_auth_mode`
- `ap_channel`
- `ap_max_connections`
- `timeout_sec`
- `sta_connect_timeout_ms`
- `sta_max_retry`
- `scan_max_results`
- `dns_captive_enabled`

Related `menuconfig` options:
- `CONFIG_MACROPAD_WIFI_SSID`
- `CONFIG_MACROPAD_WIFI_PASSWORD`
- `CONFIG_MACROPAD_NTP_SERVER`
- `CONFIG_MACROPAD_TZ`

## 7) Persistence Model
- Credentials submitted from portal are written via Wi-Fi config APIs with flash storage enabled.
- Stored credentials are reused automatically on next boot.
- Even when menuconfig SSID is set, firmware can fall back to stored credentials if the menuconfig network fails.

## 8) Security Notes
- For private provisioning AP, use WPA2 mode and `8+` character password.
- `WIFI_AUTH_OPEN` is supported for convenience/testing only.
- SoftAP does not support enterprise auth modes (for example `WIFI_AUTH_WPA3_ENTERPRISE`); if configured, firmware falls back to `WIFI_AUTH_WPA2_PSK` (or `WIFI_AUTH_OPEN` when password is too short).
- Home Assistant URL/token remain in `menuconfig`, not YAML, to reduce source leak risk.

## 9) Validation Checklist
1. Clear menuconfig SSID/password and erase stored STA creds.
2. Boot and confirm AP appears with configured SSID.
3. Open portal UI and connect to real Wi-Fi.
4. Confirm OLED provisioning status updates.
5. Confirm portal auto-stops after IP is acquired.
6. Reboot and confirm stored credentials connect without portal.
7. During active provisioning, triple-tap encoder and confirm immediate cancel.
