#include "GpuInfo.h"
#include "../Utils/Logger.h"

#ifdef TCMT_WINDOWS
// ======================== Windows Implementation ========================
#include "WMIManager.h"
#include <comutil.h>
#include <algorithm>
#include <cctype>
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
// nvmlClockType_t order (NVML API Reference Guide, nvmlClockType_t):
// GRAPHICS = 0, SM = 1, MEM = 2, VIDEO = 3. Do not "fix" this to 1 — that is
// the SM clock, which is what an off-by-one here silently returns.
constexpr int NVML_CLOCK_GRAPHICS = 0;
constexpr int NVML_CLOCK_MEM = 2;
constexpr int NVML_TEMPERATURE_GPU = 0;   // nvmlTemperatureSensors_t

// ======================== NVAPI Dynamic Loading (fan RPM) ========================
// NVML only gives fan speed as % of max. NVAPI returns actual RPM.

using NvAPI_Status = int;
using NvPhysicalGpuHandle = void*;
#define NVAPI_OK 0
#define NV_MAX_PHYSICAL_GPUS 64

static constexpr unsigned int NVAPI_Initialize_ID        = 0x0150E828;
static constexpr unsigned int NVAPI_EnumPhysicalGPUs_ID  = 0xE5AC921F;
static constexpr unsigned int NVAPI_GPU_GetCoolerSettings_ID = 0xDA141340;
static constexpr unsigned int NVAPI_GPU_ClientFanCoolersGetStatus_ID = 0x35AED5E8;

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

// NVAPI ClientFanCoolers (consumer GPU fan status — returns actual RPM)
struct NvapiClientCooler {
    unsigned int currentRpm;
    unsigned int targetMin;
    unsigned int targetMax;
    unsigned int controlMode;
    unsigned int softwareBased;
};
struct NvapiClientCoolerStatus { unsigned int version; unsigned int count; unsigned int flags; NvapiClientCooler coolers[8]; };

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
        if (!ns.module) { Logger::Debug("GPU_FAN: nvapi64.dll not found"); return ns; }
        ns.query = (NvapiQueryFn)GetProcAddress(ns.module, "nvapi_QueryInterface");
        if (!ns.query) { Logger::Debug("GPU_FAN: nvapi_QueryInterface not found"); return ns; }
        auto initFn = (NvAPI_Status(*)())ns.query(NVAPI_Initialize_ID);
        if (!initFn || initFn() != NVAPI_OK) { Logger::Debug("GPU_FAN: NVAPI Init failed"); return ns; }
        ns.gpuCount = NV_MAX_PHYSICAL_GPUS;
        auto enumFn = (NvAPI_Status(*)(NvPhysicalGpuHandle*,unsigned int*))ns.query(NVAPI_EnumPhysicalGPUs_ID);
        NvAPI_Status rc = enumFn ? enumFn(ns.gpus, &ns.gpuCount) : -1;
        if (rc != NVAPI_OK || ns.gpuCount == 0) {
            Logger::Debug("GPU_FAN: NVAPI EnumPhysicalGPUs rc=" + std::to_string(rc) + " count=" + std::to_string(ns.gpuCount));
            return ns;
        }
        ns.ready = true;
        Logger::Debug("NVAPI: " + std::to_string(ns.gpuCount) + " GPU(s)");
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
using NvmlDeviceGetCountFn = nvmlReturn_t (*)(unsigned int*);
// 功耗：毫瓦。这是 NVIDIA 卡上唯一不需要自己乘电压电流的功耗来源。
using NvmlDeviceGetPowerUsageFn = nvmlReturn_t (*)(nvmlDevice_t, unsigned int*);
using NvmlDeviceGetEnforcedPowerLimitFn = nvmlReturn_t (*)(nvmlDevice_t, unsigned int*);
// Driver release string, e.g. "560.94" — the number NVIDIA's own UI shows.
using NvmlSystemGetDriverVersionFn = nvmlReturn_t (*)(char*, unsigned int);
// Optional device queries. All of these have existed for years, but they are
// still resolved as optional so an old driver degrades to "field not shown"
// instead of disabling NVML entirely.
using NvmlDeviceGetNameFn = nvmlReturn_t (*)(nvmlDevice_t, char*, unsigned int);
using NvmlDeviceGetNumGpuCoresFn = nvmlReturn_t (*)(nvmlDevice_t, unsigned int*);
using NvmlDeviceGetMaxClockInfoFn = nvmlReturn_t (*)(nvmlDevice_t, int, unsigned int*);
using NvmlDeviceGetSerialFn = nvmlReturn_t (*)(nvmlDevice_t, char*, unsigned int);
using NvmlDeviceGetVbiosVersionFn = nvmlReturn_t (*)(nvmlDevice_t, char*, unsigned int);
using NvmlDeviceGetCurrPcieLinkGenerationFn = nvmlReturn_t (*)(nvmlDevice_t, unsigned int*);
using NvmlDeviceGetCurrPcieLinkWidthFn = nvmlReturn_t (*)(nvmlDevice_t, unsigned int*);
using NvmlDeviceGetMaxPcieLinkGenerationFn = nvmlReturn_t (*)(nvmlDevice_t, unsigned int*);
using NvmlDeviceGetMaxPcieLinkWidthFn = nvmlReturn_t (*)(nvmlDevice_t, unsigned int*);

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
    NvmlDeviceGetCountFn getDeviceCount = nullptr;
    NvmlDeviceGetPowerUsageFn getPowerUsage = nullptr;              // mW
    NvmlDeviceGetEnforcedPowerLimitFn getEnforcedPowerLimit = nullptr;  // mW
    NvmlSystemGetDriverVersionFn systemGetDriverVersion = nullptr;
    NvmlDeviceGetNameFn getDeviceName = nullptr;
    NvmlDeviceGetNumGpuCoresFn getNumGpuCores = nullptr;
    NvmlDeviceGetMaxClockInfoFn getMaxClockInfo = nullptr;
    NvmlDeviceGetSerialFn getSerial = nullptr;
    NvmlDeviceGetVbiosVersionFn getVbiosVersion = nullptr;
    NvmlDeviceGetCurrPcieLinkGenerationFn getPcieGen = nullptr;
    NvmlDeviceGetCurrPcieLinkWidthFn getPcieWidth = nullptr;
    NvmlDeviceGetMaxPcieLinkGenerationFn getMaxPcieGen = nullptr;
    NvmlDeviceGetMaxPcieLinkWidthFn getMaxPcieWidth = nullptr;
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
            Logger::Debug("NVML not available (nvml.dll not found) -- skipping NVIDIA GPU details");
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
        api.systemGetDriverVersion = reinterpret_cast<NvmlSystemGetDriverVersionFn>(
            GetProcAddress(api.module, "nvmlSystemGetDriverVersion"));

        // 功耗。老驱动 / 老卡可能没有这两个入口，取不到就保持 null，调用点看得到。
        api.getPowerUsage = reinterpret_cast<NvmlDeviceGetPowerUsageFn>(
            GetProcAddress(api.module, "nvmlDeviceGetPowerUsage"));
        api.getEnforcedPowerLimit = reinterpret_cast<NvmlDeviceGetEnforcedPowerLimitFn>(
            GetProcAddress(api.module, "nvmlDeviceGetEnforcedPowerLimit"));
        api.getDeviceName = reinterpret_cast<NvmlDeviceGetNameFn>(
            GetProcAddress(api.module, "nvmlDeviceGetName"));
        api.getNumGpuCores = reinterpret_cast<NvmlDeviceGetNumGpuCoresFn>(
            GetProcAddress(api.module, "nvmlDeviceGetNumGpuCores"));
        api.getMaxClockInfo = reinterpret_cast<NvmlDeviceGetMaxClockInfoFn>(
            GetProcAddress(api.module, "nvmlDeviceGetMaxClockInfo"));
        api.getSerial = reinterpret_cast<NvmlDeviceGetSerialFn>(
            GetProcAddress(api.module, "nvmlDeviceGetSerial"));
        api.getVbiosVersion = reinterpret_cast<NvmlDeviceGetVbiosVersionFn>(
            GetProcAddress(api.module, "nvmlDeviceGetVbiosVersion"));
        api.getPcieGen = reinterpret_cast<NvmlDeviceGetCurrPcieLinkGenerationFn>(
            GetProcAddress(api.module, "nvmlDeviceGetCurrPcieLinkGeneration"));
        api.getPcieWidth = reinterpret_cast<NvmlDeviceGetCurrPcieLinkWidthFn>(
            GetProcAddress(api.module, "nvmlDeviceGetCurrPcieLinkWidth"));
        api.getMaxPcieGen = reinterpret_cast<NvmlDeviceGetMaxPcieLinkGenerationFn>(
            GetProcAddress(api.module, "nvmlDeviceGetMaxPcieLinkGeneration"));
        api.getMaxPcieWidth = reinterpret_cast<NvmlDeviceGetMaxPcieLinkWidthFn>(
            GetProcAddress(api.module, "nvmlDeviceGetMaxPcieLinkWidth"));
        api.getDeviceCount = reinterpret_cast<NvmlDeviceGetCountFn>(
            GetProcAddress(api.module, "nvmlDeviceGetCount_v2"));
        if (!api.getDeviceCount)
            api.getDeviceCount = reinterpret_cast<NvmlDeviceGetCountFn>(
                GetProcAddress(api.module, "nvmlDeviceGetCount"));

        // Validate mandatory functions -- if any are missing, treat as unavailable
        if (!api.init || !api.shutdown || !api.getHandleByIndex) {
            Logger::Debug("NVML loaded but missing required functions -- disabling NVML");
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
        Logger::Debug("NVML: persistent session initialized");
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

// 显卡功耗，毫瓦（与 SystemInfo::gpuPower 的单位一致）。取不到返回 -1。
// 不用自己去乘电压电流：NVML 直接给这个数，而用户态也拿不到 VRM 的电压电流。
double GpuInfo::GetGpuPower() {
    auto& s = GetNvmlSession();
    if (!s.ok || !s.api->getPowerUsage) return -1;
    unsigned int mw = 0;
    if (NVML_SUCCESS != s.api->getPowerUsage(s.device, &mw)) return -1;
    return static_cast<double>(mw);
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
        VARIANT vtName, vtPnpId, vtAdapterRAM, vtCurrentClockSpeed, vtDriver;
        VariantInit(&vtName); VariantInit(&vtPnpId);
        VariantInit(&vtAdapterRAM); VariantInit(&vtCurrentClockSpeed);
        VariantInit(&vtDriver);

        if (SUCCEEDED(pclsObj->Get(L"Name", 0, &vtName, 0, 0)) && vtName.vt == VT_BSTR)
            data.name = vtName.bstrVal;
        if (SUCCEEDED(pclsObj->Get(L"PNPDeviceID", 0, &vtPnpId, 0, 0)) && vtPnpId.vt == VT_BSTR)
            data.deviceId = vtPnpId.bstrVal;
        // AMD/Intel fall back to this; NVIDIA overwrites it with the NVML
        // release string below.
        if (SUCCEEDED(pclsObj->Get(L"DriverVersion", 0, &vtDriver, 0, 0)) && vtDriver.vt == VT_BSTR) {
            std::wstring dv(vtDriver.bstrVal, SysStringLen(vtDriver.bstrVal));
            data.driver = std::string(dv.begin(), dv.end());
        }
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
        VariantClear(&vtDriver);
        pclsObj->Release();
    }
    pEnumerator->Release();

    // Query detailed info for NVIDIA GPUs (via NVML), one NVML device per
    // adapter so multi-GPU machines do not show card 0 everywhere.
    AssignNvmlDevices();
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

// WMI and NVML name the same adapter with slightly different strings, and a
// driver update can change either. Compare the alphanumeric core so a vendor
// prefix or spacing difference does not break the mapping.
static std::string NormalizeGpuName(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (std::isalnum(c)) out.push_back(static_cast<char>(std::tolower(c)));
    }
    return out;
}

// Adapter names are ASCII on every card NVML covers; folding is enough and
// avoids pulling in a code-page conversion for a comparison key.
static std::string WideNameToNarrow(const std::wstring& w) {
    std::string out;
    out.reserve(w.size());
    for (wchar_t c : w)
        out.push_back(c > 0 && c < 128 ? static_cast<char>(c) : '?');
    return out;
}

void GpuInfo::AssignNvmlDevices() {
    auto& s = GetNvmlSession();
    if (!s.ok || !s.api->getDeviceCount) return;

    unsigned int count = 0;
    if (NVML_SUCCESS != s.api->getDeviceCount(&count)) return;

    // Every NVIDIA adapter must own one NVML device. Before this, all of them
    // queried handle 0, so a two-NVIDIA machine showed the first card's
    // clocks, temperature and power on the second one as well.
    std::vector<bool> taken(gpuList.size(), false);
    for (unsigned int d = 0; d < count; ++d) {
        nvmlDevice_t dev = nullptr;
        if (NVML_SUCCESS != s.api->getHandleByIndex(d, &dev)) continue;

        std::string nvName;
        if (s.api->getDeviceName) {
            char name[96] = {0};
            if (NVML_SUCCESS == s.api->getDeviceName(dev, name, static_cast<unsigned int>(sizeof(name))))
                nvName = NormalizeGpuName(name);
        }

        int match = -1;
        if (!nvName.empty()) {
            for (size_t i = 0; i < gpuList.size(); ++i) {
                if (taken[i] || !gpuList[i].isNvidia || gpuList[i].isVirtual) continue;
                const std::string wmi = NormalizeGpuName(WideNameToNarrow(gpuList[i].name));
                if (!wmi.empty() && (wmi.find(nvName) != std::string::npos ||
                                     nvName.find(wmi) != std::string::npos)) {
                    match = static_cast<int>(i);
                    break;
                }
            }
        }
        if (match < 0) {
            for (size_t i = 0; i < gpuList.size(); ++i) {
                if (!taken[i] && gpuList[i].isNvidia && !gpuList[i].isVirtual) {
                    match = static_cast<int>(i);
                    break;
                }
            }
        }
        if (match < 0) continue;  // more NVML devices than display adapters

        taken[match] = true;
        gpuList[match].nvmlIndex = static_cast<int>(d);
        Logger::Debug("NVML: device " + std::to_string(d) + " -> adapter " + std::to_string(match));
    }
}

void GpuInfo::QueryNvidiaGpuInfo(int index) {
    auto& s = GetNvmlSession();
    if (!s.ok) return; // NVML not available (no NVIDIA GPU)

    GpuData& gpu = gpuList[index];
    nvmlDevice_t dev = nullptr;
    if (gpu.nvmlIndex >= 0 &&
        NVML_SUCCESS == s.api->getHandleByIndex(static_cast<unsigned int>(gpu.nvmlIndex), &dev)) {
        // mapped by name in AssignNvmlDevices
    } else {
        dev = nullptr;
        // Unmapped and there is only one NVIDIA adapter: device 0 is it.
        int nvidiaCount = 0;
        for (const auto& g : gpuList)
            if (g.isNvidia && !g.isVirtual) ++nvidiaCount;
        if (nvidiaCount == 1) dev = s.device;
    }
    if (!dev) return;

    // VRAM: total, used and the percentage NVML itself would report.
    nvmlMemory_t memory;
    if (s.api->getMemoryInfo && NVML_SUCCESS == s.api->getMemoryInfo(dev, &memory)) {
        gpu.dedicatedMemory = memory.total;
        gpu.memoryUsed = memory.used;
        if (memory.total > 0)
            gpu.memoryPercent = 100.0 * static_cast<double>(memory.used) /
                                static_cast<double>(memory.total);
    }

    unsigned int value = 0;
    if (s.api->getClockInfo && NVML_SUCCESS == s.api->getClockInfo(dev, NVML_CLOCK_GRAPHICS, &value))
        gpu.coreClock = static_cast<double>(value);
    if (s.api->getMaxClockInfo && NVML_SUCCESS == s.api->getMaxClockInfo(dev, NVML_CLOCK_GRAPHICS, &value))
        gpu.maxCoreClock = static_cast<int>(value);
    if (s.api->getClockInfo && NVML_SUCCESS == s.api->getClockInfo(dev, NVML_CLOCK_MEM, &value))
        gpu.memClock = static_cast<int>(value);
    if (s.api->getTemperature && NVML_SUCCESS == s.api->getTemperature(dev, NVML_TEMPERATURE_GPU, &value))
        gpu.temperature = value;

    nvmlUtilization_t util;
    if (s.api->getUtilizationRates && NVML_SUCCESS == s.api->getUtilizationRates(dev, &util))
        gpu.usage = static_cast<double>(util.gpu);

    if (s.api->getNumGpuCores && NVML_SUCCESS == s.api->getNumGpuCores(dev, &value))
        gpu.coreCount = static_cast<int>(value);

    // Power: milliwatts, the unit SystemInfo::gpuPower and the TUI use.
    if (s.api->getPowerUsage) {
        nvmlReturn_t prc = s.api->getPowerUsage(dev, &value);
        if (NVML_SUCCESS == prc) {
            gpu.powerMw = static_cast<int>(value);
        } else {
            // 0 is a real reading on an idling laptop GPU; a failed query is
            // not, and the TUI must not print "0.0 W" for it.
            Logger::Debug("NVML: power query failed (code=" + std::to_string(prc) + ")");
        }
    }
    if (s.api->getEnforcedPowerLimit && NVML_SUCCESS == s.api->getEnforcedPowerLimit(dev, &value))
        gpu.powerLimitMw = value;

    if (s.api->getPcieGen && NVML_SUCCESS == s.api->getPcieGen(dev, &value))
        gpu.pcieGen = value;
    if (s.api->getPcieWidth && NVML_SUCCESS == s.api->getPcieWidth(dev, &value))
        gpu.pcieWidth = value;
    // Current width/gen is the negotiated link, which sits at x1/x2 while the
    // card idles. The max is what the slot and card can actually do; without
    // both, "Gen4 x2" reads as a broken slot.
    if (s.api->getMaxPcieGen && NVML_SUCCESS == s.api->getMaxPcieGen(dev, &value))
        gpu.maxPcieGen = value;
    if (s.api->getMaxPcieWidth && NVML_SUCCESS == s.api->getMaxPcieWidth(dev, &value))
        gpu.maxPcieWidth = value;

    // Fans: per-fan % of max, averaged the way every monitor UI shows it.
    unsigned int fanCount = 0;
    if (s.api->getNumFans && NVML_SUCCESS == s.api->getNumFans(dev, &fanCount) && fanCount > 0) {
        gpu.fanCount = static_cast<int>(fanCount);
        unsigned int sum = 0, got = 0;
        for (unsigned int f = 0; f < fanCount && f < 8; ++f) {
            unsigned int pct = 0;
            if (s.api->getFanSpeedV2 && NVML_SUCCESS == s.api->getFanSpeedV2(dev, f, &pct)) {
                sum += pct;
                ++got;
            }
        }
        if (got == 0 && s.api->getFanSpeed && NVML_SUCCESS == s.api->getFanSpeed(dev, &sum)) got = 1;
        if (got > 0) gpu.fanPercent = static_cast<int>(sum / got);
    }

    if (s.api->getSerial) {
        char serial[64] = {0};
        if (NVML_SUCCESS == s.api->getSerial(dev, serial, static_cast<unsigned int>(sizeof(serial))) && serial[0] != '\0')
            gpu.serial = serial;
    }
    if (s.api->getVbiosVersion) {
        char vbios[80] = {0};
        if (NVML_SUCCESS == s.api->getVbiosVersion(dev, vbios, static_cast<unsigned int>(sizeof(vbios))) && vbios[0] != '\0')
            gpu.vbios = vbios;
    }

    // Get CUDA compute capability (optional, not available in all NVML versions)
    if (s.api->getCudaComputeCapability) {
        int major = 0, minor = 0;
        if (NVML_SUCCESS == s.api->getCudaComputeCapability(dev, &major, &minor)) {
            gpu.computeCapabilityMajor = major;
            gpu.computeCapabilityMinor = minor;
        }
    }

    // Driver release ("560.94"): the number NVIDIA's own control panel shows,
    // which is more useful than WMI's "31.0.15.6094" mapping-of-a-version.
    if (s.api->systemGetDriverVersion) {
        char driver[80] = {0};
        if (NVML_SUCCESS == s.api->systemGetDriverVersion(driver, sizeof(driver)) &&
            driver[0] != '\0') {
            gpu.driver = driver;
        }
    }
}

GpuInfo::NvidiaLive GpuInfo::GetNvidiaLive(int nvmlIndex) {
    NvidiaLive out;
    auto& s = GetNvmlSession();
    if (!s.ok || nvmlIndex < 0) return out;

    nvmlDevice_t dev = nullptr;
    if (NVML_SUCCESS != s.api->getHandleByIndex(static_cast<unsigned int>(nvmlIndex), &dev) || !dev)
        return out;

    unsigned int value = 0;
    nvmlUtilization_t util;
    nvmlMemory_t memory;
    if (s.api->getUtilizationRates && NVML_SUCCESS == s.api->getUtilizationRates(dev, &util))
        out.usage = static_cast<double>(util.gpu);
    if (s.api->getTemperature && NVML_SUCCESS == s.api->getTemperature(dev, NVML_TEMPERATURE_GPU, &value))
        out.temperature = static_cast<double>(value);
    if (s.api->getPowerUsage && NVML_SUCCESS == s.api->getPowerUsage(dev, &value))
        out.powerMw = static_cast<double>(value);  // includes a real 0 W
    else
        out.powerMw = -1;
    if (s.api->getClockInfo && NVML_SUCCESS == s.api->getClockInfo(dev, NVML_CLOCK_GRAPHICS, &value))
        out.clockMhz = static_cast<double>(value);
    if (s.api->getClockInfo && NVML_SUCCESS == s.api->getClockInfo(dev, NVML_CLOCK_MEM, &value))
        out.memClockMhz = static_cast<double>(value);
    if (s.api->getMemoryInfo && NVML_SUCCESS == s.api->getMemoryInfo(dev, &memory) && memory.total > 0)
        out.memoryPercent = 100.0 * static_cast<double>(memory.used) /
                            static_cast<double>(memory.total);
    out.valid = true;
    return out;
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
            cs.version = sizeof(NvapiCoolerSettings) | 0x20000;  // v2
            // Also try bare sizeof (some drivers expect this)
            auto coolerFn = (NvAPI_Status(*)(NvPhysicalGpuHandle,unsigned int,NvapiCoolerSettings*,unsigned int))
                napi.query(NVAPI_GPU_GetCoolerSettings_ID);
            if (!coolerFn || coolerFn(napi.gpus[nvapiIdx], 0, &cs, sizeof(cs)) != NVAPI_OK) {
                cs.version = sizeof(NvapiCoolerSettings);  // bare size fallback
                coolerFn = (NvAPI_Status(*)(NvPhysicalGpuHandle,unsigned int,NvapiCoolerSettings*,unsigned int))
                    napi.query(NVAPI_GPU_GetCoolerSettings_ID);
            }
            if (coolerFn && coolerFn(napi.gpus[nvapiIdx], 0, &cs, sizeof(cs)) == NVAPI_OK && cs.count > 0) {
                for (unsigned int i = 0; i < cs.count && i < 6; ++i) {
                    GpuFanInfo fi;
                    fi.index = i;
                    if (cs.coolers[i].type == 1) {
                        fi.speedRpm = static_cast<int>(cs.coolers[i].currentRpm);
                        fi.isRpm = true;
                    } else {
                        fi.speedRpm = static_cast<int>(cs.coolers[i].currentRpm); // duty%
                    }
                    result.push_back(fi);
                }
                if (!result.empty()) return result;
            }
            // ── NVAPI fallback: ClientFanCoolersGetStatus ──
            result.clear();
            NvapiClientCoolerStatus fcs = {};
            fcs.version = sizeof(NvapiClientCoolerStatus) | 0x10000;  // v1
            auto fanFn = (NvAPI_Status(*)(NvPhysicalGpuHandle, NvapiClientCoolerStatus*))
                napi.query(NVAPI_GPU_ClientFanCoolersGetStatus_ID);
            if (fanFn && fanFn(napi.gpus[nvapiIdx], &fcs) == NVAPI_OK && fcs.count > 0) {
                for (unsigned int i = 0; i < fcs.count && i < 6; ++i) {
                    GpuFanInfo fi;
                    fi.index = i;
                    fi.speedRpm = static_cast<int>(fcs.coolers[i].currentRpm);
                    fi.isRpm = true;
                    result.push_back(fi);
                }
                if (!result.empty()) return result;
            }
        }
    } else if (napi.ready && napi.gpuCount == 0) {
        Logger::Debug("GPU fans: NVAPI initialized but 0 GPUs enumerated");
    } else if (!napi.ready) {
        Logger::Debug("GPU fans: NVAPI not available, using NVML duty% only");
    }

    // ── Fallback: NVML only (percentage) ──
    if (!nvs.ok || !nvs.api->getNumFans) return result;
    unsigned int numFans = 0;
    if (NVML_SUCCESS != nvs.api->getNumFans(nvs.device, &numFans) || numFans == 0) return result;
    for (unsigned int i = 0; i < numFans && i < 6; ++i) {
        unsigned int speed = 0;
        nvmlReturn_t rc = NVML_SUCCESS + 1;
        if (nvs.api->getFanSpeedV2)
            rc = nvs.api->getFanSpeedV2(nvs.device, i, &speed);
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

#elif defined(TCMT_MACOS)
// ======================== macOS Implementation ========================
#include <sys/sysctl.h>
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <locale>
#include <map>
#include <mutex>
#include <thread>

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

// ---------------------------------------------------------------------------
// Driver / graphics API string. macOS exposes the Metal generation only in
// system_profiler's display profile ("Metal Support: Metal 4"), so it is
// parsed on a background thread and cached; RefreshUsage() attaches it once
// it lands. An Intel Mac with two GPUs gets one entry per "Chipset Model".
// ---------------------------------------------------------------------------
namespace {
std::mutex g_metalMutex;
std::map<std::string, std::string> g_metalByChipset;
std::atomic<int> g_metalState{0};      // 0 idle, 1 running
std::atomic<int64_t> g_metalStampMs{0};

int64_t MetalNowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string TrimSpaces(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

void RunDisplayProfiler() {
    std::map<std::string, std::string> found;
    FILE* fp = popen("/usr/sbin/system_profiler SPDisplaysDataType 2>/dev/null", "r");
    if (fp) {
        char buf[512];
        std::string chipset;
        while (fgets(buf, sizeof(buf), fp)) {
            const std::string line(buf);
            auto pos = line.find("Chipset Model:");
            if (pos != std::string::npos) {
                chipset = TrimSpaces(line.substr(pos + 14));
                continue;
            }
            pos = line.find("Metal Support:");
            if (pos != std::string::npos && !chipset.empty()) {
                found[chipset] = TrimSpaces(line.substr(pos + 14));
                chipset.clear();
            }
        }
        pclose(fp);
    }
    {
        std::lock_guard<std::mutex> lk(g_metalMutex);
        g_metalByChipset.swap(found);
    }
    g_metalStampMs.store(MetalNowMs());
    g_metalState.store(0);
}
}  // namespace

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
                // Apple Silicon 没有独立显存：GPU 和 CPU 共用同一块物理内存，能寻址到全部。
                // 这里以前报的是 totalMem / 4 —— 一个凭空的四分之一，被界面当成「显存」显示，
                // 也被 MCP 当成 VRAM 报给 AI。改成报真实总量，并打上 unified 标记，
                // 让上层知道这个数不是显存。
                data.dedicatedMemory = totalMem;
                data.isUnified = true;
            } else {
                data.dedicatedMemory = 0;
            }

            data.isVirtual = IsVirtualGpu(data.name);

            // Apple Silicon publishes the GPU core count on the accelerator
            // node itself; Intel Macs leave it absent (0 = unknown).
            data.coreCount = static_cast<int>(IORegistryUInt64(entry, "gpu-core-count"));

            // Read GPU utilization from PerformanceStatistics
            CFTypeRef perfStatsRef = IORegistryEntryCreateCFProperty(
                entry, CFSTR("PerformanceStatistics"), kCFAllocatorDefault, 0);
            if (perfStatsRef && CFGetTypeID(perfStatsRef) == CFDictionaryGetTypeID()) {
                CFDictionaryRef perfDict = static_cast<CFDictionaryRef>(perfStatsRef);
                auto numberValue = [&](CFStringRef key) -> int64_t {
                    CFNumberRef n = static_cast<CFNumberRef>(CFDictionaryGetValue(perfDict, key));
                    if (!n || CFGetTypeID(n) != CFNumberGetTypeID()) return 0;
                    int64_t v = 0;
                    return CFNumberGetValue(n, kCFNumberSInt64Type, &v) ? v : 0;
                };
                const int64_t utilVal = numberValue(CFSTR("Device Utilization %"));
                data.usage = static_cast<double>(utilVal);
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
        if (isAppleSilicon && totalMem > 0) {
            // 同上：统一内存报真实总量，不编四分之一
            data.dedicatedMemory = totalMem;
            data.isUnified = true;
        }
        gpuList.push_back(data);
    }

    Logger::Debug("GpuInfo: detected " + std::to_string(gpuList.size()) + " GPU(s)");
}

// Cached IOAccelerator service entries (file-static: GpuInfo is effectively
// a singleton on macOS). IOServiceGetMatchingServices enumeration is the
// expensive part of RefreshUsage; we enumerate once and keep the entries
// retained, reading PerformanceStatistics directly on later calls. Entries
// are released on re-enumeration and never leak for the process lifetime.
static std::vector<io_registry_entry_t> g_gpuEntries;

void GpuInfo::RefreshUsage() {
    if (gpuList.empty()) return;
    mach_port_t masterPort;
    if (@available(macOS 12.0, *)) {
        masterPort = kIOMainPortDefault;
    } else {
        masterPort = 0;
    }

    // (Re)enumerate only when we have no cached entries yet.
    if (g_gpuEntries.empty()) {
        io_iterator_t iter = 0;
        kern_return_t kr = IOServiceGetMatchingServices(
            masterPort,
            IOServiceMatching("IOAccelerator"),
            &iter);
        if (kr != KERN_SUCCESS) return;
        io_registry_entry_t entry;
        while ((entry = IOIteratorNext(iter)) != 0) {
            IOObjectRetain(entry);  // keep alive across RefreshUsage calls
            g_gpuEntries.push_back(entry);
        }
        IOObjectRelease(iter);
    }
    if (g_gpuEntries.empty()) return;

    size_t gpuIdx = 0;
    for (io_registry_entry_t entry : g_gpuEntries) {
        if (gpuIdx >= gpuList.size()) break;
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

    // Metal generation: refresh the cached profile (~60s) and attach the
    // value to every GPU whose chipset the profile names.
    if (g_metalState.load() == 0 && MetalNowMs() - g_metalStampMs.load() > 60000) {
        g_metalState.store(1);
        std::thread(RunDisplayProfiler).detach();
    }
    {
        std::lock_guard<std::mutex> lk(g_metalMutex);
        for (auto& gpu : gpuList) {
            if (gpu.name.empty() || !gpu.driver.empty()) continue;
            const std::string name(gpu.name.begin(), gpu.name.end());
            auto it = g_metalByChipset.find(name);
            if (it != g_metalByChipset.end()) gpu.driver = it->second;
        }
    }
}

const std::vector<GpuInfo::GpuData>& GpuInfo::GetGpuData() const { return gpuList; }

#elif defined(TCMT_LINUX)
// ======================== Linux Implementation ========================
#include "../Utils/Sysfs.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <sstream>
#include <utility>

namespace tcmt::linux {
namespace {

// ---------------------------------------------------------------------------
// NVML — minimal ABI subset, resolved at runtime.
// ---------------------------------------------------------------------------
using NvmlReturn = int;  // 0 == NVML_SUCCESS
using NvmlDevice = void*;

struct NvmlMemory {
    unsigned long long total;
    unsigned long long free;
    unsigned long long used;
};

struct NvmlUtilization {
    unsigned int gpu;
    unsigned int memory;
};

struct NvmlApi {
    void* handle = nullptr;
    NvmlReturn (*init)() = nullptr;
    NvmlReturn (*count)(unsigned int*) = nullptr;
    NvmlReturn (*handleByIndex)(unsigned int, NvmlDevice*) = nullptr;
    NvmlReturn (*name)(NvmlDevice, char*, unsigned int) = nullptr;
    NvmlReturn (*memoryInfo)(NvmlDevice, NvmlMemory*) = nullptr;
    NvmlReturn (*utilization)(NvmlDevice, NvmlUtilization*) = nullptr;
    NvmlReturn (*temperature)(NvmlDevice, int, unsigned int*) = nullptr;
    NvmlReturn (*clockInfo)(NvmlDevice, int, unsigned int*) = nullptr;
    NvmlReturn (*powerUsage)(NvmlDevice, unsigned int*) = nullptr;
    bool initialized = false;
};

template <typename T>
T LoadSymbol(void* handle, const char* name) {
    return reinterpret_cast<T>(::dlsym(handle, name));
}

NvmlApi& Nvml() {
    static NvmlApi api;
    static std::once_flag once;
    std::call_once(once, [] {
        api.handle = ::dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!api.handle) {
            api.handle = ::dlopen("libnvidia-ml.so", RTLD_NOW | RTLD_LOCAL);
        }
        if (!api.handle) {
            static std::atomic<bool> logged{false};
            LogOnce(logged,
                    "LinuxGpu: libnvidia-ml.so.1 not available; NVIDIA data via sysfs only");
            return;
        }
        api.init = LoadSymbol<NvmlReturn (*)()>(api.handle, "nvmlInit_v2");
        if (!api.init) api.init = LoadSymbol<NvmlReturn (*)()>(api.handle, "nvmlInit");
        api.count = LoadSymbol<NvmlReturn (*)(unsigned int*)>(api.handle, "nvmlDeviceGetCount_v2");
        if (!api.count) {
            api.count = LoadSymbol<NvmlReturn (*)(unsigned int*)>(api.handle,
                                                                  "nvmlDeviceGetCount");
        }
        auto getHandle = LoadSymbol<NvmlReturn (*)(unsigned int, NvmlDevice*)>(
            api.handle, "nvmlDeviceGetHandleByIndex_v2");
        if (!getHandle) {
            getHandle = LoadSymbol<NvmlReturn (*)(unsigned int, NvmlDevice*)>(
                api.handle, "nvmlDeviceGetHandleByIndex");
        }
        api.handleByIndex = getHandle;
        api.name = LoadSymbol<NvmlReturn (*)(NvmlDevice, char*, unsigned int)>(
            api.handle, "nvmlDeviceGetName");
        api.memoryInfo = LoadSymbol<NvmlReturn (*)(NvmlDevice, NvmlMemory*)>(
            api.handle, "nvmlDeviceGetMemoryInfo");
        api.utilization = LoadSymbol<NvmlReturn (*)(NvmlDevice, NvmlUtilization*)>(
            api.handle, "nvmlDeviceGetUtilizationRates");
        api.temperature = LoadSymbol<NvmlReturn (*)(NvmlDevice, int, unsigned int*)>(
            api.handle, "nvmlDeviceGetTemperature");
        api.clockInfo = LoadSymbol<NvmlReturn (*)(NvmlDevice, int, unsigned int*)>(
            api.handle, "nvmlDeviceGetClockInfo");
        api.powerUsage = LoadSymbol<NvmlReturn (*)(NvmlDevice, unsigned int*)>(
            api.handle, "nvmlDeviceGetPowerUsage");

        if (!api.init || !api.count || !api.handleByIndex) {
            ::dlclose(api.handle);
            api.handle = nullptr;
            return;
        }
        if (api.init() != 0) {
            static std::atomic<bool> logged{false};
            LogOnce(logged, "LinuxGpu: NVML init failed; NVIDIA data via sysfs only");
            return;
        }
        api.initialized = true;
    });
    return api;
}

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool ContainsAny(const std::string& haystack, const std::vector<std::string>& needles) {
    for (const auto& needle : needles) {
        if (haystack.find(needle) != std::string::npos) return true;
    }
    return false;
}

std::string PciId(unsigned int vendor, unsigned int device) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04x:%04x", vendor, device);
    return buf;
}

std::string VendorName(unsigned int vendor) {
    switch (vendor) {
        case 0x1002: return "AMD";
        case 0x10de: return "NVIDIA";
        case 0x8086: return "Intel";
        case 0x1af4: return "Red Hat/QEMU";
        case 0x15ad: return "VMware";
        case 0x80ee: return "Oracle VM";
        default: return "Unknown";
    }
}

std::string ReadUeventValue(const std::string& uevent, const std::string& key) {
    std::istringstream ss(uevent);
    std::string line;
    while (std::getline(ss, line)) {
        if (line.compare(0, key.size() + 1, key + "=") != 0) continue;
        return line.substr(key.size() + 1);
    }
    return {};
}

// Active clock from amdgpu's pp_dpm_sclk, e.g. "1: 2100Mhz *".
double ReadAmdActiveClockMHz(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) return 0.0;
    std::string line;
    while (std::getline(ifs, line)) {
        if (line.find('*') == std::string::npos) continue;
        // Some drivers print "Mhz", others "MHz".
        const std::string lower = Lower(line);
        const size_t mhz = lower.find("mhz");
        if (mhz == std::string::npos) continue;
        size_t begin = mhz;
        while (begin > 0 && (std::isdigit(static_cast<unsigned char>(line[begin - 1])) ||
                             line[begin - 1] == '.')) {
            --begin;
        }
        try {
            return std::stod(line.substr(begin, mhz - begin));
        } catch (...) {
            return 0.0;
        }
    }
    return 0.0;
}

// i915/xe often expose only a cumulative energy counter (energy1_input, µJ),
// not power1_average. Convert two samples into milliwatts.
bool ReadHwmonEnergyPower(const std::string& base, double& milliwatts) {
    uint64_t energyUj = 0;
    if (!ReadUInt64(base + "energy1_input", energyUj)) return false;

    struct EnergyState {
        std::mutex mutex;
        std::map<std::string, std::pair<uint64_t, int64_t>> previous;
    };
    static EnergyState state;

    const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
    std::lock_guard<std::mutex> lock(state.mutex);
    const auto it = state.previous.find(base);
    if (it == state.previous.end()) {
        state.previous[base] = {energyUj, nowUs};
        return false;
    }
    const uint64_t previousEnergy = it->second.first;
    const int64_t previousUs = it->second.second;
    if (energyUj < previousEnergy || nowUs - previousUs < 200000) return false;

    const double deltaUj = static_cast<double>(energyUj - previousEnergy);
    const double deltaUs = static_cast<double>(nowUs - previousUs);
    state.previous[base] = {energyUj, nowUs};
    if (deltaUs <= 0.0) return false;
    milliwatts = deltaUj * 1000.0 / deltaUs;  // µJ/µs == W
    return true;
}

// Reads temp1/power1/fan1/energy1 from a device hwmon directory. hwmon uses
// millidegrees Celsius and microwatts.
void ReadGpuHwmon(const std::string& hwmonRoot, GpuSnapshot& gpu, double& fanRpm) {
    for (const std::string& hw : ListDir(hwmonRoot, "hwmon")) {
        const std::string base = hwmonRoot + hw + "/";
        int64_t tempMilli = 0;
        if (!gpu.temperatureValid && ReadInt64(base + "temp1_input", tempMilli)) {
            const double c = static_cast<double>(tempMilli) / 1000.0;
            if (c > 0.0 && c <= 150.0) {
                gpu.temperatureC = c;
                gpu.temperatureValid = true;
            }
        }
        if (!gpu.powerValid) {
            uint64_t microwatts = 0;
            if (ReadUInt64(base + "power1_average", microwatts) ||
                ReadUInt64(base + "power1_input", microwatts)) {
                gpu.powerMilliwatts = static_cast<double>(microwatts) / 1000.0;
                gpu.powerValid = true;
            } else {
                double derivedMilliwatts = 0.0;
                if (ReadHwmonEnergyPower(base, derivedMilliwatts)) {
                    gpu.powerMilliwatts = derivedMilliwatts;
                    gpu.powerValid = true;
                }
            }
        }
        if (fanRpm <= 0.0) {
            uint64_t rpm = 0;
            if (ReadUInt64(base + "fan1_input", rpm) && rpm > 0 && rpm <= 100000) {
                fanRpm = static_cast<double>(rpm);
            }
        }
        int64_t freqHz = 0;
        if (gpu.coreClockMHz <= 0.0 && ReadInt64(base + "freq1_input", freqHz) &&
            freqHz > 0) {
            gpu.coreClockMHz = static_cast<double>(freqHz) / 1000000.0;
        }
    }
}

std::vector<GpuSnapshot> CollectNvidia() {
    std::vector<GpuSnapshot> gpus;
    NvmlApi& api = Nvml();
    if (!api.initialized) return gpus;

    unsigned int count = 0;
    if (api.count(&count) != 0) return gpus;

    for (unsigned int i = 0; i < count; ++i) {
        NvmlDevice device = nullptr;
        if (api.handleByIndex(i, &device) != 0 || !device) continue;

        GpuSnapshot gpu;
        gpu.brand = "NVIDIA";
        gpu.deviceId = "nvml:" + std::to_string(i);

        char name[96] = {};
        if (api.name && api.name(device, name, sizeof(name)) == 0) {
            gpu.name = Trim(name);
        }
        if (gpu.name.empty()) gpu.name = "NVIDIA GPU " + std::to_string(i);

        NvmlMemory memory{};
        if (api.memoryInfo && api.memoryInfo(device, &memory) == 0 && memory.total > 0) {
            gpu.memoryBytes = memory.total;
            gpu.memoryUsedBytes = memory.used;
            gpu.memoryPercent = 100.0 * static_cast<double>(memory.used) /
                                static_cast<double>(memory.total);
        }

        NvmlUtilization util{};
        if (api.utilization && api.utilization(device, &util) == 0) {
            gpu.usagePercent = static_cast<double>(util.gpu);
        }

        unsigned int temperature = 0;
        if (api.temperature && api.temperature(device, 0, &temperature) == 0 &&
            temperature > 0 && temperature <= 150) {
            gpu.temperatureC = static_cast<double>(temperature);
            gpu.temperatureValid = true;
        }

        unsigned int clockMHz = 0;
        if (api.clockInfo && api.clockInfo(device, 0, &clockMHz) == 0) {
            gpu.coreClockMHz = static_cast<double>(clockMHz);
        }

        unsigned int powerMw = 0;
        if (api.powerUsage && api.powerUsage(device, &powerMw) == 0) {
            gpu.powerMilliwatts = static_cast<double>(powerMw);
            gpu.powerValid = true;
        }

        const std::string lowerName = Lower(gpu.name);
        gpu.isVirtual = ContainsAny(lowerName, {"vgpu", "virtual", "grid"});

        gpus.push_back(std::move(gpu));
    }
    return gpus;
}

std::vector<GpuSnapshot> CollectDrm(bool nvmlHasDevices) {
    std::vector<GpuSnapshot> gpus;

    for (const std::string& card : ListDir("/sys/class/drm", "card")) {
        // Only "cardN"; "cardN-DP-1" connectors are not GPUs.
        if (card.size() <= 4) continue;
        bool digits = true;
        for (size_t i = 4; i < card.size(); ++i) {
            if (!std::isdigit(static_cast<unsigned char>(card[i]))) {
                digits = false;
                break;
            }
        }
        if (!digits) continue;

        const std::string dev = "/sys/class/drm/" + card + "/device/";
        unsigned int vendor = 0;
        if (!ReadHexUInt(dev + "vendor", vendor) || vendor == 0) continue;
        unsigned int device = 0;
        ReadHexUInt(dev + "device", device);

        // NVML already reported every NVIDIA device it can see; the DRM entry
        // would be a duplicate with less information.
        if (vendor == 0x10de && nvmlHasDevices) continue;

        GpuSnapshot gpu;
        gpu.deviceId = card;
        gpu.brand = VendorName(vendor);

        std::string product = ReadLine(dev + "product_name");
        if (product.empty()) {
            gpu.name = gpu.brand + " GPU (" + PciId(vendor, device) + ")";
        } else {
            gpu.name = product;
        }

        const std::string uevent = [&] {
            std::string text;
            ReadFile(dev + "uevent", text);
            return text;
        }();
        const std::string driver = ReadUeventValue(uevent, "DRIVER");
        const std::string lowerName = Lower(gpu.name);
        gpu.isVirtual = vendor == 0x1af4 || vendor == 0x15ad || vendor == 0x80ee ||
                        ContainsAny(Lower(driver), {"virtio", "vmwgfx", "vboxvideo"}) ||
                        ContainsAny(lowerName, {"virtual", "llvmpipe", "qemu"});

        ReadUInt64(dev + "mem_info_vram_total", gpu.memoryBytes);
        ReadUInt64(dev + "mem_info_vram_used", gpu.memoryUsedBytes);
        if (gpu.memoryBytes > 0) {
            gpu.memoryPercent = 100.0 * static_cast<double>(gpu.memoryUsedBytes) /
                                static_cast<double>(gpu.memoryBytes);
        }

        if (vendor == 0x1002) {
            uint64_t busy = 0;
            if (ReadUInt64(dev + "gpu_busy_percent", busy)) {
                gpu.usagePercent = static_cast<double>(std::min<uint64_t>(busy, 100));
            }
            gpu.coreClockMHz = ReadAmdActiveClockMHz(dev + "pp_dpm_sclk");
        } else if (vendor == 0x8086) {
            // i915 / xe expose current GT frequency, not a busy counter.
            // Reporting frequency/max as "usage" would be a fabricated number,
            // so usage stays 0 (unknown) for Intel.
            uint64_t mhz = 0;
            if (!ReadUInt64(dev + "gt_cur_freq_mhz", mhz)) {
                ReadUInt64(dev + "gt_act_freq_mhz", mhz);
            }
            if (mhz == 0) {
                ReadUInt64(dev + "tile0/gt0/freq0/cur_freq", mhz);
            }
            gpu.coreClockMHz = static_cast<double>(mhz);
            gpu.isIntegrated = gpu.memoryBytes == 0;
        } else if (vendor == 0x10de) {
            // nouveau/legacy path: no busy counters in sysfs.
            gpu.coreClockMHz = 0.0;
        }

        // Fans are surfaced by TemperatureWrapper's hwmon scan; the GPU
        // snapshot only carries data that reaches GpuSpec.
        double fanRpm = 0.0;
        ReadGpuHwmon(dev + "hwmon/", gpu, fanRpm);

        gpus.push_back(std::move(gpu));
    }
    return gpus;
}

}  // namespace

std::vector<GpuSnapshot> CollectGpus() {
    const std::vector<GpuSnapshot> nvidia = CollectNvidia();
    std::vector<GpuSnapshot> gpus = nvidia;
    std::vector<GpuSnapshot> drm = CollectDrm(!nvidia.empty());
    gpus.insert(gpus.end(), std::make_move_iterator(drm.begin()),
                std::make_move_iterator(drm.end()));

    std::stable_sort(gpus.begin(), gpus.end(),
                     [](const GpuSnapshot& a, const GpuSnapshot& b) {
        const int rankA = a.isVirtual ? 2 : (a.isIntegrated ? 1 : 0);
        const int rankB = b.isVirtual ? 2 : (b.isIntegrated ? 1 : 0);
        if (rankA != rankB) return rankA < rankB;
        if (a.memoryBytes != b.memoryBytes) return a.memoryBytes > b.memoryBytes;
        return a.name < b.name;
    });

    static std::atomic<bool> logged{false};
    if (!gpus.empty() && !logged.exchange(true)) {
        std::string line = "LinuxGpu: detected " + std::to_string(gpus.size()) + " GPU(s)";
        for (const auto& gpu : gpus) line += " [" + gpu.name + "]";
        Logger::Info(line);
    }
    return gpus;
}

}  // namespace tcmt::linux
#include "../Utils/WinUtils.h"
#include <algorithm>
#include <cwctype>

namespace {

std::wstring Widen(const std::string& s) {
    // sysfs/NVML names are UTF-8; use the shared converter instead of
    // byte-wise widening so non-ASCII board names survive.
    return WinUtils::Utf8ToWstring(s);
}

GpuInfo::GpuData ToGpuData(const tcmt::linux::GpuSnapshot& source) {
    GpuInfo::GpuData data;
    data.name = Widen(source.name);
    data.deviceId = Widen(source.deviceId);
    data.dedicatedMemory = source.memoryBytes;
    data.isUnified = source.isUnified;
    data.coreClock = source.coreClockMHz;
    data.isNvidia = source.brand == "NVIDIA";
    data.isIntegrated = source.isIntegrated;
    data.isVirtual = source.isVirtual;
    data.temperature = source.temperatureValid
                           ? static_cast<unsigned int>(source.temperatureC)
                           : 0U;
    data.usage = source.usagePercent;
    if (source.brand == "NVIDIA") {
        // /sys/module/nvidia/version holds the driver release ("550.54.14").
        std::ifstream f("/sys/module/nvidia/version");
        std::string v;
        if (std::getline(f, v)) {
            while (!v.empty() &&
                   (v.back() == '\n' || v.back() == '\r' || v.back() == ' ')) {
                v.pop_back();
            }
            if (!v.empty()) data.driver = v;
        }
    }
    return data;
}

}  // namespace

GpuInfo::GpuInfo() {
    DetectGpusViaSysfs();
}

GpuInfo::~GpuInfo() {
    Logger::Debug("GpuInfo: detection complete");
}

bool GpuInfo::IsVirtualGpu(const std::wstring& name) {
    std::wstring lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    const std::wstring keywords[] = {
        L"virtual", L"qemu", L"virtio", L"vmware", L"virtualbox",
        L"llvmpipe", L"software"
    };
    for (const auto& keyword : keywords) {
        if (lower.find(keyword) != std::wstring::npos) return true;
    }
    return false;
}

void GpuInfo::DetectGpusViaSysfs() {
    gpuList.clear();
    // Multi-GPU order is defined by LinuxGpu: discrete first (largest VRAM),
    // then integrated, then virtual. gpuList[0] is the primary GPU.
    for (const auto& gpu : tcmt::linux::CollectGpus()) {
        gpuList.push_back(ToGpuData(gpu));
    }
}

void GpuInfo::RefreshUsage() {
    const std::vector<tcmt::linux::GpuSnapshot> fresh = tcmt::linux::CollectGpus();

    // Rebuild rather than match by index: NVML and DRM both enumerate every
    // boot, and a hot-plugged card changes the list length.
    gpuList.clear();
    gpuList.reserve(fresh.size());
    for (const auto& gpu : fresh) {
        gpuList.push_back(ToGpuData(gpu));
    }
}

const std::vector<GpuInfo::GpuData>& GpuInfo::GetGpuData() const { return gpuList; }

#else
#error "Unsupported platform"
#endif
