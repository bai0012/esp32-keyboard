# Development Workflow

## 1) Feature Development Steps
1. Identify which module owns the change.
2. Implement in module first; keep `main/main.c` orchestration-focused.
3. Expose minimal public API in headers.
4. Update `main/keymap_config.h` if behavior is intended to be configurable.
5. Build and run target behavior tests.
6. Update documentation pages (mandatory).

## 2) Module Ownership Guidance
- Input orchestration and task lifecycle: `main/main.c`
- HID protocol/report behavior: `main/macropad_hid.*`
- Touch gesture/hold logic: `main/touch_slider.*`
- OLED rendering logic: `main/oled_clock.*`

## 3) Validation Checklist
- [ ] `idf.py build` passes
- [ ] No new runtime regressions in input behavior
- [ ] Touch swipe behavior verified with logs if touched
- [ ] Encoder tap and rotation behavior verified if touched
- [ ] README/wiki updated for changed behavior

## 4) Logging Practices
- Keep `ESP_LOGI` for operational signals.
- Keep high-volume diagnostic logs behind compile-time flags.
- Prefer structured/fielded logs for touch tuning.
