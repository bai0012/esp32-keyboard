# Configuration

## 1) Primary Behavior File
- `main/keymap_config.h`

This file controls:
- key mappings per layer
- encoder mappings per layer
- touch mappings per layer
- layer backlight colors
- LED brightness groups (indicator vs key LEDs)
- buzzer behavior and tone settings
- touch algorithm tuning constants
- OLED protection and brightness constants

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

## 6) OLED Protection Settings
Defined in `main/keymap_config.h`:
- `MACRO_OLED_DEFAULT_BRIGHTNESS_PERCENT`
- `MACRO_OLED_DIM_BRIGHTNESS_PERCENT`
- `MACRO_OLED_DIM_TIMEOUT_SEC`
- `MACRO_OLED_OFF_TIMEOUT_SEC`
- `MACRO_OLED_SHIFT_RANGE_PX`
- `MACRO_OLED_SHIFT_INTERVAL_SEC`
- `MACRO_OLED_I2C_SCL_HZ`

Detailed behavior and validation checklist:
- [OLED Display](OLED-Display)

## 7) LED Brightness Groups
Defined in `main/keymap_config.h`:
- `MACRO_LED_INDICATOR_BRIGHTNESS`
  - Applies to indicator LEDs (USB mounted, HID ready, layer indicator).
- `MACRO_LED_KEY_BRIGHTNESS`
  - Applies to key backlight LEDs.

## 8) System Performance + Flash Layout
Primary settings live in `sdkconfig` / `sdkconfig.defaults`:
- Flash size target: `8MB`
- CPU frequency target: `240MHz`
- Compiler optimization: performance-oriented release profile
- Partition table: `partitions_8mb_ota.csv`

Partition goals:
- Two OTA slots: `ota_0`, `ota_1`
- Reserved data partition for future config persistence:
  - `cfgstore` (`1MB`, currently unused by firmware logic)

## 9) Buzzer Settings
Defined in `main/keymap_config.h`:
- `MACRO_BUZZER_ENABLED`
- `MACRO_BUZZER_GPIO`
- `MACRO_BUZZER_DUTY_PERCENT`
- `MACRO_BUZZER_QUEUE_SIZE`
- `MACRO_BUZZER_RTTTL_NOTE_GAP_MS`

Event toggles and tones:
- `MACRO_BUZZER_STARTUP_ENABLED` + `MACRO_BUZZER_RTTTL_STARTUP`
- `MACRO_BUZZER_KEYPRESS_ENABLED` + `MACRO_BUZZER_RTTTL_KEYPRESS`
- `MACRO_BUZZER_LAYER_SWITCH_ENABLED` + `MACRO_BUZZER_RTTTL_LAYER1/2/3`
- `MACRO_BUZZER_ENCODER_STEP_ENABLED` + `MACRO_BUZZER_RTTTL_ENCODER_CW/CCW`
- `MACRO_BUZZER_ENCODER_MIN_INTERVAL_MS` (fast-spin anti-backlog)

RTTTL format used:
- `name:d=<default_duration>,o=<default_octave>,b=<bpm>:<notes>`
- Example: `l2:d=16,o=6,b=180:g,g`

Behavior details:
- [Buzzer Feedback](Buzzer-Feedback)
