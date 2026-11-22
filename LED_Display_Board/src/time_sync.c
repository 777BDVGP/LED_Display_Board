// =============================================================================
// time_sync.c — NTP Time Synchronisation
// =============================================================================
// See time_sync.h.
// =============================================================================

#include <stdlib.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"

#include "globals.h"
#include "time_sync.h"

static const char *TAG = "time_sync";

void TimeSyncTask(void *parameter)
{
    (void)parameter;

    // South Africa Standard Time, UTC+2, no DST — matches ARD's 02_Time.cpp
    // (gmtOffset_sec = 2*3600, daylightOffset_sec = 0). POSIX TZ offsets are
    // inverted (west-of-UTC positive), so UTC+2 is written "-2".
    setenv("TZ", "SAST-2", 1);
    tzset();

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("za.pool.ntp.org");
    esp_netif_sntp_init(&config);

    ESP_LOGI(TAG, "Waiting for NTP time sync...");
    while (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(2000)) == ESP_ERR_TIMEOUT) {
        ESP_LOGI(TAG, "Still waiting for NTP time sync...");
    }

    time_t now = time(NULL);
    struct tm ti;
    localtime_r(&now, &ti);
    ESP_LOGI(TAG, "NTP time synced: %04d-%02d-%02d %02d:%02d:%02d",
             ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday, ti.tm_hour, ti.tm_min, ti.tm_sec);

    xSemaphoreGive(timeSyncSemaphore);

    // SNTP resyncs itself periodically in the background — nothing more to do.
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(3600000));
    }
}
