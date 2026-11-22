// =============================================================================
// web_about.c — About Page & Multi-Clock Peer Network
// =============================================================================
// See web_about.h.
// =============================================================================

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include "app_types.h"
#include "event_store.h"
#include "files.h"
#include "fw_update.h"
#include "globals.h"
#include "ota.h"
#include "settings_store.h"
#include "valid_periods.h"
#include "web_about.h"
#include "web_core.h"
#include "wifi_setup.h"
#include "ws_hub.h"

static const char *TAG = "web_about";

#define CLOCK_NETWORK_FILE_PATH "/ClockNetwork.json"

ClockNetwork g_clockNet = { .count = 1 };

// clockLiveNames[2..4] holds the last confirmed name for each peer slot; "" =
// unreachable/--Waiting--. probeFailCount tracks consecutive probe failures so
// a peer is only declared offline after 3 misses (avoids false dropouts on
// brief network blips).
static char s_liveNames[5][CN_NAME_LEN];
static int  s_probeFailCount[5] = { 0, 0, 0, 0, 0 };
static bool s_allPeersReady     = false;

// =============================================================================
// HTTP Client Helpers
// =============================================================================

typedef struct { char *buf; size_t cap; size_t len; } http_resp_buf_t;

static esp_err_t http_capture_event_handler(esp_http_client_event_t *evt)
{
    http_resp_buf_t *rb = (http_resp_buf_t *)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && rb && rb->cap > rb->len + 1) {
        size_t room = rb->cap - rb->len - 1;
        size_t n = (size_t)evt->data_len < room ? (size_t)evt->data_len : room;
        if (n > 0) {
            memcpy(rb->buf + rb->len, evt->data, n);
            rb->len += n;
            rb->buf[rb->len] = '\0';
        }
    }
    return ESP_OK;
}

// GET url, capturing the response body into out (empty string on any failure
// or non-200 status). Returns true on HTTP 200.
static bool httpGetText(const char *url, char *out, size_t outsz)
{
    out[0] = '\0';
    http_resp_buf_t rb = { .buf = out, .cap = outsz, .len = 0 };
    esp_http_client_config_t config = {
        .url           = url,
        .method        = HTTP_METHOD_GET,
        .timeout_ms    = 3000,
        .event_handler = http_capture_event_handler,
        .user_data     = &rb,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return false;
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);
    return err == ESP_OK && status == 200;
}

static bool httpPostBody(const char *url, const char *body)
{
    esp_http_client_config_t config = { .url = url, .method = HTTP_METHOD_POST, .timeout_ms = 3000 };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return false;
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, body, strlen(body));
    esp_err_t err = esp_http_client_perform(client);
    esp_http_client_cleanup(client);
    return err == ESP_OK;
}

// =============================================================================
// /ClockNetwork.json Persistence
// =============================================================================

void ClockNetwork_Load(void)
{
    char path[64];
    snprintf(path, sizeof(path), "%s%s", SPIFFS_BASE_PATH, CLOCK_NETWORK_FILE_PATH);
    FILE *f = fopen(path, "r");
    if (!f) return;

    char buf[513];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) return;

    cJSON *item;
    if ((item = cJSON_GetObjectItemCaseSensitive(root, "count")) && cJSON_IsNumber(item))
        g_clockNet.count = item->valueint;
    if (g_clockNet.count < 1) g_clockNet.count = 1;
    if (g_clockNet.count > 4) g_clockNet.count = 4;

    #define LOAD_STR(key, field) \
        if ((item = cJSON_GetObjectItemCaseSensitive(root, key)) && cJSON_IsString(item)) \
            strlcpy(g_clockNet.field, item->valuestring, sizeof(g_clockNet.field));
    LOAD_STR("name2", name2) LOAD_STR("ip2", ip2)
    LOAD_STR("name3", name3) LOAD_STR("ip3", ip3)
    LOAD_STR("name4", name4) LOAD_STR("ip4", ip4)
    #undef LOAD_STR

    cJSON_Delete(root);
}

void ClockNetwork_Save(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "count", g_clockNet.count);
    cJSON_AddStringToObject(root, "name2", g_clockNet.name2);
    cJSON_AddStringToObject(root, "ip2", g_clockNet.ip2);
    cJSON_AddStringToObject(root, "name3", g_clockNet.name3);
    cJSON_AddStringToObject(root, "ip3", g_clockNet.ip3);
    cJSON_AddStringToObject(root, "name4", g_clockNet.name4);
    cJSON_AddStringToObject(root, "ip4", g_clockNet.ip4);

    char *out = cJSON_Print(root);
    cJSON_Delete(root);
    if (!out) return;

    char path[64];
    snprintf(path, sizeof(path), "%s%s", SPIFFS_BASE_PATH, CLOCK_NETWORK_FILE_PATH);
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(out, f);
        fclose(f);
    } else {
        ESP_LOGW(TAG, "Failed to write ClockNetwork.json");
    }
    free(out);
}

static const char *peerIp(int slot)
{
    return (slot == 2) ? g_clockNet.ip2 : (slot == 3) ? g_clockNet.ip3 : g_clockNet.ip4;
}

// =============================================================================
// Peer Probing & Event Sync
// =============================================================================

// Probe all configured peer clocks via GET /getname; update s_liveNames[].
// Declares a peer offline only after 3 consecutive failures. On reconnect
// (empty -> live name transition) queues a SYNC_CATCHUP to push all local
// event files to that peer.
static void probeAllPeers(void)
{
    for (int i = 2; i <= 4; i++) {
        const char *ip = peerIp(i);
        if (i > g_clockNet.count || ip[0] == '\0') {
            s_liveNames[i][0] = '\0';
            s_probeFailCount[i] = 0;
            continue;
        }

        bool prevEmpty = (s_liveNames[i][0] == '\0');

        char url[64];
        snprintf(url, sizeof(url), "http://%s/getname", ip);
        char name[CN_NAME_LEN];
        bool ok = httpGetText(url, name, sizeof(name));
        // trim
        size_t len = strlen(name);
        while (len > 0 && (name[len-1] == '\n' || name[len-1] == '\r' || name[len-1] == ' ')) name[--len] = '\0';

        if (ok && name[0] != '\0') {
            strlcpy(s_liveNames[i], name, sizeof(s_liveNames[i]));
            s_probeFailCount[i] = 0;
            if (prevEmpty) {
                EventSyncCmd cmd = { .type = SYNC_CATCHUP };
                strlcpy(cmd.path, ip, sizeof(cmd.path));
                xQueueSend(eventSyncQueue, &cmd, 0);
            }
        } else {
            s_probeFailCount[i]++;
            if (s_probeFailCount[i] >= 3) {
                s_liveNames[i][0] = '\0';
            }
        }

        if (i < 4) vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (g_clockNet.count < 2) {
        s_allPeersReady = true;
    } else {
        bool allOk = true;
        for (int i = 2; i <= g_clockNet.count; i++) {
            if (s_liveNames[i][0] == '\0') { allOk = false; break; }
        }
        s_allPeersReady = allOk;
    }
}

// Does local file d_name belong to sync kind tag (see enqueueSyncReplace)?
static bool fileMatchesKind(const char *d_name, const char *fpath, const char *kind)
{
    bool isVp = strncmp(d_name, VP_FILE_PREFIX, strlen(VP_FILE_PREFIX)) == 0;
    if (strcmp(kind, SYNC_TAG_VALID_PERIODS) == 0) return isVp;
    if (isVp) return false;

    EventRecord tmp;
    Event_InitDefaults(&tmp);
    if (!Event_ReadData(fpath, &tmp)) return false; // not an event file
    if (strcmp(kind, SYNC_TAG_ALL_EVENTS) == 0) return true;
    return strcmp(tmp.fields[S_EVT_TYPE].value, kind) == 0;
}

static char s_replaceBody[4096]; // only ClockNetworkTask touches this

// SYNC_REPLACE for one peer: wipe that kind on the peer, then push ours.
static void replaceKindOnPeer(const char *ip, const char *kind)
{
    char url[320];
    char reply[8];
    snprintf(url, sizeof(url), "http://%s/deletetype?t=%s", ip, kind);
    if (!httpGetText(url, reply, sizeof(reply))) {
        ESP_LOGW(TAG, "Replace %s: peer %s unreachable, skipped", kind, ip);
        return; // offline — don't push into a set we couldn't clear
    }

    DIR *dir = opendir(SPIFFS_BASE_PATH);
    if (!dir) return;
    int pushed = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        if (nlen < 6 || strcmp(ent->d_name + nlen - 5, ".json") != 0) continue;
        char fpath[300];
        snprintf(fpath, sizeof(fpath), "%s/%s", SPIFFS_BASE_PATH, ent->d_name);
        if (!fileMatchesKind(ent->d_name, fpath, kind)) continue;

        FILE *f = fopen(fpath, "r");
        if (!f) continue;
        size_t n = fread(s_replaceBody, 1, sizeof(s_replaceBody) - 1, f);
        fclose(f);
        s_replaceBody[n] = '\0';
        if (n == 0) continue;

        snprintf(url, sizeof(url), "http://%s/receiveevent?f=/%s", ip, ent->d_name);
        if (httpPostBody(url, s_replaceBody)) pushed++;
    }
    closedir(dir);
    ESP_LOGI(TAG, "Replace %s on %s: %d file(s) pushed", kind, ip, pushed);
}

static void executeEventSync(const EventSyncCmd *cmd)
{
    if (cmd->type == SYNC_REPLACE) {
        for (int i = 2; i <= g_clockNet.count; i++) {
            const char *ip = peerIp(i);
            if (ip[0] == '\0') continue;
            replaceKindOnPeer(ip, cmd->path);
        }
        return;
    }

    if (cmd->type == SYNC_SAVE) {
        char fullPath[100];
        snprintf(fullPath, sizeof(fullPath), "%s%s", SPIFFS_BASE_PATH, cmd->path);
        FILE *f = fopen(fullPath, "r");
        if (!f) return;
        char body[4096];
        size_t n = fread(body, 1, sizeof(body) - 1, f);
        fclose(f);
        body[n] = '\0';
        if (n == 0) return;

        for (int i = 2; i <= g_clockNet.count; i++) {
            const char *ip = peerIp(i);
            if (ip[0] == '\0') continue;
            char url[100];
            snprintf(url, sizeof(url), "http://%s/receiveevent?f=%s", ip, cmd->path);
            httpPostBody(url, body);
        }
    } else if (cmd->type == SYNC_DELETE) {
        for (int i = 2; i <= g_clockNet.count; i++) {
            const char *ip = peerIp(i);
            if (ip[0] == '\0') continue;
            char url[100];
            snprintf(url, sizeof(url), "http://%s/deleteevent?f=%s", ip, cmd->path);
            char discard[8];
            httpGetText(url, discard, sizeof(discard));
        }
    } else if (cmd->type == SYNC_CATCHUP) {
        // cmd->path holds the peer's IP in this case (see probeAllPeers()).
        const char *ip = cmd->path;
        if (ip[0] == '\0') return;

        DIR *dir = opendir(SPIFFS_BASE_PATH);
        if (!dir) return;
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            size_t nlen = strlen(ent->d_name);
            if (nlen < 6 || strcmp(ent->d_name + nlen - 5, ".json") != 0) continue;
            char fpath[300];
            snprintf(fpath, sizeof(fpath), "%s/%s", SPIFFS_BASE_PATH, ent->d_name);

            // Reminder Valid Period files sync alongside events; anything else
            // must parse as a valid event file.
            if (strncmp(ent->d_name, VP_FILE_PREFIX, strlen(VP_FILE_PREFIX)) != 0) {
                EventRecord tmp;
                Event_InitDefaults(&tmp);
                if (!Event_ReadData(fpath, &tmp)) continue; // not a valid event file — skip
            }
            // TODO(Phase G): also push /Exer_*.json exercise files once Exercises is ported.

            FILE *ef = fopen(fpath, "r");
            if (!ef) continue;
            char body[4096];
            size_t n = fread(body, 1, sizeof(body) - 1, ef);
            fclose(ef);
            body[n] = '\0';
            if (n == 0) continue;

            char url[320];
            snprintf(url, sizeof(url), "http://%s/receiveevent?f=/%s", ip, ent->d_name);
            httpPostBody(url, body);
        }
        closedir(dir);
    }
}

void enqueueSyncSave(const char *filePath)
{
    if (eventSyncQueue == NULL || g_clockNet.count < 2) return;
    EventSyncCmd cmd = { .type = SYNC_SAVE };
    strlcpy(cmd.path, filePath, sizeof(cmd.path));
    xQueueSend(eventSyncQueue, &cmd, 0);
}

void enqueueSyncDelete(const char *filePath)
{
    if (eventSyncQueue == NULL || g_clockNet.count < 2) return;
    EventSyncCmd cmd = { .type = SYNC_DELETE };
    strlcpy(cmd.path, filePath, sizeof(cmd.path));
    xQueueSend(eventSyncQueue, &cmd, 0);
}

void enqueueSyncReplace(const char *kind)
{
    if (eventSyncQueue == NULL || g_clockNet.count < 2) return;
    EventSyncCmd cmd = { .type = SYNC_REPLACE };
    strlcpy(cmd.path, kind, sizeof(cmd.path));
    // Bulk operation — worth waiting briefly for queue space rather than
    // silently dropping it like a single-file save would.
    if (xQueueSend(eventSyncQueue, &cmd, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGW(TAG, "Sync queue full — replace %s not sent", kind);
    }
}

void ClockNetworkTask(void *parameter)
{
    (void)parameter;

    if (xSemaphoreTake(timeSyncSemaphore, portMAX_DELAY) == pdTRUE) {
        xSemaphoreGive(timeSyncSemaphore);
    }
    ESP_LOGI(TAG, "ClockNetworkTask started");

    TickType_t lastProbe = xTaskGetTickCount();

    for (;;) {
        if (g_otaInProgress) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        TickType_t now   = xTaskGetTickCount();
        TickType_t since = now - lastProbe;
        TickType_t wait  = (since >= pdMS_TO_TICKS(30000)) ? 0 : pdMS_TO_TICKS(30000) - since;

        if (s_allPeersReady) {
            EventSyncCmd cmd;
            if (xQueueReceive(eventSyncQueue, &cmd, wait) == pdTRUE) {
                executeEventSync(&cmd);
                while (xQueueReceive(eventSyncQueue, &cmd, 0) == pdTRUE) {
                    executeEventSync(&cmd);
                }
            }
        } else {
            vTaskDelay(wait);
        }

        if ((xTaskGetTickCount() - lastProbe) >= pdMS_TO_TICKS(30000)) {
            probeAllPeers();
            lastProbe = xTaskGetTickCount();
        }
    }
}

// =============================================================================
// Clock Name Exchange Handlers
// =============================================================================

static esp_err_t getname_handler(httpd_req_t *req)
{
    char sysName[SETTING_VALUE_LEN];
    Settings_GetText(S_SYSTEM_NAME, sysName, sizeof(sysName));
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, sysName);
    return ESP_OK;
}

static esp_err_t fetchname_handler(httpd_req_t *req)
{
    char query[80] = "";
    httpd_req_get_url_query_str(req, query, sizeof(query));
    char ip[CN_IP_LEN] = "";
    httpd_query_key_value(query, "ip", ip, sizeof(ip));

    httpd_resp_set_type(req, "text/plain");
    if (ip[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "");
        return ESP_OK;
    }

    char url[64];
    snprintf(url, sizeof(url), "http://%s/getname", ip);
    char name[CN_NAME_LEN];
    httpGetText(url, name, sizeof(name));
    httpd_resp_sendstr(req, name);
    return ESP_OK;
}

static esp_err_t saveclocks_handler(httpd_req_t *req)
{
    char query[300] = "";
    httpd_req_get_url_query_str(req, query, sizeof(query));

    char val[CN_NAME_LEN];
    if (httpd_query_key_value(query, "count", val, sizeof(val)) == ESP_OK) {
        g_clockNet.count = atoi(val);
        if (g_clockNet.count < 1) g_clockNet.count = 1;
        if (g_clockNet.count > 4) g_clockNet.count = 4;
    }
    if (httpd_query_key_value(query, "name2", val, sizeof(val)) == ESP_OK) strlcpy(g_clockNet.name2, val, sizeof(g_clockNet.name2));
    if (httpd_query_key_value(query, "ip2",   val, sizeof(val)) == ESP_OK) strlcpy(g_clockNet.ip2,   val, sizeof(g_clockNet.ip2));
    if (httpd_query_key_value(query, "name3", val, sizeof(val)) == ESP_OK) strlcpy(g_clockNet.name3, val, sizeof(g_clockNet.name3));
    if (httpd_query_key_value(query, "ip3",   val, sizeof(val)) == ESP_OK) strlcpy(g_clockNet.ip3,   val, sizeof(g_clockNet.ip3));
    if (httpd_query_key_value(query, "name4", val, sizeof(val)) == ESP_OK) strlcpy(g_clockNet.name4, val, sizeof(g_clockNet.name4));
    if (httpd_query_key_value(query, "ip4",   val, sizeof(val)) == ESP_OK) strlcpy(g_clockNet.ip4,   val, sizeof(g_clockNet.ip4));

    ClockNetwork_Save();
    for (int i = 2; i <= 4; i++) {
        s_liveNames[i][0] = '\0';
        s_probeFailCount[i] = 0;
    }
    s_allPeersReady = false;

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

static esp_err_t clockstatus_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name2", s_liveNames[2]);
    cJSON_AddStringToObject(root, "name3", s_liveNames[3]);
    cJSON_AddStringToObject(root, "name4", s_liveNames[4]);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    httpd_resp_set_type(req, "application/json");
    if (out) {
        httpd_resp_sendstr(req, out);
        free(out);
    } else {
        httpd_resp_sendstr(req, "{}");
    }
    return ESP_OK;
}

// =============================================================================
// Build Date / Download Date
// =============================================================================

static void getBuildDate(char *out, size_t outsz)
{
    const char *d = __DATE__; // "Mmm dd yyyy"
    const char *t = __TIME__; // "hh:mm:ss"
    char mon[4] = { d[0], d[1], d[2], 0 };
    char day[3] = { d[4], d[5], 0 };
    char yr[5]  = { d[7], d[8], d[9], d[10], 0 };

    char *p = day;
    while (*p == ' ') p++;
    if (strlen(p) == 1) {
        snprintf(out, outsz, "0%s %s %s %s", p, mon, yr, t);
    } else {
        snprintf(out, outsz, "%s %s %s %s", p, mon, yr, t);
    }
}

// =============================================================================
// GET /about
// =============================================================================

// Minimal HTML escape for SSIDs (they may contain quotes / < / &).
static void htmlEsc(const char *in, char *out, size_t outsz)
{
    size_t o = 0;
    for (; *in && o + 7 < outsz; in++) {
        const char *rep = NULL;
        switch (*in) {
            case '&':  rep = "&amp;";  break;
            case '<':  rep = "&lt;";   break;
            case '>':  rep = "&gt;";   break;
            case '"':  rep = "&quot;"; break;
            case '\'': rep = "&#39;";  break;
            default: break;
        }
        if (rep) { size_t l = strlen(rep); memcpy(out + o, rep, l); o += l; }
        else out[o++] = *in;
    }
    out[o] = '\0';
}

// WiFi info on ABOUT — never the password.
static esp_err_t sendWifiInfoDiv(httpd_req_t *req)
{
    char localIp[16], gwIp[16], ssid[33], ssidEsc[200];
    WifiSetup_GetLocalIPString(localIp, sizeof(localIp));
    WifiSetup_GetGatewayIPString(gwIp, sizeof(gwIp));
    WifiSetup_GetCurrentSsid(ssid, sizeof(ssid));
    htmlEsc(ssid, ssidEsc, sizeof(ssidEsc));

    WifiNet *nets = calloc(WIFI_MAX_NETS, sizeof(WifiNet));
    int saved = nets ? WifiNets_Load(nets, WIFI_MAX_NETS) : 0;
    free(nets);

    char buf[700];
    int n = snprintf(buf, sizeof(buf),
        "<div class='sec'>WIFI</div><div class='ibox'>"
        "<div class='irow'><dt>Connected To</dt><dd>%s</dd></div>"
        "<div class='irow'><dt>IP Address</dt><dd>%s</dd></div>"
        "<div class='irow'><dt>Gateway</dt><dd>%s</dd></div>"
        "<a href='/wifi'><div class='irow'><dt>Saved Networks</dt><dd>%d &rsaquo;</dd></div></a></div>",
        ssidEsc, localIp, gwIp, saved);
    return httpd_resp_send_chunk(req, buf, n);
}

static esp_err_t sendOtaInfoDiv(httpd_req_t *req)
{
    char localIp[16];
    WifiSetup_GetLocalIPString(localIp, sizeof(localIp));

    char buf[512];
    int n = snprintf(buf, sizeof(buf),
        "<div class='sec'>OTA UPDATE</div><div class='ibox'>"
        "<div class='irow'><dt>OTA (Over-The-Air) Updates</dt><input type='text' id='ota_status' value='Ready to Update' readonly style='color:#22c55e;background:transparent;border:none;text-align:right;font-size:13px;padding:0;width:auto'></div>"
        "<div class='irow'><dt>Terminal Command</dt>"
        "<dd style='font-size:0.78rem;word-break:break-all;color:#94a3b8'>pio run -t upload --upload-port %s</dd></div>"
        "</div>", localIp);
    return httpd_resp_send_chunk(req, buf, n);
}

static esp_err_t sendProjectInfoDiv(httpd_req_t *req)
{
    char buildDate[32];
    getBuildDate(buildDate, sizeof(buildDate));

    char buf[500];
    int n = snprintf(buf, sizeof(buf),
        "<div class='sec'>PROJECT INFO</div><div class='ibox'>"
        "<div class='irow'><dt>Project Name</dt><dd>LED Display Board</dd></div>"
        "<div class='irow'><dt>Firmware</dt><dd>" FW_VERSION "</dd></div>"
        "<div class='irow'><dt>Build Date</dt><dd style='font-size:0.8rem'>%s</dd></div>"
        "<div class='irow'><dt>PCB</dt><dd style='font-size:0.78rem'>DevKitC_LED_DRIVER_V14</dd></div>"
        "</div>", buildDate);
    return httpd_resp_send_chunk(req, buf, n);
}

static esp_err_t sendClockRow(httpd_req_t *req, int slot, const char *label, const char *ip)
{
    bool ok = s_liveNames[slot][0] != '\0';
    const char *nm = ok ? s_liveNames[slot] : "--Waiting--";
    char buf[500];
    int n = snprintf(buf, sizeof(buf),
        "<div id='cn_%d' style='display:%s'><div class='gr'><span class='lbl'>%s</span>"
        "<div style='display:flex;gap:6px'>"
        "<input type='text' id='CN_NAME%d' readonly value='%s'%s>"
        "<input type='text' id='CN_IP%d' value='%s' placeholder='0.0.0.0' style='width:130px' onblur='cnFetchName(%d,this.value)'>"
        "</div></div></div>",
        slot, g_clockNet.count >= slot ? "block" : "none", label,
        slot, nm, ok ? " style='width:190px'" : " style='width:190px;color:#64748b;font-style:italic'",
        slot, ip, slot);
    return httpd_resp_send_chunk(req, buf, n);
}

static esp_err_t sendClockNetworkDiv(httpd_req_t *req)
{
    char sysName[SETTING_VALUE_LEN];
    Settings_GetText(S_SYSTEM_NAME, sysName, sizeof(sysName));
    char sysIp[16];
    WifiSetup_GetLocalIPString(sysIp, sizeof(sysIp));

    esp_err_t err;
    char buf[600];
    int pos = snprintf(buf, sizeof(buf),
        "<div class='sec'>CLOCK NETWORK</div>"
        "<div class='gr'><span class='lbl'>Clocks in Use</span><select id='CN_COUNT' onchange='cnShow(this.value)'>");
    for (int i = 1; i <= 4; i++) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "<option value='%d'%s>%d</option>",
                         i, g_clockNet.count == i ? " selected" : "", i);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos,
        "</select></div><div class='gr'><span class='lbl'>This Clock</span>"
        "<span style='color:#94a3b8;font-size:12px'>%s &middot; %s</span></div>", sysName, sysIp);
    if ((err = httpd_resp_send_chunk(req, buf, pos)) != ESP_OK) return err;

    if ((err = sendClockRow(req, 2, "Second", g_clockNet.ip2)) != ESP_OK) return err;
    if ((err = sendClockRow(req, 3, "Third",  g_clockNet.ip3)) != ESP_OK) return err;
    if ((err = sendClockRow(req, 4, "Fourth", g_clockNet.ip4)) != ESP_OK) return err;

    static const char saveAndScript[] =
        "<div class='save-row'><button class='btn btn-bl' onclick='cnSave()'>&#10003; SAVE NETWORK</button>"
        "<span id='CN_STATUS' style='font-size:12px;color:#22c55e;margin-left:12px'></span></div>"
        "<script>"
        "function cnShow(v){var n=parseInt(v);for(var i=2;i<=4;i++){var e=document.getElementById('cn_'+i);if(e)e.style.display=(i<=n)?'block':'none';}}"
        "function cnFetchName(n,ip){ip=ip.trim();if(!ip)return;"
        "fetch('/fetchname?ip='+encodeURIComponent(ip)).then(function(r){return r.text();})"
        ".then(function(name){var el=document.getElementById('CN_NAME'+n);if(el)el.value=name.trim()||'\xe2\x80\x94 not found \xe2\x80\x94';});}"
        "function cnSave(){var p='count='+document.getElementById('CN_COUNT').value;"
        "for(var i=2;i<=4;i++){var nm=document.getElementById('CN_NAME'+i);var ip=document.getElementById('CN_IP'+i);"
        "p+='&name'+i+'='+encodeURIComponent(nm?nm.value:'');"
        "p+='&ip'+i+'='+encodeURIComponent(ip?ip.value:'');}"
        "fetch('/saveclocks?'+p).then(function(){"
        "var s=document.getElementById('CN_STATUS');"
        "if(s){s.textContent='Saved!';setTimeout(function(){s.textContent='';},2000);}"
        "for(var i=2;i<=4;i++){var el=document.getElementById('CN_NAME'+i);"
        "if(el){el.value='--Waiting--';el.style.color='#64748b';el.style.fontStyle='italic';}}"
        "setTimeout(cnPollStatus,1000);});}"
        "function cnPollStatus(){fetch('/clockstatus').then(function(r){return r.json();})"
        ".then(function(d){[{n:2,v:d.name2},{n:3,v:d.name3},{n:4,v:d.name4}].forEach(function(x){"
        "var el=document.getElementById('CN_NAME'+x.n);if(!el)return;"
        "if(x.v&&x.v.length){el.value=x.v;el.style.color='';el.style.fontStyle='normal';}"
        "else{el.value='--Waiting--';el.style.color='#64748b';el.style.fontStyle='italic';}"
        "});}).catch(function(){});}"
        "cnPollStatus();setInterval(cnPollStatus,8000);"
        "</script>";
    return httpd_resp_send_chunk(req, saveAndScript, sizeof(saveAndScript) - 1);
}

static esp_err_t about_handler(httpd_req_t *req)
{
    esp_err_t err;
    if ((err = WebCore_SendShellOpen(req, "LED Clock - About")) != ESP_OK) return err;
    if ((err = WebCore_SendHeader(req, FA_MESS_06)) != ESP_OK) return err;
    if ((err = WebCore_SendNav(req, PS_ABOUT)) != ESP_OK) return err;
    if ((err = WebCore_SendScript(req)) != ESP_OK) return err;

    if ((err = FwUpdate_SendAboutSection(req)) != ESP_OK) return err;
    if ((err = sendOtaInfoDiv(req)) != ESP_OK) return err;

    static const char btns[] =
        "<div class='save-row'>"
        "<button class='btn btn-rd' onclick='ButtonReboot()'>&#9888; REBOOT</button>"
        "<button class='btn btn-rd' onclick=\"window.location.href='/setASP'\">&#9888; RESET ASP</button>"
        "</div>"
        "<div class='save-row'>"
        "<button class='btn btn-bl' onclick=\"window.location.href='/wifi'\">&#9881; WiFi Networks</button>"
        "</div>";
    if ((err = httpd_resp_send_chunk(req, btns, sizeof(btns) - 1)) != ESP_OK) return err;

    if ((err = sendWifiInfoDiv(req)) != ESP_OK) return err;
    if ((err = sendProjectInfoDiv(req)) != ESP_OK) return err;
    if ((err = sendClockNetworkDiv(req)) != ESP_OK) return err;

    return WebCore_SendShellClose(req);
}

// =============================================================================
// GET /setASP — erase all saved WiFi networks + reboot into the setup hotspot
// =============================================================================

static esp_err_t setasp_handler(httpd_req_t *req)
{
    WifiNets_ClearAll();
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "WiFi settings cleared - rebooting...");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK; // unreachable
}

// =============================================================================
// GET /setReboot — soft reboot (no WiFi credential change)
// =============================================================================

static esp_err_t setreboot_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "Rebooting...");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return ESP_OK; // unreachable
}

// =============================================================================
// GET /wifi — WIFI NETWORKS: saved list (never the passwords), DELETE, ADD
// POST /wifinet/add, /wifinet/delete  (form-urlencoded) — used at next start
// =============================================================================

static const char WN_TOP[] =
    "<div class='sec'>WIFI NETWORKS</div>"
    "<div style='font-size:12px;opacity:.8;margin:4px 0 8px'>At start-up the clock tries these networks, the strongest "
    "one in range first. If none can be reached for a few minutes it opens the hotspot <b>" WIFI_SETUP_SSID "</b>: "
    "connect a phone to it and add a network there. Changes are used at the next start. Passwords are never shown.</div>"
    "<div class='ibox'>";

static const char WN_ADD[] =
    "</div>"
    "<div class='sec'>ADD A NETWORK</div>"
    "<div style='font-size:12px;opacity:.8;margin:4px 0'>The same name again replaces that entry.</div>"
    "<div class='gr'><span class='lbl'>Network name</span><input type='text' id='wn_ssid' style='flex:2;width:auto;min-width:0'></div>"
    "<div class='gr'><span class='lbl'>Password</span><input type='password' id='wn_pass' placeholder='empty = open network' "
    "style='flex:2;width:auto;min-width:0;background:#0f172a;border:1px solid #334155;border-radius:4px;color:#e2e8f0;padding:5px 8px'></div>"
    "<div class='gr'><span class='lbl'>IP address</span><span>"
    "<label><input type='radio' name='wn_mode' id='wn_fixed' checked onclick='wnFix(1)'> Fixed</label> &nbsp; "
    "<label><input type='radio' name='wn_mode' id='wn_dhcp' onclick='wnFix(0)'> Automatic (DHCP)</label></span></div>"
    "<div id='wn_fx'>"
    "<div class='gr'><span class='lbl'>Fixed IP</span><input type='text' id='wn_ip' placeholder='e.g. 192.168.1.112'></div>"
    "<div class='gr'><span class='lbl'>Gateway (empty = learned from the router)</span><input type='text' id='wn_gw'></div>"
    "<div class='gr'><span class='lbl'>Subnet mask</span><input type='text' id='wn_sn' value='255.255.255.0'></div>"
    "<div class='gr'><span class='lbl'>DNS (empty = gateway)</span><input type='text' id='wn_dns'></div>"
    "</div>"
    "<div class='save-row'><button class='btn btn-gn' onclick='wnAdd()'>&#43; ADD NETWORK</button>"
    "<span id='wn_msg' style='font-size:12px;margin-left:10px'></span></div>"
    "<script>"
    "function wnFix(x){document.getElementById('wn_fx').style.display=x?'block':'none';}"
    "function wnPost(u,b){var m=document.getElementById('wn_msg');"
    "fetch(u,{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b})"
    ".then(function(r){return r.text();}).then(function(t){m.textContent=t;"
    "if(t.indexOf('OK')==0)setTimeout(function(){location.reload();},1200);})"
    ".catch(function(){m.textContent='No answer from the clock';});}"
    "function wnAdd(){var v=function(i){return encodeURIComponent(document.getElementById(i).value.trim());};"
    "var b='ssid='+v('wn_ssid')+'&pass='+encodeURIComponent(document.getElementById('wn_pass').value)+"
    "'&mode='+(document.getElementById('wn_fixed').checked?'fixed':'dhcp')+'&ip='+v('wn_ip')+'&gw='+v('wn_gw')+'&sn='+v('wn_sn')+'&dns='+v('wn_dns');"
    "wnPost('/wifinet/add',b);}"
    "function wnDel(s,cur){if(!confirm('Delete the WiFi network '+s+'?'+(cur?' The clock is connected to it now: it stays "
    "connected until the next start, then tries the other networks.':'')))return;"
    "wnPost('/wifinet/delete','ssid='+encodeURIComponent(s));}"
    "</script>";

static esp_err_t wifi_handler(httpd_req_t *req)
{
    esp_err_t err;
    if ((err = WebCore_SendShellOpen(req, "LED Clock - WiFi Networks")) != ESP_OK) return err;
    if ((err = WebCore_SendHeader(req, FA_MESS_06)) != ESP_OK) return err;
    if ((err = WebCore_SendNav(req, PS_OTHER)) != ESP_OK) return err;
    if ((err = WebCore_SendScript(req)) != ESP_OK) return err;
    if ((err = httpd_resp_send_chunk(req, WN_TOP, sizeof(WN_TOP) - 1)) != ESP_OK) return err;

    WifiNet *nets = calloc(WIFI_MAX_NETS, sizeof(WifiNet));
    int n = nets ? WifiNets_Load(nets, WIFI_MAX_NETS) : 0;
    char cur[33];
    WifiSetup_GetCurrentSsid(cur, sizeof(cur));

    if (n == 0) {
        static const char none[] = "<div class='irow'><dt>(no networks saved)</dt><dd></dd></div>";
        httpd_resp_send_chunk(req, none, sizeof(none) - 1);
    }
    for (int i = 0; i < n; i++) {
        char esc[200], ipb[16], how[64];
        htmlEsc(nets[i].ssid, esc, sizeof(esc));
        bool on = strcmp(nets[i].ssid, cur) == 0;
        if (nets[i].dhcp) {
            strlcpy(how, "automatic IP", sizeof(how));
        } else {
            WifiSetup_FormatIp(nets[i].ip, ipb, sizeof(ipb));
            bool learn = !(nets[i].gw[0] | nets[i].gw[1] | nets[i].gw[2] | nets[i].gw[3]);
            snprintf(how, sizeof(how), "fixed %s%s", ipb, learn ? " (gateway learned at next start)" : "");
        }
        char row[700];
        int rn = snprintf(row, sizeof(row),
            "<div class='irow'><dt><b>%s</b>%s<br><span style='font-size:11px;opacity:.7'>%s</span></dt>"
            "<dd><button class='btn btn-rd btn-sm' data-s='%s' onclick='wnDel(this.dataset.s,%d)'>DELETE</button></dd></div>",
            esc, on ? " <span style='color:#22c55e;font-size:11px'>(connected)</span>" : "", how, esc, on ? 1 : 0);
        if ((err = httpd_resp_send_chunk(req, row, rn)) != ESP_OK) { free(nets); return err; }
    }
    free(nets);

    if ((err = httpd_resp_send_chunk(req, WN_ADD, sizeof(WN_ADD) - 1)) != ESP_OK) return err;
    return WebCore_SendShellClose(req);
}

static int readFormBody(httpd_req_t *req, char *buf, size_t bufsz)
{
    int total = req->content_len;
    if (total <= 0 || (size_t)total >= bufsz) return -1;
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, buf + got, total - got);
        if (r <= 0) return -1;
        got += r;
    }
    buf[got] = '\0';
    return got;
}

static esp_err_t wifinet_add_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    char body[600];
    if (readFormBody(req, body, sizeof(body)) < 0) return httpd_resp_sendstr(req, "Bad request");
    WifiNet n;
    char err[120];
    if (!WifiNets_ParseForm(body, &n, err, sizeof(err))) return httpd_resp_sendstr(req, err);
    if (!WifiNets_Add(&n)) return httpd_resp_sendstr(req, "Could not save - try again");
    char msg[80];
    snprintf(msg, sizeof(msg), "OK - %s added", n.ssid);
    return httpd_resp_sendstr(req, msg);
}

static esp_err_t wifinet_delete_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    char body[200];
    if (readFormBody(req, body, sizeof(body)) < 0) return httpd_resp_sendstr(req, "Bad request");
    // Reuse the form parser for the url-decoding of "ssid".
    WifiNet n;
    char err[120];
    WifiNets_ParseForm(body, &n, err, sizeof(err));
    if (!n.ssid[0]) return httpd_resp_sendstr(req, "No network given");
    if (!WifiNets_Delete(n.ssid)) return httpd_resp_sendstr(req, "Not in the list");
    char msg[80];
    snprintf(msg, sizeof(msg), "OK - %s deleted", n.ssid);
    return httpd_resp_sendstr(req, msg);
}

// =============================================================================
// Public API
// =============================================================================

void WebAbout_RegisterRoutes(httpd_handle_t server)
{
    static const httpd_uri_t about_uri       = { .uri = "/about",       .method = HTTP_GET, .handler = about_handler };
    static const httpd_uri_t wifi_uri        = { .uri = "/wifi",        .method = HTTP_GET, .handler = wifi_handler };
    static const httpd_uri_t setasp_uri      = { .uri = "/setASP",      .method = HTTP_GET, .handler = setasp_handler };
    static const httpd_uri_t setreboot_uri   = { .uri = "/setReboot",   .method = HTTP_GET, .handler = setreboot_handler };
    static const httpd_uri_t getname_uri     = { .uri = "/getname",     .method = HTTP_GET, .handler = getname_handler };
    static const httpd_uri_t fetchname_uri   = { .uri = "/fetchname",   .method = HTTP_GET, .handler = fetchname_handler };
    static const httpd_uri_t saveclocks_uri  = { .uri = "/saveclocks",  .method = HTTP_GET, .handler = saveclocks_handler };
    static const httpd_uri_t clockstatus_uri = { .uri = "/clockstatus", .method = HTTP_GET, .handler = clockstatus_handler };

    httpd_register_uri_handler(server, &about_uri);
    httpd_register_uri_handler(server, &wifi_uri);
    static const httpd_uri_t wnadd_uri = { .uri = "/wifinet/add",    .method = HTTP_POST, .handler = wifinet_add_handler };
    static const httpd_uri_t wndel_uri = { .uri = "/wifinet/delete", .method = HTTP_POST, .handler = wifinet_delete_handler };
    httpd_register_uri_handler(server, &wnadd_uri);
    httpd_register_uri_handler(server, &wndel_uri);
    httpd_register_uri_handler(server, &setasp_uri);
    httpd_register_uri_handler(server, &setreboot_uri);
    httpd_register_uri_handler(server, &getname_uri);
    httpd_register_uri_handler(server, &fetchname_uri);
    httpd_register_uri_handler(server, &saveclocks_uri);
    httpd_register_uri_handler(server, &clockstatus_uri);
}
