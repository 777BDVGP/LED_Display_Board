// =============================================================================
// web_server.h — Shared HTTP Server Instance
// =============================================================================
// One httpd_handle_t for the whole app (page routes, file manager, WebSocket),
// unlike ARD's split ports (80 for WebServer, 81 for WebSocketsServer). Each
// page module registers its own routes onto the handle this returns.
// =============================================================================

#ifndef WEB_SERVER_H
#define WEB_SERVER_H

#include "esp_http_server.h"

// Starts the shared server (WebSocket support required — enabled via
// CONFIG_HTTPD_WS_SUPPORT). Call once; returns NULL on failure.
httpd_handle_t WebServer_Start(void);

// Returns the running server handle (NULL if not started), for modules that
// need it after the fact (e.g. the WebSocket broadcast helper).
httpd_handle_t WebServer_Handle(void);

#endif // WEB_SERVER_H
