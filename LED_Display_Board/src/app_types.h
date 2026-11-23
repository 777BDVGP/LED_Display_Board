// =============================================================================
// app_types.h — Shared FreeRTOS Queue Message Structures
// =============================================================================
// Ported from 08_APPEvents.h / 09_Other.h / 11_About.h. Fixed-size char arrays
// are used so these structs can be queued across FreeRTOS task boundaries
// without heap allocation.
// =============================================================================

#ifndef APP_TYPES_H
#define APP_TYPES_H

// Sent through displayQueue from AppLogicTask to DisplayTask when an event fires.
typedef struct {
    char label[64];    // Event label text scrolled on the matrix
    char mess[64];     // Event message text scrolled on the matrix
    char color[8];     // ECO_* colour string (e.g. "White", "Red")
    char buzzer[16];   // BUZ_* pattern string (e.g. "3 Short Beeps")
    char age[12];      // Calculated age string appended when ShowAge is true
    int  repeat;       // Number of scroll repeats (0 = endless until next event)
} DisplayMessage;

// Sent through buzzerQueue to BuzzerTask to request a specific beep pattern.
typedef struct {
    char pattern[16];  // BUZ_* pattern string (e.g. "3 Short Beeps", "Silent")
} BuzzerMessage;

// Sent through eventSyncQueue to ClockNetworkTask for inter-clock event sync.
#define SYNC_SAVE    'S'
#define SYNC_DELETE  'D'
#define SYNC_CATCHUP 'C'  // Push all local event files to one peer that just came back online
#define SYNC_REPLACE 'R'  // Make every peer's set of one file kind match ours (after a CSV import)
#define SYNC_TIMEZONE 'Z' // Send our SETTINGS Time Zone (POSIX TZ string in path) to every peer
typedef struct {
    char type;      // SYNC_SAVE / SYNC_DELETE / SYNC_CATCHUP / SYNC_REPLACE / SYNC_TIMEZONE
    char path[34];  // SPIFFS path e.g. "/Birthday_1.json"; peer IP for CATCHUP;
                    // a SYNC_TAG_* / EVY_TYPE_* kind tag for REPLACE;
                    // the POSIX TZ string for TIMEZONE (longest is 28 chars)
} EventSyncCmd;

#endif // APP_TYPES_H
