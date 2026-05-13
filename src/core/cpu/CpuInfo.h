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
    void* counterHandle;     // PDH_HCOUNTER
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
};
