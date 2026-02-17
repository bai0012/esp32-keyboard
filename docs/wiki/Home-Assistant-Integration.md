# Home Assistant Integration

## 1) Goal
Provide a robust integration so MacroPad can:
- publish runtime events to Home Assistant
- read one Home Assistant entity state for OLED display
- trigger one Home Assistant service call for direct device control

without blocking HID/input responsiveness.

Current transports:
- Home Assistant REST event bus (`POST /api/events/<event_type>`)
- Home Assistant state API (`GET /api/states/<entity_id>`)
- Home Assistant service API (`POST /api/services/<domain>/<service>`)

Current implementation:
- `main/home_assistant.c`
- `main/home_assistant.h`

## 2) Design Summary
- Asynchronous queue-based publisher:
  - Input/runtime paths enqueue events and return immediately.
  - A dedicated worker task performs HTTP requests.
- Failure isolation:
  - Publish failures do not block key/encoder/touch processing.
  - Configurable retry count and request timeout.
- Runtime feature gating:
  - Entire bridge can be disabled by config.
  - Individual event families can be enabled/disabled.
  - Display polling and control action can be enabled/disabled independently.

## 3) Configuration
YAML keys in `config/keymap_config.yaml`:
- `enabled`
- `device_name`
- `event_prefix`
- `request_timeout_ms`
- `queue_size`
- `worker_interval_ms`
- `max_retry`
- `publish_layer_switch`
- `publish_key_event`
- `publish_encoder_step`
- `publish_touch_swipe`
- `display.enabled`
- `display.entity_id`
- `display.label`
- `display.poll_interval_ms`
- `control.enabled`
- `control.tap_count`
- `control.service_domain`
- `control.service_name`
- `control.entity_id`

Menuconfig keys (`idf.py menuconfig` -> `MacroPad Configuration`):
- `MACROPAD_HA_BASE_URL`
- `MACROPAD_HA_BEARER_TOKEN`

Example:

```yaml
home_assistant:
  enabled: true
  device_name: 'esp32-macropad'
  event_prefix: 'macropad'
  request_timeout_ms: 1800
  queue_size: 24
  worker_interval_ms: 30
  max_retry: 1
  publish_layer_switch: true
  publish_key_event: false
  publish_encoder_step: false
  publish_touch_swipe: false
  display:
    enabled: true
    entity_id: 'sensor.living_room_temperature'
    label: 'TEMP'
    poll_interval_ms: 3000
  control:
    enabled: true
    tap_count: 6
    service_domain: 'light'
    service_name: 'toggle'
    entity_id: 'light.desk_lamp'
```

Sensitive data policy:
- Keep URL/token only in `menuconfig`/`sdkconfig`.
- Do not store bearer token in repo-tracked YAML.

## 4) Event Families
- `layer_switch`
  - Published from layer transition path.
- `key_event`
  - Contains layer, key index, press/release state, usage, key label.
- `encoder_step`
  - Contains layer, signed step count, mapped usage.
- `touch_swipe`
  - Contains layer, swipe direction, mapped usage.

Event type naming:
- With prefix: `<event_prefix>_<suffix>` (example: `macropad_layer_switch`)
- Without prefix: `<suffix>`

## 5) OLED State Display
- Worker periodically polls configured `home_assistant.display.entity_id`.
- Parsed `state` (and optional `friendly_name`) is cached in module state.
- Display task renders cached line with clock (example: `TEMP: 23.6`).
- Poll failures do not block input/task loops and do not clear last good state immediately.

## 6) Service Control
- `home_assistant_trigger_default_control()` enqueues one service call action.
- Runtime trigger path (current default): encoder multi-tap count from `home_assistant.control.tap_count`.
- Posted payload:
```json
{"entity_id":"light.desk_lamp"}
```

## 7) Payload Notes
Every payload includes a `device` field from `home_assistant.device_name`.

Representative payload:

```json
{
  "device": "esp32-macropad",
  "layer_index": 1,
  "layer": 2,
  "key_index": 4,
  "key": 5,
  "pressed": true,
  "usage": 4,
  "name": "K5"
}
```

## 8) API Surface
See [API Reference](API-Reference), section `Home Assistant Module`.

Core APIs:
- `home_assistant_init()`
- `home_assistant_is_enabled()`
- `home_assistant_get_display_text(...)`
- `home_assistant_trigger_default_control()`
- `home_assistant_notify_layer_switch(...)`
- `home_assistant_notify_key_event(...)`
- `home_assistant_notify_encoder_step(...)`
- `home_assistant_notify_touch_swipe(...)`
- `home_assistant_queue_custom_event(...)`

## 9) Extension Guidance
- Add new event families by:
  1. Adding a queue event type in `main/home_assistant.c`.
  2. Adding serializer logic for payload.
  3. Calling `home_assistant_notify_*` from runtime hook.
  4. Adding YAML gating key if needed.
  5. Updating this page + `Configuration` + `API Reference`.
- Keep worker asynchronous; do not publish or service-call directly from input ISR/task paths.

## 10) Validation Checklist
1. Enable HA in YAML and set valid `MACROPAD_HA_BASE_URL`/`MACROPAD_HA_BEARER_TOKEN` in menuconfig.
2. Build/flash firmware.
3. Trigger enabled event family (e.g., switch layer).
4. Verify event reception in Home Assistant automation/developer tools.
5. Enable `home_assistant.display.*` and verify OLED state line updates.
6. Enable `home_assistant.control.*` and verify encoder multi-tap triggers service action.
7. Temporarily disconnect HA and verify input responsiveness remains normal.
