#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/watchdog.h"
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
#define OLED_PRESENT_INTERVAL_US (1000000u / 60u)
#define GAME_UPDATE_INTERVAL_US (1000000u / 173u)
#define BENCH_MAX_VALID_FRAME_US 50000u
#define BENCH_FPS_GRAPH_MAX 6000
#define BENCH_SYNTHETIC_DURATION_US 15000000ull
#define BENCH_MATH_CHUNK_OPS 100000u
#define BENCH_LINE_CHUNK_OPS 1000u

typedef enum
{
    BENCH_TEST_PLAYBACK = 0,
    BENCH_TEST_ADD_SUB,
    BENCH_TEST_MUL,
    BENCH_TEST_XOR_SHIFT,
    BENCH_TEST_LINES,
    BENCH_TEST_COUNT,
} benchmark_test_t;

static uint8_t g_last_event_num = 0;
static bool g_last_event_step_on = false;
static bool g_have_event = false;

static player_t camera;
static entity_t entities[MAX_ENTITIES];
static map_t *current_map = &Level0Map;
static const dialogue_t *current_dialogue = NULL;
static millis_t use_press_ms = 0;
static bool show_fps = false;
static uint8_t current_level_num = 0;
static bool benchmark_menu_open = true;
static benchmark_test_t selected_benchmark_test = BENCH_TEST_ADD_SUB;

typedef enum
{
    DEBUG_PAGE_STATUS = 0,
    DEBUG_PAGE_FPS = 1,
    DEBUG_PAGE_BENCH_GRAPH = 2,
    DEBUG_PAGE_BENCH_SUMMARY = 3,
    DEBUG_PAGE_COUNT,
} debug_page_t;

static debug_page_t debug_page = DEBUG_PAGE_STATUS;
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

static const char *benchmark_test_name(benchmark_test_t test)
{
    switch (test)
    {
    case BENCH_TEST_PLAYBACK:
        return "GAME";
    case BENCH_TEST_ADD_SUB:
        return "ADD";
    case BENCH_TEST_MUL:
        return "MUL";
    case BENCH_TEST_XOR_SHIFT:
        return "XOR";
    case BENCH_TEST_LINES:
        return "LINE";
    default:
        return "UNK";
    }
}

static const char *benchmark_test_detail(benchmark_test_t test)
{
    switch (test)
    {
    case BENCH_TEST_PLAYBACK:
        return "REC / REPLAY";
    case BENCH_TEST_ADD_SUB:
        return "ADD + SUB";
    case BENCH_TEST_MUL:
        return "INTEGER MUL";
    case BENCH_TEST_XOR_SHIFT:
        return "XORSHIFT";
    case BENCH_TEST_LINES:
        return "RANDOM LINES";
    default:
        return "";
    }
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
    uint8_t active;
    uint8_t complete;
    benchmark_test_t test;
    uint32_t run_id;
    uint32_t progress_permille;
    uint32_t recorded_samples;
    uint32_t work_done;
    uint32_t work_total;
    uint32_t checksum;
    uint32_t frames;
    uint32_t elapsed_ms;
    uint32_t avg_fps;
    uint32_t avg_frame_us;
    uint32_t min_frame_us;
    uint32_t max_frame_us;
    uint32_t frames_over_16ms;
    uint32_t frames_over_33ms;
} benchmark_snapshot_t;

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
    benchmark_snapshot_t benchmark;
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
                                   movement_recorder_status_t movement_recorder_status,
                                   const benchmark_snapshot_t *benchmark)
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
    if (benchmark)
        g_debug_snapshot.benchmark = *benchmark;
    else
        memset(&g_debug_snapshot.benchmark, 0, sizeof(g_debug_snapshot.benchmark));
    ++g_debug_snapshot_version;

    critical_section_exit(&g_debug_snapshot_lock);
}

static void draw_debug_status_snapshot(const debug_display_snapshot_t *snapshot)
{
    const st7789_font_t *debug_font = ST7789_FONT_DEFAULT_MONO;
    char line[20];
    int16_t x_whole;
    int16_t y_whole;
    uint8_t x_frac;
    uint8_t y_frac;

    fx_to_whole_hundredths(snapshot->camera.posX, &x_whole, &x_frac);
    fx_to_whole_hundredths(snapshot->camera.posY, &y_whole, &y_frac);

    snprintf(line, sizeof(line), "%s %luM", build_arch_name(), (unsigned long)(clock_get_hz(clk_sys) / 1000000u));
    st7789_draw_debug_cell_with_font(0, 0, line, debug_font, rgb565(255, 230, 80));

    snprintf(line, sizeof(line), "FPS %lu", (unsigned long)snapshot->fps);
    st7789_draw_debug_cell_with_font(1, 0, line, debug_font, rgb565(220, 220, 220));

    snprintf(line, sizeof(line), "F %luus", (unsigned long)snapshot->frame_us);
    st7789_draw_debug_cell_with_font(2, 0, line, debug_font, rgb565(220, 220, 220));

    snprintf(line, sizeof(line), "BTN %02lX", (unsigned long)snapshot->buttons);
    st7789_draw_debug_cell_with_font(0, 1, line, debug_font, rgb565(170, 240, 255));

    snprintf(line, sizeof(line), "REC %u", (unsigned)snapshot->movement_recorder_status);
    st7789_draw_debug_cell_with_font(1, 1, line, debug_font, rgb565(170, 240, 255));

    snprintf(line, sizeof(line), "D28 %u PG 1", (unsigned)snapshot->debug_button);
    st7789_draw_debug_cell_with_font(2, 1, line, debug_font, rgb565(255, 220, 130));

    snprintf(line, sizeof(line), "X %d.%02u", x_whole, x_frac);
    st7789_draw_debug_cell_with_font(0, 2, line, debug_font, rgb565(120, 255, 120));

    snprintf(line, sizeof(line), "Y %d.%02u", y_whole, y_frac);
    st7789_draw_debug_cell_with_font(1, 2, line, debug_font, rgb565(120, 255, 120));

    snprintf(line, sizeof(line), "A %d", snapshot->camera.angle);
    st7789_draw_debug_cell_with_font(2, 2, line, debug_font, rgb565(220, 190, 255));

    snprintf(line, sizeof(line), "HP %u K %u", snapshot->camera.health, snapshot->camera.kills);
    st7789_draw_debug_cell_with_font(0, 3, line, debug_font, rgb565(255, 120, 120));

    if (snapshot->benchmark.active)
        snprintf(line, sizeof(line), "%s %lu%%", benchmark_test_name(snapshot->benchmark.test), (unsigned long)(snapshot->benchmark.progress_permille / 10u));
    else if (snapshot->have_event)
        snprintf(line, sizeof(line), "EV %u%c", (unsigned)snapshot->last_event_num, snapshot->last_event_step_on ? '+' : '-');
    else
        snprintf(line, sizeof(line), "EV --");
    st7789_draw_debug_cell_with_font(1, 3, line, debug_font, rgb565(255, 190, 120));

    snprintf(line, sizeof(line), "M %ux%u I%u", snapshot->map_width, snapshot->map_height, (unsigned)snapshot->camera.currentItem);
    st7789_draw_debug_cell_with_font(2, 3, line, debug_font, rgb565(200, 200, 200));
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

static void draw_debug_benchmark_summary(const debug_display_snapshot_t *snapshot)
{
    const st7789_font_t *debug_font = ST7789_FONT_DEFAULT_MONO;
    const benchmark_snapshot_t *b = &snapshot->benchmark;
    char line[20];

    snprintf(line, sizeof(line), "%s %s", benchmark_test_name(b->test), b->active ? "RUN" : (b->complete ? "DONE" : "WAIT"));
    st7789_draw_debug_cell_with_font(0, 0, line, debug_font, rgb565(255, 230, 80));

    snprintf(line, sizeof(line), "%s %lu", b->test == BENCH_TEST_PLAYBACK ? "FRM" : "RUNS", (unsigned long)b->work_done);
    st7789_draw_debug_cell_with_font(1, 0, line, debug_font, rgb565(220, 220, 220));

    snprintf(line, sizeof(line), "TIME %lums", (unsigned long)b->elapsed_ms);
    st7789_draw_debug_cell_with_font(2, 0, line, debug_font, rgb565(220, 220, 220));

    if (b->test == BENCH_TEST_PLAYBACK)
        snprintf(line, sizeof(line), "AVG %luFPS", (unsigned long)b->avg_fps);
    else
        snprintf(line, sizeof(line), "MRUN %lu", (unsigned long)(b->avg_fps / 1000000u));
    st7789_draw_debug_cell_with_font(0, 1, line, debug_font, rgb565(255, 255, 0));

    snprintf(line, sizeof(line), "AUS %lu", (unsigned long)b->avg_frame_us);
    st7789_draw_debug_cell_with_font(1, 1, line, debug_font, rgb565(170, 240, 255));

    if (b->test == BENCH_TEST_PLAYBACK)
        snprintf(line, sizeof(line), "TOT %lu", (unsigned long)b->work_total);
    else
        snprintf(line, sizeof(line), "LIM 15s");
    st7789_draw_debug_cell_with_font(2, 1, line, debug_font, rgb565(170, 240, 255));

    snprintf(line, sizeof(line), "MIN %lu", (unsigned long)b->min_frame_us);
    st7789_draw_debug_cell_with_font(0, 2, line, debug_font, rgb565(120, 255, 120));

    snprintf(line, sizeof(line), "MAX %lu", (unsigned long)b->max_frame_us);
    st7789_draw_debug_cell_with_font(1, 2, line, debug_font, rgb565(255, 120, 120));

    snprintf(line, sizeof(line), "PROG %lu%%", (unsigned long)(b->progress_permille / 10u));
    st7789_draw_debug_cell_with_font(2, 2, line, debug_font, rgb565(220, 190, 255));

    snprintf(line, sizeof(line), ">16 %lu", (unsigned long)b->frames_over_16ms);
    st7789_draw_debug_cell_with_font(0, 3, line, debug_font, rgb565(255, 190, 120));

    snprintf(line, sizeof(line), ">33 %lu", (unsigned long)b->frames_over_33ms);
    st7789_draw_debug_cell_with_font(1, 3, line, debug_font, rgb565(255, 190, 120));

    snprintf(line, sizeof(line), "CHK %04lX", (unsigned long)(b->checksum & 0xFFFFu));
    st7789_draw_debug_cell_with_font(2, 3, line, debug_font, rgb565(200, 200, 200));
}

#define BENCH_GRAPH_POINTS 280u

static void bench_graph_clear(plot_t *plot, int16_t history[BENCH_GRAPH_POINTS], uint32_t sums[BENCH_GRAPH_POINTS], uint16_t counts[BENCH_GRAPH_POINTS])
{
    plot_clear(plot);
    plot->count = BENCH_GRAPH_POINTS;
    for (uint16_t i = 0; i < BENCH_GRAPH_POINTS; ++i)
    {
        history[i] = 0;
        sums[i] = 0;
        counts[i] = 0;
    }
}

static void bench_graph_add_sample(plot_t *plot,
                                   int16_t history[BENCH_GRAPH_POINTS],
                                   uint32_t sums[BENCH_GRAPH_POINTS],
                                   uint16_t counts[BENCH_GRAPH_POINTS],
                                   uint32_t progress_permille,
                                   uint32_t fps)
{
    uint32_t index;

    if (progress_permille > 1000u)
        progress_permille = 1000u;
    if (fps > BENCH_FPS_GRAPH_MAX)
        fps = BENCH_FPS_GRAPH_MAX;

    index = (progress_permille * (BENCH_GRAPH_POINTS - 1u)) / 1000u;
    sums[index] += fps;
    if (counts[index] != UINT16_MAX)
        counts[index]++;

    if (counts[index] != 0)
        history[index] = (int16_t)(sums[index] / counts[index]);

    plot->count = BENCH_GRAPH_POINTS;
}

static void draw_debug_benchmark_graph(const debug_display_snapshot_t *snapshot, plot_t *bench_plot)
{
    const st7789_font_t *debug_font = ST7789_FONT_DEFAULT_MONO;
    char line[24];

    st7789_fill_rect(0, 0, ST7789_WIDTH, ST7789_HEIGHT, rgb565(0, 0, 0));
    snprintf(line, sizeof(line), "%s %lu %lu%%",
             benchmark_test_name(snapshot->benchmark.test),
             (unsigned long)snapshot->benchmark.run_id,
             (unsigned long)(snapshot->benchmark.progress_permille / 10u));
    st7789_draw_text(3, 3, ST7789_WIDTH - 2, line, debug_font, rgb565(255, 255, 255));

    snprintf(line, sizeof(line), "%s %lu AVG %lu",
             snapshot->benchmark.test == BENCH_TEST_PLAYBACK ? "F" : "R",
             (unsigned long)(snapshot->benchmark.test == BENCH_TEST_PLAYBACK ? snapshot->benchmark.frames : snapshot->benchmark.work_done),
             (unsigned long)(snapshot->benchmark.test == BENCH_TEST_PLAYBACK ? snapshot->benchmark.avg_fps : snapshot->benchmark.avg_fps / 1000000u));
    st7789_draw_text(3, 14, ST7789_WIDTH - 2, line, debug_font, rgb565(255, 255, 0));

    plot_draw(bench_plot, true);
}

static void core1_debug_display_main(void)
{
    multicore_lockout_victim_init();
    MovementRecorder_Core1LockoutReady();

    uint32_t last_seen_version = 0;
    debug_display_snapshot_t snapshot;
    plot_t fps_plot;
    plot_t actual_fps_plot;
    plot_t bench_plot;
    int16_t fps_history[198];
    int16_t actual_fps_history[198];
    static int16_t bench_history[BENCH_GRAPH_POINTS];
    static uint32_t bench_sums[BENCH_GRAPH_POINTS];
    static uint16_t bench_counts[BENCH_GRAPH_POINTS];
    uint32_t bench_graph_run_id = 0;

    plot_init(&fps_plot, 60, 5, 200, 70, 1, 0, BENCH_FPS_GRAPH_MAX, rgb565(0, 0, 0), rgb565(255, 255, 0), fps_history, (uint16_t)(sizeof(fps_history) / sizeof(fps_history[0])));
    plot_init(&actual_fps_plot, 60, 5, 200, 70, 1, 0, 200, rgb565(0, 0, 0), rgb565(255, 0, 0), actual_fps_history, (uint16_t)(sizeof(actual_fps_history) / sizeof(actual_fps_history[0])));
    plot_init(&bench_plot, 1, 25, 282, 50, 1, 0, BENCH_FPS_GRAPH_MAX, rgb565(0, 0, 0), rgb565(255, 255, 0), bench_history, BENCH_GRAPH_POINTS);
    bench_graph_clear(&bench_plot, bench_history, bench_sums, bench_counts);

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
        uint32_t graph_fps = snapshot.fps > BENCH_FPS_GRAPH_MAX ? BENCH_FPS_GRAPH_MAX : snapshot.fps;
        plot_add(&fps_plot, (int16_t)graph_fps);
        plot_add(&actual_fps_plot, (int16_t)snapshot.actual_fps);

        if (snapshot.benchmark.run_id != bench_graph_run_id)
        {
            bench_graph_run_id = snapshot.benchmark.run_id;
            bench_graph_clear(&bench_plot, bench_history, bench_sums, bench_counts);
        }

        if (snapshot.benchmark.active)
        {
            if (snapshot.frame_us <= BENCH_MAX_VALID_FRAME_US)
            {
                bench_graph_add_sample(&bench_plot,
                                       bench_history,
                                       bench_sums,
                                       bench_counts,
                                       snapshot.benchmark.progress_permille,
                                       graph_fps);
            }
        }

        st7789_fill_rect(0, 0, ST7789_WIDTH, ST7789_HEIGHT, rgb565(0, 0, 0));

        switch (snapshot.page)
        {
        case DEBUG_PAGE_STATUS:
            draw_debug_status_snapshot(&snapshot);
            break;
        case DEBUG_PAGE_FPS:
            draw_debug_fps_snapshot(&snapshot);
            plot_draw(&fps_plot, true);
            plot_draw(&actual_fps_plot, false);
            break;
        case DEBUG_PAGE_BENCH_GRAPH:
            draw_debug_benchmark_graph(&snapshot, &bench_plot);
            break;
        case DEBUG_PAGE_BENCH_SUMMARY:
            draw_debug_benchmark_summary(&snapshot);
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

static void change_level(uint8_t level)
{
    switch (level)
    {
    case 0:
        current_map = &Level0Map;
        break;
    case 1:
        current_map = &Level1Map;
        break;
    case 2:
        current_map = &Level2Map;
        for (uint8_t i = 0; i < MAX_ENTITIES; i++)
        {
            uint8_t type = rand16();
            while (type > 2)
                type = rand16();
            memcpy(&entities[i], enemieTemplates[type], sizeof(entity_t));
            RespawnEntity(current_map, &entities[i]);
        }
        break;
    case 3:
        current_map = &Level3Map;
        for (uint8_t i = 0; i < MAX_ENTITIES; i++)
        {
            memcpy(&entities[i], &soilderTemplate, sizeof(entity_t));
            RespawnEntity(current_map, &entities[i]);
        }
        break;
    default:
        return;
    }

    camera.posX = FX(current_map->DefaultSpwanPoint[0]);
    camera.posY = FX(current_map->DefaultSpwanPoint[1]);
    current_dialogue = NULL;
    current_level_num = level;
}

static void flash_screen(void)
{
    for (uint8_t i = 0; i < 16; i++)
    {
        dogm128_invert();
        dogm128_refresh();
        sleep_ms(80);
    }
}

uint8_t menuOpen = 0;

static void benchmark_start_playback(uint64_t now_us);
static void benchmark_start_synthetic(benchmark_test_t test, uint64_t now_us);

static void DrawMenu(buttons_t state, bool disallow_resume)
{
    if (menuOpen == 0)
    {
        if (state.all >= 0b11111)
            menuOpen = 1;
        else
            return;
    }
    if (menuOpen == 1)
    {
        if (state.all == 0)
            menuOpen = 2;
    }

    dogm128_fill_rect(0, 64 - 7, 128, 7, DISP_COL_WHITE);
    dogm128_hline(0, 64 - 7, 128, DISP_COL_BLACK);
    dogm128_text(1, 64 - 5, "resume");
    dogm128_text(30, 64 - 5, "fps");
    if (show_fps)
    {
        dogm128_text(64 - 10, 64 - 5, "level");
        dogm128_text(90 - 8, 64 - 5, "item");
    }
    else
    {
            dogm128_text(64 - 18, 64 - 5, "clear recording");
    }
    dogm128_text(127 - 20, 64 - 5, "reset");

    if (menuOpen == 2)
    {
        if (state.back && !disallow_resume)
            menuOpen = 0;
        if (state.front)
        {
            show_fps = !show_fps;
            menuOpen = 0;
        }
        if (state.use)
        {
            if (show_fps)
            {
                current_level_num++;
                if (current_level_num > 3)
                    current_level_num = 0;
                change_level(current_level_num);
            }
            else
            {
                MovementRecorder_Clear();
            }
            menuOpen = 0;
        }
        if (show_fps && state.left)
        {
            camera.currentItem++;
            if (camera.currentItem >= ITEM_COUNT)
                camera.currentItem = ITEM_HAND;
            menuOpen = 0;
        }
        if (state.right)
        {
            watchdog_reboot(0, 0, 0);
            while (1)
                ;
            menuOpen = 0;
        }
    }
}

static void on_map_event(uint8_t param1, uint8_t param2)
{
    g_last_event_num = param1;
    g_last_event_step_on = param2 != 0;
    g_have_event = true;

    if (param1 == 0 && param2 == 1)
    {
        flash_screen();
        change_level(1);
    }
    else if (param1 == 1 && param2 == 1)
    {
        flash_screen();
        change_level(2);
    }
    else if (param1 == 2 && param2 == 1)
    {
        flash_screen();
        change_level(3);
    }
    else if (param1 == 3 && param2 == 1)
    {
        menuOpen = 100;
    }
}

static void DrawBenchmarkMenu(buttons_t state)
{
    static buttons_t prev_state = {0};
    buttons_t pressed = {.all = (uint8_t)(state.all & (uint8_t)~prev_state.all)};
    char line[20];

    if (pressed.front)
        selected_benchmark_test = (benchmark_test_t)((selected_benchmark_test + 1) % BENCH_TEST_COUNT);
    if (pressed.back)
        selected_benchmark_test = (benchmark_test_t)((selected_benchmark_test + BENCH_TEST_COUNT - 1) % BENCH_TEST_COUNT);
    if (pressed.right && MovementRecorder_GetStatus() != MOVEMENT_RECORDER_STATUS_IDLE)
        benchmark_menu_open = false;
    if (pressed.left)
    {
        MovementRecorder_Clear();
    }
    if (pressed.use)
    {
        benchmark_menu_open = false;
        if (selected_benchmark_test == BENCH_TEST_PLAYBACK)
        {
            if (MovementRecorder_GetStatus() == MOVEMENT_RECORDER_STATUS_RECORDING)
            {
                MovementRecorder_StopRecording();
                MovementRecorder_StartReplay();
                benchmark_start_playback(to_us_since_boot(get_absolute_time()));
                debug_page = DEBUG_PAGE_BENCH_GRAPH;
            }
            else if (MovementRecorder_GetStatus() == MOVEMENT_RECORDER_STATUS_REPLAYING)
            {
                MovementRecorder_StopReplay();
                debug_page = DEBUG_PAGE_BENCH_SUMMARY;
            }
            else if (MovementRecorder_IsEmpty())
            {
                MovementRecorder_StartRecording();
            }
            else
            {
                MovementRecorder_StartReplay();
                benchmark_start_playback(to_us_since_boot(get_absolute_time()));
                debug_page = DEBUG_PAGE_BENCH_GRAPH;
            }
        }
        else
        {
            if (MovementRecorder_GetStatus() == MOVEMENT_RECORDER_STATUS_RECORDING)
                MovementRecorder_StopRecording();
            else if (MovementRecorder_GetStatus() == MOVEMENT_RECORDER_STATUS_REPLAYING)
                MovementRecorder_StopReplay();
            benchmark_start_synthetic(selected_benchmark_test, to_us_since_boot(get_absolute_time()));
            debug_page = DEBUG_PAGE_BENCH_GRAPH;
        }
    }

    dogm128_fill_rect(0, 0, 128, 64, DISP_COL_WHITE);
    dogm128_rect(0, 0, 128, 64, DISP_COL_BLACK);
    dogm128_text(4, 4, "BENCHMARKS");

    snprintf(line, sizeof(line), "< %s >", benchmark_test_name(selected_benchmark_test));
    dogm128_text(4, 16, line);
    dogm128_text(4, 24, benchmark_test_detail(selected_benchmark_test));

    if (selected_benchmark_test == BENCH_TEST_PLAYBACK)
    {
        switch (MovementRecorder_GetStatus())
        {
        case MOVEMENT_RECORDER_STATUS_RECORDING:
            dogm128_text(4, 34, "USE: STOP+REPLAY");
            break;
        case MOVEMENT_RECORDER_STATUS_REPLAYING:
            dogm128_text(4, 34, "USE: STOP REPLAY");
            break;
        default:
            dogm128_text(4, 34, MovementRecorder_IsEmpty() ? "USE: RECORD" : "USE: REPLAY");
            break;
        }
    }
    else
    {
        dogm128_text(4, 34, "USE: RUN TEST");
    }

    dogm128_text(4, 46, "FRONT/BACK: SELECT");
    dogm128_text(4, 54, MovementRecorder_GetStatus() == MOVEMENT_RECORDER_STATUS_IDLE ? "LEFT: CLEAR RECORDING" : "LEFT: CLEAR RECORDING");

    prev_state = state;
}

typedef struct
{
    bool active;
    bool complete;
    benchmark_test_t test;
    uint32_t run_id;
    uint64_t start_us;
    uint64_t elapsed_us;
    uint64_t frame_time_total_us;
    uint32_t recorded_samples;
    uint32_t work_done;
    uint32_t work_total;
    uint32_t checksum;
    uint32_t frames;
    uint32_t min_frame_us;
    uint32_t max_frame_us;
    uint32_t frames_over_16ms;
    uint32_t frames_over_33ms;
} benchmark_state_t;

static benchmark_state_t g_benchmark = {0};
static volatile uint32_t g_benchmark_sink = 0;

static void benchmark_start_common(benchmark_test_t test, uint64_t now_us)
{
    g_benchmark.active = true;
    g_benchmark.complete = false;
    g_benchmark.test = test;
    g_benchmark.run_id++;
    g_benchmark.start_us = now_us;
    g_benchmark.elapsed_us = 0;
    g_benchmark.frame_time_total_us = 0;
    g_benchmark.recorded_samples = 0;
    g_benchmark.work_done = 0;
    g_benchmark.work_total = 0;
    g_benchmark.checksum = 0x12345678u;
    g_benchmark.frames = 0;
    g_benchmark.min_frame_us = UINT32_MAX;
    g_benchmark.max_frame_us = 0;
    g_benchmark.frames_over_16ms = 0;
    g_benchmark.frames_over_33ms = 0;
}

static void benchmark_start_playback(uint64_t now_us)
{
    benchmark_start_common(BENCH_TEST_PLAYBACK, now_us);
    g_benchmark.recorded_samples = MovementRecorder_GetRecordedCount();
    g_benchmark.work_total = g_benchmark.recorded_samples;
}

static void benchmark_start_synthetic(benchmark_test_t test, uint64_t now_us)
{
    benchmark_start_common(test, now_us);
    g_benchmark.work_total = 15u;
}

static void benchmark_add_frame(uint32_t frame_len_us)
{
    if (!g_benchmark.active)
        return;
    if (frame_len_us > BENCH_MAX_VALID_FRAME_US)
        return;

    g_benchmark.frames++;
    g_benchmark.frame_time_total_us += frame_len_us;

    if (frame_len_us < g_benchmark.min_frame_us)
        g_benchmark.min_frame_us = frame_len_us;
    if (frame_len_us > g_benchmark.max_frame_us)
        g_benchmark.max_frame_us = frame_len_us;
    if (frame_len_us > 16667u)
        g_benchmark.frames_over_16ms++;
    if (frame_len_us > 33333u)
        g_benchmark.frames_over_33ms++;
}

static void benchmark_stop(uint64_t now_us)
{
    if (!g_benchmark.active)
        return;

    g_benchmark.active = false;
    g_benchmark.complete = true;
    g_benchmark.elapsed_us = now_us - g_benchmark.start_us;
    if (g_benchmark.min_frame_us == UINT32_MAX)
        g_benchmark.min_frame_us = 0;
}

static benchmark_snapshot_t benchmark_make_snapshot(uint64_t now_us)
{
    benchmark_snapshot_t snapshot = {0};
    uint64_t elapsed_us = g_benchmark.active ? (now_us - g_benchmark.start_us) : g_benchmark.elapsed_us;
    uint32_t playback_index = MovementRecorder_GetPlaybackIndex();

    snapshot.active = g_benchmark.active ? 1u : 0u;
    snapshot.complete = g_benchmark.complete ? 1u : 0u;
    snapshot.test = g_benchmark.test;
    snapshot.run_id = g_benchmark.run_id;
    snapshot.recorded_samples = g_benchmark.recorded_samples;
    snapshot.work_done = g_benchmark.work_done;
    snapshot.work_total = g_benchmark.work_total;
    snapshot.checksum = g_benchmark.checksum;
    snapshot.frames = g_benchmark.frames;
    snapshot.elapsed_ms = (uint32_t)(elapsed_us / 1000u);
    snapshot.min_frame_us = g_benchmark.min_frame_us == UINT32_MAX ? 0 : g_benchmark.min_frame_us;
    snapshot.max_frame_us = g_benchmark.max_frame_us;
    snapshot.frames_over_16ms = g_benchmark.frames_over_16ms;
    snapshot.frames_over_33ms = g_benchmark.frames_over_33ms;

    if (g_benchmark.test == BENCH_TEST_PLAYBACK && g_benchmark.recorded_samples != 0)
    {
        if (playback_index > g_benchmark.recorded_samples)
            playback_index = g_benchmark.recorded_samples;
        snapshot.progress_permille = (playback_index * 1000u) / g_benchmark.recorded_samples;
    }
    else if (g_benchmark.test != BENCH_TEST_PLAYBACK)
    {
        if (elapsed_us >= BENCH_SYNTHETIC_DURATION_US)
            snapshot.progress_permille = 1000u;
        else
            snapshot.progress_permille = (uint32_t)((elapsed_us * 1000ull) / BENCH_SYNTHETIC_DURATION_US);
    }
    else if (g_benchmark.complete)
    {
        snapshot.progress_permille = 1000u;
    }

    if (g_benchmark.frames != 0)
        snapshot.avg_frame_us = (uint32_t)(g_benchmark.frame_time_total_us / g_benchmark.frames);
    if (g_benchmark.frame_time_total_us != 0 && g_benchmark.test == BENCH_TEST_PLAYBACK)
        snapshot.avg_fps = (uint32_t)(((uint64_t)g_benchmark.frames * 1000000ull + (g_benchmark.frame_time_total_us / 2u)) / g_benchmark.frame_time_total_us);
    else if (elapsed_us != 0)
        snapshot.avg_fps = (uint32_t)(((uint64_t)g_benchmark.work_done * 1000000ull + (elapsed_us / 2u)) / elapsed_us);

    return snapshot;
}

static void DrawBenchmarkStatusScreen(const benchmark_snapshot_t *snapshot)
{
    char line[24];

    dogm128_fill_rect(0, 0, 128, 64, DISP_COL_WHITE);
    dogm128_rect(0, 0, 128, 64, DISP_COL_BLACK);

    snprintf(line, sizeof(line), "%s %s",
             benchmark_test_name(snapshot->test),
             snapshot->active ? "RUNNING" : "DONE");
    dogm128_text(4, 4, line);

    snprintf(line, sizeof(line), "PROGRESS %lu%%", (unsigned long)(snapshot->progress_permille / 10u));
    dogm128_text(4, 16, line);
    dogm128_rect(4, 25, 120, 8, DISP_COL_BLACK);
    dogm128_fill_rect(6, 27, (uint8_t)((snapshot->progress_permille * 116u) / 1000u), 4, DISP_COL_BLACK);

    snprintf(line, sizeof(line), "%s %lu",
             snapshot->test == BENCH_TEST_PLAYBACK ? "FRAMES" : "RUNS",
             (unsigned long)(snapshot->test == BENCH_TEST_PLAYBACK ? snapshot->frames : snapshot->work_done));
    dogm128_text(4, 38, line);

    if (snapshot->test == BENCH_TEST_PLAYBACK)
        snprintf(line, sizeof(line), "AVG %lu FPS", (unsigned long)snapshot->avg_fps);
    else
        snprintf(line, sizeof(line), "AVG %lu MRUN", (unsigned long)(snapshot->avg_fps / 1000000u));
    dogm128_text(4, 48, line);

    snprintf(line, sizeof(line), "TIME %lus", (unsigned long)(snapshot->elapsed_ms / 1000u));
    dogm128_text(4, 56, line);
}

static bool benchmark_is_synthetic_active(void)
{
    return g_benchmark.active && g_benchmark.test != BENCH_TEST_PLAYBACK;
}

static uint32_t bench_next_rand(uint32_t *state)
{
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static void benchmark_run_synthetic_chunk(uint64_t now_us)
{
    uint32_t chunk;
    uint64_t chunk_start_us;
    uint64_t chunk_end_us;
    uint32_t x = g_benchmark.checksum;

    if (!benchmark_is_synthetic_active())
        return;
    if (now_us - g_benchmark.start_us >= BENCH_SYNTHETIC_DURATION_US)
    {
        benchmark_stop(now_us);
        return;
    }

    chunk = (g_benchmark.test == BENCH_TEST_LINES) ? BENCH_LINE_CHUNK_OPS : BENCH_MATH_CHUNK_OPS;

    chunk_start_us = to_us_since_boot(get_absolute_time());

    switch (g_benchmark.test)
    {
    case BENCH_TEST_ADD_SUB:
        for (uint32_t i = 0; i < chunk; ++i)
        {
            x += i + 3u;
            x -= i ^ 0x5A5A5A5Au;
        }
        break;
    case BENCH_TEST_MUL:
        for (uint32_t i = 0; i < chunk; ++i)
            x = (x * 1664525u) + (i | 1u);
        break;
    case BENCH_TEST_XOR_SHIFT:
        for (uint32_t i = 0; i < chunk; ++i)
            x = bench_next_rand(&x);
        break;
    case BENCH_TEST_LINES:
        for (uint32_t i = 0; i < chunk; ++i)
        {
            uint32_t r0 = bench_next_rand(&x);
            uint32_t r1 = bench_next_rand(&x);
            dogm128_line((int)(r0 & 127u),
                         (int)((r0 >> 8) & 63u),
                         (int)(r1 & 127u),
                         (int)((r1 >> 8) & 63u),
                         (r1 & 0x10000u) ? DISP_COL_BLACK : DISP_COL_WHITE);
        }
        break;
    default:
        break;
    }

    chunk_end_us = to_us_since_boot(get_absolute_time());
    if (chunk_end_us == chunk_start_us)
        chunk_end_us++;

    g_benchmark_sink = x;
    g_benchmark.checksum = x;
    g_benchmark.work_done += chunk;
    benchmark_add_frame((uint32_t)(chunk_end_us - chunk_start_us));

    if (chunk_end_us - g_benchmark.start_us >= BENCH_SYNTHETIC_DURATION_US)
        benchmark_stop(chunk_end_us);
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

    change_level(0);
    camera.angle = FX_ANGLE_HALF;
    camera.dirX = fx_cos(camera.angle);
    camera.dirY = fx_sin(camera.angle);
    camera.planeX = fx_mul(camera.dirY, (fx_t)0x00a9);
    camera.planeY = fx_neg(fx_mul(camera.dirX, (fx_t)0x00a9));
    for (int i = 0; i < 48 - 1; i++)
        camera.zBuffer[i] = FX(64);
    camera.health = 5;
    camera.kills = 0;
    camera.currentItem = ITEM_HAND;

    MapEventCallback = on_map_event;

    bool prev_use = false;
    bool dialogue_active = false;
    bool prev_debug_button = false;
    uint64_t init_us = to_us_since_boot(get_absolute_time());
    benchmark_snapshot_t benchmark_snapshot = benchmark_make_snapshot(init_us);
    publish_debug_snapshot(debug_page, &camera, current_map, false, 0, false, 0, 0, 0, init_us, MovementRecorder_GetStatus(), &benchmark_snapshot);

    multicore_launch_core1(core1_debug_display_main);

    while (true)
    {
        static absolute_time_t last_frame_complete = {0};
        static uint64_t last_oled_present_us = 0;
        static uint64_t last_game_update_us = 0;
        static bool damage_flash_active = false;
        absolute_time_t frame_start = get_absolute_time();
        uint64_t frame_start_us = to_us_since_boot(frame_start);
        movement_recorder_status_t status_at_frame_start = MovementRecorder_GetStatus();
        bool game_frame_rendered = false;
        bool update_game = (frame_start_us - last_game_update_us) >= GAME_UPDATE_INTERVAL_US;
        uint32_t render_len_us = 1;
        static buttons_t gameplay_buttons = {0};

        if (!benchmark_menu_open &&
            selected_benchmark_test == BENCH_TEST_PLAYBACK &&
            status_at_frame_start == MOVEMENT_RECORDER_STATUS_REPLAYING &&
            !g_benchmark.active)
            benchmark_start_playback(frame_start_us);

        millis = to_ms_since_boot(frame_start);

        buttons_t buttons = buttons_from_mask(read_buttons_mask());
        buttons_t unaltered_buttons = buttons;
        bool synthetic_benchmark_active = benchmark_is_synthetic_active();
        bool game_benchmark_active =
            !benchmark_menu_open &&
            selected_benchmark_test == BENCH_TEST_PLAYBACK &&
            (MovementRecorder_GetStatus() == MOVEMENT_RECORDER_STATUS_RECORDING ||
             MovementRecorder_GetStatus() == MOVEMENT_RECORDER_STATUS_REPLAYING);
        if (synthetic_benchmark_active)
        {
            benchmark_run_synthetic_chunk(frame_start_us);
            if (!benchmark_is_synthetic_active())
                debug_page = DEBUG_PAGE_BENCH_SUMMARY;
        }

        static bool prevMenu = 0;
        if (menuOpen == 0 && game_benchmark_active)
        {
            if (update_game)
            {
                if (MovementRecorder_GetStatus() == MOVEMENT_RECORDER_STATUS_RECORDING)
                {
                    MovementRecorder_CurrentValues(buttons);
                    gameplay_buttons = buttons;
                }
                else if (MovementRecorder_GetStatus() == MOVEMENT_RECORDER_STATUS_REPLAYING)
                {
                    gameplay_buttons = MovementRecorder_GetPlaybackValues();
                }
                else
                {
                    gameplay_buttons = buttons;
                }
            }

            uint64_t render_start_us = to_us_since_boot(get_absolute_time());
            dogm128_clear();
            if (update_game)
            {
                MoveCamera(&camera, current_map, gameplay_buttons, &current_dialogue, prevMenu);
                last_game_update_us = frame_start_us;
            }
            RenderFrame(&camera, current_map);
            game_frame_rendered = true;
            if (current_map != &Level0Map && current_map != &Level1Map)
            {
                DrawEntities(&camera, entities, MAX_ENTITIES, dogm_fb, gameplay_buttons, current_map);
                if (update_game)
                    EnemyAi(&camera, entities, MAX_ENTITIES, current_map, prevMenu);
            }

            HUD_DrawBorders();
            HUD_DrawItem(camera.currentItem);
            HUD_DrawMap(current_map, &camera);
            HUD_DrawCompass(camera.angle, camera.dirX, camera.dirY);
            if (current_map != &Level0Map)
            {
                HUD_DrawBanner(current_map->Banner);
                HUD_DrawStats(camera.health, camera.kills);
                HUD_DrawItemPOV(camera.currentItem, use_press_ms + 200 > millis);
            }

            bool use_pressed = false;
            if (update_game)
            {
                use_pressed = gameplay_buttons.use && !prev_use;
                prev_use = gameplay_buttons.use;
            }

            dialogue_active = (current_dialogue != NULL);
            if (use_pressed)
                use_press_ms = millis;

            HUD_DrawDialogue(&current_dialogue, update_game && use_pressed && dialogue_active);

            if (camera.health == 0)
            {
                dogm128_fill_rect((96 / 2) - 25, (64 / 2) - 10, 50, 20, DISP_COL_WHITE);
                dogm128_text((96 / 2) - 6, (57 / 2) - 2, "RIP");
                menuOpen = 1;
            }

            set_LEDs(HUD_GetLEDHP(camera.health));
            uint64_t render_end_us = to_us_since_boot(get_absolute_time());
            render_len_us = (uint32_t)(render_end_us - render_start_us);
            if (render_len_us == 0)
                render_len_us = 1;
            prevMenu = 0;
        }
        else
            prevMenu = 1;

        if (game_benchmark_active && menuOpen == 100)
        {
            dogm128_fill_rect(0, 9, 96, 55, DISP_COL_WHITE);
            dogm128_refresh();
            sleep_ms(5000);
            dogm128_text((96 / 2) - 20, (57 / 2) - 2, "YOU JUMPED");
            dogm128_refresh();
            sleep_ms(500);
            menuOpen = 101;
        }
        if (game_benchmark_active && menuOpen == 101)
        {
            dogm128_pixel(rand16() % 128, rand16() % 64, DISP_COL_BLACK);
            sleep_ms(60);
        }

        if (benchmark_menu_open)
            DrawBenchmarkMenu(unaltered_buttons);
        else if (game_benchmark_active)
            DrawMenu(unaltered_buttons, camera.health == 0);

        absolute_time_t frame_end = get_absolute_time();
        uint64_t frame_end_us = to_us_since_boot(frame_end);
        uint32_t frame_len_us = (uint32_t)(frame_end_us - frame_start_us);
        uint32_t frame_len_actual_us = (uint32_t)(frame_end_us - to_us_since_boot(last_frame_complete));
        last_frame_complete = get_absolute_time();
        if (frame_len_us == 0)
            frame_len_us = 1;
        if (frame_len_actual_us == 0)
            frame_len_actual_us = 1;
        uint32_t fps = (1000000u + (render_len_us / 2u)) / render_len_us;
        uint32_t actual_fps = (1000000u + (frame_len_actual_us / 2u)) / frame_len_actual_us;
        movement_recorder_status_t status_after_frame = MovementRecorder_GetStatus();

        if (game_frame_rendered && g_benchmark.active && g_benchmark.test == BENCH_TEST_PLAYBACK && status_after_frame == MOVEMENT_RECORDER_STATUS_REPLAYING)
            benchmark_add_frame(render_len_us);
        if (g_benchmark.active && g_benchmark.test == BENCH_TEST_PLAYBACK && status_after_frame != MOVEMENT_RECORDER_STATUS_REPLAYING)
        {
            benchmark_stop(frame_end_us);
            debug_page = DEBUG_PAGE_BENCH_SUMMARY;
        }

        millis = to_ms_since_boot(frame_end);

        bool should_flash_damage = (int32_t)(damageFlashUntilMs - millis) > 0;
        if (should_flash_damage != damage_flash_active)
        {
            dogm128_contrast(should_flash_damage ? 0x30 : 0x8F);
            damage_flash_active = should_flash_damage;
        }

        // Short press cycles debug pages; long press opens the benchmark menu.
        bool debug_button = read_debug_button();
        static millis_t last_debug_button_press_ms = 0;
        if (debug_button != prev_debug_button)
            last_debug_button_press_ms = millis;
        if (!benchmark_menu_open && !debug_button && prev_debug_button && (millis - last_debug_button_press_ms < 500))
        {
            pageCycle();
            last_debug_button_press_ms = millis;
        }
        if (millis - last_debug_button_press_ms >= 500 && debug_button)
        {
            if (MovementRecorder_GetStatus() == MOVEMENT_RECORDER_STATUS_REPLAYING)
                MovementRecorder_StopReplay();
            if (benchmark_is_synthetic_active())
            {
                benchmark_stop(to_us_since_boot(get_absolute_time()));
                debug_page = DEBUG_PAGE_BENCH_SUMMARY;
            }
            benchmark_menu_open = true;
            last_debug_button_press_ms = millis;
        }
        prev_debug_button = debug_button;
        uint64_t publish_us = to_us_since_boot(get_absolute_time());
        benchmark_snapshot = benchmark_make_snapshot(publish_us);
        uint32_t display_fps = benchmark_is_synthetic_active() ? (benchmark_snapshot.avg_fps / 1000000u) : fps;

        publish_debug_snapshot(debug_page,
                               &camera,
                               current_map,
                               dialogue_active,
                               gameplay_buttons.all,
                               debug_button,
                               display_fps,
                               actual_fps,
                               render_len_us,
                               publish_us,
                               MovementRecorder_GetStatus(),
                               &benchmark_snapshot);

        if (!benchmark_menu_open &&
            !game_benchmark_active &&
            (benchmark_snapshot.active || benchmark_snapshot.complete))
        {
            DrawBenchmarkStatusScreen(&benchmark_snapshot);
        }

        if (show_fps)
        {
            char buf[10];
            utoa_mine((uint16_t)fps, buf, 0);
            dogm128_text(0, 0, buf);
        }

        if (publish_us - last_oled_present_us >= OLED_PRESENT_INTERVAL_US)
        {
            dogm128_refresh();
            last_oled_present_us = to_us_since_boot(get_absolute_time());
        }

        // sleep_ms(1);
    }
}
