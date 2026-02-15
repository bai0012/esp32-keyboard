# Configuration

## 1) Primary Behavior File
- `main/keymap_config.h`

This file controls:
- key mappings per layer
- encoder mappings per layer
- touch mappings per layer
- layer backlight colors
- touch algorithm tuning constants

## 2) Key Mappings
- Table: `g_macro_keymap_layers[MACRO_LAYER_COUNT][MACRO_KEY_COUNT]`
- Per key fields:
  - `gpio`
  - `active_low`
  - `led_index`
  - `type` (`MACRO_ACTION_KEYBOARD` or `MACRO_ACTION_CONSUMER`)
  - `usage`
  - `name`

## 3) Encoder Mappings
- Table: `g_encoder_layer_config`
- Per layer:
  - `button_single_usage`
  - `cw_usage`
  - `ccw_usage`

## 4) Touch Mappings
- Table: `g_touch_layer_config`
- Per layer:
  - `left_usage` (triggered by `R->L`)
  - `right_usage` (triggered by `L->R`)
  - `left_hold_repeat`, `right_hold_repeat`
  - `hold_start_ms`, `hold_repeat_ms`

## 5) Wi-Fi / NTP / Timezone
Use `idf.py menuconfig` -> `MacroPad Configuration`:
- `MACROPAD_WIFI_SSID`
- `MACROPAD_WIFI_PASSWORD`
- `MACROPAD_NTP_SERVER`
- `MACROPAD_TZ`

If SSID is empty, Wi-Fi and SNTP are disabled.
