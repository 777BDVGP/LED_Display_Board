// =============================================================================
// wifi_setup.h — WiFi join (saved networks / known networks) + setup hotspot
// =============================================================================
// Same model as the RainGauge project:
//
// /spiffs/WiFiConfig.json = a LIST of networks, newest first (max 8):
//   {"nets":[{"ssid":"..","pass":"..","dhcp":false,"ip":"192.168.1.112",
//             "gw":"192.168.1.1","sn":"255.255.255.0","dns":"192.168.1.1"}, ...]}
//   Holds the WiFi passwords, so FILES never lists, views or downloads it and
//   no page ever shows a password. The old single-network WifiSettings.json is
//   converted into this list automatically at the first start.
//
// SetupWiFi() at boot:
//   1. Nothing saved and no built-in networks (public firmware) -> setup
//      hotspot "LEDClock-Setup" (192.168.4.1) until a network is added.
//   2. Otherwise up to WIFI_ROUNDS rounds of: saved networks in range
//      (strongest first, then the rest), then the built-in WIFI_KNOWN_NETS of
//      a private build (wifi_secrets.h — never in Git / never published); a
//      known network that joins is added to the saved list.
//   3. Still nothing -> setup hotspot for 5 min, then reboot and start over.
// A fixed-IP entry without a gateway first joins by DHCP to learn gateway /
// subnet / DNS from the router, then rejoins on its fixed IP and saves them.
//
// Networks are managed on the WIFI NETWORKS page (/wifi): list (no passwords),
// DELETE, ADD. Changes are used at the next start.
// =============================================================================

#ifndef WIFI_SETUP_H
#define WIFI_SETUP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WIFI_MAX_NETS     8
#define WIFI_SETUP_SSID   "LEDClock-Setup"
#define WIFI_HOSTNAME     "ledclock"

typedef struct {
    char    ssid[33];
    char    pass[65];
    bool    dhcp;      // true = automatic IP; false = fixed ip below
    uint8_t ip[4];
    uint8_t gw[4];     // 0.0.0.0 = learned from the router at the first join
    uint8_t sn[4];
    uint8_t dns[4];
} WifiNet;

// Connects to WiFi (see above). Only returns (true) once connected — every
// failure path ends in the setup hotspot, which always reboots.
bool SetupWiFi(void);

// Saved-network list (/spiffs/WiFiConfig.json).
int  WifiNets_Load(WifiNet *out, int maxOut);
bool WifiNets_Add(const WifiNet *net);          // newest first; same SSID replaces its entry
bool WifiNets_Delete(const char *ssid);         // false if not in the list
void WifiNets_ClearAll(void);                   // RESET ASP

// Parses an application/x-www-form-urlencoded network form:
//   ssid (or ssid2 typed by hand), pass, mode=dhcp|fixed, ip, gw, sn, dns
// Returns true when valid; otherwise false with a reason in err.
bool WifiNets_ParseForm(const char *body, WifiNet *out, char *err, size_t errsz);

// SSID of the network the clock is connected to now ("" if none).
void WifiSetup_GetCurrentSsid(char *out, size_t outsz);

// Formats the station interface's current IP/gateway as "a.b.c.d" into out.
// Writes "0.0.0.0" if the interface isn't up yet.
void WifiSetup_GetLocalIPString(char *out, size_t outsz);
void WifiSetup_GetGatewayIPString(char *out, size_t outsz);

// "a.b.c.d" helpers shared with the WiFi pages.
void WifiSetup_FormatIp(const uint8_t ip[4], char *out, size_t outsz);

// True for files that hold WiFi passwords (never listed/viewed/downloaded).
bool WifiSetup_IsProtectedFile(const char *name);

#endif // WIFI_SETUP_H
