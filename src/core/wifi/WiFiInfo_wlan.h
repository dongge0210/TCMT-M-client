#pragma once

// C-compatible WLAN data struct and detection function.
// Uses the Windows Native Wi-Fi API (wlanapi.h) internally.

#include <stdbool.h>
#include <stdint.h>

#define WLAN_SSID_MAX_LEN     32
#define WLAN_BSSID_MAX_LEN    18
#define WLAN_SECURITY_MAX_LEN 32

typedef struct {
    char ssid[WLAN_SSID_MAX_LEN];
    char bssid[WLAN_BSSID_MAX_LEN];
    int32_t rssi;
    int32_t channel;
    char security[WLAN_SECURITY_MAX_LEN];
    double txRate;
    bool powerOn;
    bool isConnected;
} WlanData;

// Populates *out with current Wi-Fi state. Returns true on success.
bool WlanDetect(WlanData* out);
