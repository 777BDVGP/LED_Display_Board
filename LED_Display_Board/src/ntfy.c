// =============================================================================
// ntfy.c — ntfy.sh Push Notifications
// =============================================================================
// See ntfy.h.
// =============================================================================

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_wifi.h"

#include "files.h"
#include "ntfy.h"

static const char *TAG = "ntfy";

#define NTFY_FILE_PATH  "/NTFY.json"
#define NTFY_MSG_LEN    256

char g_ntfyCode[NTFY_CODE_LEN]      = "BOTH_NTFYS";
char g_ntfyPriority[NTFY_CODE_LEN]  = "default";
// Default topics until set on the NTFY page. A private build takes them from
// the git-ignored wifi_secrets.h; the public firmware starts with none.
#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#endif
#ifndef NTFY_DEFAULT_TOPIC1
#define NTFY_DEFAULT_TOPIC1 ""
#endif
#ifndef NTFY_DEFAULT_TOPIC2
#define NTFY_DEFAULT_TOPIC2 ""
#endif
char g_ntfyTopic1[NTFY_TOPIC_LEN]   = NTFY_DEFAULT_TOPIC1;
char g_ntfyTopic2[NTFY_TOPIC_LEN]   = NTFY_DEFAULT_TOPIC2;
bool g_ntfyEnabled                  = true;

typedef struct {
    char message[NTFY_MSG_LEN];
    char priority[NTFY_CODE_LEN];
    char code[NTFY_TOPIC_LEN];
    char code2[NTFY_TOPIC_LEN]; // second topic for BOTH_NTFYS; empty = single send
} NtfyMsg;

static QueueHandle_t s_ntfyQueue = NULL;

// =============================================================================
// Persistence  (/NTFY.json)
// =============================================================================

void Ntfy_WriteData(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "NtfyCode", g_ntfyCode);
    cJSON_AddStringToObject(root, "NtfyPriority", g_ntfyPriority);
    cJSON_AddStringToObject(root, "NtfyTopic1", g_ntfyTopic1);
    cJSON_AddStringToObject(root, "NtfyTopic2", g_ntfyTopic2);
    cJSON_AddBoolToObject(root, "NtfyEnabled", g_ntfyEnabled);

    char *out = cJSON_Print(root);
    cJSON_Delete(root);
    if (!out) return;

    char path[64];
    snprintf(path, sizeof(path), "%s%s", SPIFFS_BASE_PATH, NTFY_FILE_PATH);
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(out, f);
        fclose(f);
    } else {
        ESP_LOGW(TAG, "Failed to write NTFY.json");
    }
    free(out);
}

void Ntfy_ReadData(void)
{
    char path[64];
    snprintf(path, sizeof(path), "%s%s", SPIFFS_BASE_PATH, NTFY_FILE_PATH);
    FILE *f = fopen(path, "r");
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0 || size > 512) {
        fclose(f);
        return;
    }

    char buf[513];
    size_t rd = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[rd] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        ESP_LOGW(TAG, "NTFY.json parse error");
        return;
    }

    cJSON *item;
    if ((item = cJSON_GetObjectItemCaseSensitive(root, "NtfyCode")) && cJSON_IsString(item))
        strlcpy(g_ntfyCode, item->valuestring, sizeof(g_ntfyCode));
    if ((item = cJSON_GetObjectItemCaseSensitive(root, "NtfyPriority")) && cJSON_IsString(item))
        strlcpy(g_ntfyPriority, item->valuestring, sizeof(g_ntfyPriority));
    if ((item = cJSON_GetObjectItemCaseSensitive(root, "NtfyTopic1")) && cJSON_IsString(item))
        strlcpy(g_ntfyTopic1, item->valuestring, sizeof(g_ntfyTopic1));
    if ((item = cJSON_GetObjectItemCaseSensitive(root, "NtfyTopic2")) && cJSON_IsString(item))
        strlcpy(g_ntfyTopic2, item->valuestring, sizeof(g_ntfyTopic2));
    if ((item = cJSON_GetObjectItemCaseSensitive(root, "NtfyEnabled")) && cJSON_IsBool(item))
        g_ntfyEnabled = cJSON_IsTrue(item);

    cJSON_Delete(root);
}

// =============================================================================
// Push — FreeRTOS Queue + Task
// =============================================================================

static void ntfyPost(const char *code, const char *message, const char *priority)
{
    char url[96];
    snprintf(url, sizeof(url), "http://ntfy.sh/%s", code);

    esp_http_client_config_t config = {
        .url        = url,
        .method     = HTTP_METHOD_POST,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGW(TAG, "NTFY: http client init failed");
        return;
    }
    esp_http_client_set_header(client, "Content-Type", "text/plain");
    esp_http_client_set_header(client, "X-Priority", priority);
    esp_http_client_set_post_field(client, message, strlen(message));

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "NTFY POST -> %d (%s)", esp_http_client_get_status_code(client), code);
    } else {
        ESP_LOGW(TAG, "NTFY POST failed: %s (%s)", esp_err_to_name(err), code);
    }
    esp_http_client_cleanup(client);
}

void NtfyTask(void *pvParameters)
{
    (void)pvParameters;
    NtfyMsg msg;
    for (;;) {
        if (xQueueReceive(s_ntfyQueue, &msg, pdMS_TO_TICKS(1000)) == pdTRUE) {
            if (msg.code[0]  != '\0') ntfyPost(msg.code,  msg.message, msg.priority);
            if (msg.code2[0] != '\0') ntfyPost(msg.code2, msg.message, msg.priority);
        }
    }
}

void Ntfy_Send(const char *message, const char *priority, const char *topic)
{
    if (s_ntfyQueue == NULL)                 return;
    if (!g_ntfyEnabled)                       return; // master on/off checkbox
    if (strcmp(g_ntfyCode, "NO_NTFY") == 0)   return; // global kill-switch for this clock
    if (strcmp(topic, "NO_NTFY") == 0)        return;

    wifi_ap_record_t apInfo;
    if (esp_wifi_sta_get_ap_info(&apInfo) != ESP_OK) return;

    NtfyMsg msg;
    memset(&msg, 0, sizeof(msg));
    strlcpy(msg.message,  message,  sizeof(msg.message));
    strlcpy(msg.priority, priority, sizeof(msg.priority));

    if (strcmp(topic, "BOTH_NTFYS") == 0) {
        strlcpy(msg.code,  g_ntfyTopic1, sizeof(msg.code));
        strlcpy(msg.code2, g_ntfyTopic2, sizeof(msg.code2));
    } else {
        strlcpy(msg.code, topic, sizeof(msg.code));
        // code2 stays empty (zeroed by memset) — single send
    }

    if (xQueueSend(s_ntfyQueue, &msg, 0) == pdTRUE) {
        ESP_LOGI(TAG, "Ntfy_Send: queued");
    } else {
        ESP_LOGW(TAG, "Ntfy_Send: queue full");
    }
}

void Ntfy_Init(void)
{
    Ntfy_ReadData();
    Ntfy_WriteData(); // writes back defaults for any missing keys, matches ARD's CreateNtfyParams()

    s_ntfyQueue = xQueueCreate(1, sizeof(NtfyMsg));
    BaseType_t res = xTaskCreatePinnedToCore(NtfyTask, "NtfyTask", 4096, NULL, 1, NULL, 0);
    if (res == pdPASS) {
        ESP_LOGI(TAG, "NtfyTask created OK");
    } else {
        ESP_LOGE(TAG, "NtfyTask creation FAILED");
    }
}
