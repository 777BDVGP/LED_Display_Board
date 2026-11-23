// =============================================================================
// web_about.h — About Page & Multi-Clock Peer Network
// =============================================================================
// ESP-IDF port of LED_Display_Board_ARD's 11_About.cpp: the About page (WiFi
// info, project info, OTA info, Clock Network config), GET /wifi (edit WiFi
// credentials), and the peer-clock probing/event-sync machinery
// (ClockNetworkTask, enqueueSyncSave/enqueueSyncDelete).
// =============================================================================

#ifndef WEB_ABOUT_H
#define WEB_ABOUT_H

#include "esp_http_server.h"

#define CN_NAME_LEN  40
#define CN_IP_LEN    16

typedef struct {
    int  count;
    char name2[CN_NAME_LEN], ip2[CN_IP_LEN];
    char name3[CN_NAME_LEN], ip3[CN_IP_LEN];
    char name4[CN_NAME_LEN], ip4[CN_IP_LEN];
} ClockNetwork;

extern ClockNetwork g_clockNet;

void ClockNetwork_Load(void);
void ClockNetwork_Save(void);

// Queue a file-push / file-delete to all configured peer clocks. No-ops if
// fewer than 2 clocks are configured. Called from web_events.c after an
// event is saved/deleted.
void enqueueSyncSave(const char *filePath);
void enqueueSyncDelete(const char *filePath);

// After a bulk change (CSV import), make every peer's files of one kind match
// this clock's: the peer first deletes all of that kind (GET /deletetype),
// then every local file of that kind is pushed. kind is one of
// EVY_TYPE_BIR / EVY_TYPE_ANN / EVY_TYPE_REM, SYNC_TAG_ALL_EVENTS or
// SYNC_TAG_VALID_PERIODS.
#define SYNC_TAG_ALL_EVENTS    "AllEvents"
#define SYNC_TAG_VALID_PERIODS "ValidPeriods"
void enqueueSyncReplace(const char *kind);

// Time Zone changed on this clock's SETTINGS page: POST it to every peer's
// /settz (web_settings.c), which applies it without passing it on.
void enqueueSyncTimeZone(const char *tz);

// FreeRTOS task — drains eventSyncQueue and probes peer clocks every 30s.
// Call once, after WiFi connects (mirrors ARD's task start in setup()).
void ClockNetworkTask(void *parameter);

// Registers "/about", "/wifi", "/getname", "/fetchname", "/saveclocks",
// "/clockstatus" on the shared server.
void WebAbout_RegisterRoutes(httpd_handle_t server);

#endif // WEB_ABOUT_H
