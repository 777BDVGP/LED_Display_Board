// =============================================================================
// web_ntfy.h — NTFY Configuration Page
// =============================================================================
// ESP-IDF port of LED_Display_Board_ARD's 12_NTFY.cpp web page (handleNtfy /
// WTC_NtfyPage / UpdateNtfyPageValues / handleTestNtfy / handleFixEventTopics).
// =============================================================================

#ifndef WEB_NTFY_H
#define WEB_NTFY_H

#include "cJSON.h"
#include "esp_http_server.h"

// Called by ws_hub's dispatcher for ID_NTFY payloads.
void UpdateNtfyPageValues(cJSON *root);

// Registers "/ntfy", "/testntfy", "/fixEventTopics" on the shared server.
void WebNtfy_RegisterRoutes(httpd_handle_t server);

#endif // WEB_NTFY_H
