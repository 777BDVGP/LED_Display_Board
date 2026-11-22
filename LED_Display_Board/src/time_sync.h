// =============================================================================
// time_sync.h — NTP Time Synchronisation
// =============================================================================
// ESP-IDF port of 02_Time.cpp's TimeSyncTask: syncs system time over SNTP
// against za.pool.ntp.org, fixed UTC+2 (South Africa, no DST) — same values
// ARD uses — then releases timeSyncSemaphore for any task waiting on it.
// =============================================================================

#ifndef TIME_SYNC_H
#define TIME_SYNC_H

void TimeSyncTask(void *parameter);

#endif // TIME_SYNC_H
