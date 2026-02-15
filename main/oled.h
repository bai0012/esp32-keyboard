#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"

#define OLED_WIDTH 128
#define OLED_HEIGHT 64

typedef struct {
    uint8_t width;
    uint8_t height;
    int8_t x_offset;
    int8_t y_offset;
    uint8_t advance_x;
    const uint8_t *bitmap;
    bool bit_packed;
} oled_glyph_t;

typedef bool (*oled_font_get_glyph_fn)(void *ctx, uint32_t codepoint, oled_glyph_t *out_glyph);

typedef struct {
    oled_font_get_glyph_fn get_glyph;
    void *ctx;
    uint8_t line_height;
} oled_font_t;

typedef struct {
    const uint8_t *bitmap;
    uint16_t duration_ms;
} oled_animation_frame_t;

typedef struct {
    uint8_t width;
    uint8_t height;
    bool bit_packed;
    uint16_t frame_count;
    const oled_animation_frame_t *frames;
} oled_animation_t;

esp_err_t oled_init(void);
esp_err_t oled_set_brightness_percent(uint8_t percent);
esp_err_t oled_set_display_enabled(bool enabled);
esp_err_t oled_set_inverted(bool inverted);

void oled_clear_buffer(void);
void oled_set_pixel(int x, int y, bool on);
void oled_fill_rect(int x, int y, int w, int h, bool on);
void oled_draw_bitmap_mono(int x, int y, int w, int h, const uint8_t *bitmap, bool bit_packed);
esp_err_t oled_draw_text_utf8(int x, int y, const char *utf8, const oled_font_t *font);
esp_err_t oled_present(void);

esp_err_t oled_render_animation_frame_centered(const oled_animation_t *anim,
                                               uint16_t frame_index,
                                               int8_t shift_x,
                                               int8_t shift_y);

// Current app-level convenience renderer (clock + sync marker).
esp_err_t oled_render_clock(const struct tm *timeinfo, int8_t shift_x, int8_t shift_y);
