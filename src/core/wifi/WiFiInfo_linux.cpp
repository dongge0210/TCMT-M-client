// WiFiInfo_linux.cpp - Linux WiFi information.
//
// Preferred source: NetworkManager over D-Bus (org.freedesktop.NetworkManager).
// NM is the only unprivileged interface that reports the SSID, BSSID, frequency
// and security of the AP the system is associated with. Signal strength is not
// exposed in dBm there (NM reports a 0-100 percentage), so the dBm value is read
// from /proc/net/wireless — the same source used when NM is absent. A field that
// no source can supply is left empty / 0 instead of being estimated.
//
// The file is self-contained: libdbus-1 is optional (TCMT_HAVE_DBUS).

#include "WiFiInfo.h"
#include "../Utils/Logger.h"

#ifdef TCMT_LINUX

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <dirent.h>
#include <unistd.h>

#ifdef TCMT_HAVE_DBUS
#include <dbus/dbus.h>
#endif

namespace {

constexpr const char* kNetIfDir      = "/sys/class/net";
constexpr const char* kProcWireless  = "/proc/net/wireless";
constexpr int         kIffUp         = 0x1;

bool ReadFirstLine(const std::string& path, std::string& out) {
    std::ifstream in(path);
    if (!in) return false;
    if (!std::getline(in, out)) return false;
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    return true;
}

bool PathExists(const std::string& path) {
    return access(path.c_str(), F_OK) == 0;
}

// A wireless interface is identified by its sysfs wireless/ (or phy80211/)
// directory. Name prefixes are only a last resort: they miss predictable names
// and they match nothing when sysfs is hidden by a container.
bool IsWirelessInterface(const std::string& name) {
    if (name.empty() || name == "." || name == "..") return false;
    const std::string base = std::string(kNetIfDir) + "/" + name;
    if (PathExists(base + "/wireless") || PathExists(base + "/phy80211"))
        return true;
    return name.rfind("wl", 0) == 0 || name.rfind("wlan", 0) == 0;
}

// IFF_UP from /sys/class/net/<if>/flags. "powerOn" means the adapter is
// administratively enabled, which is exactly what IFF_UP reports.
bool InterfaceIsUp(const std::string& name) {
    std::string flags;
    if (!ReadFirstLine(std::string(kNetIfDir) + "/" + name + "/flags", flags))
        return false;
    char* end = nullptr;
    const unsigned long value = std::strtoul(flags.c_str(), &end, 0);
    if (end == flags.c_str()) return false;
    return (value & kIffUp) != 0;
}

std::vector<std::string> ListWirelessInterfaces() {
    std::vector<std::string> result;
    DIR* dir = opendir(kNetIfDir);
    if (!dir) return result;
    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        const std::string name(entry->d_name);
        if (IsWirelessInterface(name)) result.push_back(name);
    }
    closedir(dir);
    return result;
}

[[maybe_unused]] int ChannelFromFrequency(int mhz) {
    if (mhz == 2484) return 14;                 // channel 14 is the 2484 MHz exception
    if (mhz >= 2412 && mhz <= 2472) return (mhz - 2407) / 5;
    if (mhz >= 5000 && mhz < 5925) return (mhz - 5000) / 5;
    if (mhz >= 5955 && mhz <= 7115) return (mhz - 5950) / 5;
    return 0;
}

[[maybe_unused]] std::string BandFromFrequency(int mhz) {
    if (mhz >= 5955) return "6GHz";
    if (mhz >= 5000) return "5GHz";
    if (mhz >= 2400) return "2.4GHz";
    return {};
}

// /proc/net/wireless carries the kernel's dBm level:
//   wlan0: 0000   54.  -56.  -256 ...
// The trailing -256 is the "unavailable" sentinel, not a measurement.
bool ReadProcWirelessSignal(const std::string& iface, int& rssi, int& noise) {
    std::ifstream in(kProcWireless);
    if (!in) return false;
    std::string line;
    while (std::getline(in, line)) {
        char name[64] = {};
        unsigned int status = 0;
        float link = 0.0f, level = 0.0f, noiseFloor = 0.0f;
        if (std::sscanf(line.c_str(), " %63[^:]: %x %f %f %f",
                        name, &status, &link, &level, &noiseFloor) != 5) {
            continue;   // header lines have fewer fields
        }
        if (iface != name) continue;
        if (level < 0.0f) rssi = static_cast<int>(level);
        if (noiseFloor > -256.0f && noiseFloor < 0.0f)
            noise = static_cast<int>(noiseFloor);
        return true;
    }
    return false;
}

void LogData(const WiFiData& data, const char* source) {
    Logger::Debug(std::string("WiFiInfo[") + source + "]: connected=" +
                  (data.isConnected ? "yes" : "no") +
                  " ssid=" + data.ssid +
                  " bssid=" + data.bssid +
                  " rssi=" + std::to_string(data.rssi) + " dBm" +
                  " channel=" + std::to_string(data.channel) +
                  " band=" + data.band +
                  " txRate=" + std::to_string(data.txRate) + " Mbps");
}

// ============================================================================
// /proc/net/wireless fallback
// ============================================================================
void DetectViaProc(WiFiData& data) {
    const std::vector<std::string> interfaces = ListWirelessInterfaces();
    data.hasAdapter = !interfaces.empty();

    // powerOn reflects whether any wireless adapter is administratively up,
    // even when none is associated.
    for (const auto& iface : interfaces) {
        if (InterfaceIsUp(iface)) {
            data.powerOn = true;
            break;
        }
    }

    for (const auto& iface : interfaces) {
        int rssi = 0;
        int noise = 0;
        if (!ReadProcWirelessSignal(iface, rssi, noise)) continue;
        if (rssi >= 0) continue;   // no valid level: not associated
        data.isConnected = true;
        data.rssi = rssi;
        data.noise = noise;
        break;
    }
}

#ifdef TCMT_HAVE_DBUS
// ============================================================================
// NetworkManager over D-Bus
// ============================================================================

constexpr const char* kNmService         = "org.freedesktop.NetworkManager";
constexpr const char* kNmPath            = "/org/freedesktop/NetworkManager";
constexpr const char* kNmDeviceIface     = "org.freedesktop.NetworkManager.Device";
constexpr const char* kNmWirelessIface   = "org.freedesktop.NetworkManager.Device.Wireless";
constexpr const char* kNmAccessPointIface = "org.freedesktop.NetworkManager.AccessPoint";
constexpr const char* kPropertiesIface   = "org.freedesktop.DBus.Properties";
constexpr uint32_t    kNmDeviceTypeWifi  = 2;
constexpr int         kDbusTimeoutMs     = 700;

// NM_802_11_AP_SEC_* from NetworkManager's D-Bus API.
constexpr uint32_t kApSecPrivacy      = 0x1;
constexpr uint32_t kApSecKeyMgmtPsk   = 0x100;
constexpr uint32_t kApSecKeyMgmt8021x = 0x200;
constexpr uint32_t kApSecKeyMgmtSae   = 0x400;
constexpr uint32_t kApSecKeyMgmtOwe   = 0x800;

struct PropValue {
    int type = DBUS_TYPE_INVALID;
    bool boolean = false;
    uint64_t number = 0;
    int64_t signedNumber = 0;
    std::string text;
    std::vector<unsigned char> bytes;
};

using PropMap = std::map<std::string, PropValue>;

void ReadVariantValue(DBusMessageIter* value, PropValue& out) {
    const int type = dbus_message_iter_get_arg_type(value);
    out.type = type;
    switch (type) {
        case DBUS_TYPE_BOOLEAN: {
            dbus_bool_t v = FALSE;
            dbus_message_iter_get_basic(value, &v);
            out.boolean = v != FALSE;
            break;
        }
        case DBUS_TYPE_BYTE: {
            unsigned char v = 0;
            dbus_message_iter_get_basic(value, &v);
            out.number = v;
            break;
        }
        case DBUS_TYPE_UINT16: {
            dbus_uint16_t v = 0;
            dbus_message_iter_get_basic(value, &v);
            out.number = v;
            break;
        }
        case DBUS_TYPE_UINT32: {
            dbus_uint32_t v = 0;
            dbus_message_iter_get_basic(value, &v);
            out.number = v;
            break;
        }
        case DBUS_TYPE_UINT64: {
            dbus_uint64_t v = 0;
            dbus_message_iter_get_basic(value, &v);
            out.number = v;
            break;
        }
        case DBUS_TYPE_INT16: {
            dbus_int16_t v = 0;
            dbus_message_iter_get_basic(value, &v);
            out.signedNumber = v;
            break;
        }
        case DBUS_TYPE_INT32: {
            dbus_int32_t v = 0;
            dbus_message_iter_get_basic(value, &v);
            out.signedNumber = v;
            break;
        }
        case DBUS_TYPE_INT64: {
            dbus_int64_t v = 0;
            dbus_message_iter_get_basic(value, &v);
            out.signedNumber = v;
            break;
        }
        case DBUS_TYPE_STRING:
        case DBUS_TYPE_OBJECT_PATH:
        case DBUS_TYPE_SIGNATURE: {
            const char* s = nullptr;
            dbus_message_iter_get_basic(value, &s);
            if (s) out.text = s;
            break;
        }
        case DBUS_TYPE_ARRAY: {
            DBusMessageIter arr;
            dbus_message_iter_recurse(value, &arr);
            if (dbus_message_iter_get_arg_type(&arr) != DBUS_TYPE_BYTE) break;
            while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_BYTE) {
                unsigned char b = 0;
                dbus_message_iter_get_basic(&arr, &b);
                out.bytes.push_back(b);
                dbus_message_iter_next(&arr);
            }
            break;
        }
        default:
            break;
    }
}

DBusConnection* ConnectSystemBus() {
    DBusError error;
    dbus_error_init(&error);
    DBusConnection* conn = dbus_bus_get(DBUS_BUS_SYSTEM, &error);
    if (dbus_error_is_set(&error)) dbus_error_free(&error);
    if (!conn) return nullptr;
    // A bus restart must not take the TUI down with it.
    dbus_connection_set_exit_on_disconnect(conn, FALSE);
    return conn;
}

DBusMessage* SendWithReply(DBusConnection* conn, DBusMessage* message) {
    DBusError error;
    dbus_error_init(&error);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(
        conn, message, kDbusTimeoutMs, &error);
    if (dbus_error_is_set(&error)) dbus_error_free(&error);
    return reply;
}

DBusMessage* CallMethod(DBusConnection* conn, const char* service,
                        const char* path, const char* iface, const char* method) {
    DBusMessage* message = dbus_message_new_method_call(service, path, iface, method);
    if (!message) return nullptr;
    DBusMessage* reply = SendWithReply(conn, message);
    dbus_message_unref(message);
    return reply;
}

// org.freedesktop.DBus.Properties.GetAll -> a{sv}
bool GetAllProperties(DBusConnection* conn, const char* service, const char* path,
                      const char* iface, PropMap& out) {
    DBusMessage* message = dbus_message_new_method_call(
        service, path, kPropertiesIface, "GetAll");
    if (!message) return false;

    DBusMessageIter args;
    dbus_message_iter_init_append(message, &args);
    if (!dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &iface)) {
        dbus_message_unref(message);
        return false;
    }

    DBusMessage* reply = SendWithReply(conn, message);
    dbus_message_unref(message);
    if (!reply) return false;

    bool ok = false;
    DBusMessageIter it;
    if (dbus_message_iter_init(reply, &it) &&
        dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
        DBusMessageIter dict;
        dbus_message_iter_recurse(&it, &dict);
        ok = true;
        while (dbus_message_iter_get_arg_type(&dict) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter entry;
            dbus_message_iter_recurse(&dict, &entry);

            const char* key = nullptr;
            if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_STRING)
                dbus_message_iter_get_basic(&entry, &key);

            dbus_message_iter_next(&entry);
            if (dbus_message_iter_get_arg_type(&entry) == DBUS_TYPE_VARIANT) {
                DBusMessageIter variant;
                dbus_message_iter_recurse(&entry, &variant);
                PropValue value;
                ReadVariantValue(&variant, value);
                if (key) out[key] = std::move(value);
            }
            dbus_message_iter_next(&dict);
        }
    }

    dbus_message_unref(reply);
    return ok;
}

std::vector<std::string> GetObjectPathArray(DBusMessage* reply) {
    std::vector<std::string> paths;
    DBusMessageIter it;
    if (!dbus_message_iter_init(reply, &it)) return paths;
    if (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_ARRAY) return paths;

    DBusMessageIter arr;
    dbus_message_iter_recurse(&it, &arr);
    while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_OBJECT_PATH) {
        const char* path = nullptr;
        dbus_message_iter_get_basic(&arr, &path);
        if (path) paths.emplace_back(path);
        dbus_message_iter_next(&arr);
    }
    return paths;
}

std::string SecurityFromFlags(uint32_t apFlags, uint32_t wpaFlags, uint32_t rsnFlags) {
    if (rsnFlags & kApSecKeyMgmtSae) {
        return (rsnFlags & kApSecKeyMgmtPsk) ? "WPA3-SAE (transition)" : "WPA3-SAE";
    }
    if (rsnFlags & kApSecKeyMgmtOwe) return "OWE";
    if (rsnFlags & kApSecKeyMgmtPsk) return "WPA2-Personal";
    if (rsnFlags & kApSecKeyMgmt8021x) return "WPA2-Enterprise";
    if (rsnFlags != 0) return "WPA2";
    if (wpaFlags & kApSecKeyMgmtPsk) return "WPA-Personal";
    if (wpaFlags & kApSecKeyMgmt8021x) return "WPA-Enterprise";
    if (wpaFlags != 0) return "WPA";
    if (apFlags & kApSecPrivacy) return "WEP";
    return "Open";
}

struct WifiDevice {
    std::string path;
    std::string interfaceName;
    std::string accessPoint;
    double bitrateKbit = 0.0;
    bool isConnected = false;
};

// Fills data from one associated AP. Returns false when NM has no live AP.
// negotiatedRateKbit comes from Device.Wireless.Bitrate and wins over the AP's
// MaxBitRate, which is a capability, not the current rate.
bool FillFromAccessPoint(DBusConnection* conn, const std::string& apPath,
                         const std::string& interfaceName,
                         double negotiatedRateKbit, WiFiData& data) {
    if (apPath.empty() || apPath == "/") return false;

    PropMap ap;
    if (!GetAllProperties(conn, kNmService, apPath.c_str(),
                          kNmAccessPointIface, ap)) {
        return false;
    }

    const auto ssid = ap.find("Ssid");
    if (ssid != ap.end()) data.ssid.assign(ssid->second.bytes.begin(),
                                          ssid->second.bytes.end());

    const auto bssid = ap.find("HwAddress");
    if (bssid != ap.end()) data.bssid = bssid->second.text;

    int frequency = 0;
    const auto freq = ap.find("Frequency");
    if (freq != ap.end()) frequency = static_cast<int>(freq->second.number);
    data.channel = ChannelFromFrequency(frequency);
    data.band = BandFromFrequency(frequency);

    uint32_t wpaFlags = 0;
    uint32_t rsnFlags = 0;
    uint32_t apFlags = 0;
    const auto wpa = ap.find("WpaFlags");
    if (wpa != ap.end()) wpaFlags = static_cast<uint32_t>(wpa->second.number);
    const auto rsn = ap.find("RsnFlags");
    if (rsn != ap.end()) rsnFlags = static_cast<uint32_t>(rsn->second.number);
    const auto flags = ap.find("Flags");
    if (flags != ap.end()) apFlags = static_cast<uint32_t>(flags->second.number);
    data.security = SecurityFromFlags(apFlags, wpaFlags, rsnFlags);

    // NM reports both rates in kbit/s.
    double rateKbit = negotiatedRateKbit;
    if (rateKbit <= 0.0) {
        const auto maxRate = ap.find("MaxBitrate");
        if (maxRate != ap.end()) rateKbit = static_cast<double>(maxRate->second.number);
    }
    data.txRate = rateKbit / 1000.0;   // kbit/s -> Mbit/s

    // Same frequency/rate heuristic as the Windows WLAN path. NM does not
    // expose the PHY mode directly, so an unknown rate stays empty.
    if (frequency >= 6000) {
        data.wifiGen = "WiFi 6E";
    } else if (data.txRate >= 1201) {
        data.wifiGen = "WiFi 6";
    } else if (data.txRate >= 433) {
        data.wifiGen = "WiFi 5";
    } else if (data.txRate >= 150) {
        data.wifiGen = "WiFi 4";
    } else if (data.txRate > 0) {
        data.wifiGen = "WiFi 3";
    }

    // dBm comes from the kernel, not from NM's 0-100 Strength percentage.
    if (!interfaceName.empty()) {
        ReadProcWirelessSignal(interfaceName, data.rssi, data.noise);
    }

    data.isConnected = true;
    return true;
}

// Returns true when NetworkManager answered and at least one Wi-Fi device is
// known to it. False means "use the /proc fallback".
bool DetectViaNetworkManager(WiFiData& data, std::string& interfaceName) {
    DBusConnection* conn = ConnectSystemBus();
    if (!conn) return false;

    bool handled = false;

    DBusMessage* devicesReply = CallMethod(conn, kNmService, kNmPath,
                                           kNmService, "GetDevices");
    std::vector<std::string> devicePaths;
    if (devicesReply) {
        devicePaths = GetObjectPathArray(devicesReply);
        dbus_message_unref(devicesReply);
    }

    if (!devicePaths.empty()) {
        PropMap global;
        const bool haveGlobal = GetAllProperties(conn, kNmService, kNmPath,
                                                 kNmService, global);

        std::vector<WifiDevice> wifiDevices;
        bool anyInterfaceUp = false;

        for (const auto& path : devicePaths) {
            PropMap device;
            if (!GetAllProperties(conn, kNmService, path.c_str(),
                                  kNmDeviceIface, device)) {
                continue;
            }
            if (device["DeviceType"].number != kNmDeviceTypeWifi) continue;

            WifiDevice info;
            info.path = path;
            const auto iface = device.find("Interface");
            if (iface != device.end()) info.interfaceName = iface->second.text;

            if (!info.interfaceName.empty() && InterfaceIsUp(info.interfaceName))
                anyInterfaceUp = true;

            PropMap wireless;
            if (GetAllProperties(conn, kNmService, path.c_str(),
                                 kNmWirelessIface, wireless)) {
                const auto ap = wireless.find("ActiveAccessPoint");
                if (ap != wireless.end() && !ap->second.text.empty() &&
                    ap->second.text != "/") {
                    info.accessPoint = ap->second.text;
                    info.isConnected = true;
                }
                // Current negotiated rate (kbit/s).
                const auto bitrate = wireless.find("Bitrate");
                if (bitrate != wireless.end() && bitrate->second.number > 0) {
                    info.bitrateKbit = static_cast<double>(bitrate->second.number);
                }
            }

            wifiDevices.push_back(std::move(info));
        }

        if (!wifiDevices.empty()) {
            handled = true;
            data.hasAdapter = true;   // a wireless device exists, radio state below

            // Prefer an associated device; otherwise report the first adapter.
            const WifiDevice* selected = &wifiDevices.front();
            for (const auto& dev : wifiDevices) {
                if (dev.isConnected) { selected = &dev; break; }
            }
            interfaceName = selected->interfaceName;

            bool wirelessEnabled = anyInterfaceUp;
            const auto enabled = global.find("WirelessEnabled");
            if (haveGlobal && enabled != global.end())
                wirelessEnabled = enabled->second.boolean;
            data.powerOn = wirelessEnabled;

            if (selected->isConnected) {
                if (!FillFromAccessPoint(conn, selected->accessPoint,
                                         selected->interfaceName,
                                         selected->bitrateKbit, data)) {
                    data.isConnected = false;
                    data.txRate = 0.0;
                }
            }
        }
    }

    dbus_connection_unref(conn);
    return handled;
}
#endif  // TCMT_HAVE_DBUS

}  // namespace

// ============================================================================
// Public API
// ============================================================================

void WiFiInfo::Clear() {
    data_ = WiFiData{};
}

void WiFiInfo::RequestLocationAuthorization() {
    // Linux has no Location Services gate for SSID; nothing to ask for.
}

void WiFiInfo::Detect() {
    Clear();

#ifdef TCMT_HAVE_DBUS
    std::string interfaceName;
    if (DetectViaNetworkManager(data_, interfaceName)) {
        LogData(data_, "NetworkManager");
        return;
    }
#endif

    DetectViaProc(data_);
    LogData(data_, "proc");
}

const WiFiData& WiFiInfo::GetData() const {
    return data_;
}

#endif  // TCMT_LINUX
