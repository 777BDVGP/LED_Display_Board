// =============================================================================
// time_sync.h — NTP Time Synchronisation + Time Zone
// =============================================================================
// Syncs system time over SNTP against pool.ntp.org, then releases
// timeSyncSemaphore for any task waiting on it. The local time zone is the
// SETTINGS "Time Zone" (S_TIME_ZONE, a POSIX TZ string such as "GMT0" or
// "SAST-2"); default Greenwich (GMT0), or South Africa for a private build
// with PRIVATE_DEFAULT_TZ_SAST in wifi_secrets.h.
// =============================================================================

#ifndef TIME_SYNC_H
#define TIME_SYNC_H

typedef struct {
    const char *label;   // shown on the SETTINGS page
    const char *posix;   // stored value / TZ environment string
} TzOption;

extern const TzOption g_tzOptions[];
extern const int      g_tzOptionCount;

// Applies the S_TIME_ZONE setting (setenv TZ + tzset). Safe to call again
// after the setting changes.
void TimeSync_ApplyTimeZone(void);

void TimeSyncTask(void *parameter);

#endif // TIME_SYNC_H
