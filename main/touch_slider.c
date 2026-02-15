#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/touch_sensor_legacy.h"

#include "esp_log.h"

#include "keymap_config.h"

#include "touch_slider.h"

#define TAG "MACROPAD"

#define TOUCH_LEFT_PAD TOUCH_PAD_NUM11
#define TOUCH_RIGHT_PAD TOUCH_PAD_NUM10

typedef enum {
    TOUCH_SIDE_NONE = 0,
    TOUCH_SIDE_LEFT,
    TOUCH_SIDE_RIGHT,
} touch_side_t;

static uint32_t s_touch_left_baseline = 0;
static uint32_t s_touch_right_baseline = 0;
static bool s_touch_left_active = false;
static bool s_touch_right_active = false;
static touch_side_t s_touch_start_side = TOUCH_SIDE_NONE;
static TickType_t s_touch_start_tick = 0;
static bool s_touch_gesture_fired = false;
static bool s_touch_session_active = false;
static bool s_touch_seen_left = false;
static bool s_touch_seen_right = false;
static TickType_t s_touch_seen_left_tick = 0;
static TickType_t s_touch_seen_right_tick = 0;
static TickType_t s_touch_both_seen_tick = 0;
static TickType_t s_touch_opposite_dominant_tick = 0;
static TickType_t s_touch_start_dominant_tick = 0;
static TickType_t s_touch_last_gesture_tick = 0;
static int32_t s_touch_balance_filtered = 0;
static int32_t s_touch_balance_origin = 0;
static uint32_t s_touch_left_idle_noise = 0;
static uint32_t s_touch_right_idle_noise = 0;
static bool s_touch_hold_active = false;
static touch_side_t s_touch_hold_side = TOUCH_SIDE_NONE;
static uint16_t s_touch_hold_usage = 0;
static TickType_t s_touch_hold_next_tick = 0;
#if MACRO_TOUCH_DEBUG_LOG_ENABLE
static TickType_t s_touch_last_debug_tick = 0;
#endif
static TickType_t s_touch_last_sensor_active_tick = 0;

static bool touch_is_active(uint32_t raw, uint32_t baseline, bool was_active)
{
    if (baseline == 0) {
        return false;
    }
    const uint32_t delta = (baseline > raw) ? (baseline - raw) : (raw - baseline);
    const uint32_t trigger_percent_delta = (baseline * (100U - MACRO_TOUCH_TRIGGER_PERCENT)) / 100U;
    const uint32_t release_percent_delta = (baseline * (100U - MACRO_TOUCH_RELEASE_PERCENT)) / 100U;
    const uint32_t trigger_threshold = (trigger_percent_delta > MACRO_TOUCH_TRIGGER_MIN_DELTA) ?
                                       trigger_percent_delta : MACRO_TOUCH_TRIGGER_MIN_DELTA;
    const uint32_t release_threshold = (release_percent_delta > MACRO_TOUCH_RELEASE_MIN_DELTA) ?
                                       release_percent_delta : MACRO_TOUCH_RELEASE_MIN_DELTA;
    return was_active ? (delta >= release_threshold) : (delta >= trigger_threshold);
}

static inline uint32_t touch_delta(uint32_t raw, uint32_t baseline)
{
    return (baseline > raw) ? (baseline - raw) : (raw - baseline);
}

static inline uint32_t touch_apply_noise_comp(uint32_t delta, uint32_t idle_noise)
{
    const uint32_t floor = idle_noise + MACRO_TOUCH_IDLE_NOISE_MARGIN;
    return (delta > floor) ? (delta - floor) : 0U;
}

static touch_side_t touch_dominant_side_from_balance(int32_t balance)
{
    if (balance <= -(int32_t)MACRO_TOUCH_DIRECTION_DOMINANCE_DELTA) {
        return TOUCH_SIDE_LEFT;
    }
    if (balance >= (int32_t)MACRO_TOUCH_DIRECTION_DOMINANCE_DELTA) {
        return TOUCH_SIDE_RIGHT;
    }
    return TOUCH_SIDE_NONE;
}

#if MACRO_TOUCH_DEBUG_LOG_ENABLE
static const char *touch_side_to_str(touch_side_t side)
{
    if (side == TOUCH_SIDE_LEFT) {
        return "L";
    }
    if (side == TOUCH_SIDE_RIGHT) {
        return "R";
    }
    return "N";
}

static void touch_log_debug(TickType_t now,
                            uint32_t left_raw,
                            uint32_t right_raw,
                            uint32_t left_delta_raw,
                            uint32_t right_delta_raw,
                            uint32_t left_delta,
                            uint32_t right_delta,
                            uint32_t total_delta,
                            int32_t balance,
                            int32_t balance_filtered,
                            bool touch_engaged,
                            bool baseline_freeze,
                            bool left_now,
                            bool right_now,
                            touch_side_t dominant_side)
{
    const TickType_t interval_ticks = pdMS_TO_TICKS(MACRO_TOUCH_DEBUG_LOG_INTERVAL_MS);
    if ((now - s_touch_last_debug_tick) < interval_ticks) {
        return;
    }
    s_touch_last_debug_tick = now;

    ESP_LOGI(TAG,
             "Touch dbg rawL=%lu rawR=%lu baseL=%lu baseR=%lu dLr=%lu dRr=%lu nL=%lu nR=%lu dL=%lu dR=%lu tot=%lu bal=%ld dom=%s eng=%d frz=%d lNow=%d rNow=%d sess=%d seenL=%d seenR=%d start=%s fired=%d flt=%ld org=%ld trv=%ld",
             (unsigned long)left_raw,
             (unsigned long)right_raw,
             (unsigned long)s_touch_left_baseline,
             (unsigned long)s_touch_right_baseline,
             (unsigned long)left_delta_raw,
             (unsigned long)right_delta_raw,
             (unsigned long)s_touch_left_idle_noise,
             (unsigned long)s_touch_right_idle_noise,
             (unsigned long)left_delta,
             (unsigned long)right_delta,
             (unsigned long)total_delta,
             (long)balance,
             touch_side_to_str(dominant_side),
             touch_engaged,
             baseline_freeze,
             left_now,
             right_now,
             s_touch_session_active,
             s_touch_seen_left,
             s_touch_seen_right,
             touch_side_to_str(s_touch_start_side),
             s_touch_gesture_fired,
             (long)balance_filtered,
             (long)s_touch_balance_origin,
             (long)(balance_filtered - s_touch_balance_origin));
}
#else
static inline void touch_log_debug(TickType_t now,
                                   uint32_t left_raw,
                                   uint32_t right_raw,
                                   uint32_t left_delta_raw,
                                   uint32_t right_delta_raw,
                                   uint32_t left_delta,
                                   uint32_t right_delta,
                                   uint32_t total_delta,
                                   int32_t balance,
                                   int32_t balance_filtered,
                                   bool touch_engaged,
                                   bool baseline_freeze,
                                   bool left_now,
                                   bool right_now,
                                   touch_side_t dominant_side)
{
    (void)now;
    (void)left_raw;
    (void)right_raw;
    (void)left_delta_raw;
    (void)right_delta_raw;
    (void)left_delta;
    (void)right_delta;
    (void)total_delta;
    (void)balance;
    (void)balance_filtered;
    (void)touch_engaged;
    (void)baseline_freeze;
    (void)left_now;
    (void)right_now;
    (void)dominant_side;
}
#endif

static void touch_update_baseline(uint32_t *baseline, uint32_t raw)
{
    if (*baseline == 0) {
        *baseline = raw;
        return;
    }
    *baseline = ((*baseline * 31U) + raw) / 32U;
}

esp_err_t touch_slider_init(void)
{
    ESP_ERROR_CHECK(touch_pad_init());
    ESP_ERROR_CHECK(touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER));
    ESP_ERROR_CHECK(touch_pad_config(TOUCH_LEFT_PAD));
    ESP_ERROR_CHECK(touch_pad_config(TOUCH_RIGHT_PAD));
    ESP_ERROR_CHECK(touch_pad_fsm_start());

    vTaskDelay(pdMS_TO_TICKS(300));

    uint64_t left_sum = 0;
    uint64_t right_sum = 0;
    const int samples = 16;
    for (int i = 0; i < samples; ++i) {
        uint32_t lraw = 0;
        uint32_t rraw = 0;
        ESP_ERROR_CHECK(touch_pad_read_raw_data(TOUCH_LEFT_PAD, &lraw));
        ESP_ERROR_CHECK(touch_pad_read_raw_data(TOUCH_RIGHT_PAD, &rraw));
        left_sum += lraw;
        right_sum += rraw;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    s_touch_left_baseline = (uint32_t)(left_sum / samples);
    s_touch_right_baseline = (uint32_t)(right_sum / samples);
    ESP_LOGI(TAG, "Touch baseline left=%lu right=%lu",
             (unsigned long)s_touch_left_baseline,
             (unsigned long)s_touch_right_baseline);
    return ESP_OK;
}

void touch_slider_update(TickType_t now, uint8_t active_layer, touch_consumer_send_fn send_consumer)
{
    uint32_t left_raw = 0;
    uint32_t right_raw = 0;
    if (touch_pad_read_raw_data(TOUCH_LEFT_PAD, &left_raw) != ESP_OK ||
        touch_pad_read_raw_data(TOUCH_RIGHT_PAD, &right_raw) != ESP_OK) {
        return;
    }

    const bool left_now = touch_is_active(left_raw, s_touch_left_baseline, s_touch_left_active);
    const bool right_now = touch_is_active(right_raw, s_touch_right_baseline, s_touch_right_active);
    uint32_t left_log_raw = left_raw;
    uint32_t right_log_raw = right_raw;
    uint32_t left_delta_raw = touch_delta(left_raw, s_touch_left_baseline);
    uint32_t right_delta_raw = touch_delta(right_raw, s_touch_right_baseline);

    const uint32_t raw_max = (left_delta_raw > right_delta_raw) ? left_delta_raw : right_delta_raw;
    if (!s_touch_session_active && raw_max < MACRO_TOUCH_IDLE_NOISE_MAX_DELTA) {
        s_touch_left_idle_noise = ((s_touch_left_idle_noise * 31U) + left_delta_raw) / 32U;
        s_touch_right_idle_noise = ((s_touch_right_idle_noise * 31U) + right_delta_raw) / 32U;
    }

    uint32_t left_delta = touch_apply_noise_comp(left_delta_raw, s_touch_left_idle_noise);
    uint32_t right_delta = touch_apply_noise_comp(right_delta_raw, s_touch_right_idle_noise);
    if (MACRO_TOUCH_SWAP_SIDES) {
        const uint32_t tmp_raw = left_log_raw;
        left_log_raw = right_log_raw;
        right_log_raw = tmp_raw;

        const uint32_t tmp_delta = left_delta;
        left_delta = right_delta;
        right_delta = tmp_delta;
    }

    const uint32_t total_delta = left_delta + right_delta;
    const uint32_t max_delta = (left_delta > right_delta) ? left_delta : right_delta;
    const bool touch_engaged = (total_delta >= MACRO_TOUCH_CONTACT_MIN_TOTAL_DELTA) ||
                               (max_delta >= MACRO_TOUCH_CONTACT_MIN_SIDE_DELTA);
    const bool touch_sensor_active = left_now || right_now;
    if (touch_sensor_active) {
        s_touch_last_sensor_active_tick = now;
    }
    const TickType_t sensor_idle_reset_ticks = pdMS_TO_TICKS(180);
    const TickType_t sensor_activity_ref_tick =
        (s_touch_last_sensor_active_tick != 0) ? s_touch_last_sensor_active_tick : s_touch_start_tick;
    const bool session_sensor_idle_too_long =
        s_touch_session_active &&
        !touch_sensor_active &&
        (sensor_activity_ref_tick != 0) &&
        ((now - sensor_activity_ref_tick) >= sensor_idle_reset_ticks);
    const bool touch_engaged_effective =
        touch_engaged &&
        (touch_sensor_active || (s_touch_session_active && !session_sensor_idle_too_long));
    const int32_t balance = (int32_t)right_delta - (int32_t)left_delta;
    const TickType_t min_swipe_ticks = pdMS_TO_TICKS(MACRO_TOUCH_MIN_SWIPE_MS);
    const TickType_t both_seen_hold_ticks = pdMS_TO_TICKS(MACRO_TOUCH_BOTH_SIDES_HOLD_MS);
    int32_t balance_filtered_for_log = 0;
    touch_side_t dominant_side = touch_dominant_side_from_balance(balance);
    const TickType_t min_interval_ticks = pdMS_TO_TICKS(MACRO_TOUCH_MIN_INTERVAL_MS);
    const macro_touch_layer_config_t *touch_cfg = &g_touch_layer_config[active_layer];

    const bool baseline_freeze = touch_engaged_effective ||
                                 (total_delta >= MACRO_TOUCH_BASELINE_FREEZE_TOTAL_DELTA) ||
                                 (max_delta >= MACRO_TOUCH_BASELINE_FREEZE_SIDE_DELTA) ||
                                 left_now || right_now;
    if (!baseline_freeze) {
        touch_update_baseline(&s_touch_left_baseline, left_raw);
        touch_update_baseline(&s_touch_right_baseline, right_raw);
    }

    if (!touch_engaged_effective) {
        s_touch_session_active = false;
        s_touch_seen_left = false;
        s_touch_seen_right = false;
        s_touch_seen_left_tick = 0;
        s_touch_seen_right_tick = 0;
        s_touch_both_seen_tick = 0;
        s_touch_last_sensor_active_tick = 0;
        s_touch_opposite_dominant_tick = 0;
        s_touch_start_dominant_tick = 0;
        s_touch_start_side = TOUCH_SIDE_NONE;
        s_touch_start_tick = 0;
        s_touch_gesture_fired = false;
        s_touch_balance_filtered = 0;
        s_touch_balance_origin = 0;
        s_touch_hold_active = false;
        s_touch_hold_side = TOUCH_SIDE_NONE;
        s_touch_hold_usage = 0;
    } else {
        const TickType_t seq_min_ticks = pdMS_TO_TICKS(MACRO_TOUCH_SIDE_SEQUENCE_MIN_MS);
        const TickType_t seq_reorder_max_ticks = pdMS_TO_TICKS(MACRO_TOUCH_GESTURE_WINDOW_MS);
        const bool left_seen_now = (left_delta >= MACRO_TOUCH_SWIPE_SIDE_MIN_DELTA) &&
                                   (((uint64_t)left_delta * 100ULL) >=
                                    ((uint64_t)right_delta * (uint64_t)MACRO_TOUCH_SWIPE_SIDE_RELATIVE_PERCENT));
        const bool right_seen_now = (right_delta >= MACRO_TOUCH_SWIPE_SIDE_MIN_DELTA) &&
                                    (((uint64_t)right_delta * 100ULL) >=
                                     ((uint64_t)left_delta * (uint64_t)MACRO_TOUCH_SWIPE_SIDE_RELATIVE_PERCENT));

        if (left_seen_now && !s_touch_seen_left) {
            s_touch_seen_left = true;
            s_touch_seen_left_tick = now;
        }
        if (right_seen_now && !s_touch_seen_right) {
            s_touch_seen_right = true;
            s_touch_seen_right_tick = now;
        }
        if (s_touch_seen_left && s_touch_seen_right && s_touch_both_seen_tick == 0) {
            s_touch_both_seen_tick = now;
        }

        if (!s_touch_session_active) {
            s_touch_session_active = true;
            s_touch_balance_filtered = balance;
            s_touch_balance_origin = balance;
            balance_filtered_for_log = s_touch_balance_filtered;
            s_touch_start_tick = now;
            s_touch_opposite_dominant_tick = 0;
            s_touch_start_dominant_tick = 0;
            if (s_touch_seen_left && !s_touch_seen_right) {
                s_touch_start_side = TOUCH_SIDE_LEFT;
            } else if (s_touch_seen_right && !s_touch_seen_left) {
                s_touch_start_side = TOUCH_SIDE_RIGHT;
            } else if (dominant_side != TOUCH_SIDE_NONE) {
                s_touch_start_side = dominant_side;
            } else {
                s_touch_start_side = TOUCH_SIDE_NONE;
            }
            if (s_touch_start_side != TOUCH_SIDE_NONE) {
                s_touch_start_dominant_tick = now;
            }
            if (s_touch_seen_left && s_touch_seen_right) {
                const TickType_t tick_diff =
                    (s_touch_seen_left_tick > s_touch_seen_right_tick)
                        ? (s_touch_seen_left_tick - s_touch_seen_right_tick)
                        : (s_touch_seen_right_tick - s_touch_seen_left_tick);
                if (tick_diff <= seq_reorder_max_ticks) {
                    if ((s_touch_start_side == TOUCH_SIDE_LEFT) &&
                        (s_touch_seen_right_tick <= s_touch_seen_left_tick)) {
                        s_touch_seen_right_tick = s_touch_seen_left_tick + seq_min_ticks;
                    } else if ((s_touch_start_side == TOUCH_SIDE_RIGHT) &&
                               (s_touch_seen_left_tick <= s_touch_seen_right_tick)) {
                        s_touch_seen_left_tick = s_touch_seen_right_tick + seq_min_ticks;
                    }
                }
            }
            s_touch_gesture_fired = false;
        } else if (!s_touch_gesture_fired) {
            if (s_touch_start_side == TOUCH_SIDE_NONE) {
                if (s_touch_seen_left && !s_touch_seen_right) {
                    s_touch_start_side = TOUCH_SIDE_LEFT;
                } else if (s_touch_seen_right && !s_touch_seen_left) {
                    s_touch_start_side = TOUCH_SIDE_RIGHT;
                } else if (s_touch_seen_left && s_touch_seen_right &&
                           (s_touch_seen_left_tick + seq_min_ticks) <= s_touch_seen_right_tick) {
                    s_touch_start_side = TOUCH_SIDE_LEFT;
                } else if (s_touch_seen_left && s_touch_seen_right &&
                           (s_touch_seen_right_tick + seq_min_ticks) <= s_touch_seen_left_tick) {
                    s_touch_start_side = TOUCH_SIDE_RIGHT;
                }
                if (s_touch_start_side != TOUCH_SIDE_NONE && s_touch_start_dominant_tick == 0) {
                    s_touch_start_dominant_tick = now;
                }
                if (s_touch_seen_left && s_touch_seen_right) {
                    const TickType_t tick_diff =
                        (s_touch_seen_left_tick > s_touch_seen_right_tick)
                            ? (s_touch_seen_left_tick - s_touch_seen_right_tick)
                            : (s_touch_seen_right_tick - s_touch_seen_left_tick);
                    if (tick_diff <= seq_reorder_max_ticks) {
                        if ((s_touch_start_side == TOUCH_SIDE_LEFT) &&
                            (s_touch_seen_right_tick <= s_touch_seen_left_tick)) {
                            s_touch_seen_right_tick = s_touch_seen_left_tick + seq_min_ticks;
                        } else if ((s_touch_start_side == TOUCH_SIDE_RIGHT) &&
                                   (s_touch_seen_left_tick <= s_touch_seen_right_tick)) {
                            s_touch_seen_left_tick = s_touch_seen_right_tick + seq_min_ticks;
                        }
                    }
                }
            }

            s_touch_balance_filtered = ((s_touch_balance_filtered * 3) + balance) / 4;
            balance_filtered_for_log = s_touch_balance_filtered;
            const touch_side_t dominant_filtered = touch_dominant_side_from_balance(s_touch_balance_filtered);
            dominant_side = dominant_filtered;
            const bool sequence_l2r = s_touch_seen_left && s_touch_seen_right &&
                                      (s_touch_seen_right_tick > s_touch_seen_left_tick) &&
                                      ((s_touch_seen_right_tick - s_touch_seen_left_tick) >= seq_min_ticks);
            const bool sequence_r2l = s_touch_seen_left && s_touch_seen_right &&
                                      (s_touch_seen_left_tick > s_touch_seen_right_tick) &&
                                      ((s_touch_seen_left_tick - s_touch_seen_right_tick) >= seq_min_ticks);

            if (s_touch_start_side != TOUCH_SIDE_NONE && s_touch_start_dominant_tick == 0) {
                const bool start_is_dominant =
                    ((s_touch_start_side == TOUCH_SIDE_LEFT) && (dominant_filtered == TOUCH_SIDE_LEFT)) ||
                    ((s_touch_start_side == TOUCH_SIDE_RIGHT) && (dominant_filtered == TOUCH_SIDE_RIGHT));
                if (start_is_dominant) {
                    s_touch_start_dominant_tick = now;
                }
            }

            const bool opposite_dominant =
                ((s_touch_start_side == TOUCH_SIDE_LEFT) && (dominant_filtered == TOUCH_SIDE_RIGHT)) ||
                ((s_touch_start_side == TOUCH_SIDE_RIGHT) && (dominant_filtered == TOUCH_SIDE_LEFT));
            if (opposite_dominant) {
                if (s_touch_opposite_dominant_tick == 0) {
                    s_touch_opposite_dominant_tick = now;
                }
            } else {
                s_touch_opposite_dominant_tick = 0;
            }

            const TickType_t start_dom_min_ticks = pdMS_TO_TICKS(MACRO_TOUCH_START_DOMINANT_MIN_MS);
            bool start_side_stable =
                (s_touch_start_side == TOUCH_SIDE_NONE) ||
                ((s_touch_start_dominant_tick != 0) &&
                 ((now - s_touch_start_dominant_tick) >= start_dom_min_ticks));

            if (!start_side_stable &&
                (s_touch_start_side != TOUCH_SIDE_NONE) &&
                (s_touch_opposite_dominant_tick != 0) &&
                ((now - s_touch_opposite_dominant_tick) >= start_dom_min_ticks) &&
                ((dominant_filtered == TOUCH_SIDE_LEFT) || (dominant_filtered == TOUCH_SIDE_RIGHT))) {
                s_touch_start_side = dominant_filtered;
                s_touch_start_dominant_tick = now;
                s_touch_opposite_dominant_tick = 0;

                if (s_touch_seen_left && s_touch_seen_right) {
                    const TickType_t tick_diff =
                        (s_touch_seen_left_tick > s_touch_seen_right_tick)
                            ? (s_touch_seen_left_tick - s_touch_seen_right_tick)
                            : (s_touch_seen_right_tick - s_touch_seen_left_tick);
                    if (tick_diff <= seq_reorder_max_ticks) {
                        if ((s_touch_start_side == TOUCH_SIDE_LEFT) &&
                            (s_touch_seen_right_tick <= s_touch_seen_left_tick)) {
                            s_touch_seen_right_tick = s_touch_seen_left_tick + seq_min_ticks;
                        } else if ((s_touch_start_side == TOUCH_SIDE_RIGHT) &&
                                   (s_touch_seen_left_tick <= s_touch_seen_right_tick)) {
                            s_touch_seen_left_tick = s_touch_seen_right_tick + seq_min_ticks;
                        }
                    }
                }

                start_side_stable = false;
            }

            const int32_t filtered_travel = s_touch_balance_filtered - s_touch_balance_origin;
            const bool travel_l2r = filtered_travel >= (int32_t)MACRO_TOUCH_GESTURE_TRAVEL_DELTA;
            const bool travel_r2l = filtered_travel <= -(int32_t)MACRO_TOUCH_GESTURE_TRAVEL_DELTA;
            const bool opposite_hold_ready =
                (s_touch_opposite_dominant_tick != 0) &&
                ((now - s_touch_opposite_dominant_tick) >= seq_min_ticks);

            const bool crossed_l2r =
                (((sequence_l2r) ||
                  ((s_touch_start_side == TOUCH_SIDE_LEFT) && opposite_hold_ready && travel_l2r)) &&
                 (dominant_filtered == TOUCH_SIDE_RIGHT) && start_side_stable &&
                 ((s_touch_start_side == TOUCH_SIDE_LEFT) || (s_touch_start_side == TOUCH_SIDE_NONE)));

            const bool crossed_r2l =
                (((sequence_r2l) ||
                  ((s_touch_start_side == TOUCH_SIDE_RIGHT) && opposite_hold_ready && travel_r2l)) &&
                 (dominant_filtered == TOUCH_SIDE_LEFT) && start_side_stable &&
                 ((s_touch_start_side == TOUCH_SIDE_RIGHT) || (s_touch_start_side == TOUCH_SIDE_NONE)));
            const bool crossed = crossed_l2r || crossed_r2l;
            const bool both_sides_ready = s_touch_seen_left && s_touch_seen_right;
            const bool can_fire = (!MACRO_TOUCH_REQUIRE_BOTH_SIDES) || both_sides_ready;
            const bool both_sides_hold_ready =
                (!MACRO_TOUCH_REQUIRE_BOTH_SIDES) ||
                ((s_touch_both_seen_tick != 0) &&
                 ((now - s_touch_both_seen_tick) >= both_seen_hold_ticks));
            const bool long_enough = (now - s_touch_start_tick) >= min_swipe_ticks;

            if (can_fire && both_sides_hold_ready && long_enough && crossed &&
                (now - s_touch_last_gesture_tick) > min_interval_ticks) {
                uint16_t usage = 0;
                const char *gesture = "";
                bool hold_repeat = false;
                touch_side_t hold_side = TOUCH_SIDE_NONE;
                if (crossed_l2r) {
                    s_touch_start_side = TOUCH_SIDE_LEFT;
                    usage = touch_cfg->right_usage;
                    gesture = "L->R";
                    hold_repeat = touch_cfg->right_hold_repeat;
                    hold_side = TOUCH_SIDE_RIGHT;
                } else if (crossed_r2l) {
                    s_touch_start_side = TOUCH_SIDE_RIGHT;
                    usage = touch_cfg->left_usage;
                    gesture = "R->L";
                    hold_repeat = touch_cfg->left_hold_repeat;
                    hold_side = TOUCH_SIDE_LEFT;
                }

                if (usage != 0) {
                    ESP_LOGI(TAG, "Touch slide %s (L%u) rawL=%lu rawR=%lu dL=%lu dR=%lu usage=0x%X",
                             gesture,
                             (unsigned)active_layer + 1,
                             (unsigned long)left_log_raw,
                             (unsigned long)right_log_raw,
                             (unsigned long)left_delta,
                             (unsigned long)right_delta,
                             usage);
                    if (send_consumer != NULL) {
                        send_consumer(usage);
                    }

                    if (hold_repeat && touch_cfg->hold_repeat_ms > 0) {
                        s_touch_hold_active = true;
                        s_touch_hold_side = hold_side;
                        s_touch_hold_usage = usage;
                        s_touch_hold_next_tick = now + pdMS_TO_TICKS(touch_cfg->hold_start_ms);
                    } else {
                        s_touch_hold_active = false;
                        s_touch_hold_side = TOUCH_SIDE_NONE;
                        s_touch_hold_usage = 0;
                    }
                    s_touch_last_gesture_tick = now;
                    s_touch_gesture_fired = true;
                }
            }
        } else {
            balance_filtered_for_log = s_touch_balance_filtered;
            dominant_side = touch_dominant_side_from_balance(s_touch_balance_filtered);
        }
    }

    if (s_touch_hold_active) {
        const bool hold_side_active = touch_engaged_effective && (dominant_side == s_touch_hold_side);
        if (!hold_side_active) {
            s_touch_hold_active = false;
            s_touch_hold_side = TOUCH_SIDE_NONE;
            s_touch_hold_usage = 0;
        } else if (now >= s_touch_hold_next_tick) {
            if (s_touch_hold_usage != 0) {
                ESP_LOGI(TAG, "Touch hold repeat (L%u) usage=0x%X",
                         (unsigned)active_layer + 1,
                         s_touch_hold_usage);
                if (send_consumer != NULL) {
                    send_consumer(s_touch_hold_usage);
                }
            }
            s_touch_hold_next_tick = now + pdMS_TO_TICKS(touch_cfg->hold_repeat_ms);
        }
    }

    touch_log_debug(now,
                    left_log_raw,
                    right_log_raw,
                    left_delta_raw,
                    right_delta_raw,
                    left_delta,
                    right_delta,
                    total_delta,
                    balance,
                    balance_filtered_for_log,
                    touch_engaged_effective,
                    baseline_freeze,
                    left_now,
                    right_now,
                    dominant_side);

    s_touch_left_active = left_now;
    s_touch_right_active = right_now;
}
