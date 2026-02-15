#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "driver/i2c_master.h"

#include "esp_check.h"

#include "keymap_config.h"
#include "oled.h"

#define TAG "MACROPAD"

#define OLED_SDA_GPIO GPIO_NUM_15
#define OLED_SCL_GPIO GPIO_NUM_16
#define OLED_I2C_PORT 0
#define OLED_I2C_ADDR 0x3C

typedef struct {
    i2c_master_bus_handle_t bus;
    i2c_master_dev_handle_t dev;
    uint8_t fb[OLED_WIDTH * OLED_HEIGHT / 8];
    bool display_enabled;
    bool inverted;
    uint8_t brightness_percent;
} oled_state_t;

static oled_state_t s_oled = {
    .bus = NULL,
    .dev = NULL,
    .display_enabled = true,
    .inverted = false,
    .brightness_percent = 100,
};

static esp_err_t oled_send_cmd(uint8_t cmd)
{
    uint8_t payload[2] = {0x00, cmd};
    return i2c_master_transmit(s_oled.dev, payload, sizeof(payload), -1);
}

static inline uint8_t oled_percent_to_contrast(uint8_t percent)
{
    if (percent >= 100U) {
        return 0xFF;
    }
    return (uint8_t)(((uint16_t)percent * 255U) / 100U);
}

static esp_err_t oled_send_page(uint8_t page, const uint8_t *data)
{
    uint8_t payload[1 + OLED_WIDTH];
    payload[0] = 0x40;
    memcpy(&payload[1], data, OLED_WIDTH);

    ESP_RETURN_ON_ERROR(oled_send_cmd((uint8_t)(0xB0 + page)), TAG, "set page failed");
    ESP_RETURN_ON_ERROR(oled_send_cmd(0x00), TAG, "set low column failed");
    ESP_RETURN_ON_ERROR(oled_send_cmd(0x10), TAG, "set high column failed");
    return i2c_master_transmit(s_oled.dev, payload, sizeof(payload), -1);
}

void oled_clear_buffer(void)
{
    memset(s_oled.fb, 0, sizeof(s_oled.fb));
}

void oled_set_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) {
        return;
    }

    const size_t index = (size_t)x + ((size_t)y / 8U) * OLED_WIDTH;
    const uint8_t mask = (uint8_t)(1U << (y & 0x7));
    if (on) {
        s_oled.fb[index] |= mask;
    } else {
        s_oled.fb[index] &= (uint8_t)~mask;
    }
}

void oled_fill_rect(int x, int y, int w, int h, bool on)
{
    if (w <= 0 || h <= 0) {
        return;
    }

    int x2 = x + w;
    int y2 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x2 > OLED_WIDTH) x2 = OLED_WIDTH;
    if (y2 > OLED_HEIGHT) y2 = OLED_HEIGHT;

    for (int yy = y; yy < y2; ++yy) {
        for (int xx = x; xx < x2; ++xx) {
            oled_set_pixel(xx, yy, on);
        }
    }
}

void oled_draw_bitmap_mono(int x, int y, int w, int h, const uint8_t *bitmap, bool bit_packed)
{
    if (bitmap == NULL || w <= 0 || h <= 0) {
        return;
    }

    if (!bit_packed) {
        for (int yy = 0; yy < h; ++yy) {
            for (int xx = 0; xx < w; ++xx) {
                const size_t idx = (size_t)(yy * w + xx);
                oled_set_pixel(x + xx, y + yy, bitmap[idx] != 0);
            }
        }
        return;
    }

    const int row_bytes = (w + 7) / 8;
    for (int yy = 0; yy < h; ++yy) {
        for (int xx = 0; xx < w; ++xx) {
            const uint8_t byte = bitmap[(yy * row_bytes) + (xx / 8)];
            const bool on = (byte & (uint8_t)(0x80U >> (xx & 0x7))) != 0;
            oled_set_pixel(x + xx, y + yy, on);
        }
    }
}

static bool utf8_next_codepoint(const char **s, uint32_t *codepoint)
{
    const uint8_t *p = (const uint8_t *)*s;
    if (*p == 0) {
        return false;
    }

    if (*p < 0x80U) {
        *codepoint = *p;
        *s += 1;
        return true;
    }

    if ((*p & 0xE0U) == 0xC0U && p[1] != 0) {
        *codepoint = ((uint32_t)(p[0] & 0x1FU) << 6) | (uint32_t)(p[1] & 0x3FU);
        *s += 2;
        return true;
    }

    if ((*p & 0xF0U) == 0xE0U && p[1] != 0 && p[2] != 0) {
        *codepoint = ((uint32_t)(p[0] & 0x0FU) << 12) |
                     ((uint32_t)(p[1] & 0x3FU) << 6) |
                     (uint32_t)(p[2] & 0x3FU);
        *s += 3;
        return true;
    }

    if ((*p & 0xF8U) == 0xF0U && p[1] != 0 && p[2] != 0 && p[3] != 0) {
        *codepoint = ((uint32_t)(p[0] & 0x07U) << 18) |
                     ((uint32_t)(p[1] & 0x3FU) << 12) |
                     ((uint32_t)(p[2] & 0x3FU) << 6) |
                     (uint32_t)(p[3] & 0x3FU);
        *s += 4;
        return true;
    }

    // Invalid UTF-8 sequence: skip one byte and map to replacement marker.
    *codepoint = 0xFFFD;
    *s += 1;
    return true;
}

static void oled_draw_missing_glyph(int x, int y, int advance_x, uint8_t line_height)
{
    const int w = (advance_x > 4) ? (advance_x - 1) : 6;
    const int h = (line_height > 4U) ? (int)line_height - 1 : 8;
    oled_fill_rect(x, y, w, 1, true);
    oled_fill_rect(x, y + h - 1, w, 1, true);
    oled_fill_rect(x, y, 1, h, true);
    oled_fill_rect(x + w - 1, y, 1, h, true);
}

esp_err_t oled_draw_text_utf8(int x, int y, const char *utf8, const oled_font_t *font)
{
    if (utf8 == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint8_t default_advance = 8;
    const uint8_t default_line_h = (font != NULL && font->line_height > 0U) ? font->line_height : 12U;

    const char *p = utf8;
    while (*p != '\0') {
        uint32_t cp = 0;
        if (!utf8_next_codepoint(&p, &cp)) {
            break;
        }

        if (cp == '\n') {
            x = 0;
            y += default_line_h;
            continue;
        }
        if (cp == ' ') {
            x += default_advance;
            continue;
        }

        oled_glyph_t glyph = {0};
        bool have_glyph = false;
        if (font != NULL && font->get_glyph != NULL) {
            have_glyph = font->get_glyph(font->ctx, cp, &glyph);
        }

        if (have_glyph && glyph.bitmap != NULL && glyph.width > 0U && glyph.height > 0U) {
            oled_draw_bitmap_mono(x + glyph.x_offset,
                                  y + glyph.y_offset,
                                  glyph.width,
                                  glyph.height,
                                  glyph.bitmap,
                                  glyph.bit_packed);
            x += (glyph.advance_x > 0U) ? glyph.advance_x : default_advance;
        } else {
            // Font is pluggable. This fallback keeps layout stable for ASCII/CJK until glyph packs are added.
            oled_draw_missing_glyph(x, y, default_advance, default_line_h);
            x += default_advance;
        }
    }

    return ESP_OK;
}

esp_err_t oled_present(void)
{
    for (uint8_t page = 0; page < (OLED_HEIGHT / 8); ++page) {
        ESP_RETURN_ON_ERROR(oled_send_page(page, &s_oled.fb[page * OLED_WIDTH]), TAG, "flush page %u failed", page);
    }
    return ESP_OK;
}

esp_err_t oled_render_animation_frame_centered(const oled_animation_t *anim,
                                               uint16_t frame_index,
                                               int8_t shift_x,
                                               int8_t shift_y)
{
    if (anim == NULL || anim->frames == NULL || anim->frame_count == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    if (frame_index >= anim->frame_count) {
        return ESP_ERR_INVALID_ARG;
    }

    const oled_animation_frame_t *frame = &anim->frames[frame_index];
    if (frame->bitmap == NULL || anim->width == 0U || anim->height == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    const int x = ((OLED_WIDTH - (int)anim->width) / 2) + (int)shift_x;
    const int y = ((OLED_HEIGHT - (int)anim->height) / 2) + (int)shift_y;

    oled_clear_buffer();
    oled_draw_bitmap_mono(x, y, anim->width, anim->height, frame->bitmap, anim->bit_packed);
    return oled_present();
}

enum {
    SEG_A = 1 << 0,
    SEG_B = 1 << 1,
    SEG_C = 1 << 2,
    SEG_D = 1 << 3,
    SEG_E = 1 << 4,
    SEG_F = 1 << 5,
    SEG_G = 1 << 6,
};

static const uint8_t s_digit_segments[10] = {
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F,
    SEG_B | SEG_C,
    SEG_A | SEG_B | SEG_D | SEG_E | SEG_G,
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_G,
    SEG_B | SEG_C | SEG_F | SEG_G,
    SEG_A | SEG_C | SEG_D | SEG_F | SEG_G,
    SEG_A | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G,
    SEG_A | SEG_B | SEG_C,
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_E | SEG_F | SEG_G,
    SEG_A | SEG_B | SEG_C | SEG_D | SEG_F | SEG_G,
};

static void oled_draw_7seg_digit(int x, int y, int scale, int digit)
{
    const int t = scale;
    const int l = 4 * scale;
    uint8_t mask = SEG_G;
    if (digit >= 0 && digit <= 9) {
        mask = s_digit_segments[digit];
    }

    if (mask & SEG_A) oled_fill_rect(x + t, y, l, t, true);
    if (mask & SEG_B) oled_fill_rect(x + t + l, y + t, t, l, true);
    if (mask & SEG_C) oled_fill_rect(x + t + l, y + (2 * t) + l, t, l, true);
    if (mask & SEG_D) oled_fill_rect(x + t, y + (2 * l) + (2 * t), l, t, true);
    if (mask & SEG_E) oled_fill_rect(x, y + (2 * t) + l, t, l, true);
    if (mask & SEG_F) oled_fill_rect(x, y + t, t, l, true);
    if (mask & SEG_G) oled_fill_rect(x + t, y + l + t, l, t, true);
}

static void oled_draw_colon(int x, int y, int scale, bool visible)
{
    if (!visible) {
        return;
    }
    const int dot = scale + 1;
    oled_fill_rect(x, y + (3 * scale), dot, dot, true);
    oled_fill_rect(x, y + (7 * scale), dot, dot, true);
}

static void oled_draw_clock(const struct tm *timeinfo, int8_t shift_x, int8_t shift_y)
{
    const int scale = 2;
    const int t = scale;
    const int l = 4 * scale;
    const int digit_w = l + (2 * t);
    const int digit_h = (2 * l) + (3 * t);
    const int colon_w = scale + 1;
    const int gap = 2;
    const int total_w = (6 * digit_w) + (2 * colon_w) + (7 * gap);

    int x = ((OLED_WIDTH - total_w) / 2) + (int)shift_x;
    const int y = ((OLED_HEIGHT - digit_h) / 2) + (int)shift_y;

    int digits[6] = {
        timeinfo->tm_hour / 10,
        timeinfo->tm_hour % 10,
        timeinfo->tm_min / 10,
        timeinfo->tm_min % 10,
        timeinfo->tm_sec / 10,
        timeinfo->tm_sec % 10,
    };
    const bool colon_visible = (timeinfo->tm_sec % 2) == 0;

    for (int i = 0; i < 6; ++i) {
        oled_draw_7seg_digit(x, y, scale, digits[i]);
        x += digit_w + gap;
        if (i == 1 || i == 3) {
            oled_draw_colon(x, y, scale, colon_visible);
            x += colon_w + gap;
        }
    }

    const bool synced = (timeinfo->tm_year >= (2024 - 1900));
    if (synced) {
        oled_fill_rect((OLED_WIDTH - 6) + (int)shift_x, 2 + (int)shift_y, 4, 4, true);
    } else {
        oled_fill_rect(2 + (int)shift_x, (OLED_HEIGHT - 4) + (int)shift_y, OLED_WIDTH - 4, 2, true);
    }
}

esp_err_t oled_init(void)
{
    uint32_t oled_i2c_hz = (uint32_t)MACRO_OLED_I2C_SCL_HZ;
    if (oled_i2c_hz < 100000U) {
        oled_i2c_hz = 100000U;
    } else if (oled_i2c_hz > 1000000U) {
        oled_i2c_hz = 1000000U;
    }

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = OLED_I2C_PORT,
        .sda_io_num = OLED_SDA_GPIO,
        .scl_io_num = OLED_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_oled.bus));

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = OLED_I2C_ADDR,
        .scl_speed_hz = oled_i2c_hz,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_oled.bus, &dev_cfg, &s_oled.dev));

    static const uint8_t init_cmds[] = {
        0xAE, 0xD5, 0x80, 0xA8, 0x3F, 0xD3, 0x00, 0x40,
        0x8D, 0x14, 0x20, 0x00, 0xA1, 0xC8, 0xDA, 0x12,
        0x81, 0xCF, 0xD9, 0xF1, 0xDB, 0x40, 0xA4, 0xA6,
        0x2E, 0xAF,
    };

    for (size_t i = 0; i < sizeof(init_cmds); ++i) {
        ESP_RETURN_ON_ERROR(oled_send_cmd(init_cmds[i]), TAG, "oled init cmd failed");
    }

    oled_clear_buffer();
    ESP_RETURN_ON_ERROR(oled_present(), TAG, "oled initial flush failed");
    return oled_set_brightness_percent(100U);
}

esp_err_t oled_set_brightness_percent(uint8_t percent)
{
    if (percent > 100U) {
        percent = 100U;
    }
    const uint8_t contrast = oled_percent_to_contrast(percent);
    ESP_RETURN_ON_ERROR(oled_send_cmd(0x81), TAG, "set contrast cmd failed");
    ESP_RETURN_ON_ERROR(oled_send_cmd(contrast), TAG, "set contrast value failed");
    s_oled.brightness_percent = percent;
    return ESP_OK;
}

esp_err_t oled_set_display_enabled(bool enabled)
{
    ESP_RETURN_ON_ERROR(oled_send_cmd(enabled ? 0xAF : 0xAE), TAG, "set display power failed");
    s_oled.display_enabled = enabled;
    return ESP_OK;
}

esp_err_t oled_set_inverted(bool inverted)
{
    ESP_RETURN_ON_ERROR(oled_send_cmd(inverted ? 0xA7 : 0xA6), TAG, "set display invert failed");
    s_oled.inverted = inverted;
    return ESP_OK;
}

esp_err_t oled_render_clock(const struct tm *timeinfo, int8_t shift_x, int8_t shift_y)
{
    if (timeinfo == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    oled_clear_buffer();
    oled_draw_clock(timeinfo, shift_x, shift_y);
    return oled_present();
}
