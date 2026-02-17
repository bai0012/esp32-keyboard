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
- Remember: empty SSID intentionally disables SNTP.

## 6) OLED Protection Behavior Not as Expected
- Verify `oled.*` settings in `config/keymap_config.yaml`.
- If screen turns off too early/late:
  - tune `MACRO_OLED_DIM_TIMEOUT_SEC` and `MACRO_OLED_OFF_TIMEOUT_SEC`.
- If movement appears too large/small:
  - tune `MACRO_OLED_SHIFT_RANGE_PX`.
- Full OLED behavior reference:
  - [OLED Display](OLED-Display)

## 7) RGB LED Flicker
- Verify board power quality and ground integrity for the LED chain.
- Keep data line short and well-referenced to ground.
- Confirm firmware includes change-driven LED refresh and indicator debounce logic.
- If needed, reduce LED intensity via `MACRO_LED_INDICATOR_BRIGHTNESS` / `MACRO_LED_KEY_BRIGHTNESS`.
- Tune brightness groups in `config/keymap_config.yaml`:
  - `MACRO_LED_INDICATOR_BRIGHTNESS`
  - `MACRO_LED_KEY_BRIGHTNESS`

## 8) Home Assistant Events Not Received
- Confirm `home_assistant.enabled: true`.
- Validate `CONFIG_MACROPAD_HA_BASE_URL` and `CONFIG_MACROPAD_HA_BEARER_TOKEN` in menuconfig.
- Ensure event family is enabled (for example `publish_layer_switch: true`).
- Check Home Assistant is reachable from device network/VLAN.
- If using HTTPS, verify server cert chain is trusted by ESP-IDF CRT bundle.

## 9) Home Assistant State Not Shown on OLED
- Confirm `home_assistant.display.enabled: true`.
- Verify `home_assistant.display.entity_id` exists in Home Assistant.
- Use Developer Tools -> States to confirm entity returns a valid `state`.
- If custom `label` is empty, OLED line will use Home Assistant `friendly_name`.
- Check network reachability and token permission (same as event bridge).

## 10) Home Assistant Control Shortcut Not Triggering
- Confirm `home_assistant.control.enabled: true`.
- Verify `home_assistant.control.tap_count` does not conflict with:
  - layer taps (`2/3/4+`)
  - buzzer toggle tap count (if enabled)
- Verify service endpoint inputs:
  - `home_assistant.control.service_domain`
  - `home_assistant.control.service_name`
  - `home_assistant.control.entity_id`
- Test equivalent call in Home Assistant Developer Tools -> Services.
