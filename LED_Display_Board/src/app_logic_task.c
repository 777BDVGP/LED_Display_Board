// =============================================================================
// app_logic_task.c — Event Firing Task
// =============================================================================
// See app_logic_task.h. Ported from LED_Display_Board_ARD's 08_APPEvents.cpp
// (AppLogicTask).
// =============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "app_logic_task.h"
#include "app_types.h"
#include "datetime.h"
#include "event_store.h"
#include "files.h"
#include "globals.h"
#include "ntfy.h"
#include "sensors.h"
#include "web_about.h"
#include "ws_hub.h"

static const char *TAG = "app_logic";

// A DISABLED event still "fires" at its scheduled time (flips to done/FIRED,
// same as normal) but must not reach the LED display or send an NTFY push.
// Likewise a same-day TIME CHANGE (S_EVT_TIME_OVR) only ever applies to
// today's occurrence. Both are one-occurrence overrides, so they're cleared
// and re-synced to peers here once consumed — otherwise a recurring event
// (e.g. yearly birthday) disabled/retimed once would stay that way forever.
static void clearOverridesAndSync(EventRuntime *ev, bool clearDisabled, bool clearTimeOverride)
{
    char path[96];
    snprintf(path, sizeof(path), "%s%s", SPIFFS_BASE_PATH, ev->filename);

    EventRecord rec;
    Event_InitDefaults(&rec);
    if (!Event_ReadData(path, &rec)) return;

    if (clearDisabled)     strlcpy(rec.fields[S_EVT_DISABLED].value, "false", EVENT_VALUE_LEN);
    if (clearTimeOverride) strlcpy(rec.fields[S_EVT_TIME_OVR].value, "",      EVENT_VALUE_LEN);

    // Event_WriteData()->Event_GetFilename() auto-increments past any filename
    // that already exists (its Create-flow collision-avoidance) — remove the
    // original first so it lands back on the same version/name instead of a
    // fresh duplicate file.
    remove(path);
    Event_WriteData(&rec);
    enqueueSyncSave(ev->filename);
}

void AppLogicTask(void *parameter)
{
    (void)parameter;

    if (xSemaphoreTake(timeSyncSemaphore, portMAX_DELAY) != pdTRUE) {
        vTaskDelete(NULL);
        return;
    }
    xSemaphoreGive(timeSyncSemaphore);
    ESP_LOGI(TAG, "AppLogicTask started");

    for (;;) {
        char checkTime[6];
        GetCurrentTime(checkTime, sizeof(checkTime));

        for (int i = 0; i < g_eventsDisplayCount; i++) {
            EventRuntime *ev = &g_eventsDisplay[i];
            if (ev->done || !ev->active) continue;
            if (strcmp(checkTime, ev->time) < 0) continue;

            // Additional radar check
            if (ev->radar && !Sensors_GetRadarSense()) continue;

            bool wasDisabled = ev->disabled;
            ev->done = true;

            if (!wasDisabled) {
                DisplayMessage disp;
                memset(&disp, 0, sizeof(disp));
                strlcpy(disp.label,  ev->label,  sizeof(disp.label));
                strlcpy(disp.mess,   ev->mess,   sizeof(disp.mess));
                strlcpy(disp.color,  ev->color,  sizeof(disp.color));
                strlcpy(disp.buzzer, ev->buzzer, sizeof(disp.buzzer));
                disp.repeat = ev->repeat;

                // --- Age calculation (years only) ---
                if (ev->show_age && strlen(ev->date) >= 4) {
                    int birthYear = atoi(ev->date);
                    char todayStr[11];
                    GetCurDisplayDate(todayStr, sizeof(todayStr));
                    int curYear = atoi(todayStr);
                    snprintf(disp.age, sizeof(disp.age), "%d years", curYear - birthYear);
                } else {
                    disp.age[0] = '\0';
                }

                if (ev->ntfy) {
                    char ntfyMsg[128];
                    if (disp.age[0] != '\0') {
                        snprintf(ntfyMsg, sizeof(ntfyMsg), "%s - %s - %s", ev->label, ev->mess, disp.age);
                    } else {
                        snprintf(ntfyMsg, sizeof(ntfyMsg), "%s - %s", ev->label, ev->mess);
                    }
                    Ntfy_Send(ntfyMsg, ev->ntfy_priority, ev->ntfy_topic);
                }

                xQueueSend(displayQueue, &disp, 0); // no wait
            }

            // Auto-delete one-time Reminders after delivery. Each clock deletes
            // its own copy — no peer sync needed, since every clock received the
            // file at save time and fires+deletes independently.
            if (ev->one_time && strcmp(ev->type, EVY_TYPE_REM) == 0) {
                char path[96];
                snprintf(path, sizeof(path), "%s%s", SPIFFS_BASE_PATH, ev->filename);
                remove(path);
            } else if (wasDisabled || ev->time_overridden) {
                // Recurring event: clear the one-occurrence override(s) so the
                // next occurrence isn't silently skipped/retimed too.
                clearOverridesAndSync(ev, wasDisabled, ev->time_overridden);
            }

            // Tell any open Home page (this clock's own browser, not just
            // peers receiving a sync) to refresh so PENDING/DISABLED flips to
            // FIRED immediately instead of waiting for a manual reload.
            WS_Broadcast("{\"type\":\"reload_home\"}");

            break;
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
