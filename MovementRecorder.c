#include "MovementRecorder.h"
#include <string.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

#define FLASH_BASE_ADDRESS      0x10000000u
#define FLASH_TARGET_OFFSET     (2 * 1024 * 1024 - 128 * 1024)   // last 128 KB
#define MOVEMENT_MAGIC          0x4D4F5645u  // "MOVE"

#define RECORD_BUFFER_SIZE      80000u
#define FLASH_ERASE_SIZE        (128 * 1024u)
#define FLASH_PAGE_SIZE_BYTES   256u

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t count;
    uint8_t data[RECORD_BUFFER_SIZE];
} movement_flash_payload_t;

#define FLASH_PAYLOAD_SIZE      ((uint32_t)sizeof(movement_flash_payload_t))
#define FLASH_PROGRAM_SIZE      (((FLASH_PAYLOAD_SIZE + FLASH_PAGE_SIZE_BYTES - 1u) / FLASH_PAGE_SIZE_BYTES) * FLASH_PAGE_SIZE_BYTES)

typedef union {
    movement_flash_payload_t payload;
    uint8_t raw[FLASH_PROGRAM_SIZE];
} movement_flash_image_t;

static uint8_t g_recorded_buttons[RECORD_BUFFER_SIZE];
static movement_flash_image_t g_flash_image;
static const size_t g_recorded_max = RECORD_BUFFER_SIZE;
static size_t g_recorded_count = 0;
static size_t g_playback_index = 0;
static buttons_t g_playback_buttons = {0};
static movement_recorder_status_t g_status = MOVEMENT_RECORDER_STATUS_IDLE;
static millis_t g_last_time = 0;

static const movement_flash_payload_t * const flash_payload =
    (const movement_flash_payload_t *)(FLASH_BASE_ADDRESS + FLASH_TARGET_OFFSET);

static bool flash_has_valid_recording(void)
{
    if (flash_payload->magic != MOVEMENT_MAGIC)
        return false;

    if (flash_payload->count > RECORD_BUFFER_SIZE)
        return false;

    return true;
}

void MovementRecorder_Init(void)
{
    g_recorded_count = 0;
    g_playback_index = 0;
    g_playback_buttons.all = 0;
    g_status = MOVEMENT_RECORDER_STATUS_IDLE;
    g_last_time = millis;
}

void MovementRecorder_StartRecording(void)
{
    g_recorded_count = 0;
    g_status = MOVEMENT_RECORDER_STATUS_RECORDING;
    g_last_time = millis;
}

void MovementRecorder_StopRecording(void)
{
    g_status = MOVEMENT_RECORDER_STATUS_IDLE;

    memset(&g_flash_image, 0xFF, sizeof(g_flash_image));

    g_flash_image.payload.magic = MOVEMENT_MAGIC;
    g_flash_image.payload.count = (uint32_t)g_recorded_count;
    memcpy(g_flash_image.payload.data, g_recorded_buttons, g_recorded_count);

    multicore_lockout_start_blocking();
    uint32_t ints = save_and_disable_interrupts();

    flash_range_erase(FLASH_TARGET_OFFSET, FLASH_ERASE_SIZE);
    flash_range_program(FLASH_TARGET_OFFSET, g_flash_image.raw, FLASH_PROGRAM_SIZE);

    restore_interrupts(ints);
    multicore_lockout_end_blocking();
}

void MovementRecorder_StartReplay(void)
{
    g_playback_index = 0;
    g_playback_buttons.all = 0;

    if (!flash_has_valid_recording() || flash_payload->count == 0) {
        g_status = MOVEMENT_RECORDER_STATUS_IDLE;
        return;
    }

    g_status = MOVEMENT_RECORDER_STATUS_REPLAYING;
    g_last_time = millis - 10;
}

void MovementRecorder_StopReplay(void)
{
    g_status = MOVEMENT_RECORDER_STATUS_IDLE;
}

void MovementRecorder_CurrentValues(buttons_t buttons)
{
    if (g_status != MOVEMENT_RECORDER_STATUS_RECORDING)
        return;

    if ((millis - g_last_time) >= 10)
    {
        g_last_time = millis;

        if (g_recorded_count < g_recorded_max)
            g_recorded_buttons[g_recorded_count++] = buttons.all;
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

    if ((millis - g_last_time) >= 10)
    {
        g_last_time = millis;

        if (g_playback_index < flash_payload->count)
            g_playback_buttons.all = flash_payload->data[g_playback_index++];
        else
        {
            g_status = MOVEMENT_RECORDER_STATUS_IDLE;
            g_playback_buttons.all = 0;
        }
    }

    return g_playback_buttons;
}

bool MovementRecorder_IsEmpty(void)
{
    return !flash_has_valid_recording() || (flash_payload->count == 0);
}

movement_recorder_status_t MovementRecorder_GetStatus(void)
{
    return g_status;
}
