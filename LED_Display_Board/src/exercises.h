// =============================================================================
// exercises.h — Exercise Program: Data Model & LED Matrix Runner
// =============================================================================
// ESP-IDF port of LED_Display_Board_ARD's 14_Exercises.h/.cpp data model and
// runExercise() sequencer. Each saved exercise is one /Exer_NN.json file on
// SPIFFS (root-relative paths throughout, matching event_store.h's
// convention — callers doing real file I/O must prepend SPIFFS_BASE_PATH).
// Web page / AJAX handlers live in web_exercises.h/.c.
// =============================================================================

#ifndef EXERCISES_H
#define EXERCISES_H

#include <stdbool.h>
#include <stddef.h>

#define EX_FILE_PREFIX   "/Exer_"
#define EX_FILE_SUFFIX   ".json"
#define EX_MAX_SLOTS     99

#define EX_NAME_MAX      24
#define EX_STARTPOS_MAX  64
#define EX_SETINSTR_MAX  60
#define EX_MOVE_MAX       8
#define EX_FILENAME_MAX  14

#define EX_SCROLL_DEFAULT 25 // ms/pixel used when a speed field is 0 (legacy/unset)

typedef struct {
    char name[EX_NAME_MAX + 1];
    int  moves;               // 1-4
    int  sets;                // number of sets (default 1)
    int  setsWait;            // pause between sets in seconds (default 0)
    char setInstr[EX_SETINSTR_MAX + 1]; // instruction scrolled between sets (may be empty)
    int  setInstrSpeed;       // ms/pixel
    int  setInstrWait;        // seconds to hold setInstr after scroll (0 = 0.8s minimum)
    int  reps;                // repetitions per set
    char startPos[EX_STARTPOS_MAX + 1];
    int  startSpeed;          // ms/pixel
    char m[4][EX_MOVE_MAX + 1]; // movement names
    int  t[4];                  // movement hold times (seconds)
    char filename[EX_FILENAME_MAX + 1]; // "/Exer_NN.json"
} ExerciseData;

// ─── Global Exercise State ────────────────────────────────────────────────────
extern volatile bool  exerciseActive;   // true while DisplayTask is running an exercise
extern volatile bool  exerciseStop;     // set true to abort the current exercise
extern ExerciseData   pendingExercise;  // exercise to run next (written under variabMutex)

// ─── SPIFFS Helpers ───────────────────────────────────────────────────────────
// Writes the lowest unused "/Exer_NN.json" root-relative path into out, or ""
// if all 99 slots are full.
void Exercise_FindNextFilename(char *out, size_t outsz);
bool Exercise_LoadFromSPIFFS(const char *filename, ExerciseData *ex);
bool Exercise_SaveToSPIFFS(const ExerciseData *ex);

// ─── WebSocket Actions & DisplayTask Runner ───────────────────────────────────
void ExerciseStart(const char *filename); // signal DisplayTask to start an exercise
void ExerciseStop(void);                  // abort the running exercise
void runExercise(const ExerciseData *ex); // sequence runner — called by DisplayTask

#endif // EXERCISES_H
