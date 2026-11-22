// =============================================================================
// web_ntfy.c — NTFY Configuration Page
// =============================================================================
// See web_ntfy.h.
// =============================================================================

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "event_store.h"
#include "files.h"
#include "ntfy.h"
#include "settings_store.h"
#include "web_core.h"
#include "web_ntfy.h"
#include "ws_hub.h"

// ─── NTFY Page Row Index Constants (offsets from ID_NTFY) ────────────────────
#define NTFY_CODE      0
#define NTFY_PRIORITY  1
#define NTFY_TOPIC1    2
#define NTFY_TOPIC2    3
#define NTFY_ENABLED   4

// =============================================================================
// GET /ntfy
// =============================================================================

static esp_err_t ntfy_handler(httpd_req_t *req)
{
    esp_err_t err;
    if ((err = WebCore_SendShellOpen(req, "LED Clock - NTFY")) != ESP_OK) return err;
    if ((err = WebCore_SendHeader(req, FA_MESS_06)) != ESP_OK) return err;
    if ((err = WebCore_SendNav(req, PS_NTFY)) != ESP_OK) return err;
    if ((err = WebCore_SendScript(req)) != ESP_OK) return err;

    char buf[1300];
    int pos = snprintf(buf, sizeof(buf),
        "<div class='sec'>NTFY SETTINGS</div>"
        "<div class='gr'><span class='lbl'>Enable NTFY Push Messages</span><input type='checkbox' id='%d'%s></div>"
        "<div class='gr'><span class='lbl'>NTFY Topic Option 1</span><input type='text' id='%d' value='%s'></div>"
        "<div class='gr'><span class='lbl'>NTFY Topic Option 2</span><input type='text' id='%d' value='%s'></div>"
        "<div class='gr'><span class='lbl'>NTFY Topic Name</span><select id='%d'>",
        ID_NTFY + NTFY_ENABLED, g_ntfyEnabled ? " checked" : "",
        ID_NTFY + NTFY_TOPIC1, g_ntfyTopic1, ID_NTFY + NTFY_TOPIC2, g_ntfyTopic2, ID_NTFY + NTFY_CODE);

    const char *topicOpts[4] = { "NO_NTFY", "BOTH_NTFYS", g_ntfyTopic1, g_ntfyTopic2 };
    for (int i = 0; i < 4 && pos < (int)sizeof(buf) - 1; i++) {
        bool sel = strcmp(g_ntfyCode, topicOpts[i]) == 0;
        pos += snprintf(buf + pos, sizeof(buf) - pos, "<option value='%s'%s>%s</option>",
                         topicOpts[i], sel ? " selected" : "", topicOpts[i]);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos,
                     "</select></div><div class='gr'><span class='lbl'>NTFY Priority</span><select id='%d'>",
                     ID_NTFY + NTFY_PRIORITY);

    static const char *const prioOpts[] = { "min", "low", "default", "high", "urgent" };
    for (int i = 0; i < 5 && pos < (int)sizeof(buf) - 1; i++) {
        bool sel = strcmp(g_ntfyPriority, prioOpts[i]) == 0;
        pos += snprintf(buf + pos, sizeof(buf) - pos, "<option value='%s'%s>%s</option>",
                         prioOpts[i], sel ? " selected" : "", prioOpts[i]);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "</select></div>");
    if ((err = httpd_resp_send_chunk(req, buf, pos)) != ESP_OK) return err;

    if ((err = WebCore_SendUpdateButton(req)) != ESP_OK) return err;

    static const char testBtn[] =
        "<script>function ButtonNtfyTest(){fetch('/testntfy');}</script>"
        "<div class='save-row'>"
        "<button class='btn btn-bl btn-sm' onclick='ButtonNtfyTest()'>&#9993; TEST NTFY</button>"
        "</div>";
    if ((err = httpd_resp_send_chunk(req, testBtn, sizeof(testBtn) - 1)) != ESP_OK) return err;

    if (strcmp(g_ntfyCode, "NO_NTFY") == 0) {
        static const char fixBtn[] =
            "<script>function ButtonFixEvents(){"
            "if(confirm('Set ALL event NTFY topics on this clock to NO_NTFY?'))"
            "fetch('/fixEventTopics');}</script>"
            "<div class='save-row'>"
            "<button class='btn btn-rd btn-sm' onclick='ButtonFixEvents()'>"
            "&#9888; SET ALL EVENTS TO NO_NTFY</button></div>";
        if ((err = httpd_resp_send_chunk(req, fixBtn, sizeof(fixBtn) - 1)) != ESP_OK) return err;
    }

    return WebCore_SendShellClose(req);
}

// =============================================================================
// WebSocket update handler
// =============================================================================

static bool isAllDigits(const char *s)
{
    if (!s || s[0] == '\0') return false;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return false;
    }
    return true;
}

void UpdateNtfyPageValues(cJSON *root)
{
    cJSON *kv = NULL;
    cJSON_ArrayForEach(kv, root) {
        if (!isAllDigits(kv->string) || !cJSON_IsString(kv)) continue;
        int field = atoi(kv->string) - ID_NTFY;
        switch (field) {
            case NTFY_CODE:     strlcpy(g_ntfyCode,     kv->valuestring, NTFY_CODE_LEN);  break;
            case NTFY_PRIORITY: strlcpy(g_ntfyPriority, kv->valuestring, NTFY_CODE_LEN);  break;
            case NTFY_TOPIC1:   strlcpy(g_ntfyTopic1,   kv->valuestring, NTFY_TOPIC_LEN); break;
            case NTFY_TOPIC2:   strlcpy(g_ntfyTopic2,   kv->valuestring, NTFY_TOPIC_LEN); break;
            case NTFY_ENABLED:  g_ntfyEnabled = strcmp(kv->valuestring, "true") == 0;      break;
            default: break;
        }
    }
    Ntfy_WriteData();
    WS_MessUpdate("NTFY Updated!", "color: red", true);
}

// =============================================================================
// GET /testntfy
// =============================================================================

static esp_err_t testntfy_handler(httpd_req_t *req)
{
    char sysName[SETTING_VALUE_LEN];
    Settings_GetText(S_SYSTEM_NAME, sysName, sizeof(sysName));
    char msg[SETTING_VALUE_LEN + 16];
    snprintf(msg, sizeof(msg), "TEST FROM %s", sysName);

    Ntfy_Send(msg, g_ntfyPriority, g_ntfyCode);
    WS_MessUpdate("Test NTFY was sent!", "color: red", true);

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

// =============================================================================
// GET /fixEventTopics
// =============================================================================
// Scans every .json file on SPIFFS, and for any that Event_ReadData() accepts
// as a real event (its S_EVT_TYPE key is present), forces S_EVT_NTFY_TOPIC to
// NO_NTFY. Intended for secondary clocks (this clock's own NTFY globally
// disabled) that received event files from a primary clock's NTFY-enabled
// topic via sync — see [[phase-e]] once multi-clock replication exists.
// Event_WriteData() always re-derives its filename from S_EVT_TYPE+VERSION,
// so the original file must be removed first or a duplicate would be created
// (same pattern web_events.c's UpdateEventPageValues uses).
// -----------------------------------------------------------------------------
static int fixAllEventNtfyTopics(void)
{
    int count = 0;
    DIR *dir = opendir(SPIFFS_BASE_PATH);
    if (!dir) return 0;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        if (nlen < 6 || strcmp(ent->d_name + nlen - 5, ".json") != 0) continue;
        char fpath[300];
        snprintf(fpath, sizeof(fpath), "%s/%s", SPIFFS_BASE_PATH, ent->d_name);

        EventRecord tmp;
        Event_InitDefaults(&tmp);
        if (!Event_ReadData(fpath, &tmp)) continue;
        if (strcmp(tmp.fields[S_EVT_NTFY_TOPIC].value, "NO_NTFY") == 0) continue;

        strlcpy(tmp.fields[S_EVT_NTFY_TOPIC].value, "NO_NTFY", EVENT_VALUE_LEN);
        remove(fpath);
        Event_WriteData(&tmp);
        count++;
    }
    closedir(dir);
    return count;
}

static esp_err_t fixEventTopics_handler(httpd_req_t *req)
{
    if (strcmp(g_ntfyCode, "NO_NTFY") != 0) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Not applicable: Topic Name is not NO_NTFY");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK"); // release HTTP immediately, matches ARD

    WS_MessUpdate("Scanning files...", "color: orange", true);
    int n = fixAllEventNtfyTopics();
    Event_RefreshTodayList();

    char msg[64];
    snprintf(msg, sizeof(msg), "Done - %d event file(s) set to NO_NTFY", n);
    WS_MessUpdate(msg, "color: green", true);
    return ESP_OK;
}

// =============================================================================
// Public API
// =============================================================================

void WebNtfy_RegisterRoutes(httpd_handle_t server)
{
    static const httpd_uri_t ntfy_uri  = { .uri = "/ntfy",           .method = HTTP_GET, .handler = ntfy_handler };
    static const httpd_uri_t test_uri  = { .uri = "/testntfy",       .method = HTTP_GET, .handler = testntfy_handler };
    static const httpd_uri_t fix_uri   = { .uri = "/fixEventTopics", .method = HTTP_GET, .handler = fixEventTopics_handler };

    httpd_register_uri_handler(server, &ntfy_uri);
    httpd_register_uri_handler(server, &test_uri);
    httpd_register_uri_handler(server, &fix_uri);
}
