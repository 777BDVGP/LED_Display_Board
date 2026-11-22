// =============================================================================
// fw_update.h — Firmware version + update from GitHub / browser upload
// =============================================================================
// Same model as the RainGauge project:
//   CHECK FOR UPDATE    reads FW_UPDATE_URL "version.json" from GitHub Pages:
//                       {"version":"1.0.1","bin":"firmware/ledclock.bin","notes":".."}
//   INSTALL FROM GITHUB downloads that .bin over HTTPS (certificate checked
//                       against the ESP-IDF CA bundle) into the spare OTA
//                       slot, then restarts. A failed or incomplete download
//                       leaves the running firmware untouched.
//   UPLOAD & INSTALL    the same from a .bin chosen in the browser (no internet).
// Settings and all event files are kept (they live in SPIFFS, not the app).
// =============================================================================

#ifndef FW_UPDATE_H
#define FW_UPDATE_H

#include "esp_http_server.h"

#define FW_VERSION     "1.0.1"
#define FW_UPDATE_URL  "https://777bdvgp.github.io/LED_Display_Board/"

// Registers POST /fwcheck, POST /fwinstall, GET /fwstatus, POST /fwupload.
void FwUpdate_RegisterRoutes(httpd_handle_t server);

// Call once right after SetupWiFi(), before the other tasks start: performs
// an INSTALL FROM GITHUB that was requested before the last restart (fresh,
// unfragmented heap for the TLS download) and restarts into it on success.
void FwUpdate_RunPendingInstall(void);

// Streams the FIRMWARE section of the ABOUT page.
esp_err_t FwUpdate_SendAboutSection(httpd_req_t *req);

#endif // FW_UPDATE_H
