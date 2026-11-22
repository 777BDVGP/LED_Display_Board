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
#include "settings_store.h"
#include "time_sync.h"

static const char *TAG = "time_sync";

// POSIX TZ strings: the offset is west-of-UTC positive (UTC+2 is "-2"); the
// ",M..." rules switch daylight saving on and off automatically.
const TzOption g_tzOptions[] = {
    { "Greenwich (GMT / UTC, no daylight saving)",        "GMT0" },
    { "UK, Ireland, Portugal (GMT / BST)",                "GMT0BST,M3.5.0/1,M10.5.0" },
    { "Central Europe (CET / CEST)",                      "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Eastern Europe (EET / EEST)",                      "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "South Africa, Zimbabwe, Mozambique (SAST, UTC+2)", "SAST-2" },
    { "Namibia, Botswana, Zambia (CAT, UTC+2)",           "CAT-2" },
    { "Kenya, Tanzania, Uganda (EAT, UTC+3)",             "EAT-3" },
    { "Moscow, Turkey, Saudi Arabia (UTC+3)",             "<+03>-3" },
    { "Gulf: UAE, Oman (UTC+4)",                          "<+04>-4" },
    { "Pakistan (UTC+5)",                                 "PKT-5" },
    { "India, Sri Lanka (IST, UTC+5:30)",                 "IST-5:30" },
    { "Thailand, Vietnam, Indonesia West (UTC+7)",        "<+07>-7" },
    { "China, Singapore, Philippines, W. Australia (UTC+8)", "<+08>-8" },
    { "Japan, Korea (UTC+9)",                             "<+09>-9" },
    { "Queensland, Australia (AEST, UTC+10)",             "AEST-10" },
    { "Sydney, Melbourne, Canberra (AEST / AEDT)",        "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "New Zealand (NZST / NZDT)",                        "NZST-12NZDT,M9.5.0,M4.1.0/3" },
    { "Brazil: Sao Paulo, Rio (UTC-3)",                   "<-03>3" },
    { "Argentina (UTC-3)",                                "<-03>3" },
    { "US & Canada Eastern (EST / EDT)",                  "EST5EDT,M3.2.0,M11.1.0" },
    { "US & Canada Central (CST / CDT)",                  "CST6CDT,M3.2.0,M11.1.0" },
    { "US & Canada Mountain (MST / MDT)",                 "MST7MDT,M3.2.0,M11.1.0" },
    { "Arizona (MST, no daylight saving)",                "MST7" },
    { "US & Canada Pacific (PST / PDT)",                  "PST8PDT,M3.2.0,M11.1.0" },
    { "Alaska (AKST / AKDT)",                             "AKST9AKDT,M3.2.0,M11.1.0" },
    { "Hawaii (HST)",                                     "HST10" },
};
const int g_tzOptionCount = sizeof(g_tzOptions) / sizeof(g_tzOptions[0]);

void TimeSync_ApplyTimeZone(void)
{
    char tz[SETTING_VALUE_LEN];
    Settings_GetText(S_TIME_ZONE, tz, sizeof(tz));
    if (tz[0] == '\0') strlcpy(tz, "GMT0", sizeof(tz));
    setenv("TZ", tz, 1);
    tzset();
    ESP_LOGI(TAG, "Time zone: %s", tz);
}

void TimeSyncTask(void *parameter)
{
    (void)parameter;

    TimeSync_ApplyTimeZone();   // SETTINGS "Time Zone"

    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
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
