#include "UsbInfo.h"
#include "../Utils/Logger.h"

#ifdef TCMT_WINDOWS
// ======================== Windows Implementation ========================
#include <winsock2.h>
#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <usbiodef.h>
#include <regex>
#include <sstream>
#include <iomanip>
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "cfgmgr32.lib")

UsbInfo::UsbInfo() {}

UsbInfo::~UsbInfo() {}

static uint16_t ParseHex16(const std::string& str) {
    uint16_t val = 0;
    std::stringstream ss(str);
    ss >> std::hex >> val;
    return val;
}

static bool ParseVidPidFromHardwareId(const std::wstring& hwId, uint16_t& vid, uint16_t& pid) {
    // Format: USB\VID_xxxx&PID_xxxx or USB\VID_xxxx&PID_xxxx&REV_xxxx
    std::wregex pattern(L"VID_([0-9A-Fa-f]{4}).*PID_([0-9A-Fa-f]{4})");
    std::wsmatch match;
    if (std::regex_search(hwId, match, pattern) && match.size() >= 3) {
        vid = ParseHex16(std::string(match[1].first, match[1].second));
        pid = ParseHex16(std::string(match[2].first, match[2].second));
        return true;
    }
    return false;
}

void UsbInfo::Detect() {
    devices_.clear();

    HDEVINFO devInfoSet = SetupDiGetClassDevs(
        &GUID_DEVINTERFACE_USB_DEVICE,
        nullptr,
        nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE
    );

    if (devInfoSet == INVALID_HANDLE_VALUE) {
        Logger::Error("UsbInfo: SetupDiGetClassDevs failed");
        return;
    }

    SP_DEVINFO_DATA devInfoData;
    devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

    for (DWORD i = 0; SetupDiEnumDeviceInfo(devInfoSet, i, &devInfoData); ++i) {
        UsbDevice device = {};
        WCHAR hwIdBuffer[256] = {0};

        // Get hardware ID (contains VID/PID)
        if (SetupDiGetDeviceRegistryProperty(
                devInfoSet, &devInfoData,
                SPDRP_HARDWAREID, nullptr,
                reinterpret_cast<PBYTE>(hwIdBuffer), sizeof(hwIdBuffer), nullptr)) {

            std::wstring hwId(hwIdBuffer);
            if (!ParseVidPidFromHardwareId(hwId, device.vid, device.pid)) {
                continue; // Not a USB device we can parse
            }
        }

        // Get friendly name
        WCHAR nameBuffer[256] = {0};
        if (SetupDiGetDeviceRegistryProperty(
                devInfoSet, &devInfoData,
                SPDRP_FRIENDLYNAME, nullptr,
                reinterpret_cast<PBYTE>(nameBuffer), sizeof(nameBuffer), nullptr)) {
            device.name = std::string(nameBuffer, nameBuffer + wcslen(nameBuffer));
        } else {
            // Fallback to device description
            WCHAR descBuffer[256] = {0};
            if (SetupDiGetDeviceRegistryProperty(
                    devInfoSet, &devInfoData,
                    SPDRP_DEVICEDESC, nullptr,
                    reinterpret_cast<PBYTE>(descBuffer), sizeof(descBuffer), nullptr)) {
                device.name = std::string(descBuffer, descBuffer + wcslen(descBuffer));
            } else {
                device.name = "Unknown USB Device";
            }
        }

        // Get manufacturer
        WCHAR mfrBuffer[256] = {0};
        if (SetupDiGetDeviceRegistryProperty(
                devInfoSet, &devInfoData,
                SPDRP_MFG, nullptr,
                reinterpret_cast<PBYTE>(mfrBuffer), sizeof(mfrBuffer), nullptr)) {
            device.manufacturer = std::string(mfrBuffer, mfrBuffer + wcslen(mfrBuffer));
        }

        // Try to get serial number via CM_Get_Device_ID
        WCHAR instanceId[256] = {0};
        if (SetupDiGetDeviceInstanceId(devInfoSet, &devInfoData, instanceId, 256, nullptr)) {
            std::wstring instanceStr(instanceId);
            // Hardware ID format: USB\VID_xxxx&PID_xxxx\SERIAL
            // The serial number is after the last backslash
            auto pos = instanceStr.rfind(L'\\');
            if (pos != std::wstring::npos && pos + 1 < instanceStr.length()) {
                std::wstring serialPart = instanceStr.substr(pos + 1);
                // Filter out non-serial patterns (e.g. rev_xxxx or mi_xx)
                std::wregex serialPattern(L"^[A-Fa-f0-9]{8,}|^\\S{4,}");
                if (std::regex_match(serialPart, serialPattern)) {
                    device.serialNumber = std::string(serialPart.begin(), serialPart.end());
                }
            }
        }

        // Get protocol version (bcdUSB) via device property
        // Use SPDRP_MAXIMUM to get the device property list - bcdUSB is not directly
        // available through SetupAPI. We'll populate from registry if available.
        // On Windows, bcdUSB is accessible via USB device node properties:
        HKEY hKey = SetupDiOpenDevRegKey(devInfoSet, &devInfoData,
                                         DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);
        if (hKey && hKey != INVALID_HANDLE_VALUE) {
            DWORD bcdUSB = 0;
            DWORD type = 0, size = sizeof(bcdUSB);
            if (RegQueryValueExW(hKey, L"bcdUSB", nullptr, &type,
                                 reinterpret_cast<LPBYTE>(&bcdUSB), &size) == ERROR_SUCCESS) {
                // bcdUSB is in BCD format: 0x0210 = USB 2.10
                int major = (bcdUSB >> 8) & 0xFF;
                int minor = bcdUSB & 0xFF;
                device.protocolVersion = "USB " + std::to_string(major) + "." +
                                         std::to_string(minor / 10) +
                                         (minor % 10 ? std::to_string(minor % 10) : "");
            }

            DWORD devicePower = 0;
            size = sizeof(devicePower);
            if (RegQueryValueExW(hKey, L"DevicePower", nullptr, &type,
                                 reinterpret_cast<LPBYTE>(&devicePower), &size) == ERROR_SUCCESS) {
                device.maxPower = devicePower;
            }

            DWORD selfPowered = 0;
            size = sizeof(selfPowered);
            if (RegQueryValueExW(hKey, L"SelfPowered", nullptr, &type,
                                 reinterpret_cast<LPBYTE>(&selfPowered), &size) == ERROR_SUCCESS) {
                device.isSelfPowered = (selfPowered != 0);
            }

            RegCloseKey(hKey);
        }

        devices_.push_back(std::move(device));
    }

    DWORD err = GetLastError();
    if (err != ERROR_NO_MORE_ITEMS) {
        Logger::Error("UsbInfo: SetupDiEnumDeviceInfo error: " + std::to_string(err));
    }

    SetupDiDestroyDeviceInfoList(devInfoSet);

    Logger::Debug("UsbInfo: detected " + std::to_string(devices_.size()) + " USB devices");
}

const std::vector<UsbDevice>& UsbInfo::GetDevices() const {
    return devices_;
}

#elif defined(TCMT_MACOS)
// ======================== macOS Implementation ========================
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <string>
#include <vector>

UsbInfo::UsbInfo() {}

UsbInfo::~UsbInfo() {}

static std::string CFStringToString(CFStringRef str) {
    if (!str) return {};
    char buffer[256] = {0};
    if (CFStringGetCString(str, buffer, sizeof(buffer), kCFStringEncodingUTF8)) {
        return std::string(buffer);
    }
    return {};
}

static uint16_t CFNumberToUInt16(CFNumberRef num) {
    if (!num) return 0;
    SInt32 val = 0;
    CFNumberGetValue(num, kCFNumberSInt32Type, &val);
    return static_cast<uint16_t>(val);
}

static uint32_t CFNumberToUInt32(CFNumberRef num) {
    if (!num) return 0;
    SInt32 val = 0;
    CFNumberGetValue(num, kCFNumberSInt32Type, &val);
    return static_cast<uint32_t>(val);
}

static bool CFBooleanToBool(CFBooleanRef val) {
    if (!val) return false;
    return CFBooleanGetValue(val);
}

static void ProcessUSBService(io_service_t service, std::vector<UsbDevice>& devices) {
    UsbDevice device = {};

    CFMutableDictionaryRef props = nullptr;
    IOReturn ret = IORegistryEntryCreateCFProperties(service, &props, kCFAllocatorDefault, kNilOptions);
    if (ret != kIOReturnSuccess || !props) {
        if (props) CFRelease(props);
        return;
    }

    // Vendor name
    CFStringRef vendorName = (CFStringRef)CFDictionaryGetValue(props, CFSTR("USB Vendor Name"));
    device.manufacturer = CFStringToString(vendorName);

    // Product name
    CFStringRef productName = (CFStringRef)CFDictionaryGetValue(props, CFSTR("USB Product Name"));
    if (!productName)
        productName = (CFStringRef)CFDictionaryGetValue(props, CFSTR("USB Product Name"));
    if (!productName)
        productName = (CFStringRef)CFDictionaryGetValue(props, CFSTR("iProduct"));
    device.name = CFStringToString(productName);

    if (device.name.empty()) {
        device.name = "Unknown USB Device";
    }

    // VID/PID
    CFNumberRef vidNum = (CFNumberRef)CFDictionaryGetValue(props, CFSTR("idVendor"));
    CFNumberRef pidNum = (CFNumberRef)CFDictionaryGetValue(props, CFSTR("idProduct"));
    device.vid = CFNumberToUInt16(vidNum);
    device.pid = CFNumberToUInt16(pidNum);

    // Serial number
    CFStringRef serial = (CFStringRef)CFDictionaryGetValue(props, CFSTR("USB Serial Number"));
    if (!serial)
        serial = (CFStringRef)CFDictionaryGetValue(props, CFSTR("iSerialNumber"));
    device.serialNumber = CFStringToString(serial);

    // Protocol version (bcdUSB)
    CFNumberRef bcdUSB = (CFNumberRef)CFDictionaryGetValue(props, CFSTR("bcdUSB"));
    if (bcdUSB) {
        uint32_t bcdVal = CFNumberToUInt32(bcdUSB);
        int major = (bcdVal >> 8) & 0xFF;
        int minor = bcdVal & 0xFF;
        device.protocolVersion = "USB " + std::to_string(major) + "." +
                                 std::to_string(minor / 10) +
                                 (minor % 10 ? std::to_string(minor % 10) : "");
    }

    // Power
    CFNumberRef maxPower = (CFNumberRef)CFDictionaryGetValue(props, CFSTR("USB Max Power"));
    if (maxPower) {
        device.maxPower = CFNumberToUInt32(maxPower);
    }

    CFBooleanRef selfPowered = (CFBooleanRef)CFDictionaryGetValue(props, CFSTR("Self Powered"));
    if (selfPowered) {
        device.isSelfPowered = CFBooleanToBool(selfPowered);
    }

    CFRelease(props);

    // Only add if we have at least VID/PID
    if (device.vid != 0 || device.pid != 0) {
        devices.push_back(std::move(device));
    }
}

void UsbInfo::Detect() {
    devices_.clear();

    // kIOMasterPortDefault is deprecated in macOS 12.0+ but available on all deployment targets
    #pragma clang diagnostic push
    #pragma clang diagnostic ignored "-Wdeprecated-declarations"
    mach_port_t mainPort = kIOMasterPortDefault;
    #pragma clang diagnostic pop

    // Try IOUSBHostDevice (macOS 10.15+) first, fall back to IOUSBDevice
    CFMutableDictionaryRef matching = IOServiceMatching("IOUSBHostDevice");
    if (!matching) {
        matching = IOServiceMatching("IOUSBDevice");
    }
    if (!matching) {
        Logger::Error("UsbInfo: IOServiceMatching failed");
        return;
    }

    io_iterator_t iterator = 0;
    IOReturn kr = IOServiceGetMatchingServices(mainPort, matching, &iterator);
    if (kr != kIOReturnSuccess) {
        Logger::Error("UsbInfo: IOServiceGetMatchingServices failed");
        return;
    }

    io_service_t service;
    while ((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
        ProcessUSBService(service, devices_);
        IOObjectRelease(service);
    }

    IOObjectRelease(iterator);

    Logger::Debug("UsbInfo: detected " + std::to_string(devices_.size()) + " USB devices");
}

const std::vector<UsbDevice>& UsbInfo::GetDevices() const {
    return devices_;
}

#else
#error "Unsupported platform"
#endif
