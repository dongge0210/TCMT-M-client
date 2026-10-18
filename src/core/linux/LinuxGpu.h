// LinuxGpu.h — multi-vendor GPU collection for Linux.
//
// NVIDIA is queried through libnvidia-ml.so.1 loaded with dlopen(), so the
// build has no compile-time NVML dependency. AMD and Intel data comes from
// /sys/class/drm/card*/device and its hwmon child.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tcmt::linux {

struct GpuSnapshot {
    std::string name;
    std::string brand;          // NVIDIA / AMD / Intel / Unknown
    std::string deviceId;       // "card0" or "nvml:0"

    uint64_t memoryBytes = 0;   // VRAM total, 0 when unknown
    uint64_t memoryUsedBytes = 0;
    double memoryPercent = 0.0;

    double coreClockMHz = 0.0;
    double usagePercent = 0.0;
    double temperatureC = 0.0;
    bool temperatureValid = false;
    double powerMilliwatts = 0.0;
    bool powerValid = false;

    bool isVirtual = false;
    bool isIntegrated = false;
    bool isUnified = false;
};

// Enumerates every GPU it can identify. Discrete cards come first, then
// integrated GPUs, then virtual ones; within a group larger VRAM wins. The
// caller treats element 0 as the primary GPU.
std::vector<GpuSnapshot> CollectGpus();

}  // namespace tcmt::linux
