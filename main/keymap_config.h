#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "class/hid/hid.h"

typedef enum {
    MACRO_ACTION_NONE = 0,
    MACRO_ACTION_KEYBOARD,
    MACRO_ACTION_CONSUMER,
} macro_action_type_t;

typedef struct {
    gpio_num_t gpio;
    bool active_low;
    uint8_t led_index;   // 0xFF means no LED assigned
    macro_action_type_t type;
    uint16_t usage;
    const char *name;
} macro_action_config_t;

typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
} macro_rgb_t;

typedef struct {
    uint16_t button_single_usage;
    uint16_t cw_usage;
    uint16_t ccw_usage;
} macro_encoder_layer_config_t;

typedef struct {
    uint16_t left_usage;
    uint16_t right_usage;
    bool left_hold_repeat;
    bool right_hold_repeat;
    uint16_t hold_start_ms;
    uint16_t hold_repeat_ms;
} macro_touch_layer_config_t;

#define MACRO_KEY_COUNT 12
#define MACRO_LAYER_COUNT 3

/*
 * Edit these tables to change per-key behavior per layer.
 * - type = MACRO_ACTION_KEYBOARD: usage uses HID_KEY_* values.
 * - type = MACRO_ACTION_CONSUMER: usage uses HID_USAGE_CONSUMER_* values.
 * - active_low = true for keys wired to GND when pressed.
 * Keep gpio/active_low/led_index consistent across layers unless you have custom wiring.
 */
static const macro_action_config_t g_macro_keymap_layers[MACRO_LAYER_COUNT][MACRO_KEY_COUNT] = {
    // Layer 1 (default)
    {
        {GPIO_NUM_7,  true, 3,  MACRO_ACTION_KEYBOARD, HID_KEY_A,   "K1"},
        {GPIO_NUM_8,  true, 4,  MACRO_ACTION_KEYBOARD, HID_KEY_B,   "K2"},
        {GPIO_NUM_9,  true, 5,  MACRO_ACTION_KEYBOARD, HID_KEY_C,   "K3"},
        {GPIO_NUM_17, true, 6,  MACRO_ACTION_KEYBOARD, HID_KEY_D,   "K4"},
        {GPIO_NUM_18, true, 10, MACRO_ACTION_KEYBOARD, HID_KEY_F17, "K5"},
        {GPIO_NUM_12, true, 9,  MACRO_ACTION_KEYBOARD, HID_KEY_F18, "K6"},
        {GPIO_NUM_13, true, 8,  MACRO_ACTION_KEYBOARD, HID_KEY_F19, "K7"},
        {GPIO_NUM_14, true, 7,  MACRO_ACTION_KEYBOARD, HID_KEY_F20, "K8"},
        {GPIO_NUM_1,  true, 11, MACRO_ACTION_KEYBOARD, HID_KEY_F21, "K9"},
        {GPIO_NUM_2,  true, 12, MACRO_ACTION_KEYBOARD, HID_KEY_F22, "K10"},
        {GPIO_NUM_40, true, 13, MACRO_ACTION_KEYBOARD, HID_KEY_F23, "K11"},
        {GPIO_NUM_41, true, 14, MACRO_ACTION_KEYBOARD, HID_KEY_F24, "K12"},
    },
    // Layer 2
    {
        {GPIO_NUM_7,  true, 3,  MACRO_ACTION_KEYBOARD, HID_KEY_1, "K1"},
        {GPIO_NUM_8,  true, 4,  MACRO_ACTION_KEYBOARD, HID_KEY_2, "K2"},
        {GPIO_NUM_9,  true, 5,  MACRO_ACTION_KEYBOARD, HID_KEY_3, "K3"},
        {GPIO_NUM_17, true, 6,  MACRO_ACTION_KEYBOARD, HID_KEY_4, "K4"},
        {GPIO_NUM_18, true, 10, MACRO_ACTION_KEYBOARD, HID_KEY_5, "K5"},
        {GPIO_NUM_12, true, 9,  MACRO_ACTION_KEYBOARD, HID_KEY_6, "K6"},
        {GPIO_NUM_13, true, 8,  MACRO_ACTION_KEYBOARD, HID_KEY_7, "K7"},
        {GPIO_NUM_14, true, 7,  MACRO_ACTION_KEYBOARD, HID_KEY_8, "K8"},
        {GPIO_NUM_1,  true, 11, MACRO_ACTION_KEYBOARD, HID_KEY_9, "K9"},
        {GPIO_NUM_2,  true, 12, MACRO_ACTION_KEYBOARD, HID_KEY_0, "K10"},
        {GPIO_NUM_40, true, 13, MACRO_ACTION_KEYBOARD, HID_KEY_MINUS, "K11"},
        {GPIO_NUM_41, true, 14, MACRO_ACTION_KEYBOARD, HID_KEY_EQUAL, "K12"},
    },
    // Layer 3
    {
        {GPIO_NUM_7,  true, 3,  MACRO_ACTION_CONSUMER, HID_USAGE_CONSUMER_MUTE, "K1"},
        {GPIO_NUM_8,  true, 4,  MACRO_ACTION_CONSUMER, HID_USAGE_CONSUMER_VOLUME_DECREMENT, "K2"},
        {GPIO_NUM_9,  true, 5,  MACRO_ACTION_CONSUMER, HID_USAGE_CONSUMER_VOLUME_INCREMENT, "K3"},
        {GPIO_NUM_17, true, 6,  MACRO_ACTION_CONSUMER, HID_USAGE_CONSUMER_PLAY_PAUSE, "K4"},
        {GPIO_NUM_18, true, 10, MACRO_ACTION_KEYBOARD, HID_KEY_F1, "K5"},
        {GPIO_NUM_12, true, 9,  MACRO_ACTION_KEYBOARD, HID_KEY_F2, "K6"},
        {GPIO_NUM_13, true, 8,  MACRO_ACTION_KEYBOARD, HID_KEY_F3, "K7"},
        {GPIO_NUM_14, true, 7,  MACRO_ACTION_KEYBOARD, HID_KEY_F4, "K8"},
        {GPIO_NUM_1,  true, 11, MACRO_ACTION_KEYBOARD, HID_KEY_F5, "K9"},
        {GPIO_NUM_2,  true, 12, MACRO_ACTION_KEYBOARD, HID_KEY_F6, "K10"},
        {GPIO_NUM_40, true, 13, MACRO_ACTION_KEYBOARD, HID_KEY_F7, "K11"},
        {GPIO_NUM_41, true, 14, MACRO_ACTION_KEYBOARD, HID_KEY_F8, "K12"},
    },
};

/*
 * Per-layer backlight base color:
 * Key backlight uses dim + bright levels of this color.
 */
static const macro_rgb_t g_layer_backlight_color[MACRO_LAYER_COUNT] = {
    {90, 90, 0},   // Layer 1: Yellow
    {0, 90, 0},    // Layer 2: Green
    {0, 0, 90},    // Layer 3: Blue
};

// 0..255 scales applied to g_layer_backlight_color for idle and pressed key brightness.
#define MACRO_LAYER_KEY_DIM_SCALE 45
#define MACRO_LAYER_KEY_ACTIVE_SCALE 140

/*
 * Per-layer EC11 mapping.
 * Single click is sent after tap window + single-tap delay timeout.
 */
static const macro_encoder_layer_config_t g_encoder_layer_config[MACRO_LAYER_COUNT] = {
    {HID_USAGE_CONSUMER_PLAY_PAUSE, HID_USAGE_CONSUMER_VOLUME_INCREMENT, HID_USAGE_CONSUMER_VOLUME_DECREMENT},
    {HID_USAGE_CONSUMER_SCAN_NEXT_TRACK, HID_USAGE_CONSUMER_VOLUME_INCREMENT, HID_USAGE_CONSUMER_VOLUME_DECREMENT},
    {HID_USAGE_CONSUMER_PLAY_PAUSE, HID_USAGE_CONSUMER_SCAN_NEXT_TRACK, HID_USAGE_CONSUMER_SCAN_PREVIOUS_TRACK},
};

/*
 * Per-layer touch slider mapping.
 * Gesture semantics:
 * - RIGHT -> LEFT slide triggers left_usage
 * - LEFT -> RIGHT slide triggers right_usage
 * Set usage to 0 to disable an action for that direction.
 */
static const macro_touch_layer_config_t g_touch_layer_config[MACRO_LAYER_COUNT] = {
    // Layer 1: track switch one-shot on slide, no hold-repeat
    {HID_USAGE_CONSUMER_SCAN_PREVIOUS_TRACK, HID_USAGE_CONSUMER_SCAN_NEXT_TRACK, false, false, 0, 0},
    // Layer 2: volume slide supports hold-repeat at the edge (press-and-hold behavior)
    {HID_USAGE_CONSUMER_VOLUME_DECREMENT, HID_USAGE_CONSUMER_VOLUME_INCREMENT, true, true, 220, 110},
    // Layer 3: brightness one-shot by default
    {HID_USAGE_CONSUMER_BRIGHTNESS_DECREMENT, HID_USAGE_CONSUMER_BRIGHTNESS_INCREMENT, false, false, 0, 0},
};

#define MACRO_ENCODER_BUTTON_ACTIVE_LOW true
#define MACRO_ENCODER_TAP_WINDOW_MS 350
#define MACRO_ENCODER_SINGLE_TAP_DELAY_MS 120

/*
 * OLED screen-protection settings.
 * - Brightness values are in percent (0..100).
 * - Inactivity timeouts are in seconds.
 */
#define MACRO_OLED_DEFAULT_BRIGHTNESS_PERCENT 70
#define MACRO_OLED_DIM_BRIGHTNESS_PERCENT 15
#define MACRO_OLED_DIM_TIMEOUT_SEC 45
#define MACRO_OLED_OFF_TIMEOUT_SEC 180
#define MACRO_OLED_SHIFT_RANGE_PX 2
#define MACRO_OLED_SHIFT_INTERVAL_SEC 60

// Touch active threshold = baseline * percent / 100
#define MACRO_TOUCH_TRIGGER_PERCENT 85
// Touch release threshold = baseline * percent / 100 (should be > trigger threshold)
#define MACRO_TOUCH_RELEASE_PERCENT 92
// Additional delta guard (baseline - raw) to avoid false active after release.
#define MACRO_TOUCH_TRIGGER_MIN_DELTA 3500
#define MACRO_TOUCH_RELEASE_MIN_DELTA 1800
// Max time between first side and second side to count as a slide gesture.
#define MACRO_TOUCH_GESTURE_WINDOW_MS 650
// Min interval between accepted slide gestures.
#define MACRO_TOUCH_MIN_INTERVAL_MS 280
// Freeze baseline early when touch starts to avoid baseline chasing light swipes.
#define MACRO_TOUCH_BASELINE_FREEZE_TOTAL_DELTA 1200
#define MACRO_TOUCH_BASELINE_FREEZE_SIDE_DELTA 600
// Minimum combined touch strength (dL + dR) to consider finger contact valid.
#define MACRO_TOUCH_CONTACT_MIN_TOTAL_DELTA 1500
// Minimum single-side strength to treat light one-side contact as valid.
#define MACRO_TOUCH_CONTACT_MIN_SIDE_DELTA 700
// Minimum |dR-dL| to establish initial swipe side.
#define MACRO_TOUCH_START_SIDE_DELTA 250
// Minimum filtered balance travel from touch origin to fire a swipe.
#define MACRO_TOUCH_GESTURE_TRAVEL_DELTA 450
// Minimum compensated side delta to count that side as "visited" in a swipe.
#define MACRO_TOUCH_SWIPE_SIDE_MIN_DELTA 1500
// A side is considered truly visited only if it is not tiny crosstalk from the other side.
#define MACRO_TOUCH_SWIPE_SIDE_RELATIVE_PERCENT 20
// Require both sides to be visited before reporting a swipe (rejects tap spikes).
#define MACRO_TOUCH_REQUIRE_BOTH_SIDES true
// After both sides are visited, keep contact for this long before allowing a swipe.
#define MACRO_TOUCH_BOTH_SIDES_HOLD_MS 50
// Minimum delay between first-side and second-side activation to count as real swipe.
#define MACRO_TOUCH_SIDE_SEQUENCE_MIN_MS 20
// Start side must stay dominant for this long before opposite crossing can fire.
#define MACRO_TOUCH_START_DOMINANT_MIN_MS 30
// Minimum touch-session duration before swipe can be emitted.
#define MACRO_TOUCH_MIN_SWIPE_MS 100
// Minimum |dR-dL| to confirm direction switch and fire gesture.
#define MACRO_TOUCH_DIRECTION_DOMINANCE_DELTA 650
// Set true only if physical slider orientation is mirrored vs expected left/right.
#define MACRO_TOUCH_SWAP_SIDES false
// Touch diagnostics: periodic internal-state log for tuning.
#define MACRO_TOUCH_DEBUG_LOG_ENABLE false
#define MACRO_TOUCH_DEBUG_LOG_INTERVAL_MS 80
// Idle-noise compensation for touch deltas (helps reject board bias/drift).
#define MACRO_TOUCH_IDLE_NOISE_MARGIN 120
#define MACRO_TOUCH_IDLE_NOISE_MAX_DELTA 2400
