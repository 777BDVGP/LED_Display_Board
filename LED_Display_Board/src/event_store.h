// =============================================================================
// event_store.h — Event Storage & Query Engine (ESP-IDF port of EventClass)
// =============================================================================
// Each event (birthday/anniversary/reminder) is one JSON file on SPIFFS,
// keyed by symbolic field name — same schema ARD uses, so files it already
// wrote (Birthday_1.json etc.) load here unchanged.
// =============================================================================

#ifndef EVENT_STORE_H
#define EVENT_STORE_H

#include <stdbool.h>
#include <stddef.h>

#include "field_type.h"

// ─── Event Category Tags ──────────────────────────────────────────────────────
#define EVY_TYPE_BIR   "Birthday"
#define EVY_TYPE_ANN   "Anniversary"
#define EVY_TYPE_REM   "Reminder"

#define EXT_YES  true
#define EXT_NO   false

// ─── Sort Type Constants ──────────────────────────────────────────────────────
#define SORT_BY_TIME   1
#define SORT_BY_FILE   2
#define SORT_BY_LABLE  3
#define SORT_BY_DATE   4
#define SORT_BY_ANN    5   // non-Public-Holiday by date first, then Public Holiday by date
#define SORT_BY_MONTH  6   // MM-DD only (ignores year — for birthdays)

// ─── Colour Option Strings ────────────────────────────────────────────────────
#define ECO_WHITE   "White"
#define ECO_YELLOW  "Yellow"
#define ECO_GREEN   "Green"
#define ECO_RED     "Red"
#define ECO_BLUE    "Blue"

// ─── Buzzer Pattern Strings ───────────────────────────────────────────────────
#define BUZ_NONE     "Silent"
#define BUZ_SHORT    "3 Short Beeps"
#define BUZ_LONG     "5 Long Beeps"
#define BUZ_CONT_SH  "Constant Short"
#define BUZ_CONT_LO  "Constant Long"
#define BUZ_EX1      "Ex1"
#define BUZ_EX2      "Ex2"
#define BUZ_EX3      "Ex3"
#define BUZ_EX4      "Ex4"
#define BUZ_EX_SET   "ExSet"
#define BUZ_EX_DONE  "ExDone"

// ─── Event Field Index Constants (S_EVT_*) ───────────────────────────────────
#define S_EVT_LABLE       0
#define S_EVT_MESSAGE     1
#define S_EVT_DATE        2
#define S_EVT_TIME        3
#define S_EVT_SHOW_AGE    4
#define S_EVT_WEEKDAYS    5
#define S_EVT_COLOR       6
#define S_EVT_BUZZ        7
#define S_EVT_REPEAT      8
#define S_EVT_RADAR       9
#define S_EVT_NTFY        10
#define S_EVT_ACTIVE      11
#define S_EVT_VERSION     12
#define S_EVT_TYPE        13
#define S_EVT_NTFY_PRIO   14
#define S_EVT_ONETIME     15
#define S_EVT_NTFY_TOPIC  16
#define S_EVT_DISABLED    17
#define S_EVT_TIME_OVR    18
#define S_EVT_VALID_PER   19   // Reminder Valid Period ref ("/ValidPeriod_3.json"), " " = None

#define EVENT_ENTRIES     20
#define EVENT_VALUE_LEN   72

typedef struct {
    const char  *index;   // symbolic JSON key, e.g. "S_EVT_LABLE"
    const char  *info;    // human label (not persisted)
    char         value[EVENT_VALUE_LEN];
    field_type_t type;
    bool         web_display;
} EventEntry;

typedef struct {
    EventEntry fields[EVENT_ENTRIES];
} EventRecord;

// Resets every field to its EventClass-constructor default (labels/types/etc).
void Event_InitDefaults(EventRecord *ev);

// Resets fields to Create-form defaults (mirrors EventClass::InitData()).
void Event_InitData(EventRecord *ev);

// Persistence — one JSON file per event, keyed by symbolic Index.
bool Event_WriteData(EventRecord *ev);
bool Event_ReadData(const char *filepath, EventRecord *ev);
// Writes ev to an explicit full SPIFFS path (no filename/version logic) —
// for rewriting an existing file in place.
bool Event_WriteDataTo(EventRecord *ev, const char *fullPath);

// Writes the fopen-ready SPIFFS path for this event into out (e.g.
// "/spiffs/Birthday_1.json"), auto-incrementing S_EVT_VERSION until an
// unused name is found. With withExt=false, out instead gets the bare
// display name without path/extension (e.g. "Birthday_1").
void Event_GetFilename(EventRecord *ev, bool withExt, char *out, size_t outsz);

// Runtime projection of one event, populated by Event_GetList().
typedef struct {
    char type[16];
    char label[40];
    char mess[64];
    char date[11];
    char time[6];
    char color[8];
    char buzzer[16];
    int  repeat;
    bool radar;
    bool ntfy;
    char ntfy_priority[10];
    char ntfy_topic[16];
    bool show_age;
    bool active;
    bool one_time;
    bool disabled;
    bool time_overridden;   // true if S_EVT_TIME_OVR is set — applied by
                            // Event_RefreshTodayList() only, never by
                            // Event_GetList() itself (see time_override below)
    char time_override[6]; // raw S_EVT_TIME_OVR value, "" if none
    char weekdays[16];
    char filename[40];
    char valid_period[32];  // S_EVT_VALID_PER ref, "" = None
    bool done;
} EventRuntime;

// Scans every .json file on SPIFFS, filters by type/date/weekday rules
// (mirrors ARD's getEventList()), sorts by sortType, and writes up to
// maxOut results into out[]. Returns the number written.
//   type     — "" for any type, or one of EVY_TYPE_*
//   dateTime — "YYYY-MM-DD HH:MM" or "YYYY-MM-DD" or "" (no date filter)
//   weekDay  — 1=Sun..7=Sat, or 0 for "no weekday" (date-only Reminder match)
//   showExt  — true: filename includes ".json"; false: bare name
int Event_GetList(const char *type, const char *dateTime, int weekDay, bool showExt,
                   int sortType, EventRuntime *out, int maxOut);

// Recurrence-pattern evaluator: one-off date match, weekly ("1234567", Sun=1),
// or first/last-weekday-of-month ("F3M11", "L1A", ...). Used by Event_GetList()
// for Reminder-type events that carry a weekdays pattern.
bool Event_IsWeekdayValid(const char *todaysDateTime, const char *checkDate,
                           const char *weekDaysCheck, int weekday);

// ─── Today's Active Event List ───────────────────────────────────────────────
// Populated by Event_RefreshTodayList(), polled every second by AppLogicTask.
#define MAX_EVENTS_DISPLAY 40

extern EventRuntime g_eventsDisplay[MAX_EVENTS_DISPLAY];
extern int          g_eventsDisplayCount;

// Rebuilds g_eventsDisplay[] for "today" (all types, current weekday, sorted
// by time), then applies the Birthday/Anniversary global time override
// (S_BIRTHDAY_TIME_OVR) if enabled. Called once a day from CompileDateTime()
// on the day-of-month rollover (and so also once at boot, since MonthDay
// starts at -1).
void Event_RefreshTodayList(void);

#endif // EVENT_STORE_H
