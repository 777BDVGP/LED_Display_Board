// =============================================================================
// datetime.c — Date/Time Compilation & Formatting
// =============================================================================
// See datetime.h. Ported from LED_Display_Board_ARD's 02_Time.cpp.
// =============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_system.h"

#include "datetime.h"
#include "display_colors.h"
#include "event_store.h"
#include "files.h"
#include "settings_store.h"
#include "timer_engine.h"

char DateTime[40] = "";
int  MonthDay = -1;
int  WeekDay  = 1;

static const char *YR_SHORT[]    = { "J","F","M","A","M","U","Y","A","S","O","N","D" };
static const char *YR_MONTH[]    = { "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" };
static const char *WEEK_DAY_NM[] = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat","Sun" };

void DetermineWeekday(void)
{
    time_t now = time(NULL);
    struct tm ti;
    localtime_r(&now, &ti);
    WeekDay = (ti.tm_wday == 0) ? 1 : (ti.tm_wday + 1);
}

void CompileDateTime(void)
{
    time_t now = time(NULL);
    struct tm ti;
    localtime_r(&now, &ti);

    snprintf(DateTime, sizeof(DateTime), "%02d %s %04d %02d:%02d:%02d %s",
             ti.tm_mday, YR_MONTH[ti.tm_mon], 1900 + ti.tm_year,
             ti.tm_hour, ti.tm_min, ti.tm_sec, WEEK_DAY_NM[ti.tm_wday]);

    // Daily maintenance — triggers once per day at midnight / startup.
    if (MonthDay != ti.tm_mday) {
        MonthDay = ti.tm_mday;
        DetermineWeekday();
        Event_RefreshTodayList();
        ReduceFiles();
        // TODO(Phase F): Dec-31 next-year public-holiday pre-fetch once that module is ported.
    }

    // Daily reboot at the configured S_MIDNIGHT_REBOOT_TIME (default 00:05),
    // gated by S_MIDNIGHT_REBOOT_ENABLE. millis() > 300000 (5 min uptime)
    // prevents a boot-loop if the ESP32 restarts right at the reboot minute.
    static bool midnightRebootDone = false;
    char rebootTime[SETTING_VALUE_LEN];
    Settings_GetText(S_MIDNIGHT_REBOOT_TIME, rebootTime, sizeof(rebootTime));
    int rebootMins = HHMMToMinutes(rebootTime);
    int nowMins = ti.tm_hour * 60 + ti.tm_min;
    if (Settings_GetBool(S_MIDNIGHT_REBOOT_ENABLE) && rebootMins >= 0 && nowMins == rebootMins) {
        if (!midnightRebootDone && millis() > 300000UL) {
            midnightRebootDone = true;
            SaveGenReport("Daily reboot");
            vTaskDelay(pdMS_TO_TICKS(100));
            esp_restart();
        }
    } else {
        midnightRebootDone = false;
    }
}

void ReduceFiles(void)
{
    // Log rotation disabled — matches ARD (see 02_Time.cpp's commented-out body).
}

void ReduceFile(const char *filename, int entries)
{
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", SPIFFS_BASE_PATH, filename);

    FILE *f = fopen(path, "r");
    if (!f) return;

    char **lines = NULL;
    int count = 0, cap = 0;
    char buf[256];
    while (fgets(buf, sizeof(buf), f)) {
        size_t len = strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) buf[--len] = '\0';
        if (count >= cap) {
            cap = cap ? cap * 2 : 32;
            lines = realloc(lines, cap * sizeof(char *));
        }
        lines[count++] = strdup(buf);
    }
    fclose(f);

    if (count > entries) {
        char tmppath[96];
        snprintf(tmppath, sizeof(tmppath), "%s/temp.txt", SPIFFS_BASE_PATH);
        FILE *tf = fopen(tmppath, "w");
        if (tf) {
            for (int i = count - entries; i < count; i++) {
                fprintf(tf, "%s\n", lines[i]);
            }
            fclose(tf);
            remove(path);
            rename(tmppath, path);
        }
    }

    for (int i = 0; i < count; i++) free(lines[i]);
    free(lines);
}

void GetCurrentDayMonthYear(char *out, size_t outsz)
{
    time_t now = time(NULL);
    struct tm ti;
    localtime_r(&now, &ti);
    snprintf(out, outsz, "%02d %s %02d", ti.tm_mday, YR_MONTH[ti.tm_mon], (ti.tm_year + 1900) % 100);
}

void GetCurrentDateAndTime(char *out, size_t outsz)
{
    time_t now = time(NULL);
    struct tm ti;
    localtime_r(&now, &ti);
    snprintf(out, outsz, "%04d-%02d-%02d %02d:%02d", 1900 + ti.tm_year, ti.tm_mon + 1, ti.tm_mday, ti.tm_hour, ti.tm_min);
}

void GetCurDisplayDate(char *out, size_t outsz)
{
    time_t now = time(NULL);
    struct tm ti;
    localtime_r(&now, &ti);
    snprintf(out, outsz, "%04d-%02d-%02d", 1900 + ti.tm_year, ti.tm_mon + 1, ti.tm_mday);
}

void GetCurrentTime(char *out, size_t outsz)
{
    time_t now = time(NULL);
    struct tm ti;
    localtime_r(&now, &ti);
    snprintf(out, outsz, "%02d:%02d", ti.tm_hour, ti.tm_min);
}

void GetCurrentDayHrMinSec(char *out, size_t outsz)
{
    time_t now = time(NULL);
    struct tm ti;
    localtime_r(&now, &ti);
    snprintf(out, outsz, "%s%02d:%02d%02d", WEEK_DAY_NM[ti.tm_wday], ti.tm_hour, ti.tm_min, ti.tm_sec);
}

void GetCurrentDayHrMinSecMonthDate(char *out, size_t outsz)
{
    time_t now = time(NULL);
    struct tm ti;
    localtime_r(&now, &ti);

    char buf[14];
    snprintf(buf, sizeof(buf), "%s%02d:%02d%02d%02d%s",
             WEEK_DAY_NM[ti.tm_wday], ti.tm_hour, ti.tm_min, ti.tm_sec, ti.tm_mday, YR_SHORT[ti.tm_mon]);

    // Compact single-digit days: shift characters left, pad with a trailing space.
    if (buf[10] == '0') {
        buf[10] = buf[11];
        buf[11] = buf[12];
        buf[12] = ' ';
        buf[13] = '\0';
    }

    strlcpy(out, buf, outsz);
}
