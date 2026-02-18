# OTA Update

## 1) Goals
- Provide a robust OTA pipeline for ESP32-S3 with rollback safety.
- Require explicit user confirmation after first boot of a new OTA image.
- Prevent bad firmware from being permanently accepted.

## 2) Module
- `main/ota_manager.c`
- `main/ota_manager.h`

Key responsibilities:
- Start OTA download (HTTPS) from runtime API call.
- Track OTA state for REST and OLED.
- Track OTA reception progress (`received bytes`, `total bytes`, `percent`, `elapsed`).
- Detect `ESP_OTA_IMG_PENDING_VERIFY` on first boot after OTA.
- Run self-check, then wait for EC11 confirmation.
- Confirm image (`esp_ota_mark_app_valid_cancel_rollback`) or rollback on timeout (`esp_ota_mark_app_invalid_rollback_and_reboot`).

## 3) Runtime Flow
1. Trigger OTA:
   - `POST /api/v1/system/ota` (optional JSON body URL override).
2. Firmware downloads using incremental OTA (`begin/perform/finish`) with continuous progress updates.
3. On success, device reboots into new image.
4. New image starts in `PENDING_VERIFY` state (rollback enabled).
5. `ota_manager` runs self-check for `ota.self_check_duration_ms`.
   - If a check fails, firmware retries self-check several times with a short interval before deciding rollback.
6. OLED shows verification prompt.
7. User presses EC11 N times (`ota.confirm_tap_count`, default `3`) to confirm.
8. If confirmed, rollback is canceled and firmware is finalized.
9. OLED shows a short "OTA confirmed" banner, then returns to normal scene automatically.
10. If timeout expires (`ota.confirm_timeout_sec`, non-zero), device rolls back automatically.

## 4) Configuration
Section in `config/keymap_config.yaml`:
- `ota.enabled`
- `ota.allow_http`
- `ota.skip_cert_verify`
- `ota.confirm_tap_count`
- `ota.confirm_timeout_sec`
- `ota.self_check_duration_ms`
- `ota.self_check_min_heap_bytes`
  - interpreted as warning threshold for low-memory conditions during post-OTA self-check
  - rollback decisions use an additional internal hard floor to avoid false rollbacks under temporary memory pressure

Menuconfig (`MacroPad Configuration`):
- `MACROPAD_OTA_DEFAULT_URL`
- `MACROPAD_OTA_HTTP_TIMEOUT_MS`

## 5) API
Base: `/api/v1`

- `GET /system/ota`
  - Returns OTA status state machine fields and download progress fields.
- `POST /system/ota`
  - Body (optional): `{"url":"https://example/fw.bin"}` or `{"url":"http://example/fw.bin"}`
  - If body URL is omitted, `CONFIG_MACROPAD_OTA_DEFAULT_URL` is used.
  - Requires `web_service.control_enabled=true`.
  - Requires web auth if API key / Basic Auth is configured.

Transport/security behavior:
- HTTPS is always allowed.
- HTTP is allowed only when `ota.allow_http=true`.
- HTTPS cert verification can be disabled only when `ota.skip_cert_verify=true`.

`GET /state` also includes nested `ota` object.

## 6) OLED Behavior
During OTA states, OLED overlay is shown:
- self-check running
- waiting confirmation
- download in progress
- download progress bar (`[####....] NN%`) + byte counters/rate
- download failure
- rollback/confirm terminal states

## 7) Build/Boot Requirements
- Rollback is enabled in defaults:
  - `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`
  - `CONFIG_APP_ROLLBACK_ENABLE=y`
- Partition table already provides dual OTA slots:
  - `partitions_8mb_ota.csv`

## 8) Validation Checklist
1. Start OTA with a valid HTTPS firmware URL.
2. Confirm reboot into new image.
3. Verify OLED shows self-check then confirmation prompt.
4. Press EC11 `ota.confirm_tap_count` times and verify confirm success.
5. Repeat OTA test but do not confirm; verify rollback occurs after timeout.
6. Verify `GET /api/v1/system/ota` and `/api/v1/state` reflect transitions.
7. During download, verify logs show progress updates (bytes/percent).
8. After EC11 confirm, verify OLED leaves "OTA confirmed" overlay automatically.
