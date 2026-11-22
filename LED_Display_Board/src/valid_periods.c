// =============================================================================
// valid_periods.c — Reminder Valid Periods storage & date check
// =============================================================================
// See valid_periods.h.
// =============================================================================

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "cJSON.h"
#include "esp_log.h"

#include "files.h"
#include "globals.h"
#include "valid_periods.h"

static const char *TAG = "valid_periods";

bool VP_RefIsNone(const char *ref)
{
    if (!ref) return true;
    while (*ref == ' ') ref++;
    return *ref == '\0';
}

static bool isDate(const char *s)
{
    if (!s || strlen(s) != 10) return false;
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) { if (s[i] != '-') return false; }
        else if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}

static char *readWholeFile(const char *fullPath)
{
    FILE *f = fopen(fullPath, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 8192) { fclose(f); return NULL; }
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[rd] = '\0';
    return buf;
}

bool VP_Read(const char *ref, ValidPeriod *out)
{
    memset(out, 0, sizeof(*out));
    if (VP_RefIsNone(ref) || ref[0] != '/') return false;

    char full[96];
    snprintf(full, sizeof(full), SPIFFS_BASE_PATH "%s", ref);

    if (xSemaphoreTake(variabMutex, portMAX_DELAY) != pdTRUE) return false;
    char *buf = readWholeFile(full);
    xSemaphoreGive(variabMutex);
    if (!buf) return false;

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return false;

    cJSON *name = cJSON_GetObjectItemCaseSensitive(root, "VP_NAME");
    cJSON *ranges = cJSON_GetObjectItemCaseSensitive(root, "VP_RANGES");
    if (!cJSON_IsString(name) || !cJSON_IsArray(ranges)) {
        cJSON_Delete(root);
        return false;
    }

    strlcpy(out->ref, ref, sizeof(out->ref));
    strlcpy(out->name, name->valuestring, sizeof(out->name));
    cJSON *it;
    cJSON_ArrayForEach(it, ranges) {
        if (out->count >= VP_MAX_RANGES) break;
        cJSON *f = cJSON_GetObjectItemCaseSensitive(it, "F");
        cJSON *t = cJSON_GetObjectItemCaseSensitive(it, "T");
        cJSON *y = cJSON_GetObjectItemCaseSensitive(it, "Y");
        if (!cJSON_IsString(f) || !cJSON_IsString(t)) continue;
        if (!isDate(f->valuestring) || !isDate(t->valuestring)) continue;
        VpRange *r = &out->r[out->count++];
        strlcpy(r->from, f->valuestring, sizeof(r->from));
        strlcpy(r->to,   t->valuestring, sizeof(r->to));
        r->yearly = cJSON_IsTrue(y);
    }
    cJSON_Delete(root);
    return true;
}

bool VP_Write(const ValidPeriod *vp)
{
    if (VP_RefIsNone(vp->ref) || vp->ref[0] != '/') return false;

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "VP_NAME", vp->name);
    cJSON *arr = cJSON_AddArrayToObject(root, "VP_RANGES");
    for (int i = 0; i < vp->count && i < VP_MAX_RANGES; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "F", vp->r[i].from);
        cJSON_AddStringToObject(o, "T", vp->r[i].to);
        cJSON_AddBoolToObject(o, "Y", vp->r[i].yearly);
        cJSON_AddItemToArray(arr, o);
    }
    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!txt) return false;

    char full[96];
    snprintf(full, sizeof(full), SPIFFS_BASE_PATH "%s", vp->ref);

    bool ok = false;
    if (xSemaphoreTake(variabMutex, portMAX_DELAY) == pdTRUE) {
        FILE *f = fopen(full, "w");
        if (f) {
            fputs(txt, f);
            fclose(f);
            ok = true;
        } else {
            ESP_LOGE(TAG, "Failed to write %s", full);
        }
        xSemaphoreGive(variabMutex);
    }
    free(txt);
    return ok;
}

void VP_NewRef(char *out, size_t outsz)
{
    struct stat st;
    char full[96];
    for (int n = 1; n < 1000; n++) {
        snprintf(full, sizeof(full), SPIFFS_BASE_PATH "/" VP_FILE_PREFIX "%d.json", n);
        if (stat(full, &st) != 0) {
            snprintf(out, outsz, "/" VP_FILE_PREFIX "%d.json", n);
            return;
        }
    }
    snprintf(out, outsz, "/" VP_FILE_PREFIX "999.json");
}

static int cmpSummary(const void *a, const void *b)
{
    return strcasecmp(((const VpSummary *)a)->name, ((const VpSummary *)b)->name);
}

int VP_List(VpSummary *out, int maxOut)
{
    DIR *dir = opendir(SPIFFS_BASE_PATH);
    if (!dir) return 0;

    // VP_Read is ~700 bytes of struct — keep it off the caller's stack.
    ValidPeriod *tmp = malloc(sizeof(ValidPeriod));
    if (!tmp) { closedir(dir); return 0; }

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && count < maxOut) {
        if (strncmp(ent->d_name, VP_FILE_PREFIX, strlen(VP_FILE_PREFIX)) != 0) continue;
        size_t nlen = strlen(ent->d_name);
        if (nlen < 6 || strcmp(ent->d_name + nlen - 5, ".json") != 0) continue;
        char ref[VP_REF_LEN + 8];
        snprintf(ref, sizeof(ref), "/%s", ent->d_name);
        if (strlen(ref) >= VP_REF_LEN) continue;
        if (!VP_Read(ref, tmp)) continue;
        strlcpy(out[count].ref, tmp->ref, sizeof(out[count].ref));
        strlcpy(out[count].name, tmp->name, sizeof(out[count].name));
        out[count].count = tmp->count;
        count++;
    }
    closedir(dir);
    free(tmp);

    qsort(out, count, sizeof(VpSummary), cmpSummary);
    return count;
}

bool VP_GetName(const char *ref, char *out, size_t outsz)
{
    out[0] = '\0';
    if (VP_RefIsNone(ref)) return false;
    ValidPeriod *tmp = malloc(sizeof(ValidPeriod));
    if (!tmp) return false;
    bool ok = VP_Read(ref, tmp);
    if (ok) strlcpy(out, tmp->name, outsz);
    free(tmp);
    return ok;
}

static bool rangeContains(const VpRange *r, const char *d)
{
    if (r->yearly) {
        const char *f = r->from + 5, *t = r->to + 5, *x = d + 5; // MM-DD
        if (strcmp(f, t) <= 0) return strcmp(x, f) >= 0 && strcmp(x, t) <= 0;
        return strcmp(x, f) >= 0 || strcmp(x, t) <= 0;              // wraps New Year
    }
    return strcmp(d, r->from) >= 0 && strcmp(d, r->to) <= 0;
}

bool VP_IsDateValid(const char *ref, const char *dateYMD)
{
    if (VP_RefIsNone(ref)) return true;
    if (!isDate(dateYMD)) return true;

    ValidPeriod *tmp = malloc(sizeof(ValidPeriod));
    if (!tmp) return true;
    bool valid;
    if (!VP_Read(ref, tmp)) {
        valid = true; // missing/unreadable period → behave like "None"
    } else {
        valid = false;
        for (int i = 0; i < tmp->count; i++) {
            if (rangeContains(&tmp->r[i], dateYMD)) { valid = true; break; }
        }
    }
    free(tmp);
    return valid;
}
