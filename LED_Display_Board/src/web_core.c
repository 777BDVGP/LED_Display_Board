// =============================================================================
// web_core.c — Shared Page Shell, CSS, Nav, Header Banner, WebSocket JS
// =============================================================================
// See web_core.h. Ported from LED_Display_Board_ARD's 04_WebPages.cpp
// (web_Style / webSocketScript / sendWebStyleAndBody / append_FileOptionToBuffer
// / webUpateButton[_NAV] / webHTMLEnd). The embedded JS connects to the
// same-origin "/ws" instead of ARD's separate ws://host:81/ WebSocketsServer.
// =============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "datetime.h"
#include "files.h"
#include "settings_store.h"
#include "web_core.h"

static const char *TAG = "web_core";

bool g_themeLight = false;

static int  s_pendingMessCode = 0;
static char s_pendingMessText[80] = "";

// =============================================================================
// Theme
// =============================================================================

#define THEME_PATH SPIFFS_BASE_PATH "/Theme.json"

void WebCore_LoadTheme(void)
{
    FILE *f = fopen(THEME_PATH, "r");
    if (!f) return;
    char buf[64];
    size_t rd = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[rd] = '\0';

    cJSON *root = cJSON_ParseWithLength(buf, rd);
    if (root) {
        cJSON *light = cJSON_GetObjectItemCaseSensitive(root, "light");
        if (light) g_themeLight = cJSON_IsTrue(light);
        cJSON_Delete(root);
    }
}

static void saveTheme(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "light", g_themeLight);
    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (out) {
        FILE *f = fopen(THEME_PATH, "w");
        if (f) {
            fputs(out, f);
            fclose(f);
        }
        free(out);
    }
}

static esp_err_t settheme_handler(httpd_req_t *req)
{
    char query[16];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char v[4];
        if (httpd_query_key_value(query, "v", v, sizeof(v)) == ESP_OK) {
            g_themeLight = (strcmp(v, "1") == 0);
        }
    }
    saveTheme();

    char referer[128];
    strlcpy(referer, "/settings", sizeof(referer));
    size_t hdrLen = httpd_req_get_hdr_value_len(req, "Referer");
    if (hdrLen > 0 && hdrLen < sizeof(referer)) {
        httpd_req_get_hdr_value_str(req, "Referer", referer, sizeof(referer));
    }

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", referer);
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

void WebCore_RegisterRoutes(httpd_handle_t server)
{
    static const httpd_uri_t theme_uri = {
        .uri = "/settheme", .method = HTTP_GET, .handler = settheme_handler,
    };
    httpd_register_uri_handler(server, &theme_uri);
}

// =============================================================================
// CSS
// =============================================================================

// Byte-faithful port of ARD's web_Style (04_WebPages.cpp) ruleset — a hand-
// approximated version of this caused a real visual regression (centered
// header lost, nav/button palette wrong) until this was replaced with the
// literal source values.
static const char CSS_STYLE[] =
"<style>"
"*{box-sizing:border-box;margin:0;padding:0}"
"body{background:#0f172a;color:#e2e8f0;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;max-width:700px;margin:0 auto;padding-bottom:24px}"
"a{text-decoration:none;color:inherit}"
".ph{text-align:center;padding:14px 8px 2px}"
".ph h1{font-size:22px;font-weight:800;color:#38bdf8;letter-spacing:1px}"
".ph .dt{font-size:12px;color:#94a3b8;margin-top:3px}"
".ph .msg{font-size:13px;font-weight:600;color:#22d3ee;margin-top:4px;padding-bottom:6px}"
".nav{display:flex;flex-wrap:wrap;margin:8px 8px 10px;border:1px solid #38bdf8;border-radius:6px;overflow:hidden}"
".nav a{flex:1 1 auto;text-align:center;padding:9px 4px;font-size:13px;font-weight:700;color:#0f172a;background:#38bdf8;cursor:pointer;border-right:1px solid #0f172a;transition:background .15s;white-space:nowrap;letter-spacing:.3px}"
".nav a:last-child{border-right:none}"
".nav a.act{background:#1e293b;color:#38bdf8}"
".nav a:hover:not(.act){background:#7dd3fc}"
".sec{background:#1e293b;color:#94a3b8;font-size:11px;font-weight:700;text-transform:uppercase;letter-spacing:.6px;padding:10px 10px 6px;margin-top:8px;border-top:1px solid #334155}"
".sub{background:#172032;color:#64748b;font-size:10px;font-weight:700;text-transform:uppercase;letter-spacing:.5px;padding:7px 10px 4px;border-top:1px solid #1e293b}"
".gr{display:flex;align-items:center;justify-content:space-between;padding:8px 10px;border-bottom:1px solid #1e293b;font-size:13px}"
".gr:hover{background:#1a2744}"
".gr .lbl{color:#94a3b8;font-weight:600;font-size:12px;flex:1;padding-right:8px}"
"input[type=text],input[type=number],input[type=time],input[type=date]{background:#0f172a;border:1px solid #334155;border-radius:4px;color:#e2e8f0;padding:5px 8px;font-size:13px;width:160px}"
"input[type=text]:focus,input[type=number]:focus,input[type=time]:focus,input[type=date]:focus{outline:none;border-color:#38bdf8}"
"input[type=checkbox]{width:17px;height:17px;cursor:pointer;accent-color:#38bdf8}"
"select{background:#0f172a;border:1px solid #334155;border-radius:4px;color:#e2e8f0;padding:5px 8px;font-size:13px;cursor:pointer}"
"select:focus{outline:none;border-color:#38bdf8}"
".save-row{padding:12px 10px}"
".btn{padding:9px 24px;border-radius:5px;font-size:13px;font-weight:700;border:none;cursor:pointer;transition:background .15s}"
".btn-bl{background:#1d4ed8;color:#fff}.btn-bl:hover{background:#2563eb}"
".btn-rd{background:#dc2626;color:#fff}.btn-rd:hover{background:#ef4444}"
".btn-gn{background:#16a34a;color:#fff}.btn-gn:hover{background:#22c55e}"
".btn-sm{padding:5px 14px;font-size:12px}"
"table{width:100%;border-collapse:collapse;font-size:13px;margin:0}"
"td,th{padding:6px 10px;border-bottom:1px solid #1e293b}"
"tr:hover td{background:#1a2744}"
"th{color:#64748b;font-size:11px;font-weight:700;text-transform:uppercase;letter-spacing:.5px;border-bottom:2px solid #334155;text-align:left}"
"input[type=button]{background:#38bdf8;border:none;color:#0f172a;padding:2px 14px;font-size:13px;font-weight:700;cursor:pointer;border-radius:4px;margin:8px 4px;height:28px}"
"input[type=button]:hover{background:#7dd3fc}"
".ibox{padding:0 10px}"
".irow{display:flex;justify-content:space-between;padding:8px 0;border-bottom:1px solid #1e293b;font-size:13px}"
".irow dt{color:#64748b;font-weight:600}"
".irow dd{color:#e2e8f0;text-align:right}"
".tag{display:inline-block;padding:1px 7px;border-radius:4px;font-size:11px;font-weight:600}"
".tok{background:#14532d;color:#86efac}"
"body.light{background:#f8fafc;color:#0f172a}"
"body.light .ph h1{color:#0284c7}"
"body.light .ph .dt{color:#475569}"
"body.light .ph .msg{color:#0369a1}"
"body.light .nav a{color:#0f172a}"
"body.light .nav a.act{background:#dbeafe;color:#0284c7}"
"body.light .sec{background:#f1f5f9;color:#475569;border-top-color:#e2e8f0}"
"body.light .sub{background:#e8eef5;color:#64748b;border-top-color:#e2e8f0}"
"body.light .gr{border-bottom-color:#e2e8f0}"
"body.light .gr:hover{background:#f1f5f9}"
"body.light .gr .lbl{color:#475569}"
"body.light input[type=text],body.light input[type=number],body.light input[type=time],body.light input[type=date]{background:#fff;border-color:#cbd5e1;color:#0f172a}"
"body.light select{background:#fff;border-color:#cbd5e1;color:#0f172a}"
"body.light td,body.light th{border-bottom-color:#e2e8f0}"
"body.light tr:hover td{background:#f1f5f9}"
"body.light th{color:#475569;border-bottom-color:#cbd5e1}"
"body.light input[type=button]{background:#0284c7;color:#fff}"
"body.light input[type=button]:hover{background:#0369a1}"
"body.light .irow{border-bottom-color:#e2e8f0}"
"body.light .irow dt{color:#475569}"
"body.light .irow dd{color:#0f172a}"
"</style>";

// =============================================================================
// Embedded WebSocket + UpdatePage() JS
// =============================================================================

static const char SCRIPT_JS[] =
"<script>"
"var Socket;var ElementAtt;"
"function StartWebSocket(){"
"Socket=new WebSocket('ws://'+window.location.hostname+(window.location.port?':'+window.location.port:'')+'/ws');"
"Socket.onmessage=function(event){"
"var jsonObject=JSON.parse(event.data);"
"if(jsonObject.type==='ota_start'){setOtaStatus('Upload starting...','orange');return;}"
"if(jsonObject.type==='ota_progress'){setOtaStatus('Uploading... '+jsonObject.percent+'%','orange');return;}"
"if(jsonObject.type==='ota_end'){setOtaStatus('Busy rebooting...','red');"
"var poll=setInterval(function(){fetch(location.href,{method:'HEAD',cache:'no-store'}).then(function(r){if(r.ok){clearInterval(poll);location.reload();}}).catch(function(){});},2000);return;}"
"if(jsonObject.type==='ota_error'){setOtaStatus('Update failed!','red');return;}"
"if(jsonObject.type==='ota_status'){return;}"
"if(jsonObject.type==='reload_home'){if(window.location.pathname==='/')window.location.reload();return;}"
"for(var key in jsonObject){"
"if(!jsonObject.hasOwnProperty(key))continue;"
"var value=jsonObject[key];"
"if(key==='Attribute'){if(ElementAtt)ElementAtt.setAttribute('style',value);continue;}"
"var el=document.getElementById(key);"
"if(!el)continue;"
"if(el.tagName==='SELECT'){el.value=value;}"
"else if(el.type==='checkbox'){el.checked=(value==='true');}"
"else{el.value=value;el.innerHTML=value;}"
"ElementAtt=el;"
"}"
"};"
"Socket.onerror=function(){setTimeout(StartWebSocket,300);};"
"Socket.onclose=function(){setTimeout(StartWebSocket,2000);};"
"}"
"function UpdatePage(){"
"var jsonObject={};"
"var inputs=document.querySelectorAll('input');"
"for(var i=0;i<inputs.length;i++){var input=inputs[i];"
"if(input.type==='checkbox')jsonObject[input.id]=input.checked?'true':'false';"
"else jsonObject[input.id]=input.value;}"
"var selects=document.querySelectorAll('select');"
"for(var i=0;i<selects.length;i++){jsonObject[selects[i].id]=selects[i].value;}"
"Socket.send(JSON.stringify(jsonObject));"
"}"
"function ButtonReboot(){Socket.send(JSON.stringify({'REBOOT':'YES'}));}"
"function setOtaStatus(msg,color){var el=document.getElementById('ota_status');if(el){el.value=msg;el.style.color=color;}}"
"window.addEventListener('load',StartWebSocket);"
"</script>";

// =============================================================================
// Page Shell
// =============================================================================

esp_err_t WebCore_SendShellOpen(httpd_req_t *req, const char *title)
{
    httpd_resp_set_type(req, "text/html");

    char head[256];
    int n = snprintf(head, sizeof(head),
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>%s</title>", title ? title : "LED Clock");
    esp_err_t err = httpd_resp_send_chunk(req, head, n);
    if (err != ESP_OK) return err;

    err = httpd_resp_send_chunk(req, CSS_STYLE, sizeof(CSS_STYLE) - 1);
    if (err != ESP_OK) return err;

    const char *bodyOpen = g_themeLight ? "</head><body class='light'>" : "</head><body>";
    return httpd_resp_send_chunk(req, bodyOpen, strlen(bodyOpen));
}

void WebCore_SetMessage(int code, const char *text)
{
    s_pendingMessCode = code;
    strlcpy(s_pendingMessText, text ? text : "", sizeof(s_pendingMessText));
}

static void messageFor(int code, char *outText, size_t outTextSz, const char **outColor)
{
    switch (code) {
        case FA_MESS_01: strlcpy(outText, "Files on SPIFFS", outTextSz); *outColor = "#38bdf8"; break;
        case FA_MESS_02: strlcpy(outText, "Click on File to Download", outTextSz); *outColor = "#38bdf8"; break;
        case FA_MESS_03: strlcpy(outText, "Upload a File", outTextSz); *outColor = "#38bdf8"; break;
        case FA_MESS_04: strlcpy(outText, "Click on File to Delete", outTextSz); *outColor = "#38bdf8"; break;
        case FA_MESS_06: strlcpy(outText, "Selection Option", outTextSz); *outColor = "#38bdf8"; break;
        case FA_MESS_07: strlcpy(outText, "Files on SPIFFS", outTextSz); *outColor = "#38bdf8"; break;
        case FA_MESS_08: strlcpy(outText, "Select Report", outTextSz); *outColor = "#38bdf8"; break;
        case FA_MESS_09: strlcpy(outText, "File Not Found", outTextSz); *outColor = "#ef4444"; break;
        case FA_MESS_10: strlcpy(outText, "File GeneralLog.txt was deleted", outTextSz); *outColor = "#ef4444"; break;
        case FA_MESS_11: strlcpy(outText, "File IrrigationLog.txt was deleted", outTextSz); *outColor = "#ef4444"; break;
        case FA_MESS_RED: strlcpy(outText, s_pendingMessText, outTextSz); *outColor = "#ef4444"; break;
        case FA_MESS_GRN: strlcpy(outText, s_pendingMessText, outTextSz); *outColor = "#22c55e"; break;
        default: outText[0] = '\0'; *outColor = "#38bdf8"; break;
    }
}

esp_err_t WebCore_SendHeader(httpd_req_t *req, int reqMess)
{
    int actMess = (s_pendingMessCode != 0) ? s_pendingMessCode : reqMess;
    char text[80];
    const char *color;
    messageFor(actMess, text, sizeof(text), &color);
    s_pendingMessCode = 0; // one-shot — consumed exactly once

    char sysName[SETTING_VALUE_LEN];
    Settings_GetText(S_SYSTEM_NAME, sysName, sizeof(sysName));

    char buf[512];
    int n = snprintf(buf, sizeof(buf),
        "<div class='ph'><h1 id='H01'>%s</h1><div class='dt' id='H02'>%s</div>"
        "<div class='msg' id='H03' style='color:%s'>%s</div></div>",
        sysName, DateTime, color, text);
    return httpd_resp_send_chunk(req, buf, n);
}

esp_err_t WebCore_SendNav(httpd_req_t *req, int active)
{
    bool otherActive = (active == PS_OTHER || active == PS_SETTINGS || active == PS_NTFY ||
                         active == PS_ABOUT || active == PS_EXERCISES || active == PS_REMINDERS ||
                         active == PS_BIRTHDAYS || active == PS_ANNIVERSARY || active == PS_VALIDPER);
    char buf[400];
    int n = snprintf(buf, sizeof(buf),
        "<div class='nav'>"
        "<a href='/'%s>HOME</a>"
        "<a href='/quick'%s>QUICK</a>"
        "<a href='/other'%s>OTHER</a>"
        "</div>",
        active == PS_HOME ? " class='act'" : "",
        active == PS_QUICK ? " class='act'" : "",
        otherActive ? " class='act'" : "");
    return httpd_resp_send_chunk(req, buf, n);
}

esp_err_t WebCore_SendScript(httpd_req_t *req)
{
    return httpd_resp_send_chunk(req, SCRIPT_JS, sizeof(SCRIPT_JS) - 1);
}

esp_err_t WebCore_SendUpdateButton(httpd_req_t *req)
{
    static const char buf[] =
        "<div class='save-row'>"
        "<button class='btn btn-bl' onclick='UpdatePage()'>&#10003; UPDATE</button>"
        "</div>";
    return httpd_resp_send_chunk(req, buf, sizeof(buf) - 1);
}

esp_err_t WebCore_SendUpdateButtonNav(httpd_req_t *req, const char *label, const char *dest)
{
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
        "<div class='save-row'>"
        "<button class='btn btn-bl' onclick='updateAndRedirect()'>&#10003; %s</button>"
        "<button class='btn' onclick='cancelAndRedirect()'>&#10007; CANCEL</button>"
        "</div>"
        "<script>"
        "function updateAndRedirect(){UpdatePage();setTimeout(function(){window.location.href='%s';},1500);}"
        "function cancelAndRedirect(){window.location.href='%s';}"
        "</script>",
        label, dest, dest);
    return httpd_resp_send_chunk(req, buf, n);
}

esp_err_t WebCore_SendShellClose(httpd_req_t *req)
{
    static const char buf[] = "</body></html>";
    esp_err_t err = httpd_resp_send_chunk(req, buf, sizeof(buf) - 1);
    if (err != ESP_OK) return err;
    return httpd_resp_send_chunk(req, NULL, 0); // finalize chunked transfer
}
