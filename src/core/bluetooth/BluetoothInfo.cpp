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
    // Step 2: enumerate connected + paired + remembered devices (fast, no scan)
    // -----------------------------------------------------------------------
    BLUETOOTH_DEVICE_SEARCH_PARAMS searchParams;
    std::memset(&searchParams, 0, sizeof(searchParams));
    searchParams.dwSize = sizeof(BLUETOOTH_DEVICE_SEARCH_PARAMS);
    searchParams.fReturnConnected = TRUE;
    searchParams.fReturnRemembered = TRUE;   // paired devices
    searchParams.fReturnAuthenticated = TRUE;
    searchParams.fReturnUnknown = FALSE;
    searchParams.fIssueInquiry = FALSE;
    searchParams.cTimeoutMultiplier = 0;
    searchParams.hRadio = nullptr;

    BLUETOOTH_DEVICE_INFO deviceInfo;
    std::memset(&deviceInfo, 0, sizeof(deviceInfo));
    deviceInfo.dwSize = sizeof(BLUETOOTH_DEVICE_INFO);

    // Track seen addresses to avoid duplicates across search passes
    std::vector<std::string> seenAddresses;

    HBLUETOOTH_DEVICE_FIND hDeviceFind = BluetoothFindFirstDevice(&searchParams, &deviceInfo);
    if (hDeviceFind != nullptr) {
        do {
            BluetoothDeviceData dev;
            dev.name = WCharToUtf8(deviceInfo.szName);
            dev.address = FormatBtAddress(deviceInfo.Address);
            dev.connected = (deviceInfo.fConnected == TRUE);
            dev.remembered = (deviceInfo.fRemembered == TRUE);
            seenAddresses.push_back(dev.address);
            data_.devices.push_back(std::move(dev));

            deviceInfo.dwSize = sizeof(BLUETOOTH_DEVICE_INFO);
        } while (BluetoothFindNextDevice(hDeviceFind, &deviceInfo));
        BluetoothFindDeviceClose(hDeviceFind);
    }

    // -----------------------------------------------------------------------
    // Step 3: inquiry scan for nearby/discoverable devices (slow, ~2.5s)
    // -----------------------------------------------------------------------
    BLUETOOTH_DEVICE_SEARCH_PARAMS inquiryParams;
    std::memset(&inquiryParams, 0, sizeof(inquiryParams));
    inquiryParams.dwSize = sizeof(BLUETOOTH_DEVICE_SEARCH_PARAMS);
    inquiryParams.fReturnConnected = TRUE;
    inquiryParams.fReturnRemembered = TRUE;
    inquiryParams.fReturnAuthenticated = TRUE;
    inquiryParams.fReturnUnknown = TRUE;     // discoverable but unknown
    inquiryParams.fIssueInquiry = TRUE;       // perform radio scan
    inquiryParams.cTimeoutMultiplier = 2;     // ~2.56s scan
    inquiryParams.hRadio = hRadio;

    BLUETOOTH_DEVICE_INFO inquiryDeviceInfo;
    std::memset(&inquiryDeviceInfo, 0, sizeof(inquiryDeviceInfo));
    inquiryDeviceInfo.dwSize = sizeof(BLUETOOTH_DEVICE_INFO);

    HBLUETOOTH_DEVICE_FIND hInquiryFind = BluetoothFindFirstDevice(&inquiryParams, &inquiryDeviceInfo);
    if (hInquiryFind != nullptr) {
        do {
            std::string addr = FormatBtAddress(inquiryDeviceInfo.Address);
            // Skip duplicates from step 2
            bool duplicate = false;
            for (const auto& seen : seenAddresses) {
                if (seen == addr) { duplicate = true; break; }
            }
            if (!duplicate) {
                BluetoothDeviceData dev;
                dev.name = WCharToUtf8(inquiryDeviceInfo.szName);
                dev.address = addr;
                dev.connected = (inquiryDeviceInfo.fConnected == TRUE);
                dev.remembered = (inquiryDeviceInfo.fRemembered == TRUE);
                data_.devices.push_back(std::move(dev));
            }

            inquiryDeviceInfo.dwSize = sizeof(BLUETOOTH_DEVICE_INFO);
        } while (BluetoothFindNextDevice(hInquiryFind, &inquiryDeviceInfo));
        BluetoothFindDeviceClose(hInquiryFind);
    }

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
