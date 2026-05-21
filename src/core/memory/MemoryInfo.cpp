#include "MemoryInfo.h"
#include "../Utils/Logger.h"

#ifdef TCMT_WINDOWS
// ======================== Windows Implementation ========================
#include <winsock2.h>
#include <windows.h>
#include <comdef.h>
#include <Wbemidl.h>
#include <algorithm>

MemoryInfo::MemoryInfo() : ramSpeed(0) {
    memStatus.dwLength = sizeof(memStatus);
    GlobalMemoryStatusEx(&memStatus);

    // Query RAM speed and DDR type via WMI Win32_PhysicalMemory
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool comInit = SUCCEEDED(hr);
    if (hr == RPC_E_CHANGED_MODE) comInit = true;

    IWbemLocator* pLoc = nullptr;
    IWbemServices* pSvc = nullptr;
    IEnumWbemClassObject* pEnum = nullptr;

    hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
        IID_IWbemLocator, (void**)&pLoc);
    if (SUCCEEDED(hr) && pLoc) {
        hr = pLoc->ConnectServer(_bstr_t(L"root\\cimv2"), nullptr, nullptr, nullptr,
            0, nullptr, nullptr, &pSvc);
        if (SUCCEEDED(hr) && pSvc) {
            CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);

            hr = pSvc->ExecQuery(_bstr_t("WQL"),
                _bstr_t("SELECT Speed,SMBIOSMemoryType FROM Win32_PhysicalMemory"),
                WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                nullptr, &pEnum);

            if (SUCCEEDED(hr) && pEnum) {
                IWbemClassObject* pObj = nullptr;
                ULONG uReturn = 0;
                uint32_t minSpeed = 0xFFFFFFFF;
                int minType = 0;

                while (pEnum->Next(WBEM_INFINITE, 1, &pObj, &uReturn) == S_OK) {
                    VARIANT vt;
                    VariantInit(&vt);

                    if (SUCCEEDED(pObj->Get(L"Speed", 0, &vt, 0, 0)) && vt.vt == VT_I4) {
                        uint32_t s = static_cast<uint32_t>(vt.lVal);
                        if (s > 0 && s < minSpeed) minSpeed = s;
                    }
                    VariantClear(&vt);

                    if (SUCCEEDED(pObj->Get(L"SMBIOSMemoryType", 0, &vt, 0, 0)) && vt.vt == VT_I4) {
                        minType = vt.lVal;
                    }
                    VariantClear(&vt);

                    pObj->Release();
                }
                if (minSpeed != 0xFFFFFFFF) ramSpeed = minSpeed;

                switch (minType) {
                case 24: ramType = "DDR3"; break;
                case 25: ramType = "LPDDR3"; break;
                case 26: ramType = "DDR4"; break;
                case 27: ramType = "LPDDR4"; break;
                case 34: ramType = "DDR5"; break;
                case 35: ramType = "LPDDR5"; break;
                default: ramType = (minType > 0 ? "DDR" : "Unknown"); break;
                }
            }
        }
    }

    if (pEnum) pEnum->Release();
    if (pSvc) pSvc->Release();
    if (pLoc) pLoc->Release();
    if (comInit && hr != RPC_E_CHANGED_MODE) CoUninitialize();
}

uint64_t MemoryInfo::GetTotalPhysical() const     { return memStatus.ullTotalPhys; }
uint64_t MemoryInfo::GetAvailablePhysical() const  { return memStatus.ullAvailPhys; }
uint64_t MemoryInfo::GetTotalVirtual() const       { return memStatus.ullTotalVirtual; }
uint32_t MemoryInfo::GetRamSpeed() const           { return ramSpeed; }
std::string MemoryInfo::GetRamType() const         { return ramType; }

#elif defined(TCMT_MACOS)
// ======================== macOS Implementation ========================
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <mach/vm_statistics.h>

// Apple Silicon model → LPDDR lookup (third-party tools like iStat Menus do this)
struct ASMemorySpec { uint32_t speed; const char* type; };
static ASMemorySpec LookupASMemory(const char* model) {
    // M4 family
    if (strstr(model, "Mac16"))  return {7500, "LPDDR5X"};  // M4 Pro/Max
    if (strstr(model, "Mac17"))  return {7500, "LPDDR5X"};  // M4
    // M3 family
    if (strstr(model, "Mac15"))  return {6400, "LPDDR5"};   // M3 Pro/Max
    if (strstr(model, "Mac14"))  return {6400, "LPDDR5"};   // M2 / M3 base
    // M1 family
    if (strstr(model, "Mac13"))  return {6400, "LPDDR5"};   // M2 Pro/Max
    if (strstr(model, "Mac12"))  return {6400, "LPDDR5"};   // M1
    if (strstr(model, "MacBookPro18") || strstr(model, "MacBookAir10"))
        return {4266, "LPDDR4X"};  // M1 base
    // Default for unknown Apple Silicon
    return {0, "Unified LPDDR"};
}

MemoryInfo::MemoryInfo() : totalPhysical(0), availablePhysical(0), totalVirtual(0), ramSpeed(0) {
    int mib[2] = {CTL_HW, HW_MEMSIZE};
    uint64_t memSize = 0;
    size_t len = sizeof(memSize);
    if (sysctl(mib, 2, &memSize, &len, nullptr, 0) == 0) totalPhysical = memSize;

    mach_port_t host = mach_host_self();
    vm_size_t pageSize = vm_kernel_page_size;
    vm_statistics64_data_t vmStats;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(host, HOST_VM_INFO64, (host_info64_t)&vmStats, &count) == KERN_SUCCESS) {
        availablePhysical = (vmStats.free_count + vmStats.inactive_count) * pageSize;
    } else {
        availablePhysical = totalPhysical;
    }
    totalVirtual = totalPhysical;

    // Detect CPU architecture
    uint32_t cpuType = 0;
    size_t cpuTypeLen = sizeof(cpuType);
    sysctlbyname("hw.cputype", &cpuType, &cpuTypeLen, nullptr, 0);

    if (cpuType == 0x0100000C) { // ARM64 = Apple Silicon
        char model[128] = {};
        size_t modelLen = sizeof(model);
        if (sysctlbyname("hw.model", model, &modelLen, nullptr, 0) == 0) {
            auto spec = LookupASMemory(model);
            ramSpeed = spec.speed;
            ramType = spec.type;
        }
    }
    // Intel not supported for now — requires IOKit AppleSMBIOS (future)
}

uint64_t MemoryInfo::GetTotalPhysical() const     { return totalPhysical; }
uint64_t MemoryInfo::GetAvailablePhysical() const  { return availablePhysical; }
uint64_t MemoryInfo::GetTotalVirtual() const       { return totalVirtual; }
uint32_t MemoryInfo::GetRamSpeed() const           { return ramSpeed; }
std::string MemoryInfo::GetRamType() const         { return ramType; }

#else
#error "Unsupported platform"
#endif
