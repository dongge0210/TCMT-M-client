#include "CpuInfo.h"
#include "../Utils/Logger.h"

#ifdef TCMT_WINDOWS
// ======================== Windows Implementation ========================
// NOTE: winsock2.h must be included BEFORE windows.h
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

    // P-core frequency: always instance "0,0" (core 0 is always P-core on Intel)
    status = PdhAddEnglishCounter(*(PDH_HQUERY*)qh,
        L"\\Processor Information(0,0)\\Processor Frequency", 0,
        (PDH_HCOUNTER*)pch);
    if (status != ERROR_SUCCESS) {
        Logger::Warn("Cannot add P-core frequency counter (err=" + std::to_string(status) + ")");
    }

    // E-core frequency: use first E-core logical processor index
    if (smallCores > 0) {
        int htRatio = (totalCores > (largeCores + smallCores)) ? 2 : 1;
        int firstECoreIdx = largeCores * htRatio;
        wchar_t eCorePath[128];
        swprintf_s(eCorePath, 128, L"\\Processor Information(0,%d)\\Processor Frequency", firstECoreIdx);
        status = PdhAddEnglishCounter(*(PDH_HQUERY*)qh, eCorePath, 0, (PDH_HCOUNTER*)ech);
        if (status != ERROR_SUCCESS) {
            Logger::Warn("Cannot add E-core frequency counter (err=" + std::to_string(status) + ")");
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

static double ReadPdhFreqValue(void* handle) {
    if (!handle) return 0.0;
    PDH_FMT_COUNTERVALUE v;
    PDH_STATUS s = PdhGetFormattedCounterValue(
        (PDH_HCOUNTER)handle, PDH_FMT_DOUBLE, NULL, &v);
    if (s == ERROR_SUCCESS &&
        (v.CStatus == PDH_CSTATUS_VALID_DATA || v.CStatus == PDH_CSTATUS_NEW_DATA)) {
        return v.doubleValue; // MHz
    }
    return 0.0;
}

double CpuInfo::GetLargeCoreSpeed() const {
    // Try PDH per-core counter first (real-time frequency)
    double freq = ReadPdhFreqValue(pCoreFreqHandle);
    if (freq > 0.0) return freq;

    // Fallback: registry base frequency
    DWORD speed = 0;
    DWORD size = sizeof(speed);
    if (RegGetValueW(HKEY_LOCAL_MACHINE,
        L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        L"~MHz", RRF_RT_DWORD, nullptr, &speed, &size) == ERROR_SUCCESS) {
        return static_cast<double>(speed);
    }
    return 0.0;
}

double CpuInfo::GetSmallCoreSpeed() const {
    // Try PDH per-core counter first (real-time E-core frequency)
    double freq = ReadPdhFreqValue(eCoreFreqHandle);
    if (freq > 0.0) return freq;

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
        largeCores = std::max(1, totalCores / 2);
        return;
    }

    std::vector<BYTE> buffer(bufferSize);
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore,
            reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buffer.data()),
            &bufferSize)) {
        largeCores = std::max(1, totalCores / 2);
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
            largeCoreSpeed = freq64 / 1000000; // convert to MHz
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

#else
#error "Unsupported platform"
#endif
