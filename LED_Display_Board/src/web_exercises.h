// =============================================================================
// web_exercises.h — Exercises Web Page & AJAX Handlers
// =============================================================================
// ESP-IDF port of LED_Display_Board_ARD's 14_Exercises.cpp web page handlers.
// =============================================================================

#ifndef WEB_EXERCISES_H
#define WEB_EXERCISES_H

#include "esp_http_server.h"

// Registers "/exercises", "/ex_save", "/ex_load", "/ex_delete", "/ex_status",
// "/createCSVExercises", "/loadFromCSVExercises" on the shared server.
void WebExercises_RegisterRoutes(httpd_handle_t server);

#endif // WEB_EXERCISES_H
