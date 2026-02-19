# Troubleshooting

## 1) Build Issues
- Symptom: build fails due to missing IDF env
  - Action: run the init command from [Build and Flash](Build-and-Flash).

## 2) USB/HID Not Working
- Check logs for mount/ready state.
- Confirm USB cable/port quality.
- Confirm HID init path ran (`macropad_usb_init`).
- On some systems, bootloader COM port and app COM port differ (re-enumeration).
- Firmware briefly delays `MACROPAD` info logs while CDC enumerates, reducing lost early logs during COM switch.
- After a short timeout, `MACROPAD` logs resume even if CDC is still not connected.
- This log gating is non-blocking; if startup appears delayed, check boot animation/tone duration and not CDC readiness.

## 3) Touch Swipe Misses or False Triggers
- Enable debug logs (`MACRO_TOUCH_DEBUG_LOG_ENABLE`).
- Tune thresholds incrementally in `config/keymap_config.yaml` (`touch.*`).
- Verify left/right physical orientation against `MACRO_TOUCH_SWAP_SIDES`.

## 4) Encoder Issues
- Rotation too noisy:
  - verify PCNT glitch filter behavior and wiring.
- Tap actions wrong:
  - verify tap window and single-tap delay constants.

## 5) Clock Not Syncing
- Ensure SSID/password are configured.
- Confirm network reachability to NTP server.
- Validate timezone string in `MACROPAD_TZ`.
- If SSID is empty, firmware uses stored credentials or falls back to captive portal (if enabled).
- SNTP startup is deferred shortly after STA gets IP; check logs for `SNTP start scheduled` and then `Starting SNTP`.

## 6) Reboot Loop Around Wi-Fi Connect
- Check `Boot reset reason: <reason> (<id>)` near startup logs.
- If COM16 misses early boot logs, check `Boot reset reason (late): ...` in runtime logs after CDC is ready.
- If reset reason is `sw`, inspect runtime paths that can call `esp_restart` (mode switch, OTA, explicit reboot hooks).
- If reset reason is `panic`/`wdt`, enable higher log level and capture earliest crash output.
- If reset reason is `brownout`/`pwr_glitch`, check USB cable/port power stability.
- Check `task stack watermark input_task=...` in heartbeat logs; very low remaining stack indicates likely stack overflow risk.

## 7) Captive Portal Not Appearing
- Confirm `wifi_portal.enabled: true` in `config/keymap_config.yaml`.
- Confirm boot STA connect really failed or no credentials exist.
- If menuconfig SSID is set but wrong, firmware should now try stored credentials before opening portal.
- Check AP auth/password settings:
  - WPA2 requires password length >= 8.
  - If auth/password mismatch is configured, firmware may force OPEN mode.
- Check `wifi_portal.timeout_sec` is not too short.
- If phone/laptop does not pop captive UI automatically:
  - open browser manually at `http://192.168.4.1`.

## 8) OLED Protection Behavior Not as Expected
- Verify `oled.*` settings in `config/keymap_config.yaml`.
- If screen turns off too early/late:
  - tune `MACRO_OLED_DIM_TIMEOUT_SEC` and `MACRO_OLED_OFF_TIMEOUT_SEC`.
- If movement appears too large/small:
  - tune `MACRO_OLED_SHIFT_RANGE_PX`.
- Full OLED behavior reference:
  - [OLED Display](OLED-Display)

## 9) RGB LED Flicker
- Verify board power quality and ground integrity for the LED chain.
- Keep data line short and well-referenced to ground.
- Confirm firmware includes change-driven LED refresh and indicator debounce logic.
- If needed, reduce LED intensity via `MACRO_LED_INDICATOR_BRIGHTNESS` / `MACRO_LED_KEY_BRIGHTNESS`.
- Tune brightness groups in `config/keymap_config.yaml`:
  - `MACRO_LED_INDICATOR_BRIGHTNESS`
  - `MACRO_LED_KEY_BRIGHTNESS`
- If LEDs seem to "turn off unexpectedly" after idle:
  - check `led.off_timeout_sec` (`0` disables automatic RGB off)

## 10) Home Assistant Events Not Received
- Confirm `home_assistant.enabled: true`.
- Validate `CONFIG_MACROPAD_HA_BASE_URL` and `CONFIG_MACROPAD_HA_BEARER_TOKEN` in menuconfig.
- Ensure event family is enabled (for example `publish_layer_switch: true`).
- Check Home Assistant is reachable from device network/VLAN.
- If using HTTPS, verify server cert chain is trusted by ESP-IDF CRT bundle.

## 11) Home Assistant State Not Shown on OLED
- Confirm `home_assistant.display.enabled: true`.
- Verify `home_assistant.display.entity_id` exists in Home Assistant.
- Use Developer Tools -> States to confirm entity returns a valid `state`.
- If custom `label` is empty, OLED line will use Home Assistant `friendly_name`.
- Check network reachability and token permission (same as event bridge).

## 12) Home Assistant Control Shortcut Not Triggering
- Confirm `home_assistant.control.enabled: true`.
- Verify `home_assistant.control.tap_count` does not conflict with:
  - layer taps (`2/3/4+`)
  - buzzer toggle tap count (if enabled)
- Verify service endpoint inputs:
  - `home_assistant.control.service_domain`
  - `home_assistant.control.service_name`
  - `home_assistant.control.entity_id`
- Test equivalent call in Home Assistant Developer Tools -> Services.

## 13) BLE Keyboard Not Advertising
- Confirm `bluetooth.enabled: true` in `config/keymap_config.yaml`.
- Confirm current keyboard mode is `BLE`:
  - OLED BLE overlay
  - or `GET /api/v1/system/keyboard_mode`
- If mode is `USB`, switch with:
  - EC11 tap x`keyboard.mode.switch_tap_count`
  - or `POST /api/v1/system/keyboard_mode`.
- Verify build has BLE enabled in `sdkconfig.defaults` (`CONFIG_BT_ENABLED`, `CONFIG_BT_BLE_ENABLED`).
- Check heartbeat diagnostics:
  - `ble_err=<...>` and `ble_step=<...>` in `MACROPAD: alive ...`
  - `ble_step` shows the last BLE init phase (for example `set_device_name`, `config_adv_data`, `hidd_dev_init`).

## 14) BLE Pairing Fails
- Verify `MACROPAD_BLE_PASSKEY` in menuconfig matches host prompt.
- Open pairing window explicitly:
  - EC11 tap x7 in BLE mode
  - `POST /api/v1/system/ble/pair` with optional timeout.
- If pairing is stuck with stale host data:
  - clear bond via `POST /api/v1/system/ble/clear_bond`
  - remove device from host Bluetooth settings and pair again.
- Single-bond policy is active; pairing a new host replaces previous bond.

## 15) USB Keyboard Missing After Reboot
- Expected in BLE mode: USB HID is disabled intentionally (CDC remains available).
- Switch back to USB mode if host requires USB HID keyboard:
  - EC11 tap x`keyboard.mode.switch_tap_count`
  - or `POST /api/v1/system/keyboard_mode` with `{\"mode\":\"usb\"}`.
