#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "hardware/clocks.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "st7789.h"

#include "NEN-project/assets.h"
#include "NEN-project/HUDStuff.h"
#include "NEN-project/raycasting.h"
#include "NEN-project/utils.h"

volatile millis_t millis = 0;

// -------- Buttons and LEDs --------
#define IO_COUNT 5
static const uint8_t BUTTON_PINS[IO_COUNT] = {15, 13, 9, 5, 3};
static const uint8_t LED_PINS[IO_COUNT] = {14, 12, 8, 4, 2};
#define DEBUG_BUTTON_PIN 28

static uint8_t g_last_event_num = 0;
static bool g_last_event_step_on = false;
static bool g_have_event = false;

typedef enum
{
    DEBUG_PAGE_GAME = 0,
    DEBUG_PAGE_CORE = 1,
    DEBUG_PAGE_FPS = 2,
    DEBUG_PAGE_COUNT,
} debug_page_t;

static debug_page_t debug_page = DEBUG_PAGE_GAME;
static void pageCycle()
{
    debug_page = (debug_page_t)((debug_page + 1) % DEBUG_PAGE_COUNT);
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

static const char *build_arch_name(void)
{
#if defined(__riscv)
    return "RISCV";
#elif defined(__arm__) || defined(__thumb__)
    return "ARM";
#else
    return "UNK";
#endif
}

static void draw_debug_page_game(const player_t *camera,
                                 const map_t *map,
                                 const dialogue_t *dialogue,
                                 uint32_t buttons,
                                 bool debug_button,
                                 uint32_t fps,
                                 uint32_t frame_us)
{
    const st7789_font_t *debug_font = ST7789_FONT_DEFAULT_MONO;
    char line[20];
    int16_t x_whole;
    int16_t y_whole;
    uint8_t x_frac;
    uint8_t y_frac;

    fx_to_whole_hundredths(camera->posX, &x_whole, &x_frac);
    fx_to_whole_hundredths(camera->posY, &y_whole, &y_frac);

    st7789_draw_debug_cell_with_font(0, 0, "GAME PAGE", debug_font, rgb565(255, 230, 80));

    snprintf(line, sizeof(line), "FPS %lu", (unsigned long)fps);
    st7789_draw_debug_cell_with_font(1, 0, line, debug_font, rgb565(220, 220, 220));

    snprintf(line, sizeof(line), "F %luus", (unsigned long)frame_us);
    st7789_draw_debug_cell_with_font(2, 0, line, debug_font, rgb565(220, 220, 220));

    snprintf(line, sizeof(line), "BTN %02lX", (unsigned long)buttons);
    st7789_draw_debug_cell_with_font(0, 1, line, debug_font, rgb565(170, 240, 255));

    if (g_have_event)
        snprintf(line, sizeof(line), "EV %u%c", (unsigned)g_last_event_num, g_last_event_step_on ? '+' : '-');
    else
        snprintf(line, sizeof(line), "EV --");
    st7789_draw_debug_cell_with_font(1, 1, line, debug_font, rgb565(170, 240, 255));

    snprintf(line, sizeof(line), "D28 %u", debug_button ? 1u : 0u);
    st7789_draw_debug_cell_with_font(2, 1, line, debug_font, rgb565(255, 220, 130));

    snprintf(line, sizeof(line), "X %d.%02u", x_whole, x_frac);
    st7789_draw_debug_cell_with_font(0, 2, line, debug_font, rgb565(120, 255, 120));

    snprintf(line, sizeof(line), "Y %d.%02u", y_whole, y_frac);
    st7789_draw_debug_cell_with_font(1, 2, line, debug_font, rgb565(120, 255, 120));

    snprintf(line, sizeof(line), "A %d", camera->angle);
    st7789_draw_debug_cell_with_font(2, 2, line, debug_font, rgb565(220, 190, 255));

    snprintf(line, sizeof(line), "HP %u K %u", camera->health, camera->kills);
    st7789_draw_debug_cell_with_font(0, 3, line, debug_font, rgb565(255, 120, 120));

    snprintf(line, sizeof(line), "IT %u D %u", (unsigned)camera->currentItem, dialogue != NULL ? 1u : 0u);
    st7789_draw_debug_cell_with_font(1, 3, line, debug_font, rgb565(255, 190, 120));

    snprintf(line, sizeof(line), "M %ux%u", map->width, map->height);
    st7789_draw_debug_cell_with_font(2, 3, line, debug_font, rgb565(200, 200, 200));
}

static void draw_debug_page_core(const player_t *camera,
                                 const map_t *map,
                                 bool debug_button,
                                 uint32_t fps,
                                 uint32_t frame_us,
                                 uint64_t uptime_us)
{
    const st7789_font_t *debug_font = ST7789_FONT_DEFAULT_MONO;
    char line[20];
    uint32_t sys_mhz = clock_get_hz(clk_sys) / 1000000u;

    st7789_draw_debug_cell_with_font(0, 0, "CORE PAGE", debug_font, rgb565(255, 230, 80));

    snprintf(line, sizeof(line), "ARCH %s", build_arch_name());
    st7789_draw_debug_cell_with_font(1, 0, line, debug_font, rgb565(170, 240, 255));

    snprintf(line, sizeof(line), "CNUM %u", get_core_num());
    st7789_draw_debug_cell_with_font(2, 0, line, debug_font, rgb565(170, 240, 255));

    snprintf(line, sizeof(line), "SYS %luM", (unsigned long)sys_mhz);
    st7789_draw_debug_cell_with_font(0, 1, line, debug_font, rgb565(120, 255, 120));

    snprintf(line, sizeof(line), "UP %lus", (unsigned long)(uptime_us / 1000000ull));
    st7789_draw_debug_cell_with_font(1, 1, line, debug_font, rgb565(120, 255, 120));

    snprintf(line, sizeof(line), "DBG %u", debug_button ? 1u : 0u);
    st7789_draw_debug_cell_with_font(2, 1, line, debug_font, rgb565(255, 220, 130));

    snprintf(line, sizeof(line), "FPS %lu", (unsigned long)fps);
    st7789_draw_debug_cell_with_font(0, 2, line, debug_font, rgb565(220, 220, 220));

    snprintf(line, sizeof(line), "FR %luus", (unsigned long)frame_us);
    st7789_draw_debug_cell_with_font(1, 2, line, debug_font, rgb565(220, 220, 220));

    snprintf(line, sizeof(line), "ANG %d", camera->angle);
    st7789_draw_debug_cell_with_font(2, 2, line, debug_font, rgb565(220, 190, 255));

    if (g_have_event)
        snprintf(line, sizeof(line), "EV %u%c", (unsigned)g_last_event_num, g_last_event_step_on ? '+' : '-');
    else
        snprintf(line, sizeof(line), "EV --");
    st7789_draw_debug_cell_with_font(0, 3, line, debug_font, rgb565(255, 120, 120));

    snprintf(line, sizeof(line), "MAP %ux%u", map->width, map->height);
    st7789_draw_debug_cell_with_font(1, 3, line, debug_font, rgb565(200, 200, 200));

    snprintf(line, sizeof(line), "PG 2/%u", (unsigned)DEBUG_PAGE_COUNT);
    st7789_draw_debug_cell_with_font(2, 3, line, debug_font, rgb565(255, 190, 120));
}

plot_t plot_fps_big;
int16_t plot_fps_big_storage[198];
static const int16_t plot_fps_big_size = sizeof(plot_fps_big_storage) / sizeof(plot_fps_big_storage[0]);

plot_t plot_fpsActual_big;
int16_t plot_fpsActual_big_storage[198];
static const int16_t plot_fpsActual_big_size = sizeof(plot_fpsActual_big_storage) / sizeof(plot_fpsActual_big_storage[0]);

void draw_debug_fps(uint32_t fps, uint32_t actual_fps)
{
    st7789_fill_rect(0, 0, ST7789_WIDTH, ST7789_HEIGHT, rgb565(0, 0, 0));
    char line[20];
    const st7789_font_t *debug_font = ST7789_FONT_DEFAULT_MONO;

    snprintf(line, sizeof(line), "FPS %lu", (unsigned long)fps);
    st7789_draw_text(3, 3, 300, line, debug_font, rgb565(255, 255, 255));
    snprintf(line, sizeof(line), "ACT %lu", (unsigned long)actual_fps);
    st7789_draw_text(3, 14, 300, line, debug_font, rgb565(255, 255, 255));
    plot_draw(&plot_fps_big, true);
    plot_draw(&plot_fpsActual_big, false);
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

    st7789_init();
    st7789_fill_rect(0, 0, ST7789_WIDTH, ST7789_HEIGHT, rgb565(8, 8, 8));
    st7789_present_full();

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
    bool prev_debug_button = false;

    plot_init(&plot_fps_big, 60, 5, 200, 70, 1, 0, 5000, rgb565(0, 0, 0), rgb565(255, 255, 0), plot_fps_big_storage, plot_fps_big_size);
    plot_init(&plot_fpsActual_big, 60, 5, 200, 70, 1, 0, 100, rgb565(0, 0, 0), rgb565(255, 0, 0), plot_fpsActual_big_storage, plot_fpsActual_big_size);

    while (true)
    {
        static absolute_time_t last_frame_complete = {0};
        absolute_time_t frame_start = get_absolute_time();
        uint64_t frame_start_us = to_us_since_boot(frame_start);
        millis = to_ms_since_boot(frame_start);

        uint32_t button_mask = read_buttons_mask();

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
        if (use_pressed && !dialogue_active)
            use_press_ms = millis;

        HUD_DrawItemPOV(&camera, use_press_ms + 200 > millis);
        HUD_DrawDialogue(&current_dialogue, use_pressed && dialogue_active);

        set_LEDs(HUD_GetLEDHP(&camera));

        absolute_time_t frame_end = get_absolute_time();
        uint64_t frame_end_us = to_us_since_boot(frame_end);
        uint32_t frame_len_us = (uint32_t)(frame_end_us - frame_start_us);
        uint32_t frame_len_actual_us = (uint32_t)(frame_end_us - to_us_since_boot(last_frame_complete));
        last_frame_complete = get_absolute_time();
        if (frame_len_us == 0)
            frame_len_us = 1;
        if (frame_len_actual_us == 0)
            frame_len_actual_us = 1;
        uint32_t fps = (1000000u + (frame_len_us / 2u)) / frame_len_us;
        uint32_t actual_fps = (1000000u + (frame_len_actual_us / 2u)) / frame_len_actual_us;
        plot_add(&plot_fps_big, (int16_t)fps);
        plot_add(&plot_fpsActual_big, (int16_t)actual_fps);

        millis = to_ms_since_boot(frame_end);

        // Toggle debug page on GP28 rising edge
        bool debug_button = read_debug_button();
        if (debug_button && !prev_debug_button)
        {
            pageCycle();
        }
        prev_debug_button = debug_button;

        switch(debug_page)
        {
            case DEBUG_PAGE_GAME:
                draw_debug_page_game(&camera, current_map, current_dialogue, button_mask, debug_button, fps, frame_len_us);
                break;
            case DEBUG_PAGE_CORE:
                draw_debug_page_core(&camera, current_map, debug_button, fps, frame_len_us, to_us_since_boot(get_absolute_time()));
                break;
            case DEBUG_PAGE_FPS:
                draw_debug_fps(fps, actual_fps);
                break;
            default:
                break;
        }

        dogm128_refresh();
        st7789_present_full();

        // sleep_ms(1);
    }
}
