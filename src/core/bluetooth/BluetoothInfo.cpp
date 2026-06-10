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

#include <cstring>
#include <iomanip>
#include <sstream>

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
            data_.adapter.powerOn = true;
            break;
        }

        CloseHandle(hRadio);
    } while (BluetoothFindNextRadio(hRadioFind, &hRadio));

    BluetoothFindRadioClose(hRadioFind);

    // -----------------------------------------------------------------------
    // Step 2: enumerate connected devices only
    // -----------------------------------------------------------------------
    BLUETOOTH_DEVICE_SEARCH_PARAMS searchParams;
    std::memset(&searchParams, 0, sizeof(searchParams));
    searchParams.dwSize = sizeof(BLUETOOTH_DEVICE_SEARCH_PARAMS);
    searchParams.fReturnConnected = TRUE;
    searchParams.fReturnRemembered = FALSE;
    searchParams.fReturnAuthenticated = FALSE;
    searchParams.fReturnUnknown = FALSE;
    searchParams.fIssueInquiry = FALSE;
    searchParams.cTimeoutMultiplier = 0;
    searchParams.hRadio = nullptr;

    BLUETOOTH_DEVICE_INFO deviceInfo;
    std::memset(&deviceInfo, 0, sizeof(deviceInfo));
    deviceInfo.dwSize = sizeof(BLUETOOTH_DEVICE_INFO);

    HBLUETOOTH_DEVICE_FIND hDeviceFind = BluetoothFindFirstDevice(&searchParams, &deviceInfo);
    if (hDeviceFind != nullptr) {
        do {
            BluetoothDeviceData dev;
            dev.name = WCharToUtf8(deviceInfo.szName);
            dev.address = FormatBtAddress(deviceInfo.Address);
            dev.connected = (deviceInfo.fConnected == TRUE);
            dev.remembered = (deviceInfo.fRemembered == TRUE);
            data_.devices.push_back(std::move(dev));

            deviceInfo.dwSize = sizeof(BLUETOOTH_DEVICE_INFO);
        } while (BluetoothFindNextDevice(hDeviceFind, &deviceInfo));
        BluetoothFindDeviceClose(hDeviceFind);
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
// macOS stub
// ============================================================================
#else
void BluetoothInfo::Clear() {
    data_ = BluetoothData{};
}

void BluetoothInfo::Detect() {
    Clear();
    Logger::Debug("BluetoothInfo: no-op on non-Windows platform");
}
#endif

const BluetoothData& BluetoothInfo::GetData() const {
    return data_;
}
