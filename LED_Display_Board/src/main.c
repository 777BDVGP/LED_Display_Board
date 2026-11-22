// =============================================================================
// main.c — Firmware Entry Point: app_main()
// =============================================================================
// ESP-IDF port of LED_Display_Board_ARD. Boots the LED Display Board firmware:
//   1. Setup I/Os for this System
//   2. Open the file system and initialize all variables from stored files
//   3. Connect to WiFi — saved networks (/WiFiConfig.json) or the setup
//      hotspot LEDClock-Setup (see wifi_setup.h; blocks until reboot)
//   4. Setup OTA
//   5. Create FreeRTOS Tasks - just the basic tasks without any contents
//
// Board:  ESP32 DevKitC
// PCB:    DevKitC_LED_DRIVER_V14
// =============================================================================

#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "app_config.h"
#include "app_logic_task.h"
#include "app_types.h"
#include "buzzer_task.h"
#include "datetime.h"
#include "display_task.h"
#include "exercises.h"
#include "files.h"
#include "globals.h"
#include "ntfy.h"
#include "ota.h"
#include "sensors.h"
#include "settings_store.h"
#include "system_io.h"
#include "time_sync.h"
#include "timer_engine.h"
#include "web_about.h"
#include "web_core.h"
#include "web_events.h"
#include "web_exercises.h"
#include "web_files.h"
#include "web_home.h"
#include "web_ntfy.h"
#include "web_server.h"
#include "web_settings.h"
#include "web_valid_periods.h"
#include "fw_update.h"
#include "wifi_setup.h"
#include "ws_hub.h"

static const char *TAG = "main";

// ─── Display liveness heartbeat ───────────────────────────────────────────────
volatile uint32_t g_displayHeartbeat = 0;   // see globals.h — watched by TimeCheckTask

// ─── FreeRTOS Primitives ──────────────────────────────────────────────────────
QueueHandle_t     displayQueue     = NULL;  // Sends DisplayMessage items to DisplayTask
QueueHandle_t     buzzerQueue      = NULL;  // Sends BuzzerMessage items to BuzzerTask
QueueHandle_t     eventSyncQueue   = NULL;  // Sends EventSyncCmd items to ClockNetworkTask
SemaphoreHandle_t timeSyncSemaphore;        // Released by TimeSyncTask when NTP is ready
SemaphoreHandle_t timersMutex;              // Guards Timer[] array
SemaphoreHandle_t eventSemaphore;           // Signals page-selection change events
SemaphoreHandle_t variabMutex;              // Guards shared application variables

// -----------------------------------------------------------------------------
// wifiConnectLedTick  (esp_timer periodic callback — mirrors ARD's Ticker +
// ISR_LED_Setup)
// Flashes LEDOUT every 100 ms while WiFi is connecting / the setup hotspot
// is up. SetupWiFi() ends every failed path in its own timed reboot; this is
// only a last-resort safety net (WIFI_CONNECTING_COUNTS, 20 min) in case it
// ever hangs.
// -----------------------------------------------------------------------------
static esp_timer_handle_t s_wifiConnectTimer;
static uint32_t           s_wifiTimeoutCounts = 0;

static void wifiConnectLedTick(void *arg)
{
    (void)arg;
    static bool level = true;
    level = !level;
    gpio_set_level(LEDOUT, level);

    if (++s_wifiTimeoutCounts >= WIFI_CONNECTING_COUNTS) {
        ESP_LOGE(TAG, "WIFI_TIMEOUT_COUNTS - Reboot");
        esp_restart();
    }
}

// =============================================================================
// Stub FreeRTOS Tasks
// =============================================================================
// All bodies are intentionally empty placeholders for this stage — they exist
// only to prove out core/priority assignment. Each is fleshed out in a later
// stage as its supporting module (WebSocket server, display driver, buzzer,
// event system, NTP sync, clock network) gets ported.

// TimeSyncTask (time_sync.c) and DisplayTask (display_task.c) are real now.
// -----------------------------------------------------------------------------
// WebSocketTask  (FreeRTOS task — Core 0, priority 6)
// Starts the shared HTTP+WebSocket server and lets each page module register
// its own routes onto it. Ported from ARD's SetupWebPage() (called once at
// boot there; here it's the first thing this task does, since ESP-IDF's
// httpd doesn't need a per-loop handleClient() poll like Arduino's WebServer).
// -----------------------------------------------------------------------------
static void WebSocketTask(void *parameter)
{
    (void)parameter;

    WebCore_LoadTheme();

    httpd_handle_t server = WebServer_Start();
    if (server) {
        WsHub_RegisterRoutes(server);
        WebCore_RegisterRoutes(server);
        WebHome_RegisterRoutes(server);
        WebSettings_RegisterRoutes(server);
        WebEvents_RegisterRoutes(server);
        WebNtfy_RegisterRoutes(server);
        WebAbout_RegisterRoutes(server);
        WebExercises_RegisterRoutes(server);
        WebValidPeriods_RegisterRoutes(server);
        FwUpdate_RegisterRoutes(server);
        FileWebPage_RegisterRoutes(server);
    }

    for (;;) { vTaskDelay(pdMS_TO_TICKS(1000)); }
}
// -----------------------------------------------------------------------------
// TimeCheckTask  (FreeRTOS task — Core 0, priority 1, 10 ms period)
// Blocks on timeSyncSemaphore, then drives three periodic ticks:
//   50 ms — Timers_Maintain() + heartbeat LED toggle
//    1 s  — CompileDateTime() + WebSocket date/time push (H02)
//   10 s  — WiFi health check; reboots if the connection is lost
// Also auto-clears the H03 status banner ~2s after WS_MessUpdate(..., true)
// signals eventSemaphore, matching ARD's page-selection event auto-clear.
// -----------------------------------------------------------------------------
static void TimeCheckTask(void *parameter)
{
    (void)parameter;

    if (xSemaphoreTake(timeSyncSemaphore, portMAX_DELAY) == pdTRUE) {
        xSemaphoreGive(timeSyncSemaphore);
    }

    TickType_t last50ms       = xTaskGetTickCount();
    TickType_t last1s         = last50ms;
    TickType_t lastWifiCheck  = last50ms;

    bool       eventTriggered    = false;
    TickType_t eventTriggerTime  = 0;

    // DisplayTask liveness watchdog state (checked every 5 s below).
    TickType_t lastDisplayCheck  = last50ms;
    uint32_t   lastHeartbeat     = 0;
    TickType_t lastHeartbeatMove = last50ms;

    for (;;) {
        TickType_t now = xTaskGetTickCount();

        if (!eventTriggered && xSemaphoreTake(eventSemaphore, 0) == pdTRUE) {
            eventTriggered   = true;
            eventTriggerTime = now;
        }
        if (eventTriggered && (now - eventTriggerTime) >= pdMS_TO_TICKS(2000)) {
            eventTriggered = false;
            WS_ParamUpdate("", "H03", "");
        }

        if ((now - last50ms) >= pdMS_TO_TICKS(50)) {
            Timers_Maintain();
            if (Timers_Expired(TIM_LED_FLASHING_SPEED)) {
                Timers_Activate(TIM_LED_FLASHING_SPEED);
                static bool ledState = false;
                ledState = !ledState;
                gpio_set_level(LEDOUT, ledState);
            }
            last50ms += pdMS_TO_TICKS(50);
        }

        if ((now - last1s) >= pdMS_TO_TICKS(1000)) {
            CompileDateTime();
            WS_ParamUpdate(DateTime, "H02", "");
            last1s += pdMS_TO_TICKS(1000);
        }

        if ((now - lastWifiCheck) >= pdMS_TO_TICKS(10000)) {
            static int wifiFailCount = 0;
            wifi_ap_record_t apInfo;
            if (esp_wifi_sta_get_ap_info(&apInfo) != ESP_OK) {
                wifiFailCount++;
                ESP_LOGW(TAG, "WiFi check failed (%d/3)", wifiFailCount);
                // Reboot only after 3 consecutive failed checks (~30s of real
                // disconnection) — a single missed AP-info query (e.g. a brief
                // roam/retry blip) used to reboot the device immediately, which
                // was re-arming the human-detect stay-on timer on every boot
                // and made the display look like it never switched off.
                if (wifiFailCount >= 3) {
                    ESP_LOGW(TAG, "WiFi disconnected!");
                    SaveGenReport("WIFI Not connected - Reboot");
                    vTaskDelay(pdMS_TO_TICKS(100));
                    esp_restart();
                }
            } else {
                wifiFailCount = 0;
            }
            lastWifiCheck += pdMS_TO_TICKS(10000);
        }

        // ── DisplayTask liveness watchdog ────────────────────────────────────
        // DisplayTask bumps g_displayHeartbeat every render loop and every
        // scroll step. If it stops advancing for ~45 s while no exercise is
        // running, DisplayTask is wedged (blocked on RMT / a mutex / the
        // queue) — the clock is frozen on its last frame and nothing else
        // recovers it, so reboot. Only armed once DisplayTask has cleared its
        // NTP-sync gate (heartbeat leaves 0); a >60 s uptime gate avoids a
        // boot loop.
        if ((now - lastDisplayCheck) >= pdMS_TO_TICKS(5000)) {
            uint32_t hb = g_displayHeartbeat;
            if (hb != lastHeartbeat) {
                lastHeartbeat     = hb;
                lastHeartbeatMove = now;
            } else if (hb != 0 && !exerciseActive && millis() > 60000UL &&
                       (now - lastHeartbeatMove) >= pdMS_TO_TICKS(45000)) {
                ESP_LOGE(TAG, "DisplayTask frozen ~%lus - Reboot",
                         (unsigned long)((now - lastHeartbeatMove) * portTICK_PERIOD_MS / 1000));
                SaveGenReport("DisplayTask frozen - Reboot");
                vTaskDelay(pdMS_TO_TICKS(100));
                esp_restart();
            }
            lastDisplayCheck += pdMS_TO_TICKS(5000);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// AppLogicTask (app_logic_task.c), BuzzerTask (buzzer_task.c), and
// ClockNetworkTask (web_about.c) are real now.

void app_main(void)
{
    ESP_LOGI(TAG, "BootUp Clock System");

    // ── Create FreeRTOS primitives ────────────────────────────────────────────
    displayQueue = xQueueCreate(10, sizeof(DisplayMessage));
    if (!displayQueue) { ESP_LOGE(TAG, "displayQueue creation failed!"); abort(); }

    buzzerQueue = xQueueCreate(5, sizeof(BuzzerMessage));
    if (!buzzerQueue) { ESP_LOGE(TAG, "buzzerQueue creation failed!"); abort(); }

    eventSyncQueue = xQueueCreate(10, sizeof(EventSyncCmd));
    if (!eventSyncQueue) { ESP_LOGE(TAG, "eventSyncQueue creation failed!"); abort(); }

    timeSyncSemaphore = xSemaphoreCreateBinary();
    timersMutex       = xSemaphoreCreateMutex();
    eventSemaphore    = xSemaphoreCreateBinary();
    variabMutex       = xSemaphoreCreateMutex();

    // Establish default settings before anything might read them (mirrors ARD's
    // global SettingClass constructor running before setup()).
    Settings_InitDefaults();

    // 1. Setup I/Os for this System
    SetupSystemIOs();
    Sensors_Init(); // ADC1 for the LDR (GPIO35) + radar rising-edge interrupt

    // ESP32 blue LED flashes fast while Setup / WiFi connects
    const esp_timer_create_args_t timerArgs = {
        .callback = &wifiConnectLedTick,
        .name     = "wifi_connect_led",
    };
    esp_timer_create(&timerArgs, &s_wifiConnectTimer);
    esp_timer_start_periodic(s_wifiConnectTimer, 100000); // 100 ms

    // 2. Open the file system and initialize all variables from stored files
    InitFileSystem();
    ClockNetwork_Load(); // /ClockNetwork.json — mirrors ARD's InitOperationalObjects() timing

    // 3. Connect to WiFi — saved networks, else the setup hotspot (see wifi_setup.h)
    SetupWiFi();
    esp_timer_stop(s_wifiConnectTimer);
    esp_timer_delete(s_wifiConnectTimer);
    ESP_LOGI(TAG, "WiFi Connected.");

    // 4. Setup OTA
    SetupOTA();

    // NTFY push notifications — needs WiFi, so this runs after SetupWiFi()
    // rather than being folded into step 2's file-system init.
    Ntfy_Init();

    // 5. Create FreeRTOS Tasks - Just the basic tasks without any contents
    // -------------------------------------------------------------------------
    // Core / Priority assignment mirrors LED_Display_Board_ARD's main.cpp:
    //
    // Core 0 — WiFi + application logic
    //   WebSocketTask    P6  Real — shared HTTP+WebSocket server: Home/Settings*/
    //                        Events*/Files pages + "/ws" (*Settings and Events
    //                        pages still stubbed pending their own tasks)
    //   TimeSyncTask     P1  Real — NTP sync (time_sync.c)
    //   TimeCheckTask    P1  Real — timers/heartbeat/CompileDateTime/WiFi check
    //   AppLogicTask     P1  Real — fires due events into displayQueue (app_logic_task.c)
    //   BuzzerTask       P1  Real — beep-pattern state machine (buzzer_task.c)
    //   ClockNetworkTask P1  Real — peer probe + event sync (web_about.c)
    //   OTATask          P2  Real — services the OTA protocol (see ota.c)
    //
    // Core 1 — Display only
    //   DisplayTask      P5  Real — RMT-driven LED matrix; clock face + queued
    //                        event scroll (display_task.c / led_matrix.c / font5x7.c)
    // -------------------------------------------------------------------------
    xTaskCreatePinnedToCore(TimeSyncTask,      "TimeSyncTask",   4096, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(WebSocketTask,     "WebSocketTask", 16384, NULL, 6, NULL, 0);
    // 16384: CompileDateTime() -> Event_RefreshTodayList() -> Event_GetList() walks
    // every SPIFFS file with a ~1.5KB EventRecord local + cJSON parsing per file —
    // 8192 overflowed here (LoadProhibited from a stack-poisoned pointer) once that
    // call chain existed; the original 8192 only ever needed to cover a lightweight tick.
    xTaskCreatePinnedToCore(TimeCheckTask,     "TimeCheckTask", 16384, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(AppLogicTask,      "AppLogicTask",   4096, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(DisplayTask,       "DisplayTask",   20480, NULL, 5, NULL, 1);
    xTaskCreatePinnedToCore(BuzzerTask,        "BuzzerTask",     4096, NULL, 1, NULL, 0);
    xTaskCreatePinnedToCore(OTATask,           "OTATask",        4096, NULL, 2, NULL, 0);
    // 16384: same class of issue as WebSocketTask/TimeCheckTask above —
    // executeEventSync() (web_about.c) has a 4KB local body[] buffer plus
    // esp_http_client (httpGetText/httpPostBody) + opendir/readdir; 6144
    // overflowed within seconds of boot ("A stack overflow in task
    // ClockNetTask has been detected"), forcing a reboot loop that kept
    // re-arming the human-detect stay-on timer via HumanDetect_ActivateStayOn("boot").
    xTaskCreatePinnedToCore(ClockNetworkTask,  "ClockNetTask",  16384, NULL, 1, NULL, 0);

     SaveGenReport("Up and running afte reboot");
}
