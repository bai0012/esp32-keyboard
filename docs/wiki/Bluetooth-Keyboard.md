# Bluetooth Keyboard

## 1) Overview
- Firmware supports two keyboard transport modes:
  - `USB`: TinyUSB `CDC + HID keyboard/consumer`
  - `BLE`: TinyUSB `CDC only` + BLE HID keyboard/consumer
- Modes are mutually exclusive by design.
- BLE mode keeps CDC for logs/monitor but disables USB HID keyboard at descriptor level.

## 2) Runtime Mode Switching
- Encoder multi-tap:
  - `keyboard.mode.switch_tap_count` (default `5`) toggles `USB <-> BLE`.
- Web API:
  - `POST /api/v1/system/keyboard_mode` with `{"mode":"usb"}` or `{"mode":"ble"}`.
- Apply model:
  - target mode is persisted to NVS
  - firmware performs controlled reboot
  - new mode is applied after reboot

Precedence in tap dispatcher:
1. OTA confirm
2. keyboard mode switch
3. Home Assistant control
4. buzzer toggle
5. normal layer behavior

## 3) Pairing and Bonding
- Security model:
  - passkey pairing + bonding
  - static passkey is configured in `menuconfig`
- Pairing window:
  - starts automatically in BLE mode when no bond exists
  - can be started manually via API: `POST /api/v1/system/ble/pair`
  - optional body: `{"timeout_sec":120}`
- Bond policy:
  - single-bond host policy
  - when a new bond is accepted, old bond is removed
- Bond clear API:
  - `POST /api/v1/system/ble/clear_bond`

## 4) OLED Integration
When BLE mode is active, OLED transport overlay can show:
- `Keyboard: BLE`
- `Advertising` / `Connected` / `Idle`
- `Passkey XXXXXX`
- `Bonded <peer_addr>`
- `Switching to USB/BLE` (during mode apply pending reboot)

Overlay priority remains below OTA and Wi-Fi portal overlays.

## 5) Configuration

### YAML (`config/keymap_config.yaml`)
- `keyboard.mode.default`: `usb` or `ble`
- `keyboard.mode.switch_tap_count`
- `keyboard.mode.persist`
- `keyboard.mode.switch_reboot_delay_ms`
- `bluetooth.enabled`
- `bluetooth.pairing_window_sec`
- `bluetooth.disconnect_on_mode_exit`
- `bluetooth.clear_bond_on_new_pairing`

### menuconfig (`main/Kconfig.projbuild`)
- `MACROPAD_BLE_DEVICE_NAME`
- `MACROPAD_BLE_PASSKEY`
- `MACROPAD_BLE_TX_POWER` (reserved for future tuning)

## 6) REST State Fields
`GET /api/v1/state` includes BLE/mode fields:
- `keyboard_mode`
- `mode_switch_pending`
- `mode_switch_target`
- `usb_mounted`
- `usb_hid_ready`
- `ble_connected`
- `ble_bonded`
- `ble_pairing_active`
- `ble_pairing_remaining_ms`
- `ble_peer_addr`

`GET /api/v1/system/keyboard_mode` returns mode-centric status payload.

## 7) Validation Checklist
1. Boot in USB mode:
   - USB keyboard works
   - BLE does not advertise
2. Switch to BLE mode (tap x5 or API):
   - mode persists
   - reboot occurs
   - BLE keyboard advertises/connects
   - USB keyboard is disabled (CDC remains)
3. Pair with host using passkey; verify connected typing.
4. Re-pair from a second host; verify first bond is replaced.
5. Switch back to USB mode and verify BLE keyboard is no longer active.
