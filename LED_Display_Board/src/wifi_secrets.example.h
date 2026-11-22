#pragma once

// =============================================================================
// wifi_secrets.example.h — OPTIONAL template for wifi_secrets.h
// =============================================================================
// You do NOT need this file: without wifi_secrets.h the clock starts the
// "LEDClock-Setup" WiFi hotspot and you choose your network on a phone.
//
// Only if you build the firmware yourself and want your own networks / NTFY
// topics built in: copy this file to wifi_secrets.h (same folder) and fill it
// in. wifi_secrets.h is in .gitignore, so it never goes into Git.
//
// WIFI_KNOWN_NETS — each entry: SSID, password, IP the clock takes there.
//   { 0, 0, 0, 0 } = automatic IP (DHCP). Several clocks can share one
//   firmware, so normally leave the IP automatic and give each clock its fixed
//   IP on its WIFI NETWORKS page.
// A built-in network that joins is copied into the clock's saved list, so a
// later public firmware still finds it.
// =============================================================================

#define WIFI_KNOWN_NETS \
    { "MyHomeWiFi", "my-password", { 0, 0, 0, 0 } }, \
    { "OtherWiFi",  "other-pass",  { 0, 0, 0, 0 } },

// Default NTFY topics (until changed on the NTFY page).
#define NTFY_DEFAULT_TOPIC1 "MY_NTFY_TOPIC_1"
#define NTFY_DEFAULT_TOPIC2 "MY_NTFY_TOPIC_2"
