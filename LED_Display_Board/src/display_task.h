// =============================================================================
// display_task.h — LED Matrix Display Task
// =============================================================================
// First cut: scrolls "DAY HH:MM:SS" across the matrix once NTP time is
// available. Ported/rebuilt from ARD's DisplayTask (08_APPEvents.cpp), which
// will grow to also handle queued display events (displayQueue) once
// AppLogicTask and the event system are ported.
// =============================================================================

#ifndef DISPLAY_TASK_H
#define DISPLAY_TASK_H

void DisplayTask(void *parameter);

#endif // DISPLAY_TASK_H
