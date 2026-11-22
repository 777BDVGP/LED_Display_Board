// =============================================================================
// display_task.c — LED Matrix Display Task
// =============================================================================
// See display_task.h. Ported from LED_Display_Board_ARD's 08_APPEvents.cpp
// (DisplayTask). Renders queued events (label/message/age, scrolling, repeat)
// when displayQueue has data; otherwise shows the clock face (weekday/date
// alternation, day/night colour, LDR debug mode).
//
// Exercise-mode hand-off: when exerciseActive is set (exercises.c), the clock
// loop below is skipped entirely and runExercise() takes the matrix instead.
// =============================================================================

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "app_config.h"
#include "app_types.h"
#include "buzzer_task.h"
#include "datetime.h"
#include "display_colors.h"
#include "display_task.h"
#include "event_store.h"
#include "exercises.h"
#include "font5x7.h"
#include "globals.h"
#include "led_matrix.h"
#include "sensors.h"
#include "settings_store.h"

static const char *TAG = "display";

void DisplayTask(void *parameter)
{
    (void)parameter;

    if (xSemaphoreTake(timeSyncSemaphore, portMAX_DELAY) != pdTRUE) {
        vTaskDelete(NULL);
        return;
    }
    xSemaphoreGive(timeSyncSemaphore);

    matrix_init(MATRIX_PIN);
    ESP_LOGI(TAG, "DisplayTask started");

    char lastTime[14] = "";
    DisplayMessage evt;
    int  repeatValue;
    bool endless;

    int  yearDateCount = 2;
    int  weekDayCount  = 2;
    bool displayWeekDayCount = false;
    char first3[4] = "";

    // Ensure display is on until the next human trigger — must happen before
    // the first brightness sample so the stay-on timer is active for it.
    // Uses HumanDetect_ActivateStayOn() (not a raw Timers_Activate) so the
    // very first arm-on-boot already uses the configured Day/Night minutes
    // instead of TIM_HUMAN_DETECT_STAY_ON's hardcoded 5-min default.
    HumanDetect_ActivateStayOn();

    int cachedBrightness = Sensors_GetBrightness();
    TickType_t lastBrightUpdate = xTaskGetTickCount();
    matrix_set_brightness((uint8_t)cachedBrightness);

    for (;;) {
        // Liveness heartbeat — TimeCheckTask reboots the board if this stops
        // advancing (DisplayTask wedged on RMT / a mutex / the queue).
        g_displayHeartbeat++;

        // ── Exercise mode — run sequence then resume clock ──────────────────
        if (exerciseActive) {
            ExerciseData ex;
            if (xSemaphoreTake(variabMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                memcpy(&ex, &pendingExercise, sizeof(ExerciseData));
                xSemaphoreGive(variabMutex);
                runExercise(&ex);
            }
            exerciseActive = false;
            continue;
        }

        // Radar check every pass (~100-600 ms), independent of the 5 s LDR
        // refresh below — sampling the radar only inside that refresh missed
        // pulses that fell between samples, so the display was slow to wake.
        // Sensors_TakeRadarTrigger() also reports pulses latched by the ISR.
        if (Settings_GetBool(S_DISPLAY_ON_HUMAN_DET) && Sensors_TakeRadarTrigger()) {
            HumanDetect_ActivateStayOn();
            if (cachedBrightness == 0) { // display is off — wake it now
                cachedBrightness = Sensors_GetBrightness();
                matrix_set_brightness((uint8_t)cachedBrightness);
                lastBrightUpdate = xTaskGetTickCount();
            }
        }

        // Refresh brightness cache every 5 s (avoids ADC+delay every frame).
        if ((xTaskGetTickCount() - lastBrightUpdate) >= pdMS_TO_TICKS(5000)) {
            cachedBrightness = Sensors_GetBrightness();
            matrix_set_brightness((uint8_t)cachedBrightness);
            lastBrightUpdate = xTaskGetTickCount();
        }

        uint8_t cr = 255, cg = 255, cb = 255; // default white

        // -- Check event queue first --
        if (xQueueReceive(displayQueue, &evt, 0) == pdTRUE) {
            // Re-arm the stay-on timer AND apply brightness immediately — the
            // periodic 5s refresh below is too slow: without this, a message
            // queued while the display was off would draw invisibly (brightness
            // still 0) until the next refresh, sometimes finishing before it hit.
            HumanDetect_ActivateStayOn();
            cachedBrightness = Sensors_GetBrightness();
            matrix_set_brightness((uint8_t)cachedBrightness);
            lastBrightUpdate = xTaskGetTickCount();

            triggerBuzzer(evt.buzzer);

            // Optional pre-display delay showing today's date.
            int delayBeforeDisplay = Settings_GetNumber(S_BUZ_DELAY_B4_DISP);
            if (delayBeforeDisplay > 0) {
                char dateStr[12]; // "DD MMM YY"
                GetCurrentDayMonthYear(dateStr, sizeof(dateStr));
                char dd[3], mmm[4], yy[3];
                strlcpy(dd,  dateStr,     sizeof(dd));   // "DD"
                strlcpy(mmm, dateStr + 3, sizeof(mmm));  // "MMM"
                strlcpy(yy,  dateStr + 7, sizeof(yy));   // "YY"

                char colorStr[SETTING_VALUE_LEN];
                Settings_GetText(S_BUZ_DELAY_COLOR, colorStr, sizeof(colorStr));
                ParseRGBColor(colorStr, &cr, &cg, &cb);

                // Reduced spacing between fields — same 1px-tighter-than-adjacent
                // technique the HH:MM colon uses below: next field starts at
                // prev_x0 + prev_field_width - 1.
                int mmmX = 1 + font_text_width(dd) - 1;
                int yyX  = mmmX + font_text_width(mmm) - 1;

                matrix_clear();
                font_draw_text(dd,  1,    0, cr, cg, cb);
                font_draw_text(mmm, mmmX, 0, cr, cg, cb);
                font_draw_text(yy,  yyX,  0, cr, cg, cb);
                matrix_show();
                vTaskDelay(pdMS_TO_TICKS(delayBeforeDisplay));
            }

            // Build scroll text (label + mess + optional age). Sized so GCC can
            // prove the worst case (label[64]+mess[64]+age[12]+separators) always
            // fits — ARD used a plain 128-byte buffer, which risked silent
            // truncation in that same worst case.
            char scrollTxt[160];
            if (evt.age[0] != '\0') {
                snprintf(scrollTxt, sizeof(scrollTxt), "%s %s %s       ", evt.label, evt.mess, evt.age);
            } else {
                snprintf(scrollTxt, sizeof(scrollTxt), "%s %s       ", evt.label, evt.mess);
            }

            if      (strcmp(evt.color, ECO_RED)    == 0) { cr = 255; cg = 0;   cb = 0;   }
            else if (strcmp(evt.color, ECO_GREEN)  == 0) { cr = 0;   cg = 255; cb = 0;   }
            else if (strcmp(evt.color, ECO_BLUE)   == 0) { cr = 0;   cg = 0;   cb = 255; }
            else if (strcmp(evt.color, ECO_YELLOW) == 0) { cr = 255; cg = 255; cb = 0;   }
            else                                          { cr = 255; cg = 255; cb = 255; } // White default

            repeatValue = evt.repeat;
            endless = false;
            if (repeatValue < 1) {
                repeatValue = 3;
                endless = true;
            }

            for (int rep = 0; rep < repeatValue; rep++) {
                if (endless) rep--; // keeps the loop running until button-terminated

                if (Sensors_GetButtonTerminate()) break;

                int textWidth = (int)strlen(scrollTxt) * 6; // 5px font + 1px spacing
                int startX = MTX_WIDTH;
                int stopX  = -textWidth + MTX_WIDTH;

                for (int x = startX; x > stopX; x--) {
                    g_displayHeartbeat++; // keep alive through long/endless scrolls

                    if (Sensors_GetButtonTerminate()) {
                        x = stopX;          // exit inner scroll
                        rep = repeatValue;  // prevent further repeats
                        break;
                    }

                    matrix_clear();
                    font_draw_text(scrollTxt, x, 0, cr, cg, cb);
                    matrix_show();
                    vTaskDelay(pdMS_TO_TICKS(75));
                }

                if (Sensors_GetButtonTerminate()) break;
            }

            triggerBuzzer(BUZ_NONE);
            continue; // go back and check the next queued event
        }

        // -- Clock face --
        GetTimeDisplayColor(&cr, &cg, &cb);

        if (Settings_GetBool(S_DISP_TIME_PINK_HD) && Sensors_GetRadarSense()) {
            char colorStr[SETTING_VALUE_LEN];
            Settings_GetText(S_PINK_HD_COLOR, colorStr, sizeof(colorStr));
            ParseRGBColor(colorStr, &cr, &cg, &cb);
        }

        if (Settings_GetBool(S_DISP_LDR_INFO)) {
            char ldrData[10];
            Sensors_GetLDRDebugText(ldrData, sizeof(ldrData));
            matrix_clear();
            font_draw_text(ldrData, 1, 0, cr, cg, cb);
            matrix_show();
            vTaskDelay(pdMS_TO_TICKS(200));
        } else {
            char sysTimeDay[14]; // "DDDHH:MMSSDDM" (or compacted single-digit-day form)
            GetCurrentDayHrMinSecMonthDate(sysTimeDay, sizeof(sysTimeDay));

            if (strcmp(sysTimeDay, lastTime) != 0) {
                if (displayWeekDayCount) {
                    // ---- a) weekday + HH:MM (blinking colon) ----
                    strlcpy(first3, sysTimeDay, sizeof(first3)); // "DDD" weekday
                    if (weekDayCount > 0) weekDayCount--;
                    if (weekDayCount == 0) {
                        weekDayCount = Settings_GetNumber(S_WEEKDAY_DISPLAY_SEC);
                        if (Settings_GetNumber(S_YEARDATE_DISPLAY_SEC)) {
                            displayWeekDayCount = false;
                        }
                    }

                    char hr[3], min[3];
                    strlcpy(hr,  sysTimeDay + 3, sizeof(hr));
                    strlcpy(min, sysTimeDay + 6, sizeof(min));

                    // With colon
                    matrix_clear();
                    font_draw_text(first3, 1,  0, cr, cg, cb);
                    font_draw_text(hr,     22, 0, cr, cg, cb);
                    font_draw_text(":",    32, 0, cr, cg, cb);
                    font_draw_text(min,    36, 0, cr, cg, cb);
                    matrix_show();

                    vTaskDelay(pdMS_TO_TICKS(500));

                    // Without colon (blink)
                    matrix_clear();
                    font_draw_text(first3, 1,  0, cr, cg, cb);
                    font_draw_text(hr,     22, 0, cr, cg, cb);
                    font_draw_text(min,    36, 0, cr, cg, cb);
                    matrix_show();
                } else {
                    if (yearDateCount > 0) yearDateCount--;
                    if (yearDateCount == 0) {
                        yearDateCount = Settings_GetNumber(S_YEARDATE_DISPLAY_SEC);
                        if (Settings_GetNumber(S_WEEKDAY_DISPLAY_SEC) != 0) {
                            displayWeekDayCount = true;
                        }
                    }

                    char yearDateStyle[SETTING_VALUE_LEN];
                    Settings_GetText(S_YEARDATE_STYLE, yearDateStyle, sizeof(yearDateStyle));

                    if (strcmp(yearDateStyle, "Date/Time") == 0) {
                        // ---- b) "DDY" (day + single-letter month) + HH:MM ----
                        strlcpy(first3, sysTimeDay + 10, sizeof(first3)); // "MDD" year-date

                        char hr[3], min[3];
                        strlcpy(hr,  sysTimeDay + 3, sizeof(hr));
                        strlcpy(min, sysTimeDay + 6, sizeof(min));

                        // With colon
                        matrix_clear();
                        font_draw_text(first3, 1,  0, cr, cg, cb);
                        font_draw_text(hr,     22, 0, cr, cg, cb);
                        font_draw_text(":",    32, 0, cr, cg, cb);
                        font_draw_text(min,    36, 0, cr, cg, cb);
                        matrix_show();

                        vTaskDelay(pdMS_TO_TICKS(500));

                        // Without colon (blink)
                        matrix_clear();
                        font_draw_text(first3, 1,  0, cr, cg, cb);
                        font_draw_text(hr,     22, 0, cr, cg, cb);
                        font_draw_text(min,    36, 0, cr, cg, cb);
                        matrix_show();
                    } else {
                        // ---- c) "DD MMM YY" — full date, no time. Fixed columns match
                        // DATE.jpg exactly: DD at x=1, MMM at x=15, YY at x=35 (2px gaps
                        // between fields, standard 6px/char spacing within each field). ----
                        char dateStr[12]; // "DD MMM YY"
                        GetCurrentDayMonthYear(dateStr, sizeof(dateStr));
                        char dd[3], mmm[4], yy[3];
                        strlcpy(dd,  dateStr,     sizeof(dd));
                        strlcpy(mmm, dateStr + 3, sizeof(mmm));
                        strlcpy(yy,  dateStr + 7, sizeof(yy));

                        matrix_clear();
                        font_draw_text(dd,  1,  0, cr, cg, cb);
                        font_draw_text(mmm, 15, 0, cr, cg, cb);
                        font_draw_text(yy,  35, 0, cr, cg, cb);
                        matrix_show();
                    }
                }

                strlcpy(lastTime, sysTimeDay, sizeof(lastTime));
            }
        }

        vTaskDelay(pdMS_TO_TICKS(100));
    }
}
