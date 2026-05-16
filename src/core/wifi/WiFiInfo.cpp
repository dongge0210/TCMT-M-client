#include "WiFiInfo.h"
#include "../Utils/Logger.h"

#ifdef TCMT_WINDOWS
// ======================== Windows Implementation ========================

extern "C" {
#include "WiFiInfo_wlan.h"
}

void WiFiInfo::Clear() {
    data_ = WiFiData{};
}

void WiFiInfo::Detect() {
    Clear();

    WlanData wd;
    if (!WlanDetect(&wd)) {
        Logger::Info("WiFiInfo: WlanDetect failed");
        return;
    }

    data_.powerOn     = wd.powerOn;
    data_.isConnected = wd.isConnected;
    Logger::Warn("WiFiInfo: powerOn=" + std::to_string(wd.powerOn) +
                 " isConnected=" + std::to_string(wd.isConnected) +
                 " ssid=" + std::string(wd.ssid) +
                 " rssi=" + std::to_string(wd.rssi) +
                 " channel=" + std::to_string(wd.channel));
    data_.ssid        = wd.ssid;
    data_.bssid       = wd.bssid;
    data_.rssi        = wd.rssi;
    data_.channel     = wd.channel;
    data_.security    = wd.security;
    data_.band        = wd.band;
    data_.wifiGen     = wd.wifiGen;
    data_.txRate      = wd.txRate;

    if (!data_.isConnected) {
        Logger::Debug("WiFiInfo: Wi-Fi adapter on, not connected");
    } else {
        Logger::Debug("WiFiInfo: ssid=" + data_.ssid +
                      " bssid=" + data_.bssid +
                      " rssi=" + std::to_string(data_.rssi) + " dBm" +
                      " channel=" + std::to_string(data_.channel) +
                      " security=" + data_.security +
                      " txRate=" + std::to_string(data_.txRate) + " Mbps");
    }
}

const WiFiData& WiFiInfo::GetData() const { return data_; }

#else
// ==================== Stub for non-Windows platforms ====================
void WiFiInfo::Detect() {
    // No-op: WiFi monitoring is Windows-only via WLAN API.
}
void WiFiInfo::Clear() { data_ = WiFiData{}; }
const WiFiData& WiFiInfo::GetData() const { return data_; }
#endif // TCMT_WINDOWS
