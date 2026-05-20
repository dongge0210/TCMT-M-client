#pragma once
#include <cstdint>
#include <atomic>
#include <string>

// PowerMonitor — direct Apple Silicon power/frequency sampling via IOReport + IOKit
// No sudo required. Falls back to powermetrics subprocess on failure.
class PowerMonitor {
public:
    PowerMonitor();
    ~PowerMonitor();

    // Start monitoring. Returns true if direct IOReport mode succeeded.
    // On failure, caller should start the legacy powermetrics thread.
    bool Start();
    void Stop();
    bool IsRunning() const { return running_.load(); }

    // True if using direct IOReport (no sudo), false if using powermetrics fallback
    bool IsDirectMode() const { return directMode_.load(); }

    // Latest sampled values (updated every ~1s in direct mode)
    double GetPCoreFreq() const  { return pCoreFreq_.load(); }
    double GetECoreFreq() const  { return eCoreFreq_.load(); }
    double GetGpuFreq() const    { return gpuFreq_.load(); }
    double GetCpuPower() const   { return cpuPower_.load(); }
    double GetGpuPower() const   { return gpuPower_.load(); }
    double GetAnePower() const   { return anePower_.load(); }

private:
    void SampleLoop();  // Runs on background thread in direct mode
    void ParsePowerDelta(void* deltaDict);  // Parse IOReport delta for power
    int64_t ExtractChannelValue(void* channel);  // Get value from IOReport channel
    double EnergyToPower(void* channel, int64_t energyDelta);  // Convert energy delta to Watts

    std::atomic<bool> running_{false};
    std::atomic<bool> directMode_{false};

    std::atomic<double> pCoreFreq_{0.0};
    std::atomic<double> eCoreFreq_{0.0};
    std::atomic<double> gpuFreq_{0.0};
    std::atomic<double> cpuPower_{0.0};
    std::atomic<double> gpuPower_{0.0};
    std::atomic<double> anePower_{0.0};

    // Opaque handles (defined in .cpp)
    void* subs_{nullptr};    // IOReportSubscriptionRef
    void* chan_{nullptr};    // CFMutableDictionaryRef
    void* thread_{nullptr};  // std::thread*
};
