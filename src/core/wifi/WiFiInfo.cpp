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

#elif defined(TCMT_LINUX)
// ======================== Linux Implementation ========================
#include <fstream>
#include <sstream>
#include <dirent.h>
#include <cstring>
#include <cstdio>
#include <memory>
#include <unistd.h>
#include <sys/ioctl.h>
#include <net/if.h>

void WiFiInfo::Clear() {
    data_ = WiFiData{};
}

static std::string ExecCmd(const std::string& cmd) {
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) return {};
    std::string result;
    char buf[256];
    while (fgets(buf, sizeof(buf), pipe.get()) != nullptr)
        result += buf;
    return result;
}

static bool IsInterfaceUp(const std::string& ifname) {
    struct ifreq ifr;
    std::memset(&ifr, 0, sizeof(ifr));
    if (ifname.size() < sizeof(ifr.ifr_name)) {
        std::strncpy(ifr.ifr_name, ifname.c_str(), sizeof(ifr.ifr_name) - 1);
    }
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return false;
    bool up = false;
    if (ioctl(sock, SIOCGIFFLAGS, &ifr) == 0)
        up = (ifr.ifr_flags & IFF_UP) != 0;
    close(sock);
    return up;
}

static std::string FindWlanInterface() {
    DIR* dir = opendir("/sys/class/net/");
    if (!dir) return {};
    std::string iface;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name(entry->d_name);
        if (name.compare(0, 2, "wl") == 0 ||
            name.compare(0, 3, "wlp") == 0 ||
            name.compare(0, 4, "wlan") == 0) {
            iface = name;
            break;
        }
    }
    closedir(dir);
    return iface;
}

void WiFiInfo::Detect() {
    Clear();

    std::string iface = FindWlanInterface();
    if (iface.empty()) {
        Logger::Debug("WiFiInfo: no wireless interface found");
        return;
    }

    data_.powerOn = IsInterfaceUp(iface);
    if (!data_.powerOn) {
        Logger::Debug("WiFiInfo: interface " + iface + " is down");
        return;
    }

    // Use iw to get link info
    std::string linkInfo = ExecCmd("iw dev " + iface + " link 2>/dev/null");
    if (!linkInfo.empty()) {
        data_.isConnected = (linkInfo.find("Not connected") == std::string::npos);
        if (data_.isConnected) {
            // Parse SSID
            auto ssidPos = linkInfo.find("SSID: ");
            if (ssidPos != std::string::npos) {
                auto start = ssidPos + 6;
                auto end = linkInfo.find('\n', start);
                data_.ssid = linkInfo.substr(start, end - start);
            }

            // Parse signal
            auto sigPos = linkInfo.find("signal: ");
            if (sigPos != std::string::npos) {
                auto start = sigPos + 8;
                auto end = linkInfo.find(' ', start);
                std::string sigStr = linkInfo.substr(start, end - start);
                try { data_.rssi = std::stoi(sigStr); } catch (...) {}
            }

            // Parse tx bitrate
            auto ratePos = linkInfo.find("tx bitrate: ");
            if (ratePos != std::string::npos) {
                auto start = ratePos + 12;
                auto end = linkInfo.find(' ', start);
                std::string rateStr = linkInfo.substr(start, end - start);
                try { data_.txRate = std::stod(rateStr); } catch (...) {}
            }

            // Parse frequency and determine band/channel
            auto freqPos = linkInfo.find("freq: ");
            if (freqPos != std::string::npos) {
                auto start = freqPos + 6;
                auto end = linkInfo.find('\n', start);
                std::string freqStr = linkInfo.substr(start, end - start);
                int freqMhz = 0;
                try { freqMhz = std::stoi(freqStr); } catch (...) {}
                if (freqMhz >= 5000 && freqMhz < 6000) {
                    data_.band = "5GHz";
                    data_.channel = (freqMhz - 5000) / 5;
                } else if (freqMhz >= 6000) {
                    data_.band = "6GHz";
                    data_.channel = (freqMhz - 5950) / 5;
                } else {
                    data_.band = "2.4GHz";
                    data_.channel = (freqMhz - 2412) / 5 + 1;
                }

                // Determine WiFi generation
                if (freqMhz >= 6000) {
                    data_.wifiGen = "WiFi 6E";
                } else if (data_.txRate >= 1201) {
                    data_.wifiGen = "WiFi 6";
                } else if (data_.txRate >= 433) {
                    data_.wifiGen = "WiFi 5";
                } else if (data_.txRate >= 150) {
                    data_.wifiGen = "WiFi 4";
                } else {
                    data_.wifiGen = "WiFi 3";
                }
            }

            // Parse BSSID
            auto bssidPos = linkInfo.find("Connected to ");
            if (bssidPos != std::string::npos) {
                auto start = bssidPos + 13;
                auto end = linkInfo.find(' ', start);
                data_.bssid = linkInfo.substr(start, end - start);
            }
        }
    }

    data_.security = "Unknown";
    data_.locationDenied = false;

    Logger::Debug("WiFiInfo: interface=" + iface +
                  " connected=" + (data_.isConnected ? "yes" : "no") +
                  " ssid=" + data_.ssid +
                  " rssi=" + std::to_string(data_.rssi) + " dBm");
}

const WiFiData& WiFiInfo::GetData() const { return data_; }

#else
// ==================== Stub for macOS ====================
void WiFiInfo::Detect() {
    Clear();
}
void WiFiInfo::Clear() { data_ = WiFiData{}; }
const WiFiData& WiFiInfo::GetData() const { return data_; }
#endif
