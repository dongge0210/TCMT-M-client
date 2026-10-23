#include "BluetoothInfo.h"
#include "../Utils/Logger.h"

// ============================================================================
// Windows Implementation (bthprops.cpl API)
// ============================================================================
#ifdef TCMT_WINDOWS

// winsock2.h must come before windows.h to avoid Winsock redefinition errors
#include <winsock2.h>
#include <windows.h>
#include <bluetoothapis.h>
#include <bthsdpdef.h>

#pragma comment(lib, "Bthprops.lib")

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <vector>

// ---------------------------------------------------------------------------
// Helper: convert WCHAR (UTF-16) to UTF-8 std::string
// ---------------------------------------------------------------------------
static std::string WCharToUtf8(const WCHAR* wstr) {
    if (!wstr || wstr[0] == L'\0') return {};
    int len = static_cast<int>(wcslen(wstr));
    int needed = WideCharToMultiByte(CP_UTF8, 0, wstr, len, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string result(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr, len, &result[0], needed, nullptr, nullptr);
    return result;
}

// ---------------------------------------------------------------------------
// Helper: extract BTH_ADDR (ULONGLONG) from BLUETOOTH_ADDRESS union
// ---------------------------------------------------------------------------
static BTH_ADDR BtAddrToUlong(const BLUETOOTH_ADDRESS& ba) {
    // BLUETOOTH_ADDRESS is a union: { BTH_ADDR ullLong; BYTE rgBytes[6]; }
    // On older SDKs this is BLUETOOTH_ADDRESS_STRUCT; both have .ullLong.
    return ba.ullLong;
}

// ---------------------------------------------------------------------------
// Helper: format a BTH_ADDR (48-bit Bluetooth MAC) as "AA:BB:CC:DD:EE:FF"
// ---------------------------------------------------------------------------
static std::string FormatBtAddress(const BLUETOOTH_ADDRESS& ba) {
    BTH_ADDR addr = BtAddrToUlong(ba);
    std::ostringstream oss;
    for (int i = 5; i >= 0; --i) {
        if (i < 5) oss << ':';
        oss << std::hex << std::setfill('0') << std::setw(2)
            << static_cast<uint8_t>((addr >> (8ULL * i)) & 0xFF);
    }
    return oss.str();
}

// Bluetooth SIG company identifiers for the radios Windows machines ship
// with. Unknown values keep the number so the field is never empty.
static std::string BtVendorName(USHORT id) {
    switch (id) {
        case 0:   return "Ericsson";
        case 1:   return "Nokia";
        case 2:   return "Intel";
        case 3:   return "IBM";
        case 4:   return "Toshiba";
        case 5:   return "3Com";
        case 6:   return "Microsoft";
        case 9:   return "Infineon";
        case 10:  return "Qualcomm";
        case 13:  return "Texas Instruments";
        case 15:  return "Broadcom";
        case 29:  return "Qualcomm";
        case 70:  return "MediaTek";
        case 76:  return "Apple";
        case 93:  return "Realtek";
        case 117: return "Samsung";
        case 305: return "Cypress";
        default:  return "vendor " + std::to_string(id);
    }
}

// Class-of-Device major/minor classes (Bluetooth Assigned Numbers). Only the
// kinds of device you can actually pair are named; anything else stays empty
// and the UI simply omits the column.
static std::string BtDeviceType(ULONG cod) {
    const unsigned major = (cod >> 8) & 0x1F;
    const unsigned minor = (cod >> 2) & 0x3F;
    switch (major) {
        case 0x01:  // Computer
            switch (minor) {
                case 1: return "Desktop";
                case 2: return "Server";
                case 3: return "Laptop";
                case 4: return "Handheld";
                case 6: return "Wearable";
                case 7: return "Tablet";
                default: return "Computer";
            }
        case 0x02:  // Phone
            switch (minor) {
                case 1: return "Phone";
                case 2: return "Cordless phone";
                default: return "Phone";
            }
        case 0x04:  // Audio/video
            switch (minor) {
                case 1: return "Headset";
                case 2: return "Hands-free";
                case 4: return "Microphone";
                case 5: return "Speaker";
                case 6: return "Headphones";
                case 7: return "Portable audio";
                case 8: return "Car audio";
                case 10: return "Hi-Fi";
                default: return "Audio";
            }
        case 0x05: {  // Peripheral: the top two minor bits pick the device kind
            const unsigned sub = minor & 0x30;
            if (sub == 0x10) return "Keyboard";
            if (sub == 0x20) return "Mouse";
            if (sub == 0x30) return "Keyboard/mouse";
            if (minor == 0x01) return "Joystick";
            if (minor == 0x02) return "Gamepad";
            return "Peripheral";
        }
        case 0x06: return "Imaging";
        case 0x07: return "Wearable";
        case 0x08: return "Toy";
        case 0x09: return "Health";
        default: return {};
    }
}

void BluetoothInfo::Clear() {
    data_ = BluetoothData{};
}

void BluetoothInfo::Detect() {
    Clear();

    // -----------------------------------------------------------------------
    // Step 1: enumerate Bluetooth radios
    // -----------------------------------------------------------------------
    BLUETOOTH_FIND_RADIO_PARAMS radioParams;
    std::memset(&radioParams, 0, sizeof(radioParams));
    radioParams.dwSize = sizeof(BLUETOOTH_FIND_RADIO_PARAMS);

    HANDLE hRadio = nullptr;
    HBLUETOOTH_RADIO_FIND hRadioFind = BluetoothFindFirstRadio(&radioParams, &hRadio);

    if (hRadioFind == nullptr) {
        Logger::Debug("BluetoothInfo: no Bluetooth radio found");
        return;
    }

    do {
        BLUETOOTH_RADIO_INFO radioInfo;
        std::memset(&radioInfo, 0, sizeof(radioInfo));
        radioInfo.dwSize = sizeof(BLUETOOTH_RADIO_INFO);

        if (BluetoothGetRadioInfo(hRadio, &radioInfo) == ERROR_SUCCESS) {
            data_.adapter.name = WCharToUtf8(radioInfo.szName);
            data_.adapter.address = FormatBtAddress(radioInfo.address);
            // The Win32 radio info carries the SIG manufacturer ID, not a
            // chip name; that is the closest Windows gets to one.
            data_.adapter.chipset = BtVendorName(radioInfo.manufacturer);
            data_.adapter.powerOn = true;
            break;
        }

        CloseHandle(hRadio);
    } while (BluetoothFindNextRadio(hRadioFind, &hRadio));

    BluetoothFindRadioClose(hRadioFind);

    // -----------------------------------------------------------------------
    // Step 2: devices — connected first, then the paired list
    // -----------------------------------------------------------------------
    // Two passes because neither flag set alone is enough: a pass with only
    // fReturnConnected misses connected audio/BLE devices Windows does not
    // flag as such, and fConnected on a remembered-only enumeration is not
    // reliable either. The union marks a device connected if either pass has
    // seen it connected, and still lists every paired device.
    auto enumerateDevices = [](BOOL connected, BOOL remembered, BOOL authenticated,
                               std::vector<BLUETOOTH_DEVICE_INFO>& out) {
        BLUETOOTH_DEVICE_SEARCH_PARAMS searchParams;
        std::memset(&searchParams, 0, sizeof(searchParams));
        searchParams.dwSize = sizeof(BLUETOOTH_DEVICE_SEARCH_PARAMS);
        searchParams.fReturnConnected = connected;
        searchParams.fReturnRemembered = remembered;
        searchParams.fReturnAuthenticated = authenticated;
        searchParams.fReturnUnknown = FALSE;
        searchParams.fIssueInquiry = FALSE;
        searchParams.cTimeoutMultiplier = 0;
        searchParams.hRadio = nullptr;

        BLUETOOTH_DEVICE_INFO deviceInfo;
        std::memset(&deviceInfo, 0, sizeof(deviceInfo));
        deviceInfo.dwSize = sizeof(BLUETOOTH_DEVICE_INFO);

        HBLUETOOTH_DEVICE_FIND hFind = BluetoothFindFirstDevice(&searchParams, &deviceInfo);
        if (hFind == nullptr) return;
        do {
            out.push_back(deviceInfo);
            deviceInfo.dwSize = sizeof(BLUETOOTH_DEVICE_INFO);
        } while (BluetoothFindNextDevice(hFind, &deviceInfo));
        BluetoothFindDeviceClose(hFind);
    };

    std::vector<BLUETOOTH_DEVICE_INFO> connected;
    enumerateDevices(TRUE, FALSE, FALSE, connected);

    std::vector<BLUETOOTH_DEVICE_INFO> all;
    enumerateDevices(TRUE, TRUE, TRUE, all);

    for (const auto& info : all) {
        BluetoothDeviceData dev;
        dev.name = WCharToUtf8(info.szName);
        dev.address = FormatBtAddress(info.Address);
        dev.type = BtDeviceType(info.ulClassofDevice);
        const BTH_ADDR addr = BtAddrToUlong(info.Address);
        const bool inConnectedPass =
            std::any_of(connected.begin(), connected.end(),
                        [addr](const BLUETOOTH_DEVICE_INFO& c) {
                            return BtAddrToUlong(c.Address) == addr;
                        });
        dev.connected = (info.fConnected == TRUE) || inConnectedPass;
        dev.remembered = (info.fRemembered == TRUE);
        data_.devices.push_back(std::move(dev));
    }

    // NOTE: Inquiry scan (fIssueInquiry=TRUE) is intentionally NOT performed
    // here because it blocks for ~2.5s per call and Detect() runs every ~3
    // seconds.  If a nearby-device scan is needed it should be a separate
    // one-shot API, not part of the periodic collection loop.

    Logger::Debug("BluetoothInfo: detected adapter \"" + data_.adapter.name +
                  "\" with " + std::to_string(data_.devices.size()) + " connected device(s)");
}

// ============================================================================
// Linux Implementation (sysfs)
// ============================================================================
#elif defined(TCMT_LINUX)
#include <fstream>
#include <sstream>
#include <dirent.h>
#include <cstring>
#include <cstdio>
#include <memory>
#include <unistd.h>

void BluetoothInfo::Clear() {
    data_ = BluetoothData{};
}

static std::string ReadSysfsStr(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs) return {};
    std::string val;
    std::getline(ifs, val);
    return val;
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

void BluetoothInfo::Detect() {
    Clear();

    DIR* dir = opendir("/sys/class/bluetooth/");
    if (!dir) {
        Logger::Debug("BluetoothInfo: no Bluetooth adapter found");
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name(entry->d_name);
        if (name.compare(0, 3, "hci") != 0) continue;

        std::string btPath = "/sys/class/bluetooth/" + name + "/";
        std::string addr = ReadSysfsStr(btPath + "address");
        if (addr.empty()) continue;

        data_.adapter.detected = true;
        data_.adapter.address = addr;
        data_.adapter.powerOn = true;

        std::string devName = ReadSysfsStr(btPath + "name");
        if (!devName.empty()) {
            data_.adapter.name = devName;
        } else {
            // Fallback: try hcitool
            std::string hciOut = ExecCmd("hcitool dev 2>/dev/null");
            if (hciOut.find(name) != std::string::npos) {
                data_.adapter.name = name;
            } else {
                data_.adapter.name = "Bluetooth Adapter (" + name + ")";
            }
        }
        break; // Use first adapter only
    }
    closedir(dir);

    // Device list is not populated via simple sysfs reading
    // Full device enumeration requires D-Bus (bluez)

    Logger::Debug("BluetoothInfo: detected adapter \"" + data_.adapter.name +
                  "\" with " + std::to_string(data_.devices.size()) + " device(s)");
}

// ============================================================================
#else
// Unreachable: this file is registered only under if(TCMT_WINDOWS) in
// src/CMakeLists.txt. macOS uses BluetoothInfo.mm, and the TCMT_LINUX branch above is
// kept as a starting point but is not registered either. A no-op stub here would turn
// "no Bluetooth support on this platform" into "no devices", so fail loudly instead.
#error "BluetoothInfo.cpp is Windows-only. Do not add it to a non-Windows build."
#endif

const BluetoothData& BluetoothInfo::GetData() const {
    return data_;
}
