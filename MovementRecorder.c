#include "MovementRecorder.h"
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

#define FLASH_BASE_ADDRESS      0x10000000u
#define MOVEMENT_MAGIC          0x4D4F5645u  // "MOVE"

#define RECORD_SAMPLE_CAPACITY  300000u
#define FLASH_PAGE_SIZE_BYTES   256u
#define FLASH_SECTOR_SIZE_BYTES 4096u
#define FLASH_HEADER_SIZE       FLASH_PAGE_SIZE_BYTES
#define FLASH_DATA_SIZE         (((RECORD_SAMPLE_CAPACITY + FLASH_PAGE_SIZE_BYTES - 1u) / FLASH_PAGE_SIZE_BYTES) * FLASH_PAGE_SIZE_BYTES)
#define FLASH_USED_SIZE         (FLASH_HEADER_SIZE + FLASH_DATA_SIZE)
#define FLASH_ERASE_SIZE        (((FLASH_USED_SIZE + FLASH_SECTOR_SIZE_BYTES - 1u) / FLASH_SECTOR_SIZE_BYTES) * FLASH_SECTOR_SIZE_BYTES)
#define FLASH_TARGET_OFFSET     (PICO_FLASH_SIZE_BYTES - FLASH_ERASE_SIZE)
#define FLASH_DATA_OFFSET       (FLASH_TARGET_OFFSET + FLASH_HEADER_SIZE)

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t count;
} movement_flash_header_t;

static uint8_t g_flash_page[FLASH_PAGE_SIZE_BYTES];
static const size_t g_recorded_max = RECORD_SAMPLE_CAPACITY;
static size_t g_recorded_count = 0;
static size_t g_playback_index = 0;
static size_t g_recording_page_fill = 0;
static uint32_t g_next_recording_page_offset = FLASH_DATA_OFFSET;
static buttons_t g_playback_buttons = {0};
static movement_recorder_status_t g_status = MOVEMENT_RECORDER_STATUS_IDLE;
static millis_t g_last_time = 0;
static volatile bool g_core1_lockout_ready = false;

static const movement_flash_header_t * const flash_header =
    (const movement_flash_header_t *)(FLASH_BASE_ADDRESS + FLASH_TARGET_OFFSET);
static const uint8_t * const flash_samples =
    (const uint8_t *)(FLASH_BASE_ADDRESS + FLASH_DATA_OFFSET);

static void flash_erase_recording_area(void)
{
    bool use_lockout = g_core1_lockout_ready;
    if (use_lockout)
        multicore_lockout_start_blocking();
    uint32_t ints = save_and_disable_interrupts();

    flash_range_erase(FLASH_TARGET_OFFSET, FLASH_ERASE_SIZE);

    restore_interrupts(ints);
    if (use_lockout)
        multicore_lockout_end_blocking();
}

static void flash_program_page(uint32_t flash_offset, const uint8_t page[FLASH_PAGE_SIZE_BYTES])
{
    bool use_lockout = g_core1_lockout_ready;
    if (use_lockout)
        multicore_lockout_start_blocking();
    uint32_t ints = save_and_disable_interrupts();

    flash_range_program(flash_offset, page, FLASH_PAGE_SIZE_BYTES);

    restore_interrupts(ints);
    if (use_lockout)
        multicore_lockout_end_blocking();
}

static bool flash_has_valid_recording(void)
{
    if (flash_header->magic != MOVEMENT_MAGIC)
        return false;

    if (flash_header->count > RECORD_SAMPLE_CAPACITY)
        return false;

    return true;
}

static void flush_recording_page(void)
{
    if (g_recording_page_fill == 0)
        return;

    memset(g_flash_page + g_recording_page_fill, 0xFF, FLASH_PAGE_SIZE_BYTES - g_recording_page_fill);
    flash_program_page(g_next_recording_page_offset, g_flash_page);
    g_next_recording_page_offset += FLASH_PAGE_SIZE_BYTES;
    g_recording_page_fill = 0;
}

static void write_recording_header(void)
{
    movement_flash_header_t header = {
        .magic = MOVEMENT_MAGIC,
        .count = (uint32_t)g_recorded_count,
    };

    memset(g_flash_page, 0xFF, sizeof(g_flash_page));
    memcpy(g_flash_page, &header, sizeof(header));
    flash_program_page(FLASH_TARGET_OFFSET, g_flash_page);
}

void MovementRecorder_Init(void)
{
    g_recorded_count = 0;
    g_playback_index = 0;
    g_recording_page_fill = 0;
    g_next_recording_page_offset = FLASH_DATA_OFFSET;
    g_playback_buttons.all = 0;
    g_status = MOVEMENT_RECORDER_STATUS_IDLE;
    g_last_time = millis;
}

void MovementRecorder_Core1LockoutReady(void)
{
    g_core1_lockout_ready = true;
}

void MovementRecorder_Clear(void)
{
    g_recorded_count = 0;
    g_playback_index = 0;
    g_recording_page_fill = 0;
    g_next_recording_page_offset = FLASH_DATA_OFFSET;
    g_playback_buttons.all = 0;
    g_status = MOVEMENT_RECORDER_STATUS_IDLE;
    memset(g_flash_page, 0xFF, sizeof(g_flash_page));
    flash_erase_recording_area();
}

void MovementRecorder_StartRecording(void)
{
    g_recorded_count = 0;
    g_recording_page_fill = 0;
    g_next_recording_page_offset = FLASH_DATA_OFFSET;
    memset(g_flash_page, 0xFF, sizeof(g_flash_page));
    flash_erase_recording_area();
    g_status = MOVEMENT_RECORDER_STATUS_RECORDING;
    g_last_time = millis;
}

void MovementRecorder_StopRecording(void)
{
    g_status = MOVEMENT_RECORDER_STATUS_IDLE;

    flush_recording_page();
    write_recording_header();
}

void MovementRecorder_StartReplay(void)
{
    g_playback_index = 0;
    g_playback_buttons.all = 0;

    if (!flash_has_valid_recording() || flash_header->count == 0) {
        g_status = MOVEMENT_RECORDER_STATUS_IDLE;
        return;
    }

    g_status = MOVEMENT_RECORDER_STATUS_REPLAYING;
    g_last_time = millis - 1;
}

void MovementRecorder_StopReplay(void)
{
    g_status = MOVEMENT_RECORDER_STATUS_IDLE;
}

void MovementRecorder_CurrentValues(buttons_t buttons)
{
    if (g_status != MOVEMENT_RECORDER_STATUS_RECORDING)
        return;

    if (g_recorded_count < g_recorded_max)
    {
        g_flash_page[g_recording_page_fill++] = buttons.all;
        ++g_recorded_count;

        if (g_recording_page_fill == FLASH_PAGE_SIZE_BYTES)
            flush_recording_page();
    }
}

buttons_t MovementRecorder_GetPlaybackValues(void)
{
    if (g_status != MOVEMENT_RECORDER_STATUS_REPLAYING)
    {
        buttons_t tmp = {0};
        return tmp;
    }

    if (!flash_has_valid_recording()) {
        g_status = MOVEMENT_RECORDER_STATUS_IDLE;
        g_playback_buttons.all = 0;
        return g_playback_buttons;
    }

    if (g_playback_index < flash_header->count)
    {
        g_playback_buttons.all = flash_samples[g_playback_index++];
    }
    else
    {
        g_status = MOVEMENT_RECORDER_STATUS_IDLE;
        g_playback_buttons.all = 0;
    }

    return g_playback_buttons;
}

bool MovementRecorder_IsEmpty(void)
{
    return !flash_has_valid_recording() || (flash_header->count == 0);
}

movement_recorder_status_t MovementRecorder_GetStatus(void)
{
    return g_status;
}

uint32_t MovementRecorder_GetRecordedCount(void)
{
    if (g_status == MOVEMENT_RECORDER_STATUS_RECORDING)
        return (uint32_t)g_recorded_count;

    if (!flash_has_valid_recording())
        return 0;

    return flash_header->count;
}

uint32_t MovementRecorder_GetPlaybackIndex(void)
{
    return (uint32_t)g_playback_index;
}
