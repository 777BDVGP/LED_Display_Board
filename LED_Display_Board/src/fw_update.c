// =============================================================================
// fw_update.c — Firmware update from GitHub Pages / browser upload
// =============================================================================
// See fw_update.h. The HTTPS work runs in its own short-lived task so the web
// server stays responsive; the ABOUT page polls GET /fwstatus for progress.
// =============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"

#include "fw_update.h"

static const char *TAG = "fw_update";

static char          s_status[160] = "";
static char          s_latest[16]  = "";
static char          s_binPath[64] = "";
static volatile bool s_busy        = false;

static void setStatus(const char *msg)
{
    strlcpy(s_status, msg, sizeof(s_status));
    ESP_LOGI(TAG, "%s", msg);
}

// Compares "a.b.c" versions numerically. <0: a older, 0: same, >0: a newer.
static int versionCompare(const char *a, const char *b)
{
    for (int i = 0; i < 4; i++) {
        long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
        if (x != y) return x < y ? -1 : 1;
        if (*a == '.') a++;
        if (*b == '.') b++;
        if (!*a && !*b) break;
    }
    return 0;
}

// =============================================================================
// CHECK FOR UPDATE
// =============================================================================

static void doCheck(void)
{
    setStatus("Checking GitHub...");
    esp_http_client_config_t cfg = {
        .url               = FW_UPDATE_URL "version.json",
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 15000,
        .user_agent        = "LEDClock/" FW_VERSION,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { setStatus("Check failed (no memory)"); return; }

    char body[512];
    int len = -1, status = 0;
    if (esp_http_client_open(c, 0) == ESP_OK) {
        esp_http_client_fetch_headers(c);
        status = esp_http_client_get_status_code(c);
        len = esp_http_client_read_response(c, body, sizeof(body) - 1);
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);

    if (len <= 0 || status != 200) {
        char m[96];
        snprintf(m, sizeof(m), "Check failed (no answer from GitHub, HTTP %d) - is the clock online?", status);
        setStatus(m);
        return;
    }
    body[len] = '\0';
    cJSON *j = cJSON_Parse(body);
    const cJSON *ver = j ? cJSON_GetObjectItemCaseSensitive(j, "version") : NULL;
    const cJSON *bin = j ? cJSON_GetObjectItemCaseSensitive(j, "bin") : NULL;
    const cJSON *notes = j ? cJSON_GetObjectItemCaseSensitive(j, "notes") : NULL;
    if (!cJSON_IsString(ver) || !cJSON_IsString(bin)) {
        cJSON_Delete(j);
        setStatus("Check failed (version.json not readable)");
        return;
    }
    strlcpy(s_latest, ver->valuestring, sizeof(s_latest));
    strlcpy(s_binPath, bin->valuestring, sizeof(s_binPath));

    char m[160];
    int cmp = versionCompare(s_latest, FW_VERSION);
    if (cmp > 0)
        snprintf(m, sizeof(m), "Version %s is available%s%.80s", s_latest,
                 cJSON_IsString(notes) && notes->valuestring[0] ? ": " : "",
                 cJSON_IsString(notes) ? notes->valuestring : "");
    else if (cmp == 0)
        snprintf(m, sizeof(m), "Up to date (%s)", FW_VERSION);
    else
        snprintf(m, sizeof(m), "This clock (" FW_VERSION ") is newer than GitHub (%s)", s_latest);
    cJSON_Delete(j);
    setStatus(m);
}

// =============================================================================
// INSTALL FROM GITHUB
// =============================================================================

static void doInstall(void)
{
    if (!s_binPath[0]) { setStatus("Press CHECK FOR UPDATE first"); return; }

    char url[160];
    snprintf(url, sizeof(url), FW_UPDATE_URL "%s", s_binPath);
    char m[160];
    snprintf(m, sizeof(m), "Downloading %s ... (about a minute, the clock restarts by itself)", s_latest);
    setStatus(m);

    esp_http_client_config_t http = {
        .url               = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms        = 20000,
        .keep_alive_enable = true,
        .user_agent        = "LEDClock/" FW_VERSION,
    };
    esp_https_ota_config_t ota = { .http_config = &http };
    esp_err_t err = esp_https_ota(&ota);
    if (err == ESP_OK) {
        snprintf(m, sizeof(m), "Installed %s - restarting...", s_latest);
        setStatus(m);
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    }
    snprintf(m, sizeof(m), "Install failed (%s) - the old firmware is still running", esp_err_to_name(err));
    setStatus(m);
}

// =============================================================================
// Worker task + routes
// =============================================================================

typedef enum { FW_JOB_CHECK, FW_JOB_INSTALL } FwJob;

static void fwTask(void *arg)
{
    FwJob job = (FwJob)(intptr_t)arg;
    if (job == FW_JOB_CHECK) doCheck();
    else doInstall();
    s_busy = false;
    vTaskDelete(NULL);
}

static esp_err_t startJob(httpd_req_t *req, FwJob job)
{
    httpd_resp_set_type(req, "text/plain");
    if (s_busy) return httpd_resp_sendstr(req, "Busy");
    s_busy = true;
    if (xTaskCreate(fwTask, "fw_update", 8192, (void *)(intptr_t)job, 2, NULL) != pdPASS) {
        s_busy = false;
        setStatus("Could not start (no memory)");
    }
    return httpd_resp_sendstr(req, "OK");
}

static esp_err_t fwcheck_handler(httpd_req_t *req)   { return startJob(req, FW_JOB_CHECK); }
static esp_err_t fwinstall_handler(httpd_req_t *req) { return startJob(req, FW_JOB_INSTALL); }

static esp_err_t fwstatus_handler(httpd_req_t *req)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "installed", FW_VERSION);
    cJSON_AddStringToObject(j, "latest", s_latest);
    cJSON_AddStringToObject(j, "status", s_status);
    cJSON_AddBoolToObject(j, "busy", s_busy);
    char *txt = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_sendstr(req, txt ? txt : "{}");
    free(txt);
    return r;
}

// POST /fwupload — raw .bin as the request body (not multipart).
static esp_err_t fwupload_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    if (s_busy) return httpd_resp_sendstr(req, "Busy - an update is already running");
    int total = req->content_len;
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (total <= 0 || !part || (size_t)total > part->size)
        return httpd_resp_sendstr(req, "Not a firmware file (wrong size)");

    s_busy = true;
    setStatus("Receiving uploaded firmware...");
    esp_ota_handle_t h;
    esp_err_t err = esp_ota_begin(part, OTA_SIZE_UNKNOWN, &h);
    if (err != ESP_OK) { s_busy = false; setStatus("Upload failed (could not start)"); return httpd_resp_sendstr(req, s_status); }

    char *buf = malloc(4096);
    int got = 0;
    bool first = true, ok = buf != NULL;
    while (ok && got < total) {
        int r = httpd_req_recv(req, buf, (total - got) < 4096 ? (total - got) : 4096);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) { ok = false; break; }
        if (first && (unsigned char)buf[0] != 0xE9) { ok = false; break; }   // ESP image magic
        first = false;
        if (esp_ota_write(h, buf, r) != ESP_OK) { ok = false; break; }
        got += r;
    }
    free(buf);

    if (!ok) {
        esp_ota_abort(h);
        s_busy = false;
        setStatus("Upload failed - not a valid firmware file; the old firmware is still running");
        return httpd_resp_sendstr(req, s_status);
    }
    if (esp_ota_end(h) != ESP_OK || esp_ota_set_boot_partition(part) != ESP_OK) {
        s_busy = false;
        setStatus("Upload failed - the file did not verify; the old firmware is still running");
        return httpd_resp_sendstr(req, s_status);
    }
    setStatus("Uploaded firmware installed - restarting...");
    httpd_resp_sendstr(req, "OK - installed, the clock restarts now");
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return ESP_OK;
}

void FwUpdate_RegisterRoutes(httpd_handle_t server)
{
    static const httpd_uri_t check   = { .uri = "/fwcheck",   .method = HTTP_POST, .handler = fwcheck_handler };
    static const httpd_uri_t install = { .uri = "/fwinstall", .method = HTTP_POST, .handler = fwinstall_handler };
    static const httpd_uri_t status  = { .uri = "/fwstatus",  .method = HTTP_GET,  .handler = fwstatus_handler };
    static const httpd_uri_t upload  = { .uri = "/fwupload",  .method = HTTP_POST, .handler = fwupload_handler };
    httpd_register_uri_handler(server, &check);
    httpd_register_uri_handler(server, &install);
    httpd_register_uri_handler(server, &status);
    httpd_register_uri_handler(server, &upload);
}

// =============================================================================
// ABOUT page section
// =============================================================================

static const char ABOUT_FW[] =
    "<div class='sec'>FIRMWARE</div><div class='ibox'>"
    "<div class='irow'><dt>Installed</dt><dd>" FW_VERSION "</dd></div>"
    "<div class='irow'><dt>On GitHub</dt><dd id='fw_latest'>&ndash;</dd></div>"
    "<div class='irow'><dt>Status</dt><dd id='fw_status' style='font-size:12px;text-align:right'>&ndash;</dd></div>"
    "</div>"
    "<div class='save-row'>"
    "<button class='btn btn-bl' onclick='fwGo(\"/fwcheck\")'>CHECK FOR UPDATE</button>"
    "<button class='btn btn-gn' onclick=\"if(confirm('Install the version from GitHub?\\n\\nThe clock downloads it (about a minute) and restarts.\\nSettings, reminders and birthdays are kept.'))fwGo('/fwinstall')\">INSTALL FROM GITHUB</button>"
    "</div>"
    "<div class='gr'><span class='lbl'>No internet at the clock? Choose a firmware .bin</span>"
    "<span><input type='file' id='fw_file' accept='.bin' style='max-width:180px'>"
    "<button class='btn btn-bl btn-sm' onclick='fwUp()'>UPLOAD &amp; INSTALL</button></span></div>"
    "<script>"
    "var fwT=null;"
    "function fwPoll(){fetch('/fwstatus').then(function(r){return r.json();}).then(function(j){"
    "document.getElementById('fw_latest').textContent=j.latest||'\\u2013';"
    "document.getElementById('fw_status').textContent=j.status||'\\u2013';"
    "if(j.busy){fwT=setTimeout(fwPoll,1500);}}).catch(function(){fwT=setTimeout(fwPoll,3000);});}"
    "function fwGo(u){fetch(u,{method:'POST'}).then(function(){clearTimeout(fwT);setTimeout(fwPoll,500);});}"
    "function fwUp(){var f=document.getElementById('fw_file').files[0];if(!f){alert('Choose a .bin file first');return;}"
    "if(!confirm('Install '+f.name+'?\\n\\nThe clock restarts afterwards. Settings are kept.'))return;"
    "document.getElementById('fw_status').textContent='Uploading '+f.name+' ...';"
    "fetch('/fwupload',{method:'POST',body:f}).then(function(r){return r.text();})"
    ".then(function(t){document.getElementById('fw_status').textContent=t;})"
    ".catch(function(){document.getElementById('fw_status').textContent='Restarting...';});}"
    "fwPoll();"
    "</script>";

esp_err_t FwUpdate_SendAboutSection(httpd_req_t *req)
{
    return httpd_resp_send_chunk(req, ABOUT_FW, sizeof(ABOUT_FW) - 1);
}
