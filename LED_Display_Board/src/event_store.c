// =============================================================================
// event_store.c — Event Storage & Query Engine
// =============================================================================
// See event_store.h. Ported from LED_Display_Board_ARD's 08_WEBEvents.cpp
// (EventClass, getEventList, isWeekdayValid).
// =============================================================================

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_log.h"

#include "datetime.h"
#include "event_store.h"
#include "files.h"
#include "globals.h"
#include "settings_store.h"
#include "valid_periods.h"

static const char *TAG = "event_store";

// -----------------------------------------------------------------------------
// Event_InitDefaults
// Table-driven equivalent of ARD's EventClass constructor.
// -----------------------------------------------------------------------------
void Event_InitDefaults(EventRecord *ev)
{
    static const struct {
        int          idx;
        const char  *index;
        const char  *info;
        const char  *value;
        field_type_t type;
        bool         web;
    } d[] = {
        { S_EVT_LABLE,      "S_EVT_LABLE",      "Lable",   "",           FIELD_TEXT,   true  },
        { S_EVT_MESSAGE,    "S_EVT_MESSAGE",    "Message", "",           FIELD_TEXT,   true  },
        { S_EVT_DATE,       "S_EVT_DATE",       "Date",    "1957-09-06", FIELD_DATE,   true  },
        { S_EVT_TIME,       "S_EVT_TIME",       "Time",    "06:30",      FIELD_TIME,   true  },
        { S_EVT_SHOW_AGE,   "S_EVT_SHOW_AGE",   "Show Age","true",       FIELD_BOOL,   false },
        { S_EVT_WEEKDAYS,   "S_EVT_WEEKDAYS",   "Rep: Monthly or Weekly - e.g. (F/L)3(M11/A) or 13..(Su=1)", " ", FIELD_TEXT, false },
        { S_EVT_COLOR,      "S_EVT_COLOR",      "Text Color",    ECO_WHITE, FIELD_SELECT, true  },
        { S_EVT_BUZZ,       "S_EVT_BUZZ",       "Buzzer Sound",  BUZ_SHORT, FIELD_SELECT, true  },
        { S_EVT_REPEAT,     "S_EVT_REPEAT",     "Repeat Times",  "0",       FIELD_NUMB,   true  },
        { S_EVT_RADAR,      "S_EVT_RADAR",      "Detect Human",  "false",   FIELD_BOOL,   true  },
        { S_EVT_NTFY,       "S_EVT_NTFY",       "Send NTFY",     "false",   FIELD_BOOL,   true  },
        { S_EVT_ACTIVE,     "S_EVT_ACTIVE",     "Event Active",  "true",    FIELD_BOOL,   true  },
        { S_EVT_VERSION,    "S_EVT_VERSION",    "File Version",  "1",       FIELD_TEXT,   false },
        { S_EVT_TYPE,       "S_EVT_TYPE",       "Type", EVY_TYPE_ANN,       FIELD_SELECT, false },
        { S_EVT_NTFY_PRIO,  "S_EVT_NTFY_PRIO",  "NTFY Priority", "default", FIELD_SELECT, false },
        { S_EVT_ONETIME,    "S_EVT_ONETIME",    "One Time",      "false",   FIELD_BOOL,   false },
        { S_EVT_NTFY_TOPIC, "S_EVT_NTFY_TOPIC", "NTFY Topic", "BOTH_NTFYS", FIELD_SELECT, false },
        { S_EVT_DISABLED,   "S_EVT_DISABLED",   "Disabled",      "false",   FIELD_BOOL,   false },
        { S_EVT_TIME_OVR,   "S_EVT_TIME_OVR",   "Time Override (today only)", "", FIELD_TIME, false },
        { S_EVT_VALID_PER,  "S_EVT_VALID_PER",  "Valid Period",  " ",       FIELD_SELECT, false },
    };

    for (size_t n = 0; n < sizeof(d) / sizeof(d[0]); n++) {
        EventEntry *e = &ev->fields[d[n].idx];
        e->index = d[n].index;
        e->info  = d[n].info;
        strlcpy(e->value, d[n].value, EVENT_VALUE_LEN);
        e->type  = d[n].type;
        e->web_display = d[n].web;
    }
}

// -----------------------------------------------------------------------------
// Event_InitData
// Create-form reset. Mirrors ARD's EventClass::InitData().
// -----------------------------------------------------------------------------
void Event_InitData(EventRecord *ev)
{
    strlcpy(ev->fields[S_EVT_TYPE].value,     EVY_TYPE_BIR,   EVENT_VALUE_LEN);
    strlcpy(ev->fields[S_EVT_LABLE].value,    " ",            EVENT_VALUE_LEN);
    strlcpy(ev->fields[S_EVT_MESSAGE].value,  " ",            EVENT_VALUE_LEN);
    strlcpy(ev->fields[S_EVT_DATE].value,     "1900-01-01",   EVENT_VALUE_LEN);
    strlcpy(ev->fields[S_EVT_TIME].value,     "06:00",        EVENT_VALUE_LEN);
    strlcpy(ev->fields[S_EVT_WEEKDAYS].value, " ",            EVENT_VALUE_LEN);
    strlcpy(ev->fields[S_EVT_COLOR].value,    ECO_WHITE,      EVENT_VALUE_LEN);
    strlcpy(ev->fields[S_EVT_BUZZ].value,     BUZ_SHORT,      EVENT_VALUE_LEN);
    strlcpy(ev->fields[S_EVT_REPEAT].value,   "2",            EVENT_VALUE_LEN);
    strlcpy(ev->fields[S_EVT_RADAR].value,    "true",         EVENT_VALUE_LEN);
    strlcpy(ev->fields[S_EVT_ACTIVE].value,   "true",         EVENT_VALUE_LEN);
    strlcpy(ev->fields[S_EVT_VERSION].value,  "1",            EVENT_VALUE_LEN);
    strlcpy(ev->fields[S_EVT_ONETIME].value,  "false",        EVENT_VALUE_LEN);
    // TODO(Phase D): inherit this clock's own NTFY topic setting once NTFY is ported.
    strlcpy(ev->fields[S_EVT_NTFY_TOPIC].value, "BOTH_NTFYS", EVENT_VALUE_LEN);
    // s_editEvent (web_events.c) is a single shared buffer reused across every
    // request — these two have no rendered form input, so without an explicit
    // reset here a Create right after viewing/editing a DISABLED or same-day
    // retimed event would silently carry that leftover state into the new file.
    strlcpy(ev->fields[S_EVT_DISABLED].value, "false", EVENT_VALUE_LEN);
    strlcpy(ev->fields[S_EVT_TIME_OVR].value, "",      EVENT_VALUE_LEN);
    strlcpy(ev->fields[S_EVT_VALID_PER].value, " ",    EVENT_VALUE_LEN);

    ev->fields[S_EVT_LABLE].web_display     = true;
    ev->fields[S_EVT_MESSAGE].web_display   = true;
    ev->fields[S_EVT_DATE].web_display      = true;
    ev->fields[S_EVT_TIME].web_display      = true;
    ev->fields[S_EVT_SHOW_AGE].web_display  = false;
    ev->fields[S_EVT_WEEKDAYS].web_display  = false;
    ev->fields[S_EVT_COLOR].web_display     = true;
    ev->fields[S_EVT_BUZZ].web_display      = true;
    ev->fields[S_EVT_REPEAT].web_display    = true;
    ev->fields[S_EVT_RADAR].web_display     = true;
    ev->fields[S_EVT_NTFY].web_display      = true;
    ev->fields[S_EVT_ACTIVE].web_display    = true;
    ev->fields[S_EVT_VERSION].web_display   = false;
    ev->fields[S_EVT_TYPE].web_display      = false;
    ev->fields[S_EVT_NTFY_PRIO].web_display = false;
    ev->fields[S_EVT_ONETIME].web_display   = false;
}

void Event_GetFilename(EventRecord *ev, bool withExt, char *out, size_t outsz)
{
    const char *type;
    if (strcmp(ev->fields[S_EVT_TYPE].value, EVY_TYPE_BIR) == 0)      type = "Birthday";
    else if (strcmp(ev->fields[S_EVT_TYPE].value, EVY_TYPE_ANN) == 0) type = "Anniversary";
    else if (strcmp(ev->fields[S_EVT_TYPE].value, EVY_TYPE_REM) == 0) type = "Reminder";
    else                                                              type = "Event";

    int version = atoi(ev->fields[S_EVT_VERSION].value);
    struct stat st;
    char full[96];
    for (;;) {
        snprintf(full, sizeof(full), SPIFFS_BASE_PATH "/%s_%d.json", type, version);
        if (stat(full, &st) != 0) break; // free filename found
        version++;
        snprintf(ev->fields[S_EVT_VERSION].value, EVENT_VALUE_LEN, "%d", version);
    }

    if (withExt) {
        strlcpy(out, full, outsz);
    } else {
        snprintf(out, outsz, "%s_%d", type, version);
    }
}

bool Event_WriteData(EventRecord *ev)
{
    char path[96];
    Event_GetFilename(ev, true, path, sizeof(path));
    return Event_WriteDataTo(ev, path);
}

bool Event_WriteDataTo(EventRecord *ev, const char *path)
{
    bool ok = false;
    if (xSemaphoreTake(variabMutex, portMAX_DELAY) == pdTRUE) {
        cJSON *root = cJSON_CreateObject();
        for (int i = 0; i < EVENT_ENTRIES; i++) {
            cJSON *entry = cJSON_AddObjectToObject(root, ev->fields[i].index);
            cJSON_AddStringToObject(entry, "Value", ev->fields[i].value);
            cJSON_AddBoolToObject(entry, "WEBDisplay", ev->fields[i].web_display);
        }
        char *out = cJSON_Print(root);
        cJSON_Delete(root);
        if (out) {
            FILE *f = fopen(path, "w");
            if (f) {
                fputs(out, f);
                fclose(f);
                ok = true;
            } else {
                ESP_LOGE(TAG, "Failed to write %s", path);
            }
            free(out);
        }
        xSemaphoreGive(variabMutex);
    }
    return ok;
}

bool Event_ReadData(const char *filepath, EventRecord *ev)
{
    if (xSemaphoreTake(variabMutex, portMAX_DELAY) != pdTRUE) return false;

    FILE *f = fopen(filepath, "r");
    if (!f) {
        xSemaphoreGive(variabMutex);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4096) {
        fclose(f);
        xSemaphoreGive(variabMutex);
        return false;
    }
    char *buf = malloc(sz + 1);
    if (!buf) {
        fclose(f);
        xSemaphoreGive(variabMutex);
        return false;
    }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';

    cJSON *root = cJSON_ParseWithLength(buf, rd);
    free(buf);
    if (!root) {
        xSemaphoreGive(variabMutex);
        return false;
    }

    // Validate: this must be an event file (has a recognized S_EVT_TYPE),
    // which is how event files are told apart from Settings.json/NTFY.json/
    // etc. during a full-directory scan.
    cJSON *typeEntry = cJSON_GetObjectItemCaseSensitive(root, "S_EVT_TYPE");
    cJSON *typeVal = typeEntry ? cJSON_GetObjectItemCaseSensitive(typeEntry, "Value") : NULL;
    if (!cJSON_IsString(typeVal) ||
        (strcmp(typeVal->valuestring, EVY_TYPE_BIR) != 0 &&
         strcmp(typeVal->valuestring, EVY_TYPE_ANN) != 0 &&
         strcmp(typeVal->valuestring, EVY_TYPE_REM) != 0)) {
        cJSON_Delete(root);
        xSemaphoreGive(variabMutex);
        return false;
    }

    for (int i = 0; i < EVENT_ENTRIES; i++) {
        cJSON *entry = cJSON_GetObjectItemCaseSensitive(root, ev->fields[i].index);
        if (!entry) continue;
        cJSON *val = cJSON_GetObjectItemCaseSensitive(entry, "Value");
        cJSON *web = cJSON_GetObjectItemCaseSensitive(entry, "WEBDisplay");
        if (!cJSON_IsString(val) || !web) continue;
        strlcpy(ev->fields[i].value, val->valuestring, EVENT_VALUE_LEN);
        ev->fields[i].web_display = cJSON_IsTrue(web);
    }
    cJSON_Delete(root);

    xSemaphoreGive(variabMutex);
    return true;
}

// =============================================================================
// Recurrence-Pattern Evaluator
// =============================================================================

static int dayOfWeek(int y, int m, int d)
{
    if (m < 3) { m += 12; y -= 1; }
    long K = y % 100;
    long J = y / 100;
    long h = (d + (13 * (m + 1)) / 5 + K + K / 4 + J / 4 + 5 * J) % 7; // 0=Sat
    return (int)(((h + 6) % 7) + 1); // 1=Sun .. 7=Sat
}

static int daysInMonth(int y, int m)
{
    static const uint8_t dim[] = { 31,28,31,30,31,30,31,31,30,31,30,31 };
    int d = dim[m - 1];
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || (y % 400 == 0))) d = 29;
    return d;
}

bool Event_IsWeekdayValid(const char *todaysDateTime, const char *checkDate,
                           const char *weekDaysCheckIn, int weekday)
{
    char todayDateOnly[11];
    strlcpy(todayDateOnly, todaysDateTime, sizeof(todayDateOnly));

    int todayYear = 0, todayMonth = 0, todayDay = 0;
    sscanf(todayDateOnly, "%4d-%2d-%2d", &todayYear, &todayMonth, &todayDay);

    char weekDaysCheck[24];
    strlcpy(weekDaysCheck, weekDaysCheckIn, sizeof(weekDaysCheck));
    char *start = weekDaysCheck;
    while (*start == ' ') start++;
    char *end = start + strlen(start);
    while (end > start && *(end - 1) == ' ') *(--end) = '\0';

    if (start[0] == '\0') {
        return strcmp(todayDateOnly, checkDate) == 0;
    }

    // "F<n>Mmm"/"F<n>A" = first <weekday> in month mm / every month.
    // "L<n>Mmm"/"L<n>A" = last <weekday> in month mm / every month.
    if (start[0] == 'F' || start[0] == 'L') {
        char *mPosM = strchr(start, 'M');
        char *mPosA = strchr(start, 'A');
        char *mPos = NULL;
        if (mPosM && (mPosM - start) > 1) mPos = mPosM;
        else if (mPosA && (mPosA - start) > 1) mPos = mPosA;

        if (mPos) {
            int wlen = (int)(mPos - start - 1);
            if (wlen > 0 && wlen < 8) {
                char numBuf[8];
                memcpy(numBuf, start + 1, wlen);
                numBuf[wlen] = '\0';
                int w = atoi(numBuf);
                int m = (mPos == mPosA) ? todayMonth : atoi(mPos + 1);

                if (w >= 1 && w <= 7 && m >= 1 && m <= 12) {
                    int dim = daysInMonth(todayYear, m);
                    int day;
                    if (start[0] == 'F') {
                        day = 1;
                        while (day <= dim && dayOfWeek(todayYear, m, day) != w) day++;
                    } else {
                        day = dim;
                        while (day > 0 && dayOfWeek(todayYear, m, day) != w) day--;
                    }
                    char buf[11];
                    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", todayYear, m, day);
                    return strcmp(todayDateOnly, buf) == 0;
                }
            }
        }
        return false;
    }

    // Plain weekday list "1234567" (Sun=1..Sat=7).
    if (weekday >= 1 && weekday <= 7) {
        char ww = (char)('0' + weekday);
        return strchr(weekDaysCheck, ww) != NULL;
    }

    return false;
}

// =============================================================================
// Event_GetList
// =============================================================================

static int cmpByTime(const void *a, const void *b)
{
    return strcmp(((const EventRuntime *)a)->time, ((const EventRuntime *)b)->time);
}
static int cmpByFile(const void *a, const void *b)
{
    return strcmp(((const EventRuntime *)a)->filename, ((const EventRuntime *)b)->filename);
}
static int cmpByLabel(const void *a, const void *b)
{
    return strcmp(((const EventRuntime *)a)->label, ((const EventRuntime *)b)->label);
}
static int cmpByDate(const void *a, const void *b)
{
    return strcmp(((const EventRuntime *)a)->date, ((const EventRuntime *)b)->date);
}
static int cmpByAnn(const void *a, const void *b)
{
    const EventRuntime *ea = a, *eb = b;
    bool aPH = strcmp(ea->mess, "Public Holiday") == 0;
    bool bPH = strcmp(eb->mess, "Public Holiday") == 0;
    if (aPH != bPH) return aPH ? 1 : -1; // non-PH before PH
    return strcmp(ea->date, eb->date);
}
static int cmpByMonth(const void *a, const void *b)
{
    const EventRuntime *ea = a, *eb = b;
    const char *ma = strlen(ea->date) >= 10 ? ea->date + 5 : ea->date;
    const char *mb = strlen(eb->date) >= 10 ? eb->date + 5 : eb->date;
    return strcmp(ma, mb);
}

int Event_GetList(const char *type, const char *dateTime, int weekDay, bool showExt,
                   int sortType, EventRuntime *out, int maxOut)
{
    char dateOnly[11] = "";
    char timeNow[6] = "";
    size_t dtLen = strlen(dateTime);
    if (dtLen >= 16 && dateTime[10] == ' ') {
        memcpy(dateOnly, dateTime, 10); dateOnly[10] = '\0';
        memcpy(timeNow, dateTime + 11, 5); timeNow[5] = '\0';
    } else if (dtLen == 10) {
        memcpy(dateOnly, dateTime, 10); dateOnly[10] = '\0';
    }

    DIR *dir = opendir(SPIFFS_BASE_PATH);
    if (!dir) return 0;

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && count < maxOut) {
        size_t nlen = strlen(ent->d_name);
        if (nlen < 6 || strcmp(ent->d_name + nlen - 5, ".json") != 0) continue;

        char fpath[300];
        snprintf(fpath, sizeof(fpath), "%s/%s", SPIFFS_BASE_PATH, ent->d_name);

        EventRecord tmp;
        Event_InitDefaults(&tmp); // must run first — sets .index (cJSON key) for every field, which Event_ReadData looks up
        if (!Event_ReadData(fpath, &tmp)) continue;

        const char *eventType = tmp.fields[S_EVT_TYPE].value;
        bool isBirAnn = (strcmp(eventType, EVY_TYPE_BIR) == 0 || strcmp(eventType, EVY_TYPE_ANN) == 0);

        if (isBirAnn) {
            // Birthdays/Anniversaries always match by MM-DD only (year ignored) —
            // except "Public Holiday" entries (manually maintained now that the
            // PPH auto-fetch is gone): some holidays shift date year to year
            // (Easter/Good Friday/Family Day), so a stale prior-year entry must
            // not silently re-fire just because MM-DD still lines up. Those
            // require the full YYYY-MM-DD to match today exactly, so an entry
            // only fires once it's actually been updated for the current year.
            if (dateOnly[0] != '\0') {
                const char *eventDate = tmp.fields[S_EVT_DATE].value;
                if (strlen(eventDate) != 10) continue;
                bool isPublicHoliday = strcmp(tmp.fields[S_EVT_MESSAGE].value, "Public Holiday") == 0;
                if (isPublicHoliday) {
                    if (memcmp(eventDate, dateOnly, 10) != 0) continue;
                } else if (memcmp(eventDate + 5, dateOnly + 5, 5) != 0) continue;
            }
        } else if (strcmp(eventType, EVY_TYPE_REM) == 0) {
            bool validEvent = false;
            if (weekDay == 0) {
                if (dateOnly[0] == '\0' || strcmp(tmp.fields[S_EVT_DATE].value, dateOnly) == 0) {
                    validEvent = true;
                }
            } else if (tmp.fields[S_EVT_WEEKDAYS].value[0] != '\0') {
                validEvent = Event_IsWeekdayValid(dateTime, tmp.fields[S_EVT_DATE].value,
                                                   tmp.fields[S_EVT_WEEKDAYS].value, weekDay);
            } else if (dateOnly[0] == '\0' || strcmp(tmp.fields[S_EVT_DATE].value, dateOnly) == 0) {
                validEvent = true;
            }
            // Reminder Valid Period: only on a dated query (today's list etc.) —
            // plain list views (dateOnly == "") still show every reminder.
            if (validEvent && dateOnly[0] != '\0' &&
                !VP_IsDateValid(tmp.fields[S_EVT_VALID_PER].value, dateOnly)) {
                validEvent = false;
            }
            if (!validEvent) continue;
        }

        if (type && type[0] != '\0' && strcmp(eventType, type) != 0) continue;

        EventRuntime *e = &out[count];
        memset(e, 0, sizeof(*e));
        strlcpy(e->type,   eventType,                       sizeof(e->type));
        strlcpy(e->label,  tmp.fields[S_EVT_LABLE].value,    sizeof(e->label));
        strlcpy(e->mess,   tmp.fields[S_EVT_MESSAGE].value,  sizeof(e->mess));
        strlcpy(e->date,   tmp.fields[S_EVT_DATE].value,     sizeof(e->date));
        strlcpy(e->time,   tmp.fields[S_EVT_TIME].value,     sizeof(e->time));
        strlcpy(e->color,  tmp.fields[S_EVT_COLOR].value,    sizeof(e->color));
        strlcpy(e->buzzer, tmp.fields[S_EVT_BUZZ].value,     sizeof(e->buzzer));
        e->repeat = atoi(tmp.fields[S_EVT_REPEAT].value);
        e->radar  = strcmp(tmp.fields[S_EVT_RADAR].value, "true") == 0;
        e->ntfy   = strcmp(tmp.fields[S_EVT_NTFY].value, "true") == 0;
        strlcpy(e->ntfy_priority, tmp.fields[S_EVT_NTFY_PRIO].value,  sizeof(e->ntfy_priority));
        strlcpy(e->ntfy_topic,    tmp.fields[S_EVT_NTFY_TOPIC].value, sizeof(e->ntfy_topic));
        e->active   = strcmp(tmp.fields[S_EVT_ACTIVE].value, "true") == 0;
        e->one_time = strcmp(tmp.fields[S_EVT_ONETIME].value, "true") == 0;
        e->disabled = strcmp(tmp.fields[S_EVT_DISABLED].value, "true") == 0;

        // A same-day-only time override (set via the Home page's tap-the-TIME-cell
        // edit) is only ever applied to today's occurrence by
        // Event_RefreshTodayList() — never here, so general list views
        // (/birthdays, /reminders, etc., used for editing) keep showing the
        // permanent S_EVT_TIME baseline rather than today's override.
        strlcpy(e->time_override, tmp.fields[S_EVT_TIME_OVR].value, sizeof(e->time_override));
        e->time_overridden = e->time_override[0] != '\0';
        strlcpy(e->weekdays, tmp.fields[S_EVT_WEEKDAYS].value, sizeof(e->weekdays));
        if (!VP_RefIsNone(tmp.fields[S_EVT_VALID_PER].value)) {
            strlcpy(e->valid_period, tmp.fields[S_EVT_VALID_PER].value, sizeof(e->valid_period));
        }

        e->filename[0] = '/';
        if (showExt) {
            strlcpy(e->filename + 1, ent->d_name, sizeof(e->filename) - 1);
        } else {
            char base[40];
            strlcpy(base, ent->d_name, sizeof(base));
            char *dot = strrchr(base, '.');
            if (dot) *dot = '\0';
            strlcpy(e->filename + 1, base, sizeof(e->filename) - 1);
        }

        e->show_age = strcmp(tmp.fields[S_EVT_SHOW_AGE].value, "true") == 0;
        if (strcmp(eventType, EVY_TYPE_REM) == 0) e->show_age = false;

        e->done = (timeNow[0] != '\0' && strcmp(timeNow, e->time) > 0);

        count++;
    }
    closedir(dir);

    int (*cmp)(const void *, const void *) = NULL;
    switch (sortType) {
        case SORT_BY_TIME:  cmp = cmpByTime;  break;
        case SORT_BY_FILE:  cmp = cmpByFile;  break;
        case SORT_BY_LABLE: cmp = cmpByLabel; break;
        case SORT_BY_DATE:  cmp = cmpByDate;  break;
        case SORT_BY_ANN:   cmp = cmpByAnn;   break;
        case SORT_BY_MONTH: cmp = cmpByMonth; break;
        default: break;
    }
    if (cmp) qsort(out, count, sizeof(EventRuntime), cmp);

    return count;
}

// =============================================================================
// Today's Active Event List
// =============================================================================

EventRuntime g_eventsDisplay[MAX_EVENTS_DISPLAY];
int          g_eventsDisplayCount = 0;

void Event_RefreshTodayList(void)
{
    char nowStr[17];
    GetCurrentDateAndTime(nowStr, sizeof(nowStr));

    g_eventsDisplayCount = Event_GetList("", nowStr, WeekDay, true, SORT_BY_TIME,
                                          g_eventsDisplay, MAX_EVENTS_DISPLAY);

    // Apply each event's same-day time override (if any) to today's display/
    // firing copy only — Event_GetList() deliberately left e->time at the
    // permanent baseline so /birthdays, /reminders, etc. keep showing that.
    {
        char nowTime[6];
        GetCurrentTime(nowTime, sizeof(nowTime));
        for (int i = 0; i < g_eventsDisplayCount; i++) {
            EventRuntime *e = &g_eventsDisplay[i];
            if (e->time_overridden) {
                strlcpy(e->time, e->time_override, sizeof(e->time));
                e->done = (nowTime[0] != '\0' && strcmp(nowTime, e->time) > 0);
            }
        }
    }

    if (Settings_GetBool(S_BIRTHDAY_TIME_OVR)) {
        char ovTime[6];
        Settings_GetText(S_BIRTHDAY_TIME, ovTime, sizeof(ovTime));
        char nowTime[6];
        GetCurrentTime(nowTime, sizeof(nowTime));

        for (int i = 0; i < g_eventsDisplayCount; i++) {
            EventRuntime *e = &g_eventsDisplay[i];
            if (strcmp(e->type, EVY_TYPE_BIR) == 0 || strcmp(e->type, EVY_TYPE_ANN) == 0) {
                strlcpy(e->time, ovTime, sizeof(e->time));
                e->done = (nowTime[0] != '\0' && strcmp(nowTime, ovTime) > 0);
            }
        }
    }

    ESP_LOGI(TAG, "Refreshed today's events: %d found (WeekDay=%d)", g_eventsDisplayCount, WeekDay);
}
