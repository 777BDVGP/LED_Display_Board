// =============================================================================
// files.h — SPIFFS File System Setup
// =============================================================================
// ESP-IDF port of 03_Files.cpp's InitFileSystem() / InitOperationalObjects().
// =============================================================================

#ifndef FILES_H
#define FILES_H

#define SPIFFS_BASE_PATH  "/spiffs"

// Mounts SPIFFS. If the format-sentinel file (/spiffs/FormatComplete.txt) is
// absent, this is treated as a first boot: the sentinel is created and
// InitOperationalObjects() is skipped (there is nothing to load yet). On a
// normal boot (sentinel present) InitOperationalObjects() loads persisted
// application state. Called once from app_main() before WiFi is set up.
void InitFileSystem(void);

// Appends a timestamped message to /spiffs/GeneralEvents.txt, gated by the
// S_SAVELOG setting. No-ops silently (including if the setting is off or the
// file can't be opened).
void SaveGenReport(const char *message);

#endif // FILES_H
