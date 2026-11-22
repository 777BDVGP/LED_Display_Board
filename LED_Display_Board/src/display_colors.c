// =============================================================================
// display_colors.c — RGB Parsing & Day/Night Colour Selection
// =============================================================================
// See display_colors.h.
// =============================================================================

#include <stdlib.h>
#include <string.h>

#include "datetime.h"
#include "display_colors.h"
#include "settings_store.h"

void ParseRGBColor(const char *rgbString, uint8_t *r, uint8_t *g, uint8_t *b)
{
    int rgb[3] = { 1, 1, 1 };
    const char *p = rgbString;
    for (int i = 0; i < 2; i++) {
        const char *comma = strchr(p, ',');
        if (!comma) break;
        rgb[i] = atoi(p);
        p = comma + 1;
    }
    rgb[2] = atoi(p); // unconditional, matches ARD — B parses whatever's left even if a comma was missing

    for (int i = 0; i < 3; i++) {
        if (rgb[i] < 0)   rgb[i] = 0;
        if (rgb[i] > 255) rgb[i] = 255;
    }
    *r = (uint8_t)rgb[0];
    *g = (uint8_t)rgb[1];
    *b = (uint8_t)rgb[2];
}

int HHMMToMinutes(const char *t)
{
    if (!t || strlen(t) < 4) return -1;
    const char *colon = strchr(t, ':');
    if (!colon || colon == t) return -1;

    int h = atoi(t);
    int m = atoi(colon + 1);
    if (h < 0 || h > 23 || m < 0 || m > 59) return -1;

    return h * 60 + m;
}

void GetTimeDisplayColor(uint8_t *r, uint8_t *g, uint8_t *b)
{
    *r = *g = *b = 255; // fallback white

    char nowStr[6];
    GetCurrentTime(nowStr, sizeof(nowStr));
    int nowMin = HHMMToMinutes(nowStr);
    if (nowMin < 0) return;

    char dayStartStr[6], nightStartStr[6];
    Settings_GetText(S_DAY_TIME_COLOR_START, dayStartStr, sizeof(dayStartStr));
    Settings_GetText(S_NIGHT_TIME_COLOR_START, nightStartStr, sizeof(nightStartStr));

    int dayStart   = HHMMToMinutes(dayStartStr);
    int nightStart = HHMMToMinutes(nightStartStr);
    if (dayStart < 0 || nightStart < 0) return;

    bool isDay;
    if (dayStart < nightStart) {
        isDay = (nowMin >= dayStart && nowMin < nightStart);
    } else {
        isDay = (nowMin >= dayStart || nowMin < nightStart); // wrap-around (e.g. day 20:00, night 06:00)
    }

    char colorStr[SETTING_VALUE_LEN];
    Settings_GetText(isDay ? S_DAY_TIME_COLOR : S_NIGHT_TIME_COLOR, colorStr, sizeof(colorStr));
    ParseRGBColor(colorStr, r, g, b);
}
