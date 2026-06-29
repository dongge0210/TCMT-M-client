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

// ======================== NVAPI Dynamic Loading (fan RPM) ========================
// NVML only gives fan speed as % of max. NVAPI returns actual RPM.

using NvAPI_Status = int;
using NvPhysicalGpuHandle = void*;
#define NVAPI_OK 0
#define NV_MAX_PHYSICAL_GPUS 64

static constexpr unsigned int NVAPI_Initialize_ID        = 0x0150E828;
static constexpr unsigned int NVAPI_EnumPhysicalGPUs_ID  = 0xE5AC921F;
static constexpr unsigned int NVAPI_GPU_GetCoolerSettings_ID = 0xDA141340;

struct NvapiCooler {
    unsigned int type;       // 1=RPM, 2=duty%
    unsigned int controller;
    unsigned int currentRpm; // RPM if type==1, duty% if type==2
    unsigned int defaultMin;
    unsigned int defaultMax;
    unsigned int currentMin;
    unsigned int currentMax;
    unsigned int targetLevel;
    unsigned int policy;
};
struct NvapiCoolerSettings { unsigned int version; unsigned int count; NvapiCooler coolers[8]; };

using NvapiQueryFn = void* (*)(unsigned int id);

struct NvapiState {
    HMODULE module = nullptr;
    NvapiQueryFn query = nullptr;
    bool ready = false;
    NvPhysicalGpuHandle gpus[NV_MAX_PHYSICAL_GPUS] = {};
    unsigned int gpuCount = 0;
};

static NvapiState& GetNvapi() {
    static NvapiState ns;
    static bool tried = false;
    if (!tried) { tried = true;
        ns.module = LoadLibraryW(L"nvapi64.dll");
        if (!ns.module) { Logger::Debug("NVAPI: nvapi64.dll not found"); return ns; }
        ns.query = (NvapiQueryFn)GetProcAddress(ns.module, "nvapi_QueryInterface");
        if (!ns.query) { Logger::Debug("NVAPI: QueryInterface not found"); return ns; }
        auto initFn = (NvAPI_Status(*)())ns.query(NVAPI_Initialize_ID);
        if (!initFn || initFn() != NVAPI_OK) { Logger::Debug("NVAPI: Init failed"); return ns; }
        ns.gpuCount = NV_MAX_PHYSICAL_GPUS;
        auto enumFn = (NvAPI_Status(*)(NvPhysicalGpuHandle*,unsigned int*))ns.query(NVAPI_EnumPhysicalGPUs_ID);
        if (!enumFn || enumFn(ns.gpus, &ns.gpuCount) != NVAPI_OK || ns.gpuCount == 0) {
            Logger::Debug("NVAPI: no GPUs"); return ns;
        }
        ns.ready = true;
        Logger::Info("NVAPI: " + std::to_string(ns.gpuCount) + " GPU(s)");
    }
    return ns;
}

// ======================== NVML Dynamic Loading ========================
using NvmlInitFn = nvmlReturn_t (*)();
using NvmlShutdownFn = nvmlReturn_t (*)();
using NvmlDeviceGetHandleByIndexFn = nvmlReturn_t (*)(unsigned int, nvmlDevice_t*);
using NvmlDeviceGetMemoryInfoFn = nvmlReturn_t (*)(nvmlDevice_t, nvmlMemory_t*);
using NvmlDeviceGetUtilizationRatesFn = nvmlReturn_t (*)(nvmlDevice_t, nvmlUtilization_t*);
using NvmlDeviceGetTemperatureFn = nvmlReturn_t (*)(nvmlDevice_t, int, unsigned int*);
using NvmlDeviceGetClockInfoFn = nvmlReturn_t (*)(nvmlDevice_t, int, unsigned int*);
using NvmlDeviceGetCudaComputeCapabilityFn = nvmlReturn_t (*)(nvmlDevice_t, int*, int*);
using NvmlDeviceGetNumFansFn = nvmlReturn_t (*)(nvmlDevice_t, unsigned int*);
using NvmlDeviceGetFanSpeedFn = nvmlReturn_t (*)(nvmlDevice_t, unsigned int*);  // v1: single fan
using NvmlDeviceGetFanSpeedV2Fn = nvmlReturn_t (*)(nvmlDevice_t, unsigned int, unsigned int*);  // v2: per-fan index
// NVML process info (v2 layout — 24 bytes, matches driver's nvmlProcessInfo_v2_t)
// Using v1-sized struct (16 bytes) with v2 driver causes buffer overflow → garbage VRAM + TUI freeze
struct nvmlProcessInfo_t {
    unsigned int pid;
    unsigned long long usedGpuMemory;
    unsigned int gpuInstanceId;
    unsigned int computeInstanceId;
};
constexpr unsigned int NVML_MAX_PROCESSES = 16;  // 16 × 24 = 384 bytes, safe stack
using NvmlDeviceGetComputeRunningProcessesFn = nvmlReturn_t (*)(nvmlDevice_t, unsigned int*, nvmlProcessInfo_t*);
using NvmlDeviceGetCountFn = nvmlReturn_t (*)(unsigned int*);

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
    NvmlDeviceGetNumFansFn getNumFans = nullptr;
    NvmlDeviceGetFanSpeedFn getFanSpeed = nullptr;
    NvmlDeviceGetFanSpeedV2Fn getFanSpeedV2 = nullptr;  // v2: per-fan index
    NvmlDeviceGetComputeRunningProcessesFn getComputeRunningProcesses = nullptr;
    NvmlDeviceGetCountFn getDeviceCount = nullptr;
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

        // Optional NVML functions (may not exist on older drivers)
        api.getNumFans = reinterpret_cast<NvmlDeviceGetNumFansFn>(
            GetProcAddress(api.module, "nvmlDeviceGetNumFans"));
        api.getFanSpeed = reinterpret_cast<NvmlDeviceGetFanSpeedFn>(
            GetProcAddress(api.module, "nvmlDeviceGetFanSpeed"));
        api.getFanSpeedV2 = reinterpret_cast<NvmlDeviceGetFanSpeedV2Fn>(
            GetProcAddress(api.module, "nvmlDeviceGetFanSpeed_v2"));
        // Only use v1 of ComputeRunningProcesses — matches our nvmlProcessInfo_t layout.
        // v2/v3 structs are larger (extra instanceId fields + ccProtectedMemory), causing
        // buffer overrun and garbage VRAM values when interpreted as v1.
        api.getComputeRunningProcesses = reinterpret_cast<NvmlDeviceGetComputeRunningProcessesFn>(
            GetProcAddress(api.module, "nvmlDeviceGetComputeRunningProcesses"));

        api.getDeviceCount = reinterpret_cast<NvmlDeviceGetCountFn>(
            GetProcAddress(api.module, "nvmlDeviceGetCount_v2"));
        if (!api.getDeviceCount)
            api.getDeviceCount = reinterpret_cast<NvmlDeviceGetCountFn>(
                GetProcAddress(api.module, "nvmlDeviceGetCount"));

        // Validate mandatory functions -- if any are missing, treat as unavailable
        if (!api.init || !api.shutdown || !api.getHandleByIndex) {
            Logger::Warn("NVML loaded but missing required functions -- disabling NVML");
            FreeLibrary(api.module);
            api = NvmlApi{};
        }
    }
    return api;
}

// Persistent NVML session — initialized once, kept alive across program lifetime.
// Avoids the overhead of nvmlInit/nvmlShutdown on every query.
struct NvmlSession {
    NvmlApi* api = nullptr;
    nvmlDevice_t device = nullptr;
    bool ok = false;

    NvmlSession() {
        api = &GetNvmlApi();
        if (!api->init) { Logger::Debug("NVML: DLL not loaded, skipping session"); return; }
        nvmlReturn_t r = api->init();
        if (NVML_SUCCESS != r) { Logger::Debug("NVML: nvmlInit failed (code=" + std::to_string(r) + ")"); return; }
        r = api->getHandleByIndex(0, &device);
        if (NVML_SUCCESS != r) { api->shutdown(); Logger::Debug("NVML: getHandleByIndex failed"); return; }
        ok = true;
        Logger::Info("NVML: persistent session initialized");
    }

    ~NvmlSession() {
        if (ok && api && api->shutdown) api->shutdown();
    }

    // Prevent copy/move
    NvmlSession(const NvmlSession&) = delete;
    NvmlSession& operator=(const NvmlSession&) = delete;
};

static NvmlSession& GetNvmlSession() {
    static NvmlSession session;
    return session;
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
    auto& s = GetNvmlSession();
    if (!s.ok || !s.api->getMemoryInfo) return -1;
    nvmlMemory_t mem;
    if (NVML_SUCCESS != s.api->getMemoryInfo(s.device, &mem) || mem.total == 0) return -1;
    return (static_cast<double>(mem.used) / static_cast<double>(mem.total)) * 100.0;
}

double GpuInfo::GetGpuUsage() {
    auto& s = GetNvmlSession();
    if (!s.ok || !s.api->getUtilizationRates) return -1;
    nvmlUtilization_t util;
    if (NVML_SUCCESS != s.api->getUtilizationRates(s.device, &util)) return -1;
    return static_cast<double>(util.gpu);
}

double GpuInfo::GetGpuTemperature() {
    auto& s = GetNvmlSession();
    if (!s.ok || !s.api->getTemperature) return -1;
    unsigned int temp = 0;
    if (NVML_SUCCESS != s.api->getTemperature(s.device, NVML_TEMPERATURE_GPU, &temp)) return -1;
    return static_cast<double>(temp);
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
    auto& s = GetNvmlSession();
    if (!s.ok) return; // NVML not available (no NVIDIA GPU)

    // Get VRAM info
    nvmlMemory_t memory;
    if (NVML_SUCCESS == s.api->getMemoryInfo(s.device, &memory))
        gpuList[index].dedicatedMemory = memory.total;

    // Get core clock (MHz)
    unsigned int clockMHz = 0;
    if (NVML_SUCCESS == s.api->getClockInfo(s.device, NVML_CLOCK_GRAPHICS, &clockMHz))
        gpuList[index].coreClock = static_cast<double>(clockMHz);

    // Get temperature
    unsigned int temp = 0;
    if (NVML_SUCCESS == s.api->getTemperature(s.device, NVML_TEMPERATURE_GPU, &temp))
        gpuList[index].temperature = temp;

    // Get GPU usage
    nvmlUtilization_t util;
    if (NVML_SUCCESS == s.api->getUtilizationRates(s.device, &util))
        gpuList[index].usage = static_cast<double>(util.gpu);

    // Get CUDA compute capability (optional, not available in all NVML versions)
    if (s.api->getCudaComputeCapability) {
        int major = 0, minor = 0;
        if (NVML_SUCCESS == s.api->getCudaComputeCapability(s.device, &major, &minor)) {
            gpuList[index].computeCapabilityMajor = major;
            gpuList[index].computeCapabilityMinor = minor;
        }
    }
}

const std::vector<GpuInfo::GpuData>& GpuInfo::GetGpuData() const { return gpuList; }

std::vector<GpuInfo::GpuFanInfo> GpuInfo::GetGpuFans() {
    std::vector<GpuFanInfo> result;
    auto& nvs = GetNvmlSession();

    // ── Try NVAPI for actual RPM first ──
    auto& napi = GetNvapi();
    if (napi.ready && napi.gpuCount > 0 && nvs.ok) {
        unsigned int nvapiIdx = 0;
        if (nvapiIdx < napi.gpuCount) {
            NvapiCoolerSettings cs = {};
            cs.version = 2;
            auto coolerFn = (NvAPI_Status(*)(NvPhysicalGpuHandle,unsigned int,NvapiCoolerSettings*,unsigned int))
                napi.query(NVAPI_GPU_GetCoolerSettings_ID);
            if (coolerFn && coolerFn(napi.gpus[nvapiIdx], 0, &cs, sizeof(cs)) == NVAPI_OK && cs.count > 0) {
                for (unsigned int i = 0; i < cs.count && i < 6; ++i) {
                    GpuFanInfo fi;
                    fi.index = i;
                    unsigned int rpm = cs.coolers[i].currentRpm;
                    if (cs.coolers[i].type == 1) {
                        fi.speedRpm = static_cast<int>(rpm);
                        fi.isRpm = true;
                    } else {
                        fi.speedRpm = -1;
                    }
                    result.push_back(fi);
                }
                // NVML backup for PWM-only coolers
                for (auto& fi : result) {
                    if (fi.speedRpm < 0 && nvs.api->getFanSpeedV2) {
                        unsigned int pct = 0;
                        if (nvs.api->getFanSpeedV2(nvs.device, fi.index, &pct) != NVML_SUCCESS
                            && fi.index == 0 && nvs.api->getFanSpeed)
                            nvs.api->getFanSpeed(nvs.device, &pct);
                        fi.speedRpm = static_cast<int>(pct);
                    }
                }
                if (!result.empty()) return result;
            }
        }
    }

    // ── Fallback: NVML only (percentage) ──
    if (!nvs.ok || !nvs.api->getNumFans) return result;
    unsigned int numFans = 0;
    if (NVML_SUCCESS != nvs.api->getNumFans(nvs.device, &numFans) || numFans == 0) return result;
    for (unsigned int i = 0; i < numFans && i < 6; ++i) {
        unsigned int speed = 0;
        nvmlReturn_t rc = NVML_SUCCESS + 1; // non-success sentinel
        if (nvs.api->getFanSpeedV2)
            rc = nvs.api->getFanSpeedV2(nvs.device, i, &speed);
        // v2 failed or unavailable → try v1 for fan 0 only
        if (rc != NVML_SUCCESS && i == 0 && nvs.api->getFanSpeed)
            rc = nvs.api->getFanSpeed(nvs.device, &speed);
        if (rc != NVML_SUCCESS) break;
        GpuFanInfo fi;
        fi.index = i;
        fi.speedRpm = static_cast<int>(speed);
        result.push_back(fi);
    }
    return result;
}

std::vector<GpuInfo::GpuProcess> GpuInfo::GetGpuProcesses() {
    std::vector<GpuProcess> result;
    auto& s = GetNvmlSession();
    if (!s.ok || !s.api->getComputeRunningProcesses || !s.api->getDeviceCount || !s.api->getHandleByIndex) return result;
    unsigned int deviceCount = 0;
    if (NVML_SUCCESS != s.api->getDeviceCount(&deviceCount)) return result;
    nvmlProcessInfo_t infos[NVML_MAX_PROCESSES];
    for (unsigned int d = 0; d < deviceCount; ++d) {
        nvmlDevice_t dev = nullptr;
        if (NVML_SUCCESS != s.api->getHandleByIndex(d, &dev)) continue;
        unsigned int count = NVML_MAX_PROCESSES;
        if (NVML_SUCCESS != s.api->getComputeRunningProcesses(dev, &count, infos)) continue;
        for (unsigned int i = 0; i < count && i < NVML_MAX_PROCESSES; ++i) {
            if (infos[i].usedGpuMemory > 0) {
                GpuProcess p;
                p.pid = infos[i].pid;
                p.gpuIndex = d;
                p.usedGpuMemory = infos[i].usedGpuMemory;
                result.push_back(p);
            }
        }
    }
    return result;
}

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

#elif defined(TCMT_LINUX)
// ======================== Linux Implementation ========================
#include <fstream>
#include <sstream>
#include <dirent.h>
#include <cstring>
#include <algorithm>
#include <cwctype>
#include <unistd.h>

static std::string ReadSysfsStr(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs) return {};
    std::string val;
    std::getline(ifs, val);
    return val;
}

static uint16_t ReadSysfsHex16(const std::string& path) {
    std::string s = ReadSysfsStr(path);
    if (s.empty()) return 0;
    uint16_t v = 0;
    std::stringstream ss(s);
    ss >> std::hex >> v;
    return v;
}

static uint64_t ReadSysfsUint64(const std::string& path) {
    std::string s = ReadSysfsStr(path);
    if (s.empty()) return 0;
    try { return std::stoull(s); }
    catch (...) { return 0; }
}

static unsigned int ReadSysfsUint(const std::string& path) {
    std::string s = ReadSysfsStr(path);
    if (s.empty()) return 0;
    try { return static_cast<unsigned int>(std::stoul(s)); }
    catch (...) { return 0; }
}

static std::string VendorName(uint16_t vid) {
    switch (vid) {
        case 0x1002: return "AMD";
        case 0x10de: return "NVIDIA";
        case 0x8086: return "Intel";
        case 0x1af4: return "Red Hat/QEMU";
        case 0x15ad: return "VMware";
        case 0x80ee: return "Oracle VM";
        default:     return "Unknown";
    }
}

GpuInfo::GpuInfo() {
    DetectGpusViaSysfs();
}

GpuInfo::~GpuInfo() {
    Logger::Info("GPU information detection complete");
}

bool GpuInfo::IsVirtualGpu(const std::wstring& name) {
    std::wstring lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    const std::wstring keywords[] = {
        L"virtual", L"qemu", L"virtio", L"vmware", L"virtualbox",
        L"llvmpipe", L"software", L"display"
    };
    for (const auto& kw : keywords)
        if (lower.find(kw) != std::wstring::npos) return true;
    return false;
}

void GpuInfo::DetectGpusViaSysfs() {
    gpuList.clear();

    DIR* dir = opendir("/sys/class/drm/");
    if (!dir) {
        Logger::Error("GpuInfo: cannot open /sys/class/drm/");
        return;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string name(entry->d_name);
        if (name.compare(0, 4, "card") != 0) continue;
        if (name.find('-', 4) != std::string::npos) continue;

        std::string devPath = "/sys/class/drm/" + name + "/device/";

        uint16_t vendorId = ReadSysfsHex16(devPath + "vendor");
        uint16_t deviceId = ReadSysfsHex16(devPath + "device");
        if (vendorId == 0 && deviceId == 0) continue;

        GpuData data;
        data.isNvidia = (vendorId == 0x10de);
        data.isIntegrated = (vendorId == 0x8086);
        data.isVirtual = (vendorId == 0x1af4 || vendorId == 0x15ad || vendorId == 0x80ee);

        std::string prodName = ReadSysfsStr(devPath + "product_name");
        if (!prodName.empty()) {
            data.name = std::wstring(prodName.begin(), prodName.end());
        } else {
            std::string fallback = VendorName(vendorId) + " GPU ("
                + std::to_string(vendorId) + ":" + std::to_string(deviceId) + ")";
            data.name = std::wstring(fallback.begin(), fallback.end());
        }

        data.deviceId = std::wstring(name.begin(), name.end());

        data.isVirtual = data.isVirtual || IsVirtualGpu(data.name);

        // GPU memory
        if (vendorId == 0x1002) {
            data.dedicatedMemory = ReadSysfsUint64(devPath + "mem_info_vram_total");
        }

        // GPU usage
        if (vendorId == 0x1002) {
            data.usage = static_cast<double>(ReadSysfsUint(devPath + "gpu_busy_percent"));
        } else if (vendorId == 0x8086) {
            unsigned int act = ReadSysfsUint(devPath + "gt_act_freq_mhz");
            unsigned int maxV = ReadSysfsUint(devPath + "gt_max_freq_mhz");
            if (maxV > 0)
                data.usage = (static_cast<double>(act) / static_cast<double>(maxV)) * 100.0;
        }

        // GPU temperature via hwmon
        std::string hwmonDir = devPath + "hwmon/";
        DIR* hwmon = opendir(hwmonDir.c_str());
        if (hwmon) {
            struct dirent* hwEntry;
            while ((hwEntry = readdir(hwmon)) != nullptr) {
                std::string hn(hwEntry->d_name);
                if (hn.compare(0, 5, "hwmon") != 0) continue;
                std::string raw = ReadSysfsStr(hwmonDir + hn + "/temp1_input");
                if (!raw.empty()) {
                    try {
                        data.temperature = static_cast<unsigned int>(std::stoul(raw) / 1000);
                    } catch (...) {}
                    break;
                }
            }
            closedir(hwmon);
        }

        gpuList.push_back(data);

        std::string gpuNameStr(data.name.begin(), data.name.end());
        Logger::Info("Detected GPU: " + gpuNameStr +
            " (virtual: " + (data.isVirtual ? "yes" : "no") +
            ", NVIDIA: " + (data.isNvidia ? "yes" : "no") +
            ", integrated: " + (data.isIntegrated ? "yes" : "no") + ")");
    }
    closedir(dir);
}

void GpuInfo::RefreshUsage() {
    for (auto& gpu : gpuList) {
        std::string cardName(gpu.deviceId.begin(), gpu.deviceId.end());
        std::string devPath = "/sys/class/drm/" + cardName + "/device/";

        uint16_t vid = ReadSysfsHex16(devPath + "vendor");
        if (vid == 0) continue;

        if (vid == 0x1002) {
            gpu.usage = static_cast<double>(ReadSysfsUint(devPath + "gpu_busy_percent"));
        } else if (vid == 0x8086) {
            unsigned int act = ReadSysfsUint(devPath + "gt_act_freq_mhz");
            unsigned int maxV = ReadSysfsUint(devPath + "gt_max_freq_mhz");
            if (maxV > 0)
                gpu.usage = (static_cast<double>(act) / static_cast<double>(maxV)) * 100.0;
        }

        // Re-read temperature
        std::string hwmonDir = devPath + "hwmon/";
        DIR* hwmon = opendir(hwmonDir.c_str());
        if (hwmon) {
            struct dirent* hwEntry;
            while ((hwEntry = readdir(hwmon)) != nullptr) {
                std::string hn(hwEntry->d_name);
                if (hn.compare(0, 5, "hwmon") != 0) continue;
                std::string raw = ReadSysfsStr(hwmonDir + hn + "/temp1_input");
                if (!raw.empty()) {
                    try {
                        gpu.temperature = static_cast<unsigned int>(std::stoul(raw) / 1000);
                    } catch (...) {}
                    break;
                }
            }
            closedir(hwmon);
        }
    }
}

const std::vector<GpuInfo::GpuData>& GpuInfo::GetGpuData() const { return gpuList; }

#else
#error "Unsupported platform"
#endif
