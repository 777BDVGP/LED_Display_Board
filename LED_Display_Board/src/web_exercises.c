// =============================================================================
// web_exercises.c — Exercises Web Page & AJAX Handlers
// =============================================================================
// See web_exercises.h.
// =============================================================================

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "cJSON.h"

#include "exercises.h"
#include "files.h"
#include "web_about.h"
#include "web_core.h"
#include "web_exercises.h"

// =============================================================================
// Shared Helper — collect + sort "/Exer_NN.json" filenames
// =============================================================================

static char s_exFiles[EX_MAX_SLOTS][16];

static int collectSortedExerciseFiles(void)
{
    int count = 0;
    DIR *dir = opendir(SPIFFS_BASE_PATH);
    if (!dir) return 0;

    struct dirent *ent;
    while (count < EX_MAX_SLOTS && (ent = readdir(dir)) != NULL) {
        size_t nlen = strlen(ent->d_name);
        if (nlen < 6 || strncmp(ent->d_name, "Exer_", 5) != 0) continue;
        if (strcmp(ent->d_name + nlen - 5, ".json") != 0) continue;
        s_exFiles[count][0] = '/';
        strlcpy(s_exFiles[count] + 1, ent->d_name, sizeof(s_exFiles[count]) - 1);
        count++;
    }
    closedir(dir);

    for (int i = 1; i < count; i++) {
        char tmp[16];
        strlcpy(tmp, s_exFiles[i], sizeof(tmp));
        int j = i - 1;
        while (j >= 0 && strcmp(s_exFiles[j], tmp) > 0) {
            strlcpy(s_exFiles[j + 1], s_exFiles[j], sizeof(s_exFiles[j + 1]));
            j--;
        }
        strlcpy(s_exFiles[j + 1], tmp, sizeof(s_exFiles[j + 1]));
    }
    return count;
}

// =============================================================================
// GET /exercises
// =============================================================================

static esp_err_t sendExerciseDropdownOptions(httpd_req_t *req)
{
    static const char placeholder[] = "<option value=''>&#9135; Select exercise &#9135;</option>";
    esp_err_t err = httpd_resp_send_chunk(req, placeholder, sizeof(placeholder) - 1);
    if (err != ESP_OK) return err;

    int count = collectSortedExerciseFiles();
    for (int i = 0; i < count; i++) {
        ExerciseData ex;
        char exName[EX_NAME_MAX + 1];
        if (Exercise_LoadFromSPIFFS(s_exFiles[i], &ex) && ex.name[0] != '\0') {
            strlcpy(exName, ex.name, sizeof(exName));
        } else {
            char slot[8] = "";
            const char *us = strrchr(s_exFiles[i], '_');
            if (us) {
                strlcpy(slot, us + 1, sizeof(slot));
                char *dot = strchr(slot, '.');
                if (dot) *dot = '\0';
            }
            snprintf(exName, sizeof(exName), "Exercise %s", slot);
        }

        char row[80];
        int rn = snprintf(row, sizeof(row), "<option value='%s'>%s</option>", s_exFiles[i], exName);
        if ((err = httpd_resp_send_chunk(req, row, rn)) != ESP_OK) return err;
    }
    return ESP_OK;
}

static esp_err_t exercises_handler(httpd_req_t *req)
{
    esp_err_t err;
    if ((err = WebCore_SendShellOpen(req, "LED Clock - Exercises")) != ESP_OK) return err;
    if ((err = WebCore_SendHeader(req, FA_MESS_06)) != ESP_OK) return err;
    if ((err = WebCore_SendNav(req, PS_EXERCISES)) != ESP_OK) return err;
    if ((err = WebCore_SendScript(req)) != ESP_OK) return err;

    static const char part1[] =
        "<div class='sec'>PHYSICAL EXERCISES</div>"
        "<div class='gr'><span class='lbl'>Select &amp; Run</span>"
        "<div style='display:flex;gap:8px;align-items:center;flex-wrap:wrap'>"
        "<select id='ex_sel' onchange='exLoad(this.value)' style='min-width:180px'>";
    if ((err = httpd_resp_send_chunk(req, part1, sizeof(part1) - 1)) != ESP_OK) return err;

    if ((err = sendExerciseDropdownOptions(req)) != ESP_OK) return err;

    static const char part2[] =
        "</select>"
        "<button class='btn btn-bl' id='ex_btn' onclick='exToggle()'>START</button>"
        "</div></div>"
        "<div class='gr'><span class='lbl'>EXERCISE TIME</span>"
        "<span id='ex_etime' style='font-family:monospace;font-size:16px;color:#38bdf8'>00:00</span>"
        "<span class='lbl' style='padding-left:12px'>TOTAL</span>"
        "<span id='ex_ttime' style='font-family:monospace;font-size:16px;color:#22c55e'>00:00:00</span></div>"
        "<div class='sec'>EXERCISE SETUP</div>"
        "<div class='gr'><span class='lbl'>Exercise No</span>"
        "<span id='ex_no' style='color:#38bdf8;font-weight:700;font-size:15px'>&#8212;</span></div>"
        "<div class='gr'><span class='lbl'>Name</span>"
        "<input type='text' id='ex_name' value='' maxlength='24'></div>"
        "<div class='gr'><span class='lbl'>Exercise No Instruction</span>"
        "<select id='ex_startspeed' style='margin-right:6px'>"
        "<option value='5'>Speed 5</option><option value='15'>Speed 15</option>"
        "<option value='25' selected>Speed 25</option></select>"
        "<input type='text' id='ex_startpos' value='' maxlength='60' style='width:220px'></div>"
        "<div class='gr'><span class='lbl'>Number of Movements</span>"
        "<select id='ex_moves' onchange='exMovesChange(this.value)'>"
        "<option value='1'>1</option><option value='2' selected>2</option>"
        "<option value='3'>3</option><option value='4'>4</option></select></div>"
        "<div class='gr'><span class='lbl'>Sets</span>"
        "<div style='display:flex;gap:12px;align-items:center'>"
        "<input type='number' id='ex_sets' value='1' min='1' max='20' style='width:55px'>"
        "<span style='color:#64748b;font-size:11px'>Wait between sets</span>"
        "<input type='number' id='ex_setswait' value='0' min='0' max='60' style='width:55px'>"
        "<span style='color:#64748b;font-size:11px'>s</span></div></div>"
        "<div class='gr'><span class='lbl'>Set Instruction</span>"
        "<div style='display:flex;gap:6px;align-items:center;flex-wrap:wrap'>"
        "<select id='ex_setinstrspeed'>"
        "<option value='5'>Speed 5</option><option value='15'>Speed 15</option>"
        "<option value='25' selected>Speed 25</option></select>"
        "<input type='text' id='ex_setinstr' value='' maxlength='60' placeholder='e.g. Now Other Leg' style='flex:1;min-width:120px'>"
        "<span style='color:#64748b;font-size:11px'>Wait</span>"
        "<input type='number' id='ex_setinstrwait' value='0' min='0' max='99' style='width:55px'>"
        "<span style='color:#64748b;font-size:11px'>s</span></div></div>"
        "<div class='gr'><span class='lbl'>Repetitions</span>"
        "<input type='number' id='ex_reps' value='12' min='1' max='99' style='width:70px'></div>"
        "<div class='gr'><span class='lbl'>Movement 1 "
        "<span style='color:#475569;font-size:11px;font-weight:normal'>(8 chars max)</span></span>"
        "<div style='display:flex;gap:6px;align-items:center'>"
        "<input type='text' id='ex_m1' value='' maxlength='8' style='width:88px' placeholder='e.g. Upward'>"
        "<span style='color:#64748b;font-size:11px'>Hold</span>"
        "<input type='number' id='ex_t1' value='2' min='1' max='99' style='width:55px'>"
        "<span style='color:#64748b;font-size:11px'>s</span></div></div>"
        "<div class='gr'><span class='lbl'>Movement 2 "
        "<span style='color:#475569;font-size:11px;font-weight:normal'>(8 chars max)</span></span>"
        "<div style='display:flex;gap:6px;align-items:center'>"
        "<input type='text' id='ex_m2' value='' maxlength='8' style='width:88px' placeholder='e.g. Upward'>"
        "<span style='color:#64748b;font-size:11px'>Hold</span>"
        "<input type='number' id='ex_t2' value='2' min='1' max='99' style='width:55px'>"
        "<span style='color:#64748b;font-size:11px'>s</span></div></div>"
        "<div class='gr' id='ex_row3' style='display:none'><span class='lbl'>Movement 3 "
        "<span style='color:#475569;font-size:11px;font-weight:normal'>(8 chars max)</span></span>"
        "<div style='display:flex;gap:6px;align-items:center'>"
        "<input type='text' id='ex_m3' value='' maxlength='8' style='width:88px' placeholder='e.g. Upward'>"
        "<span style='color:#64748b;font-size:11px'>Hold</span>"
        "<input type='number' id='ex_t3' value='2' min='1' max='99' style='width:55px'>"
        "<span style='color:#64748b;font-size:11px'>s</span></div></div>"
        "<div class='gr' id='ex_row4' style='display:none'><span class='lbl'>Movement 4 "
        "<span style='color:#475569;font-size:11px;font-weight:normal'>(8 chars max)</span></span>"
        "<div style='display:flex;gap:6px;align-items:center'>"
        "<input type='text' id='ex_m4' value='' maxlength='8' style='width:88px' placeholder='e.g. Upward'>"
        "<span style='color:#64748b;font-size:11px'>Hold</span>"
        "<input type='number' id='ex_t4' value='2' min='1' max='99' style='width:55px'>"
        "<span style='color:#64748b;font-size:11px'>s</span></div></div>"
        "<input type='hidden' id='ex_file' value=''>"
        "<div class='save-row' style='display:flex;gap:8px;flex-wrap:wrap;align-items:center'>"
        "<button class='btn btn-bl' onclick='exSave()'>&#10003; SAVE EXERCISE</button>"
        "<button class='btn btn-rd' onclick='exDelete()'>&#128465; DELETE</button>"
        "<button class='btn' style='background:#475569;color:#fff' onclick='exNew()'>&#10011; NEW</button>"
        "<span id='ex_msg' style='font-size:12px;font-weight:600'></span></div>";
    if ((err = httpd_resp_send_chunk(req, part2, sizeof(part2) - 1)) != ESP_OK) return err;

    static const char script[] =
        "<script>"
        "var exRunning=false,exPoll=null,exStartMs=0,exTotalMs=0,exTimerInt=null;"
        "function exFmt(ms){var s=Math.floor(ms/1000),m=Math.floor(s/60);s=s%60;"
        "return(m<10?'0'+m:m)+':'+(s<10?'0'+s:s);}"
        "function exFmtH(ms){var s=Math.floor(ms/1000),m=Math.floor(s/60),h=Math.floor(m/60);"
        "s=s%60;m=m%60;"
        "return(h<10?'0'+h:h)+':'+(m<10?'0'+m:m)+':'+(s<10?'0'+s:s);}"
        "function exTickTimer(){var e=document.getElementById('ex_etime');"
        "if(e)e.textContent=exFmt(Date.now()-exStartMs);}"
        "function exMovesChange(v){v=parseInt(v);"
        "document.getElementById('ex_row3').style.display=(v>=3)?'':'none';"
        "document.getElementById('ex_row4').style.display=(v>=4)?'':'none';}"
        "function exMsg(txt,ok){var el=document.getElementById('ex_msg');"
        "el.style.color=ok?'#22c55e':'#ef4444';el.textContent=txt;"
        "if(ok)setTimeout(function(){el.textContent='';},3000);}"
        "function exNo(file){"
        "var m=file?file.match(/Exer_(\\d+)\\.json/):null;"
        "document.getElementById('ex_no').textContent=m?m[1]:'\\u2014';}"
        "function exToggle(){if(exRunning){"
        "Socket.send(JSON.stringify({ex_stop:'1'}));exSetStopped();}"
        "else{var f=document.getElementById('ex_sel').value;"
        "if(!f){exMsg('Select an exercise first',false);return;}"
        "Socket.send(JSON.stringify({ex_start:f}));exSetRunning();}}"
        "function exSetRunning(){exRunning=true;exStartMs=Date.now();"
        "exTimerInt=setInterval(exTickTimer,500);"
        "var b=document.getElementById('ex_btn');"
        "b.textContent='STOP';b.className='btn btn-rd';"
        "exPoll=setInterval(function(){"
        "fetch('/ex_status').then(function(r){return r.json();})"
        ".then(function(d){if(!d.running&&exRunning)exSetStopped();})"
        ".catch(function(){});},2000);}"
        "function exSetStopped(){if(!exRunning)return;"
        "exRunning=false;"
        "clearInterval(exPoll);exPoll=null;"
        "clearInterval(exTimerInt);exTimerInt=null;"
        "var elapsed=Date.now()-exStartMs;"
        "exTotalMs+=elapsed;"
        "var e=document.getElementById('ex_etime');"
        "if(e)e.textContent=exFmt(elapsed);"
        "var t=document.getElementById('ex_ttime');"
        "if(t)t.textContent=exFmtH(exTotalMs);"
        "var b=document.getElementById('ex_btn');"
        "b.textContent='START';b.className='btn btn-bl';}"
        "function exLoad(file){if(!file){exNew();return;}"
        "fetch('/ex_load?file='+encodeURIComponent(file))"
        ".then(function(r){return r.json();}).then(function(d){"
        "document.getElementById('ex_file').value=d.file||'';"
        "exNo(d.file||'');"
        "document.getElementById('ex_name').value=d.name||'';"
        "document.getElementById('ex_moves').value=d.moves||2;"
        "document.getElementById('ex_sets').value=d.sets||1;"
        "document.getElementById('ex_setswait').value=d.setsWait||0;"
        "document.getElementById('ex_setinstr').value=d.setInstr||'';"
        "var si=d.setInstrSpeed||0;if(si!==5&&si!==15&&si!==25)si=25;"
        "document.getElementById('ex_setinstrspeed').value=si;"
        "document.getElementById('ex_setinstrwait').value=d.setInstrWait||0;"
        "document.getElementById('ex_reps').value=d.reps||12;"
        "document.getElementById('ex_startpos').value=d.startPos||'';"
        "var ss=d.startSpeed||0;if(ss!==5&&ss!==15&&ss!==25)ss=25;"
        "document.getElementById('ex_startspeed').value=ss;"
        "document.getElementById('ex_m1').value=d.m1||'';"
        "document.getElementById('ex_t1').value=d.t1||2;"
        "document.getElementById('ex_m2').value=d.m2||'';"
        "document.getElementById('ex_t2').value=d.t2||2;"
        "document.getElementById('ex_m3').value=d.m3||'';"
        "document.getElementById('ex_t3').value=d.t3||2;"
        "document.getElementById('ex_m4').value=d.m4||'';"
        "document.getElementById('ex_t4').value=d.t4||2;"
        "exMovesChange(d.moves||2);"
        "document.getElementById('ex_msg').textContent='';});}"
        "function exSave(){"
        "var moves=parseInt(document.getElementById('ex_moves').value);"
        "if(!document.getElementById('ex_name').value.trim()){exMsg('Name is required',false);return;}"
        "if(!document.getElementById('ex_startpos').value.trim()){exMsg('Starting Position is required',false);return;}"
        "for(var i=1;i<=moves;i++){if(!document.getElementById('ex_m'+i).value.trim()){exMsg('Movement '+i+' name is required',false);return;}}"
        "var data={"
        "file:document.getElementById('ex_file').value,"
        "name:document.getElementById('ex_name').value,"
        "moves:parseInt(document.getElementById('ex_moves').value),"
        "sets:parseInt(document.getElementById('ex_sets').value),"
        "setsWait:parseInt(document.getElementById('ex_setswait').value),"
        "setInstr:document.getElementById('ex_setinstr').value,"
        "setInstrSpeed:parseInt(document.getElementById('ex_setinstrspeed').value),"
        "setInstrWait:parseInt(document.getElementById('ex_setinstrwait').value),"
        "reps:parseInt(document.getElementById('ex_reps').value),"
        "startPos:document.getElementById('ex_startpos').value,"
        "startSpeed:parseInt(document.getElementById('ex_startspeed').value),"
        "m1:document.getElementById('ex_m1').value,"
        "t1:parseInt(document.getElementById('ex_t1').value),"
        "m2:document.getElementById('ex_m2').value,"
        "t2:parseInt(document.getElementById('ex_t2').value),"
        "m3:document.getElementById('ex_m3').value,"
        "t3:parseInt(document.getElementById('ex_t3').value),"
        "m4:document.getElementById('ex_m4').value,"
        "t4:parseInt(document.getElementById('ex_t4').value)};"
        "fetch('/ex_save',{method:'POST',"
        "headers:{'Content-Type':'application/json'},"
        "body:JSON.stringify(data)})"
        ".then(function(r){return r.json();}).then(function(d){"
        "if(d.ok){"
        "document.getElementById('ex_file').value=d.file;"
        "exNo(d.file);"
        "var sel=document.getElementById('ex_sel');var found=false;"
        "for(var i=0;i<sel.options.length;i++){"
        "if(sel.options[i].value===d.file){sel.options[i].text=d.name;found=true;break;}}"
        "if(!found){var o=document.createElement('option');"
        "o.value=d.file;o.text=d.name;sel.appendChild(o);}"
        "sel.value=d.file;exMsg('Saved!',true);}"
        "else exMsg(d.msg||'Save failed',false);});}"
        "function exDelete(){var f=document.getElementById('ex_file').value;"
        "if(!f){exMsg('No exercise selected',false);return;}"
        "if(!confirm('Delete this exercise?'))return;"
        "fetch('/ex_delete?file='+encodeURIComponent(f))"
        ".then(function(r){return r.json();}).then(function(d){if(d.ok){"
        "var sel=document.getElementById('ex_sel');"
        "for(var i=0;i<sel.options.length;i++){"
        "if(sel.options[i].value===f){sel.remove(i);break;}}"
        "exNew();}});}"
        "function exNew(){"
        "document.getElementById('ex_file').value='';"
        "exNo('');"
        "document.getElementById('ex_name').value='';"
        "document.getElementById('ex_moves').value=2;"
        "document.getElementById('ex_sets').value=1;"
        "document.getElementById('ex_setswait').value=0;"
        "document.getElementById('ex_setinstr').value='';"
        "document.getElementById('ex_setinstrspeed').value=25;"
        "document.getElementById('ex_setinstrwait').value=0;"
        "document.getElementById('ex_reps').value=12;"
        "document.getElementById('ex_startpos').value='';"
        "document.getElementById('ex_startspeed').value=25;"
        "for(var i=1;i<=4;i++){"
        "var me=document.getElementById('ex_m'+i);"
        "var te=document.getElementById('ex_t'+i);"
        "if(me)me.value='';if(te)te.value=2;}"
        "document.getElementById('ex_sel').value='';"
        "document.getElementById('ex_msg').textContent='';"
        "exMovesChange(2);}"
        "exMovesChange(document.getElementById('ex_moves').value);"
        "</script>";
    if ((err = httpd_resp_send_chunk(req, script, sizeof(script) - 1)) != ESP_OK) return err;

    return WebCore_SendShellClose(req);
}

// =============================================================================
// AJAX Handlers
// =============================================================================

static bool getFileQueryArg(httpd_req_t *req, char *out, size_t outsz)
{
    char query[64] = "";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return false;
    return httpd_query_key_value(query, "file", out, outsz) == ESP_OK && out[0] != '\0';
}

static esp_err_t exsave_handler(httpd_req_t *req)
{
    int total = req->content_len;
    if (total <= 0 || total > 2048) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"Bad body\"}");
        return ESP_OK;
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
    free(body);
    if (!doc) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"Bad JSON\"}");
        return ESP_OK;
    }

    ExerciseData ex;
    memset(&ex, 0, sizeof(ex));
    cJSON *item;
    if ((item = cJSON_GetObjectItemCaseSensitive(doc, "name")) && cJSON_IsString(item))
        strlcpy(ex.name, item->valuestring, sizeof(ex.name));
    if ((item = cJSON_GetObjectItemCaseSensitive(doc, "startPos")) && cJSON_IsString(item))
        strlcpy(ex.startPos, item->valuestring, sizeof(ex.startPos));
    if ((item = cJSON_GetObjectItemCaseSensitive(doc, "setInstr")) && cJSON_IsString(item))
        strlcpy(ex.setInstr, item->valuestring, sizeof(ex.setInstr));

    #define GETINT(key, field, def) \
        ex.field = ((item = cJSON_GetObjectItemCaseSensitive(doc, key)) && cJSON_IsNumber(item)) ? item->valueint : (def)
    GETINT("moves", moves, 2);
    GETINT("sets", sets, 1);
    GETINT("setsWait", setsWait, 0);
    GETINT("setInstrSpeed", setInstrSpeed, EX_SCROLL_DEFAULT);
    GETINT("setInstrWait", setInstrWait, 0);
    GETINT("reps", reps, 12);
    GETINT("startSpeed", startSpeed, EX_SCROLL_DEFAULT);
    #undef GETINT

    for (int i = 0; i < 4; i++) {
        char mk[4], tk[4];
        snprintf(mk, sizeof(mk), "m%d", i + 1);
        snprintf(tk, sizeof(tk), "t%d", i + 1);
        if ((item = cJSON_GetObjectItemCaseSensitive(doc, mk)) && cJSON_IsString(item))
            strlcpy(ex.m[i], item->valuestring, sizeof(ex.m[i]));
        ex.t[i] = ((item = cJSON_GetObjectItemCaseSensitive(doc, tk)) && cJSON_IsNumber(item)) ? item->valueint : 2;
    }

    char reqFile[20] = "";
    if ((item = cJSON_GetObjectItemCaseSensitive(doc, "file")) && cJSON_IsString(item))
        strlcpy(reqFile, item->valuestring, sizeof(reqFile));
    cJSON_Delete(doc);

    bool useExisting = false;
    if (reqFile[0] != '\0') {
        char full[64];
        snprintf(full, sizeof(full), "%s%s", SPIFFS_BASE_PATH, reqFile);
        struct stat st;
        if (stat(full, &st) == 0) useExisting = true;
    }
    if (useExisting) {
        strlcpy(ex.filename, reqFile, sizeof(ex.filename));
    } else {
        char newFile[16];
        Exercise_FindNextFilename(newFile, sizeof(newFile));
        if (newFile[0] == '\0') {
            httpd_resp_set_status(req, "507 Insufficient Storage");
            httpd_resp_set_type(req, "application/json");
            httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"Storage full\"}");
            return ESP_OK;
        }
        strlcpy(ex.filename, newFile, sizeof(ex.filename));
    }

    if (!Exercise_SaveToSPIFFS(&ex)) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false,\"msg\":\"Write failed\"}");
        return ESP_OK;
    }
    enqueueSyncSave(ex.filename);

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", true);
    cJSON_AddStringToObject(resp, "file", ex.filename);
    cJSON_AddStringToObject(resp, "name", ex.name);
    char *out = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    httpd_resp_set_type(req, "application/json");
    if (out) { httpd_resp_sendstr(req, out); free(out); }
    else httpd_resp_sendstr(req, "{\"ok\":false}");
    return ESP_OK;
}

static esp_err_t exload_handler(httpd_req_t *req)
{
    char file[20];
    if (!getFileQueryArg(req, file, sizeof(file))) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false}");
        return ESP_OK;
    }
    char full[64];
    snprintf(full, sizeof(full), "%s%s", SPIFFS_BASE_PATH, file);
    struct stat st;
    ExerciseData ex;
    if (stat(full, &st) != 0 || !Exercise_LoadFromSPIFFS(file, &ex)) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false}");
        return ESP_OK;
    }

    cJSON *doc = cJSON_CreateObject();
    cJSON_AddStringToObject(doc, "file", ex.filename);
    cJSON_AddStringToObject(doc, "name", ex.name);
    cJSON_AddNumberToObject(doc, "moves", ex.moves);
    cJSON_AddNumberToObject(doc, "sets", ex.sets);
    cJSON_AddNumberToObject(doc, "setsWait", ex.setsWait);
    cJSON_AddStringToObject(doc, "setInstr", ex.setInstr);
    cJSON_AddNumberToObject(doc, "setInstrSpeed", ex.setInstrSpeed);
    cJSON_AddNumberToObject(doc, "setInstrWait", ex.setInstrWait);
    cJSON_AddNumberToObject(doc, "reps", ex.reps);
    cJSON_AddStringToObject(doc, "startPos", ex.startPos);
    cJSON_AddNumberToObject(doc, "startSpeed", ex.startSpeed);
    for (int i = 0; i < 4; i++) {
        char mk[4], tk[4];
        snprintf(mk, sizeof(mk), "m%d", i + 1);
        snprintf(tk, sizeof(tk), "t%d", i + 1);
        cJSON_AddStringToObject(doc, mk, ex.m[i]);
        cJSON_AddNumberToObject(doc, tk, ex.t[i]);
    }
    char *out = cJSON_PrintUnformatted(doc);
    cJSON_Delete(doc);
    httpd_resp_set_type(req, "application/json");
    if (out) { httpd_resp_sendstr(req, out); free(out); }
    else httpd_resp_sendstr(req, "{\"ok\":false}");
    return ESP_OK;
}

static esp_err_t exdelete_handler(httpd_req_t *req)
{
    char file[20];
    if (!getFileQueryArg(req, file, sizeof(file))) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "application/json");
        httpd_resp_sendstr(req, "{\"ok\":false}");
        return ESP_OK;
    }
    enqueueSyncDelete(file);
    char full[64];
    snprintf(full, sizeof(full), "%s%s", SPIFFS_BASE_PATH, file);
    remove(full);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t exstatus_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, exerciseActive ? "{\"running\":true}" : "{\"running\":false}");
    return ESP_OK;
}

// =============================================================================
// CSV Export / Import
// =============================================================================

static esp_err_t redirectDiskTools(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/disktools");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t createcsv_handler(httpd_req_t *req)
{
    char full[64];
    snprintf(full, sizeof(full), "%s/EXERCISES.CSV", SPIFFS_BASE_PATH);
    FILE *csv = fopen(full, "w");
    if (csv) {
        fputs("NAME,MOVES,SETS,SETS_WAIT,SET_INSTR,SET_INSTR_SPEED,SET_INSTR_WAIT,REPS,START_POS,START_SPEED,M1,T1,M2,T2,M3,T3,M4,T4\n", csv);
        int count = collectSortedExerciseFiles();
        for (int i = 0; i < count; i++) {
            ExerciseData ex;
            if (!Exercise_LoadFromSPIFFS(s_exFiles[i], &ex)) continue;
            fprintf(csv, "%s,%d,%d,%d,%s,%d,%d,%d,%s,%d,",
                    ex.name, ex.moves, ex.sets, ex.setsWait, ex.setInstr,
                    ex.setInstrSpeed, ex.setInstrWait, ex.reps, ex.startPos, ex.startSpeed);
            for (int m = 0; m < 4; m++) {
                fprintf(csv, "%s,%d", ex.m[m], ex.t[m]);
                if (m < 3) fputc(',', csv);
            }
            fputc('\n', csv);
        }
        fclose(csv);
    }
    return redirectDiskTools(req);
}

static esp_err_t loadcsv_handler(httpd_req_t *req)
{
    // Delete existing exercise files first.
    int count = collectSortedExerciseFiles();
    for (int i = 0; i < count; i++) {
        char full[64];
        snprintf(full, sizeof(full), "%s%s", SPIFFS_BASE_PATH, s_exFiles[i]);
        remove(full);
    }

    char csvPath[64];
    snprintf(csvPath, sizeof(csvPath), "%s/EXERCISES.CSV", SPIFFS_BASE_PATH);
    FILE *csv = fopen(csvPath, "r");
    if (csv) {
        char header[300];
        if (fgets(header, sizeof(header), csv)) {
            char line[400];
            while (fgets(line, sizeof(line), csv)) {
                size_t len = strlen(line);
                while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = '\0';
                if (len == 0) continue;

                char *fields[18];
                int i = 0;
                char *p = line;
                for (i = 0; i < 18; i++) {
                    fields[i] = p;
                    char *comma = strchr(p, ',');
                    if (!comma) break;
                    *comma = '\0';
                    p = comma + 1;
                }
                if (i < 17) continue; // need all 18 columns

                ExerciseData ex;
                memset(&ex, 0, sizeof(ex));
                strlcpy(ex.name, fields[0], sizeof(ex.name));
                ex.moves = atoi(fields[1]);
                ex.sets = atoi(fields[2]);
                ex.setsWait = atoi(fields[3]);
                strlcpy(ex.setInstr, fields[4], sizeof(ex.setInstr));
                ex.setInstrSpeed = atoi(fields[5]);
                ex.setInstrWait = atoi(fields[6]);
                ex.reps = atoi(fields[7]);
                strlcpy(ex.startPos, fields[8], sizeof(ex.startPos));
                ex.startSpeed = atoi(fields[9]);
                for (int m = 0; m < 4; m++) {
                    strlcpy(ex.m[m], fields[10 + m * 2], sizeof(ex.m[m]));
                    ex.t[m] = atoi(fields[11 + m * 2]);
                }

                char slot[16];
                Exercise_FindNextFilename(slot, sizeof(slot));
                if (slot[0] == '\0') break;
                strlcpy(ex.filename, slot, sizeof(ex.filename));
                Exercise_SaveToSPIFFS(&ex);
            }
        }
        fclose(csv);
    }

    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/exercises");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

// =============================================================================
// Public API
// =============================================================================

void WebExercises_RegisterRoutes(httpd_handle_t server)
{
    static const httpd_uri_t exercises_uri = { .uri = "/exercises", .method = HTTP_GET, .handler = exercises_handler };
    static const httpd_uri_t save_uri      = { .uri = "/ex_save",   .method = HTTP_POST, .handler = exsave_handler };
    static const httpd_uri_t load_uri      = { .uri = "/ex_load",   .method = HTTP_GET, .handler = exload_handler };
    static const httpd_uri_t delete_uri    = { .uri = "/ex_delete", .method = HTTP_GET, .handler = exdelete_handler };
    static const httpd_uri_t status_uri    = { .uri = "/ex_status", .method = HTTP_GET, .handler = exstatus_handler };
    static const httpd_uri_t csvout_uri    = { .uri = "/createCSVExercises",   .method = HTTP_GET, .handler = createcsv_handler };
    static const httpd_uri_t csvin_uri     = { .uri = "/loadFromCSVExercises", .method = HTTP_GET, .handler = loadcsv_handler };

    httpd_register_uri_handler(server, &exercises_uri);
    httpd_register_uri_handler(server, &save_uri);
    httpd_register_uri_handler(server, &load_uri);
    httpd_register_uri_handler(server, &delete_uri);
    httpd_register_uri_handler(server, &status_uri);
    httpd_register_uri_handler(server, &csvout_uri);
    httpd_register_uri_handler(server, &csvin_uri);
}
