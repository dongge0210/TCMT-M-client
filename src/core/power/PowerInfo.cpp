#include "PowerInfo.h"
#include "../Utils/Logger.h"

#ifdef TCMT_WINDOWS
// ======================== Windows Implementation ========================
// NOTE: winsock2.h must be included BEFORE windows.h and wbemidl.h
#include <winsock2.h>
#include <windows.h>
#include <winioctl.h>   // CTL_CODE / FILE_DEVICE_BATTERY：WIN32_LEAN_AND_MEAN 不会带进来
#include <wbemidl.h>
#include <comdef.h>
#include <powrprof.h>
// 电池设备接口：SetupDi 枚举 + 三个电池 ioctl
#include <setupapi.h>
#include <devguid.h>
#include <batclass.h>
#include <cstdlib>   // wcstoull：WMI 的 uint64 以 VT_BSTR 十进制字符串过来
#include "../Utils/WMIManager.h"

// NO_SUBTYPE_GUID may not be declared in newer SDKs; define it locally.
// {fea3413e-7e05-4911-9a71-700331f1c294}
static const GUID kNoSubtypeGuid = {0xfea3413e, 0x7e05, 0x4911,
    {0x9a, 0x71, 0x70, 0x03, 0x31, 0xf1, 0xc2, 0x94}};
#include "../Utils/WinUtils.h"

#pragma comment(lib, "PowrProf.lib")
#pragma comment(lib, "Setupapi.lib")

// 从 WMI 读出来的数值按 vt 分派，别假设固定类型。
// CIM 的 uint64 在 VARIANT 里是 **VT_BSTR**（十进制字符串，VARIANT 没有无符号 64 位），
// 拿 VT_I4 去比会永远不成立 —— Win32_Battery 的 DesignVoltage 就是 uint64。
static bool VariantToUInt64(const VARIANT& v, uint64_t* out) {
    if (!out) return false;
    switch (v.vt) {
        case VT_UI8: *out = static_cast<uint64_t>(v.ullVal); return true;
        case VT_I8:  *out = v.llVal < 0 ? 0 : static_cast<uint64_t>(v.llVal); return true;
        case VT_UI4: *out = v.ulVal; return true;
        case VT_I4:  *out = v.lVal < 0 ? 0 : static_cast<uint64_t>(v.lVal); return true;
        case VT_UI2: *out = v.uiVal; return true;
        case VT_I2:  *out = v.iVal < 0 ? 0 : static_cast<uint64_t>(v.iVal); return true;
        case VT_BSTR: {
            if (!v.bstrVal) return false;
            wchar_t* end = nullptr;
            const unsigned long long parsed = wcstoull(v.bstrVal, &end, 10);
            if (end == v.bstrVal) return false;   // 不是数字，别当 0
            *out = static_cast<uint64_t>(parsed);
            return true;
        }
        default: return false;
    }
}

// 电池设备接口。WMI 的 Win32_Battery 只给一部分属性（没有 Voltage、没有 CycleCount），
// 真正的明细在设备本身：SetupDi 找到电池设备 → CreateFile → 三个 ioctl。
// 这是 Windows 上唯一不靠驱动、也不靠 WMI 的电池明细来源。
//
// 拿不到就补不上，绝不编数：单位是设备自定义（BATTERY_CAPACITY_RELATIVE）时容量一律不报，
// 因为那个数的单位不是 mWh，当成容量显示就是假的。
static void AppendBatteryDeviceDetail(std::vector<BatteryInfo>& out, bool acOnline) {
    HDEVINFO hdev = SetupDiGetClassDevs(&GUID_DEVCLASS_BATTERY, nullptr, nullptr,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (hdev == INVALID_HANDLE_VALUE) return;

    for (DWORD idx = 0; ; ++idx) {
        SP_DEVICE_INTERFACE_DATA did = {};
        did.cbSize = sizeof(did);
        if (!SetupDiEnumDeviceInterfaces(hdev, nullptr, &GUID_DEVCLASS_BATTERY, idx, &did)) break;

        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetail(hdev, &did, nullptr, 0, &need, nullptr);
        if (need == 0) continue;

        std::vector<BYTE> raw(need);
        auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA>(raw.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA);
        if (!SetupDiGetDeviceInterfaceDetail(hdev, &did, detail, need, &need, nullptr)) continue;

        HANDLE h = CreateFile(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;

        DWORD returned = 0;
        ULONG tag = 0;
        ULONG timeout = 0;
        if (DeviceIoControl(h, IOCTL_BATTERY_QUERY_TAG, &timeout, sizeof(timeout),
                            &tag, sizeof(tag), &returned, nullptr) && tag != 0) {
            BATTERY_INFORMATION info = {};
            BATTERY_QUERY_INFORMATION query = {};
            query.BatteryTag = tag;
            query.InformationLevel = BatteryInformation;
            const bool haveInfo = DeviceIoControl(h, IOCTL_BATTERY_QUERY_INFORMATION,
                                                  &query, sizeof(query),
                                                  &info, sizeof(info), &returned, nullptr) != 0;

            BATTERY_WAIT_STATUS wait = {};
            wait.BatteryTag = tag;
            BATTERY_STATUS status = {};
            const bool haveStatus = DeviceIoControl(h, IOCTL_BATTERY_QUERY_STATUS,
                                                    &wait, sizeof(wait),
                                                    &status, sizeof(status), &returned, nullptr) != 0;

            const bool relative = haveInfo && (info.Capabilities & BATTERY_CAPACITY_RELATIVE) != 0;

            if (out.empty()) {
                // WMI 一条都没给（查询失败或类不可用）：这份就是唯一来源
                BatteryInfo bat;
                bat.name = "Battery";
                if (haveInfo && !relative) {
                    bat.designCapacity = info.DesignedCapacity;
                    bat.fullChargeCapacity = info.FullChargedCapacity;
                    bat.cycleCount = info.CycleCount;
                }
                if (haveStatus) {
                    bat.voltage = status.Voltage;
                    if (!relative) bat.currentCapacity = status.Capacity;
                    if (haveInfo && !relative && info.FullChargedCapacity > 0) {
                        const uint64_t pct = static_cast<uint64_t>(status.Capacity) * 100 /
                                             info.FullChargedCapacity;
                        bat.chargePercent = static_cast<uint32_t>(pct > 100 ? 100 : pct);
                    }
                }
                bat.acOnline = acOnline;
                if (bat.designCapacity > 0 && bat.fullChargeCapacity > 0 &&
                    bat.designCapacity > bat.fullChargeCapacity) {
                    bat.wearLevel = 1.0 - static_cast<double>(bat.fullChargeCapacity) /
                                          static_cast<double>(bat.designCapacity);
                }
                out.push_back(bat);
            } else if (out.size() == 1) {
                // 只有一块电池：把 WMI 缺的那几项补上，不新增一条，
                // 否则同一块电池会在界面里出现两次
                BatteryInfo& bat = out[0];
                if (haveInfo && !relative) {
                    if (bat.designCapacity == 0)     bat.designCapacity = info.DesignedCapacity;
                    if (bat.fullChargeCapacity == 0) bat.fullChargeCapacity = info.FullChargedCapacity;
                    if (bat.cycleCount == 0)         bat.cycleCount = info.CycleCount;
                }
                if (haveStatus) {
                    if (bat.voltage == 0) bat.voltage = status.Voltage;
                    if (bat.currentCapacity == 0 && !relative) bat.currentCapacity = status.Capacity;
                }
                if (bat.designCapacity > 0 && bat.fullChargeCapacity > 0 &&
                    bat.designCapacity > bat.fullChargeCapacity) {
                    bat.wearLevel = 1.0 - static_cast<double>(bat.fullChargeCapacity) /
                                          static_cast<double>(bat.designCapacity);
                }
            }
        }
        CloseHandle(h);
    }

    SetupDiDestroyDeviceInfoList(hdev);
}

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

    // 取全部属性，不挑字段。WQL 里写一个 Win32_Battery 没有的属性会让**整条查询**失败，
    // 不是只丢那一列 —— 而 Voltage 和 CycleCount 就不是这个类的属性（它只有 DesignVoltage）。
    // 一条查不到的 WQL 换来的结果是「这台机器没有电池」，比缺两列难查得多。
    IEnumWbemClassObject* pEnumerator = nullptr;
    HRESULT hres = pSvc->ExecQuery(
        _bstr_t("WQL"),
        _bstr_t("SELECT * FROM Win32_Battery"),
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

            // DesignCapacity (mWh，uint32)
            if (SUCCEEDED(pclsObj->Get(L"DesignCapacity", 0, &vtProp, 0, 0))) {
                uint64_t v = 0;
                if (VariantToUInt64(vtProp, &v)) bat.designCapacity = static_cast<uint32_t>(v);
            }
            VariantClear(&vtProp);

            // FullChargeCapacity (mWh，uint32)
            if (SUCCEEDED(pclsObj->Get(L"FullChargeCapacity", 0, &vtProp, 0, 0))) {
                uint64_t v = 0;
                if (VariantToUInt64(vtProp, &v)) bat.fullChargeCapacity = static_cast<uint32_t>(v);
            }
            VariantClear(&vtProp);

            // EstimatedChargeRemaining (percent，uint16)
            if (SUCCEEDED(pclsObj->Get(L"EstimatedChargeRemaining", 0, &vtProp, 0, 0))) {
                uint64_t v = 0;
                if (VariantToUInt64(vtProp, &v)) bat.chargePercent = static_cast<uint32_t>(v);
            }
            VariantClear(&vtProp);

            // 电压 (mV)。这个类里叫 DesignVoltage，没有叫 Voltage 的属性；而且它是 **uint64**，
            // VARIANT 里以 VT_BSTR 十进制字符串过来，按 VT_I4 读会永远读不到。
            if (SUCCEEDED(pclsObj->Get(L"DesignVoltage", 0, &vtProp, 0, 0))) {
                uint64_t v = 0;
                if (VariantToUInt64(vtProp, &v)) bat.voltage = static_cast<uint32_t>(v);
            }
            VariantClear(&vtProp);

            // CycleCount：Win32_Battery 没有这个属性，留给下面的设备接口补
            if (SUCCEEDED(pclsObj->Get(L"CycleCount", 0, &vtProp, 0, 0))) {
                uint64_t v = 0;
                if (VariantToUInt64(vtProp, &v)) bat.cycleCount = static_cast<uint32_t>(v);
            }
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

    // WMI 给不到的那几项（电压、循环次数、容量）从电池设备本身补
    AppendBatteryDeviceDetail(batteries, acOnline);

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
#include "../Utils/Sysfs.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

namespace tcmt::linux {
namespace {

std::string PowerSupplyPath(const std::string& name) {
    return "/sys/class/power_supply/" + name;
}

std::vector<std::string> ListSupplyNames() {
    return ListDir("/sys/class/power_supply");
}

uint64_t ReadU64OrZero(const std::string& path) {
    uint64_t value = 0;
    ReadUInt64(path, value);
    return value;
}

}  // namespace

std::vector<std::string> ListBatteryPaths() {
    std::vector<std::string> paths;
    for (const std::string& name : ListSupplyNames()) {
        const std::string base = PowerSupplyPath(name);
        if (ReadLine(base + "/type") != "Battery") continue;
        paths.push_back(base);
    }
    return paths;
}

BatteryDetail ReadBatteryDetail(const std::string& basePath) {
    BatteryDetail detail;
    detail.present = true;
    detail.name = basePath.substr(basePath.find_last_of('/') + 1);

    detail.status = ReadLine(basePath + "/status");
    if (detail.status == "Charging") detail.charging = true;
    else if (detail.status == "Discharging") detail.discharging = true;
    else if (detail.status == "Full") detail.full = true;

    int64_t capacity = 0;
    if (ReadInt64(basePath + "/capacity", capacity) && capacity >= 0 &&
        capacity <= 100) {
        detail.percent = static_cast<int>(capacity);
    }

    detail.energyNowUWh = ReadU64OrZero(basePath + "/energy_now");
    detail.energyFullUWh = ReadU64OrZero(basePath + "/energy_full");
    detail.energyDesignUWh = ReadU64OrZero(basePath + "/energy_full_design");
    detail.chargeNowUAh = ReadU64OrZero(basePath + "/charge_now");
    detail.chargeFullUAh = ReadU64OrZero(basePath + "/charge_full");
    detail.chargeDesignUAh = ReadU64OrZero(basePath + "/charge_full_design");
    detail.voltageUV = ReadU64OrZero(basePath + "/voltage_now");

    int64_t current = 0;
    if (ReadInt64(basePath + "/current_now", current)) {
        detail.currentNegative = current < 0;
        detail.currentUA = static_cast<uint64_t>(current < 0 ? -current : current);
    }

    detail.powerUW = ReadU64OrZero(basePath + "/power_now");
    int64_t cycles = 0;
    if (ReadInt64(basePath + "/cycle_count", cycles) && cycles > 0) {
        detail.cycleCount = static_cast<int>(cycles);
    }

    // temp is in tenths of a degree Celsius.
    int64_t tempTenths = 0;
    if (ReadInt64(basePath + "/temp", tempTenths)) {
        const double celsius = static_cast<double>(tempTenths) / 10.0;
        if (celsius > -40.0 && celsius < 150.0) {
            detail.temperatureC = celsius;
            detail.temperatureValid = true;
        }
    }

    // Capacity in mWh.
    if (detail.energyFullUWh > 0) {
        detail.currentCapacityMWh = static_cast<uint32_t>(detail.energyNowUWh / 1000ULL);
        detail.fullCapacityMWh = static_cast<uint32_t>(detail.energyFullUWh / 1000ULL);
        detail.designCapacityMWh = static_cast<uint32_t>(detail.energyDesignUWh / 1000ULL);
    } else if (detail.chargeFullUAh > 0 && detail.voltageUV > 0) {
        // µAh * µV = 1e-12 Wh; convert to mWh: /1e9.
        const auto toMWh = [](uint64_t chargeUAh, uint64_t voltageUV) -> uint32_t {
            const long double wh =
                static_cast<long double>(chargeUAh) * static_cast<long double>(voltageUV) /
                1.0e9L;
            return static_cast<uint32_t>(wh);
        };
        detail.currentCapacityMWh = toMWh(detail.chargeNowUAh, detail.voltageUV);
        detail.fullCapacityMWh = toMWh(detail.chargeFullUAh, detail.voltageUV);
        detail.designCapacityMWh = toMWh(detail.chargeDesignUAh, detail.voltageUV);
    }

    if (detail.percent < 0) {
        if (detail.energyFullUWh > 0) {
            detail.percent = static_cast<int>(std::min<uint64_t>(
                100, detail.energyNowUWh * 100 / detail.energyFullUWh));
        } else if (detail.chargeFullUAh > 0) {
            detail.percent = static_cast<int>(std::min<uint64_t>(
                100, detail.chargeNowUAh * 100 / detail.chargeFullUAh));
        }
    }

    if (detail.powerUW > 0) {
        detail.watts = static_cast<double>(detail.powerUW) / 1.0e6;
        detail.powerValid = true;
    } else if (detail.voltageUV > 0 && detail.currentUA > 0) {
        detail.watts = (static_cast<double>(detail.voltageUV) / 1.0e6) *
                       (static_cast<double>(detail.currentUA) / 1.0e6);
        detail.powerValid = true;
    }

    return detail;
}

BatteryDetail ReadPrimaryBattery() {
    const std::vector<std::string> batteries = ListBatteryPaths();
    if (batteries.empty()) return {};
    return ReadBatteryDetail(batteries.front());
}

bool ReadAcOnline() {
    for (const std::string& name : ListSupplyNames()) {
        const std::string base = PowerSupplyPath(name);
        if (ReadLine(base + "/type") == "Battery") continue;
        int64_t online = 0;
        if (ReadInt64(base + "/online", online) && online == 1) return true;
    }
    return false;
}

double SampleCpuPackagePowerMilliwatts() {
    struct State {
        std::mutex mutex;
        bool primed = false;
        int64_t prevUs = 0;
        std::map<std::string, uint64_t> prevEnergy;
        double lastMilliwatts = -1.0;
    };
    static State state;

    struct Package {
        std::string path;
        uint64_t energy = 0;
        uint64_t maxEnergy = 0;
    };
    std::vector<Package> packages;

    for (const std::string& entry : ListDir("/sys/class/powercap")) {
        const bool intel = entry.compare(0, 10, "intel-rapl") == 0;
        const bool amd = entry.compare(0, 8, "amd-rapl") == 0;
        if (!intel && !amd) continue;
        // "intel-rapl:0" is a package; "intel-rapl:0:0" is a subdomain.
        if (entry.find(':', entry.find(':') + 1) != std::string::npos) continue;

        const std::string base = "/sys/class/powercap/" + entry;
        const std::string name = ReadLine(base + "/name");
        if (name.compare(0, 7, "package") != 0) continue;

        Package package;
        package.path = base;
        if (!ReadUInt64(base + "/energy_uj", package.energy)) continue;
        ReadUInt64(base + "/max_energy_range_uj", package.maxEnergy);
        packages.push_back(std::move(package));
    }

    if (packages.empty()) {
        static std::atomic<bool> logged{false};
        LogOnce(logged,
                "LinuxPower: RAPL energy_uj unavailable (root-only by default); "
                "CPU power reported as unavailable");
        return -1.0;
    }

    const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();

    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.primed) {
        for (const auto& package : packages) state.prevEnergy[package.path] = package.energy;
        state.prevUs = nowUs;
        state.primed = true;
        return -1.0;  // no delta yet
    }

    // The power loop runs at >= 1 s; ignore re-entrant calls from the UI path.
    if (nowUs - state.prevUs < 900000) return state.lastMilliwatts;

    const uint64_t deltaUs = static_cast<uint64_t>(nowUs - state.prevUs);
    long double totalMicroJoules = 0.0L;
    for (const auto& package : packages) {
        const auto it = state.prevEnergy.find(package.path);
        const uint64_t prev = it == state.prevEnergy.end() ? package.energy : it->second;
        uint64_t delta = 0;
        if (package.energy >= prev) {
            delta = package.energy - prev;
        } else if (package.maxEnergy > 0) {
            // Counter wrapped.
            delta = package.maxEnergy - prev + package.energy;
        }
        totalMicroJoules += static_cast<long double>(delta);
        state.prevEnergy[package.path] = package.energy;
    }
    state.prevUs = nowUs;

    // µJ / µs == W; convert to mW.
    const double milliwatts =
        static_cast<double>(totalMicroJoules) * 1000.0 / static_cast<double>(deltaUs);
    state.lastMilliwatts = milliwatts;
    return milliwatts;
}

}  // namespace tcmt::linux
#include <algorithm>
#include <utility>

void PowerInfo::Detect() {
    Logger::Debug("PowerInfo: Detecting battery/power data via sysfs (Linux)");

    batteries.clear();
    powerPlan = "Default";
    acOnline = tcmt::linux::ReadAcOnline();
    systemWattage = 0.0;

    for (const std::string& base : tcmt::linux::ListBatteryPaths()) {
        const tcmt::linux::BatteryDetail detail =
            tcmt::linux::ReadBatteryDetail(base);

        BatteryInfo bat;
        bat.name = detail.name;
        bat.designCapacity = detail.designCapacityMWh;
        bat.fullChargeCapacity = detail.fullCapacityMWh;
        bat.currentCapacity = detail.currentCapacityMWh;
        // power_supply voltage_now is microvolts; BatteryInfo stores mV.
        bat.voltage = static_cast<uint32_t>(detail.voltageUV / 1000ULL);
        bat.cycleCount = static_cast<uint32_t>(detail.cycleCount);
        // chargePercent stays 0 when the kernel exposes no capacity file and
        // no energy/charge counters; there is no better sentinel in this type.
        bat.chargePercent = detail.percent >= 0 ? static_cast<uint32_t>(detail.percent) : 0;

        if (detail.designCapacityMWh > 0 && detail.fullCapacityMWh > 0) {
            bat.wearLevel = detail.designCapacityMWh > detail.fullCapacityMWh
                                ? 1.0 - static_cast<double>(detail.fullCapacityMWh) /
                                            static_cast<double>(detail.designCapacityMWh)
                                : 0.0;
        }

        if (detail.status == "Charging") bat.chargingState = ChargingState::Charging;
        else if (detail.status == "Discharging") bat.chargingState = ChargingState::Discharging;
        else if (detail.status == "Full") bat.chargingState = ChargingState::Full;
        else if (detail.status == "Not charging") bat.chargingState = ChargingState::NotCharging;
        else bat.chargingState = ChargingState::Unknown;

        if (detail.powerValid) systemWattage += detail.watts;
        bat.acOnline = acOnline;
        batteries.push_back(std::move(bat));
    }

    const std::string governor =
        tcmt::linux::ReadLine("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor");
    if (!governor.empty()) powerPlan = governor;

    Logger::Debug("PowerInfo: acOnline=" + std::string(acOnline ? "yes" : "no") +
                  " plan=" + powerPlan +
                  " batteries=" + std::to_string(batteries.size()));
}

#else
#error "Unsupported platform"
#endif
