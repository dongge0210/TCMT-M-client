// LinuxGpu.cpp — GPU enumeration for Linux.
//
// NVIDIA: libnvidia-ml.so.1 via dlopen (no build dependency).
// AMD:    /sys/class/drm/card*/device/{gpu_busy_percent,mem_info_vram_*,hwmon/*}
// Intel:  i915/xe sysfs + hwmon.
//
// Every read is best-effort; an absent file leaves the field at 0 and the
// caller treats 0 as "unknown".

#include "LinuxGpu.h"
#include "LinuxSysfs.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
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
