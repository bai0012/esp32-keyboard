#!/usr/bin/env python3
"""Generate main/keymap_config.h from config/keymap_config.yaml."""

from __future__ import annotations

import argparse
from pathlib import Path
from typing import Any

import yaml


def c_bool(value: bool) -> str:
    return "true" if bool(value) else "false"


def c_str(value: str) -> str:
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


def as_token(value: Any, field: str) -> str:
    if not isinstance(value, str) or not value:
        raise ValueError(f"{field} must be a non-empty token string")
    return value


def as_int(value: Any, field: str) -> int:
    if not isinstance(value, int):
        raise ValueError(f"{field} must be an integer")
    return value


def validate_count(items: list[Any], expected: int, field: str) -> None:
    if len(items) != expected:
        raise ValueError(f"{field} count mismatch: expected {expected}, got {len(items)}")


def render_header(cfg: dict[str, Any]) -> str:
    counts = cfg["counts"]
    key_count = as_int(counts["key"], "counts.key")
    layer_count = as_int(counts["layer"], "counts.layer")

    keymap_layers = cfg["keymap_layers"]
    validate_count(keymap_layers, layer_count, "keymap_layers")
    for idx, layer in enumerate(keymap_layers):
        keys = layer["keys"]
        validate_count(keys, key_count, f"keymap_layers[{idx}].keys")

    colors = cfg["layer_backlight_colors"]
    validate_count(colors, layer_count, "layer_backlight_colors")

    encoder_layers = cfg["encoder"]["layers"]
    validate_count(encoder_layers, layer_count, "encoder.layers")

    touch_layers = cfg["touch"]["layers"]
    validate_count(touch_layers, layer_count, "touch.layers")

    led = cfg["led"]
    encoder = cfg["encoder"]
    touch = cfg["touch"]
    oled = cfg["oled"]
    buzzer = cfg["buzzer"]
    ha = cfg.get("home_assistant", {
        "enabled": False,
        "device_name": "esp32-macropad",
        "event_prefix": "macropad",
        "request_timeout_ms": 1800,
        "queue_size": 24,
        "worker_interval_ms": 30,
        "max_retry": 1,
        "publish_layer_switch": True,
        "publish_key_event": False,
        "publish_encoder_step": False,
        "publish_touch_swipe": False,
        "display": {
            "enabled": False,
            "entity_id": "",
            "label": "HA",
            "poll_interval_ms": 3000,
        },
        "control": {
            "enabled": False,
            "tap_count": 6,
            "service_domain": "light",
            "service_name": "toggle",
            "entity_id": "",
        },
    })
    encoder_toggle = buzzer.get("encoder_toggle", {
        "enabled": False,
        "tap_count": 5,
        "on_rtttl": "",
        "off_rtttl": "",
    })

    out: list[str] = []
    out.append("// AUTO-GENERATED FILE. DO NOT EDIT.")
    out.append("// Source: config/keymap_config.yaml")
    out.append("")
    out.append("#pragma once")
    out.append("")
    out.append("#include <stdbool.h>")
    out.append("#include <stdint.h>")
    out.append("")
    out.append('#include "driver/gpio.h"')
    out.append('#include "class/hid/hid.h"')
    out.append("")
    out.append("typedef enum {")
    out.append("    MACRO_ACTION_NONE = 0,")
    out.append("    MACRO_ACTION_KEYBOARD,")
    out.append("    MACRO_ACTION_CONSUMER,")
    out.append("} macro_action_type_t;")
    out.append("")
    out.append("typedef struct {")
    out.append("    gpio_num_t gpio;")
    out.append("    bool active_low;")
    out.append("    uint8_t led_index;")
    out.append("    macro_action_type_t type;")
    out.append("    uint16_t usage;")
    out.append("    const char *name;")
    out.append("} macro_action_config_t;")
    out.append("")
    out.append("typedef struct {")
    out.append("    uint8_t r;")
    out.append("    uint8_t g;")
    out.append("    uint8_t b;")
    out.append("} macro_rgb_t;")
    out.append("")
    out.append("typedef struct {")
    out.append("    uint16_t button_single_usage;")
    out.append("    uint16_t cw_usage;")
    out.append("    uint16_t ccw_usage;")
    out.append("} macro_encoder_layer_config_t;")
    out.append("")
    out.append("typedef struct {")
    out.append("    uint16_t left_usage;")
    out.append("    uint16_t right_usage;")
    out.append("    bool left_hold_repeat;")
    out.append("    bool right_hold_repeat;")
    out.append("    uint16_t hold_start_ms;")
    out.append("    uint16_t hold_repeat_ms;")
    out.append("} macro_touch_layer_config_t;")
    out.append("")
    out.append(f"#define MACRO_KEY_COUNT {key_count}")
    out.append(f"#define MACRO_LAYER_COUNT {layer_count}")
    out.append("")
    out.append("static const macro_action_config_t g_macro_keymap_layers[MACRO_LAYER_COUNT][MACRO_KEY_COUNT] = {")
    for layer in keymap_layers:
        layer_name = layer.get("name", "Layer")
        out.append(f"    // {layer_name}")
        out.append("    {")
        for key in layer["keys"]:
            out.append(
                "        {"
                f"{as_token(key['gpio'], 'key.gpio')}, "
                f"{c_bool(key['active_low'])}, "
                f"{as_int(key['led_index'], 'key.led_index')}, "
                f"{as_token(key['type'], 'key.type')}, "
                f"{as_token(key['usage'], 'key.usage')}, "
                f"{c_str(str(key['name']))}"
                "},"
            )
        out.append("    },")
    out.append("};")
    out.append("")
    out.append("static const macro_rgb_t g_layer_backlight_color[MACRO_LAYER_COUNT] = {")
    for color in colors:
        out.append(
            "    {"
            f"{as_int(color['r'], 'layer_backlight_colors.r')}, "
            f"{as_int(color['g'], 'layer_backlight_colors.g')}, "
            f"{as_int(color['b'], 'layer_backlight_colors.b')}"
            "},"
        )
    out.append("};")
    out.append("")
    out.append(f"#define MACRO_LED_INDICATOR_BRIGHTNESS {as_int(led['indicator_brightness'], 'led.indicator_brightness')}")
    out.append(f"#define MACRO_LED_KEY_BRIGHTNESS {as_int(led['key_brightness'], 'led.key_brightness')}")
    out.append(f"#define MACRO_LAYER_KEY_DIM_SCALE {as_int(led['layer_key_dim_scale'], 'led.layer_key_dim_scale')}")
    out.append(f"#define MACRO_LAYER_KEY_ACTIVE_SCALE {as_int(led['layer_key_active_scale'], 'led.layer_key_active_scale')}")
    out.append("")
    out.append("static const macro_encoder_layer_config_t g_encoder_layer_config[MACRO_LAYER_COUNT] = {")
    for layer in encoder_layers:
        out.append(
            "    {"
            f"{as_token(layer['button_single_usage'], 'encoder.layers.button_single_usage')}, "
            f"{as_token(layer['cw_usage'], 'encoder.layers.cw_usage')}, "
            f"{as_token(layer['ccw_usage'], 'encoder.layers.ccw_usage')}"
            "},"
        )
    out.append("};")
    out.append("")
    out.append("static const macro_touch_layer_config_t g_touch_layer_config[MACRO_LAYER_COUNT] = {")
    for layer in touch_layers:
        out.append(
            "    {"
            f"{as_token(layer['left_usage'], 'touch.layers.left_usage')}, "
            f"{as_token(layer['right_usage'], 'touch.layers.right_usage')}, "
            f"{c_bool(layer['left_hold_repeat'])}, "
            f"{c_bool(layer['right_hold_repeat'])}, "
            f"{as_int(layer['hold_start_ms'], 'touch.layers.hold_start_ms')}, "
            f"{as_int(layer['hold_repeat_ms'], 'touch.layers.hold_repeat_ms')}"
            "},"
        )
    out.append("};")
    out.append("")
    out.append(f"#define MACRO_ENCODER_BUTTON_ACTIVE_LOW {c_bool(encoder['button_active_low'])}")
    out.append(f"#define MACRO_ENCODER_TAP_WINDOW_MS {as_int(encoder['tap_window_ms'], 'encoder.tap_window_ms')}")
    out.append(f"#define MACRO_ENCODER_SINGLE_TAP_DELAY_MS {as_int(encoder['single_tap_delay_ms'], 'encoder.single_tap_delay_ms')}")
    out.append("")
    out.append(f"#define MACRO_OLED_DEFAULT_BRIGHTNESS_PERCENT {as_int(oled['default_brightness_percent'], 'oled.default_brightness_percent')}")
    out.append(f"#define MACRO_OLED_DIM_BRIGHTNESS_PERCENT {as_int(oled['dim_brightness_percent'], 'oled.dim_brightness_percent')}")
    out.append(f"#define MACRO_OLED_DIM_TIMEOUT_SEC {as_int(oled['dim_timeout_sec'], 'oled.dim_timeout_sec')}")
    out.append(f"#define MACRO_OLED_OFF_TIMEOUT_SEC {as_int(oled['off_timeout_sec'], 'oled.off_timeout_sec')}")
    out.append(f"#define MACRO_OLED_SHIFT_RANGE_PX {as_int(oled['shift_range_px'], 'oled.shift_range_px')}")
    out.append(f"#define MACRO_OLED_SHIFT_INTERVAL_SEC {as_int(oled['shift_interval_sec'], 'oled.shift_interval_sec')}")
    out.append(f"#define MACRO_OLED_I2C_SCL_HZ {as_int(oled['i2c_scl_hz'], 'oled.i2c_scl_hz')}")
    out.append("")
    out.append(f"#define MACRO_BUZZER_ENABLED {c_bool(buzzer['enabled'])}")
    out.append(f"#define MACRO_BUZZER_GPIO {as_token(buzzer['gpio'], 'buzzer.gpio')}")
    out.append(f"#define MACRO_BUZZER_DUTY_PERCENT {as_int(buzzer['duty_percent'], 'buzzer.duty_percent')}")
    out.append(f"#define MACRO_BUZZER_QUEUE_SIZE {as_int(buzzer['queue_size'], 'buzzer.queue_size')}")
    out.append(f"#define MACRO_BUZZER_RTTTL_NOTE_GAP_MS {as_int(buzzer['rtttl_note_gap_ms'], 'buzzer.rtttl_note_gap_ms')}")
    out.append("")
    out.append(f"#define MACRO_BUZZER_STARTUP_ENABLED {c_bool(buzzer['startup']['enabled'])}")
    out.append(f"#define MACRO_BUZZER_RTTTL_STARTUP {c_str(str(buzzer['startup']['rtttl']))}")
    out.append("")
    out.append(f"#define MACRO_BUZZER_KEYPRESS_ENABLED {c_bool(buzzer['keypress']['enabled'])}")
    out.append(f"#define MACRO_BUZZER_RTTTL_KEYPRESS {c_str(str(buzzer['keypress']['rtttl']))}")
    out.append("")
    out.append(f"#define MACRO_BUZZER_LAYER_SWITCH_ENABLED {c_bool(buzzer['layer_switch']['enabled'])}")
    out.append(f"#define MACRO_BUZZER_RTTTL_LAYER1 {c_str(str(buzzer['layer_switch']['layer1_rtttl']))}")
    out.append(f"#define MACRO_BUZZER_RTTTL_LAYER2 {c_str(str(buzzer['layer_switch']['layer2_rtttl']))}")
    out.append(f"#define MACRO_BUZZER_RTTTL_LAYER3 {c_str(str(buzzer['layer_switch']['layer3_rtttl']))}")
    out.append("")
    out.append(f"#define MACRO_BUZZER_ENCODER_STEP_ENABLED {c_bool(buzzer['encoder_step']['enabled'])}")
    out.append(f"#define MACRO_BUZZER_RTTTL_ENCODER_CW {c_str(str(buzzer['encoder_step']['cw_rtttl']))}")
    out.append(f"#define MACRO_BUZZER_RTTTL_ENCODER_CCW {c_str(str(buzzer['encoder_step']['ccw_rtttl']))}")
    out.append(f"#define MACRO_BUZZER_ENCODER_MIN_INTERVAL_MS {as_int(buzzer['encoder_step']['min_interval_ms'], 'buzzer.encoder_step.min_interval_ms')}")
    out.append(f"#define MACRO_BUZZER_ENCODER_TOGGLE_ENABLED {c_bool(encoder_toggle['enabled'])}")
    out.append(f"#define MACRO_BUZZER_ENCODER_TOGGLE_TAP_COUNT {as_int(encoder_toggle['tap_count'], 'buzzer.encoder_toggle.tap_count')}")
    out.append(f"#define MACRO_BUZZER_RTTTL_TOGGLE_ON {c_str(str(encoder_toggle['on_rtttl']))}")
    out.append(f"#define MACRO_BUZZER_RTTTL_TOGGLE_OFF {c_str(str(encoder_toggle['off_rtttl']))}")
    out.append("")
    out.append(f"#define MACRO_HA_ENABLED {c_bool(ha['enabled'])}")
    out.append(f"#define MACRO_HA_DEVICE_NAME {c_str(str(ha['device_name']))}")
    out.append(f"#define MACRO_HA_EVENT_PREFIX {c_str(str(ha['event_prefix']))}")
    out.append(f"#define MACRO_HA_REQUEST_TIMEOUT_MS {as_int(ha['request_timeout_ms'], 'home_assistant.request_timeout_ms')}")
    out.append(f"#define MACRO_HA_QUEUE_SIZE {as_int(ha['queue_size'], 'home_assistant.queue_size')}")
    out.append(f"#define MACRO_HA_WORKER_INTERVAL_MS {as_int(ha['worker_interval_ms'], 'home_assistant.worker_interval_ms')}")
    out.append(f"#define MACRO_HA_MAX_RETRY {as_int(ha['max_retry'], 'home_assistant.max_retry')}")
    out.append(f"#define MACRO_HA_PUBLISH_LAYER_SWITCH {c_bool(ha['publish_layer_switch'])}")
    out.append(f"#define MACRO_HA_PUBLISH_KEY_EVENT {c_bool(ha['publish_key_event'])}")
    out.append(f"#define MACRO_HA_PUBLISH_ENCODER_STEP {c_bool(ha['publish_encoder_step'])}")
    out.append(f"#define MACRO_HA_PUBLISH_TOUCH_SWIPE {c_bool(ha['publish_touch_swipe'])}")
    ha_display = ha.get("display", {})
    ha_control = ha.get("control", {})
    out.append(f"#define MACRO_HA_DISPLAY_ENABLED {c_bool(ha_display.get('enabled', False))}")
    out.append(f"#define MACRO_HA_DISPLAY_ENTITY_ID {c_str(str(ha_display.get('entity_id', '')))}")
    out.append(f"#define MACRO_HA_DISPLAY_LABEL {c_str(str(ha_display.get('label', 'HA')))}")
    out.append(f"#define MACRO_HA_DISPLAY_POLL_INTERVAL_MS {as_int(ha_display.get('poll_interval_ms', 3000), 'home_assistant.display.poll_interval_ms')}")
    out.append(f"#define MACRO_HA_CONTROL_ENABLED {c_bool(ha_control.get('enabled', False))}")
    out.append(f"#define MACRO_HA_CONTROL_TAP_COUNT {as_int(ha_control.get('tap_count', 6), 'home_assistant.control.tap_count')}")
    out.append(f"#define MACRO_HA_CONTROL_DOMAIN {c_str(str(ha_control.get('service_domain', '')))}")
    out.append(f"#define MACRO_HA_CONTROL_SERVICE {c_str(str(ha_control.get('service_name', '')))}")
    out.append(f"#define MACRO_HA_CONTROL_ENTITY_ID {c_str(str(ha_control.get('entity_id', '')))}")
    out.append("")
    out.append(f"#define MACRO_TOUCH_TRIGGER_PERCENT {as_int(touch['trigger_percent'], 'touch.trigger_percent')}")
    out.append(f"#define MACRO_TOUCH_RELEASE_PERCENT {as_int(touch['release_percent'], 'touch.release_percent')}")
    out.append(f"#define MACRO_TOUCH_TRIGGER_MIN_DELTA {as_int(touch['trigger_min_delta'], 'touch.trigger_min_delta')}")
    out.append(f"#define MACRO_TOUCH_RELEASE_MIN_DELTA {as_int(touch['release_min_delta'], 'touch.release_min_delta')}")
    out.append(f"#define MACRO_TOUCH_GESTURE_WINDOW_MS {as_int(touch['gesture_window_ms'], 'touch.gesture_window_ms')}")
    out.append(f"#define MACRO_TOUCH_MIN_INTERVAL_MS {as_int(touch['min_interval_ms'], 'touch.min_interval_ms')}")
    out.append(f"#define MACRO_TOUCH_BASELINE_FREEZE_TOTAL_DELTA {as_int(touch['baseline_freeze_total_delta'], 'touch.baseline_freeze_total_delta')}")
    out.append(f"#define MACRO_TOUCH_BASELINE_FREEZE_SIDE_DELTA {as_int(touch['baseline_freeze_side_delta'], 'touch.baseline_freeze_side_delta')}")
    out.append(f"#define MACRO_TOUCH_CONTACT_MIN_TOTAL_DELTA {as_int(touch['contact_min_total_delta'], 'touch.contact_min_total_delta')}")
    out.append(f"#define MACRO_TOUCH_CONTACT_MIN_SIDE_DELTA {as_int(touch['contact_min_side_delta'], 'touch.contact_min_side_delta')}")
    out.append(f"#define MACRO_TOUCH_START_SIDE_DELTA {as_int(touch['start_side_delta'], 'touch.start_side_delta')}")
    out.append(f"#define MACRO_TOUCH_GESTURE_TRAVEL_DELTA {as_int(touch['gesture_travel_delta'], 'touch.gesture_travel_delta')}")
    out.append(f"#define MACRO_TOUCH_SWIPE_SIDE_MIN_DELTA {as_int(touch['swipe_side_min_delta'], 'touch.swipe_side_min_delta')}")
    out.append(f"#define MACRO_TOUCH_SWIPE_SIDE_RELATIVE_PERCENT {as_int(touch['swipe_side_relative_percent'], 'touch.swipe_side_relative_percent')}")
    out.append(f"#define MACRO_TOUCH_REQUIRE_BOTH_SIDES {c_bool(touch['require_both_sides'])}")
    out.append(f"#define MACRO_TOUCH_BOTH_SIDES_HOLD_MS {as_int(touch['both_sides_hold_ms'], 'touch.both_sides_hold_ms')}")
    out.append(f"#define MACRO_TOUCH_SIDE_SEQUENCE_MIN_MS {as_int(touch['side_sequence_min_ms'], 'touch.side_sequence_min_ms')}")
    out.append(f"#define MACRO_TOUCH_START_DOMINANT_MIN_MS {as_int(touch['start_dominant_min_ms'], 'touch.start_dominant_min_ms')}")
    out.append(f"#define MACRO_TOUCH_MIN_SWIPE_MS {as_int(touch['min_swipe_ms'], 'touch.min_swipe_ms')}")
    out.append(f"#define MACRO_TOUCH_DIRECTION_DOMINANCE_DELTA {as_int(touch['direction_dominance_delta'], 'touch.direction_dominance_delta')}")
    out.append(f"#define MACRO_TOUCH_SWAP_SIDES {c_bool(touch['swap_sides'])}")
    out.append(f"#define MACRO_TOUCH_DEBUG_LOG_ENABLE {c_bool(touch['debug_log_enable'])}")
    out.append(f"#define MACRO_TOUCH_DEBUG_LOG_INTERVAL_MS {as_int(touch['debug_log_interval_ms'], 'touch.debug_log_interval_ms')}")
    out.append(f"#define MACRO_TOUCH_IDLE_NOISE_MARGIN {as_int(touch['idle_noise_margin'], 'touch.idle_noise_margin')}")
    out.append(f"#define MACRO_TOUCH_IDLE_NOISE_MAX_DELTA {as_int(touch['idle_noise_max_delta'], 'touch.idle_noise_max_delta')}")
    out.append("")
    return "\n".join(out) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate keymap_config.h from YAML")
    parser.add_argument("--in", dest="input_path", required=True, help="Input YAML file path")
    parser.add_argument("--out", dest="output_path", required=True, help="Output header file path")
    args = parser.parse_args()

    input_path = Path(args.input_path)
    output_path = Path(args.output_path)

    if not input_path.is_file():
        raise FileNotFoundError(f"Input YAML not found: {input_path}")

    with input_path.open("r", encoding="utf-8") as f:
        cfg = yaml.safe_load(f)
    if not isinstance(cfg, dict):
        raise ValueError("YAML root must be a mapping")

    rendered = render_header(cfg)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(rendered, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
