#ifndef MOVEMENT_RECORDER_H
#define MOVEMENT_RECORDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "NEN-project/raycasting.h"
#include "NEN-project/utils.h"

typedef enum
{
    MOVEMENT_RECORDER_STATUS_IDLE = 0,
    MOVEMENT_RECORDER_STATUS_RECORDING = 1,
    MOVEMENT_RECORDER_STATUS_REPLAYING = 2,
} movement_recorder_status_t;

void MovementRecorder_Init(void);
void MovementRecorder_StartRecording(void);
void MovementRecorder_StopRecording(void);
void MovementRecorder_StartReplay(void);
void MovementRecorder_StopReplay(void);
void MovementRecorder_CurrentValues(buttons_t buttons);
bool MovementRecorder_IsEmpty(void);
buttons_t MovementRecorder_GetPlaybackValues(void);
movement_recorder_status_t MovementRecorder_GetStatus(void);

#endif // MOVEMENT_RECORDER_H
