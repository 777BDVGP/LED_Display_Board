// =============================================================================
// buzzer_task.h — Buzzer Driver (ESP-IDF port of 09_Other.cpp)
// =============================================================================

#ifndef BUZZER_TASK_H
#define BUZZER_TASK_H

// Enqueues a BUZ_* pattern string into buzzerQueue (capacity 5, 100 ms
// timeout). Drops + logs on a full queue.
void triggerBuzzer(const char *pattern);

// FreeRTOS task — drains buzzerQueue and drives BUZZER_PIN accordingly, plus
// polls the four buzzer test checkboxes for edge-triggered test patterns.
void BuzzerTask(void *parameter);

#endif // BUZZER_TASK_H
