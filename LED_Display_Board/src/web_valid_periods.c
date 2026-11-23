// =============================================================================
// web_valid_periods.c — Reminder Valid Periods web page
// =============================================================================
// See web_valid_periods.h and valid_periods.h.
// =============================================================================

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"

#include "event_store.h"
#include "files.h"
#include "valid_periods.h"
#include "web_about.h"
#include "web_core.h"
#include "web_valid_periods.h"

#define VP_PAGE "/validperiods"

static VpSummary s_list[VP_MAX_PERIODS];

// =============================================================================
// Helpers
// =============================================================================

// Keeps names safe to drop straight into HTML/JS/CSV: letters, digits, UTF-8
// bytes and a few harmless punctuation marks; everything else is dropped.
static void sanitizeName(const char *in, char *out, size_t outsz)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 1 < outsz; p++) {
        unsigned char c = *p;
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c >= 0x80 || c == ' ' || c == '-' || c == '_' || c == '(' || c == ')' || c == '.';
        if (ok) out[o++] = (char)c;
    }
    out[o] = '\0';
    // trim
    char *s = out;
    while (*s == ' ') s++;
    if (s != out) memmove(out, s, strlen(s) + 1);
    size_t n = strlen(out);
    while (n > 0 && out[n - 1] == ' ') out[--n] = '\0';
}

// Accepts only "/ValidPeriod_<digits>.json".
static bool isValidRef(const char *ref)
{
    static const char pre[] = "/" VP_FILE_PREFIX;
    size_t pl = sizeof(pre) - 1;
    if (strncmp(ref, pre, pl) != 0) return false;
    const char *p = ref + pl;
    if (*p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') p++;
    return strcmp(p, ".json") == 0 && strlen(ref) < VP_REF_LEN;
}

static esp_err_t sendPageTop(httpd_req_t *req)
{
    esp_err_t err;
    if ((err = WebCore_SendShellOpen(req, "LED Clock - Valid Periods")) != ESP_OK) return err;
    if ((err = WebCore_SendHeader(req, FA_MESS_06)) != ESP_OK) return err;
    if ((err = WebCore_SendNav(req, PS_VALIDPER)) != ESP_OK) return err;
    return WebCore_SendScript(req);
}

// Sets every Reminder that points at ref back to None (rewritten in place,
// synced to peers). Returns how many were changed.
static int clearReminderRefs(const char *ref)
{
    DIR *dir = opendir(SPIFFS_BASE_PATH);
    if (!dir) return 0;
    EventRecord *tmp = malloc(sizeof(EventRecord));
    if (!tmp) { closedir(dir); return 0; }

    int changed = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        if (nlen < 6 || strcmp(ent->d_name + nlen - 5, ".json") != 0) continue;
        if (strncmp(ent->d_name, VP_FILE_PREFIX, strlen(VP_FILE_PREFIX)) == 0) continue;

        char fpath[300];
        snprintf(fpath, sizeof(fpath), "%s/%s", SPIFFS_BASE_PATH, ent->d_name);
        Event_InitDefaults(tmp);
        if (!Event_ReadData(fpath, tmp)) continue;
        if (strcmp(tmp->fields[S_EVT_TYPE].value, EVY_TYPE_REM) != 0) continue;
        if (strcmp(tmp->fields[S_EVT_VALID_PER].value, ref) != 0) continue;

        strlcpy(tmp->fields[S_EVT_VALID_PER].value, " ", EVENT_VALUE_LEN);
        if (Event_WriteDataTo(tmp, fpath)) {
            char rel[300];
            snprintf(rel, sizeof(rel), "/%s", ent->d_name);
            enqueueSyncSave(rel);
            changed++;
        }
    }
    closedir(dir);
    free(tmp);
    return changed;
}

// One SPIFFS pass: for each period in list[0..n), counts the Reminders whose
// Valid Period points at it. used[] must hold n entries.
static void countReminderRefs(const VpSummary *list, int n, int *used)
{
    for (int i = 0; i < n; i++) used[i] = 0;
    if (n == 0) return;
    DIR *dir = opendir(SPIFFS_BASE_PATH);
    if (!dir) return;
    EventRecord *tmp = malloc(sizeof(EventRecord));
    if (!tmp) { closedir(dir); return; }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        if (nlen < 6 || strcmp(ent->d_name + nlen - 5, ".json") != 0) continue;
        if (strncmp(ent->d_name, VP_FILE_PREFIX, strlen(VP_FILE_PREFIX)) == 0) continue;

        char fpath[300];
        snprintf(fpath, sizeof(fpath), "%s/%s", SPIFFS_BASE_PATH, ent->d_name);
        Event_InitDefaults(tmp);
        if (!Event_ReadData(fpath, tmp)) continue;
        if (strcmp(tmp->fields[S_EVT_TYPE].value, EVY_TYPE_REM) != 0) continue;
        const char *ref = tmp->fields[S_EVT_VALID_PER].value;
        for (int i = 0; i < n; i++) {
            if (strcmp(list[i].ref, ref) == 0) { used[i]++; break; }
        }
    }
    closedir(dir);
    free(tmp);
}

// =============================================================================
// Views
// =============================================================================

static esp_err_t sendListView(httpd_req_t *req)
{
    esp_err_t err;
    static const char top[] =
        "<div class='save-row'>"
        "<button class='btn btn-bl' onclick=\"location.href='" VP_PAGE "?f_Create=1'\">&#43; Create New Valid Period</button>"
        "<button class='btn btn-rd btn-sm' onclick=\"location.href='" VP_PAGE "?f_DelList=1'\">&#10007; Delete List</button>"
        "</div><div class='sec'>REMINDER VALID PERIODS &mdash; SELECT TO EDIT</div><div class='ibox'>";
    if ((err = httpd_resp_send_chunk(req, top, sizeof(top) - 1)) != ESP_OK) return err;

    int count = VP_List(s_list, VP_MAX_PERIODS);
    if (count == 0) {
        static const char none[] = "<div class='irow'><dt>No valid periods yet</dt><dd></dd></div>";
        if ((err = httpd_resp_send_chunk(req, none, sizeof(none) - 1)) != ESP_OK) return err;
    }
    static int s_used[VP_MAX_PERIODS];
    countReminderRefs(s_list, count, s_used);
    for (int i = 0; i < count; i++) {
        char row[480];
        int n = snprintf(row, sizeof(row),
            "<a href='?f_Edit=%s'><div class='irow'><dt>%s</dt><dd>"
            "<span class='tag tok' style='margin-right:6px'>%d reminder%s</span>"
            "<span class='tag tok'>%d range%s</span></dd></div></a>",
            s_list[i].ref, s_list[i].name,
            s_used[i], s_used[i] == 1 ? "" : "s",
            s_list[i].count, s_list[i].count == 1 ? "" : "s");
        if ((err = httpd_resp_send_chunk(req, row, n)) != ESP_OK) return err;
    }
    static const char close[] = "</div>";
    return httpd_resp_send_chunk(req, close, sizeof(close) - 1);
}

static esp_err_t sendDeleteListView(httpd_req_t *req)
{
    esp_err_t err;
    static const char top[] =
        "<div class='save-row'><button class='btn btn-bl' onclick=\"location.href='" VP_PAGE "'\">&#8592; Back</button></div>"
        "<div class='sec'>SELECT TO DELETE</div>"
        "<div style='font-size:12px;opacity:.75;margin:4px 0 8px'>Reminders using a deleted period are switched back to None.</div>"
        "<div class='ibox'>";
    if ((err = httpd_resp_send_chunk(req, top, sizeof(top) - 1)) != ESP_OK) return err;

    int count = VP_List(s_list, VP_MAX_PERIODS);
    for (int i = 0; i < count; i++) {
        char row[420];
        int n = snprintf(row, sizeof(row),
            "<a href='?f_DelFile=%s' onclick=\"return confirm('Delete %s?')\"><div class='irow'><dt>%s</dt>"
            "<dd style='color:#ef4444'>&#10007;</dd></div></a>",
            s_list[i].ref, s_list[i].name, s_list[i].name);
        if ((err = httpd_resp_send_chunk(req, row, n)) != ESP_OK) return err;
    }
    static const char close[] = "</div>";
    return httpd_resp_send_chunk(req, close, sizeof(close) - 1);
}

// Editor: name + dynamic table of ranges, saved via POST /vp_save.
static const char EDITOR_HTML[] =
    "<style>"
    ".vpr{display:grid;grid-template-columns:1fr 1fr auto auto;gap:6px;align-items:center;margin:6px 0}"
    ".vpr input[type=date]{width:100%;min-width:0;box-sizing:border-box}"
    ".vph{font-size:11px;opacity:.7;font-weight:bold}"
    ".vpy{display:flex;align-items:center;gap:4px;font-size:12px;white-space:nowrap}"
    "</style>"
    "<div class='sec'>VALID PERIOD</div>"
    "<div class='gr'><span class='lbl'>Name</span><input type='text' id='vp_name' maxlength='60' style='flex:3;width:auto;min-width:0'></div>"
    "<div class='sec'>DATE RANGES (FROM &rarr; TO, BOTH DAYS INCLUDED)</div>"
    "<div class='vpr vph'><span>FROM</span><span>TO</span><span>EVERY YEAR</span><span></span></div>"
    "<div id='vp_rows'></div>"
    "<div class='save-row'><button type='button' class='btn btn-gn btn-sm' onclick='vpAdd()'>&#43; Add Range</button></div>"
    "<div style='font-size:12px;opacity:.75;margin:4px 0 8px'>"
    "Every year: the year is ignored and the range repeats each year "
    "(From after To wraps over New Year, e.g. 1 Dec &rarr; 31 Jan). "
    "Unticked: the exact dates including the year.</div>"
    "<div class='save-row'>"
    "<button class='btn btn-bl' onclick='vpSave()'>&#10003; SAVE</button>"
    "<button class='btn btn-rd btn-sm' onclick=\"location.href='" VP_PAGE "'\">&#8592; Cancel</button>"
    "</div>"
    "<script>"
    "function vpRow(f,t,y){"
    "var d=document.createElement('div');d.className='vpr';"
    "d.innerHTML=\"<input type='date' class='vf'><input type='date' class='vt'>"
    "<label class='vpy'><input type='checkbox' class='vy'>Yes</label>"
    "<button type='button' class='btn btn-rd btn-sm' style='padding:4px 10px'>&#10007;</button>\";"
    "d.querySelector('.vf').value=f||'';d.querySelector('.vt').value=t||'';d.querySelector('.vy').checked=!!y;"
    "d.querySelector('button').onclick=function(){d.remove();};"
    "document.getElementById('vp_rows').appendChild(d);}"
    "function vpAdd(){vpRow('','',false);}"
    "function vpSave(){"
    "var name=document.getElementById('vp_name').value.trim();"
    "if(!name){alert('Please enter a Name.');return;}"
    "var rows=document.querySelectorAll('#vp_rows .vpr');var r=[];"
    "for(var i=0;i<rows.length;i++){"
    "var f=rows[i].querySelector('.vf').value,t=rows[i].querySelector('.vt').value,y=rows[i].querySelector('.vy').checked;"
    "if(!f||!t){alert('Range '+(i+1)+': enter both From and To dates.');return;}"
    "if(!y&&f>t){alert('Range '+(i+1)+': From is after To.');return;}"
    "r.push({F:f,T:t,Y:y});}"
    "if(r.length===0){alert('Add at least one date range.');return;}"
    "if(r.length>30){alert('Maximum 30 ranges.');return;}"
    "fetch('/vp_save',{method:'POST',body:JSON.stringify({ref:VP.ref,name:name,ranges:r})})"
    ".then(function(x){return x.text();})"
    ".then(function(t){if(t==='OK')location.href='" VP_PAGE "';else alert('Save failed: '+t);})"
    ".catch(function(){alert('Save failed (no connection).');});}"
    "document.getElementById('vp_name').value=VP.name;"
    "if(VP.ranges.length===0)vpAdd();"
    "for(var i=0;i<VP.ranges.length;i++){var g=VP.ranges[i];vpRow(g.F,g.T,g.Y);}"
    "</script>";

static esp_err_t sendEditor(httpd_req_t *req, const ValidPeriod *vp)
{
    // Initial data as a JS object (name is sanitized, dates validated, ref
    // pattern-checked — nothing here can break out of the <script>).
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ref", vp->ref);
    cJSON_AddStringToObject(root, "name", vp->name);
    cJSON *arr = cJSON_AddArrayToObject(root, "ranges");
    for (int i = 0; i < vp->count; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "F", vp->r[i].from);
        cJSON_AddStringToObject(o, "T", vp->r[i].to);
        cJSON_AddBoolToObject(o, "Y", vp->r[i].yearly);
        cJSON_AddItemToArray(arr, o);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) return ESP_FAIL;

    esp_err_t err;
    static const char pre[] = "<script>var VP=";
    static const char post[] = ";</script>";
    if ((err = httpd_resp_send_chunk(req, pre, sizeof(pre) - 1)) == ESP_OK &&
        (err = httpd_resp_send_chunk(req, json, strlen(json))) == ESP_OK &&
        (err = httpd_resp_send_chunk(req, post, sizeof(post) - 1)) == ESP_OK) {
        err = httpd_resp_send_chunk(req, EDITOR_HTML, sizeof(EDITOR_HTML) - 1);
    }
    free(json);
    return err;
}

// =============================================================================
// GET /validperiods
// =============================================================================

static esp_err_t validperiods_handler(httpd_req_t *req)
{
    char query[160] = "";
    httpd_req_get_url_query_str(req, query, sizeof(query));

    char arg[VP_REF_LEN + 8] = "";
    esp_err_t err;

    if (httpd_query_key_value(query, "f_DelFile", arg, sizeof(arg)) == ESP_OK && isValidRef(arg)) {
        char full[96];
        snprintf(full, sizeof(full), SPIFFS_BASE_PATH "%s", arg);
        char name[VP_NAME_LEN];
        VP_GetName(arg, name, sizeof(name));
        remove(full);
        enqueueSyncDelete(arg);
        int changed = clearReminderRefs(arg);
        Event_RefreshTodayList();

        char msg[160];
        snprintf(msg, sizeof(msg), "Deleted %s - %d reminder%s set to None", name, changed, changed == 1 ? "" : "s");
        WebCore_SetMessage(FA_MESS_RED, msg);
        if ((err = sendPageTop(req)) != ESP_OK) return err;
        if ((err = sendDeleteListView(req)) != ESP_OK) return err;
        return WebCore_SendShellClose(req);
    }

    if ((err = sendPageTop(req)) != ESP_OK) return err;

    if (strstr(query, "f_DelList") != NULL) {
        if ((err = sendDeleteListView(req)) != ESP_OK) return err;
    } else if (strstr(query, "f_Create") != NULL) {
        ValidPeriod *vp = calloc(1, sizeof(ValidPeriod));
        if (!vp) return ESP_FAIL;
        err = sendEditor(req, vp);
        free(vp);
        if (err != ESP_OK) return err;
    } else if (httpd_query_key_value(query, "f_Edit", arg, sizeof(arg)) == ESP_OK && isValidRef(arg)) {
        ValidPeriod *vp = calloc(1, sizeof(ValidPeriod));
        if (!vp) return ESP_FAIL;
        if (!VP_Read(arg, vp)) {
            free(vp);
            if ((err = sendListView(req)) != ESP_OK) return err;
            return WebCore_SendShellClose(req);
        }
        err = sendEditor(req, vp);
        free(vp);
        if (err != ESP_OK) return err;
    } else {
        if ((err = sendListView(req)) != ESP_OK) return err;
    }
    return WebCore_SendShellClose(req);
}

// =============================================================================
// POST /vp_save
// =============================================================================

static esp_err_t replyText(httpd_req_t *req, const char *txt)
{
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, txt);
}

static bool isDateStr(const char *s)
{
    if (!s || strlen(s) != 10) return false;
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) { if (s[i] != '-') return false; }
        else if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

static esp_err_t vpsave_handler(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 6000) return replyText(req, "Bad request size");
    char *body = malloc(total + 1);
    if (!body) return replyText(req, "Out of memory");
    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, body + got, total - got);
        if (r <= 0) { free(body); return ESP_FAIL; }
        got += r;
    }
    body[total] = '\0';
    cJSON *root = cJSON_Parse(body);
    free(body);
    if (!root) return replyText(req, "Bad JSON");

    ValidPeriod *vp = calloc(1, sizeof(ValidPeriod));
    if (!vp) { cJSON_Delete(root); return replyText(req, "Out of memory"); }

    const char *result = "OK";
    cJSON *jref = cJSON_GetObjectItemCaseSensitive(root, "ref");
    cJSON *jname = cJSON_GetObjectItemCaseSensitive(root, "name");
    cJSON *jr = cJSON_GetObjectItemCaseSensitive(root, "ranges");

    if (!cJSON_IsString(jname) || !cJSON_IsArray(jr)) {
        result = "Missing fields";
        goto done;
    }
    sanitizeName(jname->valuestring, vp->name, sizeof(vp->name));
    if (vp->name[0] == '\0') { result = "Name is empty (use letters/digits)"; goto done; }

    cJSON *it;
    cJSON_ArrayForEach(it, jr) {
        if (vp->count >= VP_MAX_RANGES) { result = "Too many ranges (max 30)"; goto done; }
        cJSON *f = cJSON_GetObjectItemCaseSensitive(it, "F");
        cJSON *t = cJSON_GetObjectItemCaseSensitive(it, "T");
        cJSON *y = cJSON_GetObjectItemCaseSensitive(it, "Y");
        if (!cJSON_IsString(f) || !cJSON_IsString(t) || !isDateStr(f->valuestring) || !isDateStr(t->valuestring)) {
            result = "Invalid date";
            goto done;
        }
        VpRange *r = &vp->r[vp->count++];
        strlcpy(r->from, f->valuestring, sizeof(r->from));
        strlcpy(r->to, t->valuestring, sizeof(r->to));
        r->yearly = cJSON_IsTrue(y);
        if (!r->yearly && strcmp(r->from, r->to) > 0) { result = "From is after To"; goto done; }
    }
    if (vp->count == 0) { result = "Add at least one range"; goto done; }

    if (cJSON_IsString(jref) && jref->valuestring[0] != '\0') {
        if (!isValidRef(jref->valuestring)) { result = "Bad ref"; goto done; }
        strlcpy(vp->ref, jref->valuestring, sizeof(vp->ref));
    } else {
        VP_NewRef(vp->ref, sizeof(vp->ref));
    }

    if (!VP_Write(vp)) { result = "Write failed"; goto done; }
    enqueueSyncSave(vp->ref);
    Event_RefreshTodayList();

done:
    cJSON_Delete(root);
    free(vp);
    return replyText(req, result);
}

// =============================================================================
// CSV Export / Import  (Disk Tools)
// =============================================================================
// /spiffs/VALID_PERIODS.CSV — one line per range, lines of a period together:
//   REF,NAME,FROM,TO,EVERY_YEAR
//   /ValidPeriod_1.json,Gymnastics,2026-01-01,2026-07-01,false
// REF is kept so reminders that point at a period still match after a restore.

#define VP_CSV_PATH SPIFFS_BASE_PATH "/VALID_PERIODS.CSV"

static esp_err_t redirectDiskTools(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/disktools");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t createcsv_handler(httpd_req_t *req)
{
    FILE *csv = fopen(VP_CSV_PATH, "w");
    if (!csv) return redirectDiskTools(req);
    fputs("REF,NAME,FROM,TO,EVERY_YEAR\n", csv);

    ValidPeriod *vp = malloc(sizeof(ValidPeriod));
    if (vp) {
        int count = VP_List(s_list, VP_MAX_PERIODS);
        for (int i = 0; i < count; i++) {
            if (!VP_Read(s_list[i].ref, vp)) continue;
            for (int r = 0; r < vp->count; r++) {
                fprintf(csv, "%s,%s,%s,%s,%s\n", vp->ref, vp->name, vp->r[r].from, vp->r[r].to,
                        vp->r[r].yearly ? "true" : "false");
            }
        }
        free(vp);
    }
    fclose(csv);
    return redirectDiskTools(req);
}

// Excel likes to rewrite dates — accept YYYY-MM-DD or YYYY/MM/DD.
static bool normalizeCsvDate(const char *in, char *out, size_t outsz)
{
    strlcpy(out, in, outsz);
    for (char *c = out; *c; c++) if (*c == '/') *c = '-';
    return isDateStr(out);
}

static void trimField(char *s)
{
    char *a = s;
    while (*a == ' ' || *a == '"') a++;
    if (a != s) memmove(s, a, strlen(a) + 1);
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '"' || s[n - 1] == '\r' || s[n - 1] == '\n')) s[--n] = '\0';
}

static void deleteAllValidPeriodFiles(void)
{
    DIR *dir = opendir(SPIFFS_BASE_PATH);
    if (!dir) return;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strncmp(ent->d_name, VP_FILE_PREFIX, strlen(VP_FILE_PREFIX)) != 0) continue;
        char fpath[300];
        snprintf(fpath, sizeof(fpath), "%s/%s", SPIFFS_BASE_PATH, ent->d_name);
        remove(fpath);
    }
    closedir(dir);
}

static esp_err_t loadcsv_handler(httpd_req_t *req)
{
    FILE *csv = fopen(VP_CSV_PATH, "r");
    if (!csv) {
        WebCore_SetMessage(FA_MESS_RED, "VALID_PERIODS.CSV not found - nothing imported");
        return redirectDiskTools(req);
    }

    ValidPeriod *vp = calloc(1, sizeof(ValidPeriod));
    if (!vp) { fclose(csv); return redirectDiskTools(req); }

    // Same semantics as the other imports: the CSV replaces every period.
    deleteAllValidPeriodFiles();

    int periods = 0;
    char line[200];
    bool header = true;
    while (fgets(line, sizeof(line), csv)) {
        if (header) { header = false; continue; }
        char *f[5];
        int n = 0;
        char *p = line;
        while (n < 5) {
            f[n++] = p;
            char *comma = strchr(p, ',');
            if (!comma) break;
            *comma = '\0';
            p = comma + 1;
        }
        if (n < 5) continue;
        for (int i = 0; i < 5; i++) trimField(f[i]);
        if (!isValidRef(f[0])) continue;

        VpRange rg;
        if (!normalizeCsvDate(f[2], rg.from, sizeof(rg.from))) continue;
        if (!normalizeCsvDate(f[3], rg.to, sizeof(rg.to))) continue;
        rg.yearly = (strcasecmp(f[4], "true") == 0 || strcmp(f[4], "1") == 0 || strcasecmp(f[4], "yes") == 0);
        if (!rg.yearly && strcmp(rg.from, rg.to) > 0) continue;

        if (strcmp(vp->ref, f[0]) != 0) {
            // New period starts — write the one we were building.
            if (vp->ref[0] != '\0' && vp->count > 0 && VP_Write(vp)) periods++;
            // If this ref appeared earlier in the file (lines not grouped),
            // continue that period instead of starting it over.
            if (!VP_Read(f[0], vp)) {
                memset(vp, 0, sizeof(*vp));
                strlcpy(vp->ref, f[0], sizeof(vp->ref));
                sanitizeName(f[1], vp->name, sizeof(vp->name));
                if (vp->name[0] == '\0') strlcpy(vp->name, f[0] + 1, sizeof(vp->name));
            } else {
                periods--; // will be counted again when re-written
            }
        }
        if (vp->count < VP_MAX_RANGES) vp->r[vp->count++] = rg;
    }
    if (vp->ref[0] != '\0' && vp->count > 0 && VP_Write(vp)) periods++;
    fclose(csv);
    free(vp);

    Event_RefreshTodayList();
    enqueueSyncReplace(SYNC_TAG_VALID_PERIODS);

    char msg[80];
    snprintf(msg, sizeof(msg), "Imported %d valid period%s", periods, periods == 1 ? "" : "s");
    WebCore_SetMessage(FA_MESS_GRN, msg);
    return redirectDiskTools(req);
}

void WebValidPeriods_RegisterRoutes(httpd_handle_t server)
{
    static const httpd_uri_t page_uri  = { .uri = VP_PAGE,                     .method = HTTP_GET,  .handler = validperiods_handler };
    static const httpd_uri_t save_uri  = { .uri = "/vp_save",                  .method = HTTP_POST, .handler = vpsave_handler };
    static const httpd_uri_t csvout    = { .uri = "/createCSVValidPeriods",    .method = HTTP_GET,  .handler = createcsv_handler };
    static const httpd_uri_t csvin     = { .uri = "/loadFromCSVValidPeriods",  .method = HTTP_GET,  .handler = loadcsv_handler };
    httpd_register_uri_handler(server, &page_uri);
    httpd_register_uri_handler(server, &save_uri);
    httpd_register_uri_handler(server, &csvout);
    httpd_register_uri_handler(server, &csvin);
}
