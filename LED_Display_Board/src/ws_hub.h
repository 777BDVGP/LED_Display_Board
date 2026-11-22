// =============================================================================
// ws_hub.h — WebSocket Hub (ESP-IDF port of ARD's webSocketEvent/UnpackJasonPayload)
// =============================================================================
// Runs on the shared httpd server (see web_server.h) at "/ws" — same-origin,
// unlike ARD's separate port-81 WebSocketsServer instance (the embedded page
// JS connects to `ws://<host>/ws` accordingly, not `ws://<host>:81/`).
// =============================================================================

#ifndef WS_HUB_H
#define WS_HUB_H

#include <stdbool.h>

#include "esp_http_server.h"

// WebSocket page-ID bases — the first JSON key of every UpdatePage() payload
// is `ID_<PAGE> + field_index`; ws_hub dispatches on that first key.
#define ID_HOME     50
#define ID_SETTINGS 100
#define ID_EVENTS   200
#define ID_NTFY     250
#define ID_ABOUT    300

// Registers the "/ws" WebSocket route on the shared server.
void WsHub_RegisterRoutes(httpd_handle_t server);

// Broadcasts a raw JSON string to every connected WebSocket client.
void WS_Broadcast(const char *json);

// Broadcasts {"<id>": "<value>"[, "Attribute": "<attr>"]} to every client —
// port of ARD's webParamUpdate(). Key order matters to the client JS (id
// before "Attribute"); cJSON preserves insertion order so this is safe.
void WS_ParamUpdate(const char *value, const char *id, const char *attr);

// Pushes `mess` to the H03 status banner (WS_ParamUpdate(mess, "H03", attr)).
// If resetAfter, signals eventSemaphore so TimeCheckTask clears the banner
// again after ~2s (mirrors ARD's WebMessUpdate()).
void WS_MessUpdate(const char *mess, const char *attr, bool resetAfter);

// Pushes the system name (H01) + current DateTime (H02) to every client —
// call on new connection and once a second from TimeCheckTask.
void WS_PushSystemNameAndTime(void);

#endif // WS_HUB_H
