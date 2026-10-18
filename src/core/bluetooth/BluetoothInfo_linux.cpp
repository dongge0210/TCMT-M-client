// BluetoothInfo_linux.cpp - Linux Bluetooth information.
//
// Preferred source: BlueZ over the system bus (org.bluez). GetManagedObjects
// returns every adapter and device with their properties in one round trip,
// including Powered (adapter state) and Connected (device state), which sysfs
// does not expose. When BlueZ is absent the adapter list falls back to
// /sys/class/bluetooth; devices cannot be enumerated that way, so the list stays
// empty. No adapter at all leaves adapter.detected = false.
//
// libdbus-1 is optional (TCMT_HAVE_DBUS).

#include "BluetoothInfo.h"
#include "../Utils/Logger.h"

#ifdef TCMT_LINUX

#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <dirent.h>

#ifdef TCMT_HAVE_DBUS
#include <dbus/dbus.h>
#endif

namespace {

constexpr const char* kSysBluetoothDir = "/sys/class/bluetooth";

std::string ReadSysfs(const std::string& path) {
    std::ifstream in(path);
    if (!in) return {};
    std::string value;
    std::getline(in, value);
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
        value.pop_back();
    return value;
}

}  // namespace

// ============================================================================
// sysfs fallback (adapter only)
// ============================================================================
namespace {

bool DetectViaSysfs(BluetoothData& data) {
    DIR* dir = opendir(kSysBluetoothDir);
    if (!dir) return false;

    bool found = false;
    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        const std::string name(entry->d_name);
        if (name.rfind("hci", 0) != 0) continue;

        const std::string base = std::string(kSysBluetoothDir) + "/" + name;
        std::string address = ReadSysfs(base + "/address");
        if (address.empty()) continue;

        data.adapter.detected = true;
        data.adapter.address = address;
        std::string adapterName = ReadSysfs(base + "/name");
        data.adapter.name = adapterName.empty() ? name : adapterName;
        // sysfs has no "Powered" attribute. The rfkill state is readable via
        // /sys/class/rfkill, but mapping an hci index to an rfkill index is not
        // reliable, so powerOn stays false rather than being guessed.
        data.adapter.powerOn = false;
        found = true;
        break;   // first adapter only, matching the Windows implementation
    }

    closedir(dir);
    return found;
}

}  // namespace

#ifdef TCMT_HAVE_DBUS
// ============================================================================
// BlueZ over D-Bus
// ============================================================================
namespace {

constexpr const char* kBlueZService  = "org.bluez";
constexpr const char* kObjectManager = "org.freedesktop.DBus.ObjectManager";
constexpr int         kDbusTimeoutMs = 700;

struct PropValue {
    int type = DBUS_TYPE_INVALID;
    bool boolean = false;
    uint64_t number = 0;
    int64_t signedNumber = 0;
    std::string text;
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
        case DBUS_TYPE_STRING:
        case DBUS_TYPE_OBJECT_PATH: {
            const char* s = nullptr;
            dbus_message_iter_get_basic(value, &s);
            if (s) out.text = s;
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
    dbus_connection_set_exit_on_disconnect(conn, FALSE);
    return conn;
}

DBusMessage* CallGetManagedObjects(DBusConnection* conn) {
    DBusMessage* message = dbus_message_new_method_call(
        kBlueZService, "/", kObjectManager, "GetManagedObjects");
    if (!message) return nullptr;

    DBusError error;
    dbus_error_init(&error);
    DBusMessage* reply = dbus_connection_send_with_reply_and_block(
        conn, message, kDbusTimeoutMs, &error);
    if (dbus_error_is_set(&error)) dbus_error_free(&error);
    dbus_message_unref(message);
    return reply;
}

// a{sv} dictionary -> PropMap
void ReadProperties(DBusMessageIter* dict, PropMap& out) {
    while (dbus_message_iter_get_arg_type(dict) == DBUS_TYPE_DICT_ENTRY) {
        DBusMessageIter entry;
        dbus_message_iter_recurse(dict, &entry);

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
        dbus_message_iter_next(dict);
    }
}

bool DetectViaBlueZ(BluetoothData& data) {
    DBusConnection* conn = ConnectSystemBus();
    if (!conn) return false;

    DBusMessage* reply = CallGetManagedObjects(conn);
    if (!reply) {
        dbus_connection_unref(conn);
        return false;
    }

    bool sawAdapter = false;
    bool handled = false;

    DBusMessageIter it;
    if (dbus_message_iter_init(reply, &it) &&
        dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
        handled = true;
        DBusMessageIter objects;
        dbus_message_iter_recurse(&it, &objects);

        while (dbus_message_iter_get_arg_type(&objects) == DBUS_TYPE_DICT_ENTRY) {
            DBusMessageIter objectEntry;
            dbus_message_iter_recurse(&objects, &objectEntry);

            dbus_message_iter_next(&objectEntry);   // skip object path
            if (dbus_message_iter_get_arg_type(&objectEntry) != DBUS_TYPE_ARRAY) {
                dbus_message_iter_next(&objects);
                continue;
            }

            DBusMessageIter interfaces;
            dbus_message_iter_recurse(&objectEntry, &interfaces);

            while (dbus_message_iter_get_arg_type(&interfaces) == DBUS_TYPE_DICT_ENTRY) {
                DBusMessageIter ifaceEntry;
                dbus_message_iter_recurse(&interfaces, &ifaceEntry);

                const char* ifaceName = nullptr;
                if (dbus_message_iter_get_arg_type(&ifaceEntry) == DBUS_TYPE_STRING)
                    dbus_message_iter_get_basic(&ifaceEntry, &ifaceName);

                dbus_message_iter_next(&ifaceEntry);
                PropMap props;
                if (dbus_message_iter_get_arg_type(&ifaceEntry) == DBUS_TYPE_ARRAY) {
                    DBusMessageIter propDict;
                    dbus_message_iter_recurse(&ifaceEntry, &propDict);
                    ReadProperties(&propDict, props);
                }

                if (ifaceName && std::strcmp(ifaceName, "org.bluez.Adapter1") == 0) {
                    if (!sawAdapter) {   // first adapter only
                        data.adapter.detected = true;
                        data.adapter.powerOn = props["Powered"].boolean;
                        data.adapter.address = props["Address"].text;
                        data.adapter.name = props["Name"].text;
                        if (data.adapter.name.empty())
                            data.adapter.name = props["Alias"].text;
                        sawAdapter = true;
                    }
                } else if (ifaceName &&
                           std::strcmp(ifaceName, "org.bluez.Device1") == 0) {
                    // Only connected devices, matching the Windows implementation.
                    if (!props["Connected"].boolean) {
                        dbus_message_iter_next(&interfaces);
                        continue;
                    }
                    BluetoothDeviceData device;
                    device.connected = true;
                    device.remembered = props["Paired"].boolean;
                    device.address = props["Address"].text;
                    device.name = props["Name"].text;
                    if (device.name.empty()) device.name = props["Alias"].text;
                    if (props["RSSI"].type == DBUS_TYPE_INT16)
                        device.rssi = static_cast<int>(props["RSSI"].signedNumber);
                    data.devices.push_back(std::move(device));
                }

                dbus_message_iter_next(&interfaces);
            }

            dbus_message_iter_next(&objects);
        }
    }

    dbus_message_unref(reply);
    dbus_connection_unref(conn);
    return handled;
}

}  // namespace
#endif  // TCMT_HAVE_DBUS

// ============================================================================
// Public API
// ============================================================================

void BluetoothInfo::Clear() {
    data_ = BluetoothData{};
}

void BluetoothInfo::Detect() {
    Clear();

#ifdef TCMT_HAVE_DBUS
    if (DetectViaBlueZ(data_)) {
        Logger::Debug("BluetoothInfo[BlueZ]: adapter \"" + data_.adapter.name +
                      "\" powerOn=" + (data_.adapter.powerOn ? "yes" : "no") +
                      " devices=" + std::to_string(data_.devices.size()));
        return;
    }
#endif

    DetectViaSysfs(data_);
    Logger::Debug("BluetoothInfo[sysfs]: adapter \"" + data_.adapter.name +
                  "\" detected=" + (data_.adapter.detected ? "yes" : "no"));
}

const BluetoothData& BluetoothInfo::GetData() const {
    return data_;
}

#endif  // TCMT_LINUX
