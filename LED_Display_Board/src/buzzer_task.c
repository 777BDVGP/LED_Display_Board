// =============================================================================
// buzzer_task.c — Buzzer Driver
// =============================================================================
// See buzzer_task.h. Ported from LED_Display_Board_ARD's 09_Other.cpp.
// =============================================================================

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_log.h"

#include "app_config.h"
#include "app_types.h"
#include "buzzer_task.h"
#include "event_store.h"
#include "globals.h"
#include "settings_store.h"
#include "timer_engine.h"

static const char *TAG = "buzzer";

void triggerBuzzer(const char *pattern)
{
    BuzzerMessage msg;
    strlcpy(msg.pattern, pattern, sizeof(msg.pattern));

    if (xQueueSend(buzzerQueue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Buzzer queue full!");
    }
}

void BuzzerTask(void *parameter)
{
    (void)parameter;
    vTaskDelay(pdMS_TO_TICKS(5000));

    gpio_reset_pin(BUZZER_PIN);
    gpio_set_direction(BUZZER_PIN, GPIO_MODE_OUTPUT);

    BuzzerMessage msg;
    char currentPattern[16];
    strlcpy(currentPattern, BUZ_NONE, sizeof(currentPattern));
    uint32_t lastToggle = 0;
    bool buzzerState = false;
    int beepCount = 0;
    int maxBeeps = 0;
    int onTime = 0;
    int offTime = 0;
    bool isContinuous = false;

    bool testShortActive = false;
    bool testLongActive = false;
    bool testConsShortActive = false;
    bool testConsLongActive = false;

    if (xSemaphoreTake(timeSyncSemaphore, portMAX_DELAY) != pdTRUE) {
        vTaskDelete(NULL);
        return;
    }
    xSemaphoreGive(timeSyncSemaphore);
    ESP_LOGI(TAG, "BuzzerTask started");

    for (;;) {
        // --- Test patterns (checkboxes, priority order) ---
        bool shortTest = Settings_GetBool(S_BUZ_3_SHORT_TEST);
        if (shortTest && !testShortActive) {
            triggerBuzzer(BUZ_SHORT);
            testShortActive = true;
        } else if (!shortTest && testShortActive) {
            triggerBuzzer(BUZ_NONE);
            testShortActive = false;
        }

        bool longTest = Settings_GetBool(S_BUZ_5_LONG_TEST);
        if (longTest && !testLongActive) {
            triggerBuzzer(BUZ_LONG);
            testLongActive = true;
        } else if (!longTest && testLongActive) {
            triggerBuzzer(BUZ_NONE);
            testLongActive = false;
        }

        bool consShortTest = Settings_GetBool(S_BUZ_CONS_SHORT_TEST);
        if (consShortTest && !testConsShortActive) {
            triggerBuzzer(BUZ_CONT_SH);
            testConsShortActive = true;
        } else if (!consShortTest && testConsShortActive) {
            triggerBuzzer(BUZ_NONE);
            testConsShortActive = false;
        }

        bool consLongTest = Settings_GetBool(S_BUZ_CONS_LONG_TEST);
        if (consLongTest && !testConsLongActive) {
            triggerBuzzer(BUZ_CONT_LO);
            testConsLongActive = true;
        } else if (!consLongTest && testConsLongActive) {
            triggerBuzzer(BUZ_NONE);
            testConsLongActive = false;
        }

        // --- Command events from queue ---
        if (xQueueReceive(buzzerQueue, &msg, pdMS_TO_TICKS(10)) == pdTRUE) {
            strlcpy(currentPattern, msg.pattern, sizeof(currentPattern));
            beepCount = 0;
            buzzerState = true;
            gpio_set_level(BUZZER_PIN, 1);
            lastToggle = millis();

            if (strcmp(currentPattern, BUZ_NONE) == 0) {
                isContinuous = false;
                maxBeeps = 0;
                gpio_set_level(BUZZER_PIN, 0);
            } else if (strcmp(currentPattern, BUZ_SHORT) == 0) {
                isContinuous = false;
                maxBeeps = 3;
                onTime  = Settings_GetNumber(S_BUZ_3_SHORT_ON);
                offTime = Settings_GetNumber(S_BUZ_3_SHORT_OFF);
            } else if (strcmp(currentPattern, BUZ_LONG) == 0) {
                isContinuous = false;
                maxBeeps = 5;
                onTime  = Settings_GetNumber(S_BUZ_5_LONG_ON);
                offTime = Settings_GetNumber(S_BUZ_5_LONG_OFF);
            } else if (strcmp(currentPattern, BUZ_CONT_SH) == 0) {
                isContinuous = true;
                onTime  = Settings_GetNumber(S_BUZ_CONS_SHORT_ON);
                offTime = Settings_GetNumber(S_BUZ_CONS_SHORT_OFF);
            } else if (strcmp(currentPattern, BUZ_CONT_LO) == 0) {
                isContinuous = true;
                onTime  = Settings_GetNumber(S_BUZ_CONS_LONG_ON);
                offTime = Settings_GetNumber(S_BUZ_CONS_LONG_OFF);
            } else if (strcmp(currentPattern, BUZ_EX1) == 0) {
                isContinuous = false; maxBeeps = 1;
                onTime  = Settings_GetNumber(S_BUZ_3_SHORT_ON);
                offTime = Settings_GetNumber(S_BUZ_3_SHORT_OFF);
            } else if (strcmp(currentPattern, BUZ_EX2) == 0) {
                isContinuous = false; maxBeeps = 2;
                onTime  = Settings_GetNumber(S_BUZ_3_SHORT_ON);
                offTime = Settings_GetNumber(S_BUZ_3_SHORT_OFF);
            } else if (strcmp(currentPattern, BUZ_EX3) == 0) {
                isContinuous = false; maxBeeps = 3;
                onTime  = Settings_GetNumber(S_BUZ_3_SHORT_ON);
                offTime = Settings_GetNumber(S_BUZ_3_SHORT_OFF);
            } else if (strcmp(currentPattern, BUZ_EX4) == 0) {
                isContinuous = false; maxBeeps = 4;
                onTime  = Settings_GetNumber(S_BUZ_3_SHORT_ON);
                offTime = Settings_GetNumber(S_BUZ_3_SHORT_OFF);
            } else if (strcmp(currentPattern, BUZ_EX_SET) == 0) {
                isContinuous = false; maxBeeps = 1;
                onTime = 1000; offTime = 0;
            } else if (strcmp(currentPattern, BUZ_EX_DONE) == 0) {
                isContinuous = false; maxBeeps = 3;
                onTime = 500; offTime = 500;
            } else {
                ESP_LOGW(TAG, "Unknown pattern: %s", currentPattern);
                strlcpy(currentPattern, BUZ_NONE, sizeof(currentPattern));
            }
        }

        // --- Pattern execution ---
        if (strcmp(currentPattern, BUZ_NONE) != 0) {
            uint32_t now = millis();

            if (buzzerState) {
                if (now - lastToggle >= (uint32_t)onTime) {
                    gpio_set_level(BUZZER_PIN, 0);
                    buzzerState = false;
                    lastToggle = now;

                    if (!isContinuous) {
                        beepCount++;
                        if (beepCount >= maxBeeps) {
                            strlcpy(currentPattern, BUZ_NONE, sizeof(currentPattern));
                        }
                    }
                }
            } else if (isContinuous || beepCount < maxBeeps) {
                if (now - lastToggle >= (uint32_t)offTime) {
                    gpio_set_level(BUZZER_PIN, 1);
                    buzzerState = true;
                    lastToggle = now;
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
