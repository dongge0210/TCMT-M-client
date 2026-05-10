// WiFiInfo.mm — macOS Wi-Fi monitoring via CoreWLAN.framework
// Objective-C++ (.mm) — compiled only when TCMT_MACOS is defined

#include "WiFiInfo.h"
#include "../Utils/Logger.h"

#ifdef TCMT_MACOS

#import <CoreWLAN/CoreWLAN.h>
#include <nlohmann/json.hpp>

// Convert CWSecurity enum to a human-readable string.
// CoreWLAN defines CWSecurity as an NSInteger-backed enum; the value ranges
// are documented in <CoreWLAN/CoreWLANConstants.h>.
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

        // --- Unknown ---
        default:                                 return "Unknown";
    }
}

// Convert CWChannelBand to a channel band suffix string
static std::string ChannelBandToString(CWChannelBand band) {
    switch (band) {
        case kCWChannelBand2GHz:  return " (2.4 GHz)";
        case kCWChannelBand5GHz:  return " (5 GHz)";
        case kCWChannelBand6GHz:  return " (6 GHz)";
        default:                  return "";
    }
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

        // --- BSSID (MAC address of the access point, no Location Services needed) ---
        NSString* bssidStr = [interface bssid];
        if (bssidStr) data_.bssid = [bssidStr UTF8String];

        // --- SSID (may be empty on macOS 15+ without Location Services) ---
        NSString* ssidStr = [interface ssid];
        if (ssidStr) data_.ssid = [ssidStr UTF8String];

        // isConnected: BSSID is available regardless of Location Services.
        // SSID may be hidden on macOS 15+ but we're still connected.
        data_.isConnected = !data_.bssid.empty();

        // macOS 15+ blocks SSID/BSSID/channel without Location Services.
        // Fallback: system_profiler SPAirPortDataType bypasses the restriction.
        // Run once per program lifetime and cache results (popen is slow, ~1s).
        static std::string s_cachedSSID, s_cachedBSSID, s_cachedSecurity;
        static int s_cachedRSSI = 0, s_cachedChannel = 0;
        static double s_cachedTxRate = 0;
        static bool s_triedSystemProfiler = false;

        if (data_.ssid.empty() || data_.bssid.empty()) {
            if (s_triedSystemProfiler) {
                // Restore cached data
                if (!s_cachedSSID.empty()) data_.ssid = s_cachedSSID;
                if (!s_cachedBSSID.empty()) data_.bssid = s_cachedBSSID;
                if (!s_cachedSecurity.empty()) data_.security = s_cachedSecurity;
                if (s_cachedRSSI != 0) data_.rssi = s_cachedRSSI;
                if (s_cachedChannel != 0) data_.channel = s_cachedChannel;
                if (s_cachedTxRate > 0) data_.txRate = s_cachedTxRate;
            } else {
                s_triedSystemProfiler = true;
                FILE* fp = popen("system_profiler SPAirPortDataType -json 2>/dev/null", "r");
                std::string json;
                char buf[4096];
                while (fgets(buf, sizeof(buf), fp)) json += buf;
                pclose(fp);
                try {
                    auto j = nlohmann::json::parse(json.empty() ? "{}" : json);
                    if (j.contains("SPAirPortDataType") && j["SPAirPortDataType"].is_array()) {
                        for (auto& item : j["SPAirPortDataType"]) {
                            if (item.contains("spairport_airport_interfaces")) {
                                for (auto& iface : item["spairport_airport_interfaces"]) {
                                    // BSSID from interface-level field
                                    if (data_.bssid.empty() && iface.contains("spairport_wireless_mac_address"))
                                        data_.bssid = iface["spairport_wireless_mac_address"].get<std::string>();
                                    // Current network info (contains SSID/channel/rate/security/signal)
                                    if (iface.contains("spairport_current_network_information")) {
                                        auto& net = iface["spairport_current_network_information"];
                                        if (data_.ssid.empty() && net.contains("_name"))
                                            data_.ssid = net["_name"].get<std::string>();
                                        if (net.contains("spairport_network_channel")) {
                                            std::string ch = net["spairport_network_channel"].get<std::string>();
                                            // Parse "157 (5GHz, 80MHz)" -> channel=157
                                            size_t sp = ch.find(' ');
                                            data_.channel = (sp != std::string::npos) ? std::stoi(ch.substr(0, sp)) : std::stoi(ch);
                                        }
                                        if (data_.rssi == 0 && net.contains("spairport_signal_noise")) {
                                            std::string sn = net["spairport_signal_noise"].get<std::string>();
                                            // "-53 dBm / -91 dBm" -> RSSI = -53
                                            data_.rssi = std::stoi(sn);
                                        }
                                        if (data_.security.empty() && net.contains("spairport_security_mode")) {
                                            std::string sec = net["spairport_security_mode"].get<std::string>();
                                            // "spairport_security_mode_wpa2_personal" -> "WPA2-Personal"
                                            if (sec.find("wpa3") != std::string::npos) data_.security = "WPA3";
                                            else if (sec.find("wpa2") != std::string::npos) data_.security = "WPA2-Personal";
                                            else if (sec.find("wpa") != std::string::npos) data_.security = "WPA-Personal";
                                            else if (sec.find("wep") != std::string::npos) data_.security = "WEP";
                                            else if (sec.find("none") != std::string::npos) data_.security = "Open";
                                        }
                                        if (data_.txRate == 0 && net.contains("spairport_network_rate"))
                                            data_.txRate = net["spairport_network_rate"].get<double>();
                                    }
                                }
                            }
                        }
                    }
                } catch (...) {}
                // Cache parsed values for future Detect() calls (Clear() resets data_)
                s_cachedSSID = data_.ssid; s_cachedBSSID = data_.bssid;
                s_cachedSecurity = data_.security; s_cachedRSSI = data_.rssi;
                s_cachedChannel = data_.channel; s_cachedTxRate = data_.txRate;
            } // system_profiler fallback
            data_.isConnected = !data_.bssid.empty();
        }

        if (!data_.isConnected) {
            Logger::Debug("WiFiInfo: Wi-Fi is on but not connected to any network");
            return;
        }

        // --- RSSI (dBm) ---
        data_.rssi = static_cast<int>([interface rssiValue]);

        // --- Noise (dBm) ---
        data_.noise = static_cast<int>([interface noiseMeasurement]);

        // --- Channel ---
        CWChannel* wlanChannel = [interface wlanChannel];
        if (wlanChannel) {
            data_.channel = static_cast<int>([wlanChannel channelNumber]);
        }

        // --- Security type ---
        CWSecurity secType = [interface security];
        data_.security = SecurityToString(secType);

        // --- Transmit rate (Mbps) ---
        data_.txRate = [interface transmitRate];

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
// TCMT-M builds on Windows too; on that platform there is no CoreWLAN.
// A Windows Wi-Fi module would need the Native Wi-Fi API (wlanapi.h).

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
