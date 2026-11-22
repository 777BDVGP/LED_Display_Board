// =============================================================================
// wifi_setup.c — WiFi join (saved networks / known networks) + setup hotspot
// =============================================================================
// See wifi_setup.h for the start-up order. ESP-IDF port of the RainGauge
// project's 05_WiFi.cpp (station via esp_wifi + esp_netif, hotspot page via
// esp_http_server + the copied ESP-IDF example dns_server).
// =============================================================================

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "cJSON.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "dns_server.h"
#include "files.h"
#include "wifi_setup.h"

// Optional private networks — wifi_secrets.h is git-ignored and never copied
// into the public project (see wifi_secrets.example.h).
#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#endif

static const char *TAG = "wifi_setup";

#define WIFI_CONFIG_PATH   SPIFFS_BASE_PATH "/WiFiConfig.json"
#define WIFI_OLD_PATH      SPIFFS_BASE_PATH "/WifiSettings.json"   // pre-list single network

#define WIFI_ROUNDS        6                  // saved + known rounds before the setup hotspot
#define WIFI_SAVED_TRIES   2                  // attempts per round when only ONE network is saved
#define WIFI_CONNECT_MS    20000              // per join attempt
#define WIFI_ROUND_GAP_MS  10000              // pause between rounds (router may be restarting)
#define WIFI_PORTAL_MS     (5 * 60 * 1000)    // hotspot after failed rounds, then reboot
#define WIFI_SCAN_MAX      24

typedef struct {
    const char *ssid;
    const char *pass;
    uint8_t     ip[4];   // 0.0.0.0 = automatic IP (DHCP)
} KnownNet;

#ifdef WIFI_KNOWN_NETS
static const KnownNet KNOWN_NETS[] = { WIFI_KNOWN_NETS };
#define KNOWN_NET_COUNT ((int)(sizeof(KNOWN_NETS) / sizeof(KNOWN_NETS[0])))
#else
#define KNOWN_NET_COUNT 0                          // public firmware: setup hotspot only
#endif

static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif  = NULL;
static bool         s_wifiStarted = false;

// =============================================================================
// Small helpers
// =============================================================================

static bool ipIsZero(const uint8_t ip[4]) { return !(ip[0] | ip[1] | ip[2] | ip[3]); }

void WifiSetup_FormatIp(const uint8_t ip[4], char *out, size_t outsz)
{
    snprintf(out, outsz, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
}

static bool parseIp(const char *s, uint8_t out[4])
{
    if (!s) return false;
    int a, b, c, d;
    char tail;
    if (sscanf(s, "%d.%d.%d.%d%c", &a, &b, &c, &d, &tail) != 4) return false;
    if (a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255 || d < 0 || d > 255) return false;
    out[0] = a; out[1] = b; out[2] = c; out[3] = d;
    return true;
}

static void addrToIp(uint32_t addr, uint8_t out[4])
{
    // esp_ip4_addr_t is in network byte order: first octet in the low byte.
    out[0] = addr & 0xFF; out[1] = (addr >> 8) & 0xFF; out[2] = (addr >> 16) & 0xFF; out[3] = (addr >> 24) & 0xFF;
}

bool WifiSetup_IsProtectedFile(const char *name)
{
    if (!name) return false;
    while (*name == '/') name++;
    return strcasecmp(name, "WiFiConfig.json") == 0 || strcasecmp(name, "WifiSettings.json") == 0;
}

// In-place decode of an application/x-www-form-urlencoded value.
static void urlDecode(char *buf)
{
    char *src = buf, *dst = buf;
    while (*src) {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else if (*src == '%' && isxdigit((unsigned char)src[1]) && isxdigit((unsigned char)src[2])) {
            char hex[3] = { src[1], src[2], '\0' };
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

static void formField(const char *body, const char *key, char *out, size_t outsz, bool trim)
{
    out[0] = '\0';
    esp_err_t r = httpd_query_key_value(body, key, out, outsz);
    if (r != ESP_OK && r != ESP_ERR_HTTPD_RESULT_TRUNC) { out[0] = '\0'; return; }   // truncated: kept, length checks catch it
    urlDecode(out);
    if (trim) {
        char *s = out;
        while (*s == ' ') s++;
        if (s != out) memmove(out, s, strlen(s) + 1);
        size_t n = strlen(out);
        while (n > 0 && out[n - 1] == ' ') out[--n] = '\0';
    }
}

// =============================================================================
// /WiFiConfig.json — a LIST of networks, newest first
// =============================================================================

static void netFromJson(const cJSON *o, WifiNet *n)
{
    memset(n, 0, sizeof(*n));
    n->dhcp = true;
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(o, "ssid");
    const cJSON *pass = cJSON_GetObjectItemCaseSensitive(o, "pass");
    const cJSON *dhcp = cJSON_GetObjectItemCaseSensitive(o, "dhcp");
    if (cJSON_IsString(ssid)) strlcpy(n->ssid, ssid->valuestring, sizeof(n->ssid));
    if (cJSON_IsString(pass)) strlcpy(n->pass, pass->valuestring, sizeof(n->pass));
    if (cJSON_IsBool(dhcp)) n->dhcp = cJSON_IsTrue(dhcp);
    if (n->dhcp) return;

    const cJSON *ip = cJSON_GetObjectItemCaseSensitive(o, "ip");
    if (!cJSON_IsString(ip) || !parseIp(ip->valuestring, n->ip)) { n->dhcp = true; return; }
    const char *keys[3] = { "gw", "sn", "dns" };
    uint8_t *dst[3] = { n->gw, n->sn, n->dns };
    for (int i = 0; i < 3; i++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, keys[i]);
        if (cJSON_IsString(v)) parseIp(v->valuestring, dst[i]);   // missing = 0.0.0.0 = learn
    }
}

static bool saveNets(const WifiNet *nets, int n)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "nets");
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", nets[i].ssid);
        cJSON_AddStringToObject(o, "pass", nets[i].pass);
        cJSON_AddBoolToObject(o, "dhcp", nets[i].dhcp);
        if (!nets[i].dhcp) {
            char b[16];
            WifiSetup_FormatIp(nets[i].ip, b, sizeof(b));  cJSON_AddStringToObject(o, "ip", b);
            if (!ipIsZero(nets[i].gw))  { WifiSetup_FormatIp(nets[i].gw,  b, sizeof(b)); cJSON_AddStringToObject(o, "gw",  b); }
            if (!ipIsZero(nets[i].sn))  { WifiSetup_FormatIp(nets[i].sn,  b, sizeof(b)); cJSON_AddStringToObject(o, "sn",  b); }
            if (!ipIsZero(nets[i].dns)) { WifiSetup_FormatIp(nets[i].dns, b, sizeof(b)); cJSON_AddStringToObject(o, "dns", b); }
        }
        cJSON_AddItemToArray(arr, o);
    }
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) return false;

    FILE *f = fopen(WIFI_CONFIG_PATH, "w");
    bool ok = false;
    if (f) {
        ok = fputs(txt, f) >= 0;
        fclose(f);
    }
    free(txt);
    if (!ok) ESP_LOGE(TAG, "Could not write WiFiConfig.json");
    return ok;
}

// Converts the old single-network WifiSettings.json ({ssid, pass, ip:[4],
// gw:[4], sn:[4], dns:[4]}) into the list file, then removes it. Only called
// when WiFiConfig.json doesn't exist yet.
static void migrateOldFile(void)
{
    FILE *f = fopen(WIFI_OLD_PATH, "r");
    if (!f) return;
    char buf[600];
    size_t rd = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[rd] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) return;
    WifiNet n;
    memset(&n, 0, sizeof(n));
    const cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    const cJSON *pass = cJSON_GetObjectItemCaseSensitive(root, "pass");
    if (cJSON_IsString(ssid) && ssid->valuestring[0] && cJSON_IsString(pass)) {
        strlcpy(n.ssid, ssid->valuestring, sizeof(n.ssid));
        strlcpy(n.pass, pass->valuestring, sizeof(n.pass));
        const char *keys[4] = { "ip", "gw", "sn", "dns" };
        uint8_t *dst[4] = { n.ip, n.gw, n.sn, n.dns };
        for (int i = 0; i < 4; i++) {
            const cJSON *a = cJSON_GetObjectItemCaseSensitive(root, keys[i]);
            if (cJSON_IsArray(a) && cJSON_GetArraySize(a) == 4)
                for (int k = 0; k < 4; k++) dst[i][k] = (uint8_t)cJSON_GetArrayItem(a, k)->valueint;
        }
        n.dhcp = ipIsZero(n.ip);
        if (saveNets(&n, 1)) {
            remove(WIFI_OLD_PATH);
            char ipb[16];
            WifiSetup_FormatIp(n.ip, ipb, sizeof(ipb));
            ESP_LOGI(TAG, "Converted WifiSettings.json -> WiFiConfig.json (%s on %s)", n.ssid, ipb);
        }
    }
    cJSON_Delete(root);
}

int WifiNets_Load(WifiNet *out, int maxOut)
{
    FILE *f = fopen(WIFI_CONFIG_PATH, "r");
    if (!f) {
        migrateOldFile();
        f = fopen(WIFI_CONFIG_PATH, "r");
        if (!f) return 0;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 4096) { fclose(f); return 0; }
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return 0; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) { ESP_LOGW(TAG, "WiFiConfig.json parse error"); return 0; }

    int n = 0;
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "nets");
    const cJSON *o;
    if (cJSON_IsArray(arr)) {
        cJSON_ArrayForEach(o, arr) {
            if (n >= maxOut) break;
            netFromJson(o, &out[n]);
            if (out[n].ssid[0]) n++;
        }
    }
    cJSON_Delete(root);
    return n;
}

bool WifiNets_Add(const WifiNet *net)
{
    WifiNet *nets = calloc(WIFI_MAX_NETS + 1, sizeof(WifiNet));
    if (!nets) return false;
    int n = WifiNets_Load(nets + 1, WIFI_MAX_NETS);
    nets[0] = *net;
    int k = 1;
    for (int i = 1; i <= n; i++)
        if (strcmp(nets[i].ssid, net->ssid) != 0 && k < WIFI_MAX_NETS) nets[k++] = nets[i];
    bool ok = saveNets(nets, k);
    free(nets);
    return ok;
}

bool WifiNets_Delete(const char *ssid)
{
    WifiNet *nets = calloc(WIFI_MAX_NETS, sizeof(WifiNet));
    if (!nets) return false;
    int n = WifiNets_Load(nets, WIFI_MAX_NETS), k = 0;
    for (int i = 0; i < n; i++) if (strcmp(nets[i].ssid, ssid) != 0) nets[k++] = nets[i];
    bool changed = (k != n);
    if (changed) saveNets(nets, k);
    free(nets);
    return changed;
}

void WifiNets_ClearAll(void)
{
    remove(WIFI_CONFIG_PATH);
    remove(WIFI_OLD_PATH);
    ESP_LOGI(TAG, "All saved WiFi networks removed");
}

bool WifiNets_ParseForm(const char *body, WifiNet *out, char *err, size_t errsz)
{
    memset(out, 0, sizeof(*out));
    err[0] = '\0';
    char ssid[64], pass[96], mode[12], ip[24], gw[24], sn[24], dns[24];
    formField(body, "ssid2", ssid, sizeof(ssid), true);
    if (!ssid[0]) formField(body, "ssid", ssid, sizeof(ssid), true);
    formField(body, "pass", pass, sizeof(pass), false);
    formField(body, "mode", mode, sizeof(mode), true);

    if (!ssid[0] || strlen(ssid) > 32) { strlcpy(err, "Choose or type a WiFi network name (max 32 characters).", errsz); return false; }
    if (strlen(pass) > 63)             { strlcpy(err, "The WiFi password can be at most 63 characters.", errsz); return false; }
    strlcpy(out->ssid, ssid, sizeof(out->ssid));
    strlcpy(out->pass, pass, sizeof(out->pass));
    out->dhcp = strcmp(mode, "fixed") != 0;
    if (out->dhcp) return true;

    formField(body, "ip", ip, sizeof(ip), true);
    formField(body, "gw", gw, sizeof(gw), true);
    formField(body, "sn", sn, sizeof(sn), true);
    formField(body, "dns", dns, sizeof(dns), true);
    if (!parseIp(ip, out->ip)) { strlcpy(err, "The fixed IP must be like 192.168.1.112.", errsz); return false; }
    if (!gw[0]) return true;   // gateway/subnet/DNS learned from the router at the first join
    if (!parseIp(gw, out->gw)) { strlcpy(err, "The gateway must be like 192.168.1.1 (or leave it empty: learned from the router).", errsz); return false; }
    if (!sn[0]) { out->sn[0] = 255; out->sn[1] = 255; out->sn[2] = 255; out->sn[3] = 0; }
    else if (!parseIp(sn, out->sn)) { strlcpy(err, "The subnet mask must be like 255.255.255.0.", errsz); return false; }
    if (!dns[0]) memcpy(out->dns, out->gw, 4);
    else if (!parseIp(dns, out->dns)) { strlcpy(err, "DNS must be like 192.168.1.1 (or leave it empty).", errsz); return false; }
    return true;
}

// =============================================================================
// Station join
// =============================================================================

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define WIFI_STA_MAX_RETRY  4

typedef struct {
    EventGroupHandle_t eg;
    int                retryCount;
} StaConnectCtx;

static void staEventHandler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    StaConnectCtx *ctx = (StaConnectCtx *)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        // The AP may refuse-then-accept (e.g. "comeback time"), so one
        // disconnect is not necessarily a real failure.
        if (ctx->retryCount < WIFI_STA_MAX_RETRY) {
            ctx->retryCount++;
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(ctx->eg, WIFI_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(ctx->eg, WIFI_CONNECTED_BIT);
    }
}

static void wifiStop(void)
{
    if (s_wifiStarted) {
        esp_wifi_disconnect();
        esp_wifi_stop();
        s_wifiStarted = false;
        vTaskDelay(pdMS_TO_TICKS(300));
    }
}

static void wifiStartSta(void)
{
    if (!s_wifiStarted) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
        s_wifiStarted = true;
    }
}

// Joins one network (DHCP or fixed IP). On success the connection stays up.
static bool joinWith(const WifiNet *n)
{
    char ipb[16];
    WifiSetup_FormatIp(n->ip, ipb, sizeof(ipb));
    ESP_LOGI(TAG, "WiFi: joining %s (%s)", n->ssid, n->dhcp ? "DHCP" : ipb);

    wifiStop();
    esp_netif_t *sta = s_sta_netif;
    if (n->dhcp) {
        esp_netif_dhcpc_start(sta);   // "already started" is fine
    } else {
        esp_netif_dhcpc_stop(sta);
        esp_netif_ip_info_t info = { 0 };
        info.ip.addr      = ESP_IP4TOADDR(n->ip[0], n->ip[1], n->ip[2], n->ip[3]);
        info.gw.addr      = ESP_IP4TOADDR(n->gw[0], n->gw[1], n->gw[2], n->gw[3]);
        info.netmask.addr = ESP_IP4TOADDR(n->sn[0], n->sn[1], n->sn[2], n->sn[3]);
        esp_netif_set_ip_info(sta, &info);
        esp_netif_dns_info_t dnsInfo = { 0 };
        dnsInfo.ip.type = ESP_IPADDR_TYPE_V4;
        dnsInfo.ip.u_addr.ip4.addr = ESP_IP4TOADDR(n->dns[0], n->dns[1], n->dns[2], n->dns[3]);
        esp_netif_set_dns_info(sta, ESP_NETIF_DNS_MAIN, &dnsInfo);
    }

    wifi_config_t wc = { 0 };
    // Fixed-size fields: a 32-char SSID / 64-char key has no terminator.
    memcpy(wc.sta.ssid, n->ssid, strnlen(n->ssid, sizeof(wc.sta.ssid)));
    memcpy(wc.sta.password, n->pass, strnlen(n->pass, sizeof(wc.sta.password)));
    wc.sta.threshold.authmode = n->pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    // No modem sleep: avoids RMT timing glitches on the LED matrix.
    esp_wifi_set_ps(WIFI_PS_NONE);

    StaConnectCtx ctx = { .eg = xEventGroupCreate(), .retryCount = 0 };
    esp_event_handler_instance_t instWifi, instIp;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &staEventHandler, &ctx, &instWifi);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &staEventHandler, &ctx, &instIp);

    ESP_ERROR_CHECK(esp_wifi_start());   // STA_START -> esp_wifi_connect()
    s_wifiStarted = true;

    EventBits_t bits = xEventGroupWaitBits(ctx.eg, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(WIFI_CONNECT_MS));

    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instIp);
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instWifi);
    vEventGroupDelete(ctx.eg);

    if (bits & WIFI_CONNECTED_BIT) {
        esp_netif_ip_info_t got;
        esp_netif_get_ip_info(sta, &got);
        ESP_LOGI(TAG, "WiFi: connected to %s, IP = " IPSTR, n->ssid, IP2STR(&got.ip));
        return true;
    }
    ESP_LOGW(TAG, "WiFi: could not join %s", n->ssid);
    wifiStop();
    return false;
}

// Reads gateway / subnet / DNS the router gave on the current DHCP connection.
static void learnFromRouter(WifiNet *n)
{
    esp_netif_ip_info_t info;
    esp_netif_get_ip_info(s_sta_netif, &info);
    addrToIp(info.gw.addr, n->gw);
    addrToIp(info.netmask.addr, n->sn);
    if (ipIsZero(n->dns)) {
        esp_netif_dns_info_t d;
        if (esp_netif_get_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &d) == ESP_OK)
            addrToIp(d.ip.u_addr.ip4.addr, n->dns);
        if (ipIsZero(n->dns)) memcpy(n->dns, n->gw, 4);
    }
}

// Scans and returns the best RSSI per SSID in names[] (INT_MIN = not seen).
static void scanRssi(const char *const *names, int count, int *rssi)
{
    for (int i = 0; i < count; i++) rssi[i] = INT_MIN;
    wifiStop();
    wifiStartSta();
    wifi_ap_record_t *recs = malloc(WIFI_SCAN_MAX * sizeof(wifi_ap_record_t));
    if (!recs) return;
    uint16_t num = WIFI_SCAN_MAX;
    if (esp_wifi_scan_start(NULL, true) == ESP_OK && esp_wifi_scan_get_ap_records(&num, recs) == ESP_OK) {
        for (int j = 0; j < num; j++)
            for (int i = 0; i < count; i++)
                if (strcmp((const char *)recs[j].ssid, names[i]) == 0 && recs[j].rssi > rssi[i]) rssi[i] = recs[j].rssi;
    }
    free(recs);
    wifiStop();
}

// Step 1: saved networks — in range first (strongest), then the rest.
static bool joinSaved(void)
{
    WifiNet *nets = calloc(WIFI_MAX_NETS, sizeof(WifiNet));
    if (!nets) return false;
    int n = WifiNets_Load(nets, WIFI_MAX_NETS);
    if (n == 0) { free(nets); return false; }

    const char *names[WIFI_MAX_NETS];
    int rssi[WIFI_MAX_NETS];
    for (int i = 0; i < n; i++) names[i] = nets[i].ssid;
    scanRssi(names, n, rssi);

    bool tried[WIFI_MAX_NETS] = { false };
    int tries = (n == 1) ? WIFI_SAVED_TRIES : 1;
    bool joined = false;
    for (int k = 0; k < n && !joined; k++) {
        int best = -1;
        for (int i = 0; i < n; i++)
            if (!tried[i] && (best < 0 || rssi[i] > rssi[best])) best = i;
        tried[best] = true;
        WifiNet *c = &nets[best];

        for (int t = 0; t < tries && !joined; t++) {
            bool learned = false;
            if (!c->dhcp && (ipIsZero(c->gw) || ipIsZero(c->sn))) {
                // Join by DHCP once to learn the router's gateway / subnet / DNS.
                WifiNet d = *c;
                d.dhcp = true;
                if (!joinWith(&d)) continue;
                learnFromRouter(c);
                learned = true;
            }
            if (joinWith(c)) {
                joined = true;
                if (learned) {
                    WifiNet *all = calloc(WIFI_MAX_NETS, sizeof(WifiNet));
                    if (all) {
                        int m = WifiNets_Load(all, WIFI_MAX_NETS);
                        for (int i = 0; i < m; i++) if (strcmp(all[i].ssid, c->ssid) == 0) all[i] = *c;
                        saveNets(all, m);
                        free(all);
                    }
                    char gwb[16];
                    WifiSetup_FormatIp(c->gw, gwb, sizeof(gwb));
                    ESP_LOGI(TAG, "WiFi: gateway %s learned from the router and saved", gwb);
                }
            }
        }
    }
    free(nets);
    return joined;
}

// Step 2: built-in networks (private build only). A known network that joins
// is added to the saved list, so a later public firmware still finds it.
static bool scanAndJoinKnown(void)
{
#ifdef WIFI_KNOWN_NETS
    const char *names[KNOWN_NET_COUNT];
    int rssi[KNOWN_NET_COUNT];
    bool tried[KNOWN_NET_COUNT];
    for (int i = 0; i < KNOWN_NET_COUNT; i++) { names[i] = KNOWN_NETS[i].ssid; tried[i] = false; }
    scanRssi(names, KNOWN_NET_COUNT, rssi);

    for (;;) {
        int best = -1;
        for (int i = 0; i < KNOWN_NET_COUNT; i++)
            if (!tried[i] && rssi[i] != INT_MIN && (best < 0 || rssi[i] > rssi[best])) best = i;
        if (best < 0) break;
        tried[best] = true;
        const KnownNet *k = &KNOWN_NETS[best];

        WifiNet c;
        memset(&c, 0, sizeof(c));
        strlcpy(c.ssid, k->ssid, sizeof(c.ssid));
        strlcpy(c.pass, k->pass, sizeof(c.pass));
        c.dhcp = true;
        if (!joinWith(&c)) continue;
        if (!ipIsZero(k->ip)) {
            learnFromRouter(&c);
            c.dhcp = false;
            memcpy(c.ip, k->ip, 4);
            if (!joinWith(&c)) continue;
        }
        WifiNets_Add(&c);
        ESP_LOGI(TAG, "WiFi: joined built-in network %s - added to the saved networks", c.ssid);
        return true;
    }
#endif
    return false;
}

// =============================================================================
// Setup hotspot
// =============================================================================

static char          *s_portalOptions = NULL;   // <option>s from the scan at start
static WifiNet        s_pendingNet;
static volatile bool  s_pendingSave   = false;
static volatile bool  s_pendingCancel = false;

static const char PORTAL_PAGE_1[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'><title>LED Clock WiFi Setup</title>"
    "<style>body{font-family:sans-serif;max-width:420px;margin:0 auto;padding:12px;background:#f8fafc;color:#0f172a}"
    "h2{color:#0284c7;text-align:center;margin:6px 0}label{display:block;margin:10px 0 3px;font-weight:600;font-size:14px}"
    "input,select{width:100%;box-sizing:border-box;padding:8px;font-size:15px;border:1px solid #cbd5e1;border-radius:5px}"
    ".r{display:flex;gap:16px;margin-top:6px}.r label{font-weight:400;margin:0}.r input{width:auto}"
    "button{width:100%;padding:12px;margin-top:16px;font-size:16px;font-weight:700;border:0;border-radius:6px;color:#fff;background:#16a34a}"
    ".c{background:#64748b}p{font-size:13px;color:#475569}</style></head><body>"
    "<h2>LED Clock &mdash; WiFi Setup</h2>"
    "<p>Choose the WiFi network the clock must join. After SAVE it restarts and joins it. "
    "Then open the clock's IP address in a browser on that network.</p>"
    "<form method='POST' action='/save'>"
    "<label>WiFi network</label><select name='ssid'>";

static const char PORTAL_PAGE_2[] =
    "<option value=''>Other (type below)...</option></select>"
    "<label>...or type the network name</label><input name='ssid2' placeholder='only if not in the list'>"
    "<label>WiFi password</label><input name='pass' type='password' placeholder='leave empty for an open network'>"
    "<label>IP address</label>"
    "<div class='r'><label><input type='radio' name='mode' value='fixed' checked onclick='f(1)'> Fixed IP</label>"
    "<label><input type='radio' name='mode' value='dhcp' onclick='f(0)'> Automatic (DHCP)</label></div>"
    "<div id='fx'>"
    "<label>Fixed IP (e.g. 192.168.1.112)</label><input name='ip'>"
    "<label>Gateway / router (empty = learned from the router)</label><input name='gw'>"
    "<label>Subnet mask</label><input name='sn' value='255.255.255.0'>"
    "<label>DNS (empty = same as gateway)</label><input name='dns'>"
    "<p>A fixed IP is recommended: it is how you open the clock's pages and how the clocks find each other.</p></div>"
    "<button type='submit'>SAVE &amp; RESTART</button></form>"
    "<form method='POST' action='/cancel'><button class='c' type='submit'>CANCEL (keep the old setting)</button></form>"
    "<script>function f(x){document.getElementById('fx').style.display=x?'block':'none';}</script>"
    "</body></html>";

static esp_err_t portal_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send_chunk(req, PORTAL_PAGE_1, sizeof(PORTAL_PAGE_1) - 1);
    if (s_portalOptions) httpd_resp_send_chunk(req, s_portalOptions, strlen(s_portalOptions));
    httpd_resp_send_chunk(req, PORTAL_PAGE_2, sizeof(PORTAL_PAGE_2) - 1);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static int readBody(httpd_req_t *req, char *buf, size_t bufsz)
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

static esp_err_t portal_save_handler(httpd_req_t *req)
{
    char body[600];
    httpd_resp_set_type(req, "text/html");
    if (readBody(req, body, sizeof(body)) < 0) return httpd_resp_sendstr(req, "<p>Bad request</p><p><a href='/'>Back</a></p>");

    WifiNet n;
    char err[120];
    if (!WifiNets_ParseForm(body, &n, err, sizeof(err))) {
        char page[300];
        snprintf(page, sizeof(page),
                 "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                 "<p style='font-family:sans-serif;color:#b91c1c'>%s</p><p><a href='/'>Back</a></p>", err);
        return httpd_resp_sendstr(req, page);
    }
    char ipb[16];
    WifiSetup_FormatIp(n.ip, ipb, sizeof(ipb));
    char page[700];
    snprintf(page, sizeof(page),
             "<meta name='viewport' content='width=device-width,initial-scale=1'>"
             "<div style='font-family:sans-serif;text-align:center;padding:20px'><h3>Saved</h3>"
             "<p>The clock restarts and joins the network.</p>"
             "<p>Connect your phone back to your normal WiFi and open<br><b>%s%s%s</b></p>"
             "<p style='color:#64748b;font-size:13px'>If it cannot join, the " WIFI_SETUP_SSID " hotspot comes back "
             "after a few minutes so you can try again.</p></div>",
             n.dhcp ? "the IP your router gave it" : "http://", n.dhcp ? "" : ipb, n.dhcp ? "" : "/");
    httpd_resp_sendstr(req, page);
    s_pendingNet = n;
    s_pendingSave = true;
    return ESP_OK;
}

static esp_err_t portal_cancel_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, "<p style='font-family:sans-serif'>Cancelled - the clock restarts with its old setting.</p>");
    s_pendingCancel = true;
    return ESP_OK;
}

static esp_err_t portal_404_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(s_ap_netif, &ip);
    char loc[32];
    snprintf(loc, sizeof(loc), "http://" IPSTR "/", IP2STR(&ip.ip));
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", loc);
    // iOS needs a body to detect the captive portal.
    httpd_resp_send(req, "Redirecting to the WiFi setup page", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

// Builds <option>s of networks in range, strongest first, each SSID once.
static void buildPortalOptions(void)
{
    const size_t cap = 2400;
    s_portalOptions = calloc(1, cap);
    if (!s_portalOptions) return;

    wifiStop();
    wifiStartSta();
    wifi_ap_record_t *recs = malloc(WIFI_SCAN_MAX * sizeof(wifi_ap_record_t));
    if (!recs) return;
    uint16_t num = WIFI_SCAN_MAX;
    if (esp_wifi_scan_start(NULL, true) == ESP_OK && esp_wifi_scan_get_ap_records(&num, recs) == ESP_OK) {
        // esp_wifi_scan_get_ap_records already returns strongest first.
        for (int j = 0; j < num; j++) {
            const char *s = (const char *)recs[j].ssid;
            if (!s[0] || strchr(s, '\'') || strchr(s, '<') || strchr(s, '&')) continue;
            char key[48];
            snprintf(key, sizeof(key), "value='%s'", s);
            if (strstr(s_portalOptions, key)) continue;
            char opt[128];
            snprintf(opt, sizeof(opt), "<option value='%s'>%s  (%d dBm)</option>", s, s, recs[j].rssi);
            strlcat(s_portalOptions, opt, cap);
        }
    }
    free(recs);
    wifiStop();
}

// Opens the setup hotspot. timeoutMs = 0: until a network is saved.
// Always ends in a reboot (save, cancel or timeout) — never returns.
static void runSetupPortal(uint32_t timeoutMs, const char *why)
{
    ESP_LOGW(TAG, "WiFi: setup hotspot " WIFI_SETUP_SSID " started (%s)", why);
    buildPortalOptions();

    s_ap_netif = esp_netif_create_default_wifi_ap();
    wifi_config_t ap = {
        .ap = {
            .ssid           = WIFI_SETUP_SSID,
            .ssid_len       = sizeof(WIFI_SETUP_SSID) - 1,
            .channel        = 1,
            .max_connection = 4,
            .authmode       = WIFI_AUTH_OPEN,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_ERROR_CHECK(esp_wifi_start());
    s_wifiStarted = true;

    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_open_sockets = 7;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 8192;
    if (httpd_start(&server, &cfg) == ESP_OK) {
        static const httpd_uri_t root   = { .uri = "/",       .method = HTTP_GET,  .handler = portal_get_handler };
        static const httpd_uri_t save   = { .uri = "/save",   .method = HTTP_POST, .handler = portal_save_handler };
        static const httpd_uri_t cancel = { .uri = "/cancel", .method = HTTP_POST, .handler = portal_cancel_handler };
        httpd_register_uri_handler(server, &root);
        httpd_register_uri_handler(server, &save);
        httpd_register_uri_handler(server, &cancel);
        httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, portal_404_handler);
    }
    dns_server_config_t dnsCfg = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    start_dns_server(&dnsCfg);

    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(s_ap_netif, &ip);
    ESP_LOGI(TAG, "WiFi: setup page at http://" IPSTR, IP2STR(&ip.ip));

    TickType_t start = xTaskGetTickCount();
    for (;;) {
        if (s_pendingSave) {
            vTaskDelay(pdMS_TO_TICKS(1500));   // let the "Saved" page reach the phone
            WifiNets_Add(&s_pendingNet);
            ESP_LOGI(TAG, "WiFi: setup added network %s", s_pendingNet.ssid);
            esp_restart();
        }
        if (s_pendingCancel) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }
        if (timeoutMs && (xTaskGetTickCount() - start) > pdMS_TO_TICKS(timeoutMs)) {
            ESP_LOGW(TAG, "WiFi: setup hotspot timed out - restarting to try the saved networks again");
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

// =============================================================================
// Runtime reconnect
// =============================================================================
// Installed once connected and never removed: without it the driver gives up
// after the first post-boot disconnect (e.g. an unanswered PMF SA-Query) and
// the clock stays offline until power-cycled.
static void runtimeDisconnectHandler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg; (void)base; (void)id; (void)data;
    ESP_LOGW(TAG, "WiFi: disconnected, reconnecting...");
    esp_wifi_connect();
}

// =============================================================================
// Public API
// =============================================================================

bool SetupWiFi(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t initCfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&initCfg));
    esp_wifi_set_storage(WIFI_STORAGE_RAM);   // our own file, not the NVS copy
    s_sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_sta_netif, WIFI_HOSTNAME);

    WifiNet probe[1];
    bool haveSaved = WifiNets_Load(probe, 1) > 0;
    if (!haveSaved && KNOWN_NET_COUNT == 0) runSetupPortal(0, "no WiFi network saved yet");

    for (int round = 0; round < WIFI_ROUNDS; round++) {
        if (round > 0) {
            ESP_LOGW(TAG, "WiFi: round %d failed - retry in %d s", round, WIFI_ROUND_GAP_MS / 1000);
            vTaskDelay(pdMS_TO_TICKS(WIFI_ROUND_GAP_MS));
        }
        if (joinSaved() || scanAndJoinKnown()) {
            esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                                &runtimeDisconnectHandler, NULL, NULL);
            return true;
        }
    }
    // Nothing joined for several minutes: open the hotspot for a while (maybe
    // the password changed), then reboot and try again.
    runSetupPortal(WIFI_PORTAL_MS, "no saved network reachable");
    return false;   // not reached
}

void WifiSetup_GetCurrentSsid(char *out, size_t outsz)
{
    out[0] = '\0';
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) strlcpy(out, (const char *)ap.ssid, outsz);
}

void WifiSetup_GetLocalIPString(char *out, size_t outsz)
{
    esp_netif_ip_info_t info = { 0 };
    if (s_sta_netif) esp_netif_get_ip_info(s_sta_netif, &info);
    snprintf(out, outsz, IPSTR, IP2STR(&info.ip));
}

void WifiSetup_GetGatewayIPString(char *out, size_t outsz)
{
    esp_netif_ip_info_t info = { 0 };
    if (s_sta_netif) esp_netif_get_ip_info(s_sta_netif, &info);
    snprintf(out, outsz, IPSTR, IP2STR(&info.gw));
}
