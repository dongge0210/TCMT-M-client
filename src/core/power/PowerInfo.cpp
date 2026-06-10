#include "PowerInfo.h"
#include "../Utils/Logger.h"

#ifdef TCMT_WINDOWS
// ======================== Windows Implementation ========================
// NOTE: winsock2.h must be included BEFORE windows.h and wbemidl.h
#include <winsock2.h>
#include <windows.h>
#include <wbemidl.h>
#include <comdef.h>
#include <powrprof.h>
#include "../Utils/WmiManager.h"

// NO_SUBTYPE_GUID may not be declared in newer SDKs; define it locally.
// {fea3413e-7e05-4911-9a71-700331f1c294}
static const GUID kNoSubtypeGuid = {0xfea3413e, 0x7e05, 0x4911,
    {0x9a, 0x71, 0x70, 0x03, 0x31, 0xf1, 0xc2, 0x94}};
#include "../Utils/WinUtils.h"

#pragma comment(lib, "PowrProf.lib")

void PowerInfo::Detect() {
    Logger::Debug("PowerInfo: Detecting battery/power data via WMI + Power API");

    // --- AC status via GetSystemPowerStatus ---
    SYSTEM_POWER_STATUS powerStatus;
    if (GetSystemPowerStatus(&powerStatus)) {
        acOnline = (powerStatus.ACLineStatus == 1);
    }

    // --- Active power plan via PowerGetActiveScheme ---
    {
        GUID* pActiveGuid = nullptr;
        if (PowerGetActiveScheme(nullptr, &pActiveGuid) == ERROR_SUCCESS && pActiveGuid) {
            // Read the friendly name
            wchar_t friendlyName[256] = {0};
            DWORD bufSize = sizeof(friendlyName);
            if (PowerReadFriendlyName(nullptr, pActiveGuid,
                    &kNoSubtypeGuid, nullptr, (PUCHAR)friendlyName, &bufSize) == ERROR_SUCCESS) {
                powerPlan = WinUtils::WstringToString(std::wstring(friendlyName));
            } else {
                // Fallback: convert GUID to string
                wchar_t guidStr[64] = {0};
                StringFromGUID2(*pActiveGuid, guidStr, 64);
                powerPlan = WinUtils::WstringToString(std::wstring(guidStr));
            }
            LocalFree(pActiveGuid);
        }
    }

    // --- Batteries via WMI Win32_Battery ---
    WmiManager wmi;
    if (!wmi.IsInitialized()) {
        Logger::Error("PowerInfo: WMI service not initialized");
        return;
    }

    IWbemServices* pSvc = wmi.GetWmiService();
    if (!pSvc) {
        Logger::Error("PowerInfo: WMI service invalid");
        return;
    }

    IEnumWbemClassObject* pEnumerator = nullptr;
    HRESULT hres = pSvc->ExecQuery(
        _bstr_t("WQL"),
        _bstr_t("SELECT Name, DesignCapacity, FullChargeCapacity, "
                "EstimatedChargeRemaining, BatteryStatus, Voltage, "
                "CycleCount, Chemistry FROM Win32_Battery"),
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
        nullptr,
        &pEnumerator);

    if (SUCCEEDED(hres) && pEnumerator) {
        IWbemClassObject* pclsObj = nullptr;
        ULONG uReturn = 0;
        while (pEnumerator->Next(WBEM_INFINITE, 1, &pclsObj, &uReturn) == S_OK && pclsObj) {
            BatteryInfo bat;
            VARIANT vtProp;
            VariantInit(&vtProp);

            // Name
            if (SUCCEEDED(pclsObj->Get(L"Name", 0, &vtProp, 0, 0)) && vtProp.vt == VT_BSTR)
                bat.name = WinUtils::WstringToString(std::wstring(vtProp.bstrVal));
            VariantClear(&vtProp);

            // DesignCapacity (mWh)
            if (SUCCEEDED(pclsObj->Get(L"DesignCapacity", 0, &vtProp, 0, 0)) && vtProp.vt == VT_I4)
                bat.designCapacity = static_cast<uint32_t>(vtProp.lVal);
            VariantClear(&vtProp);

            // FullChargeCapacity (mWh)
            if (SUCCEEDED(pclsObj->Get(L"FullChargeCapacity", 0, &vtProp, 0, 0)) && vtProp.vt == VT_I4)
                bat.fullChargeCapacity = static_cast<uint32_t>(vtProp.lVal);
            VariantClear(&vtProp);

            // EstimatedChargeRemaining (percent)
            if (SUCCEEDED(pclsObj->Get(L"EstimatedChargeRemaining", 0, &vtProp, 0, 0)) && vtProp.vt == VT_I2)
                bat.chargePercent = static_cast<uint32_t>(vtProp.iVal);
            VariantClear(&vtProp);

            // Voltage (mV) — WMI reports in millivolts
            if (SUCCEEDED(pclsObj->Get(L"Voltage", 0, &vtProp, 0, 0)) && vtProp.vt == VT_I4)
                bat.voltage = static_cast<uint32_t>(vtProp.lVal);
            VariantClear(&vtProp);

            // CycleCount (Windows 8+)
            if (SUCCEEDED(pclsObj->Get(L"CycleCount", 0, &vtProp, 0, 0)) && vtProp.vt == VT_I4)
                bat.cycleCount = static_cast<uint32_t>(vtProp.lVal);
            VariantClear(&vtProp);

            // BatteryStatus — 1=discharging, 2=AC connected, 3=fully charged,
            // 4=low, 5=critical, 6=charging, 7=charging+high, 8=charging+low,
            // 9=charging+critical, 10=undefined, 11=partially charged
            if (SUCCEEDED(pclsObj->Get(L"BatteryStatus", 0, &vtProp, 0, 0)) && vtProp.vt == VT_I2) {
                int status = vtProp.iVal;
                switch (status) {
                    case 1:  bat.chargingState = ChargingState::Discharging; break;
                    case 2:  bat.chargingState = ChargingState::NotCharging; break;
                    case 3:  bat.chargingState = ChargingState::Full;        break;
                    case 6:
                    case 7:
                    case 8:
                    case 9:  bat.chargingState = ChargingState::Charging;    break;
                    default: bat.chargingState = ChargingState::Unknown;     break;
                }
            }
            VariantClear(&vtProp);

            // AC online
            bat.acOnline = acOnline;

            // Compute current capacity from percentage
            if (bat.fullChargeCapacity > 0 && bat.chargePercent > 0) {
                bat.currentCapacity = bat.fullChargeCapacity * bat.chargePercent / 100;
            }

            // Wear level
            if (bat.designCapacity > 0) {
                bat.wearLevel = bat.designCapacity > bat.fullChargeCapacity
                    ? 1.0 - static_cast<double>(bat.fullChargeCapacity) / bat.designCapacity
                    : 0.0;
            }

            batteries.push_back(bat);
            pclsObj->Release();
        }
        pEnumerator->Release();
    }

    Logger::Debug("PowerInfo: acOnline=" + std::string(acOnline ? "yes" : "no") +
                  " plan=" + powerPlan +
                  " batteries=" + std::to_string(batteries.size()));
}

#elif defined(TCMT_MACOS)
// ======================== macOS Implementation ========================
#include <IOKit/IOKitLib.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/ps/IOPSKeys.h>
#include <CoreFoundation/CoreFoundation.h>

void PowerInfo::Detect() {
    Logger::Debug("PowerInfo: Detecting battery/power data via IOKit + IOPowerSources");

    // --- AC status ---
    // Read the "External Connected" property from IOPowerSources
    CFTypeRef powerSources = IOPSCopyPowerSourcesInfo();
    if (powerSources) {
        CFArrayRef list = IOPSCopyPowerSourcesList(powerSources);
        if (list) {
            CFIndex count = CFArrayGetCount(list);
            for (CFIndex i = 0; i < count; i++) {
                CFTypeRef ps = CFArrayGetValueAtIndex(list, i);
                CFDictionaryRef dict = IOPSGetPowerSourceDescription(powerSources, ps);
                if (!dict) continue;

                // Power source state dict
                CFDictionaryRef psDict = (CFDictionaryRef)dict;

                // AC Power
                CFStringRef state = (CFStringRef)CFDictionaryGetValue(psDict, CFSTR(kIOPSPowerSourceStateKey));
                if (state && CFStringCompare(state, CFSTR(kIOPSACPowerValue), 0) == kCFCompareEqualTo) {
                    acOnline = true;
                }

                BatteryInfo bat;

                // Name
                CFStringRef nameVal = (CFStringRef)CFDictionaryGetValue(psDict, CFSTR(kIOPSNameKey));
                if (nameVal && CFGetTypeID(nameVal) == CFStringGetTypeID()) {
                    char buf[256] = {0};
                    if (CFStringGetCString(nameVal, buf, sizeof(buf), kCFStringEncodingUTF8))
                        bat.name = buf;
                }

                // Capacity — current capacity (mAh)
                CFNumberRef capVal = (CFNumberRef)CFDictionaryGetValue(psDict, CFSTR(kIOPSCurrentCapacityKey));
                if (capVal && CFGetTypeID(capVal) == CFNumberGetTypeID()) {
                    int val = 0;
                    CFNumberGetValue(capVal, kCFNumberIntType, &val);
                    bat.currentCapacity = static_cast<uint32_t>(val);
                }

                // Max capacity (mAh)
                CFNumberRef maxCapVal = (CFNumberRef)CFDictionaryGetValue(psDict, CFSTR(kIOPSMaxCapacityKey));
                if (maxCapVal && CFGetTypeID(maxCapVal) == CFNumberGetTypeID()) {
                    int val = 0;
                    CFNumberGetValue(maxCapVal, kCFNumberIntType, &val);
                    bat.fullChargeCapacity = static_cast<uint32_t>(val);
                }

                // Design capacity (mAh)
                CFNumberRef desCapVal = (CFNumberRef)CFDictionaryGetValue(psDict, CFSTR(kIOPSDesignCapacityKey));
                if (desCapVal && CFGetTypeID(desCapVal) == CFNumberGetTypeID()) {
                    int val = 0;
                    CFNumberGetValue(desCapVal, kCFNumberIntType, &val);
                    bat.designCapacity = static_cast<uint32_t>(val);
                }

                // Voltage (mV)
                CFNumberRef voltVal = (CFNumberRef)CFDictionaryGetValue(psDict, CFSTR(kIOPSVoltageKey));
                if (voltVal && CFGetTypeID(voltVal) == CFNumberGetTypeID()) {
                    int val = 0;
                    CFNumberGetValue(voltVal, kCFNumberIntType, &val);
                    bat.voltage = static_cast<uint32_t>(val);
                }

                // Cycle count — not available via IOPS; would need AppleSmartBattery IOKit

                // Charge percent
                if (bat.fullChargeCapacity > 0 && bat.currentCapacity <= bat.fullChargeCapacity) {
                    bat.chargePercent = bat.currentCapacity * 100 / bat.fullChargeCapacity;
                } else {
                    // Fallback: read directly
                    CFNumberRef pctVal = (CFNumberRef)CFDictionaryGetValue(psDict, CFSTR("Current Capacity"));
                    if (pctVal && CFGetTypeID(pctVal) == CFNumberGetTypeID()) {
                        int val = 0;
                        CFNumberGetValue(pctVal, kCFNumberIntType, &val);
                        bat.chargePercent = static_cast<uint32_t>(val);
                    }
                }

                // Wear level
                if (bat.designCapacity > 0) {
                    bat.wearLevel = bat.designCapacity > bat.fullChargeCapacity
                        ? 1.0 - static_cast<double>(bat.fullChargeCapacity) / bat.designCapacity
                        : 0.0;
                }

                // Charging state
                CFStringRef stateVal = (CFStringRef)CFDictionaryGetValue(psDict, CFSTR(kIOPSPowerSourceStateKey));
                if (stateVal && CFGetTypeID(stateVal) == CFStringGetTypeID()) {
                    if (CFStringCompare(stateVal, CFSTR(kIOPSACPowerValue), 0) == kCFCompareEqualTo) {
                        bat.chargingState = acOnline ? ChargingState::Full : ChargingState::Charging;
                    } else if (CFStringCompare(stateVal, CFSTR(kIOPSBatteryPowerValue), 0) == kCFCompareEqualTo) {
                        bat.chargingState = ChargingState::Discharging;
                    } else if (CFStringCompare(stateVal, CFSTR(kIOPSOffLineValue), 0) == kCFCompareEqualTo) {
                        bat.chargingState = ChargingState::NotCharging;
                    }
                }

                // Is charging flag
                CFBooleanRef isCharging = (CFBooleanRef)CFDictionaryGetValue(psDict, CFSTR(kIOPSIsChargingKey));
                if (isCharging && CFBooleanGetValue(isCharging)) {
                    bat.chargingState = ChargingState::Charging;
                }

                bat.acOnline = acOnline;

                // If no name was provided, generate one
                if (bat.name.empty()) {
                    bat.name = "InternalBattery-" + std::to_string(i);
                }

                batteries.push_back(bat);
            }
            CFRelease(list);
        }
        CFRelease(powerSources);
    }

    // Fallback: AppleSmartBattery IOKit service (for detailed data not in IOPowerSources)
    if (batteries.empty()) {
        mach_port_t masterPort;
        if (@available(macOS 12.0, *)) {
            masterPort = kIOMainPortDefault;
        } else {
            masterPort = kIOMasterPortDefault;
        }

        io_registry_entry_t batteryEntry = IOServiceGetMatchingService(
            masterPort,
            IOServiceMatching("AppleSmartBattery"));

        if (batteryEntry) {
            BatteryInfo bat;
            bat.name = "InternalBattery-0";

            CFMutableDictionaryRef props = nullptr;
            if (IORegistryEntryCreateCFProperties(batteryEntry, &props, kCFAllocatorDefault, 0) == KERN_SUCCESS && props) {
                auto readInt = [props](const char* key) -> uint32_t {
                    CFNumberRef val = (CFNumberRef)CFDictionaryGetValue(props, CFStringCreateWithCString(
                        kCFAllocatorDefault, key, kCFStringEncodingUTF8));
                    if (val && CFGetTypeID(val) == CFNumberGetTypeID()) {
                        int v = 0;
                        CFNumberGetValue(val, kCFNumberIntType, &v);
                        return static_cast<uint32_t>(v);
                    }
                    return 0;
                };

                bat.designCapacity = readInt("DesignCapacity");
                bat.fullChargeCapacity = readInt("MaxCapacity");
                bat.currentCapacity = readInt("CurrentCapacity");
                bat.voltage = readInt("Voltage");
                bat.cycleCount = readInt("CycleCount");

                // Compute percentage
                if (bat.fullChargeCapacity > 0) {
                    bat.chargePercent = bat.currentCapacity * 100 / bat.fullChargeCapacity;
                }

                // Wear level
                if (bat.designCapacity > 0) {
                    bat.wearLevel = bat.designCapacity > bat.fullChargeCapacity
                        ? 1.0 - static_cast<double>(bat.fullChargeCapacity) / bat.designCapacity
                        : 0.0;
                }

                bat.acOnline = acOnline;
                bat.chargingState = acOnline ? ChargingState::Charging : ChargingState::Discharging;

                batteries.push_back(bat);
                CFRelease(props);
            }
            IOObjectRelease(batteryEntry);
        }
    }

    // macOS does not expose a power plan concept like Windows
    powerPlan = "Default (macOS)";

    Logger::Debug("PowerInfo: acOnline=" + std::string(acOnline ? "yes" : "no") +
                  " batteries=" + std::to_string(batteries.size()));
}

#elif defined(TCMT_LINUX)
// ======================== Linux Implementation ========================
#include <fstream>
#include <sstream>
#include <dirent.h>
#include <cstring>
#include <algorithm>
#include <cctype>

void PowerInfo::Detect() {
    Logger::Debug("PowerInfo: Detecting battery/power data via sysfs (Linux)");

    acOnline = false;
    batteries.clear();
    powerPlan = "Default";

    DIR* psDir = opendir("/sys/class/power_supply");
    if (psDir) {
        struct dirent* entry;
        while ((entry = readdir(psDir)) != nullptr) {
            std::string name(entry->d_name);
            if (name == "." || name == "..") continue;

            std::string onlinePath = "/sys/class/power_supply/" + name + "/online";
            std::ifstream onlineFile(onlinePath);
            if (onlineFile.is_open()) {
                int online = 0;
                onlineFile >> online;
                if (online == 1) {
                    acOnline = true;
                    break;
                }
            }
        }
        closedir(psDir);
    }

    psDir = opendir("/sys/class/power_supply");
    if (psDir) {
        struct dirent* entry;
        while ((entry = readdir(psDir)) != nullptr) {
            std::string name(entry->d_name);
            if (name == "." || name == "..") continue;

            std::string typePath = "/sys/class/power_supply/" + name + "/type";
            std::string psType;
            {
                std::ifstream tf(typePath);
                if (tf.is_open()) std::getline(tf, psType);
            }
            psType.erase(0, psType.find_first_not_of(" \t\n\r"));
            psType.erase(psType.find_last_not_of(" \t\n\r") + 1);
            if (psType != "Battery") continue;

            BatteryInfo bat;
            bat.name = name;

            auto readUint64 = [](const std::string& path) -> uint64_t {
                std::ifstream f(path);
                if (!f.is_open()) return 0;
                uint64_t val = 0;
                f >> val;
                return val;
            };

            std::string base = "/sys/class/power_supply/" + name;

            uint64_t energyNow = readUint64(base + "/energy_now");
            uint64_t energyFull = readUint64(base + "/energy_full");
            uint64_t energyFullDesign = readUint64(base + "/energy_full_design");
            uint64_t chargeNow = readUint64(base + "/charge_now");
            uint64_t chargeFull = readUint64(base + "/charge_full");
            uint64_t chargeFullDesign = readUint64(base + "/charge_full_design");

            if (energyNow > 0 && energyFull > 0) {
                bat.currentCapacity = static_cast<uint32_t>(energyNow / 1000);
                bat.fullChargeCapacity = static_cast<uint32_t>(energyFull / 1000);
                bat.designCapacity = (energyFullDesign > 0)
                    ? static_cast<uint32_t>(energyFullDesign / 1000)
                    : bat.fullChargeCapacity;
            } else if (chargeNow > 0 && chargeFull > 0) {
                bat.currentCapacity = static_cast<uint32_t>(chargeNow / 1000);
                bat.fullChargeCapacity = static_cast<uint32_t>(chargeFull / 1000);
                bat.designCapacity = (chargeFullDesign > 0)
                    ? static_cast<uint32_t>(chargeFullDesign / 1000)
                    : bat.fullChargeCapacity;
            }

            uint64_t uv = readUint64(base + "/voltage_now");
            if (uv > 0)
                bat.voltage = static_cast<uint32_t>(uv / 1000);

            bat.cycleCount = static_cast<uint32_t>(readUint64(base + "/cycle_count"));

            if (bat.fullChargeCapacity > 0 && bat.currentCapacity <= bat.fullChargeCapacity)
                bat.chargePercent = bat.currentCapacity * 100 / bat.fullChargeCapacity;
            else if (bat.fullChargeCapacity > 0)
                bat.chargePercent = 100;

            if (bat.designCapacity > 0) {
                bat.wearLevel = bat.designCapacity > bat.fullChargeCapacity
                    ? 1.0 - static_cast<double>(bat.fullChargeCapacity) / bat.designCapacity
                    : 0.0;
            }

            std::string status;
            {
                std::ifstream sf(base + "/status");
                if (sf.is_open()) std::getline(sf, status);
            }
            status.erase(0, status.find_first_not_of(" \t\n\r"));
            status.erase(status.find_last_not_of(" \t\n\r") + 1);

            if (status == "Charging")
                bat.chargingState = ChargingState::Charging;
            else if (status == "Discharging")
                bat.chargingState = ChargingState::Discharging;
            else if (status == "Full")
                bat.chargingState = ChargingState::Full;
            else if (status == "Not charging")
                bat.chargingState = ChargingState::NotCharging;
            else
                bat.chargingState = ChargingState::Unknown;

            bat.acOnline = acOnline;
            batteries.push_back(bat);
        }
        closedir(psDir);
    }

    {
        std::ifstream govFile("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
        if (govFile.is_open()) {
            std::string gov;
            std::getline(govFile, gov);
            gov.erase(0, gov.find_first_not_of(" \t\n\r"));
            gov.erase(gov.find_last_not_of(" \t\n\r") + 1);
            if (!gov.empty()) powerPlan = gov;
        }
    }

    Logger::Debug("PowerInfo: acOnline=" + std::string(acOnline ? "yes" : "no") +
                  " plan=" + powerPlan +
                  " batteries=" + std::to_string(batteries.size()));
}

#else
#error "Unsupported platform"
#endif
