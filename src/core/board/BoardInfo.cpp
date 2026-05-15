#include "BoardInfo.h"
#include "../Utils/Logger.h"

#ifdef TCMT_WINDOWS
// ======================== Windows Implementation ========================
// NOTE: winsock2.h must be included BEFORE windows.h and wbemidl.h
#include <winsock2.h>
#include <windows.h>
#include <wbemidl.h>
#include <comdef.h>
#include "../Utils/WmiManager.h"
#include "../Utils/WinUtils.h"

void BoardInfo::Detect() {
    Logger::Debug("BoardInfo: Detecting motherboard/BIOS data via WMI");

    WmiManager wmi;
    if (!wmi.IsInitialized()) {
        Logger::Error("BoardInfo: WMI service not initialized");
        return;
    }

    IWbemServices* pSvc = wmi.GetWmiService();
    if (!pSvc) {
        Logger::Error("BoardInfo: WMI service invalid");
        return;
    }

    // --- Win32_BaseBoard ---
    IEnumWbemClassObject* pEnumerator = nullptr;
    HRESULT hres = pSvc->ExecQuery(
        _bstr_t("WQL"),
        _bstr_t("SELECT Manufacturer, Product, SerialNumber, Version FROM Win32_BaseBoard"),
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
        nullptr,
        &pEnumerator);

    if (SUCCEEDED(hres) && pEnumerator) {
        IWbemClassObject* pclsObj = nullptr;
        ULONG uReturn = 0;
        if (pEnumerator->Next(WBEM_INFINITE, 1, &pclsObj, &uReturn) == S_OK) {
            VARIANT vtProp;
            VariantInit(&vtProp);

            if (SUCCEEDED(pclsObj->Get(L"Manufacturer", 0, &vtProp, 0, 0)) && vtProp.vt == VT_BSTR)
                manufacturer = WinUtils::WstringToString(std::wstring(vtProp.bstrVal));
            VariantClear(&vtProp);

            if (SUCCEEDED(pclsObj->Get(L"Product", 0, &vtProp, 0, 0)) && vtProp.vt == VT_BSTR)
                product = WinUtils::WstringToString(std::wstring(vtProp.bstrVal));
            VariantClear(&vtProp);

            if (SUCCEEDED(pclsObj->Get(L"SerialNumber", 0, &vtProp, 0, 0)) && vtProp.vt == VT_BSTR)
                serialNumber = WinUtils::WstringToString(std::wstring(vtProp.bstrVal));
            VariantClear(&vtProp);

            if (SUCCEEDED(pclsObj->Get(L"Version", 0, &vtProp, 0, 0)) && vtProp.vt == VT_BSTR)
                version = WinUtils::WstringToString(std::wstring(vtProp.bstrVal));
            VariantClear(&vtProp);

            pclsObj->Release();
        }
        pEnumerator->Release();
    }

    // --- Win32_BIOS ---
    pEnumerator = nullptr;
    hres = pSvc->ExecQuery(
        _bstr_t("WQL"),
        _bstr_t("SELECT Manufacturer, SMBIOSBIOSVersion, ReleaseDate FROM Win32_BIOS"),
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
        nullptr,
        &pEnumerator);

    if (SUCCEEDED(hres) && pEnumerator) {
        IWbemClassObject* pclsObj = nullptr;
        ULONG uReturn = 0;
        if (pEnumerator->Next(WBEM_INFINITE, 1, &pclsObj, &uReturn) == S_OK) {
            VARIANT vtProp;
            VariantInit(&vtProp);

            if (SUCCEEDED(pclsObj->Get(L"Manufacturer", 0, &vtProp, 0, 0)) && vtProp.vt == VT_BSTR)
                biosVendor = WinUtils::WstringToString(std::wstring(vtProp.bstrVal));
            VariantClear(&vtProp);

            if (SUCCEEDED(pclsObj->Get(L"SMBIOSBIOSVersion", 0, &vtProp, 0, 0)) && vtProp.vt == VT_BSTR)
                biosVersion = WinUtils::WstringToString(std::wstring(vtProp.bstrVal));
            VariantClear(&vtProp);

            if (SUCCEEDED(pclsObj->Get(L"ReleaseDate", 0, &vtProp, 0, 0)) && vtProp.vt == VT_BSTR) {
                // WMI returns date in DMTF format: yyyymmddHHMMSS.xxxxxx+UUU
                std::wstring wDate = vtProp.bstrVal;
                if (wDate.length() >= 8) {
                    biosDate = WinUtils::WstringToString(
                        wDate.substr(0, 4) + L"-" +
                        wDate.substr(4, 2) + L"-" +
                        wDate.substr(6, 2));
                }
            }
            VariantClear(&vtProp);

            pclsObj->Release();
        }
        pEnumerator->Release();
    }

    // --- Win32_ComputerSystemProduct ---
    pEnumerator = nullptr;
    hres = pSvc->ExecQuery(
        _bstr_t("WQL"),
        _bstr_t("SELECT UUID FROM Win32_ComputerSystemProduct"),
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
        nullptr,
        &pEnumerator);

    if (SUCCEEDED(hres) && pEnumerator) {
        IWbemClassObject* pclsObj = nullptr;
        ULONG uReturn = 0;
        if (pEnumerator->Next(WBEM_INFINITE, 1, &pclsObj, &uReturn) == S_OK) {
            VARIANT vtProp;
            VariantInit(&vtProp);

            if (SUCCEEDED(pclsObj->Get(L"UUID", 0, &vtProp, 0, 0)) && vtProp.vt == VT_BSTR)
                systemUuid = WinUtils::WstringToString(std::wstring(vtProp.bstrVal));
            VariantClear(&vtProp);

            pclsObj->Release();
        }
        pEnumerator->Release();
    }

    // --- Win32_SystemEnclosure ---
    pEnumerator = nullptr;
    hres = pSvc->ExecQuery(
        _bstr_t("WQL"),
        _bstr_t("SELECT ChassisTypes FROM Win32_SystemEnclosure"),
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
        nullptr,
        &pEnumerator);

    if (SUCCEEDED(hres) && pEnumerator) {
        IWbemClassObject* pclsObj = nullptr;
        ULONG uReturn = 0;
        if (pEnumerator->Next(WBEM_INFINITE, 1, &pclsObj, &uReturn) == S_OK) {
            VARIANT vtProp;
            VariantInit(&vtProp);

            if (SUCCEEDED(pclsObj->Get(L"ChassisTypes", 0, &vtProp, 0, 0)) && vtProp.vt == (VT_ARRAY | VT_I4)) {
                // ChassisTypes is a uint16 array; extract first element
                SAFEARRAY* psa = vtProp.parray;
                long lBound = 0, uBound = 0;
                SafeArrayGetLBound(psa, 1, &lBound);
                SafeArrayGetUBound(psa, 1, &uBound);
                if (uBound >= lBound) {
                    int16_t chassisVal = 0;
                    long idx = lBound;
                    SafeArrayGetElement(psa, &idx, &chassisVal);
                    switch (chassisVal) {
                        case 1:  chassisType = "Other"; break;
                        case 2:  chassisType = "Unknown"; break;
                        case 3:  chassisType = "Desktop"; break;
                        case 4:  chassisType = "Low Profile Desktop"; break;
                        case 5:  chassisType = "Pizza Box"; break;
                        case 6:  chassisType = "Mini Tower"; break;
                        case 7:  chassisType = "Tower"; break;
                        case 8:  chassisType = "Portable"; break;
                        case 9:  chassisType = "Laptop"; break;
                        case 10: chassisType = "Notebook"; break;
                        case 11: chassisType = "Handheld"; break;
                        case 12: chassisType = "Docking Station"; break;
                        case 13: chassisType = "All-in-One"; break;
                        case 14: chassisType = "Sub-Notebook"; break;
                        case 15: chassisType = "Space-Saving"; break;
                        case 16: chassisType = "Lunch Box"; break;
                        case 17: chassisType = "Main System Chassis"; break;
                        case 18: chassisType = "Expansion Chassis"; break;
                        case 19: chassisType = "Sub-Chassis"; break;
                        case 20: chassisType = "Bus Expansion Chassis"; break;
                        case 21: chassisType = "Peripheral Chassis"; break;
                        case 22: chassisType = "Storage Chassis"; break;
                        case 23: chassisType = "Rack Mount Chassis"; break;
                        case 24: chassisType = "Sealed-Case PC"; break;
                        default: chassisType = "Chassis (" + std::to_string(chassisVal) + ")"; break;
                    }
                }
            }
            VariantClear(&vtProp);
            pclsObj->Release();
        }
        pEnumerator->Release();
    }

    Logger::Debug("BoardInfo: manufacturer=" + manufacturer +
                  " product=" + product +
                  " serial=" + serialNumber);
}

#elif defined(TCMT_MACOS)
// ======================== macOS Implementation ========================
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <sys/sysctl.h>
#include <cstring>

// Helper: read a string property from IOKit registry entry
static std::string IORegString(io_registry_entry_t entry, const char* key) {
    CFStringRef cfKey = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
    if (!cfKey) return {};
    CFTypeRef ref = IORegistryEntryCreateCFProperty(entry, cfKey, kCFAllocatorDefault, 0);
    CFRelease(cfKey);
    if (!ref) return {};
    std::string result;
    if (CFGetTypeID(ref) == CFStringGetTypeID()) {
        char buf[512] = {0};
        if (CFStringGetCString((CFStringRef)ref, buf, sizeof(buf), kCFStringEncodingUTF8))
            result = buf;
    }
    CFRelease(ref);
    return result;
}

void BoardInfo::Detect() {
    Logger::Debug("BoardInfo: Detecting board/BIOS data via IOKit + sysctl");

    // --- IOPlatformExpertDevice: manufacturer, product, serial, UUID ---
    mach_port_t masterPort;
    if (@available(macOS 12.0, *)) {
        masterPort = kIOMainPortDefault;
    } else {
        masterPort = kIOMasterPortDefault;
    }

    io_registry_entry_t platform = IOServiceGetMatchingService(
        masterPort,
        IOServiceMatching("IOPlatformExpertDevice"));

    if (platform) {
        manufacturer = IORegString(platform, "manufacturer");
        if (manufacturer.empty())
            manufacturer = "Apple Inc.";

        product = IORegString(platform, "board-id");
        if (product.empty())
            product = IORegString(platform, "model");

        serialNumber = IORegString(platform, "IOPlatformSerialNumber");

        systemUuid = IORegString(platform, "IOPlatformUUID");

        version = IORegString(platform, "version");

        IOObjectRelease(platform);
    }

    // Fallback for product: hw.model sysctl
    if (product.empty()) {
        char model[128] = {0};
        size_t len = sizeof(model);
        if (sysctlbyname("hw.model", model, &len, nullptr, 0) == 0) {
            product = model;
        }
    }

    // --- BIOS / firmware info via sysctl ---
    {
        char buf[256] = {0};
        size_t len = sizeof(buf);
        if (sysctlbyname("machdep.cpu.vendor", buf, &len, nullptr, 0) == 0) {
            // Not BIOS vendor on macOS, but good to have
        }
    }

    // Apple does not expose typical BIOS info on ARM Macs.
    // Read boot-args or firmware version where available.
    {
        io_registry_entry_t options = IOServiceGetMatchingService(
            masterPort,
            IOServiceMatching("IODeviceTree"));
        if (options) {
            // IODT doesn't have BIOS fields either; release and move on
            IOObjectRelease(options);
        }
    }

    // Fill in sensible defaults for macOS
    biosVendor = "Apple Inc.";
    biosVersion = "N/A (Apple Silicon/EFI)";
    biosDate = "";

    // Chassis type: detect via hw.model prefix
    if (product.find("MacBook") != std::string::npos) {
        chassisType = "Laptop";
    } else if (product.find("Macmini") != std::string::npos) {
        chassisType = "Desktop";
    } else if (product.find("MacPro") != std::string::npos || product.find("Mac") == 0) {
        chassisType = "Desktop";
    } else if (product.find("iMac") != std::string::npos) {
        chassisType = "All-in-One";
    } else if (product.find("MacBook") != std::string::npos) {
        chassisType = "Notebook";
    } else {
        chassisType = "Unknown";
    }

    Logger::Debug("BoardInfo: manufacturer=" + manufacturer +
                  " product=" + product +
                  " serial=" + serialNumber);
}

#else
#error "Unsupported platform"
#endif
