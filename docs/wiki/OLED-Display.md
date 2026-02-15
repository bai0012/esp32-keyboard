# OLED Display

## 1) Scope
This page documents the OLED subsystem behavior, protection policies, and tuning knobs.

Primary source files:
- `main/oled_clock.c`
- `main/oled_clock.h`
- `main/main.c` (`display_task`)
- `main/keymap_config.h`

## 2) What Is Rendered
- Main content: `HH:MM:SS` digital clock.
- Sync marker:
  - SNTP synced: marker at top-right.
  - Not synced: marker near bottom.
- All content is rendered into an internal framebuffer, then pushed over I2C.

## 3) Render Pipeline
1. `display_task` gets current local time.
2. OLED protection state is updated (dim/off/inversion/shift).
3. `oled_clock_render(timeinfo, shift_x, shift_y)` draws the frame.
4. Framebuffer is flushed to the panel.

## 4) Burn-In Protection Policies
### 4.1 Universal Pixel Shifting
- Applies to all rendered clock content as one shifted frame.
- Shift range: `+/- MACRO_OLED_SHIFT_RANGE_PX`.
- Shift interval: `MACRO_OLED_SHIFT_INTERVAL_SEC`.

### 4.2 Inactivity Dimming and Screen Off
- Dim after: `MACRO_OLED_DIM_TIMEOUT_SEC`.
- Off after: `MACRO_OLED_OFF_TIMEOUT_SEC`.
- Wake condition: any user activity (key, encoder, touch) restores panel and normal brightness.

### 4.3 Hourly Inversion
- Panel inversion toggles every hour to distribute wear.
- Guard: inversion scheduling starts only after time is considered synced/valid.
- Pre-sync placeholder time does not trigger false inversion transitions.

## 5) Brightness Control
- Default brightness: `MACRO_OLED_DEFAULT_BRIGHTNESS_PERCENT` (default `70`).
- Dimmed brightness: `MACRO_OLED_DIM_BRIGHTNESS_PERCENT`.
- Runtime APIs:
  - `oled_clock_set_brightness_percent()`
  - `oled_clock_set_display_enabled()`
  - `oled_clock_set_inverted()`

## 6) Configuration Knobs
Defined in `main/keymap_config.h`:
- `MACRO_OLED_DEFAULT_BRIGHTNESS_PERCENT`
- `MACRO_OLED_DIM_BRIGHTNESS_PERCENT`
- `MACRO_OLED_DIM_TIMEOUT_SEC`
- `MACRO_OLED_OFF_TIMEOUT_SEC`
- `MACRO_OLED_SHIFT_RANGE_PX`
- `MACRO_OLED_SHIFT_INTERVAL_SEC`

## 7) Validation Checklist
1. Boot without Wi-Fi: confirm no unexpected inversion when sync is unavailable.
2. Enable Wi-Fi SNTP: confirm inversion cadence starts only after valid time.
3. Stay idle past dim timeout: verify brightness drops.
4. Stay idle past off timeout: verify panel turns off.
5. Press any input: verify immediate wake and normal brightness restore.
6. Observe minute boundaries: verify subtle randomized pixel shift.

## 8) Common Tuning Notes
- If dimming is too aggressive: increase `MACRO_OLED_DIM_TIMEOUT_SEC`.
- If display feels too static: increase `MACRO_OLED_SHIFT_INTERVAL_SEC` frequency (lower value) or range.
- If movement is distracting: reduce `MACRO_OLED_SHIFT_RANGE_PX`.
- If panel wear risk is a concern: lower default brightness or shorten dim timeout.
