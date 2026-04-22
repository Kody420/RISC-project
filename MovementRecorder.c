#include "MovementRecorder.h"
#include <inttypes.h>
#include <stddef.h>

buttons_t g_recorded_buttons[80000]; // at 10ms per sample, this allows for about 16 minutes of recording
const size_t g_recorded_max = sizeof(g_recorded_buttons) / sizeof(g_recorded_buttons[0]);
size_t g_recorded_count = 0;
size_t g_playback_index = 0;
movement_recorder_status_t g_status = MOVEMENT_RECORDER_STATUS_IDLE;
millis_t g_last_time = 0;


void MovementRecorder_Init(void)
{
    g_recorded_count = 0;
    g_playback_index = 0;
    g_status = MOVEMENT_RECORDER_STATUS_IDLE;
}

void MovementRecorder_StartRecording(void)
{
    g_status = MOVEMENT_RECORDER_STATUS_RECORDING;
}

void MovementRecorder_StopRecording(void)
{
    g_status = MOVEMENT_RECORDER_STATUS_IDLE;
}

void MovementRecorder_StartReplay(void)
{
    g_playback_index = 0;
    g_status = MOVEMENT_RECORDER_STATUS_REPLAYING;
}

void MovementRecorder_StopReplay(void)
{
    g_status = MOVEMENT_RECORDER_STATUS_IDLE;
}

void MovementRecorder_CurrentValues(buttons_t buttons)
{
    if (g_status == MOVEMENT_RECORDER_STATUS_RECORDING)
    {
        if (millis - g_last_time >= 10)
        {
            g_last_time = millis;
            if (g_recorded_count < g_recorded_max)
                g_recorded_buttons[g_recorded_count++] = buttons;
        }
    }
}

buttons_t MovementRecorder_GetPlaybackValues(void)
{
    if (g_status == MOVEMENT_RECORDER_STATUS_REPLAYING)
    {
        if (millis - g_last_time >= 10)
        {
            g_last_time = millis;
            if (g_playback_index < g_recorded_count)
                return g_recorded_buttons[g_playback_index++];
        }
    }

    buttons_t empty = {0};
    return empty;
}

movement_recorder_status_t MovementRecorder_GetStatus(void)
{
    return g_status;
}
