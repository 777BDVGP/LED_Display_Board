// =============================================================================
// app_logic_task.h — Event Firing Task (ESP-IDF port of 08_APPEvents.cpp)
// =============================================================================

#ifndef APP_LOGIC_TASK_H
#define APP_LOGIC_TASK_H

// FreeRTOS task — polls g_eventsDisplay every second and fires due events
// (optional radar gate) into displayQueue for DisplayTask to render.
void AppLogicTask(void *parameter);

#endif // APP_LOGIC_TASK_H
