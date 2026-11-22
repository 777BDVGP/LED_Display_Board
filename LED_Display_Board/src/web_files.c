// =============================================================================
// web_files.c — SPIFFS File Manager Web Server
// =============================================================================
// See web_files.h. Ported from LED_Display_Board_ARD's 03_Files.cpp AJAX file
// manager: a JSON file listing (/api/files), download/view/delete by name,
// and a multipart upload endpoint (/fupload) — esp_http_server has no
// built-in multipart parser, so file_upload_handler() implements a small
// streaming one (single "file" field only, which is all the upload form
// sends).
// =============================================================================

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/param.h>
#include <sys/stat.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_spiffs.h"

#include "files.h"
#include "web_core.h"
#include "web_files.h"
#include "wifi_setup.h"

static const char *TAG = "web_files";

// =============================================================================
// File Manager Page  (GET /files)
// =============================================================================
// Now streamed through the shared WebCore shell (ShellOpen/Header/Nav/Script)
// like every other real page, instead of a standalone hardcoded HTML document
// — that standalone version had no header/nav bar and never included the
// shared stylesheet, so it also silently ignored the Light/Dark theme toggle.
// Page-specific bits not covered by CSS_STYLE (progress bar, view-panel pre
// box) get one small supplemental <style> block; everything else (.sec,
// .btn/.btn-bl/.btn-rd/.btn-sm, table/th/td) reuses the shared classes.
// =============================================================================

static const char FILES_EXTRA_STYLE[] =
"<style>"
".fs-bar-bg{background:#0f172a;border-radius:3px;height:8px;margin:6px 10px 2px;overflow:hidden}"
".fs-bar-fg{background:#38bdf8;height:100%}"
"body.light .fs-bar-bg{background:#e2e8f0}"
"#ftbl{padding:0 10px}"
"#vpanel pre{border:1px solid #334155;border-radius:4px;padding:10px;margin:0 10px 10px;"
"max-height:360px;overflow:auto;white-space:pre-wrap;word-break:break-all;font-size:11px;"
"background:#0f172a;color:#e2e8f0}"
"body.light #vpanel pre{background:#fff;border-color:#cbd5e1;color:#0f172a}"
"</style>";

static const char FILES_BODY[] =
"<div class='sec'>SPIFFS STORAGE</div>"
"<div style='padding:8px 10px 0;font-size:13px;color:#94a3b8' id='fsinfo'>Loading...</div>"
"<div class='fs-bar-bg'><div class='fs-bar-fg' id='fsbar' style='width:0%'></div></div>"
"<div class='sec'>FILES</div>"
"<div id='ftbl'>Loading...</div>"
"<div id='vpanel' style='display:none'>"
"<div class='sec' style='display:flex;justify-content:space-between;align-items:center;text-transform:none;letter-spacing:normal'>"
"<span id='vfname' style='font-family:monospace;font-size:11px'></span>"
"<button class='btn btn-rd btn-sm' onclick='closeView()'>Close</button></div>"
"<pre id='vcontent'></pre></div>"
"<div class='sec'>UPLOAD</div>"
"<div class='save-row'>"
"<form method='post' enctype='multipart/form-data' action='/fupload'>"
"<input type='file' name='fupload' style='margin-bottom:8px;display:block'>"
"<button class='btn btn-bl' type='submit'>Upload</button></form></div>";

static const char FILES_SCRIPT[] =
"<script>"
"var FL=[];"
"function fmtSz(b){if(b<1024)return b+' B';if(b<1048576)return (b/1024).toFixed(1)+' KB';return (b/1048576).toFixed(1)+' MB';}"
"function loadFiles(){fetch('/api/files').then(function(r){return r.json();}).then(function(j){"
"var pct=j.total?Math.round(j.used/j.total*100):0;"
"document.getElementById('fsinfo').innerHTML='Used '+fmtSz(j.used)+' of '+fmtSz(j.total)+' ('+pct+'%)';"
"document.getElementById('fsbar').style.width=pct+'%';"
"if(!j.files||j.files.length===0){document.getElementById('ftbl').innerHTML='<p style=\"padding:8px 0;color:#64748b\">No files.</p>';return;}"
"function fExt(n){var i=n.lastIndexOf('.');return i>=0?n.slice(i+1).toLowerCase():'';}"
"j.files.sort(function(a,b){"
"var ea=fExt(a.name),eb=fExt(b.name);"
"if(ea!==eb)return ea<eb?-1:1;"
"var na=a.name.toLowerCase(),nb=b.name.toLowerCase();"
"return na<nb?-1:(na>nb?1:0);"
"});"
"FL=j.files.map(function(f){return f.name;});"
"var h='<table><tr><th>Name</th><th>Size</th><th></th></tr>';"
"j.files.forEach(function(f,i){"
"h+='<tr><td style=\"font-family:monospace\">'+f.name+'</td>';"
"h+='<td style=\"color:#64748b;white-space:nowrap\">'+fmtSz(f.size)+'</td>';"
"h+='<td style=\"text-align:right;white-space:nowrap\">';"
"h+='<a href=\"/files/download?f='+encodeURIComponent(f.name)+'\" class=\"btn btn-bl btn-sm\">DL</a> ';"
"h+='<button class=\"btn btn-sm\" style=\"background:#334155;color:#fff\" onclick=\"viewFile('+i+')\">View</button> ';"
"h+='<button class=\"btn btn-rd btn-sm\" onclick=\"delFile('+i+')\">Del</button>';"
"h+='</td></tr>';});"
"h+='</table>';document.getElementById('ftbl').innerHTML=h;"
"}).catch(function(){document.getElementById('ftbl').innerHTML='<p>Load failed.</p>';});}"
"function delFile(i){var name=FL[i];if(!confirm('Delete '+name+'?'))return;"
"fetch('/files/delete?f='+encodeURIComponent(name)).then(function(r){return r.json();}).then(function(j){if(j.ok)loadFiles();else alert('Error: '+j.msg);});}"
"function viewFile(i){var name=FL[i];"
"document.getElementById('vcontent').textContent='Loading...';"
"document.getElementById('vfname').textContent=name;"
"document.getElementById('vpanel').style.display='block';"
"fetch('/files/view?f='+encodeURIComponent(name)).then(function(r){return r.text();}).then(function(t){document.getElementById('vcontent').textContent=t;}).catch(function(){document.getElementById('vcontent').textContent='(error)';});}"
"function closeView(){document.getElementById('vpanel').style.display='none';}"
"loadFiles();"
"</script>";

static esp_err_t files_page_handler(httpd_req_t *req)
{
    esp_err_t err;
    if ((err = WebCore_SendShellOpen(req, "LED Clock - Files")) != ESP_OK) return err;
    if ((err = WebCore_SendHeader(req, FA_MESS_07)) != ESP_OK) return err;
    if ((err = WebCore_SendNav(req, PS_OTHER)) != ESP_OK) return err;
    if ((err = WebCore_SendScript(req)) != ESP_OK) return err;
    if ((err = httpd_resp_send_chunk(req, FILES_EXTRA_STYLE, sizeof(FILES_EXTRA_STYLE) - 1)) != ESP_OK) return err;
    if ((err = httpd_resp_send_chunk(req, FILES_BODY, sizeof(FILES_BODY) - 1)) != ESP_OK) return err;
    if ((err = httpd_resp_send_chunk(req, FILES_SCRIPT, sizeof(FILES_SCRIPT) - 1)) != ESP_OK) return err;
    return WebCore_SendShellClose(req);
}

// =============================================================================
// AJAX File-Manager Endpoints
// =============================================================================

static bool getFilenameArg(httpd_req_t *req, char *name, size_t nameSize)
{
    char query[160];
    name[0] = '\0';
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        return false;
    }
    return httpd_query_key_value(query, "f", name, nameSize) == ESP_OK && name[0] != '\0';
}

static esp_err_t api_files_handler(httpd_req_t *req)
{
    size_t total = 0, used = 0;
    esp_spiffs_info(NULL, &total, &used);

    const int cap = 4096;
    char *json = malloc(cap);
    if (!json) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    int pos = snprintf(json, cap, "{\"used\":%u,\"total\":%u,\"files\":[", (unsigned)used, (unsigned)total);

    DIR *dir = opendir(SPIFFS_BASE_PATH);
    bool first = true;
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (pos > cap - 128) break; // guard against overflow on a huge directory
            if (WifiSetup_IsProtectedFile(ent->d_name)) continue; // holds WiFi passwords — never listed
            char path[sizeof(SPIFFS_BASE_PATH) + sizeof(ent->d_name) + 1]; // d_name is char[256] — GCC can't assume SPIFFS's real (much shorter) name limit
            snprintf(path, sizeof(path), "%s/%s", SPIFFS_BASE_PATH, ent->d_name);
            struct stat st;
            if (stat(path, &st) != 0) continue;
            pos += snprintf(json + pos, cap - pos, "%s{\"name\":\"%s\",\"size\":%ld}",
                             first ? "" : ",", ent->d_name, (long)st.st_size);
            first = false;
        }
        closedir(dir);
    }
    pos += snprintf(json + pos, cap - pos, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, pos);
    free(json);
    return ESP_OK;
}

static esp_err_t file_download_handler(httpd_req_t *req)
{
    char name[64];
    if (!getFilenameArg(req, name, sizeof(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing filename");
        return ESP_FAIL;
    }
    if (WifiSetup_IsProtectedFile(name)) { // WiFi passwords: never viewed, downloaded or deleted here
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Protected file");
        return ESP_FAIL;
    }

    char path[96];
    snprintf(path, sizeof(path), "%s/%s", SPIFFS_BASE_PATH, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_FAIL;
    }

    char disp[100];
    snprintf(disp, sizeof(disp), "attachment; filename=%s", name);
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    httpd_resp_set_type(req, "application/octet-stream");

    char buf[512];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, r) != ESP_OK) {
            fclose(f);
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t file_view_handler(httpd_req_t *req)
{
    char name[64];
    if (!getFilenameArg(req, name, sizeof(name))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing filename");
        return ESP_FAIL;
    }
    if (WifiSetup_IsProtectedFile(name)) { // WiFi passwords: never viewed, downloaded or deleted here
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Protected file");
        return ESP_FAIL;
    }

    char path[96];
    snprintf(path, sizeof(path), "%s/%s", SPIFFS_BASE_PATH, name);
    FILE *f = fopen(path, "rb");
    if (!f) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "text/plain");
    char buf[512];
    size_t r;
    while ((r = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, r) != ESP_OK) {
            fclose(f);
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t file_delete_handler(httpd_req_t *req)
{
    char name[64];
    httpd_resp_set_type(req, "application/json");
    if (!getFilenameArg(req, name, sizeof(name))) {
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"missing filename\"}");
        return ESP_OK;
    }
    if (WifiSetup_IsProtectedFile(name)) { // WiFi passwords: never viewed, downloaded or deleted here
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Protected file");
        return ESP_FAIL;
    }

    char path[96];
    snprintf(path, sizeof(path), "%s/%s", SPIFFS_BASE_PATH, name);
    bool ok = remove(path) == 0;
    httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false,\"msg\":\"delete failed\"}");
    return ESP_OK;
}

// -----------------------------------------------------------------------------
// findMarker
// Naive byte-string search — a small local substitute for memmem() (not
// guaranteed present in ESP-IDF's newlib), used to locate the multipart
// boundary while streaming an upload to file.
// -----------------------------------------------------------------------------
static uint8_t *findMarker(uint8_t *hay, int hayLen, const char *needle, int needleLen)
{
    if (hayLen < needleLen) return NULL;
    for (int i = 0; i <= hayLen - needleLen; i++) {
        if (memcmp(hay + i, needle, needleLen) == 0) {
            return hay + i;
        }
    }
    return NULL;
}

// -----------------------------------------------------------------------------
// file_upload_handler  (POST /fupload — multipart/form-data)
// esp_http_server has no built-in multipart parser. This streams the request
// body in fixed-size chunks, holding back only enough trailing bytes to
// detect a boundary marker split across two reads, and writes everything
// else straight to the destination file. Only the single "file" field the
// upload form sends is supported (this is not a general MIME parser).
// -----------------------------------------------------------------------------
static esp_err_t file_upload_handler(httpd_req_t *req)
{
    char contentType[160];
    if (httpd_req_get_hdr_value_str(req, "Content-Type", contentType, sizeof(contentType)) != ESP_OK ||
        !strstr(contentType, "multipart/form-data")) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Expected multipart/form-data");
        return ESP_FAIL;
    }
    char *b = strstr(contentType, "boundary=");
    if (!b) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing boundary");
        return ESP_FAIL;
    }
    b += strlen("boundary=");

    char marker[160];
    int markerLen = snprintf(marker, sizeof(marker), "\r\n--%s", b);

    const int bufCap = 1536;
    uint8_t *buf = malloc(bufCap);
    if (!buf) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    int remaining = (int)req->content_len;
    int filled    = 0;

    // Read until we've seen the part's header terminator, extracting filename.
    bool haveHeader = false;
    char filename[64] = "upload.bin";
    int dataOffset = 0;

    while (!haveHeader && remaining > 0 && filled < bufCap - 1) {
        int r = httpd_req_recv(req, (char *)buf + filled, MIN(remaining, bufCap - 1 - filled));
        if (r <= 0) {
            free(buf);
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        filled += r;
        remaining -= r;
        buf[filled] = '\0';

        char *hdrEnd = strstr((char *)buf, "\r\n\r\n");
        if (hdrEnd) {
            haveHeader = true;
            char *fnameStart = strstr((char *)buf, "filename=\"");
            if (fnameStart && fnameStart < hdrEnd) {
                fnameStart += strlen("filename=\"");
                char *fnameEnd = strchr(fnameStart, '"');
                if (fnameEnd && fnameEnd < hdrEnd) {
                    size_t n = (size_t)(fnameEnd - fnameStart);
                    if (n >= sizeof(filename)) n = sizeof(filename) - 1;
                    memcpy(filename, fnameStart, n);
                    filename[n] = '\0';
                }
            }
            dataOffset = (int)((hdrEnd + 4) - (char *)buf);
        }
    }

    if (!haveHeader || filename[0] == '\0') {
        free(buf);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Malformed multipart body");
        return ESP_FAIL;
    }

    char path[96];
    snprintf(path, sizeof(path), "%s/%s", SPIFFS_BASE_PATH, filename);
    remove(path); // remove any previous version rather than appending to it
    FILE *f = fopen(path, "wb");
    if (!f) {
        free(buf);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // Shift the file-data bytes already read (past the part header) to the front.
    filled -= dataOffset;
    memmove(buf, buf + dataOffset, filled);

    bool ok = true;
    while (true) {
        uint8_t *found = findMarker(buf, filled, marker, markerLen);
        if (found) {
            int dataEnd = (int)(found - buf);
            if (dataEnd > 0) fwrite(buf, 1, dataEnd, f);
            break;
        }

        // No marker yet — flush everything except a tail long enough to still
        // contain a split marker, then read more.
        int safe = filled - (markerLen - 1);
        if (safe > 0) {
            fwrite(buf, 1, safe, f);
            memmove(buf, buf + safe, filled - safe);
            filled -= safe;
        }

        if (remaining <= 0) {
            ok = false; // ran out of body before finding the closing boundary
            break;
        }
        int r = httpd_req_recv(req, (char *)buf + filled, MIN(remaining, bufCap - filled));
        if (r <= 0) {
            ok = false;
            break;
        }
        filled += r;
        remaining -= r;
    }

    fclose(f);
    free(buf);

    if (!ok) {
        remove(path);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload truncated");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Uploaded %s", filename);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_sendstr(req, "<script>location.href='/files';</script>");
    return ESP_OK;
}

// =============================================================================
// Disk Tools Page  (GET /disktools)
// =============================================================================
// A link hub to the type-filtered CSV export/import routes already
// implemented in web_events.c / web_exercises.c. Uses the shared WebCore
// shell (same as ARD's DiskToolsPage, which streams through
// sendWebStyleAndBody() rather than the standalone /files page's own markup).
// -----------------------------------------------------------------------------

static esp_err_t disktools_handler(httpd_req_t *req)
{
    esp_err_t err;
    if ((err = WebCore_SendShellOpen(req, "LED Clock - Disk Tools")) != ESP_OK) return err;
    if ((err = WebCore_SendHeader(req, FA_MESS_06)) != ESP_OK) return err;
    if ((err = WebCore_SendNav(req, PS_OTHER)) != ESP_OK) return err;
    if ((err = WebCore_SendScript(req)) != ESP_OK) return err;

    static const char body[] =
        "<div class='sec'>EXPORT TO CSV</div>"
        "<div class='save-row'>"
        "<button class='btn btn-bl btn-sm' onclick=\"location.href='/createCSVBirthday'\">&#8659; BIRTHDAYS</button>"
        "<button class='btn btn-bl btn-sm' onclick=\"location.href='/createCSVAnniversary'\">&#8659; ANNIVERSARYS</button>"
        "<button class='btn btn-bl btn-sm' onclick=\"location.href='/createCSVReminder'\">&#8659; REMINDERS</button>"
        "<button class='btn btn-bl btn-sm' onclick=\"location.href='/createCSVExercises'\">&#8659; EXERCISES</button>"
        "<button class='btn btn-bl btn-sm' onclick=\"location.href='/createCSVValidPeriods'\">&#8659; VALID PERIODS</button>"
        "</div>"
        "<div class='sec'>IMPORT FROM CSV</div>"
        "<div class='save-row'>"
        "<button class='btn btn-rd btn-sm' onclick=\"location.href='/loadFromCSVBirthday'\">&#8657; BIRTHDAYS</button>"
        "<button class='btn btn-rd btn-sm' onclick=\"location.href='/loadFromCSVAnniversary'\">&#8657; ANNIVERSARYS</button>"
        "<button class='btn btn-rd btn-sm' onclick=\"location.href='/loadFromCSVReminder'\">&#8657; REMINDERS</button>"
        "<button class='btn btn-rd btn-sm' onclick=\"location.href='/loadFromCSVExercises'\">&#8657; EXERCISES</button>"
        "<button class='btn btn-rd btn-sm' onclick=\"location.href='/loadFromCSVValidPeriods'\">&#8657; VALID PERIODS</button>"
        "</div>"
        "<div style='font-size:12px;opacity:.75;margin:8px 0'>"
        "Import replaces all entries of that kind on this clock, then copies the same set to the other clocks "
        "(Exercises are not copied). Download the CSV via FILES to keep a backup on your PC.</div>";
    if ((err = httpd_resp_send_chunk(req, body, sizeof(body) - 1)) != ESP_OK) return err;

    return WebCore_SendShellClose(req);
}

// =============================================================================
// Public API
// =============================================================================

// Registers file-manager routes on the shared server (see web_server.h).
// "/" is no longer registered here — web_home.c's real Home page owns it now
// that Phase C exists; the file manager keeps just "/files" and its AJAX API.
void FileWebPage_RegisterRoutes(httpd_handle_t server)
{
    static const httpd_uri_t files_uri     = { .uri = "/files",          .method = HTTP_GET,  .handler = files_page_handler };
    static const httpd_uri_t api_uri       = { .uri = "/api/files",      .method = HTTP_GET,  .handler = api_files_handler };
    static const httpd_uri_t dl_uri        = { .uri = "/files/download", .method = HTTP_GET,  .handler = file_download_handler };
    static const httpd_uri_t view_uri      = { .uri = "/files/view",     .method = HTTP_GET,  .handler = file_view_handler };
    static const httpd_uri_t del_uri       = { .uri = "/files/delete",   .method = HTTP_GET,  .handler = file_delete_handler };
    static const httpd_uri_t upload_uri    = { .uri = "/fupload",        .method = HTTP_POST, .handler = file_upload_handler };
    static const httpd_uri_t disktools_uri = { .uri = "/disktools",      .method = HTTP_GET,  .handler = disktools_handler };

    httpd_register_uri_handler(server, &disktools_uri);
    httpd_register_uri_handler(server, &files_uri);
    httpd_register_uri_handler(server, &api_uri);
    httpd_register_uri_handler(server, &dl_uri);
    httpd_register_uri_handler(server, &view_uri);
    httpd_register_uri_handler(server, &del_uri);
    httpd_register_uri_handler(server, &upload_uri);

    ESP_LOGI(TAG, "File web routes registered");
}
