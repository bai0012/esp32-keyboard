#include "buzzer.h"

#include <ctype.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>

#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_log.h"

#include "keymap_config.h"

#define TAG "BUZZER"

#define BUZZER_SPEED_MODE LEDC_LOW_SPEED_MODE
#define BUZZER_TIMER LEDC_TIMER_1
#define BUZZER_CHANNEL LEDC_CHANNEL_1
#define BUZZER_DUTY_RES LEDC_TIMER_10_BIT
#define BUZZER_DUTY_MAX ((1U << BUZZER_DUTY_RES) - 1U)
#define BUZZER_INIT_FREQ_HZ 2000U

#if (MACRO_BUZZER_QUEUE_SIZE < 1)
#error "MACRO_BUZZER_QUEUE_SIZE must be >= 1"
#endif

typedef struct {
    uint16_t frequency_hz;
    uint16_t duration_ms;
    uint16_t silence_ms;
} buzzer_tone_t;

typedef struct {
    uint16_t default_duration;
    uint8_t default_octave;
    uint16_t bpm;
    const char *notes;
} rtttl_cfg_t;

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

static inline const char *skip_spaces(const char *s)
{
    while (*s != '\0' && isspace((unsigned char)*s)) {
        ++s;
    }
    return s;
}

static bool parse_u16(const char **p, uint16_t *out)
{
    const char *s = *p;
    if (!isdigit((unsigned char)*s)) {
        return false;
    }

    uint32_t value = 0;
    while (isdigit((unsigned char)*s)) {
        value = (value * 10U) + (uint32_t)(*s - '0');
        if (value > 65535U) {
            value = 65535U;
        }
        ++s;
    }
    *out = (uint16_t)value;
    *p = s;
    return true;
}

static esp_err_t rtttl_parse_header(const char *rtttl, rtttl_cfg_t *cfg)
{
    const char *first_colon = strchr(rtttl, ':');
    if (first_colon == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const char *second_colon = strchr(first_colon + 1, ':');
    if (second_colon == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    cfg->default_duration = 4;
    cfg->default_octave = 6;
    cfg->bpm = 140;
    cfg->notes = second_colon + 1;

    const char *s = first_colon + 1;
    while (s < second_colon) {
        s = skip_spaces(s);
        if (s >= second_colon) {
            break;
        }
        if (*s == ',') {
            ++s;
            continue;
        }

        const char key = (char)tolower((unsigned char)*s);
        ++s;
        s = skip_spaces(s);
        if (*s != '=') {
            while (s < second_colon && *s != ',') {
                ++s;
            }
            continue;
        }
        ++s;
        s = skip_spaces(s);

        uint16_t value = 0;
        if (!parse_u16(&s, &value)) {
            return ESP_ERR_INVALID_ARG;
        }

        if (key == 'd' && value > 0) {
            cfg->default_duration = value;
        } else if (key == 'o' && value <= 9U) {
            cfg->default_octave = (uint8_t)value;
        } else if (key == 'b' && value > 0) {
            cfg->bpm = value;
        }

        while (s < second_colon && *s != ',') {
            ++s;
        }
        if (*s == ',') {
            ++s;
        }
    }

    if (cfg->bpm == 0 || cfg->default_duration == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static uint16_t rtttl_note_to_freq(char note, bool sharp, uint8_t octave)
{
    static const uint16_t base_oct4[12] = {
        262, 277, 294, 311, 330, 349, 370, 392, 415, 440, 466, 494,
    };

    uint8_t semitone = 0;
    switch ((char)tolower((unsigned char)note)) {
    case 'c':
        semitone = 0;
        break;
    case 'd':
        semitone = 2;
        break;
    case 'e':
        semitone = 4;
        break;
    case 'f':
        semitone = 5;
        break;
    case 'g':
        semitone = 7;
        break;
    case 'a':
        semitone = 9;
        break;
    case 'b':
        semitone = 11;
        break;
    default:
        return 0;
    }

    if (sharp && semitone < 11U) {
        semitone++;
    }

    uint32_t freq = base_oct4[semitone];
    if (octave > 4U) {
        for (uint8_t i = 0; i < (uint8_t)(octave - 4U); ++i) {
            freq <<= 1U;
        }
    } else if (octave < 4U) {
        for (uint8_t i = 0; i < (uint8_t)(4U - octave); ++i) {
            freq >>= 1U;
        }
    }

    if (freq > 20000U) {
        freq = 20000U;
    } else if (freq == 0U) {
        freq = 1U;
    }
    return (uint16_t)freq;
}

static uint16_t rtttl_duration_ms(const rtttl_cfg_t *cfg, uint16_t duration, uint8_t dots)
{
    uint32_t whole_ms = (240000UL / cfg->bpm);
    uint32_t note_ms = whole_ms / duration;
    uint32_t ext = note_ms / 2U;
    while (dots-- > 0U && ext > 0U) {
        note_ms += ext;
        ext /= 2U;
    }
    if (note_ms == 0U) {
        note_ms = 1U;
    } else if (note_ms > 65535U) {
        note_ms = 65535U;
    }
    return (uint16_t)note_ms;
}

static esp_err_t queue_silence(uint16_t duration_ms)
{
    const buzzer_tone_t tone = {
        .frequency_hz = 0,
        .duration_ms = duration_ms,
        .silence_ms = 0,
    };
    return queue_push(&tone);
}

static esp_err_t buzzer_set_frequency(uint16_t frequency_hz)
{
    const uint32_t actual = ledc_set_freq(BUZZER_SPEED_MODE, BUZZER_TIMER, frequency_hz);
    if (actual != 0) {
        return ESP_OK;
    }

    // Recover from runtime timer state mismatch by reconfiguring this timer.
    const ledc_timer_config_t timer_cfg = {
        .speed_mode = BUZZER_SPEED_MODE,
        .duty_resolution = BUZZER_DUTY_RES,
        .timer_num = BUZZER_TIMER,
        .freq_hz = frequency_hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    return ledc_timer_config(&timer_cfg);
}

static esp_err_t buzzer_output_enable(uint16_t frequency_hz)
{
    if (buzzer_set_frequency(frequency_hz) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
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
    if (s_current_tone.frequency_hz == 0U) {
        s_tone_active = false;
        s_silence_active = true;
        s_phase_deadline = now + ms_to_ticks_nonzero(s_current_tone.duration_ms);
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
        .freq_hz = BUZZER_INIT_FREQ_HZ,
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

esp_err_t buzzer_play_rtttl(const char *rtttl)
{
    if (!MACRO_BUZZER_ENABLED || !s_initialized) {
        return ESP_OK;
    }
    if (rtttl == NULL || rtttl[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    rtttl_cfg_t cfg = {0};
    ESP_RETURN_ON_ERROR(rtttl_parse_header(rtttl, &cfg), TAG, "invalid RTTTL header");

    const char *s = cfg.notes;
    while (*s != '\0') {
        s = skip_spaces(s);
        if (*s == ',') {
            ++s;
            continue;
        }
        if (*s == '\0') {
            break;
        }

        uint16_t duration = cfg.default_duration;
        uint16_t parsed_num = 0;
        if (parse_u16(&s, &parsed_num) && parsed_num > 0) {
            duration = parsed_num;
        }
        if (*s == '\0') {
            break;
        }

        char note = (char)tolower((unsigned char)*s);
        if (strchr("abcdefgp", note) == NULL) {
            while (*s != '\0' && *s != ',') {
                ++s;
            }
            continue;
        }
        ++s;

        bool sharp = false;
        if (*s == '#') {
            sharp = true;
            ++s;
        }

        uint8_t dots = 0;
        while (*s == '.') {
            dots++;
            ++s;
        }

        uint8_t octave = cfg.default_octave;
        parsed_num = 0;
        if (parse_u16(&s, &parsed_num) && parsed_num <= 9U) {
            octave = (uint8_t)parsed_num;
        }

        while (*s == '.') {
            dots++;
            ++s;
        }

        const uint16_t note_ms = rtttl_duration_ms(&cfg, duration, dots);
        if (note == 'p') {
            ESP_RETURN_ON_ERROR(queue_silence(note_ms), TAG, "buzzer queue full");
        } else {
            const uint16_t freq = rtttl_note_to_freq(note, sharp, octave);
            if (freq == 0U) {
                return ESP_ERR_INVALID_ARG;
            }

            uint16_t tone_ms = note_ms;
            uint16_t gap_ms = 0;
            if (MACRO_BUZZER_RTTTL_NOTE_GAP_MS > 0U && note_ms > (MACRO_BUZZER_RTTTL_NOTE_GAP_MS + 1U)) {
                gap_ms = MACRO_BUZZER_RTTTL_NOTE_GAP_MS;
                tone_ms = (uint16_t)(note_ms - gap_ms);
            }
            ESP_RETURN_ON_ERROR(buzzer_play_tone_ex(freq, tone_ms, gap_ms), TAG, "buzzer queue full");
        }

        while (*s != '\0' && *s != ',') {
            ++s;
        }
        if (*s == ',') {
            ++s;
        }
    }
    return ESP_OK;
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
    esp_err_t err = buzzer_play_rtttl(MACRO_BUZZER_RTTTL_STARTUP);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "startup RTTTL invalid: %s", esp_err_to_name(err));
    }
}

void buzzer_play_keypress(void)
{
    if (!MACRO_BUZZER_KEYPRESS_ENABLED) {
        return;
    }
    esp_err_t err = buzzer_play_rtttl(MACRO_BUZZER_RTTTL_KEYPRESS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "keypress RTTTL invalid: %s", esp_err_to_name(err));
    }
}

void buzzer_play_layer_switch(uint8_t layer_index)
{
    if (!MACRO_BUZZER_LAYER_SWITCH_ENABLED) {
        return;
    }
    const char *rtttl = MACRO_BUZZER_RTTTL_LAYER1;
    if (layer_index == 1U) {
        rtttl = MACRO_BUZZER_RTTTL_LAYER2;
    } else if (layer_index >= 2U) {
        rtttl = MACRO_BUZZER_RTTTL_LAYER3;
    }
    esp_err_t err = buzzer_play_rtttl(rtttl);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "layer RTTTL invalid: layer=%u err=%s", (unsigned)layer_index + 1U, esp_err_to_name(err));
    }
}

void buzzer_play_encoder_step(int8_t direction)
{
    if (!MACRO_BUZZER_ENCODER_STEP_ENABLED) {
        return;
    }
    const char *rtttl = (direction >= 0) ? MACRO_BUZZER_RTTTL_ENCODER_CW : MACRO_BUZZER_RTTTL_ENCODER_CCW;
    esp_err_t err = buzzer_play_rtttl(rtttl);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "encoder RTTTL invalid: %s", esp_err_to_name(err));
    }
}
