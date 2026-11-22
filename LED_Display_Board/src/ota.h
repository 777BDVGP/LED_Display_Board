// =============================================================================
// ota.h — Over-The-Air Firmware Update (ArduinoOTA-compatible receiver)
// =============================================================================
// ESP-IDF port of LED_Display_Board_ARD's 10_OTA.cpp. Plain ESP-IDF has no
// ArduinoOTA equivalent, so this reimplements its UDP-invite + TCP-transfer
// wire protocol so that PlatformIO's `pio run -t upload --upload-port <ip>`
// (espota.py) keeps working unchanged against this firmware.
// =============================================================================

#ifndef OTA_H
#define OTA_H

#include <stdbool.h>

// True for the duration of an active firmware transfer. ClockNetworkTask
// checks this to pause peer HTTP traffic while OTA owns the WiFi radio.
extern volatile bool g_otaInProgress;

// Binds the UDP:3232 OTA discovery listener. Call once from app_main() after
// WiFi has connected, before OTATask is created.
void SetupOTA(void);

// FreeRTOS task — services incoming OTA invites and firmware transfers.
// Reboots the device on a successful update; otherwise loops and keeps
// listening. Created in app_main() (stage 5).
void OTATask(void *parameter);

#endif // OTA_H
