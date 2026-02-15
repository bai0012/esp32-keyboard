#include "buzzer.h"

#include <stdbool.h>

#include "driver/ledc.h"
#include "esp_log.h"

#include "keymap_config.h"

#define TAG "BUZZER"

#define BUZZER_SPEED_MODE LEDC_LOW_SPEED_MODE
#define BUZZER_TIMER LEDC_TIMER_1
#define BUZZER_CHANNEL LEDC_CHANNEL_1
#define BUZZER_DUTY_RES LEDC_TIMER_10_BIT
#define BUZZER_DUTY_MAX ((1U << BUZZER_DUTY_RES) - 1U)

#if (MACRO_BUZZER_QUEUE_SIZE < 1)
#error "MACRO_BUZZER_QUEUE_SIZE must be >= 1"
#endif

typedef struct {
    uint16_t frequency_hz;
    uint16_t duration_ms;
    uint16_t silence_ms;
} buzzer_tone_t;

static buzzer_tone_t s_queue[MACRO_BUZZER_QUEUE_SIZE];
static uint8_t s_queue_head = 0;
static uint8_t s_queue_tail = 0;
static uint8_t s_queue_count = 0;
static buzzer_tone_t s_current_tone = {0};
static TickType_t s_phase_deadline = 0;
static bool s_initialized = false;
static bool s_tone_active = false;
static bool s_silence_active = false;

static inline bool tick_reached(TickType_t now, TickType_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static TickType_t ms_to_ticks_nonzero(uint16_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    if (ticks == 0 && ms > 0) {
        ticks = 1;
    }
    return ticks;
}

static uint32_t duty_from_percent(uint8_t duty_percent)
{
    if (duty_percent >= 100) {
        return BUZZER_DUTY_MAX;
    }
    return ((uint32_t)duty_percent * BUZZER_DUTY_MAX) / 100U;
}

static void queue_clear(void)
{
    s_queue_head = 0;
    s_queue_tail = 0;
    s_queue_count = 0;
}

static esp_err_t queue_push(const buzzer_tone_t *tone)
{
    if (s_queue_count >= MACRO_BUZZER_QUEUE_SIZE) {
        return ESP_ERR_NO_MEM;
    }
    s_queue[s_queue_tail] = *tone;
    s_queue_tail = (uint8_t)((s_queue_tail + 1U) % MACRO_BUZZER_QUEUE_SIZE);
    s_queue_count++;
    return ESP_OK;
}

static bool queue_pop(buzzer_tone_t *tone_out)
{
    if (s_queue_count == 0) {
        return false;
    }
    *tone_out = s_queue[s_queue_head];
    s_queue_head = (uint8_t)((s_queue_head + 1U) % MACRO_BUZZER_QUEUE_SIZE);
    s_queue_count--;
    return true;
}

static esp_err_t buzzer_output_enable(uint16_t frequency_hz)
{
    if (ledc_set_freq(BUZZER_SPEED_MODE, BUZZER_TIMER, frequency_hz) == 0) {
        return ESP_FAIL;
    }
    ESP_ERROR_CHECK_WITHOUT_ABORT(ledc_set_duty(BUZZER_SPEED_MODE,
                                                BUZZER_CHANNEL,
                                                duty_from_percent(MACRO_BUZZER_DUTY_PERCENT)));
    ESP_ERROR_CHECK_WITHOUT_ABORT(ledc_update_duty(BUZZER_SPEED_MODE, BUZZER_CHANNEL));
    return ESP_OK;
}

static void buzzer_output_disable(void)
{
    ESP_ERROR_CHECK_WITHOUT_ABORT(ledc_set_duty(BUZZER_SPEED_MODE, BUZZER_CHANNEL, 0));
    ESP_ERROR_CHECK_WITHOUT_ABORT(ledc_update_duty(BUZZER_SPEED_MODE, BUZZER_CHANNEL));
}

static void buzzer_start_next_tone(TickType_t now)
{
    if (!queue_pop(&s_current_tone)) {
        return;
    }
    if (buzzer_output_enable(s_current_tone.frequency_hz) != ESP_OK) {
        ESP_LOGW(TAG, "failed to start tone freq=%u", (unsigned)s_current_tone.frequency_hz);
        s_current_tone.frequency_hz = 0;
        return;
    }
    s_tone_active = true;
    s_silence_active = false;
    s_phase_deadline = now + ms_to_ticks_nonzero(s_current_tone.duration_ms);
}

esp_err_t buzzer_init(void)
{
    if (!MACRO_BUZZER_ENABLED) {
        return ESP_OK;
    }
    if (s_initialized) {
        return ESP_OK;
    }

    const ledc_timer_config_t timer_cfg = {
        .speed_mode = BUZZER_SPEED_MODE,
        .duty_resolution = BUZZER_DUTY_RES,
        .timer_num = BUZZER_TIMER,
        .freq_hz = MACRO_BUZZER_KEYPRESS_FREQ_HZ,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    const ledc_channel_config_t channel_cfg = {
        .gpio_num = MACRO_BUZZER_GPIO,
        .speed_mode = BUZZER_SPEED_MODE,
        .channel = BUZZER_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BUZZER_TIMER,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel_cfg));
    buzzer_output_disable();

    queue_clear();
    s_tone_active = false;
    s_silence_active = false;
    s_initialized = true;
    ESP_LOGI(TAG, "ready gpio=%d duty=%u%%", (int)MACRO_BUZZER_GPIO, (unsigned)MACRO_BUZZER_DUTY_PERCENT);
    return ESP_OK;
}

void buzzer_stop(void)
{
    if (!MACRO_BUZZER_ENABLED || !s_initialized) {
        return;
    }
    queue_clear();
    s_tone_active = false;
    s_silence_active = false;
    s_current_tone = (buzzer_tone_t){0};
    buzzer_output_disable();
}

esp_err_t buzzer_play_tone_ex(uint16_t frequency_hz, uint16_t duration_ms, uint16_t silence_ms)
{
    if (!MACRO_BUZZER_ENABLED || !s_initialized) {
        return ESP_OK;
    }
    if (frequency_hz == 0 || duration_ms == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    const buzzer_tone_t tone = {
        .frequency_hz = frequency_hz,
        .duration_ms = duration_ms,
        .silence_ms = silence_ms,
    };
    return queue_push(&tone);
}

esp_err_t buzzer_play_tone(uint16_t frequency_hz, uint16_t duration_ms)
{
    return buzzer_play_tone_ex(frequency_hz, duration_ms, 0);
}

void buzzer_update(TickType_t now)
{
    if (!MACRO_BUZZER_ENABLED || !s_initialized) {
        return;
    }

    if (!s_tone_active && !s_silence_active) {
        buzzer_start_next_tone(now);
        return;
    }

    if (s_tone_active && tick_reached(now, s_phase_deadline)) {
        buzzer_output_disable();
        s_tone_active = false;

        if (s_current_tone.silence_ms > 0) {
            s_silence_active = true;
            s_phase_deadline = now + ms_to_ticks_nonzero(s_current_tone.silence_ms);
            return;
        }

        buzzer_start_next_tone(now);
        return;
    }

    if (s_silence_active && tick_reached(now, s_phase_deadline)) {
        s_silence_active = false;
        buzzer_start_next_tone(now);
    }
}

void buzzer_play_startup(void)
{
    if (!MACRO_BUZZER_STARTUP_ENABLED) {
        return;
    }
    (void)buzzer_play_tone_ex(MACRO_BUZZER_STARTUP_FREQ1_HZ,
                              MACRO_BUZZER_STARTUP_TONE_MS,
                              MACRO_BUZZER_STARTUP_GAP_MS);
    (void)buzzer_play_tone(MACRO_BUZZER_STARTUP_FREQ2_HZ, MACRO_BUZZER_STARTUP_TONE_MS);
}

void buzzer_play_keypress(void)
{
    if (!MACRO_BUZZER_KEYPRESS_ENABLED) {
        return;
    }
    (void)buzzer_play_tone(MACRO_BUZZER_KEYPRESS_FREQ_HZ, MACRO_BUZZER_KEYPRESS_MS);
}

void buzzer_play_layer_switch(uint8_t layer_index)
{
    if (!MACRO_BUZZER_LAYER_SWITCH_ENABLED) {
        return;
    }
    const uint16_t freq =
        (uint16_t)(MACRO_BUZZER_LAYER_BASE_FREQ_HZ + ((uint16_t)layer_index * MACRO_BUZZER_LAYER_STEP_HZ));
    (void)buzzer_play_tone(freq, MACRO_BUZZER_LAYER_MS);
}

void buzzer_play_encoder_step(int8_t direction)
{
    if (!MACRO_BUZZER_ENCODER_STEP_ENABLED) {
        return;
    }
    const uint16_t freq = (direction >= 0) ? MACRO_BUZZER_ENCODER_CW_FREQ_HZ : MACRO_BUZZER_ENCODER_CCW_FREQ_HZ;
    (void)buzzer_play_tone(freq, MACRO_BUZZER_ENCODER_MS);
}
