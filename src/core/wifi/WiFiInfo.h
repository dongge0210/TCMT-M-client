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
    double txRate = 0;      // Transmit rate in Mbps
    bool powerOn = false;   // WiFi adapter enabled
    bool isConnected = false;
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
