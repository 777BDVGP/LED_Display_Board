// =============================================================================
// web_files.h — SPIFFS File Manager Web Server
// =============================================================================
// Port of LED_Display_Board_ARD's 03_Files.cpp file-manager UI (FilesPage /
// handleApiFiles / handleFileDownload / handleFileView / handleFileDeleteAJAX
// / handleFileUpload). Registers onto the shared server (see web_server.h) —
// "/" now belongs to web_home.c's real Home page; this owns "/files" only.
// =============================================================================

#ifndef WEB_FILES_H
#define WEB_FILES_H

#include "esp_http_server.h"

// Registers "/files" and its AJAX API on the shared server.
void FileWebPage_RegisterRoutes(httpd_handle_t server);

#endif // WEB_FILES_H
