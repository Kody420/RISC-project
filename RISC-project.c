#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "hardware/spi.h"
#include "pico/stdlib.h"

#include "third_party/NEN-project/assets.h"
#include "third_party/NEN-project/HUDStuff.h"
#include "third_party/NEN-project/raycasting.h"
#include "third_party/NEN-project/utils.h"

volatile millis_t millis = 0;

// -------- Buttons and LEDs --------
#define IO_COUNT 5
static const uint8_t BUTTON_PINS[IO_COUNT] = {15, 13, 9, 5, 3};
static const uint8_t LED_PINS[IO_COUNT] = {14, 12, 8, 4, 2};
#define DEBUG_BUTTON_PIN 28

// -------- ST7789 debug display (SPI0) --------
#define ST7789_SPI spi0
#define ST7789_SCK_PIN 18
#define ST7789_MOSI_PIN 19
#define ST7789_CS_PIN 21
#define ST7789_DC_PIN 20
#define ST7789_RST_PIN 17
#define ST7789_SPI_BAUDRATE (24 * 1000 * 1000)

#define ST7789_WIDTH 284
#define ST7789_HEIGHT 76
#define ST7789_XSTART 18
#define ST7789_YSTART 82
#define ST7789_MADCTL 0x60

static uint8_t g_last_event_num = 0;
static bool g_last_event_step_on = false;
static bool g_have_event = false;

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

static void io_init(void)
{
    for (size_t i = 0; i < IO_COUNT; ++i)
    {
        gpio_init(BUTTON_PINS[i]);
        gpio_set_dir(BUTTON_PINS[i], GPIO_IN);
        gpio_pull_up(BUTTON_PINS[i]);

        gpio_init(LED_PINS[i]);
        gpio_set_dir(LED_PINS[i], GPIO_OUT);
        gpio_put(LED_PINS[i], false);
    }

    gpio_init(DEBUG_BUTTON_PIN);
    gpio_set_dir(DEBUG_BUTTON_PIN, GPIO_IN);
    gpio_pull_up(DEBUG_BUTTON_PIN);
}

static uint32_t read_buttons_mask(void)
{
    uint32_t mask = 0;
    for (size_t i = 0; i < IO_COUNT; ++i)
    {
        if (!gpio_get(BUTTON_PINS[i]))
            mask |= (1u << i);
    }
    return mask;
}

static bool read_debug_button(void)
{
    return !gpio_get(DEBUG_BUTTON_PIN);
}

static buttons_t buttons_from_mask(uint32_t mask)
{
    buttons_t buttons = {0};
    buttons.front = (mask & (1u << 0)) != 0;
    buttons.back = (mask & (1u << 1)) != 0;
    buttons.use = (mask & (1u << 2)) != 0;
    buttons.left = (mask & (1u << 3)) != 0;
    buttons.right = (mask & (1u << 4)) != 0;
    return buttons;
}

static void set_LED(uint8_t button, bool state)
{
    if (button < 1 || button > IO_COUNT)
        return;
    gpio_put(LED_PINS[button - 1], state);
}

static void set_LEDs(uint8_t state)
{
    for (size_t i = 0; i < IO_COUNT; ++i)
        set_LED((uint8_t)(i + 1), (state & (1u << i)) != 0);
}

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

static void st7789_write_repeated_color(uint16_t color, uint32_t count)
{
    uint8_t buf[128];
    for (size_t i = 0; i < sizeof(buf); i += 2)
    {
        buf[i] = (uint8_t)(color >> 8);
        buf[i + 1] = (uint8_t)color;
    }

    st7789_select();
    st7789_dc_data();
    while (count > 0)
    {
        uint32_t chunk_pixels = count > (sizeof(buf) / 2u) ? (sizeof(buf) / 2u) : count;
        spi_write_blocking(ST7789_SPI, buf, chunk_pixels * 2u);
        count -= chunk_pixels;
    }
    st7789_deselect();
}

static void st7789_fill_rect(uint16_t x, uint16_t y, uint16_t w, uint16_t h, uint16_t color)
{
    if (x >= ST7789_WIDTH || y >= ST7789_HEIGHT || w == 0 || h == 0)
        return;

    uint16_t x_end = (uint16_t)(x + w - 1);
    uint16_t y_end = (uint16_t)(y + h - 1);
    if (x_end >= ST7789_WIDTH)
        x_end = ST7789_WIDTH - 1;
    if (y_end >= ST7789_HEIGHT)
        y_end = ST7789_HEIGHT - 1;

    st7789_set_window(x, y, x_end, y_end);
    st7789_write_repeated_color(color, (uint32_t)(x_end - x + 1) * (uint32_t)(y_end - y + 1));
}

static void st7789_init(void)
{
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

    st7789_write_cmd(0x21);
    st7789_write_cmd(0x13);
    st7789_write_cmd(0x29);
    sleep_ms(20);
}

static const uint8_t debug_font3x5[][3] = {
    {0x00, 0x00, 0x00}, {0x00, 0x17, 0x00}, {0x03, 0x00, 0x03}, {0x1F, 0x0A, 0x1F}, {0x12, 0x1F, 0x09},
    {0x19, 0x04, 0x13}, {0x0A, 0x15, 0x1A}, {0x00, 0x03, 0x00}, {0x0E, 0x11, 0x00}, {0x00, 0x11, 0x0E},
    {0x05, 0x02, 0x05}, {0x04, 0x0E, 0x04}, {0x10, 0x08, 0x00}, {0x04, 0x04, 0x04}, {0x00, 0x10, 0x00},
    {0x18, 0x04, 0x03}, {0x1F, 0x11, 0x1F}, {0x12, 0x1F, 0x10}, {0x1D, 0x15, 0x17}, {0x11, 0x15, 0x1F},
    {0x07, 0x04, 0x1F}, {0x17, 0x15, 0x1D}, {0x1F, 0x15, 0x1D}, {0x01, 0x01, 0x1F}, {0x1F, 0x15, 0x1F},
    {0x17, 0x15, 0x1F}, {0x00, 0x0A, 0x00}, {0x00, 0x14, 0x00}, {0x04, 0x0A, 0x11}, {0x0A, 0x0A, 0x0A},
    {0x11, 0x0A, 0x04}, {0x01, 0x15, 0x03}, {0x0E, 0x15, 0x16}, {0x1E, 0x05, 0x1E}, {0x1F, 0x15, 0x0A},
    {0x0E, 0x11, 0x11}, {0x1F, 0x11, 0x0E}, {0x1F, 0x15, 0x15}, {0x1F, 0x05, 0x05}, {0x0E, 0x11, 0x1D},
    {0x1F, 0x04, 0x1F}, {0x11, 0x1F, 0x11}, {0x08, 0x10, 0x0F}, {0x1F, 0x04, 0x1B}, {0x1F, 0x10, 0x10},
    {0x1F, 0x06, 0x1F}, {0x1F, 0x0E, 0x1F}, {0x0E, 0x11, 0x0E}, {0x1F, 0x05, 0x02}, {0x0E, 0x19, 0x1E},
    {0x1F, 0x0D, 0x16}, {0x12, 0x15, 0x09}, {0x01, 0x1F, 0x01}, {0x0F, 0x10, 0x0F}, {0x07, 0x18, 0x07},
    {0x1F, 0x0C, 0x1F}, {0x1B, 0x04, 0x1B}, {0x03, 0x1C, 0x03}, {0x19, 0x15, 0x13}, {0x00, 0x1F, 0x11},
    {0x03, 0x04, 0x18}, {0x11, 0x1F, 0x00}, {0x02, 0x01, 0x02}, {0x10, 0x10, 0x10}, {0x01, 0x02, 0x00}};

#define DBG_FONT_SCALE 2
#define DBG_CHAR_W (3 * DBG_FONT_SCALE)
#define DBG_CHAR_H (5 * DBG_FONT_SCALE)
#define DBG_CHAR_ADV (4 * DBG_FONT_SCALE)

#define DBG_GRID_COLS 3
#define DBG_GRID_ROWS 4
#define DBG_GRID_MARGIN_X 2
#define DBG_GRID_MARGIN_Y 2
#define DBG_GRID_GAP_X 2
#define DBG_GRID_GAP_Y 2

static void st7789_draw_char(uint16_t x, uint16_t y, char c, uint16_t color)
{
    if ((uint8_t)c >= 'a' && (uint8_t)c <= 'z')
        c = (char)(c - 32);
    if ((uint8_t)c < 32 || (uint8_t)c > 96)
        c = 32;

    const uint8_t *col_data = debug_font3x5[(uint8_t)c - 32];

    for (uint8_t col = 0; col < 3; ++col)
    {
        for (uint8_t row = 0; row < 5; ++row)
        {
            if (col_data[col] & (1u << row))
            {
                st7789_fill_rect((uint16_t)(x + (uint16_t)(col * DBG_FONT_SCALE)),
                                 (uint16_t)(y + (uint16_t)(row * DBG_FONT_SCALE)),
                                 DBG_FONT_SCALE,
                                 DBG_FONT_SCALE,
                                 color);
            }
        }
    }
}

static void st7789_draw_text(uint16_t x, uint16_t y, uint16_t max_x, const char *text, uint16_t color)
{
    while (*text)
    {
        if ((uint16_t)(x + DBG_CHAR_W) > max_x)
            break;

        st7789_draw_char(x, y, *text++, color);

        x = (uint16_t)(x + DBG_CHAR_ADV);
        if (x >= ST7789_WIDTH)
            break;
    }
}

static void fx_to_whole_hundredths(fx_t v, int16_t *whole, uint8_t *hundredths)
{
    int16_t w = FX_TO_INT(v);
    fx_t frac = fx_sub(v, FX_FROM_INT(w));
    if (frac < 0)
        frac = fx_neg(frac);

    *whole = w;
    *hundredths = (uint8_t)(((uint16_t)frac * 100u) >> 8);
}

static void st7789_draw_debug_cell(uint8_t col, uint8_t row, const char *text, uint16_t text_color)
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
                     text_color);
}

static void draw_debug_screen(const player_t *camera,
                              const map_t *map,
                              const dialogue_t *dialogue,
                              uint32_t buttons,
                              bool debug_button,
                              uint32_t fps,
                              uint32_t frame_ms)
{
    char line[20];
    int16_t x_whole;
    int16_t y_whole;
    uint8_t x_frac;
    uint8_t y_frac;

    fx_to_whole_hundredths(camera->posX, &x_whole, &x_frac);
    fx_to_whole_hundredths(camera->posY, &y_whole, &y_frac);

    st7789_draw_debug_cell(0, 0, "NEN DBG", rgb565(255, 230, 80));

    snprintf(line, sizeof(line), "FPS %lu", (unsigned long)fps);
    st7789_draw_debug_cell(1, 0, line, rgb565(220, 220, 220));

    snprintf(line, sizeof(line), "F %lums", (unsigned long)frame_ms);
    st7789_draw_debug_cell(2, 0, line, rgb565(220, 220, 220));

    snprintf(line, sizeof(line), "BTN %02lX", (unsigned long)buttons);
    st7789_draw_debug_cell(0, 1, line, rgb565(170, 240, 255));

    if (g_have_event)
        snprintf(line, sizeof(line), "EV %u%c", (unsigned)g_last_event_num, g_last_event_step_on ? '+' : '-');
    else
        snprintf(line, sizeof(line), "EV --");
    st7789_draw_debug_cell(1, 1, line, rgb565(170, 240, 255));

    snprintf(line, sizeof(line), "D28 %u", debug_button ? 1u : 0u);
    st7789_draw_debug_cell(2, 1, line, rgb565(255, 220, 130));

    snprintf(line, sizeof(line), "X %d.%02u", x_whole, x_frac);
    st7789_draw_debug_cell(0, 2, line, rgb565(120, 255, 120));

    snprintf(line, sizeof(line), "Y %d.%02u", y_whole, y_frac);
    st7789_draw_debug_cell(1, 2, line, rgb565(120, 255, 120));

    snprintf(line, sizeof(line), "A %d", camera->angle);
    st7789_draw_debug_cell(2, 2, line, rgb565(220, 190, 255));

    snprintf(line, sizeof(line), "HP %u K %u", camera->health, camera->kills);
    st7789_draw_debug_cell(0, 3, line, rgb565(255, 120, 120));

    snprintf(line, sizeof(line), "IT %u D %u", (unsigned)camera->currentItem, dialogue != NULL ? 1u : 0u);
    st7789_draw_debug_cell(1, 3, line, rgb565(255, 190, 120));

    snprintf(line, sizeof(line), "M %ux%u", map->width, map->height);
    st7789_draw_debug_cell(2, 3, line, rgb565(200, 200, 200));
}

static void on_map_event(uint8_t param1, uint8_t param2)
{
    g_last_event_num = param1;
    g_last_event_step_on = param2 != 0;
    g_have_event = true;
}

int main(void)
{
    stdio_init_all();
    io_init();

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

    st7789_init();
    st7789_fill_rect(0, 0, ST7789_WIDTH, ST7789_HEIGHT, rgb565(8, 8, 8));

    dogm128_init();
    dogm128_contrast(0x8F);

    map_t *current_map = &WallDemoMap;
    const dialogue_t *current_dialogue = NULL;
    millis_t use_press_ms = 0;

    player_t camera;
    camera.posX = FX(current_map->DefaultSpwanPoint[0]);
    camera.posY = FX(current_map->DefaultSpwanPoint[1]);
    camera.angle = FX_ANGLE_HALF;
    camera.dirX = fx_cos(camera.angle);
    camera.dirY = fx_sin(camera.angle);
    camera.planeX = fx_mul(camera.dirY, (fx_t)0x00a9);
    camera.planeY = fx_neg(fx_mul(camera.dirX, (fx_t)0x00a9));
    camera.health = 5;
    camera.kills = 0;
    camera.currentItem = ITEM_HAND;

    MapEventCallback = on_map_event;

    bool prev_use = false;
    uint32_t last_debug_draw_ms = 0;

    while (true)
    {
        uint32_t frame_start_ms = to_ms_since_boot(get_absolute_time());
        millis = frame_start_ms;

        uint32_t button_mask = read_buttons_mask();
        bool debug_button = read_debug_button();
        buttons_t buttons = buttons_from_mask(button_mask);

        MoveCamera(&camera, current_map, buttons, &current_dialogue);

        dogm128_clear();
        RenderFrame(&camera, current_map);

        HUD_DrawBanner(current_map->Banner);
        HUD_DrawBorders();
        HUD_DrawItem(camera.currentItem);
        HUD_DrawMap(current_map, &camera);
        HUD_DrawCompass(&camera);
        HUD_DrawStats(&camera);

        bool use_pressed = buttons.use && !prev_use;
        prev_use = buttons.use;

        bool dialogue_active = (current_dialogue != NULL);
        HUD_DrawDialogue(&current_dialogue, use_pressed && dialogue_active);
        if (use_pressed && !dialogue_active)
            use_press_ms = millis;

        HUD_DrawItemPOV(&camera, use_press_ms + 200 > millis);
        dogm128_refresh();

        set_LEDs(HUD_GetLEDHP(&camera));

        uint32_t frame_end_ms = to_ms_since_boot(get_absolute_time());
        uint32_t frame_len_ms = frame_end_ms - frame_start_ms;
        if (frame_len_ms == 0)
            frame_len_ms = 1;
        uint32_t fps = 1000u / frame_len_ms;

        millis = frame_end_ms;

        if (frame_end_ms - last_debug_draw_ms >= 120u)
        {
            draw_debug_screen(&camera, current_map, current_dialogue, button_mask, debug_button, fps, frame_len_ms);
            last_debug_draw_ms = frame_end_ms;
        }

        sleep_ms(1);
    }
}
