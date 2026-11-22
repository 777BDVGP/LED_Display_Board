// =============================================================================
// timer_engine.c — Non-Blocking Software Timers
// =============================================================================
// See timer_engine.h. Ported from LED_Display_Board_ARD's 02_Time.cpp.
// =============================================================================

#include "freertos/FreeRTOS.h"

#include "globals.h"
#include "timer_engine.h"

typedef struct {
    bool     status;      // true = running
    unsigned units;
    unsigned timerUnits;  // MS / SEC / MIN
    uint32_t expireTime;  // millis() value at which the timer expires
} TimerEntry;

static TimerEntry s_timers[TIM_TOTAL] = {
    { false, 1500, MS,  0 },  // TIM_LED_FLASHING_SPEED
    { false,    5, MIN, 0 },  // TIM_HUMAN_DETECT_STAY_ON
};

void Timers_Maintain(void)
{
    if (xSemaphoreTake(timersMutex, portMAX_DELAY) == pdTRUE) {
        uint32_t now = millis();
        for (int i = 0; i < TIM_TOTAL; i++) {
            if (s_timers[i].status && now > s_timers[i].expireTime) {
                s_timers[i].status = false;
            }
        }
        xSemaphoreGive(timersMutex);
    }
}

void Timers_Activate(int index)
{
    if (index < 0 || index >= TIM_TOTAL) return;
    if (xSemaphoreTake(timersMutex, portMAX_DELAY) == pdTRUE) {
        s_timers[index].expireTime = millis() + (s_timers[index].units * s_timers[index].timerUnits);
        s_timers[index].status = true;
        xSemaphoreGive(timersMutex);
    }
}

void Timers_Deactivate(int index)
{
    if (index < 0 || index >= TIM_TOTAL) return;
    if (xSemaphoreTake(timersMutex, portMAX_DELAY) == pdTRUE) {
        s_timers[index].status = false;
        xSemaphoreGive(timersMutex);
    }
}

bool Timers_Expired(int index)
{
    if (index < 0 || index >= TIM_TOTAL) return true;
    bool result = true;
    if (xSemaphoreTake(timersMutex, portMAX_DELAY) == pdTRUE) {
        result = !s_timers[index].status;
        xSemaphoreGive(timersMutex);
    }
    return result;
}

void Timers_SetValue(int index, unsigned int units, unsigned int timerUnits)
{
    if (index < 0 || index >= TIM_TOTAL) return;
    if (xSemaphoreTake(timersMutex, portMAX_DELAY) == pdTRUE) {
        s_timers[index].units = units;
        s_timers[index].timerUnits = timerUnits;
        xSemaphoreGive(timersMutex);
    }
}

int Timers_RemainingSec(int index)
{
    if (index < 0 || index >= TIM_TOTAL) return 0;
    int result = 0;
    uint32_t now = millis();
    if (xSemaphoreTake(timersMutex, portMAX_DELAY) == pdTRUE) {
        if (s_timers[index].status) {
            result = (now > s_timers[index].expireTime) ? 0 : (int)((s_timers[index].expireTime - now) / SEC);
        }
        xSemaphoreGive(timersMutex);
    }
    return result;
}
