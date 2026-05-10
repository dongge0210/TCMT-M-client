// WiFiInfo.mm — macOS Wi-Fi monitoring via CoreWLAN.framework
// Objective-C++ (.mm) — compiled only when TCMT_MACOS is defined

#include "WiFiInfo.h"
#include "../Utils/Logger.h"

#ifdef TCMT_MACOS

#import <CoreWLAN/CoreWLAN.h>
#import <CoreLocation/CoreLocation.h>

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

        // --- BSSID (MAC address of the access point) ---
        NSString* bssidStr = [interface bssid];
        if (bssidStr) {
            data_.bssid = [bssidStr UTF8String];
        }

        // --- SSID ---
        NSString* ssidStr = [interface ssid];
        if (ssidStr) {
            data_.ssid = [ssidStr UTF8String];
        }

        // If there is no SSID we are not connected to a network,
        // but the adapter may still be powered on.
        if (data_.ssid.empty()) {
            // macOS 15+ requires Location Services permission for SSID.
            // Check once and log a warning if denied.
            static bool s_locationWarningLogged = false;
            if (!s_locationWarningLogged) {
                CLAuthorizationStatus auth = [CLLocationManager authorizationStatus];
                if (auth == kCLAuthorizationStatusDenied) {
                    data_.locationDenied = true;
                    Logger::Warn("WiFi: Location Services denied — SSID unavailable. "
                                 "Enable in System Settings > Privacy > Location Services > Terminal.app");
                    s_locationWarningLogged = true;
                }
            }
            Logger::Debug("WiFiInfo: Wi-Fi is on but not connected to any network");
            return;
        }

        data_.isConnected = true;

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
