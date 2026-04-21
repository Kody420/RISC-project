#include "st7789.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "hardware/spi.h"
#include "pico/stdlib.h"

#define ST7789_SPI spi0
#define ST7789_SCK_PIN 18
#define ST7789_MOSI_PIN 19
#define ST7789_CS_PIN 21
#define ST7789_DC_PIN 20
#define ST7789_RST_PIN 17
#define ST7789_SPI_BAUDRATE (100 * 1000 * 1000)

#define ST7789_XSTART 18
#define ST7789_YSTART 82
#define ST7789_MADCTL 0x60
#define ST7789_TRANSFER_SLICES 8u
#define ST7789_U8G2_FONT_DATA_STRUCT_SIZE 23u

#define DBG_GRID_COLS 3
#define DBG_GRID_ROWS 4
#define DBG_GRID_MARGIN_X 2
#define DBG_GRID_MARGIN_Y 2
#define DBG_GRID_GAP_X 2
#define DBG_GRID_GAP_Y 2

#define RGB565_WHITE 0xFFFF

static uint8_t g_st7789_framebuffer[ST7789_HEIGHT][ST7789_WIDTH][2];

typedef struct
{
    uint8_t bits_per_0;
    uint8_t bits_per_1;
    uint8_t bits_per_char_width;
    uint8_t bits_per_char_height;
    uint8_t bits_per_char_x;
    uint8_t bits_per_char_y;
    uint8_t bits_per_delta_x;
    int8_t max_char_height;
    int8_t ascent_A;
    int8_t x_offset;
    int8_t y_offset;
    uint16_t start_pos_upper_A;
    uint16_t start_pos_lower_a;
    uint16_t start_pos_unicode;
} st7789_u8g2_font_info_t;

typedef struct
{
    const uint8_t *decode_ptr;
    int16_t target_x;
    int16_t target_y;
    uint8_t x;
    uint8_t y;
    uint8_t glyph_width;
    uint8_t glyph_height;
    uint8_t decode_bit_pos;
} st7789_u8g2_font_decode_t;

typedef struct
{
    uint8_t width;
    int8_t x_offset;
    int8_t y_offset;
    int8_t delta_x;
} st7789_u8g2_glyph_metrics_t;

static inline void st7789_select(void)
{
    gpio_put(ST7789_CS_PIN, false);
}

static inline void st7789_deselect(void)
{
    gpio_put(ST7789_CS_PIN, true);
}

static inline void st7789_dc_command(void)
{
    gpio_put(ST7789_DC_PIN, false);
}

static inline void st7789_dc_data(void)
{
    gpio_put(ST7789_DC_PIN, true);
}

static void st7789_write_cmd(uint8_t cmd)
{
    st7789_select();
    st7789_dc_command();
    spi_write_blocking(ST7789_SPI, &cmd, 1);
    st7789_deselect();
}

static void st7789_write_data(const uint8_t *data, size_t len)
{
    if (len == 0)
        return;

    st7789_select();
    st7789_dc_data();
    spi_write_blocking(ST7789_SPI, data, len);
    st7789_deselect();
}

static void st7789_hw_reset(void)
{
    gpio_put(ST7789_RST_PIN, true);
    sleep_ms(5);
    gpio_put(ST7789_RST_PIN, false);
    sleep_ms(20);
    gpio_put(ST7789_RST_PIN, true);
    sleep_ms(120);
}

static void st7789_set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint16_t xs = (uint16_t)(x0 + ST7789_XSTART);
    uint16_t xe = (uint16_t)(x1 + ST7789_XSTART);
    uint16_t ys = (uint16_t)(y0 + ST7789_YSTART);
    uint16_t ye = (uint16_t)(y1 + ST7789_YSTART);

    uint8_t col_data[4] = {(uint8_t)(xs >> 8), (uint8_t)xs, (uint8_t)(xe >> 8), (uint8_t)xe};
    uint8_t row_data[4] = {(uint8_t)(ys >> 8), (uint8_t)ys, (uint8_t)(ye >> 8), (uint8_t)ye};

    st7789_write_cmd(0x2A);
    st7789_write_data(col_data, sizeof(col_data));

    st7789_write_cmd(0x2B);
    st7789_write_data(row_data, sizeof(row_data));

    st7789_write_cmd(0x2C);
}

static void st7789_present_rows(uint16_t y_start, uint16_t y_end)
{
    if (y_start >= ST7789_HEIGHT || y_start >= y_end)
        return;
    if (y_end > ST7789_HEIGHT)
        y_end = ST7789_HEIGHT;

    st7789_set_window(0, y_start, ST7789_WIDTH - 1, (uint16_t)(y_end - 1));

    st7789_select();
    st7789_dc_data();
    for (uint16_t row = y_start; row < y_end; ++row)
    {
        spi_write_blocking(ST7789_SPI, &g_st7789_framebuffer[row][0][0], sizeof(g_st7789_framebuffer[row]));
    }
    st7789_deselect();
}

static uint8_t st7789_u8g2_font_get_byte(const st7789_font_t *font, uint8_t offset)
{
    return font[offset];
}

static uint16_t st7789_u8g2_font_get_word(const st7789_font_t *font, uint8_t offset)
{
    uint16_t value = (uint16_t)font[offset] << 8;
    value |= font[(uint8_t)(offset + 1u)];
    return value;
}

static void st7789_u8g2_read_font_info(const st7789_font_t *font, st7789_u8g2_font_info_t *info)
{
    info->bits_per_0 = st7789_u8g2_font_get_byte(font, 2);
    info->bits_per_1 = st7789_u8g2_font_get_byte(font, 3);
    info->bits_per_char_width = st7789_u8g2_font_get_byte(font, 4);
    info->bits_per_char_height = st7789_u8g2_font_get_byte(font, 5);
    info->bits_per_char_x = st7789_u8g2_font_get_byte(font, 6);
    info->bits_per_char_y = st7789_u8g2_font_get_byte(font, 7);
    info->bits_per_delta_x = st7789_u8g2_font_get_byte(font, 8);
    info->max_char_height = (int8_t)st7789_u8g2_font_get_byte(font, 10);
    info->x_offset = (int8_t)st7789_u8g2_font_get_byte(font, 11);
    info->y_offset = (int8_t)st7789_u8g2_font_get_byte(font, 12);
    info->ascent_A = (int8_t)st7789_u8g2_font_get_byte(font, 13);
    info->start_pos_upper_A = st7789_u8g2_font_get_word(font, 17);
    info->start_pos_lower_a = st7789_u8g2_font_get_word(font, 19);
    info->start_pos_unicode = st7789_u8g2_font_get_word(font, 21);
}

static uint8_t st7789_u8g2_get_unsigned_bits(st7789_u8g2_font_decode_t *decode, uint8_t bit_count)
{
    uint16_t value;
    uint8_t bit_pos;
    uint8_t bit_pos_plus_count;

    if (bit_count == 0)
        return 0;

    bit_pos = decode->decode_bit_pos;
    value = decode->decode_ptr[0];
    value >>= bit_pos;

    bit_pos_plus_count = (uint8_t)(bit_pos + bit_count);
    if (bit_pos_plus_count >= 8u)
    {
        uint8_t shift = (uint8_t)(8u - bit_pos);
        decode->decode_ptr++;
        value |= (uint16_t)decode->decode_ptr[0] << shift;
        bit_pos_plus_count = (uint8_t)(bit_pos_plus_count - 8u);
    }

    value &= (uint16_t)((1u << bit_count) - 1u);
    decode->decode_bit_pos = bit_pos_plus_count;
    return (uint8_t)value;
}

static int8_t st7789_u8g2_get_signed_bits(st7789_u8g2_font_decode_t *decode, uint8_t bit_count)
{
    int8_t value;
    int8_t delta;

    if (bit_count == 0)
        return 0;

    value = (int8_t)st7789_u8g2_get_unsigned_bits(decode, bit_count);
    delta = 1;
    bit_count--;
    delta = (int8_t)(delta << bit_count);
    return (int8_t)(value - delta);
}

static void st7789_draw_hline_clipped(int16_t x, int16_t y, uint8_t len, uint16_t color)
{
    int16_t x_end;
    uint16_t draw_x;
    uint16_t draw_w;

    if (len == 0 || y < 0 || y >= ST7789_HEIGHT)
        return;

    x_end = (int16_t)(x + (int16_t)len - 1);
    if (x_end < 0 || x >= ST7789_WIDTH)
        return;

    draw_x = (uint16_t)(x < 0 ? 0 : x);
    if (x_end >= ST7789_WIDTH)
        x_end = ST7789_WIDTH - 1;
    draw_w = (uint16_t)(x_end - (int16_t)draw_x + 1);

    st7789_fill_rect(draw_x, (uint16_t)y, draw_w, 1, color);
}

static void st7789_u8g2_decode_len(st7789_u8g2_font_decode_t *decode, uint8_t len, uint16_t color, bool is_foreground)
{
    uint8_t remaining = len;
    uint8_t local_x = decode->x;
    uint8_t local_y = decode->y;

    while (remaining > 0)
    {
        uint8_t run = (uint8_t)(decode->glyph_width - local_x);
        if (run > remaining)
            run = remaining;

        if (is_foreground)
        {
            st7789_draw_hline_clipped((int16_t)(decode->target_x + local_x),
                                      (int16_t)(decode->target_y + local_y),
                                      run,
                                      color);
        }

        remaining = (uint8_t)(remaining - run);
        local_x = (uint8_t)(local_x + run);

        if (local_x >= decode->glyph_width)
        {
            local_x = 0;
            local_y++;
        }

        if (remaining == 0)
            break;
    }

    decode->x = local_x;
    decode->y = local_y;
}

static void st7789_u8g2_setup_decode(const st7789_u8g2_font_info_t *info,
                                     const uint8_t *glyph_data,
                                     st7789_u8g2_font_decode_t *decode)
{
    decode->decode_ptr = glyph_data;
    decode->decode_bit_pos = 0;
    decode->glyph_width = st7789_u8g2_get_unsigned_bits(decode, info->bits_per_char_width);
    decode->glyph_height = st7789_u8g2_get_unsigned_bits(decode, info->bits_per_char_height);
    decode->x = 0;
    decode->y = 0;
}

static const uint8_t *st7789_u8g2_find_glyph(const st7789_font_t *font,
                                             const st7789_u8g2_font_info_t *info,
                                             uint16_t encoding)
{
    const uint8_t *glyph = font + ST7789_U8G2_FONT_DATA_STRUCT_SIZE;

    if (encoding <= 255u)
    {
        if (encoding >= 'a')
            glyph += info->start_pos_lower_a;
        else if (encoding >= 'A')
            glyph += info->start_pos_upper_A;

        for (;;)
        {
            if (glyph[1] == 0)
                break;
            if (glyph[0] == (uint8_t)encoding)
                return glyph + 2;
            glyph += glyph[1];
        }

        return NULL;
    }

    glyph += info->start_pos_unicode;
    {
        const uint8_t *lookup = glyph;
        uint16_t current_encoding;

        do
        {
            glyph += st7789_u8g2_font_get_word(lookup, 0);
            current_encoding = st7789_u8g2_font_get_word(lookup, 2);
            lookup += 4;
        } while (current_encoding < encoding);
    }

    for (;;)
    {
        uint16_t current_encoding = (uint16_t)glyph[0] << 8;
        current_encoding |= glyph[1];
        if (current_encoding == 0)
            break;
        if (current_encoding == encoding)
            return glyph + 3;
        glyph += glyph[2];
    }

    return NULL;
}

static bool st7789_u8g2_get_glyph_metrics(const st7789_font_t *font,
                                          uint16_t encoding,
                                          st7789_u8g2_font_info_t *info,
                                          st7789_u8g2_glyph_metrics_t *metrics)
{
    const uint8_t *glyph_data;
    st7789_u8g2_font_decode_t decode;

    if (font == NULL || metrics == NULL)
        return false;

    st7789_u8g2_read_font_info(font, info);
    glyph_data = st7789_u8g2_find_glyph(font, info, encoding);
    if (glyph_data == NULL)
        return false;

    st7789_u8g2_setup_decode(info, glyph_data, &decode);
    metrics->width = decode.glyph_width;
    metrics->x_offset = st7789_u8g2_get_signed_bits(&decode, info->bits_per_char_x);
    metrics->y_offset = st7789_u8g2_get_signed_bits(&decode, info->bits_per_char_y);
    metrics->delta_x = st7789_u8g2_get_signed_bits(&decode, info->bits_per_delta_x);
    return true;
}

static bool st7789_utf8_next(const char **text, uint16_t *encoding)
{
    const uint8_t *cursor = (const uint8_t *)*text;

    if (*cursor == 0)
        return false;

    if (cursor[0] < 0x80u)
    {
        *encoding = cursor[0];
        *text += 1;
        return true;
    }

    if ((cursor[0] & 0xE0u) == 0xC0u && (cursor[1] & 0xC0u) == 0x80u)
    {
        *encoding = (uint16_t)(cursor[0] & 0x1Fu) << 6;
        *encoding |= (uint16_t)(cursor[1] & 0x3Fu);
        *text += 2;
        return true;
    }

    if ((cursor[0] & 0xF0u) == 0xE0u && (cursor[1] & 0xC0u) == 0x80u && (cursor[2] & 0xC0u) == 0x80u)
    {
        *encoding = (uint16_t)(cursor[0] & 0x0Fu) << 12;
        *encoding |= (uint16_t)(cursor[1] & 0x3Fu) << 6;
        *encoding |= (uint16_t)(cursor[2] & 0x3Fu);
        *text += 3;
        return true;
    }

    *encoding = '?';
    *text += 1;
    return true;
}

uint16_t st7789_font_get_line_height(const st7789_font_t *font)
{
    st7789_u8g2_font_info_t info;

    if (font == NULL)
        return 0;

    st7789_u8g2_read_font_info(font, &info);
    return (uint16_t)(info.max_char_height > 0 ? info.max_char_height : 0);
}

uint16_t st7789_draw_glyph(uint16_t x, uint16_t y, uint16_t encoding, const st7789_font_t *font, uint16_t color)
{
    const uint8_t *glyph_data;
    st7789_u8g2_font_info_t info;
    st7789_u8g2_font_decode_t decode;
    int8_t glyph_x;
    int8_t glyph_y;
    int8_t delta_x;

    if (font == NULL)
        return 0;

    st7789_u8g2_read_font_info(font, &info);
    glyph_data = st7789_u8g2_find_glyph(font, &info, encoding);
    if (glyph_data == NULL)
        return 0;

    st7789_u8g2_setup_decode(&info, glyph_data, &decode);
    glyph_x = st7789_u8g2_get_signed_bits(&decode, info.bits_per_char_x);
    glyph_y = st7789_u8g2_get_signed_bits(&decode, info.bits_per_char_y);
    delta_x = st7789_u8g2_get_signed_bits(&decode, info.bits_per_delta_x);

    if (decode.glyph_width > 0)
    {
        decode.target_x = (int16_t)x + glyph_x;
        decode.target_y = (int16_t)y + info.ascent_A - decode.glyph_height - glyph_y;

        for (;;)
        {
            uint8_t zero_run = st7789_u8g2_get_unsigned_bits(&decode, info.bits_per_0);
            uint8_t one_run = st7789_u8g2_get_unsigned_bits(&decode, info.bits_per_1);
            do
            {
                st7789_u8g2_decode_len(&decode, zero_run, color, false);
                st7789_u8g2_decode_len(&decode, one_run, color, true);
            } while (st7789_u8g2_get_unsigned_bits(&decode, 1) != 0u);

            if (decode.y >= decode.glyph_height)
                break;
        }
    }

    return (uint16_t)(delta_x > 0 ? delta_x : 0);
}

uint16_t st7789_text_width(const st7789_font_t *font, const char *text)
{
    const char *cursor = text;
    uint16_t encoding;
    uint16_t width = 0;
    uint8_t last_glyph_width = 0;
    int8_t last_x_offset = 0;
    int8_t last_delta_x = 0;
    int8_t initial_x_offset = -64;
    bool has_glyph = false;
    st7789_u8g2_font_info_t info;
    st7789_u8g2_glyph_metrics_t metrics;

    if (font == NULL || text == NULL)
        return 0;

    while (st7789_utf8_next(&cursor, &encoding))
    {
        if (!st7789_u8g2_get_glyph_metrics(font, encoding, &info, &metrics))
            continue;

        if (!has_glyph)
        {
            initial_x_offset = metrics.x_offset;
            has_glyph = true;
        }

        width = (uint16_t)(width + (metrics.delta_x > 0 ? metrics.delta_x : 0));
        last_glyph_width = metrics.width;
        last_x_offset = metrics.x_offset;
        last_delta_x = metrics.delta_x;
    }

    if (!has_glyph || last_glyph_width == 0)
        return width;

    if (last_delta_x > 0)
        width = (uint16_t)(width - last_delta_x);
    width = (uint16_t)(width + last_glyph_width);
    if (last_x_offset > 0)
        width = (uint16_t)(width + last_x_offset);
    if (initial_x_offset > 0)
        width = (uint16_t)(width + initial_x_offset);

    return width;
}

uint16_t st7789_draw_text(uint16_t x, uint16_t y, uint16_t max_x, const char *text, const st7789_font_t *font, uint16_t color)
{
    const char *cursor = text;
    uint16_t encoding;
    uint16_t start_x = x;
    st7789_u8g2_font_info_t info;
    st7789_u8g2_glyph_metrics_t metrics;

    if (font == NULL || text == NULL)
        return 0;

    while (st7789_utf8_next(&cursor, &encoding))
    {
        int16_t glyph_right;
        int16_t advance_right;
        int16_t draw_right;
        uint16_t advance;

        if (!st7789_u8g2_get_glyph_metrics(font, encoding, &info, &metrics))
            continue;

        glyph_right = (int16_t)x + (metrics.x_offset > 0 ? metrics.x_offset : 0) + metrics.width;
        advance_right = (int16_t)x + metrics.delta_x;
        draw_right = glyph_right > advance_right ? glyph_right : advance_right;
        if (draw_right > max_x)
            break;

        advance = st7789_draw_glyph(x, y, encoding, font, color);
        x = (uint16_t)(x + advance);
        if (x >= ST7789_WIDTH)
            break;
    }

    return (uint16_t)(x - start_x);
}

void st7789_init(void)
{
    spi_init(ST7789_SPI, ST7789_SPI_BAUDRATE);
    gpio_set_function(ST7789_SCK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(ST7789_MOSI_PIN, GPIO_FUNC_SPI);

    gpio_init(ST7789_CS_PIN);
    gpio_set_dir(ST7789_CS_PIN, GPIO_OUT);
    gpio_put(ST7789_CS_PIN, true);

    gpio_init(ST7789_DC_PIN);
    gpio_set_dir(ST7789_DC_PIN, GPIO_OUT);
    gpio_put(ST7789_DC_PIN, true);

    gpio_init(ST7789_RST_PIN);
    gpio_set_dir(ST7789_RST_PIN, GPIO_OUT);
    gpio_put(ST7789_RST_PIN, true);

    st7789_hw_reset();

    st7789_write_cmd(0x01);
    sleep_ms(150);

    st7789_write_cmd(0x11);
    sleep_ms(120);

    {
        uint8_t pixel_format = 0x55;
        st7789_write_cmd(0x3A);
        st7789_write_data(&pixel_format, 1);
    }

    {
        uint8_t madctl = ST7789_MADCTL;
        st7789_write_cmd(0x36);
        st7789_write_data(&madctl, 1);
    }

    st7789_write_cmd(0x20);
    st7789_write_cmd(0x13);
    st7789_write_cmd(0x29);
    sleep_ms(20);
}

static void st7789_draw_rect(int x, int y, int w, int h, uint16_t color)
{
    if (w <= 0 || h <= 0)
        return;

    st7789_draw_hline(x, y, w, color);
    st7789_draw_hline(x, y + h - 1, w, color);
    st7789_draw_vline(x, y, h, color);
    st7789_draw_vline(x + w - 1, y, h, color);
}

void st7789_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    if (x >= ST7789_WIDTH || y >= ST7789_HEIGHT || w == 0 || h == 0)
        return;

    uint16_t x_end = (uint16_t)(x + w - 1);
    uint16_t y_end = (uint16_t)(y + h - 1);
    if (x_end >= ST7789_WIDTH)
        x_end = ST7789_WIDTH - 1;
    if (y_end >= ST7789_HEIGHT)
        y_end = ST7789_HEIGHT - 1;

    uint8_t color_hi = (uint8_t)(color >> 8);
    uint8_t color_lo = (uint8_t)color;
    for (uint16_t row = y; row <= y_end; ++row)
    {
        uint8_t *pixel = &g_st7789_framebuffer[row][x][0];
        for (uint16_t col = x; col <= x_end; ++col)
        {
            pixel[0] = color_hi;
            pixel[1] = color_lo;
            pixel += 2;
        }
    }
}

void st7789_send_buffer(const uint8_t *buffer)
{
    if (buffer == NULL)
        return;

    st7789_set_window(0, 0, ST7789_WIDTH - 1, ST7789_HEIGHT - 1);

    st7789_select();
    st7789_dc_data();
    spi_write_blocking(ST7789_SPI, buffer, ST7789_FRAMEBUFFER_SIZE_BYTES);
    st7789_deselect();
}

void st7789_present_full(void)
{
    st7789_send_buffer(&g_st7789_framebuffer[0][0][0]);
}

void st7789_refresh_hook(uint8_t slice, void *context)
{
    (void)context;

    uint16_t y_start = (uint16_t)(((uint32_t)slice * ST7789_HEIGHT) / ST7789_TRANSFER_SLICES);
    uint16_t y_end = (uint16_t)((((uint32_t)slice + 1u) * ST7789_HEIGHT) / ST7789_TRANSFER_SLICES);

    st7789_present_rows(y_start, y_end);
}

void st7789_draw_debug_cell_with_font(uint8_t col, uint8_t row, const char *text, const st7789_font_t *font, uint16_t text_color)
{
    const uint16_t bg = rgb565(10, 10, 10);
    const uint16_t border = rgb565(35, 35, 35);

    const uint16_t cell_w = (uint16_t)((ST7789_WIDTH - 2 * DBG_GRID_MARGIN_X - (DBG_GRID_COLS - 1) * DBG_GRID_GAP_X) / DBG_GRID_COLS);
    const uint16_t cell_h = (uint16_t)((ST7789_HEIGHT - 2 * DBG_GRID_MARGIN_Y - (DBG_GRID_ROWS - 1) * DBG_GRID_GAP_Y) / DBG_GRID_ROWS);

    if (col >= DBG_GRID_COLS || row >= DBG_GRID_ROWS)
        return;

    uint16_t x = (uint16_t)(DBG_GRID_MARGIN_X + col * (cell_w + DBG_GRID_GAP_X));
    uint16_t y = (uint16_t)(DBG_GRID_MARGIN_Y + row * (cell_h + DBG_GRID_GAP_Y));

    st7789_fill_rect(x, y, cell_w, cell_h, bg);
    st7789_fill_rect(x, y, cell_w, 1, border);
    st7789_fill_rect(x, (uint16_t)(y + cell_h - 1), cell_w, 1, border);
    st7789_fill_rect(x, y, 1, cell_h, border);
    st7789_fill_rect((uint16_t)(x + cell_w - 1), y, 1, cell_h, border);

    st7789_draw_text((uint16_t)(x + 2),
                     (uint16_t)(y + 3),
                     (uint16_t)(x + cell_w - 3),
                     text,
                     font,
                     text_color);
}

void st7789_draw_debug_cell(uint8_t col, uint8_t row, const char *text, uint16_t text_color)
{
    st7789_draw_debug_cell_with_font(col, row, text, ST7789_FONT_DEFAULT_MONO, text_color);
}

static inline void st7789_draw_pixel(int x, int y, uint16_t color)
{
    if (x < 0 || x >= ST7789_WIDTH || y < 0 || y >= ST7789_HEIGHT)
        return;

    g_st7789_framebuffer[y][x][0] = (uint8_t)(color >> 8);   // MSB
    g_st7789_framebuffer[y][x][1] = (uint8_t)(color & 0xFF); // LSB
}

void st7789_draw_line(int x0, int y0, int x1, int y1, uint16_t color)
{
    int dx = abs(x1 - x0);
    int sx = (x0 < x1) ? 1 : -1;
    int dy = -abs(y1 - y0);
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;
    int e2;

    while (1)
    {
        st7789_draw_pixel(x0, y0, color);

        if (x0 == x1 && y0 == y1)
            break;

        e2 = 2 * err;

        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }

        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
    }
}

static void st7789_draw_hline(int x, int y, int w, uint16_t color)
{
    if (w <= 0 || y < 0 || y >= ST7789_HEIGHT)
        return;

    if (x < 0)
    {
        w += x;
        x = 0;
    }

    if (x + w > ST7789_WIDTH)
        w = ST7789_WIDTH - x;

    if (w <= 0)
        return;

    for (int i = 0; i < w; i++)
        st7789_draw_pixel(x + i, y, color);
}

static void st7789_draw_vline(int x, int y, int h, uint16_t color)
{
    if (h <= 0 || x < 0 || x >= ST7789_WIDTH)
        return;
        
        if (y < 0)
    {
        h += y;
        y = 0;
    }

    if (y + h > ST7789_HEIGHT)
        h = ST7789_HEIGHT - y;

    if (h <= 0)
    return;
    
    for (int i = 0; i < h; i++)
    st7789_draw_pixel(x, y + i, color);
}

// --- PLOT MESS ---

static int plot_inner_width(const plot_t *p)
{
    return (p->width > 2) ? (p->width - 2) : 0;
}

static int plot_inner_height(const plot_t *p)
{
    return (p->height > 2) ? (p->height - 2) : 0;
}

static void plot_get_scale(const plot_t *p, int16_t *mn, int16_t *mx)
{
    if (p->count == 0)
    {
        *mn = 0;
        *mx = 1;
        return;
    }

    if (p->min_value == 0 && p->max_value == 0)
    {
        int16_t lo = p->history[0];
        int16_t hi = p->history[0];

        for (uint16_t i = 1; i < p->count; i++)
        {
            int16_t v = p->history[i];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }

        if (lo == hi)
        {
            lo -= 1;
            hi += 1;
        }

        *mn = lo;
        *mx = hi;
    }
    else
    {
        *mn = p->min_value;
        *mx = p->max_value;

        if (*mn == *mx)
            *mx = *mn + 1;
    }
}

static int plot_value_to_y(const plot_t *p, int16_t value, int16_t mn, int16_t mx)
{
    int h = plot_inner_height(p);

    if (value < mn) value = mn;
    if (value > mx) value = mx;

    int num = ((int)value - (int)mn) * (h - 1);
    int den = (int)mx - (int)mn;
    int y_from_bottom = (den > 0) ? (num / den) : 0;

    return p->y + 1 + (h - 1 - y_from_bottom);
}

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
               uint16_t history_buffer_len)
{
    if (!p)
        return;

    p->x = x;
    p->y = y;
    p->width = width;
    p->height = height;
    p->time_scale = (time_scale == 0) ? 1 : time_scale;
    p->tick = 0;
    p->min_value = min_value;
    p->max_value = max_value;
    p->bg_color = bg_color;
    p->line_color = line_color;
    p->border_color = 0xFFFF;
    p->history = history_buffer;
    p->history_len = history_buffer_len;
    p->count = 0;

    if (history_buffer)
    {
        for (uint16_t i = 0; i < history_buffer_len; i++)
            history_buffer[i] = 0;
    }
}

void plot_clear(plot_t *p)
{
    if (!p || !p->history)
        return;

    p->tick = 0;
    p->count = 0;

    for (uint16_t i = 0; i < p->history_len; i++)
        p->history[i] = 0;
}

void plot_draw(plot_t *p, bool clear_before_draw)
{
    if (!p || !p->history)
        return;

    int inner_w = plot_inner_width(p);
    int inner_h = plot_inner_height(p);

    if (inner_w <= 0 || inner_h <= 0)
        return;

    if (clear_before_draw)
        st7789_fill_rect(p->x, p->y, p->width, p->height, p->bg_color);
    st7789_draw_rect(p->x, p->y, p->width, p->height, p->border_color);

    if (p->count < 2)
        return;

    int16_t mn, mx;
    plot_get_scale(p, &mn, &mx);

    int prev_x = p->x + 1;
    int prev_y = plot_value_to_y(p, p->history[0], mn, mx);

    for (uint16_t i = 1; i < p->count; i++)
    {
        int x = p->x + 1 + i;
        int y = plot_value_to_y(p, p->history[i], mn, mx);
        st7789_draw_line(prev_x, prev_y, x, y, p->line_color);
        prev_x = x;
        prev_y = y;
    }
}

void plot_add(plot_t *p, int16_t value)
{
    if (!p || !p->history || p->history_len == 0)
        return;

    p->tick++;

    if (p->count == 0)
    {
        p->history[0] = value;
        p->count = 1;
        p->tick = 0;
        return;
    }

    /* update last visible point immediately */
    p->history[p->count - 1] = value;

    if (p->tick >= p->time_scale)
    {
        p->tick = 0;

        if (p->count < p->history_len)
        {
            p->history[p->count] = value;
            p->count++;
        }
        else
        {
            for (uint16_t i = 0; i < p->history_len - 1; i++)
                p->history[i] = p->history[i + 1];

            p->history[p->history_len - 1] = value;
        }
    }
}