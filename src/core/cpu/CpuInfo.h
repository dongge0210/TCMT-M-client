#pragma once
#include <string>
#include <vector>
#include <cstdint>

// Platform macro detection (if not defined in CMake)
#if !defined(TCMT_WINDOWS) && !defined(TCMT_MACOS) && !defined(TCMT_LINUX)
    #if defined(_WIN32) || defined(_WIN64)
        #define TCMT_WINDOWS
    #elif defined(__APPLE__) && defined(__MACH__)
        #define TCMT_MACOS
    #elif defined(__linux__)
        #define TCMT_LINUX
    #endif
#endif

class CpuInfo {
public:
    CpuInfo();
    ~CpuInfo();

    double GetUsage();
    std::string GetName();
    int GetTotalCores() const;
    int GetSmallCores() const;
    int GetLargeCores() const;
    double GetLargeCoreSpeed() const;    // Performance core frequency
    double GetSmallCoreSpeed() const;    // Efficiency core frequency
    uint32_t GetCurrentSpeed() const;    // Kept for compatibility
    bool IsHyperThreadingEnabled() const;
    bool IsVirtualizationEnabled() const;

    // Last CPU usage sample interval (ms)
    double GetLastSampleIntervalMs() const { return lastSampleIntervalMs; }

private:
    void DetectCores();
    void InitializeCounter();
    void CleanupCounter();
    void UpdateCoreSpeeds();
    std::string GetNameFromRegistry();
    double updateUsage();

    // Basic info
    std::string cpuName;
    int totalCores;
    int smallCores;
    int largeCores;
    double cpuUsage;

    // Frequency info
    double largeCoreSpeed;
    double smallCoreSpeed;
    double lastSampleIntervalMs;

#ifdef TCMT_WINDOWS
    void* queryHandle;       // PDH_HQUERY
    void* counterHandle;     // PDH_HCOUNTER (CPU usage)
    void* pCoreFreqHandle;   // PDH_HCOUNTER (P-core % perf)
    void* eCoreFreqHandle;   // PDH_HCOUNTER (E-core % perf)
    double pCoreBaseFreq;    // nominal MHz (registry)
    double eCoreBaseFreq;    // nominal MHz (registry)
    bool counterInitialized;
    uint32_t lastUpdateTime;
    uint32_t lastSampleTick;
    uint32_t prevSampleTick;
#endif

#ifdef TCMT_MACOS
    uint64_t prevTotalTicks;
    uint64_t prevIdleTicks;
    uint64_t prevSampleTimeMs;
    // P-core / E-core grouped frequencies
    std::vector<double> pCoreSpeeds;
    std::vector<double> eCoreSpeeds;
#endif

#ifdef TCMT_LINUX
    uint64_t prevTotalTicks;
    uint64_t prevIdleTicks;
    uint64_t prevSampleTimeMs;
    double largeCoreSpeedAvg;
    double smallCoreSpeedAvg;
#endif
};

#ifdef TCMT_LINUX

#include <string>
#include <vector>

namespace tcmt::linux {

struct CpuSnapshot {
    double totalUsage = 0.0;   // 0-100
    double intervalMs = 0.0;   // time since the previous sample
    int logicalCores = 0;

    bool hasPackageTemp = false;
    double packageTempC = 0.0;

    // One entry per logical CPU (index == cpuN). A zero entry means the
    // kernel does not expose that reading, not that it is really zero.
    std::vector<double> coreUsage;
    std::vector<double> coreFreqMHz;
    std::vector<double> coreTempC;
};

// Reads /proc/stat, cpufreq/scaling_cur_freq and hwmon. Updates the delta
// state used for usage percentages, then returns the current sample.
CpuSnapshot SampleCpu();

// Copy of the last successful SampleCpu() result.
CpuSnapshot GetLatestCpu();

int ReadLogicalCpuCount();
int ReadPhysicalCoreCount();
std::string ReadCpuModel();

}  // namespace tcmt::linux

#endif  // TCMT_LINUX
