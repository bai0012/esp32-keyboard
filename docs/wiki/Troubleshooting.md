# Troubleshooting

## 1) Build Issues
- Symptom: build fails due to missing IDF env
  - Action: run the init command from [Build and Flash](Build-and-Flash.md).

## 2) USB/HID Not Working
- Check logs for mount/ready state.
- Confirm USB cable/port quality.
- Confirm HID init path ran (`macropad_usb_init`).

## 3) Touch Swipe Misses or False Triggers
- Enable debug logs (`MACRO_TOUCH_DEBUG_LOG_ENABLE`).
- Tune thresholds incrementally in `main/keymap_config.h`.
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
