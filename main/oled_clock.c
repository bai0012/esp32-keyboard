#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "driver/i2c_master.h"

#include "esp_check.h"

#include "oled_clock.h"

#define TAG "MACROPAD"

#define OLED_SDA_GPIO GPIO_NUM_15
#define OLED_SCL_GPIO GPIO_NUM_16
#define OLED_I2C_PORT 0
#define OLED_I2C_ADDR 0x3C
#define OLED_WIDTH 128
#define OLED_HEIGHT 64

static i2c_master_bus_handle_t s_i2c_bus;
static i2c_master_dev_handle_t s_oled_dev;
static uint8_t s_oled_fb[OLED_WIDTH * OLED_HEIGHT / 8];
static bool s_display_enabled = true;
static bool s_inverted = false;
static uint8_t s_brightness_percent = 100;

static esp_err_t oled_send_cmd(uint8_t cmd)
{
    uint8_t payload[2] = {0x00, cmd};
    return i2c_master_transmit(s_oled_dev, payload, sizeof(payload), -1);
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
    return i2c_master_transmit(s_oled_dev, payload, sizeof(payload), -1);
}

static void oled_clear_buffer(void)
{
    memset(s_oled_fb, 0, sizeof(s_oled_fb));
}

static void oled_set_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) {
        return;
    }

    const size_t index = (size_t)x + ((size_t)y / 8U) * OLED_WIDTH;
    const uint8_t mask = (uint8_t)(1U << (y & 0x7));

    if (on) {
        s_oled_fb[index] |= mask;
    } else {
        s_oled_fb[index] &= (uint8_t)~mask;
    }
}

static void oled_fill_rect(int x, int y, int w, int h, bool on)
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

static esp_err_t oled_flush(void)
{
    for (uint8_t page = 0; page < (OLED_HEIGHT / 8); ++page) {
        ESP_RETURN_ON_ERROR(oled_send_page(page, &s_oled_fb[page * OLED_WIDTH]), TAG, "flush page %u failed", page);
    }
    return ESP_OK;
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

esp_err_t oled_clock_init(void)
{
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = OLED_I2C_PORT,
        .sda_io_num = OLED_SDA_GPIO,
        .scl_io_num = OLED_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &s_i2c_bus));

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = OLED_I2C_ADDR,
        .scl_speed_hz = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(s_i2c_bus, &dev_cfg, &s_oled_dev));

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
    ESP_RETURN_ON_ERROR(oled_flush(), TAG, "oled initial flush failed");
    return oled_clock_set_brightness_percent(100U);
}

esp_err_t oled_clock_set_brightness_percent(uint8_t percent)
{
    if (percent > 100U) {
        percent = 100U;
    }
    const uint8_t contrast = oled_percent_to_contrast(percent);
    ESP_RETURN_ON_ERROR(oled_send_cmd(0x81), TAG, "set contrast cmd failed");
    ESP_RETURN_ON_ERROR(oled_send_cmd(contrast), TAG, "set contrast value failed");
    s_brightness_percent = percent;
    return ESP_OK;
}

esp_err_t oled_clock_set_display_enabled(bool enabled)
{
    ESP_RETURN_ON_ERROR(oled_send_cmd(enabled ? 0xAF : 0xAE), TAG, "set display power failed");
    s_display_enabled = enabled;
    return ESP_OK;
}

esp_err_t oled_clock_set_inverted(bool inverted)
{
    ESP_RETURN_ON_ERROR(oled_send_cmd(inverted ? 0xA7 : 0xA6), TAG, "set display invert failed");
    s_inverted = inverted;
    return ESP_OK;
}

esp_err_t oled_clock_render(const struct tm *timeinfo, int8_t shift_x, int8_t shift_y)
{
    if (timeinfo == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    (void)s_display_enabled;
    (void)s_inverted;
    (void)s_brightness_percent;

    oled_clear_buffer();
    oled_draw_clock(timeinfo, shift_x, shift_y);
    return oled_flush();
}
