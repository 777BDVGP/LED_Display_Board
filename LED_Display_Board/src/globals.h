// =============================================================================
// globals.h — Shared FreeRTOS Primitive Externs
// =============================================================================
// Defined in main.c; extern'd here so any module can reference them without
// re-declaring. Mirrors the "Global Variable Externs" section of ARD's
// 01_Main.h.
// =============================================================================

#ifndef GLOBALS_H
#define GLOBALS_H

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

// Bumped by DisplayTask on every render loop iteration (and every scroll step).
// TimeCheckTask's liveness watchdog reboots the board if this stops advancing
// while no exercise is running — catches DisplayTask blocked on RMT / a mutex /
// the queue, which no other watchdog recovers. Does not touch the LED transfer.
extern volatile uint32_t g_displayHeartbeat;

extern QueueHandle_t     displayQueue;      // Sends DisplayMessage items to DisplayTask
extern QueueHandle_t     buzzerQueue;       // Sends BuzzerMessage items to BuzzerTask
extern QueueHandle_t     eventSyncQueue;    // Sends EventSyncCmd items to ClockNetworkTask
extern SemaphoreHandle_t timeSyncSemaphore; // Released by TimeSyncTask when NTP is ready
extern SemaphoreHandle_t timersMutex;       // Guards Timer[] array
extern SemaphoreHandle_t eventSemaphore;    // Signals page-selection change events
extern SemaphoreHandle_t variabMutex;       // Guards shared application variables

#endif // GLOBALS_H
