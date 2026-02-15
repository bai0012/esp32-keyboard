# OLED Display

## 1) Scope
This page documents the OLED subsystem behavior, protection policies, and tuning knobs.

Primary source files:
- `main/oled.c`
- `main/oled.h`
- `main/oled_animation_assets.h` (generated)
- `main/main.c` (`display_task`)
- `config/keymap_config.yaml` (generated into `main/keymap_config.h`)
- `assets/animations/manifest.yaml`
- `tools/generate_oled_animation_header.py`

## 2) What Is Rendered
- Current scene: `HH:MM:SS` digital clock.
- Sync marker:
  - SNTP synced: marker at top-right.
  - Not synced: marker near bottom.
- All scene content is rendered into an internal framebuffer, then pushed over I2C.
- The module also exposes generic primitives for future text/bitmap/animation scenes.

## 3) Render Pipeline
1. `display_task` gets current local time.
2. OLED protection state is updated (dim/off/inversion/shift).
3. `oled_render_clock(timeinfo, shift_x, shift_y)` draws the current clock scene.
4. Framebuffer is flushed to the panel.

Boot path:
1. `app_main()` initializes OLED.
2. Boot animation frames from `g_oled_boot_animation` are rendered in sequence.
3. Runtime display task starts afterward.

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
  - `oled_set_brightness_percent()`
  - `oled_set_display_enabled()`
  - `oled_set_inverted()`

## 6) Text and CJK Compatibility
- UTF-8 text entry point: `oled_draw_text_utf8()`.
- Font lookup is callback-based (`oled_font_t` + `get_glyph`), so glyph storage is decoupled from renderer.
- Chinese/CJK support path:
  1. add a glyph provider for required codepoint ranges,
  2. map UTF-8 codepoints to bitmaps/advance metrics,
  3. call `oled_draw_text_utf8()` with that font.
- Missing glyphs currently render as fallback boxes to keep layout stable during incremental font rollout.

## 7) Animation Assets
- Source directory: `assets/animations/`
- Manifest: `assets/animations/manifest.yaml`
- Generated header: `main/oled_animation_assets.h`
- Frame format support (build-time conversion):
  - native: `.pbm` (P1/P4, no extra Python dependency)
  - optional (Pillow): `.png`, `.bmp`, `.jpg`, `.jpeg`
- Robustness guards in startup playback:
  - max frame count cap
  - per-frame duration clamp
  - max total boot-animation duration cap
  - safe skip when assets are missing/empty

## 8) Configuration Knobs
Defined in `config/keymap_config.yaml` under `oled.*` (generated macros in `main/keymap_config.h`):
- `MACRO_OLED_DEFAULT_BRIGHTNESS_PERCENT`
- `MACRO_OLED_DIM_BRIGHTNESS_PERCENT`
- `MACRO_OLED_DIM_TIMEOUT_SEC`
- `MACRO_OLED_OFF_TIMEOUT_SEC`
- `MACRO_OLED_SHIFT_RANGE_PX`
- `MACRO_OLED_SHIFT_INTERVAL_SEC`
- `MACRO_OLED_I2C_SCL_HZ`

I2C speed notes:
- Default is configured above standard 400kHz to improve perceived refresh responsiveness.
- Valid runtime clamp range in code: `100kHz .. 1MHz`.

## 9) Validation Checklist
1. Boot without Wi-Fi: confirm no unexpected inversion when sync is unavailable.
2. Enable Wi-Fi SNTP: confirm inversion cadence starts only after valid time.
3. Stay idle past dim timeout: verify brightness drops.
4. Stay idle past off timeout: verify panel turns off.
5. Press any input: verify immediate wake and normal brightness restore.
6. Observe minute boundaries: verify subtle randomized pixel shift.
7. Render UTF-8 strings with test glyph callbacks and verify fallback behavior for missing glyphs.
8. Verify boot animation renders both frames and startup continues even if one frame is invalid.

## 10) Common Tuning Notes
- If dimming is too aggressive: increase `MACRO_OLED_DIM_TIMEOUT_SEC`.
- If display feels too static: increase `MACRO_OLED_SHIFT_INTERVAL_SEC` frequency (lower value) or range.
- If movement is distracting: reduce `MACRO_OLED_SHIFT_RANGE_PX`.
- If panel wear risk is a concern: lower default brightness or shorten dim timeout.
