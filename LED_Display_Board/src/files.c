// =============================================================================
// files.c — SPIFFS File System Setup
// =============================================================================
// Mounts SPIFFS and, on a normal (non-first) boot, loads persisted application
// state. Ported from LED_Display_Board_ARD's 03_Files.cpp (InitFileSystem /
// InitOperationalObjects).
// =============================================================================

#include <stdio.h>
#include <sys/stat.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_spiffs.h"

#include "datetime.h"
#include "files.h"
#include "settings_store.h"

static const char *TAG = "files";

#define FORMAT_COMPLETE_PATH  SPIFFS_BASE_PATH "/FormatComplete.txt"

// -----------------------------------------------------------------------------
// InitOperationalObjects
// Loads all persisted application objects from SPIFFS JSON files.
// Called by InitFileSystem() on a normal (non-first-boot) boot.
//
// TODO(Phase D/E): LoadTheme() / LoadClockNetwork() from ARD's 03_Files.cpp /
// 11_About.cpp once those modules are ported.
// -----------------------------------------------------------------------------
static void InitOperationalObjects(void)
{
    CreateSettingObject();
}

// -----------------------------------------------------------------------------
// SaveGenReport
// Appends a timestamped message to /spiffs/GeneralEvents.txt. Logging is
// gated by the S_SAVELOG setting so it can be disabled at runtime.
// -----------------------------------------------------------------------------
void SaveGenReport(const char *message)
{
    if (!Settings_GetBool(S_SAVELOG)) return;

    char stamp[20];
    GetCurrentDateAndTime(stamp, sizeof(stamp));

    FILE *f = fopen(SPIFFS_BASE_PATH "/GeneralEvents.txt", "a");
    if (!f) return;
    fprintf(f, "%s %s\n", stamp, message);
    fclose(f);
}

// -----------------------------------------------------------------------------
// InitFileSystem
// Mounts SPIFFS (auto-formatting on first mount / corruption). If the
// format-sentinel file (/spiffs/FormatComplete.txt) is absent, this boot is
// treated as a fresh start: the sentinel is written and InitOperationalObjects()
// is skipped. On a normal boot (sentinel present), InitOperationalObjects() is
// called to load all persisted application state.
// Called once from app_main() before any networking is set up.
// -----------------------------------------------------------------------------
void InitFileSystem(void)
{
    ESP_LOGI(TAG, "Mounting SPIFFS...");

    esp_vfs_spiffs_conf_t conf = {
        .base_path              = SPIFFS_BASE_PATH,
        .partition_label        = NULL,
        .max_files               = 8,
        .format_if_mount_failed = true,
    };

    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount or format SPIFFS");
        } else if (ret == ESP_ERR_NOT_FOUND) {
            ESP_LOGE(TAG, "Failed to find SPIFFS partition");
        } else {
            ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
        }
        return;
    }

    size_t total = 0, used = 0;
    if (esp_spiffs_info(conf.partition_label, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "SPIFFS: %u / %u bytes used", (unsigned)used, (unsigned)total);
    }

    struct stat st;
    if (stat(FORMAT_COMPLETE_PATH, &st) != 0) {
        ESP_LOGI(TAG, "First boot - writing format sentinel");
        FILE *f = fopen(FORMAT_COMPLETE_PATH, "w");
        if (f) {
            fputs("Deleting this file will re-format SPIFFS.\n", f);
            fclose(f);
            ESP_LOGI(TAG, "FormatComplete.txt created");
        } else {
            ESP_LOGE(TAG, "Failed to create %s", FORMAT_COMPLETE_PATH);
        }
    } else {
        ESP_LOGI(TAG, "File system started");
        InitOperationalObjects();
    }
}
