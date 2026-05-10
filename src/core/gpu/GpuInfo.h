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
        uint64_t dedicatedMemory = 0;
        double coreClock = 0.0;
        bool isNvidia = false;
        bool isIntegrated = false;
        bool isVirtual = false;
        int computeCapabilityMajor = 0;
        int computeCapabilityMinor = 0;
        unsigned int temperature = 0;
        double usage = 0.0;  // GPU usage (0-100)
    };

#ifdef TCMT_WINDOWS
    GpuInfo(WmiManager& manager);
    // Returns VRAM usage % (0-100) via NVML, or -1 if unavailable
    static double GetVramUsagePercent();
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

    std::vector<GpuData> gpuList;
};
