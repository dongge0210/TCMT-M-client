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
                int moduleCount = 0;

                while (pEnum->Next(WBEM_INFINITE, 1, &pObj, &uReturn) == S_OK) {
                    ++moduleCount;
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
                ramSlotsUsed = moduleCount;

                // Total slots live in the SMBIOS physical memory array
                // (Type 16); Win32_PhysicalMemory only lists installed sticks,
                // so an empty slot is invisible there.
                IEnumWbemClassObject* pArr = nullptr;
                if (SUCCEEDED(pSvc->ExecQuery(_bstr_t(L"WQL"),
                        _bstr_t("SELECT MemoryDevices FROM Win32_PhysicalMemoryArray"),
                        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                        nullptr, &pArr)) && pArr) {
                    IWbemClassObject* pSlot = nullptr;
                    ULONG retSlot = 0;
                    while (pArr->Next(WBEM_INFINITE, 1, &pSlot, &retSlot) == S_OK) {
                        VARIANT v;
                        VariantInit(&v);
                        if (SUCCEEDED(pSlot->Get(L"MemoryDevices", 0, &v, 0, 0))) {
                            if (v.vt == VT_I4) ramSlotsTotal += v.intVal;
                            else if (v.vt == VT_UI4) ramSlotsTotal += static_cast<int>(v.uintVal);
                        }
                        VariantClear(&v);
                        pSlot->Release();
                    }
                    pArr->Release();
                }

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
int MemoryInfo::GetRamSlotsTotal() const           { return ramSlotsTotal; }
int MemoryInfo::GetRamSlotsUsed() const            { return ramSlotsUsed; }

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
// Soldered unified memory: there are no slots to count.
int MemoryInfo::GetRamSlotsTotal() const           { return 0; }
int MemoryInfo::GetRamSlotsUsed() const            { return 0; }

#elif defined(TCMT_LINUX)
// ======================== Linux Implementation ========================
#include "../Utils/Sysfs.h"
#include <atomic>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace {

// /proc/meminfo values are kB. Returns 0 when the key is absent.
uint64_t MeminfoKb(const std::string& line, const char* key) {
    const size_t keyLen = std::strlen(key);
    if (line.compare(0, keyLen, key) != 0) return 0;
    std::istringstream ss(line.substr(keyLen));
    uint64_t kb = 0;
    if (!(ss >> kb)) return 0;
    return kb;
}

}  // namespace

MemoryInfo::MemoryInfo()
    : totalPhysical(0), availablePhysical(0), totalVirtual(0),
      swapTotal(0), swapFree(0), ramSpeed(0) {
    uint64_t memFree = 0, buffers = 0, cached = 0, sReclaimable = 0;
    bool haveAvailable = false;

    std::ifstream meminfo("/proc/meminfo");
    if (meminfo.is_open()) {
        std::string line;
        while (std::getline(meminfo, line)) {
            if (line.compare(0, 9, "MemTotal:") == 0) {
                totalPhysical = MeminfoKb(line, "MemTotal:") * 1024ULL;
            } else if (line.compare(0, 13, "MemAvailable:") == 0) {
                // MemAvailable already accounts for reclaimable page cache
                // and slab; it is the value "free"-style tools report.
                availablePhysical = MeminfoKb(line, "MemAvailable:") * 1024ULL;
                haveAvailable = true;
            } else if (line.compare(0, 8, "MemFree:") == 0) {
                memFree = MeminfoKb(line, "MemFree:") * 1024ULL;
            } else if (line.compare(0, 8, "Buffers:") == 0) {
                buffers = MeminfoKb(line, "Buffers:") * 1024ULL;
            } else if (line.compare(0, 7, "Cached:") == 0) {
                cached = MeminfoKb(line, "Cached:") * 1024ULL;
            } else if (line.compare(0, 14, "SReclaimable:") == 0) {
                sReclaimable = MeminfoKb(line, "SReclaimable:") * 1024ULL;
            } else if (line.compare(0, 10, "SwapTotal:") == 0) {
                swapTotal = MeminfoKb(line, "SwapTotal:") * 1024ULL;
            } else if (line.compare(0, 9, "SwapFree:") == 0) {
                swapFree = MeminfoKb(line, "SwapFree:") * 1024ULL;
            }
        }
    } else {
        // MemoryLoop constructs a MemoryInfo every second; one line is enough.
        static std::atomic<bool> logged{false};
        tcmt::linux::LogOnce(logged,
                             "MemoryInfo: /proc/meminfo unreadable; memory stats unavailable",
                             true);
    }

    // Kernels before 3.14 have no MemAvailable. Approximate it the same way
    // the kernel does: free + reclaimable cache/slab.
    if (!haveAvailable) {
        availablePhysical = memFree + buffers + cached + sReclaimable;
    }
    if (availablePhysical > totalPhysical) availablePhysical = totalPhysical;

    totalVirtual = totalPhysical + swapTotal;
    // RAM type/speed live in DMI (root-only on most distributions). There is
    // no unprivileged sysfs equivalent, so leave them empty instead of
    // guessing from the CPU model.
    ramSpeed = 0;
    ramType.clear();
}

uint64_t MemoryInfo::GetTotalPhysical() const     { return totalPhysical; }
uint64_t MemoryInfo::GetAvailablePhysical() const  { return availablePhysical; }
uint64_t MemoryInfo::GetTotalVirtual() const       { return totalVirtual; }
uint32_t MemoryInfo::GetRamSpeed() const           { return ramSpeed; }
std::string MemoryInfo::GetRamType() const         { return ramType; }
int MemoryInfo::GetRamSlotsTotal() const           { return 0; }
int MemoryInfo::GetRamSlotsUsed() const            { return 0; }

#else
#error "Unsupported platform"
#endif
