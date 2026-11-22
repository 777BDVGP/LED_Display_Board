// =============================================================================
// web_home.h — Home Page + Other Hub Page
// =============================================================================
// ESP-IDF port of LED_Display_Board_ARD's 06_Home.cpp.
// =============================================================================

#ifndef WEB_HOME_H
#define WEB_HOME_H

#include "esp_http_server.h"

// Registers "/" (today's events) and "/other" (nav hub) on the shared server.
void WebHome_RegisterRoutes(httpd_handle_t server);

#endif // WEB_HOME_H
