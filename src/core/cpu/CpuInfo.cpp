#include "CpuInfo.h"
#include "../Utils/Logger.h"

#ifdef TCMT_WINDOWS
// ======================== Windows Implementation ========================
// NOTE: winsock2.h must be included BEFORE windows.h
#define NOMINMAX
#include <winsock2.h>
#include <windows.h>
#include <intrin.h>
#include <pdh.h>

#pragma comment(lib, "pdh.lib")

#ifndef PDH_CSTATUS_VALID_DATA
#define PDH_CSTATUS_VALID_DATA 0x00000000L
#endif
#ifndef PDH_CSTATUS_NEW_DATA
#define PDH_CSTATUS_NEW_DATA 0x00000001L
#endif

CpuInfo::CpuInfo()
    : totalCores(0), largeCores(0), smallCores(0), cpuUsage(0.0),
      pCoreFreqHandle(nullptr), eCoreFreqHandle(nullptr),
      pCoreBaseFreq(0.0), eCoreBaseFreq(0.0),
      counterInitialized(false), lastUpdateTime(0),
      lastSampleTick(0), prevSampleTick(0), lastSampleIntervalMs(0.0) {
    try {
        DetectCores();
        cpuName = GetNameFromRegistry();
        InitializeCounter();
        UpdateCoreSpeeds();
    }
    catch (const std::exception& e) {
        Logger::Error("CPUInfoInitializeFailed: " + std::string(e.what()));
    }
}

CpuInfo::~CpuInfo() { CleanupCounter(); }

void CpuInfo::InitializeCounter() {
    auto qh = reinterpret_cast<void**>(&queryHandle);
    auto ch = reinterpret_cast<void**>(&counterHandle);
    auto pch = reinterpret_cast<void**>(&pCoreFreqHandle);
    auto ech = reinterpret_cast<void**>(&eCoreFreqHandle);
    *qh = nullptr; *ch = nullptr; *pch = nullptr; *ech = nullptr;

    PDH_STATUS status = PdhOpenQuery(NULL, 0, (PDH_HQUERY*)qh);
    if (status != ERROR_SUCCESS) {
        Logger::Error("Cannot open performance counter query");
        return;
    }

    // CPU usage counter
    status = PdhAddEnglishCounter(*(PDH_HQUERY*)qh,
        L"\\Processor(_Total)\\% Processor Time", 0,
        (PDH_HCOUNTER*)ch);
    if (status != ERROR_SUCCESS) {
        Logger::Error("Cannot add CPU usage counter");
        PdhCloseQuery(*(PDH_HQUERY*)qh);
        *qh = nullptr;
        return;
    }

    // P-core: % Processor Performance (relative to nominal)
    status = PdhAddEnglishCounter(*(PDH_HQUERY*)qh,
        L"\\Processor Information(0,0)\\% Processor Performance", 0,
        (PDH_HCOUNTER*)pch);
    if (status != ERROR_SUCCESS) {
        Logger::Warn("Cannot add P-core % performance counter (err=" + std::to_string(status) + ")");
    }

    // E-core: % Processor Performance
    if (smallCores > 0) {
        int htRatio = (totalCores > (largeCores + smallCores)) ? 2 : 1;
        int firstECoreIdx = largeCores * htRatio;
        wchar_t eCorePath[128];
        swprintf_s(eCorePath, 128, L"\\Processor Information(0,%d)\\%% Processor Performance", firstECoreIdx);
        status = PdhAddEnglishCounter(*(PDH_HQUERY*)qh, eCorePath, 0, (PDH_HCOUNTER*)ech);
        if (status != ERROR_SUCCESS) {
            Logger::Warn("Cannot add E-core % performance counter (err=" + std::to_string(status) + ")");
        }
    }
// Read nominal base frequencies from registry (MHz)
    // P-core (processor 0)
    {
        DWORD speed = 0, size = sizeof(speed);
        if (RegGetValueW(HKEY_LOCAL_MACHINE,
            L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
            L"~MHz", RRF_RT_DWORD, nullptr, &speed, &size) == ERROR_SUCCESS) {
            pCoreBaseFreq = static_cast<double>(speed);
        }
    }
    // E-core (first E-core logical processor)
    if (smallCores > 0) {
        int htRatio = (totalCores > (largeCores + smallCores)) ? 2 : 1;
        int firstECoreIdx = largeCores * htRatio;
        wchar_t keyPath[128];
        swprintf_s(keyPath, 128, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\%d", firstECoreIdx);
        DWORD speed = 0, size = sizeof(speed);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, keyPath,
            L"~MHz", RRF_RT_DWORD, nullptr, &speed, &size) == ERROR_SUCCESS) {
            eCoreBaseFreq = static_cast<double>(speed);
        } else {
            eCoreBaseFreq = pCoreBaseFreq; // fallback
        }
    }

    status = PdhCollectQueryData(*(PDH_HQUERY*)qh);
    if (status != ERROR_SUCCESS) {
        Logger::Error("Cannot collect performance counter data");
        PdhCloseQuery(*(PDH_HQUERY*)qh);
        *qh = nullptr; *ch = nullptr; *pch = nullptr; *ech = nullptr;
        return;
    }

    counterInitialized = true;
}

static double ReadPdhPctValue(void* handle) {
    if (!handle) return 0.0;
    PDH_FMT_COUNTERVALUE v;
    PDH_STATUS s = PdhGetFormattedCounterValue(
        (PDH_HCOUNTER)handle, PDH_FMT_DOUBLE, NULL, &v);
    if (s == ERROR_SUCCESS &&
        (v.CStatus == PDH_CSTATUS_VALID_DATA || v.CStatus == PDH_CSTATUS_NEW_DATA)) {
        return v.doubleValue; // percentage (0-100+, >100 = turbo)
    }
    return 0.0;
}

double CpuInfo::GetLargeCoreSpeed() const {
    // % Processor Performance × base frequency = real-time MHz
    double pct = ReadPdhPctValue(pCoreFreqHandle);
    if (pct > 0.0 && pCoreBaseFreq > 0.0)
        return pCoreBaseFreq * (pct / 100.0);

    // Fallback: registry base frequency
    return pCoreBaseFreq;
}

double CpuInfo::GetSmallCoreSpeed() const {
    // % Processor Performance × base frequency = real-time MHz
    double pct = ReadPdhPctValue(eCoreFreqHandle);
    if (pct > 0.0 && eCoreBaseFreq > 0.0)
        return eCoreBaseFreq * (pct / 100.0);

    // Fallback: P-core frequency as approximation
    return GetLargeCoreSpeed();
}

void CpuInfo::CleanupCounter() {
    if (counterInitialized && queryHandle) {
        PdhCloseQuery((PDH_HQUERY)queryHandle);
        counterInitialized = false;
    }
}

void CpuInfo::DetectCores() {
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);
    totalCores = (int)sysInfo.dwNumberOfProcessors;
    largeCores = 0;
    smallCores = 0;

    // Use GetLogicalProcessorInformationEx for correct P/E core detection
    // on Intel hybrid (12th gen+) where EfficiencyClass distinguishes cores
    DWORD bufferSize = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &bufferSize);
    if (bufferSize == 0) {
        // Fallback: assume all performance cores
        largeCores = (std::max)(1, totalCores / 2);
        return;
    }

    std::vector<BYTE> buffer(bufferSize);
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()),
            &bufferSize)) {
        largeCores = (std::max)(1, totalCores / 2);
        return;
    }

    BYTE* ptr = buffer.data();
    DWORD offset = 0;
    while (offset < bufferSize) {
        auto* info = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(ptr + offset);
        if (info->Size == 0) break;
        if (info->Relationship == RelationProcessorCore) {
            // EfficiencyClass: 0=unknown, 1=E-core, 2=P-core (Win10 21H2+)
            if (info->Processor.EfficiencyClass == 1)
                smallCores++;
            else
                largeCores++;
        }
        offset += info->Size;
    }

    // If no distinction found (pre-Win10 or non-hybrid), treat all as P-core
    if (largeCores == 0) {
        largeCores = totalCores;
        smallCores = 0;
    }
}

void CpuInfo::UpdateCoreSpeeds() {
    // Windows-specific - frequency from registry
}

double CpuInfo::updateUsage() {
    if (!counterInitialized) return cpuUsage;

    static DWORD lastCollectTime = 0;
    DWORD currentTime = GetTickCount();
    DWORD delta = currentTime - lastCollectTime;
    if (delta < 1000) return cpuUsage;
    lastCollectTime = currentTime;

    PDH_STATUS status = PdhCollectQueryData((PDH_HQUERY)queryHandle);
    if (status != ERROR_SUCCESS) return cpuUsage;

    PDH_FMT_COUNTERVALUE counterValue;
    status = PdhGetFormattedCounterValue((PDH_HCOUNTER)counterHandle,
        PDH_FMT_DOUBLE, NULL, &counterValue);
    if (status != ERROR_SUCCESS) return cpuUsage;

    prevSampleTick = lastSampleTick;
    lastSampleTick = currentTime;
    if (prevSampleTick != 0) {
        lastSampleIntervalMs = static_cast<double>(lastSampleTick - prevSampleTick);
    }

    if (counterValue.CStatus == PDH_CSTATUS_VALID_DATA || counterValue.CStatus == PDH_CSTATUS_NEW_DATA) {
        double newUsage = counterValue.doubleValue;
        if (newUsage < 0.0) newUsage = 0.0;
        if (newUsage > 100.0) newUsage = 100.0;
        if (cpuUsage > 0.0) cpuUsage = (cpuUsage * 0.8) + (newUsage * 0.2);
        else cpuUsage = newUsage;
    }
    return cpuUsage;
}

double CpuInfo::GetUsage() { return updateUsage(); }

std::string CpuInfo::GetNameFromRegistry() {
    HKEY hKey;
    char buffer[128];
    DWORD size = sizeof(buffer);
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        if (RegQueryValueExA(hKey, "ProcessorNameString", nullptr, nullptr, (LPBYTE)buffer, &size) == ERROR_SUCCESS) {
            RegCloseKey(hKey);
            return std::string(buffer, size - 1);
        }
        RegCloseKey(hKey);
    }
    return "Unknown CPU";
}

int CpuInfo::GetTotalCores() const { return totalCores; }
int CpuInfo::GetSmallCores() const { return smallCores; }
int CpuInfo::GetLargeCores() const { return largeCores; }

std::string CpuInfo::GetName() { return cpuName; }

bool CpuInfo::IsHyperThreadingEnabled() const {
    return (totalCores > (largeCores + smallCores));
}

bool CpuInfo::IsVirtualizationEnabled() const {
    int cpuInfo[4];
    __cpuid(cpuInfo, 1);
    bool hasVMX = (cpuInfo[2] & (1 << 5)) != 0;
    if (!hasVMX) return false;

    bool isVMXEnabled = false;
    __try {
        unsigned __int64 msrValue = __readmsr(0x3A);
        isVMXEnabled = (msrValue & 0x5) != 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        isVMXEnabled = false;
    }
    return isVMXEnabled;
}

#elif defined(TCMT_MACOS)
// ======================== macOS Implementation ========================
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/mach_host.h>
#include <unistd.h>
// Note: cpuid.h is x86-only, no ARM equivalent needed

static uint64_t GetTickCountMs() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

CpuInfo::CpuInfo()
    : totalCores(0), smallCores(0), largeCores(0), cpuUsage(0.0),
      largeCoreSpeed(0.0), smallCoreSpeed(0.0), lastSampleIntervalMs(0.0),
      prevTotalTicks(0), prevIdleTicks(0), prevSampleTimeMs(0) {
    try {
        DetectCores();
        cpuName = GetNameFromRegistry();
        InitializeCounter();
        UpdateCoreSpeeds();
    }
    catch (const std::exception& e) {
        Logger::Error("CPUInfoInitializeFailed: " + std::string(e.what()));
    }
}

CpuInfo::~CpuInfo() { CleanupCounter(); }

void CpuInfo::InitializeCounter() {
    // host_statistics is the non-deprecated API that works on both
    // Intel and Apple Silicon (host_processor_info is deprecated).
    host_cpu_load_info_data_t cpuLoad;
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;

    kern_return_t err = host_statistics(
        mach_host_self(), HOST_CPU_LOAD_INFO,
        (host_info_t)&cpuLoad, &count);

    if (err != KERN_SUCCESS) return;

    uint64_t totalTicks = cpuLoad.cpu_ticks[CPU_STATE_USER]
                        + cpuLoad.cpu_ticks[CPU_STATE_SYSTEM]
                        + cpuLoad.cpu_ticks[CPU_STATE_NICE]
                        + cpuLoad.cpu_ticks[CPU_STATE_IDLE];
    uint64_t idleTicks  = cpuLoad.cpu_ticks[CPU_STATE_IDLE];

    prevTotalTicks = totalTicks;
    prevIdleTicks  = idleTicks;
    prevSampleTimeMs = GetTickCountMs();
}

void CpuInfo::CleanupCounter() {
    // Nothing to clean up on macOS
}

void CpuInfo::DetectCores() {
    // Total logical cores
    int ncpu = 0;
    size_t len = sizeof(ncpu);
    if (sysctlbyname("hw.logicalcpu", &ncpu, &len, nullptr, 0) == 0) {
        totalCores = ncpu;
    }

    // Apple Silicon: use perflevel sysctl (macOS 12+)
    // perflevel0 = performance cores (P), perflevel1 = efficiency cores (E)
    int pCores = 0, eCores = 0;
    len = sizeof(pCores);
    bool hasPerfLevel = (sysctlbyname("hw.perflevel0.logicalcpu", &pCores, &len, nullptr, 0) == 0);
    len = sizeof(eCores);
    hasPerfLevel = hasPerfLevel && (sysctlbyname("hw.perflevel1.logicalcpu", &eCores, &len, nullptr, 0) == 0);

    if (hasPerfLevel && (pCores + eCores) > 0) {
        largeCores = pCores;
        smallCores = eCores;
    } else if (sysctlbyname("hw.performancecores", &pCores, &len, nullptr, 0) == 0) {
        // Fallback: performancecores available (some macOS versions)
        largeCores = pCores;
        int physical = 0;
        len = sizeof(physical);
        sysctlbyname("hw.physicalcpu", &physical, &len, nullptr, 0);
        smallCores = physical - pCores;
    } else {
        // Fallback: assume all physical cores are performance (Intel Macs)
        int phys = 0;
        len = sizeof(phys);
        if (sysctlbyname("hw.physicalcpu", &phys, &len, nullptr, 0) == 0) {
            largeCores = phys;
        }
    }

    // Hyper-threading: if logical > physical
    if (totalCores > (largeCores + smallCores)) {
        // HT enabled (Intel)
    } else if (largeCores + smallCores == totalCores) {
        // No HT (Apple Silicon)
    }

    Logger::Debug("CpuInfo: total=" + std::to_string(totalCores)
               + " P=" + std::to_string(largeCores)
               + " E=" + std::to_string(smallCores));
}

void CpuInfo::UpdateCoreSpeeds() {
    // CPU frequency via sysctl (in MHz)
    uint32_t freq = 0;
    size_t len = sizeof(freq);
    if (sysctlbyname("hw.cpufrequency", &freq, &len, nullptr, 0) == 0) {
        largeCoreSpeed = freq;
    } else {
        // Fallback: try cpu_freq
        uint64_t freq64 = 0;
        len = sizeof(freq64);
        if (sysctlbyname("hw.cpufrequency_max", &freq64, &len, nullptr, 0) == 0) {
            // Divide in double: hw.cpufrequency_max is in Hz and integer
            // division would truncate before the assignment.
            largeCoreSpeed = static_cast<double>(freq64) / 1000000.0; // convert to MHz
        }
    }
    smallCoreSpeed = largeCoreSpeed * 0.6; // E-cores typically lower freq
}

std::string CpuInfo::GetNameFromRegistry() {
    char model[256] = {0};
    size_t len = sizeof(model);
    if (sysctlbyname("machdep.cpu.brand_string", model, &len, nullptr, 0) == 0) {
        return std::string(model);
    }
    // Apple Silicon: hw.machine + hw.model
    char machine[128] = {0};
    len = sizeof(machine);
    if (sysctlbyname("hw.machine", machine, &len, nullptr, 0) == 0) {
        return std::string(machine);
    }
    return "Unknown CPU";
}

double CpuInfo::updateUsage() {
    uint64_t nowMs = GetTickCountMs();
    uint64_t elapsedMs = nowMs - prevSampleTimeMs;
    // Ensure at least 100ms between samples for accuracy
    if (elapsedMs < 100) return cpuUsage;

    host_cpu_load_info_data_t cpuLoad;
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;

    kern_return_t err = host_statistics(
        mach_host_self(), HOST_CPU_LOAD_INFO,
        (host_info_t)&cpuLoad, &count);

    if (err != KERN_SUCCESS) return cpuUsage;

    uint64_t totalTicks = cpuLoad.cpu_ticks[CPU_STATE_USER]
                        + cpuLoad.cpu_ticks[CPU_STATE_SYSTEM]
                        + cpuLoad.cpu_ticks[CPU_STATE_NICE]
                        + cpuLoad.cpu_ticks[CPU_STATE_IDLE];
    uint64_t idleTicks  = cpuLoad.cpu_ticks[CPU_STATE_IDLE];

    uint64_t deltaTotal = totalTicks - prevTotalTicks;
    uint64_t deltaIdle  = idleTicks  - prevIdleTicks;

    if (deltaTotal > 0) {
        double newUsage = 100.0 * (1.0 - (double)deltaIdle / (double)deltaTotal);
        if (newUsage < 0.0) newUsage = 0.0;
        if (newUsage > 100.0) newUsage = 100.0;
        // EMA smoothing: same as Windows
        if (cpuUsage > 0.0) cpuUsage = (cpuUsage * 0.8) + (newUsage * 0.2);
        else cpuUsage = newUsage;
    }

    prevTotalTicks = totalTicks;
    prevIdleTicks  = idleTicks;
    prevSampleTimeMs = nowMs;
    lastSampleIntervalMs = (double)elapsedMs;

    return cpuUsage;
}

double CpuInfo::GetUsage() { return updateUsage(); }

int CpuInfo::GetTotalCores() const { return totalCores; }
int CpuInfo::GetSmallCores() const { return smallCores; }
int CpuInfo::GetLargeCores() const { return largeCores; }

double CpuInfo::GetLargeCoreSpeed() const {
    // Update on demand
    uint64_t freq = 0;
    size_t len = sizeof(freq);
    if (sysctlbyname("hw.cpufrequency", &freq, &len, nullptr, 0) == 0) {
        return freq; // already in MHz
    }
    return largeCoreSpeed;
}

double CpuInfo::GetSmallCoreSpeed() const {
    // E-core freq: hw.cpufrequency_min or approximate
    uint64_t freq = 0;
    size_t len = sizeof(freq);
    if (sysctlbyname("hw.cpufrequency_min", &freq, &len, nullptr, 0) == 0) {
        return freq;
    }
    return largeCoreSpeed * 0.6;
}

uint32_t CpuInfo::GetCurrentSpeed() const {
    return (uint32_t)GetLargeCoreSpeed();
}

std::string CpuInfo::GetName() { return cpuName; }

bool CpuInfo::IsHyperThreadingEnabled() const {
    // Apple Silicon has no HT; Intel Macs may
    int physical = 0, logical = 0;
    size_t len = sizeof(physical);
    sysctlbyname("hw.physicalcpu", &physical, &len, nullptr, 0);
    len = sizeof(logical);
    sysctlbyname("hw.logicalcpu", &logical, &len, nullptr, 0);
    return logical > physical;
}

bool CpuInfo::IsVirtualizationEnabled() const {
    // Apple Silicon (ARM64): no x86 cpuid available.
    // Check if running under a hypervisor via sysctl.
    int isHypervisor = 0;
    size_t len = sizeof(isHypervisor);
    if (sysctlbyname("hw.hypervisor", &isHypervisor, &len, nullptr, 0) == 0) {
        return isHypervisor != 0;
    }
    return false;
}

#elif defined(TCMT_LINUX)
// ======================== Linux Implementation ========================
#include "../Utils/Sysfs.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <unistd.h>

namespace tcmt::linux {
namespace {

struct CpuTimes {
    uint64_t total = 0;
    uint64_t idle = 0;
};

struct SampleState {
    std::mutex mutex;
    bool primed = false;
    CpuTimes prevAggregate;
    std::vector<CpuTimes> prevCores;
    int64_t prevMs = 0;
    CpuSnapshot latest;
};

SampleState& State() {
    static SampleState state;
    return state;
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// /proc/stat cpu line: user nice system idle iowait irq softirq steal
// guest guest_nice. guest/guest_nice are already included in user/nice, and
// total only needs the first eight fields.
bool ParseCpuLine(const std::string& line, CpuTimes& out) {
    std::istringstream ss(line);
    std::string tag;
    if (!(ss >> tag)) return false;
    uint64_t v[10] = {};
    int n = 0;
    while (n < 10 && (ss >> v[n])) ++n;
    if (n < 4) return false;

    // iowait is idle time from the scheduler's point of view; tools such as
    // top/htop include it in idle.
    out.total = 0;
    for (int i = 0; i < n && i < 8; ++i) out.total += v[i];
    out.idle = v[3] + (n > 4 ? v[4] : 0);
    return out.total > 0;
}

double UsagePercent(const CpuTimes& now, const CpuTimes& prev) {
    if (now.total <= prev.total) return 0.0;
    const uint64_t totalDelta = now.total - prev.total;
    const uint64_t idleDelta = now.idle >= prev.idle ? now.idle - prev.idle : 0;
    if (totalDelta == 0) return 0.0;
    double pct = 100.0 * (1.0 - static_cast<double>(idleDelta) /
                                   static_cast<double>(totalDelta));
    if (pct < 0.0) pct = 0.0;
    if (pct > 100.0) pct = 100.0;
    return pct;
}

bool IsCpuHwmon(const std::string& name) {
    static const char* kNames[] = {
        "coretemp", "k10temp", "zenpower", "fam15h_power",
        "cpu_thermal", "soc_thermal", "cpu-thermal", "soc-thermal",
    };
    for (const char* n : kNames) {
        if (name == n) return true;
    }
    return false;
}

bool PlausibleTemp(double c) { return c > 0.0 && c <= 150.0; }

// Map from (physical package, core id) to the first logical CPU in that core,
// plus the per-package core order used by coretemp ("Core 0", "Core 1", ...).
struct CoreMap {
    std::map<long long, int> byPackageCore;
    std::map<uint64_t, std::vector<int>> orderedPackageCores;
};

CoreMap BuildCoreToCpuMap(int logicalCores) {
    CoreMap map;
    for (int i = 0; i < logicalCores; ++i) {
        const std::string base =
            "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/topology/";
        uint64_t pkg = 0, core = 0;
        if (!ReadUInt64(base + "physical_package_id", pkg)) continue;
        if (!ReadUInt64(base + "core_id", core)) continue;
        const long long key = static_cast<long long>(pkg) * 1000000LL +
                              static_cast<long long>(core);
        if (map.byPackageCore.find(key) != map.byPackageCore.end()) continue;
        map.byPackageCore[key] = i;
        // coretemp numbers cores in ascending logical-CPU order within a
        // package, skipping siblings.
        map.orderedPackageCores[pkg].push_back(i);
    }
    return map;
}

void ReadCpuTemperatures(CpuSnapshot& snap) {
    const int n = snap.logicalCores;
    snap.coreTempC.assign(static_cast<size_t>(n), 0.0);

    const CoreMap coreToCpu = BuildCoreToCpuMap(n);
    bool havePackage = false;
    double packageTemp = 0.0;

    for (const std::string& hw : ListDir("/sys/class/hwmon", "hwmon")) {
        const std::string base = "/sys/class/hwmon/" + hw;
        const std::string name = ReadLine(base + "/name");
        if (!IsCpuHwmon(name)) continue;

        std::map<int, double> inputs;
        std::map<int, std::string> labels;
        for (const std::string& file : ListDir(base)) {
            if (file.compare(0, 4, "temp") != 0) continue;
            const size_t underscore = file.find('_', 4);
            if (underscore == std::string::npos) continue;
            int index = 0;
            try {
                index = std::stoi(file.substr(4, underscore - 4));
            } catch (...) {
                continue;
            }
            const std::string suffix = file.substr(underscore);
            if (suffix == "_input") {
                int64_t milli = 0;
                if (ReadInt64(base + "/" + file, milli)) {
                    const double celsius = static_cast<double>(milli) / 1000.0;
                    if (PlausibleTemp(celsius)) inputs[index] = celsius;
                }
            } else if (suffix == "_label") {
                labels[index] = ReadLine(base + "/" + file);
            }
        }

        // coretemp names its package sensor "Package id N"; the Core N labels
        // in the same hwmon device belong to that package.
        int packageId = 0;
        for (const auto& [index, label] : labels) {
            if (label.compare(0, 11, "Package id ") == 0) {
                try {
                    packageId = std::stoi(label.substr(11));
                } catch (...) {
                    packageId = 0;
                }
                break;
            }
        }

        for (const auto& [index, celsius] : inputs) {
            const auto lit = labels.find(index);
            const std::string label = lit == labels.end() ? std::string() : lit->second;

            if (label.compare(0, 11, "Package id ") == 0 ||
                label == "Tctl" || label == "Tdie" || label == "Tccd1" ||
                label == "Tccd2" || label == "Tccd3" || label == "Tccd4" ||
                (label.empty() && name != "coretemp")) {
                // "TccdN" is a CCD, not a core; only Tctl/Tdie drive the
                // package number. Multiple candidates: take the hottest.
                if (label.rfind("Tccd", 0) == 0 && label != "Tccd1") continue;
                if (!havePackage || celsius > packageTemp) packageTemp = celsius;
                havePackage = true;
                continue;
            }

            int cpuIndex = -1;
            if (name == "coretemp" && label.compare(0, 5, "Core ") == 0) {
                int coreId = -1;
                try {
                    coreId = std::stoi(label.substr(5));
                } catch (...) {
                    coreId = -1;
                }
                if (coreId >= 0) {
                    // coretemp numbers cores in the package's CPU-mask order,
                    // which is what orderedPackageCores represents.
                    const auto ordered =
                        coreToCpu.orderedPackageCores.find(static_cast<uint64_t>(packageId));
                    if (ordered != coreToCpu.orderedPackageCores.end() &&
                        coreId < static_cast<int>(ordered->second.size())) {
                        cpuIndex = ordered->second[static_cast<size_t>(coreId)];
                    } else {
                        auto it = coreToCpu.byPackageCore.find(
                            static_cast<long long>(packageId) * 1000000LL + coreId);
                        if (it == coreToCpu.byPackageCore.end()) {
                            it = coreToCpu.byPackageCore.find(coreId);  // single package
                        }
                        if (it != coreToCpu.byPackageCore.end()) cpuIndex = it->second;
                        else if (coreId < n) cpuIndex = coreId;
                    }
                }
            } else if (name == "zenpower" && label.size() >= 2 && label[0] == 'C' &&
                       std::isdigit(static_cast<unsigned char>(label[1]))) {
                try {
                    cpuIndex = std::stoi(label.substr(1));
                } catch (...) {
                    cpuIndex = -1;
                }
                if (cpuIndex >= n) cpuIndex = -1;
            }

            if (cpuIndex >= 0 && cpuIndex < n && snap.coreTempC[cpuIndex] <= 0.0) {
                snap.coreTempC[cpuIndex] = celsius;
            }
        }
    }

    // Fallback: ACPI thermal zones with a CPU-ish type.
    if (!havePackage) {
        for (const std::string& zone : ListDir("/sys/class/thermal", "thermal_zone")) {
            const std::string base = "/sys/class/thermal/" + zone;
            std::string type = ReadLine(base + "/type");
            std::transform(type.begin(), type.end(), type.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const bool cpuLike = type.find("x86_pkg_temp") != std::string::npos ||
                                 type.find("cpu") != std::string::npos ||
                                 type.find("soc") != std::string::npos ||
                                 type.find("package") != std::string::npos;
            if (!cpuLike) continue;
            int64_t milli = 0;
            if (!ReadInt64(base + "/temp", milli)) continue;
            const double celsius = static_cast<double>(milli) / 1000.0;
            if (!PlausibleTemp(celsius)) continue;
            if (!havePackage || celsius > packageTemp) packageTemp = celsius;
            havePackage = true;
        }
    }

    snap.hasPackageTemp = havePackage;
    snap.packageTempC = havePackage ? packageTemp : 0.0;
}

}  // namespace

CpuSnapshot SampleCpu() {
    CpuSnapshot snap;

    std::ifstream stat("/proc/stat");
    if (!stat.is_open()) {
        static std::atomic<bool> logged{false};
        LogOnce(logged, "LinuxCpu: /proc/stat unreadable; CPU usage unavailable");
        return GetLatestCpu();
    }

    CpuTimes aggregate;
    bool haveAggregate = false;
    std::vector<CpuTimes> cores;
    std::string line;
    while (std::getline(stat, line)) {
        if (line.compare(0, 3, "cpu") != 0) break;  // cpu lines come first
        if (line.size() > 4 && std::isdigit(static_cast<unsigned char>(line[3]))) {
            int index = 0;
            try {
                index = std::stoi(line.substr(3, line.find(' ') - 3));
            } catch (...) {
                continue;
            }
            if (index < 0) continue;
            CpuTimes times;
            if (!ParseCpuLine(line, times)) continue;
            if (static_cast<size_t>(index) >= cores.size()) {
                cores.resize(static_cast<size_t>(index) + 1);
            }
            cores[static_cast<size_t>(index)] = times;
        } else {
            haveAggregate = ParseCpuLine(line, aggregate);
        }
    }

    snap.logicalCores = static_cast<int>(cores.size());
    if (!haveAggregate) {
        for (const auto& times : cores) {
            aggregate.total += times.total;
            aggregate.idle += times.idle;
        }
    }

    const int64_t now = NowMs();
    const int n = snap.logicalCores;
    snap.coreUsage.assign(static_cast<size_t>(n), 0.0);
    snap.coreFreqMHz.assign(static_cast<size_t>(n), 0.0);

    {
        SampleState& state = State();
        std::lock_guard<std::mutex> lock(state.mutex);

        if (state.primed) {
            snap.totalUsage = UsagePercent(aggregate, state.prevAggregate);
            if (state.prevCores.size() == cores.size()) {
                for (int i = 0; i < n; ++i) {
                    snap.coreUsage[static_cast<size_t>(i)] =
                        UsagePercent(cores[static_cast<size_t>(i)],
                                     state.prevCores[static_cast<size_t>(i)]);
                }
            }
            snap.intervalMs = static_cast<double>(now - state.prevMs);
        }

        state.prevAggregate = aggregate;
        state.prevCores = cores;
        state.prevMs = now;
        state.primed = true;
    }

    for (int i = 0; i < n; ++i) {
        const std::string path = "/sys/devices/system/cpu/cpu" +
                                 std::to_string(i) +
                                 "/cpufreq/scaling_cur_freq";
        uint64_t khz = 0;
        if (ReadUInt64(path, khz) && khz > 0) {
            snap.coreFreqMHz[static_cast<size_t>(i)] =
                static_cast<double>(khz) / 1000.0;
        }
    }

    ReadCpuTemperatures(snap);

    {
        SampleState& state = State();
        std::lock_guard<std::mutex> lock(state.mutex);
        state.latest = snap;
    }
    return snap;
}

CpuSnapshot GetLatestCpu() {
    SampleState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.latest;
}

int ReadLogicalCpuCount() {
    std::ifstream cpuinfo("/proc/cpuinfo");
    int count = 0;
    if (cpuinfo.is_open()) {
        std::string line;
        while (std::getline(cpuinfo, line)) {
            if (line.compare(0, 9, "processor") == 0) ++count;
        }
    }
    if (count > 0) return count;
    const long online = ::sysconf(_SC_NPROCESSORS_ONLN);
    return online > 0 ? static_cast<int>(online) : 1;
}

int ReadPhysicalCoreCount() {
    const int logical = ReadLogicalCpuCount();
    std::map<long long, bool> seen;
    for (int i = 0; i < logical; ++i) {
        const std::string base =
            "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/topology/";
        uint64_t pkg = 0, core = 0;
        if (!ReadUInt64(base + "physical_package_id", pkg)) continue;
        if (!ReadUInt64(base + "core_id", core)) continue;
        seen[static_cast<long long>(pkg) * 1000000LL + static_cast<long long>(core)] = true;
    }
    if (!seen.empty()) return static_cast<int>(seen.size());

    // ARM/MIPS kernels may not expose topology. /proc/cpuinfo sometimes has a
    // "cpu cores" line; otherwise logical == physical is the least wrong answer.
    std::ifstream cpuinfo("/proc/cpuinfo");
    if (cpuinfo.is_open()) {
        std::string line;
        int cores = 0;
        while (std::getline(cpuinfo, line)) {
            const size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            if (Trim(line.substr(0, colon)) != "cpu cores") continue;
            try {
                cores = std::max(cores, std::stoi(Trim(line.substr(colon + 1))));
            } catch (...) {
            }
        }
        if (cores > 0) return cores;
    }
    return logical;
}

std::string ReadCpuModel() {
    std::ifstream cpuinfo("/proc/cpuinfo");
    if (cpuinfo.is_open()) {
        std::string line;
        while (std::getline(cpuinfo, line)) {
            const size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            const std::string key = Trim(line.substr(0, colon));
            if (key != "model name" && key != "Model" && key != "Hardware" &&
                key != "cpu model") {
                continue;
            }
            const std::string value = Trim(line.substr(colon + 1));
            if (!value.empty()) return value;
        }
    }

    // ARM boards often only report the model through the device tree.
    std::string dt;
    if (ReadFile("/sys/firmware/devicetree/base/model", dt)) {
        const size_t nul = dt.find('\0');
        if (nul != std::string::npos) dt.resize(nul);
        dt = Trim(dt);
        if (!dt.empty()) return dt;
    }
    return "Unknown CPU";
}

}  // namespace tcmt::linux
#include <sys/time.h>
#include <unistd.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <set>

static uint64_t GetTickCountMs() {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

CpuInfo::CpuInfo()
    : totalCores(0), smallCores(0), largeCores(0), cpuUsage(0.0),
      largeCoreSpeed(0.0), smallCoreSpeed(0.0), lastSampleIntervalMs(0.0),
      prevTotalTicks(0), prevIdleTicks(0), prevSampleTimeMs(0),
      largeCoreSpeedAvg(0.0), smallCoreSpeedAvg(0.0) {
    try {
        DetectCores();
        cpuName = GetNameFromRegistry();
        InitializeCounter();
        UpdateCoreSpeeds();
    }
    catch (const std::exception& e) {
        Logger::Error("CPUInfoInitializeFailed: " + std::string(e.what()));
    }
}

CpuInfo::~CpuInfo() { CleanupCounter(); }

void CpuInfo::InitializeCounter() {
    std::ifstream stat("/proc/stat");
    if (!stat.is_open()) {
        Logger::Error("Cannot open /proc/stat");
        return;
    }
    std::string line;
    std::getline(stat, line);
    stat.close();
    if (line.substr(0, 4) != "cpu ") return;

    std::istringstream ss(line);
    std::string cpu;
    ss >> cpu;
    uint64_t user, nice, system, idle, iowait, irq, softirq, steal;
    if (!(ss >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal))
        return;

    prevTotalTicks = user + nice + system + idle + iowait + irq + softirq + steal;
    // iowait is idle from the scheduler's point of view (same as top/htop).
    prevIdleTicks = idle + iowait;
    prevSampleTimeMs = GetTickCountMs();
}

void CpuInfo::CleanupCounter() {
    // Nothing to clean up on Linux
}

void CpuInfo::DetectCores() {
    totalCores = tcmt::linux::ReadLogicalCpuCount();
    if (totalCores <= 0) totalCores = 1;

    // Physical cores come from the topology sysfs, counting unique
    // (package, core id) pairs. /proc/cpuinfo "core id" repeats the same id
    // on every package, so a plain set would under-count multi-socket boxes.
    int physicalCores = tcmt::linux::ReadPhysicalCoreCount();
    if (physicalCores <= 0 || physicalCores > totalCores) physicalCores = totalCores;

    largeCores = physicalCores;
    smallCores = 0;

    Logger::Debug("CpuInfo: total=" + std::to_string(totalCores)
               + " physical=" + std::to_string(largeCores));
}

void CpuInfo::UpdateCoreSpeeds() {
    std::vector<double> frequencies;
    for (int i = 0; i < totalCores; i++) {
        std::string path = "/sys/devices/system/cpu/cpu"
                         + std::to_string(i)
                         + "/cpufreq/scaling_cur_freq";
        std::ifstream freqFile(path);
        if (freqFile.is_open()) {
            unsigned long freqKhz = 0;
            freqFile >> freqKhz;
            if (freqKhz > 0) {
                frequencies.push_back(static_cast<double>(freqKhz) / 1000.0);
            }
        }
    }

    if (!frequencies.empty()) {
        double sum = 0.0;
        for (double f : frequencies) sum += f;
        largeCoreSpeedAvg = sum / static_cast<double>(frequencies.size());
    } else {
        std::ifstream maxFreqFile("/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq");
        if (maxFreqFile.is_open()) {
            unsigned long freqKhz = 0;
            maxFreqFile >> freqKhz;
            largeCoreSpeedAvg = static_cast<double>(freqKhz) / 1000.0;
        }
    }
    smallCoreSpeedAvg = largeCoreSpeedAvg;
}

std::string CpuInfo::GetNameFromRegistry() {
    // Handles x86 ("model name"), ARM ("Model"/"Hardware") and the device
    // tree fallback used by most ARM boards.
    return tcmt::linux::ReadCpuModel();
}

double CpuInfo::updateUsage() {
    uint64_t nowMs = GetTickCountMs();
    uint64_t elapsedMs = nowMs - prevSampleTimeMs;
    if (elapsedMs < 100) return cpuUsage;

    std::ifstream stat("/proc/stat");
    if (!stat.is_open()) return cpuUsage;

    std::string line;
    std::getline(stat, line);
    stat.close();
    if (line.substr(0, 4) != "cpu ") return cpuUsage;

    std::istringstream ss(line);
    std::string cpu;
    ss >> cpu;
    uint64_t user, nice, system, idle, iowait, irq, softirq, steal;
    if (!(ss >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal))
        return cpuUsage;

    uint64_t totalTicks = user + nice + system + idle + iowait + irq + softirq + steal;
    uint64_t totalDelta = totalTicks - prevTotalTicks;
    uint64_t idleTicks = idle + iowait;
    uint64_t idleDelta = idleTicks >= prevIdleTicks ? idleTicks - prevIdleTicks : 0;

    if (totalDelta > 0) {
        double newUsage = 100.0 * (1.0 - static_cast<double>(idleDelta)
                                         / static_cast<double>(totalDelta));
        if (newUsage < 0.0) newUsage = 0.0;
        if (newUsage > 100.0) newUsage = 100.0;
        if (cpuUsage > 0.0) cpuUsage = (cpuUsage * 0.8) + (newUsage * 0.2);
        else cpuUsage = newUsage;
    }

    prevTotalTicks = totalTicks;
    prevIdleTicks = idleTicks;
    prevSampleTimeMs = nowMs;
    lastSampleIntervalMs = static_cast<double>(elapsedMs);

    return cpuUsage;
}

double CpuInfo::GetUsage() { return updateUsage(); }

int CpuInfo::GetTotalCores() const { return totalCores; }
int CpuInfo::GetSmallCores() const { return smallCores; }
int CpuInfo::GetLargeCores() const { return largeCores; }

double CpuInfo::GetLargeCoreSpeed() const {
    return largeCoreSpeedAvg;
}

double CpuInfo::GetSmallCoreSpeed() const {
    return smallCoreSpeedAvg;
}

uint32_t CpuInfo::GetCurrentSpeed() const {
    return static_cast<uint32_t>(largeCoreSpeedAvg);
}

std::string CpuInfo::GetName() { return cpuName; }

bool CpuInfo::IsHyperThreadingEnabled() const {
    return totalCores > (largeCores + smallCores);
}

bool CpuInfo::IsVirtualizationEnabled() const {
    std::ifstream cpuinfo("/proc/cpuinfo");
    if (!cpuinfo.is_open()) return false;
    std::string line;
    while (std::getline(cpuinfo, line)) {
        if (line.substr(0, 6) == "flags\t") {
            return line.find("vmx") != std::string::npos ||
                   line.find("svm") != std::string::npos;
        }
    }
    return false;
}

#else
#error "Unsupported platform"
#endif
