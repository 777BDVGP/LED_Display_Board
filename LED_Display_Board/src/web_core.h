// =============================================================================
// web_core.h — Shared Page Shell, CSS, Nav, Header Banner, WebSocket JS
// =============================================================================
// ESP-IDF port of LED_Display_Board_ARD's 04_WebPages.cpp shared plumbing.
// Every page handler streams: ShellOpen -> Header -> Nav -> Script -> (its own
// content) -> [UpdateButton[Nav]] -> ShellClose, using httpd_resp_send_chunk()
// per call, matching ARD's chunked-transfer / bounded-RAM streaming pattern.
// =============================================================================

#ifndef WEB_CORE_H
#define WEB_CORE_H

#include <stdbool.h>

#include "esp_http_server.h"

// ─── Main Navigation Page Identifiers ────────────────────────────────────────
#define PS_HOME         1
#define PS_SETTINGS     2
#define PS_BIRTHDAYS    3
#define PS_ANNIVERSARY  4
#define PS_REMINDERS    5
#define PS_NTFY         6
#define PS_ABOUT        7
#define PS_OTHER        8
#define PS_QUICK        9
#define PS_EXERCISES   10
#define PS_VALIDPER    11

// ─── Header Banner Message Codes (FA_MESS_*) ─────────────────────────────────
#define FA_MESS_01   1  // "Files on SPIFFS"
#define FA_MESS_02   2  // "Click on File to Download"
#define FA_MESS_03   3  // "Upload a File"
#define FA_MESS_04   4  // "Click on File to Delete"
#define FA_MESS_06   6  // "Selection Option"
#define FA_MESS_07   7  // "Files on SPIFFS"
#define FA_MESS_08   8  // "Select Report"
#define FA_MESS_09   9  // "File Not Found"
#define FA_MESS_10  10  // "File GeneralLog.txt was deleted"
#define FA_MESS_11  11  // "File IrrigationLog.txt was deleted"
#define FA_MESS_RED 12  // dynamic message, red
#define FA_MESS_GRN 13  // dynamic message, green

// ─── Theme ────────────────────────────────────────────────────────────────────
// Persisted to /spiffs/Theme.json; toggled via GET /settheme?v=0|1.
extern bool g_themeLight;
void WebCore_LoadTheme(void);
void WebCore_RegisterRoutes(httpd_handle_t server); // registers /settheme

// One-shot override for the next header banner (mirrors ARD's SetMess/SpesialStr).
// Consumed exactly once by the next WebCore_SendHeader() call.
void WebCore_SetMessage(int code, const char *text);

// ─── Page Shell Pieces (each sends exactly one or more httpd chunks) ────────
esp_err_t WebCore_SendShellOpen(httpd_req_t *req, const char *title);
esp_err_t WebCore_SendHeader(httpd_req_t *req, int reqMess);
esp_err_t WebCore_SendNav(httpd_req_t *req, int active);
esp_err_t WebCore_SendScript(httpd_req_t *req);
esp_err_t WebCore_SendUpdateButton(httpd_req_t *req);
esp_err_t WebCore_SendUpdateButtonNav(httpd_req_t *req, const char *label, const char *dest);
esp_err_t WebCore_SendShellClose(httpd_req_t *req);

#endif // WEB_CORE_H
