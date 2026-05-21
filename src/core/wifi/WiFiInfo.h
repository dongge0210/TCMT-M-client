#pragma once

#include <string>
#include <cstdint>

struct WiFiData {
    std::string ssid;       // Connected network name
    std::string bssid;      // AP MAC address
    int rssi = 0;           // Signal strength in dBm (e.g. -45)
    int noise = 0;          // Noise in dBm
    int channel = 0;        // Channel number
    std::string security;   // "WPA2-Personal", "WPA3-SAE", etc.
    std::string band;       // "2.4GHz", "5GHz", "6GHz"
    std::string wifiGen;    // "WiFi 4", "WiFi 5", "WiFi 6", "WiFi 7"
    double txRate = 0;      // Transmit rate in Mbps
    bool powerOn = false;   // WiFi adapter enabled
    bool isConnected = false;
    bool locationDenied = false; // macOS 15+: SSID unavailable due to Location Services denial
};

class WiFiInfo {
public:
    WiFiInfo() = default;
    ~WiFiInfo() = default;

    void Detect();
    const WiFiData& GetData() const;

private:
    WiFiData data_;
    void Clear();
};
