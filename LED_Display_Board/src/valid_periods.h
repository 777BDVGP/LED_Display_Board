// =============================================================================
// valid_periods.h — Reminder Valid Periods (named sets of From→To date ranges)
// =============================================================================
// A Valid Period is a named list of date ranges, stored one per file as
// /spiffs/ValidPeriod_<n>.json. A Reminder can reference one (by that file's
// root-relative path, e.g. "/ValidPeriod_3.json", in S_EVT_VALID_PER); it then
// only fires on dates inside one of the period's ranges.
//
// Each range is either a fixed span (From/To compared as full YYYY-MM-DD) or
// "every year" (only MM-DD compared, year ignored; From > To wraps across New
// Year, e.g. 1 Dec → 31 Jan). Both ends are inclusive.
//
// File format:
//   {"VP_NAME":"Gymnastics","VP_RANGES":[{"F":"2026-01-01","T":"2026-07-01","Y":true}, ...]}
// =============================================================================

#ifndef VALID_PERIODS_H
#define VALID_PERIODS_H

#include <stdbool.h>
#include <stddef.h>

#define VP_FILE_PREFIX   "ValidPeriod_"
#define VP_MAX_RANGES    30   // keeps a file well under the 4 KB peer-sync body limit
#define VP_MAX_PERIODS   20
#define VP_NAME_LEN      64
#define VP_REF_LEN       32   // "/ValidPeriod_123.json"

typedef struct {
    char from[11];   // YYYY-MM-DD
    char to[11];     // YYYY-MM-DD
    bool yearly;     // true: compare MM-DD only, every year
} VpRange;

typedef struct {
    char    ref[VP_REF_LEN];   // root-relative path, "" for a not-yet-saved period
    char    name[VP_NAME_LEN];
    int     count;
    VpRange r[VP_MAX_RANGES];
} ValidPeriod;

typedef struct {
    char ref[VP_REF_LEN];
    char name[VP_NAME_LEN];
    int  count;
} VpSummary;

// True if ref is empty / blank (= "None", reminder behaves as normal).
bool VP_RefIsNone(const char *ref);

// Load / save one period. ref is root-relative ("/ValidPeriod_3.json").
bool VP_Read(const char *ref, ValidPeriod *out);
bool VP_Write(const ValidPeriod *vp);   // vp->ref must be set

// Picks the first unused "/ValidPeriod_<n>.json" name.
void VP_NewRef(char *out, size_t outsz);

// Lists all periods (name + range count), sorted by name. Returns count.
int VP_List(VpSummary *out, int maxOut);

// Writes the period's display name into out; returns false (and out = "")
// if ref is None or the file is missing.
bool VP_GetName(const char *ref, char *out, size_t outsz);

// Is dateYMD ("YYYY-MM-DD") inside the referenced period? A None ref or a
// missing file (e.g. deleted period, peer not yet synced) counts as valid, so
// the reminder simply behaves as normal.
bool VP_IsDateValid(const char *ref, const char *dateYMD);

#endif // VALID_PERIODS_H
