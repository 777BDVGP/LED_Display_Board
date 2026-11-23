// =============================================================================
// web_settings.c — Settings Page
// =============================================================================
// Ported from LED_Display_Board_ARD's 07_Settings.cpp (handleSettings /
// WTC_Settings / UpdateSettingsPageValues). The dead WTC_Column/JS-table
// pipeline is skipped (see Phase C infra notes) — rows are built as direct
// HTML, matching what ARD's settingRow()/settingSection() actually emit.
// =============================================================================

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "event_store.h"

#include "field_type.h"
#include "sensors.h"
#include "settings_store.h"
#include "time_sync.h"
#include "web_about.h"
#include "web_core.h"
#include "web_settings.h"
#include "ws_hub.h"

// =============================================================================
// Row / Section Builders
// =============================================================================

static esp_err_t sendSection(httpd_req_t *req, const char *title)
{
    char buf[100];
    int n = snprintf(buf, sizeof(buf), "<div class='sec'>%s</div>", title);
    return httpd_resp_send_chunk(req, buf, n);
}

static esp_err_t sendSubSection(httpd_req_t *req, const char *title)
{
    char buf[100];
    int n = snprintf(buf, sizeof(buf), "<div class='sub'>%s</div>", title);
    return httpd_resp_send_chunk(req, buf, n);
}

static esp_err_t sendSettingRow(httpd_req_t *req, int idx)
{
    if (!g_settings[idx].web_display) return ESP_OK; // hidden field — matches ARD's settingRow() early return

    char value[SETTING_VALUE_LEN];
    Settings_GetText(idx, value, sizeof(value));

    char buf[300];
    int n;
    if (g_settings[idx].type == FIELD_BOOL) {
        n = snprintf(buf, sizeof(buf),
            "<div class='gr'><span class='lbl'>%s</span>"
            "<input type='checkbox' id='%d'%s></div>",
            g_settings[idx].info, ID_SETTINGS + idx, strcmp(value, "true") == 0 ? " checked" : "");
    } else if (g_settings[idx].type == FIELD_NUMB) {
        // min='0': Settings_ValidateNEGNumeric (settings_store.c) already
        // clamps every FIELD_NUMB save to >=0 server-side; this just makes
        // the browser enforce (and the spinner reflect) the same floor.
        n = snprintf(buf, sizeof(buf),
            "<div class='gr'><span class='lbl'>%s</span>"
            "<input type='number' min='0' id='%d' value='%s'></div>",
            g_settings[idx].info, ID_SETTINGS + idx, value);
    } else {
        n = snprintf(buf, sizeof(buf),
            "<div class='gr'><span class='lbl'>%s</span>"
            "<input type='%s' id='%d' value='%s'></div>",
            g_settings[idx].info, field_type_html(g_settings[idx].type), ID_SETTINGS + idx, value);
    }
    return httpd_resp_send_chunk(req, buf, n);
}

// -----------------------------------------------------------------------------
// sendYearDateRow
// Hand-built row combining S_YEARDATE_DISPLAY_SEC's number input with the
// S_YEARDATE_STYLE select on the same line — mirrors how S_EVT_NTFY_TOPIC/
// S_EVT_NTFY_PRIO share one row in web_events.c. S_YEARDATE_STYLE itself has
// web_display=false so the generic sendSettingRows() loop skips it.
// -----------------------------------------------------------------------------
static esp_err_t sendYearDateRow(httpd_req_t *req)
{
    char secVal[SETTING_VALUE_LEN];
    Settings_GetText(S_YEARDATE_DISPLAY_SEC, secVal, sizeof(secVal));
    char styleVal[SETTING_VALUE_LEN];
    Settings_GetText(S_YEARDATE_STYLE, styleVal, sizeof(styleVal));

    static const char *const styles[] = { "Full Date", "Date/Time" };

    char buf[400];
    int pos = snprintf(buf, sizeof(buf),
        "<div class='gr'><span class='lbl'>%s</span>"
        "<input type='number' min='0' id='%d' value='%s' style='max-width:70px'>"
        "<select id='%d' style='max-width:110px'>",
        g_settings[S_YEARDATE_DISPLAY_SEC].info, ID_SETTINGS + S_YEARDATE_DISPLAY_SEC, secVal,
        ID_SETTINGS + S_YEARDATE_STYLE);
    for (int i = 0; i < 2 && pos < (int)sizeof(buf) - 1; i++) {
        bool sel = strcmp(styleVal, styles[i]) == 0;
        pos += snprintf(buf + pos, sizeof(buf) - pos, "<option value='%s'%s>%s</option>",
                         styles[i], sel ? " selected" : "", styles[i]);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "</select></div>");
    return httpd_resp_send_chunk(req, buf, pos);
}

// -----------------------------------------------------------------------------
// sendDetectOffRow
// Hand-built row combining a Day/Night detect-off start time with its minutes
// value on one line — same "web_display=false on the secondary field" pattern
// as sendYearDateRow. Shared by both the day and night rows.
// -----------------------------------------------------------------------------
static esp_err_t sendDetectOffRow(httpd_req_t *req, int startIdx, int minIdx, int minValue)
{
    char startVal[SETTING_VALUE_LEN];
    Settings_GetText(startIdx, startVal, sizeof(startVal));
    char minVal[SETTING_VALUE_LEN];
    Settings_GetText(minIdx, minVal, sizeof(minVal));

    char buf[400];
    int n = snprintf(buf, sizeof(buf),
        "<div class='gr'><span class='lbl'>%s</span>"
        "<span style='font-size:0.72em;color:#94a3b8;margin-right:6px'>HH:MM (Start Time)</span>"
        "<input type='time' id='%d' value='%s' style='max-width:90px'>"
        "<input type='number' min='%d' id='%d' value='%s' style='max-width:60px'></div>",
        g_settings[minIdx].info, ID_SETTINGS + startIdx, startVal, minValue, ID_SETTINGS + minIdx, minVal);
    return httpd_resp_send_chunk(req, buf, n);
}

// -----------------------------------------------------------------------------
// sendTimeZoneRow — S_TIME_ZONE as a dropdown of g_tzOptions (time_sync.c).
// A stored value that isn't in the list (e.g. set by hand in Settings.json)
// is kept and shown as "Custom".
// -----------------------------------------------------------------------------
static esp_err_t sendTimeZoneRow(httpd_req_t *req)
{
    char cur[SETTING_VALUE_LEN];
    Settings_GetText(S_TIME_ZONE, cur, sizeof(cur));

    esp_err_t err;
    char buf[200];
    int n = snprintf(buf, sizeof(buf),
        "<div class='gr'><span class='lbl'>Time Zone</span>"
        "<select id='%d' style='flex:3;min-width:0;max-width:75%%'>", ID_SETTINGS + S_TIME_ZONE);
    if ((err = httpd_resp_send_chunk(req, buf, n)) != ESP_OK) return err;

    bool found = false;
    for (int i = 0; i < g_tzOptionCount; i++) {
        bool sel = !found && strcmp(cur, g_tzOptions[i].posix) == 0;
        if (sel) found = true;
        n = snprintf(buf, sizeof(buf), "<option value='%s'%s>%s</option>",
                     g_tzOptions[i].posix, sel ? " selected" : "", g_tzOptions[i].label);
        if ((err = httpd_resp_send_chunk(req, buf, n)) != ESP_OK) return err;
    }
    if (!found) {
        n = snprintf(buf, sizeof(buf), "<option value='%s' selected>Custom: %s</option>", cur, cur);
        if ((err = httpd_resp_send_chunk(req, buf, n)) != ESP_OK) return err;
    }
    static const char close[] =
        "</select></div>"
        "<div style='font-size:12px;opacity:.75;margin:2px 10px 6px'>Daylight saving switches by itself where the zone has it.</div>";
    return httpd_resp_send_chunk(req, close, sizeof(close) - 1);
}

static esp_err_t sendSettingRows(httpd_req_t *req, int fromIdx, int toIdxExclusive)
{
    for (int i = fromIdx; i < toIdxExclusive; i++) {
        esp_err_t err = sendSettingRow(req, i);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

// =============================================================================
// GET /settings
// =============================================================================

static esp_err_t settings_handler(httpd_req_t *req)
{
    esp_err_t err;
    if ((err = WebCore_SendShellOpen(req, "LED Clock - Settings")) != ESP_OK) return err;
    if ((err = WebCore_SendHeader(req, FA_MESS_06)) != ESP_OK) return err;
    if ((err = WebCore_SendNav(req, PS_SETTINGS)) != ESP_OK) return err;
    if ((err = WebCore_SendScript(req)) != ESP_OK) return err;

    if ((err = sendSection(req, "SYSTEM")) != ESP_OK) return err;
    if ((err = sendSettingRows(req, 0, S_DISPLAY_LINE_1)) != ESP_OK) return err;

    if ((err = sendSection(req, "TIME ZONE")) != ESP_OK) return err;
    if ((err = sendTimeZoneRow(req)) != ESP_OK) return err;

    if ((err = sendSection(req, "BRIGHTNESS")) != ESP_OK) return err;
    if ((err = sendSettingRows(req, S_DISPLAY_LINE_1 + 1, S_DISPLAY_LINE_2)) != ESP_OK) return err;

    if ((err = sendSection(req, "HUMAN DETECT COLOUR")) != ESP_OK) return err;
    if ((err = sendSettingRows(req, S_DISPLAY_LINE_2 + 1, S_DISPLAY_LINE_3)) != ESP_OK) return err;

    if ((err = sendSection(req, "DISPLAY ON / OFF")) != ESP_OK) return err;
    if ((err = sendSettingRow(req, S_DISPLAY_ON_HUMAN_DET)) != ESP_OK) return err;
    if ((err = sendDetectOffRow(req, S_DAY_DETECT_OFF_START, S_DAY_DETECT_OFF_MIN, 0)) != ESP_OK) return err;
    if ((err = sendDetectOffRow(req, S_NIGHT_DETECT_OFF_START, S_NIGHT_DETECT_OFF_SEC, NIGHT_DETECT_OFF_MIN_SEC)) != ESP_OK) return err;

    if ((err = sendSection(req, "BUZZER DELAY")) != ESP_OK) return err;
    if ((err = sendSettingRows(req, S_DISPLAY_LINE_4 + 1, S_DISPLAY_LINE_5)) != ESP_OK) return err;

    if ((err = sendSection(req, "TIME DISPLAY")) != ESP_OK) return err;
    if ((err = sendSettingRow(req, S_WEEKDAY_DISPLAY_SEC)) != ESP_OK) return err;
    if ((err = sendYearDateRow(req)) != ESP_OK) return err;

    if ((err = sendSection(req, "DAY / NIGHT COLOURS")) != ESP_OK) return err;
    if ((err = sendSettingRows(req, S_DISPLAY_LINE_6 + 1, S_DISPLAY_LINE_7)) != ESP_OK) return err;

    if ((err = sendSection(req, "BUZZER PATTERNS")) != ESP_OK) return err;
    if ((err = sendSubSection(req, "3 SHORT BEEPS")) != ESP_OK) return err;
    if ((err = sendSettingRows(req, S_BUZ_3_SHORT_TEST, S_BUZ_3_SHORT_OFF + 1)) != ESP_OK) return err;
    if ((err = sendSubSection(req, "5 LONG BEEPS")) != ESP_OK) return err;
    if ((err = sendSettingRows(req, S_BUZ_5_LONG_TEST, S_BUZ_5_LONG_OFF + 1)) != ESP_OK) return err;
    if ((err = sendSubSection(req, "CONSTANT SHORT")) != ESP_OK) return err;
    if ((err = sendSettingRows(req, S_BUZ_CONS_SHORT_TEST, S_BUZ_CONS_SHORT_OFF + 1)) != ESP_OK) return err;
    if ((err = sendSubSection(req, "CONSTANT LONG")) != ESP_OK) return err;
    if ((err = sendSettingRows(req, S_BUZ_CONS_LONG_TEST, S_BUZ_CONS_LONG_OFF + 1)) != ESP_OK) return err;

    if ((err = sendSection(req, "MIDNIGHT REBOOT")) != ESP_OK) return err;
    if ((err = sendSettingRow(req, S_MIDNIGHT_REBOOT_TIME)) != ESP_OK) return err;
    if ((err = sendSettingRow(req, S_MIDNIGHT_REBOOT_ENABLE)) != ESP_OK) return err;

    if ((err = sendSection(req, "BIRTHDAYS &amp; ANNIVERSARIES")) != ESP_OK) return err;
    {
        // S_BIRTHDAY_TIME is web_display=false (stays hidden from SYSTEM's loop
        // above) — rendered manually here instead, matching ARD.
        char birthdayTime[SETTING_VALUE_LEN];
        Settings_GetText(S_BIRTHDAY_TIME, birthdayTime, sizeof(birthdayTime));
        char buf[300];
        int n = snprintf(buf, sizeof(buf),
            "<div class='gr'><span class='lbl'>Display Time (HH:MM)</span>"
            "<input type='time' id='%d' value='%s'></div>",
            ID_SETTINGS + S_BIRTHDAY_TIME, birthdayTime);
        if ((err = httpd_resp_send_chunk(req, buf, n)) != ESP_OK) return err;
    }
    if ((err = sendSettingRow(req, S_BIRTHDAY_TIME_OVR)) != ESP_OK) return err;

    if ((err = sendSection(req, "WEB DISPLAY THEME")) != ESP_OK) return err;
    {
        char buf[700];
        int n = snprintf(buf, sizeof(buf),
            "<div class='gr'><span class='lbl'>Theme</span>"
            "<div style='display:flex;gap:20px;align-items:center'>"
            "<label style='cursor:pointer;display:flex;gap:6px;align-items:center'>"
            "<input type='radio' name='gthm' value='0' onchange='setTheme(this.value)'%s> Dark</label>"
            "<label style='cursor:pointer;display:flex;gap:6px;align-items:center'>"
            "<input type='radio' name='gthm' value='1' onchange='setTheme(this.value)'%s> Light</label>"
            "</div></div>"
            "<script>function setTheme(v){fetch('/settheme?v='+v).then(function(){location.reload();});}</script>",
            g_themeLight ? "" : " checked", g_themeLight ? " checked" : "");
        if ((err = httpd_resp_send_chunk(req, buf, n)) != ESP_OK) return err;
    }

    if ((err = WebCore_SendUpdateButton(req)) != ESP_OK) return err;
    return WebCore_SendShellClose(req);
}

// =============================================================================
// WebSocket update handler
// =============================================================================

static bool isAllDigits(const char *s)
{
    if (!s || s[0] == '\0') return false;
    for (const char *p = s; *p; p++) {
        if (!isdigit((unsigned char)*p)) return false;
    }
    return true;
}

// -----------------------------------------------------------------------------
// UpdateSettingsPageValues
// Called by ws_hub's dispatcher for ID_SETTINGS payloads. Matches every key
// to a g_settings[] index by numeric ID, applies type-specific validation
// (RGB string, non-negative integer), persists, and re-derives the
// human-detect stay-on timer. Ported from ARD's UpdateSettingsPageValues().
// -----------------------------------------------------------------------------
void UpdateSettingsPageValues(cJSON *root)
{
    bool birthdayTimeInvalid = false;
    char tzBefore[SETTING_VALUE_LEN];
    Settings_GetText(S_TIME_ZONE, tzBefore, sizeof(tzBefore));

    cJSON *kv = NULL;
    cJSON_ArrayForEach(kv, root) {
        const char *key = kv->string;
        if (!isAllDigits(key)) continue;

        int i = atoi(key) - ID_SETTINGS;
        if (i < 0 || i >= SETTING_ENTRIES) continue;

        char value[SETTING_VALUE_LEN];
        if (cJSON_IsString(kv)) {
            strlcpy(value, kv->valuestring, sizeof(value));
        } else if (cJSON_IsBool(kv)) {
            strlcpy(value, cJSON_IsTrue(kv) ? "true" : "false", sizeof(value));
        } else if (cJSON_IsNumber(kv)) {
            snprintf(value, sizeof(value), "%g", kv->valuedouble);
        } else {
            value[0] = '\0';
        }

        if (i == S_DAY_TIME_COLOR || i == S_NIGHT_TIME_COLOR) {
            char validated[SETTING_VALUE_LEN];
            Settings_ValidateRGBString(value, validated, sizeof(validated));
            Settings_PutText(i, validated);
        } else if (g_settings[i].type == FIELD_NUMB) {
            char validated[SETTING_VALUE_LEN];
            Settings_ValidateNEGNumeric(value, validated, sizeof(validated));
            if (i == S_NIGHT_DETECT_OFF_SEC && atoi(validated) < NIGHT_DETECT_OFF_MIN_SEC) {
                snprintf(validated, sizeof(validated), "%d", NIGHT_DETECT_OFF_MIN_SEC);
            }
            Settings_PutText(i, validated);
        } else {
            Settings_PutText(i, value);
        }
    }

    // If the birthday override is enabled, a valid HH:MM must be set.
    if (Settings_GetBool(S_BIRTHDAY_TIME_OVR)) {
        char t[SETTING_VALUE_LEN];
        Settings_GetText(S_BIRTHDAY_TIME, t, sizeof(t));
        if (strlen(t) < 4 || strchr(t, ':') == NULL) {
            Settings_PutBool(S_BIRTHDAY_TIME_OVR, false);
            birthdayTimeInvalid = true;
        }
    }

    if (birthdayTimeInvalid) {
        WS_MessUpdate("Invalid birthday time - Override disabled!", "color: red", true);
    } else {
        WS_MessUpdate("Settings Updated!", "color: red", true);
    }
    Settings_WriteData();

    // New time zone: apply now (no restart) and rebuild today's events, since
    // the local date may have changed with it.
    char tzAfter[SETTING_VALUE_LEN];
    Settings_GetText(S_TIME_ZONE, tzAfter, sizeof(tzAfter));
    if (strcmp(tzBefore, tzAfter) != 0) {
        if (tzAfter[0] == 0) Settings_PutText(S_TIME_ZONE, tzBefore);
        TimeSync_ApplyTimeZone();
        Event_RefreshTodayList();
        Settings_GetText(S_TIME_ZONE, tzAfter, sizeof(tzAfter));
        enqueueSyncTimeZone(tzAfter);   // the other clocks follow
    }

    // Un-dim on save: if the display is currently off (stay-on timer expired),
    // saving Settings — especially a new Day/Night detect-off duration — should
    // wake it immediately with the fresh values rather than leaving it dark
    // until the next radar trigger. DisplayTask's periodic brightness refresh
    // (display_task.c) picks this up within ~5s.
    HumanDetect_ActivateStayOn();
}

// POST /settz  body: POSIX TZ string — sent by a peer clock whose Time Zone
// was just changed. Only values from g_tzOptions are accepted. Applied here
// and NOT passed on, so a change can never bounce between clocks.
static esp_err_t settz_handler(httpd_req_t *req)
{
    char tz[SETTING_VALUE_LEN];
    int len = req->content_len;
    if (len <= 0 || len >= (int)sizeof(tz)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad time zone");
        return ESP_FAIL;
    }
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, tz + got, len - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) return ESP_FAIL;
        got += r;
    }
    tz[got] = '\0';

    bool known = false;
    for (int i = 0; i < g_tzOptionCount; i++) {
        if (strcmp(g_tzOptions[i].posix, tz) == 0) { known = true; break; }
    }
    if (!known) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "unknown time zone");
        return ESP_FAIL;
    }

    char cur[SETTING_VALUE_LEN];
    Settings_GetText(S_TIME_ZONE, cur, sizeof(cur));
    if (strcmp(cur, tz) != 0) {
        Settings_PutText(S_TIME_ZONE, tz);
        Settings_WriteData();
        TimeSync_ApplyTimeZone();
        Event_RefreshTodayList();
    }
    return httpd_resp_sendstr(req, "OK");
}

void WebSettings_RegisterRoutes(httpd_handle_t server)
{
    static const httpd_uri_t settz_uri = {
        .uri = "/settz", .method = HTTP_POST, .handler = settz_handler,
    };
    httpd_register_uri_handler(server, &settz_uri);
    static const httpd_uri_t settings_uri = {
        .uri = "/settings", .method = HTTP_GET, .handler = settings_handler,
    };
    httpd_register_uri_handler(server, &settings_uri);
}
