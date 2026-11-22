// =============================================================================
// web_settings.h — Settings Page
// =============================================================================
// ESP-IDF port of LED_Display_Board_ARD's 07_Settings.cpp web page handler.
// =============================================================================

#ifndef WEB_SETTINGS_H
#define WEB_SETTINGS_H

#include "esp_http_server.h"

// Registers "/settings" on the shared server.
void WebSettings_RegisterRoutes(httpd_handle_t server);

#endif // WEB_SETTINGS_H
