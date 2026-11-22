// =============================================================================
// ntfy.h — ntfy.sh Push Notifications
// =============================================================================
// ESP-IDF port of LED_Display_Board_ARD's 12_NTFY.cpp (MessageNTFY/NtfyTask).
// Persistence: /NTFY.json on SPIFFS. Push via FreeRTOS queue + dedicated task,
// same decoupled-from-the-caller design ARD uses (event firing never blocks
// on an HTTP POST).
// =============================================================================

#ifndef NTFY_H
#define NTFY_H

#include <stdbool.h>
#include <stddef.h>

#define NTFY_CODE_LEN   16
#define NTFY_TOPIC_LEN  64

// Current configuration — loaded by Ntfy_Init(), written by UpdateNtfyPageValues().
extern char g_ntfyCode[NTFY_CODE_LEN];       // NO_NTFY / BOTH_NTFYS / <topic1> / <topic2> — default topic selection
extern char g_ntfyPriority[NTFY_CODE_LEN];   // min / low / default / high / urgent
extern char g_ntfyTopic1[NTFY_TOPIC_LEN];
extern char g_ntfyTopic2[NTFY_TOPIC_LEN];
extern bool g_ntfyEnabled;                   // master on/off switch — independent of g_ntfyCode

// Loads /NTFY.json (writing back defaults for any missing keys), creates the
// send queue, and starts NtfyTask. Call once, after WiFi connects.
void Ntfy_Init(void);

void Ntfy_WriteData(void);
void Ntfy_ReadData(void);

// Queues a push notification and returns immediately (silently dropped if the
// queue is full, WiFi is down, g_ntfyEnabled is false, or NTFY is
// globally/per-topic disabled).
// topic: "NO_NTFY" (dropped) / "BOTH_NTFYS" (both configured topics) / a
// literal ntfy.sh topic name.
void Ntfy_Send(const char *message, const char *priority, const char *topic);

// FreeRTOS task body — pop the queue, POST to http://ntfy.sh/<topic>.
void NtfyTask(void *pvParameters);

#endif // NTFY_H
