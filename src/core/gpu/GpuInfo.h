#pragma once
#include <vector>
#include <string>
#include <cstdint>

// Platform macro detection
#if !defined(TCMT_WINDOWS) && !defined(TCMT_MACOS) && !defined(TCMT_LINUX)
    #if defined(_WIN32) || defined(_WIN64)
        #define TCMT_WINDOWS
    #elif defined(__APPLE__) && defined(__MACH__)
        #define TCMT_MACOS
    #elif defined(__linux__)
        #define TCMT_LINUX
    #endif
#endif

#ifdef TCMT_WINDOWS
// winsock2.h must be before windows.h
#include <winsock2.h>
#include <windows.h>
#include <d3d11.h>
#if defined(SUPPORT_DIRECTX)
#include <dxgi.h>
#endif
#include <wbemidl.h>
#endif

#ifdef TCMT_MACOS
// IOKit/CoreFoundation for GPU enumeration
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#endif

class WmiManager;

class GpuInfo {
public:
    struct GpuData {
        std::wstring name;
        std::wstring deviceId;
        // 独立显存字节数。Apple Silicon 上是**统一内存总量** —— 那种机器没有独立显存，
        // 见 GpuInfo.cpp 里赋值处，以及 isUnified。
        uint64_t dedicatedMemory = 0;
        // true 表示 dedicatedMemory 报的其实是统一内存，不是 VRAM。
        // 界面靠它决定要不要画「显存」那一行 —— 名字叫显存、数是系统内存，会骗人。
        bool isUnified = false;
        double coreClock = 0.0;
        bool isNvidia = false;
        bool isIntegrated = false;
        bool isVirtual = false;
        int computeCapabilityMajor = 0;
        int computeCapabilityMinor = 0;
        unsigned int temperature = 0;
        double usage = 0.0;  // GPU usage (0-100)
        // Graphics API / driver string the platform exposes: "Metal 4" on
        // macOS, the driver release on Windows/Linux NVIDIA. Empty when the
        // platform has no readable value.
        std::string driver;
        // Apple Silicon: core count from AGXAccelerator ("gpu-core-count").
        int coreCount = 0;
    };

#ifdef TCMT_WINDOWS
    GpuInfo(WmiManager& manager);
    // Returns VRAM usage % (0-100) via NVML, or -1 if unavailable
    static double GetVramUsagePercent();
    // Returns GPU usage % (0-100) via NVML, or -1 if unavailable
    static double GetGpuUsage();
    // Returns GPU temperature (C) via NVML, or -1 if unavailable
    static double GetGpuTemperature();
    // Returns GPU power draw in mW via NVML, or -1 if unavailable
    static double GetGpuPower();
    // GPU fan info (multi-fan support)
    struct GpuFanInfo {
        unsigned int index = 0;
        int speedRpm = 0;       // RPM from NVAPI, or duty% from NVML
        bool isRpm = false;     // true if speedRpm is actual RPM (NVAPI)
    };
    // Returns per-fan speeds via NVML (NVML reports % of max, not actual RPM)
    static std::vector<GpuFanInfo> GetGpuFans();
#else
    GpuInfo();
#endif
    ~GpuInfo();

    const std::vector<GpuData>& GetGpuData() const;

#ifdef TCMT_WINDOWS
private:
    void DetectGpusViaWmi();
    void QueryIntelGpuInfo(int index);
    void QueryNvidiaGpuInfo(int index);
    bool IsVirtualGpu(const std::wstring& name);
    WmiManager& wmiManager;
    IWbemServices* pSvc = nullptr;
#endif

#ifdef TCMT_MACOS
public:
    void RefreshUsage();  // Re-read GPU utilization from IOKit
private:
    void DetectGpusViaMetal();  // uses IOKit (Metal-agnostic)
    bool IsVirtualGpu(const std::wstring& name);
#endif

#ifdef TCMT_LINUX
public:
    void RefreshUsage();  // Re-read GPU utilization from sysfs
private:
    void DetectGpusViaSysfs();
    bool IsVirtualGpu(const std::wstring& name);
#endif

    std::vector<GpuData> gpuList;
};

#ifdef TCMT_LINUX

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

#endif  // TCMT_LINUX
