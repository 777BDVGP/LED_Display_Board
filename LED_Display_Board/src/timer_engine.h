// =============================================================================
// timer_engine.h — Non-Blocking Software Timers (ESP-IDF port of 02_Time.h)
// =============================================================================
// Named timer_engine (not timers.h) to avoid any confusion with FreeRTOS's
// own "freertos/timers.h".
// =============================================================================

#ifndef TIMER_ENGINE_H
#define TIMER_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_timer.h"

#define TIM_LED_FLASHING_SPEED     0   // Heartbeat LED toggle interval (1500 ms)
#define TIM_HUMAN_DETECT_STAY_ON   1   // Display-on delay after radar (5 min)
#define TIM_TOTAL                  2

#define MS   1        // Milliseconds multiplier
#define SEC  1000      // Seconds multiplier (units x 1000 ms)
#define MIN  60000      // Minutes multiplier (units x 60000 ms)

// Arduino-style millis() — used for non-blocking timeout comparisons, not
// wall-clock display (that's datetime.h, backed by NTP-synced time()).
static inline uint32_t millis(void) { return (uint32_t)(esp_timer_get_time() / 1000ULL); }

void Timers_Maintain(void);                                          // call every ~50 ms
void Timers_Activate(int index);
void Timers_Deactivate(int index);
bool Timers_Expired(int index);
void Timers_SetValue(int index, unsigned int units, unsigned int timerUnits);
int  Timers_RemainingSec(int index);

#endif // TIMER_ENGINE_H
