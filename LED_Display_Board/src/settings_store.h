// =============================================================================
// settings_store.h — Application Settings (ESP-IDF port of SettingClass)
// =============================================================================
// Single-instance runtime configuration table, persisted to
// /spiffs/Settings.json keyed by Info label — same key scheme ARD uses, so
// this reads a Settings.json file the Arduino build already wrote.
// =============================================================================

#ifndef SETTINGS_STORE_H
#define SETTINGS_STORE_H

#include <stdbool.h>
#include <stddef.h>

#include "field_type.h"

// ─── Setting Index Constants (S_*) ───────────────────────────────────────────
#define S_DEVICE_NAME               0   // OTA / mDNS hostname
#define S_SYSTEM_NAME               1   // Human-readable system name
#define S_SAVELOG                   2   // Enable general event logging to SPIFFS
#define S_BIRTHDAY_TIME             3   // Default time to trigger birthday events
#define S_DISPLAY_LINE_1            4   // -- separator --
#define S_DISP_LDR_INFO             5   // Show raw LDR data on display
#define S_LDR_RAW_MIN               6   // LDR ADC value at minimum light (dark)
#define S_LDR_RAW_MAX               7   // LDR ADC value at maximum light (bright)
#define S_DISP_BRIGHT_MIN           8   // Minimum LED matrix brightness level
#define S_DISP_BRIGHT_MAX           9   // Maximum LED matrix brightness level
#define S_DISPLAY_LINE_2           10   // -- separator --
#define S_DISP_TIME_PINK_HD        11   // Use alternate colour when human detected
#define S_PINK_HD_COLOR            12   // Alternate colour RGB string (R,G,B)
#define S_DISPLAY_LINE_3           13   // -- separator --
#define S_DISPLAY_ON_HUMAN_DET     14   // Only show display when radar detects human
#define S_DAY_DETECT_OFF_START     15   // Day window start (HH:MM) for post-detect stay-on
#define S_DAY_DETECT_OFF_MIN       16   // Minutes to stay on after detection ends, day window
#define S_NIGHT_DETECT_OFF_START   17   // Night window start (HH:MM) for post-detect stay-on
#define S_NIGHT_DETECT_OFF_SEC     18   // Seconds to stay on after detection ends, night window
#define NIGHT_DETECT_OFF_MIN_SEC   5    // Lower bound for S_NIGHT_DETECT_OFF_SEC
#define S_DISPLAY_LINE_4           19   // -- separator --
#define S_BUZ_DELAY_B4_DISP        20   // Delay (ms) between buzzer and message display
#define S_BUZ_DELAY_COLOR          21   // Colour during buzzer delay (R,G,B)
#define S_DISPLAY_LINE_5           22   // -- separator --
#define S_WEEKDAY_DISPLAY_SEC      23   // Seconds to show weekday on matrix
#define S_YEARDATE_DISPLAY_SEC     24   // Seconds to show date on matrix
#define S_YEARDATE_STYLE           25   // "Full Date" (c) or "Date/Time" (b) — see DisplayTask
#define S_DISPLAY_LINE_6           26   // -- separator --
#define S_DAY_TIME_COLOR_START     27   // Time at which day-colour takes effect (HH:MM)
#define S_NIGHT_TIME_COLOR_START   28   // Time at which night-colour takes effect (HH:MM)
#define S_DAY_TIME_COLOR           29   // Day-time display colour (R,G,B)
#define S_NIGHT_TIME_COLOR         30   // Night-time display colour (R,G,B)
#define S_DISPLAY_LINE_7           31   // -- separator --
#define S_BUZ_3_SHORT_TEST         32   // Checkbox: trigger 3-short-beep test pattern
#define S_BUZ_3_SHORT_ON           33   // 3-short beep ON duration (ms)
#define S_BUZ_3_SHORT_OFF          34   // 3-short beep OFF duration (ms)
#define S_BUZ_5_LONG_TEST          35   // Checkbox: trigger 5-long-beep test pattern
#define S_BUZ_5_LONG_ON            36   // 5-long beep ON duration (ms)
#define S_BUZ_5_LONG_OFF           37   // 5-long beep OFF duration (ms)
#define S_BUZ_CONS_SHORT_TEST      38   // Checkbox: trigger continuous-short-beep test
#define S_BUZ_CONS_SHORT_ON        39   // Continuous short beep ON duration (ms)
#define S_BUZ_CONS_SHORT_OFF       40   // Continuous short beep OFF duration (ms)
#define S_BUZ_CONS_LONG_TEST       41   // Checkbox: trigger continuous-long-beep test
#define S_BUZ_CONS_LONG_ON         42   // Continuous long beep ON duration (ms)
#define S_BUZ_CONS_LONG_OFF        43   // Continuous long beep OFF duration (ms)
#define S_BIRTHDAY_TIME_OVR        44   // Enable global time override for B&A events
#define S_MIDNIGHT_REBOOT_TIME     45   // Daily reboot time (HH:MM)
#define S_MIDNIGHT_REBOOT_ENABLE   46   // Enable/disable the daily reboot

#define SETTING_ENTRIES            47
#define SETTING_VALUE_LEN          64

typedef struct {
    const char  *info;                     // human label — also the persisted JSON key
    char         value[SETTING_VALUE_LEN]; // always stored as a string, like ARD
    field_type_t type;
    bool         web_display;
} SettingEntry;

extern SettingEntry g_settings[SETTING_ENTRIES];

// Resets g_settings[] to constructor defaults (labels/types/web_display/values).
void Settings_InitDefaults(void);

// Persistence — /spiffs/Settings.json, keyed by Info label.
void Settings_ReadData(void);
void Settings_WriteData(void);

// Loads persisted settings and derives dependent timer values (the
// human-detect display-stay-on duration). Call once at boot, after
// Settings_InitDefaults().
void CreateSettingObject(void);

// ── Typed Accessors (mutex-protected via variabMutex) ─────────────────────────
void Settings_PutText(int idx, const char *value);
void Settings_PutBool(int idx, bool value);
void Settings_PutNumber(int idx, int value);
void Settings_GetText(int idx, char *out, size_t outsz);
bool Settings_GetBool(int idx);
int  Settings_GetNumber(int idx);

// ── Input Validation Helpers ───────────────────────────────────────────────────
bool Settings_IsNumericOrNeg(const char *s);
bool Settings_IsNumeric(const char *s);
void Settings_ValidateNEGNumeric(const char *in, char *out, size_t outsz);
void Settings_ValidateRGBString(const char *in, char *out, size_t outsz);

#endif // SETTINGS_STORE_H
