// =============================================================================
// ota.c — Over-The-Air Firmware Update (ArduinoOTA-compatible receiver)
// =============================================================================
// Reimplements the ArduinoOTA wire protocol (see arduino-esp32's
// libraries/ArduinoOTA/src/ArduinoOTA.cpp and tools/espota.py) directly on
// top of ESP-IDF's esp_ota_ops + lwip sockets, so PlatformIO's
// `pio run -t upload --upload-port <device_ip>` keeps working against this
// firmware exactly as it does against LED_Display_Board_ARD.
//
// Protocol:
//   1. Host (espota.py) sends a UDP datagram to this device's port 3232:
//        "<cmd> <hostTcpPort> <sizeBytes> <md5Hex>\n"
//      cmd: 0 = flash app image, 100 = SPIFFS image (not yet supported here).
//   2. No password is configured (ARD never calls ArduinoOTA.setPassword()),
//      so we reply "OK" over UDP immediately — no AUTH challenge.
//   3. We open a TCP connection back to the sender's IP : hostTcpPort.
//   4. Host streams the firmware in ~1024-byte chunks; after each chunk we
//      write it to the OTA partition and reply with the cumulative byte
//      count as ASCII decimal (matches Arduino's Update-class flow control).
//   5. Once all bytes are received, we verify the running MD5, call
//      esp_ota_end() + esp_ota_set_boot_partition(), reply "OK", and reboot.
// =============================================================================

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>

#include "lwip/netdb.h"
#include "lwip/sockets.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "mbedtls/md5.h"

#include "app_config.h"
#include "ota.h"
#include "ws_hub.h"

static const char *TAG = "ota";

#define U_FLASH   0
#define U_SPIFFS  100

static int s_udp_sock = -1;

volatile bool g_otaInProgress = false;

// -----------------------------------------------------------------------------
// SetupOTA
// Binds the UDP:3232 OTA discovery listener (mirrors ArduinoOTA.begin()).
// -----------------------------------------------------------------------------
void SetupOTA(void)
{
    s_udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_udp_sock < 0) {
        ESP_LOGE(TAG, "Unable to create OTA UDP socket: errno %d", errno);
        return;
    }

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port        = htons(OTA_UDP_PORT),
    };
    if (bind(s_udp_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "OTA UDP bind failed: errno %d", errno);
        close(s_udp_sock);
        s_udp_sock = -1;
        return;
    }

    // Short receive timeout so OTATask's loop stays cooperative.
    struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
    setsockopt(s_udp_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ESP_LOGI(TAG, "OTA ready on UDP port %d (ArduinoOTA-compatible)", OTA_UDP_PORT);
    ESP_LOGI(TAG, "Use: pio run -t upload --upload-port <device_ip>");
}

static bool sendAck(int sock, uint32_t written)
{
    char buf[16];
    int len = snprintf(buf, sizeof(buf), "%u", (unsigned)written);
    return send(sock, buf, len, 0) == len;
}

// -----------------------------------------------------------------------------
// runUpdate
// Handles one accepted OTA invite end-to-end: connect back to the host,
// stream the image into the next OTA partition, verify MD5, and reboot on
// success. Returns (without rebooting) on any failure so OTATask can keep
// listening for another attempt.
// -----------------------------------------------------------------------------
static void runUpdate(int cmd, const struct sockaddr_in *remote, uint16_t host_port,
                       uint32_t size, const char *md5hex)
{
    if (cmd != U_FLASH) {
        ESP_LOGW(TAG, "SPIFFS-image OTA (cmd=%d) not supported yet — rejecting", cmd);
        return;
    }

    WS_Broadcast("{\"type\":\"ota_start\",\"message\":\"OTA Update Started\"}");

    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        ESP_LOGE(TAG, "No OTA partition available");
        WS_Broadcast("{\"type\":\"ota_error\",\"message\":\"OTA Update Failed\"}");
        return;
    }

    esp_ota_handle_t ota_handle;
    if (esp_ota_begin(part, size, &ota_handle) != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed");
        WS_Broadcast("{\"type\":\"ota_error\",\"message\":\"OTA Update Failed\"}");
        return;
    }

    int tcp_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (tcp_sock < 0) {
        ESP_LOGE(TAG, "OTA TCP socket failed: errno %d", errno);
        esp_ota_abort(ota_handle);
        WS_Broadcast("{\"type\":\"ota_error\",\"message\":\"OTA Update Failed\"}");
        return;
    }

    struct sockaddr_in dest = *remote;
    dest.sin_port = htons(host_port);

    struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
    setsockopt(tcp_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (connect(tcp_sock, (struct sockaddr *)&dest, sizeof(dest)) != 0) {
        ESP_LOGE(TAG, "OTA TCP connect failed: errno %d", errno);
        close(tcp_sock);
        esp_ota_abort(ota_handle);
        WS_Broadcast("{\"type\":\"ota_error\",\"message\":\"OTA Update Failed\"}");
        return;
    }

    ESP_LOGI(TAG, "OTA Start: size=%u md5=%s", (unsigned)size, md5hex);

    mbedtls_md5_context md5ctx;
    mbedtls_md5_init(&md5ctx);
    mbedtls_md5_starts(&md5ctx);

    static uint8_t buf[1460];
    uint32_t written     = 0;
    uint32_t last_percent = 200; // sentinel — never matches a real percent
    bool     ok           = true;

    while (written < size) {
        int r = recv(tcp_sock, buf, sizeof(buf), 0);
        if (r <= 0) {
            ESP_LOGE(TAG, "OTA receive failed/closed: errno %d", errno);
            ok = false;
            break;
        }
        if (esp_ota_write(ota_handle, buf, (size_t)r) != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed");
            ok = false;
            break;
        }
        mbedtls_md5_update(&md5ctx, buf, (size_t)r);
        written += (uint32_t)r;

        if (!sendAck(tcp_sock, written)) {
            ESP_LOGW(TAG, "OTA ack send failed");
        }

        uint32_t percent = size ? (written * 100 / size) : 0;
        if (percent != last_percent && percent % 10 == 0) {
            ESP_LOGI(TAG, "OTA progress: %u%%", (unsigned)percent);
            last_percent = percent;
            char wsMess[64];
            snprintf(wsMess, sizeof(wsMess), "{\"type\":\"ota_progress\",\"percent\":%u}", (unsigned)percent);
            WS_Broadcast(wsMess);
        }
    }

    uint8_t digest[16];
    mbedtls_md5_finish(&md5ctx, digest);
    mbedtls_md5_free(&md5ctx);

    char digestHex[33];
    for (int i = 0; i < 16; i++) {
        snprintf(&digestHex[i * 2], 3, "%02x", digest[i]);
    }

    if (ok && written == size && strcasecmp(digestHex, md5hex) == 0 &&
        esp_ota_end(ota_handle) == ESP_OK && esp_ota_set_boot_partition(part) == ESP_OK) {
        send(tcp_sock, "OK", 2, 0);
        ESP_LOGI(TAG, "OTA End - rebooting");
        WS_Broadcast("{\"type\":\"ota_end\",\"message\":\"OTA Update Complete - Rebooting\"}");
        close(tcp_sock);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
        return; // unreachable
    }

    ESP_LOGE(TAG, "OTA failed: written=%u/%u md5 got=%s want=%s",
              (unsigned)written, (unsigned)size, digestHex, md5hex);
    esp_ota_abort(ota_handle);
    send(tcp_sock, "FAIL", 4, 0);
    close(tcp_sock);
    WS_Broadcast("{\"type\":\"ota_error\",\"message\":\"OTA Update Failed\"}");
}

// -----------------------------------------------------------------------------
// OTATask  (FreeRTOS task)
// Polls the UDP OTA-invite socket and hands off each valid invite to
// runUpdate(). Mirrors ArduinoOTA.handle() being called every ~10 ms.
// -----------------------------------------------------------------------------
void OTATask(void *parameter)
{
    if (s_udp_sock < 0) {
        ESP_LOGE(TAG, "OTA not initialized - SetupOTA() must run first");
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "OTA Task Started");

    char rxbuf[128];
    struct sockaddr_in from;
    socklen_t fromlen;

    for (;;) {
        fromlen = sizeof(from);
        int len = recvfrom(s_udp_sock, rxbuf, sizeof(rxbuf) - 1, 0, (struct sockaddr *)&from, &fromlen);
        if (len <= 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        rxbuf[len] = '\0';

        int cmd = 0, host_port = 0;
        unsigned long sizeField = 0;
        char md5hex[40] = {0};
        if (sscanf(rxbuf, "%d %d %lu %39s", &cmd, &host_port, &sizeField, md5hex) != 4) {
            continue;
        }
        uint32_t size = (uint32_t)sizeField;
        if ((cmd != U_FLASH && cmd != U_SPIFFS) || strlen(md5hex) != 32) {
            continue;
        }

        // No password configured (mirrors ARD, which never calls
        // ArduinoOTA.setPassword()) — reply OK immediately, no AUTH challenge.
        sendto(s_udp_sock, "OK", 2, 0, (struct sockaddr *)&from, fromlen);

        g_otaInProgress = true;
        runUpdate(cmd, &from, (uint16_t)host_port, size, md5hex);
        // runUpdate() reboots the device on success; on failure it returns
        // and we go back to listening for another invite.
        g_otaInProgress = false;
    }
}
