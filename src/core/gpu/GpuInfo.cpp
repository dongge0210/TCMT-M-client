#include "GpuInfo.h"
#include "../Utils/Logger.h"

#ifdef TCMT_WINDOWS
// ======================== Windows Implementation ========================
#include "WmiManager.h"
#include <comutil.h>
#include <algorithm>
#include <cwctype>

// ======================== NVML Dynamic Loading ========================
// NVML (NVIDIA Management Library) is loaded at runtime via LoadLibrary/GetProcAddress
// to avoid hard dependency on NVIDIA CUDA Toolkit. On systems without an NVIDIA GPU,
// nvml.dll won't be present and all NVML calls are silently skipped.
//
// This replaces the previous static link to nvml.lib which caused the program
// to fail on non-NVIDIA systems.

// Minimum NVML types needed for function pointer signatures
using nvmlDevice_t = void*;
using nvmlReturn_t = unsigned int;

struct nvmlMemory_t {
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
};

struct nvmlUtilization_t {
    unsigned int gpu;
    unsigned int memory;
};

// NVML constants (not in header, so define locally)
constexpr nvmlReturn_t NVML_SUCCESS = 0;
constexpr int NVML_CLOCK_GRAPHICS = 1;
constexpr int NVML_TEMPERATURE_GPU = 0;

// Function pointer types for NVML API
using NvmlInitFn = nvmlReturn_t (*)();
using NvmlShutdownFn = nvmlReturn_t (*)();
using NvmlDeviceGetHandleByIndexFn = nvmlReturn_t (*)(unsigned int, nvmlDevice_t*);
using NvmlDeviceGetMemoryInfoFn = nvmlReturn_t (*)(nvmlDevice_t, nvmlMemory_t*);
using NvmlDeviceGetUtilizationRatesFn = nvmlReturn_t (*)(nvmlDevice_t, nvmlUtilization_t*);
using NvmlDeviceGetTemperatureFn = nvmlReturn_t (*)(nvmlDevice_t, int, unsigned int*);
using NvmlDeviceGetClockInfoFn = nvmlReturn_t (*)(nvmlDevice_t, int, unsigned int*);
using NvmlDeviceGetCudaComputeCapabilityFn = nvmlReturn_t (*)(nvmlDevice_t, int*, int*);

// Runtime NVML function table -- loaded once on first use
struct NvmlApi {
    HMODULE module = nullptr;

    NvmlInitFn init = nullptr;
    NvmlShutdownFn shutdown = nullptr;
    NvmlDeviceGetHandleByIndexFn getHandleByIndex = nullptr;
    NvmlDeviceGetMemoryInfoFn getMemoryInfo = nullptr;
    NvmlDeviceGetUtilizationRatesFn getUtilizationRates = nullptr;
    NvmlDeviceGetTemperatureFn getTemperature = nullptr;
    NvmlDeviceGetClockInfoFn getClockInfo = nullptr;
    NvmlDeviceGetCudaComputeCapabilityFn getCudaComputeCapability = nullptr;
};

// Singleton accessor: loads nvml.dll on first call, returns the function table.
// If the DLL is not available (no NVIDIA driver installed), all function pointers
// remain null and no further NVML queries are attempted.
static NvmlApi& GetNvmlApi() {
    static NvmlApi api;
    static bool attempted = false;
    if (!attempted) {
        attempted = true;
        api.module = LoadLibraryW(L"nvml.dll");
        if (!api.module) {
            Logger::Info("NVML not available (nvml.dll not found) -- skipping NVIDIA GPU details");
            return api;
        }

        // Resolve all NVML functions by their exported names
        api.init = reinterpret_cast<NvmlInitFn>(
            GetProcAddress(api.module, "nvmlInit_v2"));

        api.shutdown = reinterpret_cast<NvmlShutdownFn>(
            GetProcAddress(api.module, "nvmlShutdown"));

        api.getHandleByIndex = reinterpret_cast<NvmlDeviceGetHandleByIndexFn>(
            GetProcAddress(api.module, "nvmlDeviceGetHandleByIndex_v2"));

        api.getMemoryInfo = reinterpret_cast<NvmlDeviceGetMemoryInfoFn>(
            GetProcAddress(api.module, "nvmlDeviceGetMemoryInfo"));

        api.getUtilizationRates = reinterpret_cast<NvmlDeviceGetUtilizationRatesFn>(
            GetProcAddress(api.module, "nvmlDeviceGetUtilizationRates"));

        api.getTemperature = reinterpret_cast<NvmlDeviceGetTemperatureFn>(
            GetProcAddress(api.module, "nvmlDeviceGetTemperature"));

        api.getClockInfo = reinterpret_cast<NvmlDeviceGetClockInfoFn>(
            GetProcAddress(api.module, "nvmlDeviceGetClockInfo"));

        api.getCudaComputeCapability = reinterpret_cast<NvmlDeviceGetCudaComputeCapabilityFn>(
            GetProcAddress(api.module, "nvmlDeviceGetCudaComputeCapability"));

        // Validate mandatory functions -- if any are missing, treat as unavailable
        if (!api.init || !api.shutdown || !api.getHandleByIndex) {
            Logger::Warn("NVML loaded but missing required functions -- disabling NVML");
            FreeLibrary(api.module);
            api = NvmlApi{};
        }
    }
    return api;
}

GpuInfo::GpuInfo(WmiManager& manager) : wmiManager(manager) {
    if (!wmiManager.IsInitialized()) {
        Logger::Error("WMI service not initialized");
        return;
    }
    pSvc = wmiManager.GetWmiService();
    DetectGpusViaWmi();
}

GpuInfo::~GpuInfo() {
    Logger::Info("GPU information detection complete");
}

double GpuInfo::GetVramUsagePercent() {
    auto& nvml = GetNvmlApi();
    if (!nvml.init || !nvml.getMemoryInfo) return -1;
    nvmlReturn_t r = nvml.init();
    if (NVML_SUCCESS != r) return -1;
    nvmlDevice_t device;
    r = nvml.getHandleByIndex(0, &device);
    if (NVML_SUCCESS != r) { nvml.shutdown(); return -1; }
    nvmlMemory_t mem;
    r = nvml.getMemoryInfo(device, &mem);
    nvml.shutdown();
    if (NVML_SUCCESS != r || mem.total == 0) return -1;
    return (static_cast<double>(mem.used) / static_cast<double>(mem.total)) * 100.0;
}

bool GpuInfo::IsVirtualGpu(const std::wstring& name) {
    const std::vector<std::wstring> virtualGpuNames = {
        L"Microsoft Basic Display Adapter", L"Microsoft Hyper-V Video",
        L"VMware SVGA 3D", L"VirtualBox Graphics Adapter",
        L"Todesk Virtual Display Adapter", L"Parsec Virtual Display Adapter",
        L"TeamViewer Display", L"AnyDesk Display", L"VNC Display",
        L"Citrix Display", L"Remote Desktop Display", L"RDP Display",
        L"Standard VGA Graphics Adapter", L"Generic PnP Monitor",
        L"Virtual Desktop Infrastructure", L"VDI Display",
        L"Cloud Display", L"Remote Graphics",
        L"AskLinkIddDriver Device"
    };
    std::wstring lowerName = name;
    std::transform(lowerName.begin(), lowerName.end(), lowerName.begin(),
        [](wchar_t c) { return ::towlower(c); });
    for (const auto& vg : virtualGpuNames) {
        std::wstring lv = vg;
        std::transform(lv.begin(), lv.end(), lv.begin(), [](wchar_t c) { return ::towlower(c); });
        if (lowerName.find(lv) != std::wstring::npos) return true;
    }
    const std::vector<std::wstring> keywords = {
        L"virtual", L"remote", L"basic", L"generic", L"standard vga",
        L"rdp", L"vnc", L"citrix", L"vmware", L"virtualbox", L"hyper-v"
    };
    for (const auto& kw : keywords)
        if (lowerName.find(kw) != std::wstring::npos) return true;
    return false;
}

void GpuInfo::DetectGpusViaWmi() {
    IEnumWbemClassObject* pEnumerator = nullptr;
    HRESULT hres = pSvc->ExecQuery(bstr_t("WQL"), bstr_t("SELECT * FROM Win32_VideoController"),
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &pEnumerator);
    if (FAILED(hres)) { Logger::Error("WMI query failed"); return; }

    ULONG uReturn = 0;
    IWbemClassObject* pclsObj = nullptr;
    while (pEnumerator->Next(WBEM_INFINITE, 1, &pclsObj, &uReturn) == S_OK) {
        GpuData data;
        VARIANT vtName, vtPnpId, vtAdapterRAM, vtCurrentClockSpeed;
        VariantInit(&vtName); VariantInit(&vtPnpId);
        VariantInit(&vtAdapterRAM); VariantInit(&vtCurrentClockSpeed);

        if (SUCCEEDED(pclsObj->Get(L"Name", 0, &vtName, 0, 0)) && vtName.vt == VT_BSTR)
            data.name = vtName.bstrVal;
        if (SUCCEEDED(pclsObj->Get(L"PNPDeviceID", 0, &vtPnpId, 0, 0)) && vtPnpId.vt == VT_BSTR)
            data.deviceId = vtPnpId.bstrVal;
        if (SUCCEEDED(pclsObj->Get(L"AdapterRAM", 0, &vtAdapterRAM, 0, 0)) && vtAdapterRAM.vt == VT_UI4)
            data.dedicatedMemory = static_cast<uint64_t>(vtAdapterRAM.uintVal);
        if (SUCCEEDED(pclsObj->Get(L"CurrentClockSpeed", 0, &vtCurrentClockSpeed, 0, 0)) && vtCurrentClockSpeed.vt == VT_UI4)
            data.coreClock = static_cast<double>(vtCurrentClockSpeed.uintVal) / 1e6;

        data.isVirtual = IsVirtualGpu(data.name);
        std::wstring nameStr(data.name.begin(), data.name.end());
        data.isNvidia = nameStr.find(L"NVIDIA") != std::wstring::npos;
        data.isIntegrated = data.deviceId.find(L"VEN_8086") != std::wstring::npos;
        gpuList.push_back(data);

        // Log GPU detection
        std::string gpuNameStr(data.name.begin(), data.name.end());
        Logger::Info("Detected GPU: " + gpuNameStr +
                    " (virtual: " + (data.isVirtual ? "yes" : "no") +
                    ", NVIDIA: " + (data.isNvidia ? "yes" : "no") +
                    ", integrated: " + (data.isIntegrated ? "yes" : "no") + ")");

        VariantClear(&vtName); VariantClear(&vtPnpId);
        VariantClear(&vtAdapterRAM); VariantClear(&vtCurrentClockSpeed);
        pclsObj->Release();
    }
    pEnumerator->Release();

    // Query detailed info for NVIDIA GPUs (via NVML)
    for (size_t i = 0; i < gpuList.size(); ++i) {
        if (gpuList[i].isNvidia && !gpuList[i].isVirtual)
            QueryNvidiaGpuInfo(static_cast<int>(i));
    }
}

void GpuInfo::QueryIntelGpuInfo(int index) {
    IDXGIFactory* pFactory = nullptr;
    if (FAILED(CreateDXGIFactory(__uuidof(IDXGIFactory), (void**)&pFactory))) return;
    IDXGIAdapter* pAdapter = nullptr;
    if (SUCCEEDED(pFactory->EnumAdapters(0, &pAdapter))) {
        DXGI_ADAPTER_DESC desc;
        if (SUCCEEDED(pAdapter->GetDesc(&desc)))
            gpuList[index].dedicatedMemory = desc.DedicatedVideoMemory;
        pAdapter->Release();
    }
    pFactory->Release();
}

void GpuInfo::QueryNvidiaGpuInfo(int index) {
    auto& nvml = GetNvmlApi();
    if (!nvml.init) {
        // NVML DLL not available (no NVIDIA driver) -- silently skip detailed query
        return;
    }

    nvmlReturn_t result = nvml.init();
    if (NVML_SUCCESS != result) {
        Logger::Error("NVML initialization failed (error code: " + std::to_string(result) + ")");
        return;
    }

    nvmlDevice_t device;
    result = nvml.getHandleByIndex(0, &device);
    if (NVML_SUCCESS != result) {
        nvml.shutdown();
        Logger::Error("NVML failed to get device handle");
        return;
    }

    // Get VRAM info
    nvmlMemory_t memory;
    result = nvml.getMemoryInfo(device, &memory);
    if (NVML_SUCCESS == result) {
        gpuList[index].dedicatedMemory = memory.total;
    }

    // Get core clock (MHz)
    unsigned int clockMHz = 0;
    result = nvml.getClockInfo(device, NVML_CLOCK_GRAPHICS, &clockMHz);
    if (NVML_SUCCESS == result) {
        gpuList[index].coreClock = static_cast<double>(clockMHz);
    }

    // Get temperature
    unsigned int temp = 0;
    #pragma warning(push)
    #pragma warning(disable: 4996)
    result = nvml.getTemperature(device, NVML_TEMPERATURE_GPU, &temp);
    #pragma warning(pop)
    if (NVML_SUCCESS == result) gpuList[index].temperature = temp;

    // Get GPU usage
    nvmlUtilization_t util;
    result = nvml.getUtilizationRates(device, &util);
    if (NVML_SUCCESS == result) {
        gpuList[index].usage = static_cast<double>(util.gpu);
    }

    // Get CUDA compute capability (optional, not available in all NVML versions)
    if (nvml.getCudaComputeCapability) {
        int major = 0, minor = 0;
        result = nvml.getCudaComputeCapability(device, &major, &minor);
        if (NVML_SUCCESS == result) {
            gpuList[index].computeCapabilityMajor = major;
            gpuList[index].computeCapabilityMinor = minor;
        }
    }

    nvml.shutdown();
}

const std::vector<GpuInfo::GpuData>& GpuInfo::GetGpuData() const { return gpuList; }

#elif defined(TCMT_MACOS)
// ======================== macOS Implementation ========================
#include <sys/sysctl.h>
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <algorithm>
#include <locale>
#include <cstring>

// Get IOKit property as string
static std::string IORegistryString(io_registry_entry_t entry, const char* key) {
    CFTypeRef ref = IORegistryEntryCreateCFProperty(
        entry, CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8),
        kCFAllocatorDefault, 0);
    if (!ref) return "";
    std::string result;
    if (CFGetTypeID(ref) == CFStringGetTypeID()) {
        char buf[256] = {0};
        if (CFStringGetCString((CFStringRef)ref, buf, sizeof(buf), kCFStringEncodingUTF8))
            result = buf;
    }
    CFRelease(ref);
    return result;
}

// Get IOKit property as uint64
static uint64_t IORegistryUInt64(io_registry_entry_t entry, const char* key) {
    CFTypeRef ref = IORegistryEntryCreateCFProperty(
        entry, CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8),
        kCFAllocatorDefault, 0);
    if (!ref) return 0;
    uint64_t result = 0;
    if (CFGetTypeID(ref) == CFNumberGetTypeID()) {
        CFNumberGetValue((CFNumberRef)ref, kCFNumberSInt64Type, &result);
    }
    CFRelease(ref);
    return result;
}

GpuInfo::GpuInfo() {
    DetectGpusViaMetal();
}

GpuInfo::~GpuInfo() {
    Logger::Info("GPU information detection complete");
}

bool GpuInfo::IsVirtualGpu(const std::wstring& name) {
    std::wstring lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    const std::wstring virtualKeywords[] = {L"virtual", L"software", L"display"};
    for (const auto& kw : virtualKeywords)
        if (lower.find(kw) != std::wstring::npos) return true;
    return false;
}

void GpuInfo::DetectGpusViaMetal() {
    gpuList.clear();

    // Get machine model for GPU identification
    char machine[128] = {0};
    size_t len = sizeof(machine);
    sysctlbyname("hw.machine", machine, &len, nullptr, 0);

    // Get total physical memory (for unified memory estimate on Apple Silicon)
    uint64_t totalMem = 0;
    len = sizeof(totalMem);
    sysctlbyname("hw.memsize", &totalMem, &len, nullptr, 0);

    // Determine if Apple Silicon
    bool isAppleSilicon = (strncmp(machine, "arm", 3) == 0 ||
                           strncmp(machine, "Mac", 3) == 0);

    // Use IOKit IOAccelerator to enumerate GPUs
    mach_port_t masterPort;
    if (@available(macOS 12.0, *)) {
        masterPort = kIOMainPortDefault;
    } else {
        masterPort = 0; // Will fallback to kIOMasterPortDefault on older systems
    }

    io_iterator_t iter = 0;
    kern_return_t kr = IOServiceGetMatchingServices(
        masterPort,
        IOServiceMatching("IOAccelerator"),
        &iter);

    if (kr != KERN_SUCCESS) {
        // Fallback: try IOGraphics
        kr = IOServiceGetMatchingServices(
            masterPort,
            IOServiceMatching("IOGraphics"),
            &iter);
    }

    if (kr == KERN_SUCCESS) {
        io_registry_entry_t entry;
        while ((entry = IOIteratorNext(iter)) != 0) {
            std::string accelName = IORegistryString(entry, "IOClass");
            std::string model = IORegistryString(entry, "model");
            if (model.empty()) model = IORegistryString(entry, "IOName");

            uint64_t vram = IORegistryUInt64(entry, "VRAM,mb");
            if (vram == 0) vram = IORegistryUInt64(entry, "VRAM");

            GpuData data{};
            data.isIntegrated = true;
            data.isNvidia = (model.find("NVIDIA") != std::string::npos);
            data.isVirtual = false;
            data.coreClock = 0.0;
            data.temperature = 0;

            if (!model.empty()) {
                data.name = std::wstring(model.begin(), model.end());
            } else if (!accelName.empty()) {
                data.name = std::wstring(accelName.begin(), accelName.end());
            } else {
                data.name = L"Apple GPU";
            }

            if (vram > 0) {
                data.dedicatedMemory = vram * 1024 * 1024;
            } else if (isAppleSilicon && totalMem > 0) {
                // Apple Silicon uses unified memory: GPU uses a portion of system RAM
                data.dedicatedMemory = totalMem / 4; // rough estimate
            } else {
                data.dedicatedMemory = 0;
            }

            data.isVirtual = IsVirtualGpu(data.name);

            // Read GPU utilization from PerformanceStatistics
            CFTypeRef perfStatsRef = IORegistryEntryCreateCFProperty(
                entry, CFSTR("PerformanceStatistics"), kCFAllocatorDefault, 0);
            if (perfStatsRef && CFGetTypeID(perfStatsRef) == CFDictionaryGetTypeID()) {
                CFDictionaryRef perfDict = static_cast<CFDictionaryRef>(perfStatsRef);
                CFNumberRef utilRef = static_cast<CFNumberRef>(
                    CFDictionaryGetValue(perfDict, CFSTR("Device Utilization %")));
                if (utilRef && CFGetTypeID(utilRef) == CFNumberGetTypeID()) {
                    int utilVal = 0;
                    if (CFNumberGetValue(utilRef, kCFNumberIntType, &utilVal)) {
                        data.usage = static_cast<double>(utilVal);
                    }
                }
            }
            if (perfStatsRef) CFRelease(perfStatsRef);

            gpuList.push_back(data);
            IOObjectRelease(entry);
        }
        IOObjectRelease(iter);
    }

    // Also check for discrete GPU via IONetworking or additional classes
    io_iterator_t diskIter = 0;
    kr = IOServiceGetMatchingServices(
        masterPort,
        IOServiceMatching("IOPlatformDevice"),
        &diskIter);
    if (kr == KERN_SUCCESS) {
        io_registry_entry_t entry;
        while ((entry = IOIteratorNext(diskIter)) != 0) {
            std::string name = IORegistryString(entry, "IOName");
            if (name.find("gpu") != std::string::npos || name.find("nvme") != std::string::npos) {
                // Already handled above
            }
            IOObjectRelease(entry);
        }
        IOObjectRelease(diskIter);
    }

    // Fallback: if nothing found, create a single entry from machine model
    if (gpuList.empty()) {
        GpuData data;
        data.name = std::wstring(L"Apple GPU (") + std::wstring(machine, machine + strlen(machine)) + L")";
        data.isIntegrated = true;
        data.isNvidia = false;
        data.isVirtual = false;
        data.coreClock = 0.0;
        data.temperature = 0;
        if (totalMem > 0) {
            data.dedicatedMemory = isAppleSilicon ? (totalMem / 4) : 0;
        }
        gpuList.push_back(data);
    }

    Logger::Debug("GpuInfo: detected " + std::to_string(gpuList.size()) + " GPU(s)");
}

void GpuInfo::RefreshUsage() {
    mach_port_t masterPort;
    if (@available(macOS 12.0, *)) {
        masterPort = kIOMainPortDefault;
    } else {
        masterPort = 0;
    }

    io_iterator_t iter = 0;
    kern_return_t kr = IOServiceGetMatchingServices(
        masterPort,
        IOServiceMatching("IOAccelerator"),
        &iter);
    if (kr != KERN_SUCCESS) return;

    size_t gpuIdx = 0;
    io_registry_entry_t entry;
    while ((entry = IOIteratorNext(iter)) != 0) {
        if (gpuIdx < gpuList.size()) {
            CFTypeRef perfStatsRef = IORegistryEntryCreateCFProperty(
                entry, CFSTR("PerformanceStatistics"), kCFAllocatorDefault, 0);
            if (perfStatsRef && CFGetTypeID(perfStatsRef) == CFDictionaryGetTypeID()) {
                CFDictionaryRef perfDict = static_cast<CFDictionaryRef>(perfStatsRef);
                CFNumberRef utilRef = static_cast<CFNumberRef>(
                    CFDictionaryGetValue(perfDict, CFSTR("Device Utilization %")));
                if (utilRef && CFGetTypeID(utilRef) == CFNumberGetTypeID()) {
                    int utilVal = 0;
                    if (CFNumberGetValue(utilRef, kCFNumberIntType, &utilVal)) {
                        gpuList[gpuIdx].usage = static_cast<double>(utilVal);
                    }
                }
            }
            if (perfStatsRef) CFRelease(perfStatsRef);
            gpuIdx++;
        }
        IOObjectRelease(entry);
    }
    IOObjectRelease(iter);
}

const std::vector<GpuInfo::GpuData>& GpuInfo::GetGpuData() const { return gpuList; }

#else
#error "Unsupported platform"
#endif
