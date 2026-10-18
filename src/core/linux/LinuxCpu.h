// LinuxCpu.h — /proc/stat, cpufreq and hwmon CPU temperature sampling.
//
// SampleCpu() is called by the CPU collector thread. GetLatestCpu() returns
// the last sample without touching the kernel, so the UI snapshot path never
// blocks on /proc or sysfs.
#pragma once

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
