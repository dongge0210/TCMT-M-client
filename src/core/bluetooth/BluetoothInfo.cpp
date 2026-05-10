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
// Helper: format a BTH_ADDR (48-bit Bluetooth MAC) as "AA:BB:CC:DD:EE:FF"
// ---------------------------------------------------------------------------
static std::string FormatBtAddress(BTH_ADDR addr) {
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
        // No Bluetooth radio present or accessible -- return empty data
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

            // Try to honour the first radio found (usually the only one)
            break;
        }

        CloseHandle(hRadio);
    } while (BluetoothFindNextRadio(hRadioFind, &hRadio));

    BluetoothFindRadioClose(hRadioFind);

    // -----------------------------------------------------------------------
    // Step 2: enumerate connected/paired devices
    // -----------------------------------------------------------------------
    BLUETOOTH_DEVICE_SEARCH_PARAMS searchParams;
    std::memset(&searchParams, 0, sizeof(searchParams));
    searchParams.dwSize = sizeof(BLUETOOTH_DEVICE_SEARCH_PARAMS);
    searchParams.fReturnConnected = TRUE;
    searchParams.fReturnAuthenticated = FALSE;
    searchParams.fReturnRemembered = FALSE;
    searchParams.fReturnUnknown = FALSE;
    searchParams.fIssueInquiry = FALSE;
    searchParams.cTimeoutMultiplier = 0;
    searchParams.hRadio = nullptr; // search across all radios

    BLUETOOTH_DEVICE_INFO deviceInfo;
    std::memset(&deviceInfo, 0, sizeof(deviceInfo));
    deviceInfo.dwSize = sizeof(BLUETOOTH_DEVICE_INFO);

    HBLUETOOTH_DEVICE_FIND hDeviceFind = BluetoothFindFirstDevice(&searchParams, &deviceInfo);
    if (hDeviceFind == nullptr) {
        Logger::Debug("BluetoothInfo: no Bluetooth devices found");
        return;
    }

    do {
        BluetoothDeviceData dev;
        dev.name = WCharToUtf8(deviceInfo.szName);
        dev.address = FormatBtAddress(deviceInfo.Address);
        dev.connected = (deviceInfo.fConnected == TRUE);

        // RSSI is available on Windows 10 Anniversary Update (1607) and later.
        // The BLUETOOTH_DEVICE_INFO struct only exposes these members when
        // NTDDI_VERSION >= NTDDI_WIN10_RS1.  Use __if_exists so the code
        // compiles regardless of SDK version.
        {
            LONG rssiVal = 0;

#if defined(__clang__) || defined(__GNUC__)
            // GCC/Clang don't support __if_exists; skip RSSI for portability.
            (void)rssiVal;
#else
            // MSVC: __if_exists is a compile-time guard.
            __if_exists(deviceInfo.RSSI) {
                rssiVal = deviceInfo.RSSI;
            }
            __if_exists(deviceInfo.EmulatedRSSI) {
                if (rssiVal == 0) {
                    rssiVal = deviceInfo.EmulatedRSSI;
                }
            }
#endif
            if (rssiVal != 0) {
                dev.rssi = static_cast<int>(rssiVal);
            }
        }

        data_.devices.push_back(std::move(dev));

        // Reset dwSize before the next call (SDK requirement)
        deviceInfo.dwSize = sizeof(BLUETOOTH_DEVICE_INFO);
    } while (BluetoothFindNextDevice(hDeviceFind, &deviceInfo));

    BluetoothFindDeviceClose(hDeviceFind);

    Logger::Debug("BluetoothInfo: detected adapter \"" + data_.adapter.name +
                  "\" with " + std::to_string(data_.devices.size()) + " connected device(s)");
}

// ============================================================================
// Non-Windows stub
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
