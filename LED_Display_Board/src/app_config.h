// =============================================================================
// app_config.h — Shared Pin Assignments & Board-Wide Constants
// =============================================================================
// ESP-IDF port of LED_Display_Board_ARD. Ported from 01_Main.h / 05_ThisSystem.h.
// =============================================================================

#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include "driver/gpio.h"

// WiFi setup hotspot name / hostname: see wifi_setup.h (WIFI_SETUP_SSID).

// ─── Hardware Pin Aliases ─────────────────────────────────────────────────────
#define LEDOUT       GPIO_NUM_2    // GPIO 2  — Blue activity LED (ESP32 on-board LED)
#define TERM_BUTTON  GPIO_NUM_27   // GPIO 27 — Push-button terminate / cancel input
#define RADAR_SENSE  GPIO_NUM_23   // GPIO 23 — Radar presence-sensor digital output
#define BUZZER_PIN   GPIO_NUM_17   // GPIO 17 — Buzzer output (2N2222 driver)
#define MATRIX_PIN   GPIO_NUM_33   // GPIO 33 — NeoPixel data line to the 6x8 LED matrix
#define LDR_ADC_CHANNEL_GPIO 35    // GPIO 35 — LDR analog input (ADC1)

// ─── WiFi Connect Timeout ─────────────────────────────────────────────────────
// LED-flash tick increments a counter every 100 ms during WiFi connect.
// If it reaches WIFI_CONNECTING_COUNTS the firmware reboots. Last-resort only:
// SetupWiFi()'s retry rounds (~7 min) + setup hotspot (5 min) must fit inside.
#define WIFI_CONNECTING_COUNTS  12000 // 12000 x 100 ms = 20 min

// ─── OTA ──────────────────────────────────────────────────────────────────────
#define OTA_UDP_PORT  3232   // ArduinoOTA-compatible discovery port (matches espota.py default)

#endif // APP_CONFIG_H
