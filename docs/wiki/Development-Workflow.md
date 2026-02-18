# Development Workflow

## 1) Feature Development Steps
1. Identify which module owns the change.
2. Implement in module first; keep `main/main.c` orchestration-focused.
3. Expose minimal public API in headers.
4. Update `config/keymap_config.yaml` if behavior is intended to be configurable.
5. Regenerate `main/keymap_config.h` (automatic during build, or manually via generator script).
6. For OLED animations, update `assets/animations/manifest.yaml` and frame files.
7. Build and run target behavior tests.
8. Update documentation pages (mandatory).
9. Run post-change automation pipeline (`tools/post_change_pipeline.ps1`) for build/commit/push.

## 2) Module Ownership Guidance
- Input orchestration and task lifecycle: `main/main.c`
- HID protocol/report behavior: `main/macropad_hid.*`
- Touch gesture/hold logic: `main/touch_slider.*`
- OLED rendering logic: `main/oled.*`
- OLED animation assets/generator: `assets/animations/*`, `tools/generate_oled_animation_header.py`
- Local REST API foundation: `main/web_service.*`

## 3) Validation Checklist
- [ ] `idf.py build` passes
- [ ] No new runtime regressions in input behavior
- [ ] Touch swipe behavior verified with logs if touched
- [ ] Encoder tap and rotation behavior verified if touched
- [ ] README/wiki updated for changed behavior
- [ ] Main repo committed/pushed once per request
- [ ] Wiki committed/pushed if wiki pages changed

## 5) Standard Pipeline Commands
```powershell
# Start of a request
.\tools\post_change_pipeline.ps1 -Mode begin

# End of a request (after edits)
.\tools\post_change_pipeline.ps1 -Mode finish -Type feat -Message "short summary"
```

Rules enforced by the script:
- build gate (`idf.py build`) always runs before commit
- one commit per request
- stage only files changed since baseline
- push main repo with one rebase-retry on rejection
- sync/push wiki only when `docs/wiki/*.md` changed

## 4) Logging Practices
- Keep `ESP_LOGI` for operational signals.
- Keep high-volume diagnostic logs behind compile-time flags.
- Prefer structured/fielded logs for touch tuning.
