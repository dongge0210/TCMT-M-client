// BluetoothInfo.mm — macOS Bluetooth monitoring via IOBluetooth.framework
// Objective-C++ — compiled only when TCMT_MACOS is defined

#include "BluetoothInfo.h"
#include "../Utils/Logger.h"

#ifdef TCMT_MACOS

#import <IOBluetooth/IOBluetooth.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace {

// IOBluetooth exposes addresses, RSSI and connection state, but no battery
// or device type — those only exist in system_profiler's Bluetooth profile.
// The profile takes ~1s, so it runs on a background thread and is republished
// roughly every 30 seconds; Detect() uses the cached copy when it exists.
struct ProfDevice {
    std::string name;
    std::string address;
    std::string type;
    int rssi = 0;
    bool connected = false;
    std::vector<std::pair<std::string, int>> batteries;
};

std::mutex g_profMutex;
std::vector<ProfDevice> g_profDevices;
std::atomic<bool> g_profReady{false};
std::atomic<int> g_profState{0};      // 0 idle, 1 running
std::atomic<int64_t> g_profStampMs{0};

int64_t ProfNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string TrimCopy(const std::string& s) {
    size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return {};
    size_t e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

size_t IndentOf(const std::string& s) {
    size_t i = 0;
    while (i < s.size() && s[i] == ' ') ++i;
    return i;
}

void RunProfiler() {
    std::vector<ProfDevice> out;
    // system_profiler lives in /usr/sbin, not /usr/bin; an absolute path also
    // keeps it working when the app is launched from Finder with a minimal PATH.
    FILE* fp = popen("/usr/sbin/system_profiler SPBluetoothDataType 2>/dev/null", "r");
    if (fp) {
        char buf[512];
        std::string section;       // "conn" / "disc"
        int deviceIndent = -1;
        ProfDevice cur;
        bool have = false;
        while (fgets(buf, sizeof(buf), fp)) {
            std::string s(buf);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            const std::string t = TrimCopy(s);
            if (t.empty()) continue;
            const size_t indent = IndentOf(s);

            if (indent == 6 && (t == "Connected:" || t == "Not Connected:")) {
                if (have) { out.push_back(cur); have = false; }
                cur = ProfDevice{};
                section = (t == "Connected:") ? "conn" : "disc";
                deviceIndent = -1;
                continue;
            }
            if (section.empty()) continue;

            // Device header: "          Name:" at the section's device indent.
            if (t.back() == ':' && (deviceIndent < 0 || indent == size_t(deviceIndent))) {
                if (have) out.push_back(cur);
                cur = ProfDevice{};
                cur.name = t.substr(0, t.size() - 1);
                cur.connected = (section == "conn");
                have = true;
                deviceIndent = static_cast<int>(indent);
                continue;
            }
            if (!have) continue;

            const auto colon = t.find(": ");
            if (colon == std::string::npos) continue;
            const std::string key = t.substr(0, colon);
            const std::string val = t.substr(colon + 2);
            if (key == "Address") cur.address = val;
            else if (key == "Minor Type") cur.type = val;
            else if (key == "RSSI") cur.rssi = std::atoi(val.c_str());
            else if (key.size() > 14 && key.compare(key.size() - 14, 14, " Battery Level") == 0) {
                // "Battery Level" → no label; "Left Battery Level" → "Left".
                cur.batteries.push_back({key.substr(0, key.size() - 14),
                                         std::atoi(val.c_str())});
            }
        }
        if (have) out.push_back(cur);
        pclose(fp);
    }
    Logger::Debug("BluetoothInfo: profile parsed " + std::to_string(out.size()) + " device(s)");
    {
        std::lock_guard<std::mutex> lk(g_profMutex);
        g_profDevices.swap(out);
        g_profReady = true;
    }
    g_profStampMs.store(ProfNowMs());
    g_profState.store(0);
}

} // namespace

void BluetoothInfo::Detect() {
    Clear();

    @autoreleasepool {
        IOBluetoothHostController* ctrl = [IOBluetoothHostController defaultController];
        if (!ctrl) {
            Logger::Debug("BluetoothInfo: no controller");
            return;
        }

        data_.adapter.detected = true;
        data_.adapter.powerOn = ([ctrl powerState] == kBluetoothHCIPowerStateON);
        NSString* addr = [ctrl addressAsString];
        if (addr) data_.adapter.address = [addr UTF8String];
        NSString* name = [ctrl nameAsString];
        if (name) data_.adapter.name = [name UTF8String];

        // Refresh the profile cache at most every 30s; Detect() runs ~3s.
        if (g_profState.load() == 0 &&
            ProfNowMs() - g_profStampMs.load() > 30000) {
            g_profState.store(1);
            std::thread(RunProfiler).detach();
        }

        bool usedProfile = false;
        if (g_profReady.load()) {
            std::lock_guard<std::mutex> lk(g_profMutex);
            for (const auto& pd : g_profDevices) {
                BluetoothDeviceData d;
                d.name = pd.name;
                d.address = pd.address;
                d.type = pd.type;
                d.rssi = pd.rssi;
                d.connected = pd.connected;
                d.remembered = true;
                d.batteries = pd.batteries;
                data_.devices.push_back(std::move(d));
            }
            usedProfile = !g_profDevices.empty();
        }

        // Before the first profile lands, fall back to the connected list from
        // IOBluetooth so the panel is not empty for the first seconds.
        if (!usedProfile) {
            NSArray* paired = [IOBluetoothDevice pairedDevices];
            if (paired) {
                for (IOBluetoothDevice* dev in paired) {
                    if (![dev isConnected]) continue;
                    BluetoothDeviceData d;
                    if ([dev name]) d.name = [[dev name] UTF8String];
                    if ([dev addressString]) d.address = [[dev addressString] UTF8String];
                    d.connected = true;
                    d.remembered = true;
                    d.rssi = (int)[dev RSSI];
                    data_.devices.push_back(std::move(d));
                }
            }
        }
        // NOTE: Inquiry scan (IOBluetoothDeviceInquiry) is NOT performed here
        // because it blocks for 3-5 seconds per call and Detect() runs every
        // ~3 seconds.  If a nearby-device scan is needed it should be a
        // separate one-shot API, not part of the periodic collection loop.

        Logger::Debug("BluetoothInfo: " + data_.adapter.name +
                      " devices=" + std::to_string(data_.devices.size()));
    }
}

void BluetoothInfo::Clear() { data_ = BluetoothData{}; }
const BluetoothData& BluetoothInfo::GetData() const { return data_; }

#else
// File-level platform split: registered only under if(TCMT_MACOS) in
// src/CMakeLists.txt, so this branch is unreachable. The non-macOS
// implementation is core/bluetooth/BluetoothInfo.cpp (Windows, registered in
// TCMT.vcxproj). A no-op stub here would make "collected nothing" and "not
// implemented at all" indistinguishable, so fail loudly instead.
#error "BluetoothInfo.mm is macOS-only. Do not add it to a non-Apple build."
#endif
