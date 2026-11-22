// =============================================================================
// settings_store.c — Application Settings (ESP-IDF port of SettingClass)
// =============================================================================
// See settings_store.h. Ported from LED_Display_Board_ARD's 07_Settings.cpp.
// =============================================================================

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "files.h"
#include "globals.h"
#include "settings_store.h"

static const char *TAG = "settings";

SettingEntry g_settings[SETTING_ENTRIES];

#define SETTINGS_PATH  SPIFFS_BASE_PATH "/Settings.json"

// -----------------------------------------------------------------------------
// Settings_InitDefaults
// Table-driven equivalent of ARD's SettingClass constructor. S_DISPLAY_LINE_*
// separators are left with an empty Info label (never rendered, never
// persisted — see the info[0]=='\0' guards in ReadData/WriteData below).
// -----------------------------------------------------------------------------
void Settings_InitDefaults(void)
{
    static const struct {
        int          idx;
        const char  *info;
        const char  *value;
        field_type_t type;
        bool         web;
    } d[] = {
        { S_DEVICE_NAME,            "OTA Hostname",                      "LED_CLOCK_LR",  FIELD_TEXT, true  },
        { S_SYSTEM_NAME,            "Device Name",                       "Clock System",  FIELD_TEXT, true  },
        { S_SAVELOG,                "Save Log Active",                   "true",          FIELD_BOOL, true  },
        { S_BIRTHDAY_TIME,          "Birthday Display Time",             "07:00",         FIELD_TIME, false },
        { S_DISPLAY_LINE_1,         "",                                  "",              FIELD_TEXT, true  },
        { S_DISP_LDR_INFO,          "Display LDR Data",                  "false",         FIELD_BOOL, true  },
        { S_LDR_RAW_MIN,            "Raw LDR Minimum",                   "600",           FIELD_NUMB, true  },
        { S_LDR_RAW_MAX,            "Raw LDR Maximum",                   "3500",          FIELD_NUMB, true  },
        { S_DISP_BRIGHT_MIN,        "Min Display Brightness",            "1",             FIELD_NUMB, true  },
        { S_DISP_BRIGHT_MAX,        "Max Display Brightness",            "10",            FIELD_NUMB, true  },
        { S_DISPLAY_LINE_2,         "",                                  "",              FIELD_TEXT, true  },
        { S_DISP_TIME_PINK_HD,      "On Human Detect DIFFERENT Colour",  "false",         FIELD_BOOL, true  },
        { S_PINK_HD_COLOR,          "DIFFERENT Colour (RGB)",            "255, 184, 193", FIELD_TEXT, true  },
        { S_DISPLAY_LINE_3,         "",                                  "",              FIELD_TEXT, true  },
        { S_DISPLAY_ON_HUMAN_DET,   "Time on only when Human Detect",    "false",         FIELD_BOOL, true  },
        { S_DAY_DETECT_OFF_START,   "Day Detect-Off Start",              "06:00",         FIELD_TIME, false },
        { S_DAY_DETECT_OFF_MIN,     "Day Time After Detect off (Min)",   "10",            FIELD_NUMB, true  },
        { S_NIGHT_DETECT_OFF_START, "Night Detect-Off Start",            "23:00",         FIELD_TIME, false },
        { S_NIGHT_DETECT_OFF_SEC,   "Night Time After Detect off (Sec)", "600",           FIELD_NUMB, true  },
        { S_DISPLAY_LINE_4,         "",                                  "",              FIELD_TEXT, true  },
        { S_BUZ_DELAY_B4_DISP,      "Delay Before Message Display (MS)", "0",             FIELD_NUMB, true  },
        { S_BUZ_DELAY_COLOR,        "Delay Before Display Colour (RGB)", "255, 184, 193", FIELD_TEXT, true  },
        { S_DISPLAY_LINE_5,         "",                                  "",              FIELD_TEXT, true  },
        { S_WEEKDAY_DISPLAY_SEC,    "Weekday Display (Sec)",             "1",             FIELD_NUMB, true  },
        { S_YEARDATE_DISPLAY_SEC,   "YearDate Display (Sec)",            "1",             FIELD_NUMB, true  },
        { S_YEARDATE_STYLE,         "YearDate Style",                    "Full Date",     FIELD_SELECT, false },
        { S_DISPLAY_LINE_6,         "",                                  "",              FIELD_TEXT, true  },
        { S_DAY_TIME_COLOR_START,   "Day Time Colour Start",             "06:00",         FIELD_TIME, true  },
        { S_NIGHT_TIME_COLOR_START, "Night Time Colour Start",           "23:00",         FIELD_TIME, true  },
        { S_DAY_TIME_COLOR,         "Day Time Colour (RGB)",             "200,200,200",   FIELD_TEXT, true  },
        { S_NIGHT_TIME_COLOR,       "Night Time Colour (RGB)",           "0,0,200",       FIELD_TEXT, true  },
        { S_DISPLAY_LINE_7,         "",                                  "",              FIELD_TEXT, true  },
        { S_BUZ_3_SHORT_TEST,       "Test Buzzer 3 Short",               "false",         FIELD_BOOL, true  },
        { S_BUZ_3_SHORT_ON,         "Buzzer 3 Short On (MS)",            "100",           FIELD_NUMB, true  },
        { S_BUZ_3_SHORT_OFF,        "Buzzer 3 Short Off (MS)",           "100",           FIELD_NUMB, true  },
        { S_BUZ_5_LONG_TEST,        "Test Buzzer 5 Long",                "false",         FIELD_BOOL, true  },
        { S_BUZ_5_LONG_ON,          "Buzzer 5 Long On (MS)",             "1200",          FIELD_NUMB, true  },
        { S_BUZ_5_LONG_OFF,         "Buzzer 5 Long Off (MS)",            "300",           FIELD_NUMB, true  },
        { S_BUZ_CONS_SHORT_TEST,    "Test Buzzer Constant Short",        "false",         FIELD_BOOL, true  },
        { S_BUZ_CONS_SHORT_ON,      "Buzzer Constant Short On (MS)",     "100",           FIELD_NUMB, true  },
        { S_BUZ_CONS_SHORT_OFF,     "Buzzer Constant Short Off (MS)",    "5000",          FIELD_NUMB, true  },
        { S_BUZ_CONS_LONG_TEST,     "Test Buzzer Constant Long",         "false",         FIELD_BOOL, true  },
        { S_BUZ_CONS_LONG_ON,       "Buzzer Constant Long On (MS)",      "2000",          FIELD_NUMB, true  },
        { S_BUZ_CONS_LONG_OFF,      "Buzzer Constant Long Off (MS)",     "500",           FIELD_NUMB, true  },
        { S_BIRTHDAY_TIME_OVR,      "Birthday & Anniversaries Override", "false",         FIELD_BOOL, true  },
        { S_MIDNIGHT_REBOOT_TIME,   "Time",                              "00:05",         FIELD_TIME, true  },
        { S_MIDNIGHT_REBOOT_ENABLE, "Enable Reboot",                     "true",          FIELD_BOOL, true  },
    };

    for (size_t n = 0; n < sizeof(d) / sizeof(d[0]); n++) {
        SettingEntry *e = &g_settings[d[n].idx];
        e->info = d[n].info;
        strlcpy(e->value, d[n].value, SETTING_VALUE_LEN);
        e->type = d[n].type;
        e->web_display = d[n].web;
    }
}

void Settings_WriteData(void)
{
    if (xSemaphoreTake(variabMutex, portMAX_DELAY) != pdTRUE) return;

    cJSON *root = cJSON_CreateObject();
    for (int i = 0; i < SETTING_ENTRIES; i++) {
        if (g_settings[i].info[0] == '\0') continue; // separator placeholder — not persisted
        cJSON *entry = cJSON_AddObjectToObject(root, g_settings[i].info);
        cJSON_AddStringToObject(entry, "Value", g_settings[i].value);
    }

    char *out = cJSON_Print(root);
    cJSON_Delete(root);
    if (out) {
        FILE *f = fopen(SETTINGS_PATH, "w");
        if (f) {
            fputs(out, f);
            fclose(f);
        } else {
            ESP_LOGE(TAG, "Failed to write Settings.json");
        }
        free(out);
    }

    xSemaphoreGive(variabMutex);
}

void Settings_ReadData(void)
{
    if (xSemaphoreTake(variabMutex, portMAX_DELAY) != pdTRUE) return;

    FILE *f = fopen(SETTINGS_PATH, "r");
    if (!f) {
        xSemaphoreGive(variabMutex);
        return;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 8192) {
        fclose(f);
        xSemaphoreGive(variabMutex);
        return;
    }
    char *buf = malloc(sz + 1);
    if (!buf) {
        fclose(f);
        xSemaphoreGive(variabMutex);
        return;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';

    cJSON *root = cJSON_ParseWithLength(buf, rd);
    free(buf);
    if (root) {
        for (int i = 0; i < SETTING_ENTRIES; i++) {
            if (g_settings[i].info[0] == '\0') continue;
            cJSON *entry = cJSON_GetObjectItemCaseSensitive(root, g_settings[i].info);
            if (!entry) continue;
            cJSON *val = cJSON_GetObjectItemCaseSensitive(entry, "Value");
            if (cJSON_IsString(val)) {
                strlcpy(g_settings[i].value, val->valuestring, SETTING_VALUE_LEN);
            }
        }

        // Migration: the night detect-off duration used to be stored in minutes
        // under "Night Time After Detect off (Min)". If only the old key exists,
        // carry its value over converted to seconds (persisted on next save).
        if (!cJSON_GetObjectItemCaseSensitive(root, g_settings[S_NIGHT_DETECT_OFF_SEC].info)) {
            cJSON *oldEntry = cJSON_GetObjectItemCaseSensitive(root, "Night Time After Detect off (Min)");
            cJSON *oldVal = oldEntry ? cJSON_GetObjectItemCaseSensitive(oldEntry, "Value") : NULL;
            if (cJSON_IsString(oldVal)) {
                int secs = atoi(oldVal->valuestring) * 60;
                if (secs < NIGHT_DETECT_OFF_MIN_SEC) secs = NIGHT_DETECT_OFF_MIN_SEC;
                snprintf(g_settings[S_NIGHT_DETECT_OFF_SEC].value, SETTING_VALUE_LEN, "%d", secs);
            }
        }
        cJSON_Delete(root);
    }

    xSemaphoreGive(variabMutex);
}

void CreateSettingObject(void)
{
    Settings_ReadData();
    // TIM_HUMAN_DETECT_STAY_ON's duration is no longer preset here — it's day/night
    // dependent, so HumanDetect_ActivateStayOn() (sensors.c) sets it fresh on every
    // activation instead. See S_DAY_DETECT_OFF_MIN / S_NIGHT_DETECT_OFF_SEC.
}

// None of the Put* setters below touch .type — a field's type is schema (set
// once by Settings_InitDefaults() and used to pick its web input widget), not
// a runtime property. Settings_PutText() used to stamp .type = FIELD_TEXT on
// every call; since UpdateSettingsPageValues() (web_settings.c) calls it for
// FIELD_NUMB fields too (after Settings_ValidateNEGNumeric), that silently
// downgraded every numeric setting to a plain text input the first time the
// Settings page was ever saved after boot.
void Settings_PutText(int idx, const char *value)
{
    if (idx < 0 || idx >= SETTING_ENTRIES) return;
    if (xSemaphoreTake(variabMutex, portMAX_DELAY) == pdTRUE) {
        strlcpy(g_settings[idx].value, value, SETTING_VALUE_LEN);
        xSemaphoreGive(variabMutex);
    }
}

void Settings_PutBool(int idx, bool value)
{
    if (idx < 0 || idx >= SETTING_ENTRIES) return;
    if (xSemaphoreTake(variabMutex, portMAX_DELAY) == pdTRUE) {
        strlcpy(g_settings[idx].value, value ? "true" : "false", SETTING_VALUE_LEN);
        xSemaphoreGive(variabMutex);
    }
}

void Settings_PutNumber(int idx, int value)
{
    if (idx < 0 || idx >= SETTING_ENTRIES) return;
    if (xSemaphoreTake(variabMutex, portMAX_DELAY) == pdTRUE) {
        snprintf(g_settings[idx].value, SETTING_VALUE_LEN, "%d", value);
        xSemaphoreGive(variabMutex);
    }
}

void Settings_GetText(int idx, char *out, size_t outsz)
{
    out[0] = '\0';
    if (idx < 0 || idx >= SETTING_ENTRIES) return;
    if (xSemaphoreTake(variabMutex, portMAX_DELAY) == pdTRUE) {
        strlcpy(out, g_settings[idx].value, outsz);
        xSemaphoreGive(variabMutex);
    }
}

bool Settings_GetBool(int idx)
{
    if (idx < 0 || idx >= SETTING_ENTRIES) return false;
    bool ret = false;
    if (xSemaphoreTake(variabMutex, portMAX_DELAY) == pdTRUE) {
        ret = strcmp(g_settings[idx].value, "true") == 0;
        xSemaphoreGive(variabMutex);
    }
    return ret;
}

int Settings_GetNumber(int idx)
{
    if (idx < 0 || idx >= SETTING_ENTRIES) return 0;
    int ret = 0;
    if (xSemaphoreTake(variabMutex, portMAX_DELAY) == pdTRUE) {
        ret = atoi(g_settings[idx].value);
        xSemaphoreGive(variabMutex);
    }
    return ret;
}

bool Settings_IsNumericOrNeg(const char *s)
{
    if (!s || s[0] == '\0') return false;
    if (strcmp(s, "-") == 0) return true;
    int start = (s[0] == '-') ? 1 : 0;
    if (s[start] == '\0') return false;
    for (int i = start; s[i]; i++) {
        if (!isdigit((unsigned char)s[i])) return false;
    }
    return true;
}

bool Settings_IsNumeric(const char *s)
{
    if (!s || s[0] == '\0') return false;
    for (int i = 0; s[i]; i++) {
        if (!isdigit((unsigned char)s[i])) return false;
    }
    return true;
}

void Settings_ValidateNEGNumeric(const char *in, char *out, size_t outsz)
{
    if (!Settings_IsNumericOrNeg(in) || strcmp(in, "-") == 0) {
        strlcpy(out, "0", outsz);
        return;
    }
    int num = atoi(in);
    if (num < 0) num = 0;
    snprintf(out, outsz, "%d", num);
}

void Settings_ValidateRGBString(const char *in, char *out, size_t outsz)
{
    int rgb[3] = { 255, 255, 255 };
    const char *p = in;
    for (int part = 0; part < 3 && p; part++) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        char seg[16];
        if (len >= sizeof(seg)) len = sizeof(seg) - 1;
        memcpy(seg, p, len);
        seg[len] = '\0';

        if (Settings_IsNumeric(seg)) {
            int v = atoi(seg);
            rgb[part] = (v < 0 || v > 255) ? 255 : v;
        }
        p = comma ? comma + 1 : NULL;
    }
    snprintf(out, outsz, "%d,%d,%d", rgb[0], rgb[1], rgb[2]);
}
