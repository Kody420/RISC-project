#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hardware/clocks.h"
#include "pico/critical_section.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "st7789.h"
#include "MovementRecorder.h"

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
    buttons.front = (mask & (1u << 1)) != 0;
    buttons.back = (mask & (1u << 0)) != 0;
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

typedef struct
{
    fx_t posX;
    fx_t posY;
    fx_t angle;
    uint8_t health;
    uint8_t kills;
    item_t currentItem;
} debug_camera_snapshot_t;

typedef struct
{
    debug_page_t page;
    debug_camera_snapshot_t camera;
    uint8_t map_width;
    uint8_t map_height;
    uint8_t dialogue_active;
    uint8_t debug_button;
    uint8_t have_event;
    uint8_t last_event_num;
    uint8_t last_event_step_on;
    uint32_t buttons;
    uint32_t fps;
    uint32_t actual_fps;
    uint32_t frame_us;
    uint64_t uptime_us;
    movement_recorder_status_t movement_recorder_status;
} debug_display_snapshot_t;

static critical_section_t g_debug_snapshot_lock;
static volatile uint32_t g_debug_snapshot_version = 0;
static debug_display_snapshot_t g_debug_snapshot;

static void publish_debug_snapshot(debug_page_t page,
                                   const player_t *camera,
                                   const map_t *map,
                                   bool dialogue_active,
                                   uint32_t buttons,
                                   bool debug_button,
                                   uint32_t fps,
                                   uint32_t actual_fps,
                                   uint32_t frame_us,
                                   uint64_t uptime_us,
                                   movement_recorder_status_t movement_recorder_status)
{
    critical_section_enter_blocking(&g_debug_snapshot_lock);

    g_debug_snapshot.page = page;
    g_debug_snapshot.camera.posX = camera->posX;
    g_debug_snapshot.camera.posY = camera->posY;
    g_debug_snapshot.camera.angle = camera->angle;
    g_debug_snapshot.camera.health = camera->health;
    g_debug_snapshot.camera.kills = camera->kills;
    g_debug_snapshot.camera.currentItem = camera->currentItem;
    g_debug_snapshot.map_width = map->width;
    g_debug_snapshot.map_height = map->height;
    g_debug_snapshot.dialogue_active = dialogue_active ? 1u : 0u;
    g_debug_snapshot.debug_button = debug_button ? 1u : 0u;
    g_debug_snapshot.have_event = g_have_event ? 1u : 0u;
    g_debug_snapshot.last_event_num = g_last_event_num;
    g_debug_snapshot.last_event_step_on = g_last_event_step_on ? 1u : 0u;
    g_debug_snapshot.buttons = buttons;
    g_debug_snapshot.fps = fps;
    g_debug_snapshot.actual_fps = actual_fps;
    g_debug_snapshot.frame_us = frame_us;
    g_debug_snapshot.uptime_us = uptime_us;
    g_debug_snapshot.movement_recorder_status = movement_recorder_status;
    ++g_debug_snapshot_version;

    critical_section_exit(&g_debug_snapshot_lock);
}

static void draw_debug_page_game_snapshot(const debug_display_snapshot_t *snapshot)
{
    const st7789_font_t *debug_font = ST7789_FONT_DEFAULT_MONO;
    char line[20];
    int16_t x_whole;
    int16_t y_whole;
    uint8_t x_frac;
    uint8_t y_frac;

    fx_to_whole_hundredths(snapshot->camera.posX, &x_whole, &x_frac);
    fx_to_whole_hundredths(snapshot->camera.posY, &y_whole, &y_frac);

    st7789_draw_debug_cell_with_font(0, 0, "GAME PAGE", debug_font, rgb565(255, 230, 80));

    snprintf(line, sizeof(line), "FPS %lu", (unsigned long)snapshot->fps);
    st7789_draw_debug_cell_with_font(1, 0, line, debug_font, rgb565(220, 220, 220));

    snprintf(line, sizeof(line), "F %luus", (unsigned long)snapshot->frame_us);
    st7789_draw_debug_cell_with_font(2, 0, line, debug_font, rgb565(220, 220, 220));

    snprintf(line, sizeof(line), "BTN %02lX", (unsigned long)snapshot->buttons);
    st7789_draw_debug_cell_with_font(0, 1, line, debug_font, rgb565(170, 240, 255));

    if (snapshot->have_event)
        snprintf(line, sizeof(line), "EV %u%c", (unsigned)snapshot->last_event_num, snapshot->last_event_step_on ? '+' : '-');
    else
        snprintf(line, sizeof(line), "EV --");
    st7789_draw_debug_cell_with_font(1, 1, line, debug_font, rgb565(170, 240, 255));

    snprintf(line, sizeof(line), "D28 %u", (unsigned)snapshot->debug_button);
    st7789_draw_debug_cell_with_font(2, 1, line, debug_font, rgb565(255, 220, 130));

    snprintf(line, sizeof(line), "X %d.%02u", x_whole, x_frac);
    st7789_draw_debug_cell_with_font(0, 2, line, debug_font, rgb565(120, 255, 120));

    snprintf(line, sizeof(line), "Y %d.%02u", y_whole, y_frac);
    st7789_draw_debug_cell_with_font(1, 2, line, debug_font, rgb565(120, 255, 120));

    snprintf(line, sizeof(line), "A %d", snapshot->camera.angle);
    st7789_draw_debug_cell_with_font(2, 2, line, debug_font, rgb565(220, 190, 255));

    snprintf(line, sizeof(line), "HP %u K %u", snapshot->camera.health, snapshot->camera.kills);
    st7789_draw_debug_cell_with_font(0, 3, line, debug_font, rgb565(255, 120, 120));

    snprintf(line, sizeof(line), "IT %u D %u", (unsigned)snapshot->camera.currentItem, (unsigned)snapshot->dialogue_active);
    st7789_draw_debug_cell_with_font(1, 3, line, debug_font, rgb565(255, 190, 120));

    snprintf(line, sizeof(line), "M %ux%u", snapshot->map_width, snapshot->map_height);
    st7789_draw_debug_cell_with_font(2, 3, line, debug_font, rgb565(200, 200, 200));
}

static void draw_debug_page_core_snapshot(const debug_display_snapshot_t *snapshot)
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

    snprintf(line, sizeof(line), "UP %lus", (unsigned long)(snapshot->uptime_us / 1000000ull));
    st7789_draw_debug_cell_with_font(1, 1, line, debug_font, rgb565(120, 255, 120));

    snprintf(line, sizeof(line), "DBG %u", (unsigned)snapshot->debug_button);
    st7789_draw_debug_cell_with_font(2, 1, line, debug_font, rgb565(255, 220, 130));

    snprintf(line, sizeof(line), "FPS %lu", (unsigned long)snapshot->fps);
    st7789_draw_debug_cell_with_font(0, 2, line, debug_font, rgb565(220, 220, 220));

    snprintf(line, sizeof(line), "FR %luus", (unsigned long)snapshot->frame_us);
    st7789_draw_debug_cell_with_font(1, 2, line, debug_font, rgb565(220, 220, 220));

    snprintf(line, sizeof(line), "ANG %d", snapshot->camera.angle);
    st7789_draw_debug_cell_with_font(2, 2, line, debug_font, rgb565(220, 190, 255));

    if (snapshot->have_event)
        snprintf(line, sizeof(line), "EV %u%c", (unsigned)snapshot->last_event_num, snapshot->last_event_step_on ? '+' : '-');
    else
        snprintf(line, sizeof(line), "EV --");
    st7789_draw_debug_cell_with_font(0, 3, line, debug_font, rgb565(255, 120, 120));

    snprintf(line, sizeof(line), "MAP %ux%u", snapshot->map_width, snapshot->map_height);
    st7789_draw_debug_cell_with_font(1, 3, line, debug_font, rgb565(200, 200, 200));

    snprintf(line, sizeof(line), "PG 2/%u", (unsigned)DEBUG_PAGE_COUNT);
    st7789_draw_debug_cell_with_font(2, 3, line, debug_font, rgb565(255, 190, 120));
}

static void draw_debug_fps_snapshot(const debug_display_snapshot_t *snapshot)
{
    char line[20];
    const st7789_font_t *debug_font = ST7789_FONT_DEFAULT_MONO;

    st7789_fill_rect(0, 0, ST7789_WIDTH, ST7789_HEIGHT, rgb565(0, 0, 0));
    snprintf(line, sizeof(line), "FPS %lu", (unsigned long)snapshot->fps);
    st7789_draw_text(3, 3, 300, line, debug_font, rgb565(255, 255, 255));
    snprintf(line, sizeof(line), "ACT %lu", (unsigned long)snapshot->actual_fps);
    st7789_draw_text(3, 14, 300, line, debug_font, rgb565(255, 255, 255));
}

static void core1_debug_display_main(void)
{
    uint32_t last_seen_version = 0;
    debug_display_snapshot_t snapshot;
    plot_t fps_plot;
    plot_t actual_fps_plot;
    int16_t fps_history[198];
    int16_t actual_fps_history[198];

    plot_init(&fps_plot, 60, 5, 200, 70, 1, 0, 5000, rgb565(0, 0, 0), rgb565(255, 255, 0), fps_history, (uint16_t)(sizeof(fps_history) / sizeof(fps_history[0])));
    plot_init(&actual_fps_plot, 60, 5, 200, 70, 1, 0, 200, rgb565(0, 0, 0), rgb565(255, 0, 0), actual_fps_history, (uint16_t)(sizeof(actual_fps_history) / sizeof(actual_fps_history[0])));

    while (true)
    {
        uint32_t version;

        critical_section_enter_blocking(&g_debug_snapshot_lock);
        version = g_debug_snapshot_version;
        if (version != last_seen_version)
            snapshot = g_debug_snapshot;
        critical_section_exit(&g_debug_snapshot_lock);

        if (version == last_seen_version)
        {
            tight_loop_contents();
            continue;
        }

        last_seen_version = version;
        plot_add(&fps_plot, (int16_t)snapshot.fps);
        plot_add(&actual_fps_plot, (int16_t)snapshot.actual_fps);

        st7789_fill_rect(0, 0, ST7789_WIDTH, ST7789_HEIGHT, rgb565(0, 0, 0));

        switch (snapshot.page)
        {
            case DEBUG_PAGE_GAME:
                draw_debug_page_game_snapshot(&snapshot);
                break;
            case DEBUG_PAGE_CORE:
                draw_debug_page_core_snapshot(&snapshot);
                break;
            case DEBUG_PAGE_FPS:
                draw_debug_fps_snapshot(&snapshot);
                plot_draw(&fps_plot, true);
                plot_draw(&actual_fps_plot, false);
                break;
            default:
                break;
        }

        switch (snapshot.movement_recorder_status)
        {
        case MOVEMENT_RECORDER_STATUS_IDLE:
            st7789_fill_rect(ST7789_WIDTH - 20, ST7789_HEIGHT - 20, ST7789_WIDTH, ST7789_HEIGHT, rgb565(10, 10, 10));
            break;
        case MOVEMENT_RECORDER_STATUS_RECORDING:
            st7789_fill_rect(ST7789_WIDTH - 20, ST7789_HEIGHT - 20, ST7789_WIDTH, ST7789_HEIGHT, rgb565(255, 0, 0));
            break;
        case MOVEMENT_RECORDER_STATUS_REPLAYING:
            st7789_fill_rect(ST7789_WIDTH - 20, ST7789_HEIGHT - 20, ST7789_WIDTH, ST7789_HEIGHT, rgb565(0, 255, 0));
            break;
        default:
            break;
        }

        st7789_present_full();
    }
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
    critical_section_init(&g_debug_snapshot_lock);

    st7789_init();
    st7789_fill_rect(0, 0, ST7789_WIDTH, ST7789_HEIGHT, rgb565(8, 8, 8));
    st7789_present_full();

    dogm128_init();
    dogm128_contrast(0x8F);

    MovementRecorder_Init();

    map_t *current_map = &WallDemoMap;
    const dialogue_t *current_dialogue = NULL;
    millis_t use_press_ms = 0;
    static entity_t entities[MAX_ENTITIES];

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

entities[0].posX = FX(5);
    entities[0].posY = FX(5);
    entities[0].health = 100;
    entities[0].sprite = &blobSprite;
    entities[0].ratio = 0x00c0;
    entities[0].heightOffset = FX(1);
    entities[0].walking = 1;
    entities[0].movementModifier = FX(1);
    entities[0].lateralModifier = FX(1);
    entities[0].hitDistance = FX(3);
   
    entities[1].posX = FX(8);
    entities[1].posY = FX(8);
    entities[1].health = 100;
    entities[1].sprite = &ctyrruckaSprite;
    entities[1].ratio = FX(1);
    entities[1].heightOffset = FX(0);
    entities[1].walking = 1;
    entities[1].movementModifier = FX(2);  // 1.5625
    entities[1].lateralModifier = FX(1);
    entities[1].hitDistance = FX(4);

    entities[2].posX = FX(3);
    entities[2].posY = FX(3);
    entities[2].health = 100;
    entities[2].sprite = &chapadloSprite;
    entities[2].ratio = 0X00C0;
    entities[2].heightOffset = FX(0);
    entities[2].walking = 1;
    entities[2].movementModifier = 0X01C0;
    entities[2].lateralModifier = FX(1);
    entities[2].hitDistance = FX(6);

    entities[3].posX = FX(6);
    entities[3].posY = FX(2);
    entities[3].health = 100;
    entities[3].sprite = &soilderSprite;
    entities[3].ratio = 0X0120;
    entities[3].heightOffset = FX(0);
    entities[3].walking = 1;
    entities[3].movementModifier = FX(3);
    entities[3].lateralModifier = FX(1);
    entities[3].hitDistance = FX(20);
    
    entities[4].posX = FX(7);
    entities[4].posY = FX(9);
    entities[4].health = 100;
    entities[4].sprite = &ctyrruckaSprite;
    entities[4].ratio = FX(1);
    entities[4].heightOffset = FX(0);
    entities[4].walking = 0;

    bool prev_use = false;
    bool prev_debug_button = false;
    publish_debug_snapshot(debug_page, &camera, current_map, false, 0, false, 0, 0, 0, to_us_since_boot(get_absolute_time()), MovementRecorder_GetStatus());
    multicore_launch_core1(core1_debug_display_main);

    while (true)
    {
        static absolute_time_t last_frame_complete = {0};
        absolute_time_t frame_start = get_absolute_time();
        uint64_t frame_start_us = to_us_since_boot(frame_start);
        millis = to_ms_since_boot(frame_start);

        uint32_t button_mask = read_buttons_mask();

        buttons_t buttons = buttons_from_mask(button_mask);
        MovementRecorder_CurrentValues(buttons);
        if (MovementRecorder_GetStatus() == MOVEMENT_RECORDER_STATUS_REPLAYING)
            buttons = MovementRecorder_GetPlaybackValues();

        MoveCamera(&camera, current_map, buttons, &current_dialogue);

        dogm128_clear();
        RenderFrame(&camera, current_map);
        DrawEntities(&camera, entities, MAX_ENTITIES, dogm_fb, buttons);
        EnemyAi(&camera, entities, MAX_ENTITIES, current_map);

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

        millis = to_ms_since_boot(frame_end);




        // Toggle debug page on GP28 rising edge and also do button recording
        bool debug_button = read_debug_button();
        static millis_t last_debug_button_press_ms = 0;
        if (debug_button && !prev_debug_button)
            last_debug_button_press_ms = millis;
        if (!debug_button && prev_debug_button && (millis - last_debug_button_press_ms < 500))
        {
            pageCycle();
            last_debug_button_press_ms = millis;
        }
        if (millis - last_debug_button_press_ms >= 500 && debug_button)
        {
            switch(MovementRecorder_GetStatus())
            {
                case MOVEMENT_RECORDER_STATUS_IDLE:
                    MovementRecorder_Init();
                    MovementRecorder_StartRecording();
                    break;
                case MOVEMENT_RECORDER_STATUS_RECORDING:
                    MovementRecorder_StartReplay();
                    break;
                case MOVEMENT_RECORDER_STATUS_REPLAYING:
                    MovementRecorder_StopReplay();
                    break;
            }
            last_debug_button_press_ms = millis;
        }
        prev_debug_button = debug_button;

        publish_debug_snapshot(debug_page,
                               &camera,
                               current_map,
                               dialogue_active,
                               button_mask,
                               debug_button,
                               fps,
                               actual_fps,
                               frame_len_us,
                               to_us_since_boot(get_absolute_time()),
                               MovementRecorder_GetStatus());

        dogm128_refresh();

        // sleep_ms(1);
    }
}
