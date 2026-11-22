// =============================================================================
// exercises.c — Exercise Program: Data Model & LED Matrix Runner
// =============================================================================
// See exercises.h.
// =============================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "cJSON.h"

#include "buzzer_task.h"
#include "event_store.h" // BUZ_* pattern strings
#include "exercises.h"
#include "files.h"
#include "font5x7.h"
#include "globals.h"
#include "led_matrix.h"

volatile bool  exerciseActive  = false;
volatile bool  exerciseStop    = false;
ExerciseData   pendingExercise = { 0 };

// =============================================================================
// SPIFFS Helpers
// =============================================================================

void Exercise_FindNextFilename(char *out, size_t outsz)
{
    out[0] = '\0';
    for (int i = 1; i <= EX_MAX_SLOTS; i++) {
        char rel[16];
        snprintf(rel, sizeof(rel), "/Exer_%02d.json", i);
        char full[64];
        snprintf(full, sizeof(full), "%s%s", SPIFFS_BASE_PATH, rel);
        struct stat st;
        if (stat(full, &st) != 0) {
            strlcpy(out, rel, outsz);
            return;
        }
    }
}

bool Exercise_LoadFromSPIFFS(const char *filename, ExerciseData *ex)
{
    memset(ex, 0, sizeof(*ex));

    char full[64];
    snprintf(full, sizeof(full), "%s%s", SPIFFS_BASE_PATH, filename);
    FILE *f = fopen(full, "r");
    if (!f) return false;

    char buf[900];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    cJSON *doc = cJSON_Parse(buf);
    if (!doc) return false;

    cJSON *item;
    if ((item = cJSON_GetObjectItemCaseSensitive(doc, "name")) && cJSON_IsString(item))
        strlcpy(ex->name, item->valuestring, sizeof(ex->name));
    if ((item = cJSON_GetObjectItemCaseSensitive(doc, "startPos")) && cJSON_IsString(item))
        strlcpy(ex->startPos, item->valuestring, sizeof(ex->startPos));
    if ((item = cJSON_GetObjectItemCaseSensitive(doc, "setInstr")) && cJSON_IsString(item))
        strlcpy(ex->setInstr, item->valuestring, sizeof(ex->setInstr));
    strlcpy(ex->filename, filename, sizeof(ex->filename));

    #define GETINT(key, field, def) \
        ex->field = ((item = cJSON_GetObjectItemCaseSensitive(doc, key)) && cJSON_IsNumber(item)) ? item->valueint : (def)
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
            strlcpy(ex->m[i], item->valuestring, sizeof(ex->m[i]));
        ex->t[i] = ((item = cJSON_GetObjectItemCaseSensitive(doc, tk)) && cJSON_IsNumber(item)) ? item->valueint : 2;
    }

    cJSON_Delete(doc);
    return true;
}

bool Exercise_SaveToSPIFFS(const ExerciseData *ex)
{
    cJSON *doc = cJSON_CreateObject();
    cJSON_AddStringToObject(doc, "name", ex->name);
    cJSON_AddNumberToObject(doc, "moves", ex->moves);
    cJSON_AddNumberToObject(doc, "sets", ex->sets);
    cJSON_AddNumberToObject(doc, "setsWait", ex->setsWait);
    cJSON_AddStringToObject(doc, "setInstr", ex->setInstr);
    cJSON_AddNumberToObject(doc, "setInstrSpeed", ex->setInstrSpeed);
    cJSON_AddNumberToObject(doc, "setInstrWait", ex->setInstrWait);
    cJSON_AddNumberToObject(doc, "reps", ex->reps);
    cJSON_AddStringToObject(doc, "startPos", ex->startPos);
    cJSON_AddNumberToObject(doc, "startSpeed", ex->startSpeed);
    for (int i = 0; i < 4; i++) {
        char mk[4], tk[4];
        snprintf(mk, sizeof(mk), "m%d", i + 1);
        snprintf(tk, sizeof(tk), "t%d", i + 1);
        cJSON_AddStringToObject(doc, mk, ex->m[i]);
        cJSON_AddNumberToObject(doc, tk, ex->t[i]);
    }

    char *out = cJSON_Print(doc);
    cJSON_Delete(doc);
    if (!out) return false;

    char full[64];
    snprintf(full, sizeof(full), "%s%s", SPIFFS_BASE_PATH, ex->filename);
    bool ok = false;
    FILE *f = fopen(full, "w");
    if (f) {
        fputs(out, f);
        fclose(f);
        ok = true;
    }
    free(out);
    return ok;
}

// =============================================================================
// WebSocket Actions
// =============================================================================

void ExerciseStart(const char *filename)
{
    if (!filename || filename[0] == '\0') return;

    char full[64];
    snprintf(full, sizeof(full), "%s%s", SPIFFS_BASE_PATH, filename);
    struct stat st;
    if (stat(full, &st) != 0) return;

    if (xSemaphoreTake(variabMutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        exerciseStop = false;
        Exercise_LoadFromSPIFFS(filename, &pendingExercise);
        xSemaphoreGive(variabMutex);
    }
    exerciseActive = true; // signal DisplayTask to run the exercise
}

void ExerciseStop(void)
{
    exerciseStop = true;
}

// =============================================================================
// runExercise
// =============================================================================
// Executes one exercise session on the LED matrix. Called directly from
// DisplayTask when exerciseActive is set — DisplayTask's clock loop is
// suspended for the duration so the matrix belongs exclusively to this
// function, matching ARD's single-writer design.
// =============================================================================

static void scrollTextOnce(const char *text, int scrollDelay, uint8_t r, uint8_t g, uint8_t b)
{
    int textWidth = (int)strlen(text) * 6;
    int startX = MTX_WIDTH;
    int stopX  = MTX_WIDTH - textWidth;
    if (stopX > 0) stopX = 0; // short text: stop left-aligned

    for (int x = startX; x > stopX && !exerciseStop; x--) {
        matrix_clear();
        font_draw_text(text, x, 0, r, g, b);
        matrix_show();
        vTaskDelay(pdMS_TO_TICKS(scrollDelay));
    }
}

void runExercise(const ExerciseData *ex)
{
    // ── (1) Scroll starting position (yellow) ───────────────────────────────
    if (ex->startPos[0] != '\0' && !exerciseStop) {
        int scrollDelay = (ex->startSpeed > 0) ? ex->startSpeed : EX_SCROLL_DEFAULT;
        scrollTextOnce(ex->startPos, scrollDelay, 255, 255, 0);
        if (!exerciseStop) vTaskDelay(pdMS_TO_TICKS(1000)); // hold last word before reps begin
    }

    int totalSets = (ex->sets > 0) ? ex->sets : 1;

    for (int set = 1; set <= totalSets && !exerciseStop; set++) {
        // Scroll Set Instruction (before Set 2, 3, ... — skipped for Set 1)
        if (set > 1 && ex->setInstr[0] != '\0' && !exerciseStop) {
            int scrollDelay = (ex->setInstrSpeed > 0) ? ex->setInstrSpeed : EX_SCROLL_DEFAULT;
            scrollTextOnce(ex->setInstr, scrollDelay, 255, 255, 0);
            int holdMs = (ex->setInstrWait > 0) ? ex->setInstrWait * 1000 : 800;
            for (int h = 0; h < holdMs && !exerciseStop; h += 100) vTaskDelay(pdMS_TO_TICKS(100));
        }

        // "Set X/N" (orange) — only when sets > 1
        if (totalSets > 1) {
            char setTxt[32];
            snprintf(setTxt, sizeof(setTxt), "Set %d/%d", set, totalSets);
            int setX = (MTX_WIDTH - (int)strlen(setTxt) * 6) / 2;
            if (setX < 0) setX = 0;

            triggerBuzzer(BUZ_EX_SET);
            matrix_clear();
            font_draw_text(setTxt, setX, 0, 255, 140, 0);
            matrix_show();

            int waitMs = (ex->setsWait > 0) ? ex->setsWait * 1000 : 800;
            for (int w = 0; w < waitMs && !exerciseStop; w += 100) vTaskDelay(pdMS_TO_TICKS(100));
            if (exerciseStop) break;
        }

        // Rep loop for this set
        for (int rep = 1; rep <= ex->reps && !exerciseStop; rep++) {
            if (ex->reps > 1) {
                char repTxt[24];
                snprintf(repTxt, sizeof(repTxt), "Rep %d", rep);
                int repX = (MTX_WIDTH - (int)strlen(repTxt) * 6) / 2;
                if (repX < 0) repX = 0;

                matrix_clear();
                font_draw_text(repTxt, repX, 0, 0, 210, 180);
                matrix_show();
                vTaskDelay(pdMS_TO_TICKS(800));
                if (exerciseStop) break;
            }

            static const char *const buzPatterns[4] = { BUZ_EX1, BUZ_EX2, BUZ_EX3, BUZ_EX4 };
            for (int m = 0; m < ex->moves && !exerciseStop; m++) {
                triggerBuzzer(buzPatterns[m]);

                int nameX = (MTX_WIDTH - (int)strlen(ex->m[m]) * 6) / 2;
                if (nameX < 0) nameX = 0;

                matrix_clear();
                // Movements 1,3 (index 0,2) = Yellow; Movements 2,4 (index 1,3) = Green
                if (m % 2 == 0) font_draw_text(ex->m[m], nameX, 0, 255, 255, 0);
                else            font_draw_text(ex->m[m], nameX, 0, 0, 220, 0);
                matrix_show();

                for (int s = 0; s < ex->t[m] && !exerciseStop; s++) vTaskDelay(pdMS_TO_TICKS(1000));
            }
        }
    }

    // ── (3) All Done (only if completed, not stopped) ───────────────────────
    triggerBuzzer(BUZ_NONE); // stop any lingering movement beep
    if (!exerciseStop) {
        static const char doneTxt[] = "All Done";
        int doneX = (MTX_WIDTH - (int)strlen(doneTxt) * 6) / 2;
        if (doneX < 0) doneX = 0;
        triggerBuzzer(BUZ_EX_DONE); // 3 x 0.5s beeps simultaneous with All Done display
        matrix_clear();
        font_draw_text(doneTxt, doneX, 0, 255, 140, 0);
        matrix_show();
        vTaskDelay(pdMS_TO_TICKS(2600)); // hold until all 3 beeps finish (2.5s) + buffer
    }
    matrix_clear();
    matrix_show();
}
