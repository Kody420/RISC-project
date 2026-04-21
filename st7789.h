#pragma once

#ifndef RISC_PROJECT_ST7789_H
#define RISC_PROJECT_ST7789_H

#include <stdint.h>
#include <stdbool.h>

#define ST7789_WIDTH 284
#define ST7789_HEIGHT 76
#define ST7789_FRAMEBUFFER_SIZE_BYTES (ST7789_WIDTH * ST7789_HEIGHT * 2u)
#define ST7789_FRAME_INTERVAL_US (1000000u / 60u)

typedef uint8_t st7789_font_t;

extern const st7789_font_t u8g2_font_t0_11_tr[];
extern const st7789_font_t u8g2_font_inr16_mf[];

#define ST7789_FONT_DEFAULT_MONO u8g2_font_t0_11_tr
#define ST7789_FONT_DEFAULT_SANS u8g2_font_inr16_mf

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

/*
 * h: 0–255  (maps to 0–360°)
 * s: 0–255
 * v: 0–255
 */
static inline uint16_t hsv565(uint8_t h, uint8_t s, uint8_t v)
{
    if (s == 0)
    {
        // grayscale
        return rgb565(v, v, v);
    }

    uint8_t region = h / 43;              // 256 / 6 ≈ 43
    uint8_t remainder = (h - region * 43) * 6;

    uint8_t p = (v * (255 - s)) >> 8;
    uint8_t q = (v * (255 - ((s * remainder) >> 8))) >> 8;
    uint8_t t = (v * (255 - ((s * (255 - remainder)) >> 8))) >> 8;

    uint8_t r, g, b;

    switch (region)
    {
        default:
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        case 5: r = v; g = p; b = q; break;
    }

    return rgb565(r, g, b);
}

void st7789_init(void);
void st7789_send_buffer(const uint8_t *buffer);
void st7789_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color);
void st7789_present_full(void);
void st7789_refresh_hook(uint8_t slice, void *context);
uint16_t st7789_font_get_line_height(const st7789_font_t *font);
uint16_t st7789_text_width(const st7789_font_t *font, const char *text);
uint16_t st7789_draw_text(uint16_t x, uint16_t y, uint16_t max_x, const char *text, const st7789_font_t *font, uint16_t color);
uint16_t st7789_draw_glyph(uint16_t x, uint16_t y, uint16_t encoding, const st7789_font_t *font, uint16_t color);
void st7789_draw_debug_cell_with_font(uint8_t col, uint8_t row, const char *text, const st7789_font_t *font, uint16_t text_color);
void st7789_draw_debug_cell(uint8_t col, uint8_t row, const char *text, uint16_t text_color);
static inline void st7789_draw_pixel(int x, int y, uint16_t color);
void st7789_draw_line(int x0, int y0, int x1, int y1, uint16_t color);
static void st7789_draw_rect(int x, int y, int w, int h, uint16_t color);
static void st7789_draw_hline(int x, int y, int w, uint16_t color);
static void st7789_draw_vline(int x, int y, int h, uint16_t color);

/*
 * plot_t
 *
 * Object-like plot structure.
 *
 * Behavior:
 * - Fixed position and size (x, y, width, height)
 * - Scrolls left over time
 * - New samples added via plot_add()
 *
 * time_scale:
 *   Number of plot_add() calls required before the plot shifts by 1 pixel.
 *   Example:
 *     1  = shift every update (fast)
 *     10 = slower movement
 *
 * Vertical scaling:
 * - If min_value != 0 OR max_value != 0 → fixed scaling
 * - If min_value == 0 AND max_value == 0 → auto-scaling from visible data
 *
 * history:
 * - Must be provided by user
 * - Recommended size = width - 2 (inner drawable width)
 */

typedef struct
{
    int16_t x;
    int16_t y;
    uint16_t width;
    uint16_t height;

    /* number of plot_add() calls per 1 pixel shift */
    uint16_t time_scale;
    uint16_t tick;

    /*
     * Fixed vertical scale:
     * if both are 0 => auto-scale from visible data
     */
    int16_t min_value;
    int16_t max_value;

    uint16_t bg_color;
    uint16_t line_color;
    uint16_t border_color;

    /*
     * One entry per inner X pixel is recommended:
     * history_len = width - 2
     */
    int16_t *history;
    uint16_t history_len;

    /*
     * How many history entries are currently valid
     */
    uint16_t count;

} plot_t;

void plot_init(plot_t *p,
               int16_t x,
               int16_t y,
               uint16_t width,
               uint16_t height,
               uint16_t time_scale,
               int16_t min_value,
               int16_t max_value,
               uint16_t bg_color,
               uint16_t line_color,
               int16_t *history_buffer,
               uint16_t history_buffer_len);

void plot_clear(plot_t *p);
void plot_add(plot_t *p, int16_t value);
void plot_draw(plot_t *p, bool clear_before_draw);

#endif