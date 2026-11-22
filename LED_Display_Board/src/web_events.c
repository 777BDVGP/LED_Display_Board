// =============================================================================
// web_events.c — Birthdays / Anniversary / Reminders / Quick Pages + CSV
// =============================================================================
// Ported from LED_Display_Board_ARD's 08_WEBEvents.cpp. Birthdays/Anniversary/
// Reminders are handled by one parameterized handler (ARD's three near-
// identical handleBirthdays/handleAnniversary/handleReminders collapsed into
// one function driven by EventPageConfig) rather than copy-pasted three times.
//
// A single file-scope EventRecord (s_editEvent) mirrors ARD's global
// EventObject: populated by the Create/Edit branch when a form page loads,
// then mutated in place by UpdateEventPageValues() when the WebSocket save
// message arrives — same two-request-cycle design ARD uses.
// =============================================================================

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"

#include "datetime.h"
#include "event_store.h"
#include "field_type.h"
#include "files.h"
#include "ntfy.h"
#include "valid_periods.h"
#include "web_about.h"
#include "web_core.h"
#include "web_events.h"
#include "ws_hub.h"

// =============================================================================
// Shared Edit State (mirrors ARD's global EventObject / EditFileName)
// =============================================================================

static EventRecord s_editEvent;
static char        s_editFileName[64] = ""; // SPIFFS-root-relative path w/ ext, "" = create

// =============================================================================
// Small Helpers
// =============================================================================

static bool isAllDigits(const char *s)
{
    if (!s || s[0] == '\0') return false;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return false;
    }
    return true;
}

static void trimInPlace(char *s)
{
    char *start = s;
    while (*start == ' ') start++;
    char *end = start + strlen(start);
    while (end > start && *(end - 1) == ' ') *(--end) = '\0';
    if (start != s) memmove(s, start, strlen(start) + 1);
}

// =============================================================================
// Event Field Row Renderers  (ESP-IDF port of ARD's fillEventToBuffer/evt*Row)
// =============================================================================

static esp_err_t sendEventTextRow(httpd_req_t *req, EventRecord *ev, int idx)
{
    if (!ev->fields[idx].web_display) return ESP_OK;
    char buf[220];
    int n = snprintf(buf, sizeof(buf),
        "<div class='gr'><span class='lbl'>%s</span><input type='%s' id='%d' value='%s'></div>",
        ev->fields[idx].info, field_type_html(ev->fields[idx].type), ID_EVENTS + idx, ev->fields[idx].value);
    return httpd_resp_send_chunk(req, buf, n);
}

static esp_err_t sendEventCheckboxRow(httpd_req_t *req, EventRecord *ev, int idx)
{
    if (!ev->fields[idx].web_display) return ESP_OK;
    char buf[200];
    int n = snprintf(buf, sizeof(buf),
        "<div class='gr'><span class='lbl'>%s</span><input type='checkbox' id='%d'%s></div>",
        ev->fields[idx].info, ID_EVENTS + idx, strcmp(ev->fields[idx].value, "true") == 0 ? " checked" : "");
    return httpd_resp_send_chunk(req, buf, n);
}

static esp_err_t sendEventSelectRow(httpd_req_t *req, EventRecord *ev, int idx,
                                     const char *const *options, int numOptions)
{
    if (!ev->fields[idx].web_display) return ESP_OK;
    char buf[500];
    int pos = snprintf(buf, sizeof(buf), "<div class='gr'><span class='lbl'>%s</span><select id='%d'>",
                        ev->fields[idx].info, ID_EVENTS + idx);
    for (int i = 0; i < numOptions && pos < (int)sizeof(buf) - 1; i++) {
        bool sel = strcmp(ev->fields[idx].value, options[i]) == 0;
        pos += snprintf(buf + pos, sizeof(buf) - pos, "<option value='%s'%s>%s</option>",
                         options[i], sel ? " selected" : "", options[i]);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "</select></div>");
    return httpd_resp_send_chunk(req, buf, pos);
}

// -----------------------------------------------------------------------------
// sendReminderRepeatRow
// The Reminders-only "Repeat" UI: a type selector plus weekday/multi-day/month
// controls that live-encode into the hidden S_EVT_WEEKDAYS input as the user
// changes them (repEncode()), and decode a persisted pattern back into the
// controls on load (repDecode()). Grammar mirrors Event_IsWeekdayValid()
// exactly (plain digits / "F<n>M<m>"+"F<n>A" / "L<n>M<m>"+"L<n>A").
// -----------------------------------------------------------------------------
static esp_err_t sendReminderRepeatRow(httpd_req_t *req, EventRecord *ev)
{
    int dateId = ID_EVENTS + S_EVT_DATE;
    int wdId   = ID_EVENTS + S_EVT_WEEKDAYS;
    const char *dateVal = ev->fields[S_EVT_DATE].value;
    const char *wdVal   = ev->fields[S_EVT_WEEKDAYS].value;

    char buf[3600];
    int n = snprintf(buf, sizeof(buf),
        "<div class='gr'><span class='lbl'>Repeat</span>"
        "<select id='rep_type' onchange='repChange()'>"
        "<option value='once'>Once on Date</option>"
        "<option value='week1'>Once per Week</option>"
        "<option value='weekmulti'>Multiple per Week</option>"
        "<option value='first'>First Week of Month</option>"
        "<option value='last'>Last Week of Month</option>"
        "</select></div>"
        "<div class='gr' id='rep_wd_row'><span class='lbl'>Weekday</span>"
        "<select id='rep_wd' onchange='repEncode()'>"
        "<option value='1'>Sun</option><option value='2'>Mon</option><option value='3'>Tue</option>"
        "<option value='4'>Wed</option><option value='5'>Thu</option><option value='6'>Fri</option>"
        "<option value='7'>Sat</option>"
        "</select></div>"
        "<div class='gr' id='rep_multi_row'><span class='lbl'>Days (e.g. 1567, Su=1)</span>"
        "<input type='text' id='rep_multi' oninput='repEncode()'></div>"
        "<div class='gr' id='rep_mo_row'><span class='lbl'>Month</span>"
        "<select id='rep_mo' onchange='repEncode()'>"
        "<option value='A'>All Months</option>"
        "<option value='1'>Jan</option><option value='2'>Feb</option><option value='3'>Mar</option>"
        "<option value='4'>Apr</option><option value='5'>May</option><option value='6'>Jun</option>"
        "<option value='7'>Jul</option><option value='8'>Aug</option><option value='9'>Sep</option>"
        "<option value='10'>Oct</option><option value='11'>Nov</option><option value='12'>Dec</option>"
        "</select></div>"
        "<div class='gr' id='rep_date_row'><span class='lbl'>Date</span><input type='date' id='%d' value='%s'></div>"
        "<input type='hidden' id='%d' value='%s'>"
        "<script>"
        "function repChange(){"
        "var t=document.getElementById('rep_type').value;"
        "document.getElementById('rep_wd_row').style.display=(t==='week1'||t==='first'||t==='last')?'flex':'none';"
        "document.getElementById('rep_multi_row').style.display=(t==='weekmulti')?'flex':'none';"
        "document.getElementById('rep_mo_row').style.display=(t==='first'||t==='last')?'flex':'none';"
        "document.getElementById('rep_date_row').style.display=(t==='once')?'flex':'none';"
        "repEncode();"
        "}"
        "function repEncode(){"
        "var t=document.getElementById('rep_type').value;"
        "var out=' ';"
        "if(t==='once'){out=' ';}"
        "else if(t==='week1'){out=document.getElementById('rep_wd').value;}"
        "else if(t==='weekmulti'){out=document.getElementById('rep_multi').value;}"
        "else if(t==='first'||t==='last'){"
        "var wd=document.getElementById('rep_wd').value;"
        "var mo=document.getElementById('rep_mo').value;"
        "var pre=(t==='first')?'F':'L';"
        "out=pre+wd+(mo==='A'?'A':('M'+mo));"
        "}"
        "document.getElementById('%d').value=out;"
        "}"
        "function repDecode(s){"
        "s=s.trim();"
        "if(s===''){document.getElementById('rep_type').value='once';}"
        "else if(/^[1-7]$/.test(s)){document.getElementById('rep_type').value='week1';document.getElementById('rep_wd').value=s;}"
        "else if(/^[1-7]{2,7}$/.test(s)){document.getElementById('rep_type').value='weekmulti';document.getElementById('rep_multi').value=s;}"
        "else if(s[0]==='F'||s[0]==='L'){"
        "document.getElementById('rep_type').value=(s[0]==='F')?'first':'last';"
        "var rest=s.substring(1);"
        "var mIdx=rest.indexOf('M');"
        "var aIdx=rest.indexOf('A');"
        "if(mIdx>=0){document.getElementById('rep_wd').value=rest.substring(0,mIdx);document.getElementById('rep_mo').value=rest.substring(mIdx+1);}"
        "else if(aIdx>=0){document.getElementById('rep_wd').value=rest.substring(0,aIdx);document.getElementById('rep_mo').value='A';}"
        "}else{document.getElementById('rep_type').value='once';}"
        "repChange();"
        "}"
        "repDecode('%s');"
        "</script>",
        dateId, dateVal, wdId, wdVal, wdId, wdVal);
    return httpd_resp_send_chunk(req, buf, n);
}

static esp_err_t sendNtfyRow(httpd_req_t *req, EventRecord *ev)
{
    // Topic options: the two fixed pseudo-topics plus this clock's two
    // actual configured NTFY topic names (set on /ntfy), not literal
    // "Topic1"/"Topic2" placeholders — g_ntfyTopic1/g_ntfyTopic2 are the
    // real runtime-configurable globals from ntfy.c, read fresh on every
    // render since these are pointers to their live storage.
    const char *const topics[] = { "NO_NTFY", "BOTH_NTFYS", g_ntfyTopic1, g_ntfyTopic2 };
    static const char *const prios[]  = { "min", "low", "default", "high", "urgent" };

    char buf[900];
    int pos = snprintf(buf, sizeof(buf),
        "<div class='gr'><span class='lbl'>NTFY</span>"
        "<div style='display:flex;gap:6px;flex:0 0 auto'>"
        "<select id='%d' style='max-width:150px'>", ID_EVENTS + S_EVT_NTFY_TOPIC);
    for (int i = 0; i < 4 && pos < (int)sizeof(buf) - 1; i++) {
        bool sel = strcmp(ev->fields[S_EVT_NTFY_TOPIC].value, topics[i]) == 0;
        pos += snprintf(buf + pos, sizeof(buf) - pos, "<option value='%s'%s>%s</option>",
                         topics[i], sel ? " selected" : "", topics[i]);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos, "</select><select id='%d' style='max-width:90px'>",
                     ID_EVENTS + S_EVT_NTFY_PRIO);
    for (int i = 0; i < 5 && pos < (int)sizeof(buf) - 1; i++) {
        bool sel = strcmp(ev->fields[S_EVT_NTFY_PRIO].value, prios[i]) == 0;
        pos += snprintf(buf + pos, sizeof(buf) - pos, "<option value='%s'%s>%s</option>",
                         prios[i], sel ? " selected" : "", prios[i]);
    }
    pos += snprintf(buf + pos, sizeof(buf) - pos,
                     "</select><input type='checkbox' id='%d'%s></div></div>",
                     ID_EVENTS + S_EVT_NTFY, strcmp(ev->fields[S_EVT_NTFY].value, "true") == 0 ? " checked" : "");
    return httpd_resp_send_chunk(req, buf, pos);
}

static esp_err_t maybeSendOneTimeRow(httpd_req_t *req, EventRecord *ev)
{
    if (strcmp(ev->fields[S_EVT_TYPE].value, EVY_TYPE_REM) != 0) return ESP_OK;
    char buf[200];
    int n = snprintf(buf, sizeof(buf),
        "<div class='gr'><span class='lbl'>One Time</span><input type='checkbox' id='%d'%s></div>",
        ID_EVENTS + S_EVT_ONETIME, strcmp(ev->fields[S_EVT_ONETIME].value, "true") == 0 ? " checked" : "");
    return httpd_resp_send_chunk(req, buf, n);
}

// Reminders-only "Valid Period" dropdown: None + every saved period.
static VpSummary s_vpScratch[VP_MAX_PERIODS];

static esp_err_t maybeSendValidPeriodRow(httpd_req_t *req, EventRecord *ev)
{
    if (strcmp(ev->fields[S_EVT_TYPE].value, EVY_TYPE_REM) != 0) return ESP_OK;
    const char *cur = ev->fields[S_EVT_VALID_PER].value;
    bool curNone = VP_RefIsNone(cur);

    esp_err_t err;
    char buf[200];
    int n = snprintf(buf, sizeof(buf),
        "<div class='gr'><span class='lbl'>Valid Period</span>"
        "<select id='%d' style='flex:3;min-width:0;max-width:75%%'>"
        "<option value=' '%s>None</option>",
        ID_EVENTS + S_EVT_VALID_PER, curNone ? " selected" : "");
    if ((err = httpd_resp_send_chunk(req, buf, n)) != ESP_OK) return err;

    int count = VP_List(s_vpScratch, VP_MAX_PERIODS);
    bool found = curNone;
    for (int i = 0; i < count; i++) {
        bool sel = !curNone && strcmp(cur, s_vpScratch[i].ref) == 0;
        if (sel) found = true;
        n = snprintf(buf, sizeof(buf), "<option value='%s'%s>%s</option>",
                     s_vpScratch[i].ref, sel ? " selected" : "", s_vpScratch[i].name);
        if ((err = httpd_resp_send_chunk(req, buf, n)) != ESP_OK) return err;
    }
    // Referenced period no longer exists — keep the value so a save doesn't
    // silently change it, but label it clearly.
    if (!found) {
        n = snprintf(buf, sizeof(buf), "<option value='%s' selected>(missing) %s</option>", cur, cur);
        if ((err = httpd_resp_send_chunk(req, buf, n)) != ESP_OK) return err;
    }
    static const char close[] = "</select></div>";
    return httpd_resp_send_chunk(req, close, sizeof(close) - 1);
}

static esp_err_t sendEventForm(httpd_req_t *req, EventRecord *ev)
{
    static const char *const colorOpts[] = { ECO_WHITE, ECO_YELLOW, ECO_GREEN, ECO_RED, ECO_BLUE };
    static const char *const buzzOpts[]  = { BUZ_NONE, BUZ_SHORT, BUZ_LONG, BUZ_CONT_SH, BUZ_CONT_LO };

    esp_err_t err;
    if ((err = sendEventTextRow(req, ev, S_EVT_LABLE)) != ESP_OK) return err;
    if ((err = sendEventTextRow(req, ev, S_EVT_MESSAGE)) != ESP_OK) return err;

    if (ev->fields[S_EVT_WEEKDAYS].web_display) {
        if ((err = sendReminderRepeatRow(req, ev)) != ESP_OK) return err;
    } else {
        if ((err = sendEventTextRow(req, ev, S_EVT_DATE)) != ESP_OK) return err;
    }
    if ((err = sendEventTextRow(req, ev, S_EVT_TIME)) != ESP_OK) return err;
    if ((err = sendEventCheckboxRow(req, ev, S_EVT_SHOW_AGE)) != ESP_OK) return err;
    if ((err = sendEventSelectRow(req, ev, S_EVT_COLOR, colorOpts, 5)) != ESP_OK) return err;
    if ((err = sendEventSelectRow(req, ev, S_EVT_BUZZ, buzzOpts, 5)) != ESP_OK) return err;
    if ((err = sendEventTextRow(req, ev, S_EVT_REPEAT)) != ESP_OK) return err;
    if ((err = sendEventCheckboxRow(req, ev, S_EVT_RADAR)) != ESP_OK) return err;
    if ((err = sendNtfyRow(req, ev)) != ESP_OK) return err;
    if ((err = maybeSendOneTimeRow(req, ev)) != ESP_OK) return err;
    if ((err = maybeSendValidPeriodRow(req, ev)) != ESP_OK) return err;
    if ((err = sendEventCheckboxRow(req, ev, S_EVT_ACTIVE)) != ESP_OK) return err;
    return ESP_OK;
}

// =============================================================================
// Create-Form Defaults  (ESP-IDF port of append_Form_Create_EventsToBuffer)
// =============================================================================

static void applyCreateDefaults(EventRecord *ev, const char *type)
{
    Event_InitData(ev);

    if (strcmp(type, EVY_TYPE_BIR) == 0) {
        strlcpy(ev->fields[S_EVT_TYPE].value, EVY_TYPE_BIR, EVENT_VALUE_LEN);
        ev->fields[S_EVT_WEEKDAYS].web_display = false;
        ev->fields[S_EVT_SHOW_AGE].web_display = true;
        strlcpy(ev->fields[S_EVT_REPEAT].value, "5", EVENT_VALUE_LEN);
        strlcpy(ev->fields[S_EVT_TIME].value, "07:00", EVENT_VALUE_LEN);
        strlcpy(ev->fields[S_EVT_MESSAGE].value, "Verjaar", EVENT_VALUE_LEN);
        strlcpy(ev->fields[S_EVT_COLOR].value, ECO_YELLOW, EVENT_VALUE_LEN);
        strlcpy(ev->fields[S_EVT_BUZZ].value, BUZ_CONT_SH, EVENT_VALUE_LEN);
    } else if (strcmp(type, EVY_TYPE_ANN) == 0) {
        strlcpy(ev->fields[S_EVT_TYPE].value, EVY_TYPE_ANN, EVENT_VALUE_LEN);
        ev->fields[S_EVT_WEEKDAYS].web_display = false;
        ev->fields[S_EVT_SHOW_AGE].web_display = true;
        strlcpy(ev->fields[S_EVT_REPEAT].value, "5", EVENT_VALUE_LEN);
        strlcpy(ev->fields[S_EVT_MESSAGE].value, "Anniversary", EVENT_VALUE_LEN);
        strlcpy(ev->fields[S_EVT_COLOR].value, ECO_BLUE, EVENT_VALUE_LEN);
        strlcpy(ev->fields[S_EVT_BUZZ].value, BUZ_CONT_SH, EVENT_VALUE_LEN);
    } else { // EVY_TYPE_REM
        strlcpy(ev->fields[S_EVT_TYPE].value, EVY_TYPE_REM, EVENT_VALUE_LEN);
        ev->fields[S_EVT_WEEKDAYS].web_display = true;
        ev->fields[S_EVT_SHOW_AGE].web_display = false;
        char today[11];
        GetCurDisplayDate(today, sizeof(today));
        strlcpy(ev->fields[S_EVT_DATE].value, today, EVENT_VALUE_LEN);
        strlcpy(ev->fields[S_EVT_MESSAGE].value, "Reminder", EVENT_VALUE_LEN);
        strlcpy(ev->fields[S_EVT_COLOR].value, ECO_RED, EVENT_VALUE_LEN);
        strlcpy(ev->fields[S_EVT_BUZZ].value, BUZ_LONG, EVENT_VALUE_LEN);
    }
}

// =============================================================================
// List / Delete-List Views  (ESP-IDF port of append_Form_EventsToBuffer /
// append_delete_EventsToBuffer)
// =============================================================================

typedef struct {
    const char *type;      // EVY_TYPE_*
    const char *basePath;  // "/birthdays" etc
    const char *typeLabel; // "Birthday" — used in the Create-button text
    int         pageSelect;
    int         sortType;
} EventPageConfig;

// =============================================================================
// Reminder grouping & short badges (Reminders list only)
// =============================================================================

enum { RG_DAILY, RG_WEEKLY, RG_MONTHLY, RG_YEARLY, RG_ONCE, RG_EXPIRED, RG_COUNT };

static const char *const s_rgKey[RG_COUNT]   = { "daily", "weekly", "monthly", "yearly", "once", "expired" };
static const char *const s_rgTitle[RG_COUNT] = { "DAILY", "WEEKLY", "MONTHLY", "YEARLY", "ONCE", "ONCE &ndash; EXPIRED" };
static const char *const s_dayShort[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
static const char *const s_monShort[] = { "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" };

// Sun=1..Sat=7  ->  Mon=1..Sun=7 (lists read Monday-first)
static int monFirst(int d) { return d == 1 ? 7 : d - 1; }

typedef struct {
    int  group;
    char ord;      // 'F' / 'L' for monthly/yearly
    int  wd;       // 1..7 (Sun=1) for monthly/yearly
    int  month;    // 1..12 for yearly
    bool days[8];  // [1..7] Sun=1, for daily/weekly
} ReminderPattern;

static void parseReminderPattern(const EventRuntime *e, const char *today, ReminderPattern *p)
{
    memset(p, 0, sizeof(*p));
    char w[24];
    strlcpy(w, e->weekdays, sizeof(w));
    trimInPlace(w);

    if (w[0] == '\0') {
        p->group = (strlen(e->date) == 10 && strcmp(e->date, today) < 0) ? RG_EXPIRED : RG_ONCE;
        return;
    }
    if (w[0] == 'F' || w[0] == 'L') {
        p->ord = w[0];
        p->wd = atoi(w + 1);
        char *m = strchr(w, 'M');
        if (m) { p->group = RG_YEARLY; p->month = atoi(m + 1); }
        else     p->group = RG_MONTHLY;
        return;
    }
    int n = 0;
    for (const char *c = w; *c; c++) {
        if (*c >= '1' && *c <= '7' && !p->days[*c - '0']) { p->days[*c - '0'] = true; n++; }
    }
    p->group = (n == 7) ? RG_DAILY : RG_WEEKLY;
}

// "16:15 · Mon, Wed · NTFY · RADAR" style badge.
static void buildReminderBadge(const EventRuntime *e, const ReminderPattern *p, char *out, size_t outsz)
{
    char desc[64] = "";
    switch (p->group) {
        case RG_DAILY:
            strlcpy(desc, "Every day", sizeof(desc));
            break;
        case RG_WEEKLY: {
            bool wkdays = p->days[2] && p->days[3] && p->days[4] && p->days[5] && p->days[6] && !p->days[1] && !p->days[7];
            bool wkend  = p->days[1] && p->days[7] && !p->days[2] && !p->days[3] && !p->days[4] && !p->days[5] && !p->days[6];
            if (wkdays)     strlcpy(desc, "Weekdays (Mon-Fri)", sizeof(desc));
            else if (wkend) strlcpy(desc, "Weekends", sizeof(desc));
            else {
                for (int mf = 1; mf <= 7; mf++) {          // Mon..Sun order
                    int d = (mf == 7) ? 1 : mf + 1;
                    if (!p->days[d]) continue;
                    if (desc[0]) strlcat(desc, ", ", sizeof(desc));
                    strlcat(desc, s_dayShort[d - 1], sizeof(desc));
                }
            }
            break;
        }
        case RG_MONTHLY:
        case RG_YEARLY: {
            const char *ord = (p->ord == 'F') ? "1st" : "Last";
            const char *wd  = (p->wd >= 1 && p->wd <= 7) ? s_dayShort[p->wd - 1] : "?";
            if (p->group == RG_MONTHLY) {
                snprintf(desc, sizeof(desc), "%s %s every month", ord, wd);
            } else {
                const char *mo = (p->month >= 1 && p->month <= 12) ? s_monShort[p->month - 1] : "?";
                snprintf(desc, sizeof(desc), "%s %s of %s", ord, wd, mo);
            }
            break;
        }
        default: // once / expired
            strlcpy(desc, e->date, sizeof(desc));
            for (char *c = desc; *c; c++) if (*c == '-') *c = '/';
            break;
    }
    snprintf(out, outsz, "%s &middot; %s%s%s", e->time, desc,
             e->ntfy ? " &middot; NTFY" : "", e->radar ? " &middot; RADAR" : "");
}

// Sort key inside a group: whatever you'd scan for first in that group.
static void reminderSortKey(const EventRuntime *e, const ReminderPattern *p, char *out, size_t outsz)
{
    switch (p->group) {
        case RG_DAILY:
            snprintf(out, outsz, "%s", e->time);
            break;
        case RG_WEEKLY: {
            int first = 8;
            for (int d = 1; d <= 7; d++) if (p->days[d] && monFirst(d) < first) first = monFirst(d);
            snprintf(out, outsz, "%d%s", first, e->time);
            break;
        }
        case RG_MONTHLY:
            snprintf(out, outsz, "%c%d%s", p->ord == 'F' ? '0' : '1', monFirst(p->wd >= 1 && p->wd <= 7 ? p->wd : 1), e->time);
            break;
        case RG_YEARLY:
            snprintf(out, outsz, "%02d%c%s", p->month, p->ord == 'F' ? '0' : '1', e->time);
            break;
        default:
            snprintf(out, outsz, "%s%s", e->date, e->time);
            break;
    }
}

static esp_err_t sendEventListRow(httpd_req_t *req, EventRuntime *e, const char *reminderBadge)
{
    char badgeText[160] = "";
    if (reminderBadge) {
        strlcpy(badgeText, reminderBadge, sizeof(badgeText));
    } else {
        strlcpy(badgeText, e->date, sizeof(badgeText));
        for (char *p = badgeText; *p; p++) if (*p == '-') *p = '/';
    }

    char vpName[VP_NAME_LEN + 12] = "";
    if (e->valid_period[0] != '\0' && VP_GetName(e->valid_period, vpName + 10, sizeof(vpName) - 10)) {
        memcpy(vpName, " &middot; ", 10);
    }

    bool isBirthdayPast = false;
    if (strcmp(e->type, EVY_TYPE_BIR) == 0 && strlen(e->date) == 10) {
        char today[11];
        GetCurDisplayDate(today, sizeof(today));
        isBirthdayPast = strcmp(today + 5, e->date + 5) > 0; // MM-DD compare
    }
    bool isOneTimeActive = (strcmp(e->type, EVY_TYPE_REM) == 0) && e->one_time && e->active;
    bool isPublicHoliday = strcmp(e->mess, "Public Holiday") == 0;

    const char *cls = "tag tok";
    char styleAttr[56] = "";
    if (!e->active) {
        strlcpy(styleAttr, " style='background:#dc2626'", sizeof(styleAttr));
        cls = "tag";
    } else if (isBirthdayPast) {
        strlcpy(styleAttr, " style='background:#450a0a;color:#fca5a5'", sizeof(styleAttr));
        cls = "tag";
    } else if (isOneTimeActive || isPublicHoliday) {
        // Explicit white text: plain .tag has no color rule of its own, so it
        // was inheriting the page's base text color — near-black in the Light
        // theme, unreadable on this blue background.
        strlcpy(styleAttr, " style='background:#1d4ed8;color:#fff'", sizeof(styleAttr));
        cls = "tag";
    }

    char row[640];
    int n = snprintf(row, sizeof(row),
        "<a href='?f_File=%s'><div class='irow'><dt>%s</dt><dd><span class='%s'%s>%s%s</span></dd></div></a>",
        e->filename, e->label, cls, styleAttr, badgeText, vpName);
    return httpd_resp_send_chunk(req, row, n);
}

// List views hold more than today's-display list does — the Reminders /
// Birthdays lists only grow. Static: too big for a request's stack frame.
#define EVENT_LIST_MAX 64
static EventRuntime s_listScratch[EVENT_LIST_MAX];

typedef struct {
    int  idx;
    int  group;
    char key[24];
} ReminderSlot;
static ReminderSlot s_remSlots[EVENT_LIST_MAX];

static int cmpReminderSlot(const void *a, const void *b)
{
    const ReminderSlot *x = a, *y = b;
    if (x->group != y->group) return x->group - y->group;
    int c = strcmp(x->key, y->key);
    if (c) return c;
    return strcasecmp(s_listScratch[x->idx].label, s_listScratch[y->idx].label);
}

// Search box + foldable groups. Fold state is a per-browser convenience
// (localStorage, guarded — the page works fine without it).
static const char REMINDER_LIST_HEAD[] =
    "<style>"
    ".rgh{cursor:pointer;user-select:none}"
    ".rgh:before{content:'\\25BE  '}"
    ".rgrp.fold .rgh:before{content:'\\25B8  '}"
    ".rgrp.fold .rgb{display:none}"
    ".rgrp.srch .rgb{display:block}"
    ".rgx .irow{opacity:.55}"
    "</style>"
    "<div class='gr'><input type='text' id='rsq' placeholder='&#128269; Search reminders...' oninput='rsF()' "
    "autocomplete='off' style='width:100%;box-sizing:border-box'></div>";

static const char REMINDER_LIST_TAIL[] =
    "<script>"
    "function rgGet(){try{return JSON.parse(localStorage.getItem('remFold')||'[]');}catch(e){return[];}}"
    "function rgSet(a){try{localStorage.setItem('remFold',JSON.stringify(a));}catch(e){}}"
    "function rgT(h){var g=h.parentNode;g.classList.toggle('fold');"
    "var a=[];document.querySelectorAll('.rgrp.fold').forEach(function(x){a.push(x.dataset.g);});rgSet(a);}"
    "function rsF(){var q=document.getElementById('rsq').value.trim().toLowerCase();"
    "document.querySelectorAll('.rgrp').forEach(function(g){var n=0;"
    "g.querySelectorAll('.rgb > a').forEach(function(a){var v=!q||a.textContent.toLowerCase().indexOf(q)>=0;"
    "a.style.display=v?'':'none';if(v)n++;});"
    "g.style.display=(n||!q)?'':'none';g.classList.toggle('srch',!!q);});}"
    "rgGet().forEach(function(k){var g=document.querySelector(\".rgrp[data-g='\"+k+\"']\");if(g)g.classList.add('fold');});"
    "</script>";

// forDelete: same groups/search/badges, but each row is a tap-to-delete link.
static esp_err_t sendReminderGroupedList(httpd_req_t *req, int count, bool forDelete)
{
    esp_err_t err;
    if ((err = httpd_resp_send_chunk(req, REMINDER_LIST_HEAD, sizeof(REMINDER_LIST_HEAD) - 1)) != ESP_OK) return err;

    char today[11];
    GetCurDisplayDate(today, sizeof(today));

    int groupCount[RG_COUNT] = { 0 };
    for (int i = 0; i < count; i++) {
        ReminderPattern p;
        parseReminderPattern(&s_listScratch[i], today, &p);
        s_remSlots[i].idx = i;
        s_remSlots[i].group = p.group;
        reminderSortKey(&s_listScratch[i], &p, s_remSlots[i].key, sizeof(s_remSlots[i].key));
        groupCount[p.group]++;
    }
    qsort(s_remSlots, count, sizeof(ReminderSlot), cmpReminderSlot);

    int cur = -1;
    for (int s = 0; s < count; s++) {
        EventRuntime *e = &s_listScratch[s_remSlots[s].idx];
        int g = s_remSlots[s].group;
        if (g != cur) {
            if (cur >= 0) {
                static const char endGrp[] = "</div></div>";
                if ((err = httpd_resp_send_chunk(req, endGrp, sizeof(endGrp) - 1)) != ESP_OK) return err;
            }
            cur = g;
            char hdr[200];
            int n = snprintf(hdr, sizeof(hdr),
                "<div class='rgrp' data-g='%s'><div class='sec rgh' onclick='rgT(this)'>%s (%d)</div>"
                "<div class='ibox rgb%s'>",
                s_rgKey[g], s_rgTitle[g], groupCount[g], g == RG_EXPIRED ? " rgx" : "");
            if ((err = httpd_resp_send_chunk(req, hdr, n)) != ESP_OK) return err;
        }
        ReminderPattern p;
        parseReminderPattern(e, today, &p);
        char badge[160];
        buildReminderBadge(e, &p, badge, sizeof(badge));
        if (forDelete) {
            char row[480];
            int rn = snprintf(row, sizeof(row),
                "<a href='?f_DelFile=%s'><div class='irow'><dt>%s</dt><dd>"
                "<span style='font-size:11px;opacity:.7;margin-right:10px'>%s</span>"
                "<span style='color:#ef4444'>&#10007;</span></dd></div></a>",
                e->filename, e->label, badge);
            if ((err = httpd_resp_send_chunk(req, row, rn)) != ESP_OK) return err;
        } else if ((err = sendEventListRow(req, e, badge)) != ESP_OK) {
            return err;
        }
    }
    if (cur >= 0) {
        static const char endGrp[] = "</div></div>";
        if ((err = httpd_resp_send_chunk(req, endGrp, sizeof(endGrp) - 1)) != ESP_OK) return err;
    }
    return httpd_resp_send_chunk(req, REMINDER_LIST_TAIL, sizeof(REMINDER_LIST_TAIL) - 1);
}

// Plain search box for the Birthdays / Anniversary lists (no grouping):
// filters the rows of the single .ibox list as you type.
static esp_err_t sendSimpleSearchBox(httpd_req_t *req, const char *what)
{
    char buf[260];
    int n = snprintf(buf, sizeof(buf),
        "<div class='gr'><input type='text' id='ssq' placeholder='&#128269; Search %s...' oninput='ssF()' "
        "autocomplete='off' style='width:100%%;box-sizing:border-box'></div>", what);
    return httpd_resp_send_chunk(req, buf, n);
}

static const char SIMPLE_SEARCH_SCRIPT[] =
    "<script>function ssF(){var q=document.getElementById('ssq').value.trim().toLowerCase();"
    "document.querySelectorAll('.ibox > a').forEach(function(a){"
    "a.style.display=(!q||a.textContent.toLowerCase().indexOf(q)>=0)?'':'none';});}</script>";

static esp_err_t sendListView(httpd_req_t *req, const EventPageConfig *cfg)
{
    esp_err_t err;
    bool isRem = strcmp(cfg->type, EVY_TYPE_REM) == 0;
    char buf[360];
    int n = snprintf(buf, sizeof(buf),
        "<div class='save-row'>"
        "<button class='btn btn-bl' onclick=\"location.href='%s?f_Create=1'\">&#43; Create New %s</button>"
        "<button class='btn btn-rd btn-sm' onclick=\"location.href='%s?f_DelList=1'\">&#10007; Delete List</button>"
        "</div>%s",
        cfg->basePath, cfg->typeLabel, cfg->basePath,
        isRem ? "" : "<div class='sec'>SELECT TO EDIT</div>");
    if ((err = httpd_resp_send_chunk(req, buf, n)) != ESP_OK) return err;
    if (!isRem) {
        if ((err = sendSimpleSearchBox(req, cfg->pageSelect == PS_BIRTHDAYS ? "birthdays" : "anniversaries")) != ESP_OK) return err;
        if ((err = httpd_resp_send_chunk(req, "<div class='ibox'>", 18)) != ESP_OK) return err;
    }

    int count = Event_GetList(cfg->type, "", 0, true, cfg->sortType, s_listScratch, EVENT_LIST_MAX);
    if (isRem) return sendReminderGroupedList(req, count, false);

    for (int i = 0; i < count; i++) {
        if ((err = sendEventListRow(req, &s_listScratch[i], NULL)) != ESP_OK) return err;
    }

    static const char close[] = "</div>";
    if ((err = httpd_resp_send_chunk(req, close, sizeof(close) - 1)) != ESP_OK) return err;
    return httpd_resp_send_chunk(req, SIMPLE_SEARCH_SCRIPT, sizeof(SIMPLE_SEARCH_SCRIPT) - 1);
}

static esp_err_t sendDeleteListView(httpd_req_t *req, const EventPageConfig *cfg)
{
    esp_err_t err;
    bool isRem = strcmp(cfg->type, EVY_TYPE_REM) == 0;
    char buf[300];
    int n = snprintf(buf, sizeof(buf),
        "<div class='save-row'><button class='btn btn-bl' onclick=\"location.href='%s'\">&#8592; Back</button></div>"
        "<div class='sec'>SELECT TO DELETE</div>", cfg->basePath);
    if ((err = httpd_resp_send_chunk(req, buf, n)) != ESP_OK) return err;
    if (!isRem) {
        if ((err = sendSimpleSearchBox(req, cfg->pageSelect == PS_BIRTHDAYS ? "birthdays" : "anniversaries")) != ESP_OK) return err;
        if ((err = httpd_resp_send_chunk(req, "<div class='ibox'>", 18)) != ESP_OK) return err;
    }

    int count = Event_GetList(cfg->type, "", 0, true, cfg->sortType, s_listScratch, EVENT_LIST_MAX);
    if (isRem) return sendReminderGroupedList(req, count, true);

    for (int i = 0; i < count; i++) {
        char row[300];
        int rn = snprintf(row, sizeof(row),
            "<a href='?f_DelFile=%s'><div class='irow'><dt>%s</dt><dd style='color:#ef4444'>&#10007;</dd></div></a>",
            s_listScratch[i].filename, s_listScratch[i].label);
        if ((err = httpd_resp_send_chunk(req, row, rn)) != ESP_OK) return err;
    }

    static const char close[] = "</div>";
    if ((err = httpd_resp_send_chunk(req, close, sizeof(close) - 1)) != ESP_OK) return err;
    return httpd_resp_send_chunk(req, SIMPLE_SEARCH_SCRIPT, sizeof(SIMPLE_SEARCH_SCRIPT) - 1);
}

// =============================================================================
// Main Event Page Handler  (ESP-IDF port of handleBirthdays/Anniversary/Reminders)
// =============================================================================

static esp_err_t genericEventPageHandler(httpd_req_t *req, const EventPageConfig *cfg)
{
    char query[200] = "";
    httpd_req_get_url_query_str(req, query, sizeof(query));

    char delFile[64] = "";
    bool hasDelFile = httpd_query_key_value(query, "f_DelFile", delFile, sizeof(delFile)) == ESP_OK && delFile[0] != '\0';
    bool hasDelList = strstr(query, "f_DelList") != NULL;
    bool hasCreate  = strstr(query, "f_Create") != NULL;
    char fileArg[64] = "";
    bool hasFile    = httpd_query_key_value(query, "f_File", fileArg, sizeof(fileArg)) == ESP_OK && fileArg[0] != '\0';

    if (hasDelFile) {
        char path[100];
        snprintf(path, sizeof(path), "%s%s", SPIFFS_BASE_PATH, delFile);
        remove(path);
        Event_RefreshTodayList();
        enqueueSyncDelete(delFile);
        WS_MessUpdate("Event Deleted!", "color: red", false);
        vTaskDelay(pdMS_TO_TICKS(1000)); // lets the WS message land before the list re-renders, matches ARD
    }

    esp_err_t err;
    if ((err = WebCore_SendShellOpen(req, cfg->typeLabel)) != ESP_OK) return err;
    if ((err = WebCore_SendHeader(req, FA_MESS_06)) != ESP_OK) return err;
    if ((err = WebCore_SendNav(req, cfg->pageSelect)) != ESP_OK) return err;
    if ((err = WebCore_SendScript(req)) != ESP_OK) return err;

    if (hasDelFile || hasDelList) {
        if ((err = sendDeleteListView(req, cfg)) != ESP_OK) return err;
    } else if (hasCreate) {
        applyCreateDefaults(&s_editEvent, cfg->type);
        s_editFileName[0] = '\0'; // signals Create, not Update
        if ((err = sendEventForm(req, &s_editEvent)) != ESP_OK) return err;
        if ((err = WebCore_SendUpdateButtonNav(req, "SAVE", cfg->basePath)) != ESP_OK) return err;
        return WebCore_SendShellClose(req);
    } else if (hasFile) {
        char path[100];
        snprintf(path, sizeof(path), "%s%s", SPIFFS_BASE_PATH, fileArg);
        // Files written before Valid Periods existed have no S_EVT_VALID_PER
        // key, and Event_ReadData leaves absent keys untouched — reset it so a
        // previous edit's value can't leak in.
        strlcpy(s_editEvent.fields[S_EVT_VALID_PER].value, " ", EVENT_VALUE_LEN);
        if (Event_ReadData(path, &s_editEvent)) {
            strlcpy(s_editFileName, fileArg, sizeof(s_editFileName));
        } else {
            s_editFileName[0] = '\0';
        }
        if ((err = sendEventForm(req, &s_editEvent)) != ESP_OK) return err;
        if ((err = WebCore_SendUpdateButtonNav(req, "UPDATE", cfg->basePath)) != ESP_OK) return err;
        return WebCore_SendShellClose(req);
    } else {
        if ((err = sendListView(req, cfg)) != ESP_OK) return err;
    }

    return WebCore_SendShellClose(req);
}

static esp_err_t birthdays_handler(httpd_req_t *req)
{
    static const EventPageConfig cfg = { EVY_TYPE_BIR, "/birthdays", "Birthday", PS_BIRTHDAYS, SORT_BY_MONTH };
    return genericEventPageHandler(req, &cfg);
}
static esp_err_t anniversary_handler(httpd_req_t *req)
{
    static const EventPageConfig cfg = { EVY_TYPE_ANN, "/anniversary", "Anniversary", PS_ANNIVERSARY, SORT_BY_ANN };
    return genericEventPageHandler(req, &cfg);
}
static esp_err_t reminders_handler(httpd_req_t *req)
{
    static const EventPageConfig cfg = { EVY_TYPE_REM, "/reminders", "Reminder", PS_REMINDERS, SORT_BY_LABLE };
    return genericEventPageHandler(req, &cfg);
}

// =============================================================================
// Quick Page  (ESP-IDF port of handleQuick)
// =============================================================================

// Indirected through out+outsz params (rather than snprintf'ing straight into
// a local fixed array at the call site) so GCC's -Wformat-truncation can't
// "prove" a theoretical overflow from tm_year/tm_mon's full int range —
// same pattern datetime.c's Get*() helpers already use for this reason.
static void formatDate(const struct tm *ti, char *out, size_t outsz)
{
    snprintf(out, outsz, "%04d-%02d-%02d", 1900 + ti->tm_year, ti->tm_mon + 1, ti->tm_mday);
}
static void formatTime(const struct tm *ti, char *out, size_t outsz)
{
    snprintf(out, outsz, "%02d:%02d", ti->tm_hour, ti->tm_min);
}

static esp_err_t quick_handler(httpd_req_t *req)
{
    // Always start from a clean slate — unlike the birthday/anniversary/reminder
    // pages, Quick never sets s_editFileName itself; clearing it here prevents a
    // stale value from an earlier edit-page visit causing UpdateEventPageValues
    // to delete the wrong file on save.
    s_editFileName[0] = '\0';

    esp_err_t err;
    if ((err = WebCore_SendShellOpen(req, "LED Clock - Quick")) != ESP_OK) return err;
    if ((err = WebCore_SendHeader(req, FA_MESS_06)) != ESP_OK) return err;
    if ((err = WebCore_SendNav(req, PS_QUICK)) != ESP_OK) return err;
    if ((err = WebCore_SendScript(req)) != ESP_OK) return err;

    // Default Date/Time to 5 minutes from now (not just-now) — computed from a
    // single future timestamp so a rollover past midnight bumps the date and
    // time together consistently, rather than "today" + "current HH:MM" going
    // out of sync near midnight.
    time_t future = time(NULL) + 5 * 60;
    struct tm ti;
    localtime_r(&future, &ti);
    char today[16];
    formatDate(&ti, today, sizeof(today));
    char defTime[8];
    formatTime(&ti, defTime, sizeof(defTime));

    char buf[2800];
    int n = snprintf(buf, sizeof(buf),
        "<div class='sec'>QUICK REMINDER</div>"
        "<div class='gr'><span class='lbl'>Label</span><input type='text' id='%d'></div>"
        "<div class='gr'><span class='lbl'>Date</span><input type='date' id='%d' value='%s'></div>"
        "<div class='gr'><span class='lbl'>Time</span><input type='time' id='%d' value='%s'></div>"
        "<div class='gr'><span class='lbl'>Adjust Time</span>"
        "<span style='display:flex;gap:6px;align-items:center;flex:0 0 auto'>"
        "<input type='number' id='qmin' value='40' min='1' style='max-width:80px'>"
        "<span>min</span>"
        "<button type='button' class='btn btn-bl btn-sm' onclick='setOffset()'>Set</button>"
        "</span></div>"
        "<div class='gr'><span class='lbl'>Text Color</span><select id='%d'>"
        "<option value='" ECO_WHITE  "'>"          ECO_WHITE  "</option>"
        "<option value='" ECO_YELLOW "' selected>" ECO_YELLOW "</option>"
        "<option value='" ECO_GREEN  "'>"          ECO_GREEN  "</option>"
        "<option value='" ECO_RED    "'>"          ECO_RED    "</option>"
        "<option value='" ECO_BLUE   "'>"          ECO_BLUE   "</option>"
        "</select></div>"
        "<div class='gr'><span class='lbl'>Buzzer Sound</span><select id='%d'>"
        "<option value='" BUZ_NONE    "'>"          BUZ_NONE    "</option>"
        "<option value='" BUZ_SHORT   "' selected>" BUZ_SHORT   "</option>"
        "<option value='" BUZ_LONG    "'>"          BUZ_LONG    "</option>"
        "<option value='" BUZ_CONT_SH "'>"          BUZ_CONT_SH "</option>"
        "<option value='" BUZ_CONT_LO "'>"          BUZ_CONT_LO "</option>"
        "</select></div>"
        "<div class='gr'><span class='lbl'>Repeat Times</span>"
        "<input type='number' id='%d' value='2' min='0' style='max-width:80px'></div>"
        "<div class='gr'><span class='lbl'>Detect Human</span><input type='checkbox' id='%d'></div>"
        "<div class='gr'><span class='lbl'>Send NTFY</span><div style='display:flex;gap:6px'>"
        "<select id='%d'><option value='NO_NTFY' selected>NO_NTFY</option><option value='BOTH_NTFYS'>BOTH_NTFYS</option>"
        "<option value='%s'>%s</option><option value='%s'>%s</option></select>"
        "<select id='%d'><option value='min'>min</option><option value='low'>low</option>"
        "<option value='default'>default</option><option value='high'>high</option>"
        "<option value='urgent' selected>urgent</option></select>"
        "<input type='checkbox' id='%d'></div></div>"
        "<div class='gr'><span class='lbl'>One Time</span><input type='checkbox' id='%d' checked></div>"
        "<input type='hidden' id='%d' value=' '>"
        "<input type='hidden' id='%d' value='false'>"
        "<input type='hidden' id='%d' value=' '>"
        "<input type='hidden' id='%d' value='true'>"
        "<input type='hidden' id='%d' value='1'>"
        "<input type='hidden' id='%d' value='Reminder'>",
        ID_EVENTS + S_EVT_LABLE,
        ID_EVENTS + S_EVT_DATE, today,
        ID_EVENTS + S_EVT_TIME, defTime,
        ID_EVENTS + S_EVT_COLOR,
        ID_EVENTS + S_EVT_BUZZ,
        ID_EVENTS + S_EVT_REPEAT,
        ID_EVENTS + S_EVT_RADAR,
        ID_EVENTS + S_EVT_NTFY_TOPIC, g_ntfyTopic1, g_ntfyTopic1, g_ntfyTopic2, g_ntfyTopic2,
        ID_EVENTS + S_EVT_NTFY_PRIO,
        ID_EVENTS + S_EVT_NTFY,
        ID_EVENTS + S_EVT_ONETIME,
        ID_EVENTS + S_EVT_MESSAGE,
        ID_EVENTS + S_EVT_SHOW_AGE,
        ID_EVENTS + S_EVT_WEEKDAYS,
        ID_EVENTS + S_EVT_ACTIVE,
        ID_EVENTS + S_EVT_VERSION,
        ID_EVENTS + S_EVT_TYPE);
    if ((err = httpd_resp_send_chunk(req, buf, n)) != ESP_OK) return err;

    // s_editEvent is shared and mutated in place on save, so explicitly send
    // "None" — otherwise a Valid Period left over from an earlier edit would
    // ride along into the Quick reminder.
    n = snprintf(buf, sizeof(buf), "<input type='hidden' id='%d' value=' '>", ID_EVENTS + S_EVT_VALID_PER);
    if ((err = httpd_resp_send_chunk(req, buf, n)) != ESP_OK) return err;

    char script[1600];
    n = snprintf(script, sizeof(script),
        "<script>"
        "function setOffset(){"
        "var m=parseInt(document.getElementById('qmin').value,10);"
        "if(!m||m<1){alert('Enter how many minutes from now.');return;}"
        "var x=new Date(Date.now()+m*60000);"
        "var p=function(v){return(v<10?'0':'')+v;};"
        "document.getElementById('%d').value=x.getFullYear()+'-'+p(x.getMonth()+1)+'-'+p(x.getDate());"
        "document.getElementById('%d').value=p(x.getHours())+':'+p(x.getMinutes());"
        "}"
        "function validateAndSave(){"
        "var lbl=document.getElementById('%d').value.trim();"
        "if(!lbl){alert('Please enter a Label before saving.');return;}"
        "var d=document.getElementById('%d').value;var t=document.getElementById('%d').value;"
        "if(!d||!t){alert('Please enter a Date and Time before saving.');return;}"
        "var target=new Date(d+'T'+t);var now=new Date();"
        "if((target-now)<120000){alert('Date & Time must be at least 2 minutes in the future.');return;}"
        "UpdatePage();setTimeout(function(){window.location.href='/';},1500);"
        "}"
        "</script>"
        "<div class='save-row'><button class='btn btn-bl' onclick='validateAndSave()'>&#10003; SAVE</button></div>",
        ID_EVENTS + S_EVT_DATE, ID_EVENTS + S_EVT_TIME,
        ID_EVENTS + S_EVT_LABLE, ID_EVENTS + S_EVT_DATE, ID_EVENTS + S_EVT_TIME);
    if ((err = httpd_resp_send_chunk(req, script, n)) != ESP_OK) return err;

    return WebCore_SendShellClose(req);
}

// =============================================================================
// WebSocket Update Handler  (ESP-IDF port of UpdateEventPageValues)
// =============================================================================

void UpdateEventPageValues(cJSON *root)
{
    cJSON *kv = NULL;
    cJSON_ArrayForEach(kv, root) {
        if (!isAllDigits(kv->string)) continue;
        int i = atoi(kv->string) - ID_EVENTS;
        if (i < 0 || i >= EVENT_ENTRIES) continue;

        char value[EVENT_VALUE_LEN];
        if (cJSON_IsString(kv)) {
            strlcpy(value, kv->valuestring, sizeof(value));
        } else if (cJSON_IsBool(kv)) {
            strlcpy(value, cJSON_IsTrue(kv) ? "true" : "false", sizeof(value));
        } else if (cJSON_IsNumber(kv)) {
            snprintf(value, sizeof(value), "%g", kv->valuedouble);
        } else {
            value[0] = '\0';
        }
        trimInPlace(value);
        strlcpy(s_editEvent.fields[i].value, value[0] != '\0' ? value : " ", EVENT_VALUE_LEN);
    }

    if (s_editFileName[0] != '\0') {
        char path[100];
        snprintf(path, sizeof(path), "%s%s", SPIFFS_BASE_PATH, s_editFileName);
        remove(path);
    }

    char savedPath[100];
    Event_GetFilename(&s_editEvent, true, savedPath, sizeof(savedPath)); // "/spiffs/Birthday_1.json"
    Event_WriteData(&s_editEvent);

    WS_MessUpdate("Events Data Updated!", "color: red", false);
    Event_RefreshTodayList();
    enqueueSyncSave(savedPath + strlen(SPIFFS_BASE_PATH)); // peers expect a root-relative path, e.g. "/Birthday_1.json"
}

// =============================================================================
// CSV Export / Import  (ESP-IDF port of createCSVFileFiltered / readCSVFileFiltered
// / deleteEventsByType in ARD's 03_Files.cpp). ARD's "old format" (30/31-column)
// backward-compat branch is dropped — that only mattered for files exported
// before S_EVT_NTFY_TOPIC existed, which this fresh port never produces.
// =============================================================================

static void createCSVFiltered(const char *csvPath, const char *typeFilter)
{
    char fullPath[100];
    snprintf(fullPath, sizeof(fullPath), "%s%s", SPIFFS_BASE_PATH, csvPath);
    FILE *csv = fopen(fullPath, "w");
    if (!csv) return;

    fputs("LABLE,MESSAGE,DATE,TIME,SHOW_AGE,WEEKDAYS,COLOR,BUZZ,REPEAT,RADAR,NTFY,ACTIVE,VERSION,TYPE,NTFY_PRIO,ONETIME,NTFY_TOPIC,DISABLED,TIME_OVR,VALID_PER,"
          "D_LABLE,D_MESSAGE,D_DATE,D_TIME,D_SHOW_AGE,D_WEEKDAYS,D_COLOR,D_BUZZ,D_REPEAT,D_RADAR,D_NTFY,D_ACTIVE,D_VERSION,D_TYPE,D_NTFY_PRIO,D_ONETIME,D_NTFY_TOPIC,D_DISABLED,D_TIME_OVR,D_VALID_PER\n",
          csv);

    DIR *dir = opendir(SPIFFS_BASE_PATH);
    if (!dir) { fclose(csv); return; }
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        if (nlen < 6 || strcmp(ent->d_name + nlen - 5, ".json") != 0) continue;
        char fpath[300];
        snprintf(fpath, sizeof(fpath), "%s/%s", SPIFFS_BASE_PATH, ent->d_name);

        EventRecord tmp;
        Event_InitDefaults(&tmp);
        if (!Event_ReadData(fpath, &tmp)) continue;
        if (typeFilter[0] != '\0' && strcmp(tmp.fields[S_EVT_TYPE].value, typeFilter) != 0) continue;

        for (int i = 0; i < EVENT_ENTRIES; i++) { fputs(tmp.fields[i].value, csv); fputc(',', csv); }
        for (int i = 0; i < EVENT_ENTRIES; i++) {
            fputs(tmp.fields[i].web_display ? "true" : "false", csv);
            if (i < EVENT_ENTRIES - 1) fputc(',', csv);
        }
        fputc('\n', csv);
    }
    closedir(dir);
    fclose(csv);
}

static void readCSVFiltered(const char *csvPath, const char *typeFilter)
{
    char fullPath[100];
    snprintf(fullPath, sizeof(fullPath), "%s%s", SPIFFS_BASE_PATH, csvPath);
    FILE *csv = fopen(fullPath, "r");
    if (!csv) return;

    char header[900];
    if (!fgets(header, sizeof(header), csv)) { fclose(csv); return; }

    char line[900];
    while (fgets(line, sizeof(line), csv)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
        if (len == 0) continue;

        char *fields[2 * EVENT_ENTRIES + 4];
        const int maxFields = (int)(sizeof(fields) / sizeof(fields[0]));
        int i = 0;
        char *p = line;
        for (i = 0; i < maxFields; i++) {
            fields[i] = p;
            char *comma = strchr(p, ',');
            if (!comma) break;
            *comma = '\0';
            p = comma + 1;
        }
        int nFields = (i < maxFields) ? i + 1 : maxFields;

        // Column count per row = 2 x fields-in-that-format. Current format has
        // EVENT_ENTRIES fields; files exported before S_EVT_VALID_PER existed
        // have one fewer (those reminders import with Valid Period = None).
        int nEntries;
        if (nFields >= 2 * EVENT_ENTRIES)            nEntries = EVENT_ENTRIES;
        else if (nFields >= 2 * (EVENT_ENTRIES - 1)) nEntries = EVENT_ENTRIES - 1;
        else continue; // need Value + WEBDisplay columns for every field

        const char *eventType = fields[S_EVT_TYPE];
        if (typeFilter[0] != '\0' && strcmp(eventType, typeFilter) != 0) continue;

        EventRecord tmp;
        Event_InitDefaults(&tmp);
        for (int j = 0; j < nEntries; j++) {
            strlcpy(tmp.fields[j].value, fields[j], EVENT_VALUE_LEN);
            if (tmp.fields[j].type == FIELD_DATE) {
                for (char *c = tmp.fields[j].value; *c; c++) if (*c == '/') *c = '-';
            }
            tmp.fields[j].web_display = (strcasecmp(fields[nEntries + j], "true") == 0);
        }
        Event_WriteData(&tmp);
    }
    fclose(csv);
}

static void deleteEventsByType(const char *typeFilter)
{
    DIR *dir = opendir(SPIFFS_BASE_PATH);
    if (!dir) return;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        if (nlen < 6 || strcmp(ent->d_name + nlen - 5, ".json") != 0) continue;
        char fpath[300];
        snprintf(fpath, sizeof(fpath), "%s/%s", SPIFFS_BASE_PATH, ent->d_name);
        EventRecord tmp;
        Event_InitDefaults(&tmp);
        if (!Event_ReadData(fpath, &tmp)) continue;
        if (typeFilter[0] == '\0' || strcmp(tmp.fields[S_EVT_TYPE].value, typeFilter) == 0) {
            remove(fpath);
        }
    }
    closedir(dir);
}

static esp_err_t redirectOther(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/other");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// Imports replace every event of that type locally, then the same set is
// replicated to the peer clocks (enqueueSyncReplace) so all clocks match.
static esp_err_t createCSVBirthday_handler(httpd_req_t *req)
{ createCSVFiltered("/BIRTHDAY_EVENTS.CSV", EVY_TYPE_BIR); return redirectOther(req); }
static esp_err_t loadCSVBirthday_handler(httpd_req_t *req)
{ deleteEventsByType(EVY_TYPE_BIR); readCSVFiltered("/BIRTHDAY_EVENTS.CSV", EVY_TYPE_BIR); Event_RefreshTodayList(); enqueueSyncReplace(EVY_TYPE_BIR); return redirectOther(req); }
static esp_err_t createCSVAnniversary_handler(httpd_req_t *req)
{ createCSVFiltered("/ANNIVERSARY_EVENTS.CSV", EVY_TYPE_ANN); return redirectOther(req); }
static esp_err_t loadCSVAnniversary_handler(httpd_req_t *req)
{ deleteEventsByType(EVY_TYPE_ANN); readCSVFiltered("/ANNIVERSARY_EVENTS.CSV", EVY_TYPE_ANN); Event_RefreshTodayList(); enqueueSyncReplace(EVY_TYPE_ANN); return redirectOther(req); }
static esp_err_t createCSVReminder_handler(httpd_req_t *req)
{ createCSVFiltered("/REMINDER_EVENTS.CSV", EVY_TYPE_REM); return redirectOther(req); }
static esp_err_t loadCSVReminder_handler(httpd_req_t *req)
{ deleteEventsByType(EVY_TYPE_REM); readCSVFiltered("/REMINDER_EVENTS.CSV", EVY_TYPE_REM); Event_RefreshTodayList(); enqueueSyncReplace(EVY_TYPE_REM); return redirectOther(req); }

// GET /createCSV and /loadfromCSV — the type-agnostic ALL_EVENTS.CSV variant.
// ARD renders /files inline instead of redirecting; a redirect is functionally
// identical to the user (same page, one extra round trip) and keeps this
// handler consistent with the type-filtered CSV routes above.
static esp_err_t redirectFiles(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/files");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}
static esp_err_t createCSVAll_handler(httpd_req_t *req)
{ createCSVFiltered("/ALL_EVENTS.CSV", ""); return redirectFiles(req); }
static esp_err_t loadCSVAll_handler(httpd_req_t *req)
{ deleteEventsByType(""); readCSVFiltered("/ALL_EVENTS.CSV", ""); Event_RefreshTodayList(); enqueueSyncReplace(SYNC_TAG_ALL_EVENTS); return redirectFiles(req); }

// =============================================================================
// Peer Sync Receivers  (ESP-IDF port of handleReceiveEvent / handleDeleteEvent)
// =============================================================================

static bool getFileArg(httpd_req_t *req, char *out, size_t outsz)
{
    char query[128] = "";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return false;
    char raw[80];
    if (httpd_query_key_value(query, "f", raw, sizeof(raw)) != ESP_OK || raw[0] == '\0') return false;
    if (raw[0] == '/') {
        strlcpy(out, raw, outsz);
    } else {
        snprintf(out, outsz, "/%s", raw);
    }
    return true;
}

// POST /receiveevent?f=/Birthday_1.json  body: raw JSON
// Writes a pushed event file to SPIFFS and reschedules today's events.
// Enforces this clock's NTFY policy on the incoming topic: a secondary clock
// (g_ntfyCode == NO_NTFY) stamps every received event NO_NTFY so it can never
// itself send a push; a master clock restores its own topic (or whatever this
// file already had locally) if the incoming copy was NO_NTFY-stamped by a
// secondary, so a catch-up sync after this clock's reboot can't silently wipe
// its NTFY configuration.
static esp_err_t receiveevent_handler(httpd_req_t *req)
{
    char path[64];
    if (!getFileArg(req, path, sizeof(path))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing f");
        return ESP_FAIL;
    }
    size_t plen = strlen(path);
    if (plen < 6 || strcmp(path + plen - 5, ".json") != 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path");
        return ESP_FAIL;
    }

    int total = req->content_len;
    if (total <= 0 || total > 4096) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad body");
        return ESP_FAIL;
    }
    char *body = malloc(total + 1);
    if (!body) { httpd_resp_send_500(req); return ESP_FAIL; }
    int received = 0;
    while (received < total) {
        int r = httpd_req_recv(req, body + received, total - received);
        if (r <= 0) { free(body); return ESP_FAIL; }
        received += r;
    }
    body[total] = '\0';

    cJSON *doc = cJSON_Parse(body);
    // The NTFY policy below only applies to event files — Valid Period files
    // (/ValidPeriod_<n>.json) arrive through this same route and are stored as-is.
    if (doc && !cJSON_GetObjectItemCaseSensitive(doc, "S_EVT_TYPE")) {
        cJSON_Delete(doc);
        doc = NULL;
    }
    if (doc) {
        cJSON *topicObj = cJSON_GetObjectItemCaseSensitive(doc, "S_EVT_NTFY_TOPIC");
        if (strcmp(g_ntfyCode, "NO_NTFY") == 0) {
            if (!topicObj) {
                topicObj = cJSON_AddObjectToObject(doc, "S_EVT_NTFY_TOPIC");
                cJSON_AddBoolToObject(topicObj, "WEBDisplay", false);
            }
            cJSON *val = cJSON_GetObjectItemCaseSensitive(topicObj, "Value");
            if (val && cJSON_IsString(val)) cJSON_SetValuestring(val, "NO_NTFY");
            else cJSON_AddStringToObject(topicObj, "Value", "NO_NTFY");
        } else {
            cJSON *val = topicObj ? cJSON_GetObjectItemCaseSensitive(topicObj, "Value") : NULL;
            if (val && cJSON_IsString(val) && strcmp(val->valuestring, "NO_NTFY") == 0) {
                char restoreTo[NTFY_TOPIC_LEN];
                strlcpy(restoreTo, g_ntfyCode, sizeof(restoreTo));

                char fullPath[100];
                snprintf(fullPath, sizeof(fullPath), "%s%s", SPIFFS_BASE_PATH, path);
                FILE *ef = fopen(fullPath, "r");
                if (ef) {
                    char exBuf[2048];
                    size_t n = fread(exBuf, 1, sizeof(exBuf) - 1, ef);
                    exBuf[n] = '\0';
                    fclose(ef);
                    cJSON *exDoc = cJSON_Parse(exBuf);
                    if (exDoc) {
                        cJSON *exTopic = cJSON_GetObjectItemCaseSensitive(exDoc, "S_EVT_NTFY_TOPIC");
                        cJSON *exVal = exTopic ? cJSON_GetObjectItemCaseSensitive(exTopic, "Value") : NULL;
                        if (exVal && cJSON_IsString(exVal) && exVal->valuestring[0] != '\0') {
                            strlcpy(restoreTo, exVal->valuestring, sizeof(restoreTo));
                        }
                        cJSON_Delete(exDoc);
                    }
                }
                cJSON_SetValuestring(val, restoreTo);
            }
        }
        free(body);
        body = cJSON_Print(doc);
        cJSON_Delete(doc);
    }

    char fullPath[100];
    snprintf(fullPath, sizeof(fullPath), "%s%s", SPIFFS_BASE_PATH, path);
    FILE *f = body ? fopen(fullPath, "w") : NULL;
    if (!f) {
        free(body);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Write failed");
        return ESP_FAIL;
    }
    fputs(body, f);
    fclose(f);
    free(body);

    Event_RefreshTodayList();
    WS_Broadcast("{\"type\":\"reload_home\"}");

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

// GET /deletetype?t=<kind> — peer side of enqueueSyncReplace(): delete every
// local file of that kind before the sender pushes its own set.
static esp_err_t deletetype_handler(httpd_req_t *req)
{
    char query[64] = "";
    char kind[24] = "";
    httpd_req_get_url_query_str(req, query, sizeof(query));
    httpd_query_key_value(query, "t", kind, sizeof(kind));

    if (strcmp(kind, SYNC_TAG_VALID_PERIODS) == 0) {
        DIR *dir = opendir(SPIFFS_BASE_PATH);
        if (dir) {
            struct dirent *ent;
            while ((ent = readdir(dir)) != NULL) {
                if (strncmp(ent->d_name, VP_FILE_PREFIX, strlen(VP_FILE_PREFIX)) != 0) continue;
                char fpath[300];
                snprintf(fpath, sizeof(fpath), "%s/%s", SPIFFS_BASE_PATH, ent->d_name);
                remove(fpath);
            }
            closedir(dir);
        }
    } else if (strcmp(kind, SYNC_TAG_ALL_EVENTS) == 0) {
        deleteEventsByType("");
    } else if (strcmp(kind, EVY_TYPE_BIR) == 0 || strcmp(kind, EVY_TYPE_ANN) == 0 ||
               strcmp(kind, EVY_TYPE_REM) == 0) {
        deleteEventsByType(kind);
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Unknown kind");
        return ESP_FAIL;
    }
    Event_RefreshTodayList();

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

// GET /deleteevent?f=/Birthday_1.json
static esp_err_t deleteevent_handler(httpd_req_t *req)
{
    char path[64];
    if (!getFileArg(req, path, sizeof(path))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing f");
        return ESP_FAIL;
    }

    char fullPath[100];
    snprintf(fullPath, sizeof(fullPath), "%s%s", SPIFFS_BASE_PATH, path);
    remove(fullPath);
    Event_RefreshTodayList();

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

// =============================================================================
// GET /toggledisable?f=/Birthday_1.json — Home page tap-to-disable/re-enable
// =============================================================================
// A PENDING event flips to DISABLED (silently skips the LED display when it
// fires) or back. Rejected once the event has already fired (FIRED events are
// immutable). Reuses the same save+sync path as a normal edit so all peer
// clocks pick up the change.
static esp_err_t toggledisable_handler(httpd_req_t *req)
{
    char path[64];
    if (!getFileArg(req, path, sizeof(path))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing f");
        return ESP_FAIL;
    }

    EventRuntime *match = NULL;
    for (int i = 0; i < g_eventsDisplayCount; i++) {
        if (strcmp(g_eventsDisplay[i].filename, path) == 0) { match = &g_eventsDisplay[i]; break; }
    }
    if (!match) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_FAIL;
    }
    if (match->done) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Already fired");
        return ESP_FAIL;
    }

    char fullPath[100];
    snprintf(fullPath, sizeof(fullPath), "%s%s", SPIFFS_BASE_PATH, path);
    EventRecord rec;
    Event_InitDefaults(&rec);
    if (!Event_ReadData(fullPath, &rec)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_FAIL;
    }

    bool nowDisabled = strcmp(rec.fields[S_EVT_DISABLED].value, "true") != 0;
    strlcpy(rec.fields[S_EVT_DISABLED].value, nowDisabled ? "true" : "false", EVENT_VALUE_LEN);
    // Event_WriteData()->Event_GetFilename() auto-increments past any filename
    // that already exists (its Create-flow collision-avoidance) — remove the
    // original first so it lands back on the same version/name instead of a
    // fresh duplicate file (matches UpdateEventPageValues's pattern).
    remove(fullPath);
    Event_WriteData(&rec);

    Event_RefreshTodayList();
    enqueueSyncSave(path);
    WS_Broadcast("{\"type\":\"reload_home\"}");

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

// =============================================================================
// GET /changetime?f=/Birthday_1.json&t=HH:MM — Home page tap-the-TIME-cell edit
// =============================================================================
// Same-day-only, like /toggledisable: writes S_EVT_TIME_OVR rather than the
// permanent S_EVT_TIME baseline, so today's occurrence fires at the new time
// but future occurrences fall back to the original (AppLogicTask clears the
// override once it's consumed). Rejected once the event has already fired.
static esp_err_t changetime_handler(httpd_req_t *req)
{
    char query[128] = "";
    httpd_req_get_url_query_str(req, query, sizeof(query));

    char path[64];
    if (!getFileArg(req, path, sizeof(path))) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing f");
        return ESP_FAIL;
    }

    char newTime[8] = "";
    httpd_query_key_value(query, "t", newTime, sizeof(newTime));
    if (strlen(newTime) != 5 || newTime[2] != ':' ||
        newTime[0] < '0' || newTime[0] > '9' || newTime[1] < '0' || newTime[1] > '9' ||
        newTime[3] < '0' || newTime[3] > '9' || newTime[4] < '0' || newTime[4] > '9') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad time");
        return ESP_FAIL;
    }
    int hh = (newTime[0] - '0') * 10 + (newTime[1] - '0');
    int mm = (newTime[3] - '0') * 10 + (newTime[4] - '0');
    if (hh > 23 || mm > 59) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad time");
        return ESP_FAIL;
    }

    // A same-day override already in the past would flip to done=true the
    // instant Event_RefreshTodayList() runs below — before AppLogicTask's
    // firing loop ever sees it as newly-due — so the override would never
    // get cleared and would silently "fire" without ever actually reaching
    // the display/NTFY. Reject up front instead of creating that stuck state.
    char checkTime[6];
    GetCurrentTime(checkTime, sizeof(checkTime));
    if (strcmp(newTime, checkTime) <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Time must be in the future");
        return ESP_FAIL;
    }

    EventRuntime *match = NULL;
    for (int i = 0; i < g_eventsDisplayCount; i++) {
        if (strcmp(g_eventsDisplay[i].filename, path) == 0) { match = &g_eventsDisplay[i]; break; }
    }
    if (!match) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_FAIL;
    }
    if (match->done) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Already fired");
        return ESP_FAIL;
    }

    char fullPath[100];
    snprintf(fullPath, sizeof(fullPath), "%s%s", SPIFFS_BASE_PATH, path);
    EventRecord rec;
    Event_InitDefaults(&rec);
    if (!Event_ReadData(fullPath, &rec)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
        return ESP_FAIL;
    }

    strlcpy(rec.fields[S_EVT_TIME_OVR].value, newTime, EVENT_VALUE_LEN);
    // See the matching comment in toggledisable_handler — must delete the
    // original file first or Event_WriteData() writes a new duplicate.
    remove(fullPath);
    Event_WriteData(&rec);

    Event_RefreshTodayList();
    enqueueSyncSave(path);
    WS_Broadcast("{\"type\":\"reload_home\"}");

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

// =============================================================================
// GET /resetoverrides — Home page "RESET TIME/STATUS" button
// =============================================================================
// Clears both same-day overrides (DISABLED and the tap-the-TIME-cell change)
// on every one of today's events in one pass, reverting them to their
// permanent baseline (original time, not disabled). Deliberately NOT
// restricted to not-yet-fired events like /toggledisable and /changetime are
// — this button's whole purpose is to also recover an event that got stuck
// showing FIRED-but-never-actually-fired because its override pointed at a
// time already in the past (see the /changetime past-time guard). Clearing
// the override here doesn't retroactively fire anything; it just lets
// Event_RefreshTodayList() re-derive `done` from the real baseline, so if
// that baseline is still in the future the event correctly drops back to
// PENDING instead of staying permanently stuck.
static esp_err_t resetoverrides_handler(httpd_req_t *req)
{
    int changed = 0;
    for (int i = 0; i < g_eventsDisplayCount; i++) {
        EventRuntime *e = &g_eventsDisplay[i];
        if (!e->disabled && !e->time_overridden) continue;

        char fullPath[100];
        snprintf(fullPath, sizeof(fullPath), "%s%s", SPIFFS_BASE_PATH, e->filename);
        EventRecord rec;
        Event_InitDefaults(&rec);
        if (!Event_ReadData(fullPath, &rec)) continue;

        strlcpy(rec.fields[S_EVT_DISABLED].value, "false", EVENT_VALUE_LEN);
        strlcpy(rec.fields[S_EVT_TIME_OVR].value, "", EVENT_VALUE_LEN);

        // See the matching comment in toggledisable_handler — must delete the
        // original file first or Event_WriteData() writes a new duplicate.
        remove(fullPath);
        Event_WriteData(&rec);
        enqueueSyncSave(e->filename);
        changed++;
    }

    Event_RefreshTodayList();
    if (changed > 0) WS_Broadcast("{\"type\":\"reload_home\"}");

    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
}

// =============================================================================
// Public API
// =============================================================================

void WebEvents_RegisterRoutes(httpd_handle_t server)
{
    // One-time init — establishes s_editEvent.fields[i].index (the cJSON key
    // each field maps to), which Event_ReadData/Event_WriteData rely on and
    // which nothing else ever touches afterward, so this never needs repeating.
    Event_InitDefaults(&s_editEvent);

    static const httpd_uri_t birthdays_uri    = { .uri = "/birthdays",   .method = HTTP_GET, .handler = birthdays_handler };
    static const httpd_uri_t anniversary_uri  = { .uri = "/anniversary", .method = HTTP_GET, .handler = anniversary_handler };
    static const httpd_uri_t reminders_uri    = { .uri = "/reminders",   .method = HTTP_GET, .handler = reminders_handler };
    static const httpd_uri_t quick_uri        = { .uri = "/quick",       .method = HTTP_GET, .handler = quick_handler };
    static const httpd_uri_t csv_bir_out_uri  = { .uri = "/createCSVBirthday",       .method = HTTP_GET, .handler = createCSVBirthday_handler };
    static const httpd_uri_t csv_bir_in_uri   = { .uri = "/loadFromCSVBirthday",     .method = HTTP_GET, .handler = loadCSVBirthday_handler };
    static const httpd_uri_t csv_ann_out_uri  = { .uri = "/createCSVAnniversary",    .method = HTTP_GET, .handler = createCSVAnniversary_handler };
    static const httpd_uri_t csv_ann_in_uri   = { .uri = "/loadFromCSVAnniversary",  .method = HTTP_GET, .handler = loadCSVAnniversary_handler };
    static const httpd_uri_t csv_rem_out_uri  = { .uri = "/createCSVReminder",       .method = HTTP_GET, .handler = createCSVReminder_handler };
    static const httpd_uri_t csv_rem_in_uri   = { .uri = "/loadFromCSVReminder",     .method = HTTP_GET, .handler = loadCSVReminder_handler };
    static const httpd_uri_t receive_uri      = { .uri = "/receiveevent",           .method = HTTP_POST, .handler = receiveevent_handler };
    static const httpd_uri_t delete_uri       = { .uri = "/deleteevent",            .method = HTTP_GET, .handler = deleteevent_handler };
    static const httpd_uri_t toggledis_uri    = { .uri = "/toggledisable",          .method = HTTP_GET, .handler = toggledisable_handler };
    static const httpd_uri_t changetime_uri   = { .uri = "/changetime",             .method = HTTP_GET, .handler = changetime_handler };
    static const httpd_uri_t resetovr_uri     = { .uri = "/resetoverrides",         .method = HTTP_GET, .handler = resetoverrides_handler };
    static const httpd_uri_t csv_all_out_uri  = { .uri = "/createCSV",              .method = HTTP_GET, .handler = createCSVAll_handler };
    static const httpd_uri_t csv_all_in_uri   = { .uri = "/loadfromCSV",            .method = HTTP_GET, .handler = loadCSVAll_handler };
    static const httpd_uri_t deletetype_uri   = { .uri = "/deletetype",             .method = HTTP_GET, .handler = deletetype_handler };

    httpd_register_uri_handler(server, &birthdays_uri);
    httpd_register_uri_handler(server, &anniversary_uri);
    httpd_register_uri_handler(server, &reminders_uri);
    httpd_register_uri_handler(server, &quick_uri);
    httpd_register_uri_handler(server, &csv_bir_out_uri);
    httpd_register_uri_handler(server, &csv_bir_in_uri);
    httpd_register_uri_handler(server, &csv_ann_out_uri);
    httpd_register_uri_handler(server, &csv_ann_in_uri);
    httpd_register_uri_handler(server, &csv_rem_out_uri);
    httpd_register_uri_handler(server, &csv_rem_in_uri);
    httpd_register_uri_handler(server, &receive_uri);
    httpd_register_uri_handler(server, &delete_uri);
    httpd_register_uri_handler(server, &toggledis_uri);
    httpd_register_uri_handler(server, &changetime_uri);
    httpd_register_uri_handler(server, &resetovr_uri);
    httpd_register_uri_handler(server, &csv_all_out_uri);
    httpd_register_uri_handler(server, &csv_all_in_uri);
    httpd_register_uri_handler(server, &deletetype_uri);
}
