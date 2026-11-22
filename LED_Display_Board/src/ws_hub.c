// =============================================================================
// ws_hub.c — WebSocket Hub
// =============================================================================
// See ws_hub.h. Ported from LED_Display_Board_ARD's 04_WebPages.cpp
// (webSocketEvent / UnpackJasonPayload / webParamUpdate / WebMessUpdate).
// =============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"

#include "datetime.h"
#include "exercises.h"
#include "globals.h"
#include "settings_store.h"
#include "web_server.h"
#include "ws_hub.h"

static const char *TAG = "ws_hub";

// Update handlers, implemented in their own page modules; forward-declared
// here rather than via a shared header since each is that module's own
// public API surface, and this is their only other caller.
void UpdateSettingsPageValues(cJSON *root); // web_settings.c
void UpdateEventPageValues(cJSON *root);    // web_events.c
void UpdateNtfyPageValues(cJSON *root);     // web_ntfy.c

typedef struct {
    httpd_handle_t hd;
    int             fd;
    char           *payload; // owned by this struct, freed after send
} ws_send_arg_t;

static void ws_send_work(void *arg)
{
    ws_send_arg_t *a = arg;
    httpd_ws_frame_t frame = { 0 };
    frame.type    = HTTPD_WS_TYPE_TEXT;
    frame.payload = (uint8_t *)a->payload;
    frame.len     = strlen(a->payload);
    httpd_ws_send_frame_async(a->hd, a->fd, &frame);
    free(a->payload);
    free(a);
}

void WS_Broadcast(const char *json)
{
    httpd_handle_t server = WebServer_Handle();
    if (!server || !json) return;

    int fds[10];
    size_t numFds = 10;
    if (httpd_get_client_list(server, &numFds, fds) != ESP_OK) return;

    for (size_t i = 0; i < numFds; i++) {
        if (httpd_ws_get_fd_info(server, fds[i]) != HTTPD_WS_CLIENT_WEBSOCKET) continue;

        ws_send_arg_t *a = malloc(sizeof(ws_send_arg_t));
        if (!a) continue;
        a->hd = server;
        a->fd = fds[i];
        a->payload = strdup(json);
        if (!a->payload) {
            free(a);
            continue;
        }

        if (httpd_queue_work(server, ws_send_work, a) != ESP_OK) {
            free(a->payload);
            free(a);
        }
    }
}

void WS_ParamUpdate(const char *value, const char *id, const char *attr)
{
    cJSON *doc = cJSON_CreateObject();
    cJSON_AddStringToObject(doc, id, value);
    if (attr && attr[0] != '\0') cJSON_AddStringToObject(doc, "Attribute", attr);

    char *out = cJSON_PrintUnformatted(doc);
    cJSON_Delete(doc);
    if (out) {
        WS_Broadcast(out);
        free(out);
    }
}

void WS_MessUpdate(const char *mess, const char *attr, bool resetAfter)
{
    WS_ParamUpdate(mess, "H03", attr);
    if (resetAfter) {
        xSemaphoreGive(eventSemaphore);
    }
}

void WS_PushSystemNameAndTime(void)
{
    char sysName[SETTING_VALUE_LEN];
    Settings_GetText(S_SYSTEM_NAME, sysName, sizeof(sysName));
    WS_ParamUpdate(sysName, "H01", "");
    WS_ParamUpdate(DateTime, "H02", "");
}

// -----------------------------------------------------------------------------
// ws_dispatch
// Mirrors ARD's UnpackJasonPayload(): reads the first JSON key as an integer
// page-ID (a non-numeric key parses to 0, same as Arduino's String::toInt()
// — atoi() has the same fallback behavior), and routes to that page's update
// handler. cJSON preserves object insertion order, so root->child really is
// the first key the browser serialized.
// -----------------------------------------------------------------------------
static void ws_dispatch(const char *payload, size_t len)
{
    cJSON *root = cJSON_ParseWithLength(payload, len);
    if (!root) return;

    cJSON *first = root->child;
    if (!first || !first->string) {
        cJSON_Delete(root);
        return;
    }
    int pageId = atoi(first->string);

    switch (pageId) {
        case ID_HOME:
            break; // no updates from the Home page
        case ID_SETTINGS:
            UpdateSettingsPageValues(root);
            break;
        case ID_EVENTS:
            UpdateEventPageValues(root);
            break;
        case ID_NTFY:
            UpdateNtfyPageValues(root);
            break;
        case 0: {
            cJSON *kv = NULL;
            cJSON_ArrayForEach(kv, root) {
                if (strcmp(kv->string, "REBOOT") == 0 && cJSON_IsString(kv) && strcmp(kv->valuestring, "YES") == 0) {
                    ESP_LOGW(TAG, "Reboot requested via web UI");
                    WS_MessUpdate("Rebooted, Communications lost!", "color: red", true);
                    vTaskDelay(pdMS_TO_TICKS(100));
                    esp_restart();
                } else if (strcmp(kv->string, "ex_start") == 0 && cJSON_IsString(kv)) {
                    ExerciseStart(kv->valuestring);
                    break;
                } else if (strcmp(kv->string, "ex_stop") == 0) {
                    ExerciseStop();
                    break;
                }
            }
            break;
        }
        default:
            break; // unrecognized page ID — silently dropped, matches ARD
    }

    cJSON_Delete(root);
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        // Handshake just completed — broadcast H01/H02 to every client, matching
        // ARD's WStype_CONNECTED behavior (broadcast, not unicast to the new client).
        WS_PushSystemNameAndTime();
        return ESP_OK;
    }

    httpd_ws_frame_t frame = { .type = HTTPD_WS_TYPE_TEXT };
    esp_err_t ret = httpd_ws_recv_frame(req, &frame, 0);
    if (ret != ESP_OK) return ret;
    if (frame.len == 0) return ESP_OK;

    uint8_t *buf = calloc(1, frame.len + 1);
    if (!buf) return ESP_ERR_NO_MEM;
    frame.payload = buf;
    ret = httpd_ws_recv_frame(req, &frame, frame.len);
    if (ret == ESP_OK) {
        ws_dispatch((const char *)buf, frame.len);
    }
    free(buf);
    return ret;
}

void WsHub_RegisterRoutes(httpd_handle_t server)
{
    static const httpd_uri_t ws_uri = {
        .uri          = "/ws",
        .method       = HTTP_GET,
        .handler      = ws_handler,
        .is_websocket = true,
    };
    httpd_register_uri_handler(server, &ws_uri);
}
