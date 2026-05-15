// WiFiInfo.mm — macOS Wi-Fi monitoring via CoreWLAN.framework
// Objective-C++ (.mm) — compiled only when TCMT_MACOS is defined

#include "WiFiInfo.h"
#include "../Utils/Logger.h"

#ifdef TCMT_MACOS

#import <CoreWLAN/CoreWLAN.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <mutex>
#include <thread>

// Convert CWSecurity enum to a human-readable string.
static std::string SecurityToString(CWSecurity security) {
    switch (security) {
        case kCWSecurityNone:             return "Open";
        case kCWSecurityWEP:              return "WEP";
        case kCWSecurityWPAPersonal:      return "WPA-Personal";
        case kCWSecurityWPAPersonalMixed: return "WPA/WPA2-Personal";
        case kCWSecurityWPA2Personal:     return "WPA2-Personal";
        case kCWSecurityWPAEnterprise:    return "WPA-Enterprise";
        case kCWSecurityWPAEnterpriseMixed: return "WPA/WPA2-Enterprise";
        case kCWSecurityWPA2Enterprise:   return "WPA2-Enterprise";
        case kCWSecurityWPA3Personal:     return "WPA3-SAE";
        case kCWSecurityWPA3Enterprise:   return "WPA3-Enterprise";
        case kCWSecurityDynamicWEP:       return "Dynamic WEP";
        default:                          return "Unknown";
    }
}

// --- Async system_profiler state ---
// system_profiler SPAirPortDataType takes ~1s and blocks the caller.
// We run it once on a background thread so the main loop never stalls.
static std::atomic<int> s_profilerState{0}; // 0=idle, 1=running, 2=done
static std::mutex s_cacheMutex;
static std::string s_cachedSSID, s_cachedBSSID, s_cachedSecurity;
static int s_cachedRSSI = 0, s_cachedChannel = 0;
static double s_cachedTxRate = 0;

static void RunSystemProfiler() {
    std::string json;
    FILE* fp = popen("system_profiler SPAirPortDataType -json 2>/dev/null", "r");
    if (fp) {
        char buf[4096];
        while (fgets(buf, sizeof(buf), fp)) json += buf;
        pclose(fp);
    }

    std::string ssid, bssid, security;
    int rssi = 0, channel = 0;
    double txRate = 0;

    try {
        auto j = nlohmann::json::parse(json.empty() ? "{}" : json);
        if (j.contains("SPAirPortDataType") && j["SPAirPortDataType"].is_array()) {
            for (auto& item : j["SPAirPortDataType"]) {
                if (item.contains("spairport_airport_interfaces")) {
                    for (auto& iface : item["spairport_airport_interfaces"]) {
                        if (bssid.empty() && iface.contains("spairport_wireless_mac_address"))
                            bssid = iface["spairport_wireless_mac_address"].get<std::string>();
                        if (iface.contains("spairport_current_network_information")) {
                            auto& net = iface["spairport_current_network_information"];
                            if (ssid.empty() && net.contains("_name"))
                                ssid = net["_name"].get<std::string>();
                            if (net.contains("spairport_network_channel")) {
                                std::string ch = net["spairport_network_channel"].get<std::string>();
                                size_t sp = ch.find(' ');
                                channel = (sp != std::string::npos) ? std::stoi(ch.substr(0, sp)) : std::stoi(ch);
                            }
                            if (rssi == 0 && net.contains("spairport_signal_noise")) {
                                std::string sn = net["spairport_signal_noise"].get<std::string>();
                                rssi = std::stoi(sn);
                            }
                            if (security.empty() && net.contains("spairport_security_mode")) {
                                std::string sec = net["spairport_security_mode"].get<std::string>();
                                if (sec.find("wpa3") != std::string::npos) security = "WPA3";
                                else if (sec.find("wpa2") != std::string::npos) security = "WPA2-Personal";
                                else if (sec.find("wpa") != std::string::npos) security = "WPA-Personal";
                                else if (sec.find("wep") != std::string::npos) security = "WEP";
                                else if (sec.find("none") != std::string::npos) security = "Open";
                            }
                            if (txRate == 0 && net.contains("spairport_network_rate"))
                                txRate = net["spairport_network_rate"].get<double>();
                        }
                    }
                }
            }
        }
    } catch (...) {}

    // Publish cache
    {
        std::lock_guard<std::mutex> lk(s_cacheMutex);
        s_cachedSSID = ssid;
        s_cachedBSSID = bssid;
        s_cachedSecurity = security;
        s_cachedRSSI = rssi;
        s_cachedChannel = channel;
        s_cachedTxRate = txRate;
    }
    s_profilerState.store(2, std::memory_order_release);
}

void WiFiInfo::Detect() {
    Clear();

    @autoreleasepool {
        CWWiFiClient* wifiClient = [CWWiFiClient sharedWiFiClient];
        if (!wifiClient) {
            Logger::Warn("WiFiInfo: CWWiFiClient returned nil");
            return;
        }

        CWInterface* interface = [wifiClient interface];
        if (!interface) {
            Logger::Warn("WiFiInfo: No default Wi-Fi interface found");
            return;
        }

        // --- Power state ---
        data_.powerOn = [interface powerOn];
        if (!data_.powerOn) {
            Logger::Debug("WiFiInfo: Wi-Fi adapter is powered off");
            return;
        }

        // --- BSSID / SSID (may be nil on macOS 15+ without Location Services) ---
        NSString* bssidStr = [interface bssid];
        if (bssidStr) data_.bssid = [bssidStr UTF8String];

        NSString* ssidStr = [interface ssid];
        if (ssidStr) data_.ssid = [ssidStr UTF8String];

        // --- system_profiler fallback (async, non-blocking) ---
        // CoreWLAN may block SSID/BSSID on macOS 15+ without Location Services.
        // system_profiler SPAirPortDataType bypasses the restriction but is slow (~1s).
        // We run it once on a background thread so the main loop never stalls.
        if (data_.ssid.empty() || data_.bssid.empty()) {
            int state = s_profilerState.load(std::memory_order_acquire);
            if (state == 2) {
                std::lock_guard<std::mutex> lk(s_cacheMutex);
                if (!s_cachedSSID.empty()) data_.ssid = s_cachedSSID;
                if (!s_cachedBSSID.empty()) data_.bssid = s_cachedBSSID;
                if (!s_cachedSecurity.empty()) data_.security = s_cachedSecurity;
                if (s_cachedRSSI != 0) data_.rssi = s_cachedRSSI;
                if (s_cachedChannel != 0) data_.channel = s_cachedChannel;
                if (s_cachedTxRate > 0) data_.txRate = s_cachedTxRate;
            } else if (state == 0) {
                s_profilerState.store(1, std::memory_order_release);
                std::thread(RunSystemProfiler).detach();
            }
            // state == 1: still running, no cached data yet — use CoreWLAN-only this cycle
        }

        data_.isConnected = !data_.bssid.empty();

        // --- RSSI / Noise / Channel / Security / TxRate (CoreWLAN) ---
        // CoreWLAN RSSI/channel are available even without Location Services.
        // Only override cached system_profiler values when CoreWLAN gives valid data.
        int cwRssi = static_cast<int>([interface rssiValue]);
        if (cwRssi != 0 || data_.rssi == 0) data_.rssi = cwRssi;
        int cwNoise = static_cast<int>([interface noiseMeasurement]);
        if (cwNoise != 0 || data_.noise == 0) data_.noise = cwNoise;
        CWChannel* wlanChannel = [interface wlanChannel];
        if (wlanChannel) {
            int ch = static_cast<int>([wlanChannel channelNumber]);
            if (ch != 0 || data_.channel == 0) data_.channel = ch;
        }
        CWSecurity secType = [interface security];
        std::string secStr = SecurityToString(secType);
        if (!secStr.empty() && secStr != "Unknown") data_.security = secStr;
        else if (data_.security.empty()) data_.security = secStr;
        double cwRate = [interface transmitRate];
        if (cwRate > 0 || data_.txRate == 0) data_.txRate = cwRate;

        Logger::Debug("WiFiInfo: ssid=" + data_.ssid +
                      " bssid=" + data_.bssid +
                      " rssi=" + std::to_string(data_.rssi) + " dBm" +
                      " noise=" + std::to_string(data_.noise) + " dBm" +
                      " channel=" + std::to_string(data_.channel) +
                      " security=" + data_.security +
                      " txRate=" + std::to_string(data_.txRate) + " Mbps");
    }
}

void WiFiInfo::Clear() {
    data_ = WiFiData{};
}

const WiFiData& WiFiInfo::GetData() const {
    return data_;
}

#else
// ======================== Non-macOS stub ========================

void WiFiInfo::Detect() {
    Logger::Debug("WiFiInfo: not implemented on this platform");
}

void WiFiInfo::Clear() {
    data_ = WiFiData{};
}

const WiFiData& WiFiInfo::GetData() const {
    return data_;
}

#endif
