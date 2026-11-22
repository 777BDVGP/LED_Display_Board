// =============================================================================
// web_events.h — Birthdays / Anniversary / Reminders / Quick Pages
// =============================================================================
// ESP-IDF port of LED_Display_Board_ARD's 08_WEBEvents.cpp web page handlers.
// =============================================================================

#ifndef WEB_EVENTS_H
#define WEB_EVENTS_H

#include "esp_http_server.h"

// Registers "/birthdays", "/anniversary", "/reminders", "/quick" on the shared server.
void WebEvents_RegisterRoutes(httpd_handle_t server);

#endif // WEB_EVENTS_H
