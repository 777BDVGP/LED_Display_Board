// =============================================================================
// datetime.h — Date/Time Compilation & Formatting (ESP-IDF port of 02_Time.cpp)
// =============================================================================

#ifndef DATETIME_H
#define DATETIME_H

#include <stddef.h>

extern char DateTime[40];  // "DD Mon YYYY HH:MM:SS Day" — refreshed every second
extern int  MonthDay;      // last-seen day-of-month; -1 until first CompileDateTime()
extern int  WeekDay;       // 1=Sunday .. 7=Saturday

// Reads current SNTP time, refreshes DateTime[], and on a day-of-month change
// triggers daily maintenance (weekday recompute, log rotation) plus the
// configurable daily reboot (S_MIDNIGHT_REBOOT_TIME / S_MIDNIGHT_REBOOT_ENABLE
// in settings_store.h). Call once a second (from TimeCheckTask).
void CompileDateTime(void);

// Sets WeekDay from the current local time. Called once a day by CompileDateTime().
void DetermineWeekday(void);

void GetCurrentDayMonthYear(char *out, size_t outsz);          // "DD MMM YY"
void GetCurrentDateAndTime(char *out, size_t outsz);            // "YYYY-MM-DD HH:MM"
void GetCurDisplayDate(char *out, size_t outsz);                 // "YYYY-MM-DD"
void GetCurrentTime(char *out, size_t outsz);                    // "HH:MM"
void GetCurrentDayHrMinSec(char *out, size_t outsz);              // "DDDHH:MMSS"
void GetCurrentDayHrMinSecMonthDate(char *out, size_t outsz);     // "DDDHH:MMSSDDM"

// Log-file trimming utility (keeps only the most recent 'entries' lines).
// ReduceFiles() itself is currently a no-op, matching ARD (log rotation is
// disabled there too — see the commented-out body in 02_Time.cpp).
void ReduceFiles(void);
void ReduceFile(const char *filename, int entries);

#endif // DATETIME_H
