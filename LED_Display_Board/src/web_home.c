// =============================================================================
// web_home.c — Home Page + Other Hub Page
// =============================================================================
// See web_home.h. Ported from LED_Display_Board_ARD's 06_Home.cpp
// (handleRoot / WTC_HomePage / handleOther).
// =============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"

#include "datetime.h"
#include "event_store.h"
#include "web_core.h"
#include "web_home.h"

// -----------------------------------------------------------------------------
// root_handler  (GET /)
// Today's events table: one row per active event in g_eventsDisplay, streamed
// per-row (bounded RAM regardless of list length, matches ARD's per-row flush).
// -----------------------------------------------------------------------------
static esp_err_t root_handler(httpd_req_t *req)
{
    esp_err_t err;
    if ((err = WebCore_SendShellOpen(req, "LED Clock - Home")) != ESP_OK) return err;
    if ((err = WebCore_SendHeader(req, FA_MESS_01)) != ESP_OK) return err;
    if ((err = WebCore_SendNav(req, PS_HOME)) != ESP_OK) return err;
    if ((err = WebCore_SendScript(req)) != ESP_OK) return err;

    static const char secOpen[] =
        "<div class='sec' style='display:flex;justify-content:space-between;align-items:center'>"
        "<span>TODAY'S EVENTS</span>"
        "<button class='btn btn-rd btn-sm' onclick='resetOverrides()'>RESET TIME/STATUS</button>"
        "</div>"
        "<table><tr><th>TYPE</th><th>EVENT</th><th>TIME</th><th>STATUS</th></tr>";
    if ((err = httpd_resp_send_chunk(req, secOpen, sizeof(secOpen) - 1)) != ESP_OK) return err;

    int shown = 0;
    for (int i = 0; i < g_eventsDisplayCount; i++) {
        EventRuntime *e = &g_eventsDisplay[i];
        if (!e->active) continue;
        shown++;

        char badge[220];
        if (e->done) {
            strlcpy(badge, "<span class='tag' style='background:#450a0a;color:#fca5a5'>FIRED</span>", sizeof(badge));
        } else if (e->disabled) {
            snprintf(badge, sizeof(badge),
                     "<span class='tag' style='background:#374151;color:#d1d5db;cursor:pointer' "
                     "onclick=\"toggleDisable('%s',true)\">DISABLED</span>", e->filename);
        } else {
            char icons[24] = "";
            if (e->ntfy)  strlcat(icons, " NTFY", sizeof(icons));
            if (e->radar) strlcat(icons, " RADAR", sizeof(icons));
            snprintf(badge, sizeof(badge),
                     "<span class='tag tok' style='cursor:pointer' onclick=\"toggleDisable('%s',false)\">PENDING%s%s%s</span>",
                     e->filename, e->radar ? " - HUMAN" : "", e->one_time ? " - ONE TIME" : "", icons);
        }

        // EVENT column: label, plus the Message field (if set), plus — for
        // Birthday/Anniversary — the computed age, mirroring the "%d years"
        // age AppLogicTask (app_logic_task.c) attaches when the event actually
        // fires, so it's visible here too rather than only at fire time.
        char desc[160];
        strlcpy(desc, e->label, sizeof(desc));
        {
            char mess[64];
            strlcpy(mess, e->mess, sizeof(mess));
            char *p = mess;
            while (*p == ' ') p++;
            size_t len = strlen(p);
            while (len > 0 && p[len - 1] == ' ') p[--len] = '\0';
            if (p[0] != '\0') {
                strlcat(desc, " - ", sizeof(desc));
                strlcat(desc, p, sizeof(desc));
            }
        }
        if ((strcmp(e->type, EVY_TYPE_BIR) == 0 || strcmp(e->type, EVY_TYPE_ANN) == 0) && strlen(e->date) >= 4) {
            int birthYear = atoi(e->date);
            char today[11];
            GetCurDisplayDate(today, sizeof(today));
            int curYear = atoi(today);
            if (birthYear > 0 && curYear > birthYear) {
                char ageStr[24];
                snprintf(ageStr, sizeof(ageStr), " (%d years)", curYear - birthYear);
                strlcat(desc, ageStr, sizeof(desc));
            }
        }

        char timeCell[280];
        if (e->done) {
            strlcpy(timeCell, e->time, sizeof(timeCell));
        } else {
            snprintf(timeCell, sizeof(timeCell),
                     "<input type='time' value='%s' onchange=\"changeTime('%s',this.value)\" "
                     "style='background:transparent;border:1px solid #334155;color:inherit;"
                     "border-radius:4px;padding:2px 4px;font:inherit;width:90px'>",
                     e->time, e->filename);
        }

        char row[900];
        int n = snprintf(row, sizeof(row), "<tr><td>%s</td><td>%s</td><td>%s</td><td>%s</td></tr>",
                          e->type, desc, timeCell, badge);
        if ((err = httpd_resp_send_chunk(req, row, n)) != ESP_OK) return err;
    }

    if (shown == 0) {
        static const char none[] =
            "<tr><td colspan='4' style='text-align:center;color:#64748b;padding:16px'>No events scheduled today</td></tr>";
        if ((err = httpd_resp_send_chunk(req, none, sizeof(none) - 1)) != ESP_OK) return err;
    }

    static const char tableClose[] = "</table>";
    if ((err = httpd_resp_send_chunk(req, tableClose, sizeof(tableClose) - 1)) != ESP_OK) return err;

    static const char toggleScript[] =
        "<script>function toggleDisable(f,isDisabled){"
        "fetch('/toggledisable?f='+f).then(function(r){"
        "if(r.ok){location.reload();}else{r.text().then(function(t){alert('Could not update: '+(t||r.status));});}"
        "});}"
        "function changeTime(f,newTime){"
        "if(!newTime)return;"
        "fetch('/changetime?f='+f+'&t='+newTime).then(function(r){"
        "if(r.ok){location.reload();}else{r.text().then(function(t){alert('Could not update time: '+(t||r.status));});}"
        "});}"
        "function resetOverrides(){"
        "fetch('/resetoverrides').then(function(r){"
        "if(r.ok){location.reload();}else{r.text().then(function(t){alert('Could not reset: '+(t||r.status));});}"
        "});}</script>";
    if ((err = httpd_resp_send_chunk(req, toggleScript, sizeof(toggleScript) - 1)) != ESP_OK) return err;

    return WebCore_SendShellClose(req);
}

// -----------------------------------------------------------------------------
// other_handler  (GET /other)
// Navigation hub: links to Settings/Files/NTFY/About/Exercises/DiskTools, plus
// system actions (Reboot/Reset ASP/Edit WiFi). Some link targets (Settings,
// NTFY, About, Exercises, DiskTools, Reset ASP, Edit WiFi) aren't ported yet —
// expected 404s until their phases land.
// -----------------------------------------------------------------------------
static esp_err_t other_handler(httpd_req_t *req)
{
    esp_err_t err;
    if ((err = WebCore_SendShellOpen(req, "LED Clock - Other")) != ESP_OK) return err;
    if ((err = WebCore_SendHeader(req, FA_MESS_06)) != ESP_OK) return err;
    if ((err = WebCore_SendNav(req, PS_OTHER)) != ESP_OK) return err;
    if ((err = WebCore_SendScript(req)) != ESP_OK) return err;

    static const char content[] =
        "<div class='sec'>OTHER</div>"
        "<div style='display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:8px'>"
        "<a class='btn btn-gn' href='/reminders'>REMINDERS</a>"
        "<a class='btn btn-bl' href='/settings'>SETTINGS</a>"
        "<a class='btn btn-gn' href='/validperiods'>REMINDER VALID PERIODS</a>"
        "<a class='btn btn-bl' href='/about'>ABOUT</a>"
        "<a class='btn btn-gn' href='/birthdays'>BIRTHDAYS</a>"
        "<a class='btn btn-bl' href='/ntfy'>NTFY</a>"
        "<a class='btn btn-gn' href='/anniversary'>ANNIVERSARYS</a>"
        "<a class='btn btn-bl' href='/files'>FILES</a>"
        "<a class='btn btn-bl' href='/disktools'>DISK TOOLS</a>"
        "<a class='btn btn-bl' href='/exercises'>EXERCISES</a>"
        "</div>"
        "<div class='sec'>SYSTEM</div>"
        "<div style='display:grid;grid-template-columns:1fr 1fr;gap:8px;margin-top:8px'>"
        "<button class='btn btn-rd' onclick='ButtonReboot()'>REBOOT</button>"
        "<a class='btn btn-rd' href='/setASP'>RESET ASP</a>"
        "<a class='btn' style='background:#f97316;color:#0f172a;grid-column:1/-1' href='/wifi'>WIFI NETWORKS</a>"
        "</div>";
    if ((err = httpd_resp_send_chunk(req, content, sizeof(content) - 1)) != ESP_OK) return err;

    return WebCore_SendShellClose(req);
}

void WebHome_RegisterRoutes(httpd_handle_t server)
{
    static const httpd_uri_t root_uri  = { .uri = "/",      .method = HTTP_GET, .handler = root_handler };
    static const httpd_uri_t other_uri = { .uri = "/other", .method = HTTP_GET, .handler = other_handler };
    httpd_register_uri_handler(server, &root_uri);
    httpd_register_uri_handler(server, &other_uri);
}
