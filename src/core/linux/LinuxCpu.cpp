// LinuxCpu.cpp — CPU sampling for Linux.
//
// Sources:
//   /proc/stat                                  total + per-logical-CPU jiffies
//   /proc/cpuinfo                               model name, logical CPU count
//   /sys/devices/system/cpu/cpuN/topology/*     package/core id (physical cores)
//   /sys/devices/system/cpu/cpuN/cpufreq/scaling_cur_freq   kHz
//   /sys/class/hwmon/hwmon*/{name,tempN_input,tempN_label}  coretemp/k10temp
//   /sys/class/thermal/thermal_zone*/{type,temp}            fallback
//
// Nothing throws; every failure path returns "unknown" (0).

#include "LinuxCpu.h"
#include "LinuxSysfs.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <unistd.h>

namespace tcmt::linux {
namespace {

struct CpuTimes {
    uint64_t total = 0;
    uint64_t idle = 0;
};

struct SampleState {
    std::mutex mutex;
    bool primed = false;
    CpuTimes prevAggregate;
    std::vector<CpuTimes> prevCores;
    int64_t prevMs = 0;
    CpuSnapshot latest;
};

SampleState& State() {
    static SampleState state;
    return state;
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// /proc/stat cpu line: user nice system idle iowait irq softirq steal
// guest guest_nice. guest/guest_nice are already included in user/nice, and
// total only needs the first eight fields.
bool ParseCpuLine(const std::string& line, CpuTimes& out) {
    std::istringstream ss(line);
    std::string tag;
    if (!(ss >> tag)) return false;
    uint64_t v[10] = {};
    int n = 0;
    while (n < 10 && (ss >> v[n])) ++n;
    if (n < 4) return false;

    // iowait is idle time from the scheduler's point of view; tools such as
    // top/htop include it in idle.
    out.total = 0;
    for (int i = 0; i < n && i < 8; ++i) out.total += v[i];
    out.idle = v[3] + (n > 4 ? v[4] : 0);
    return out.total > 0;
}

double UsagePercent(const CpuTimes& now, const CpuTimes& prev) {
    if (now.total <= prev.total) return 0.0;
    const uint64_t totalDelta = now.total - prev.total;
    const uint64_t idleDelta = now.idle >= prev.idle ? now.idle - prev.idle : 0;
    if (totalDelta == 0) return 0.0;
    double pct = 100.0 * (1.0 - static_cast<double>(idleDelta) /
                                   static_cast<double>(totalDelta));
    if (pct < 0.0) pct = 0.0;
    if (pct > 100.0) pct = 100.0;
    return pct;
}

bool IsCpuHwmon(const std::string& name) {
    static const char* kNames[] = {
        "coretemp", "k10temp", "zenpower", "fam15h_power",
        "cpu_thermal", "soc_thermal", "cpu-thermal", "soc-thermal",
    };
    for (const char* n : kNames) {
        if (name == n) return true;
    }
    return false;
}

bool PlausibleTemp(double c) { return c > 0.0 && c <= 150.0; }

// Map from (physical package, core id) to the first logical CPU in that core,
// plus the per-package core order used by coretemp ("Core 0", "Core 1", ...).
struct CoreMap {
    std::map<long long, int> byPackageCore;
    std::map<uint64_t, std::vector<int>> orderedPackageCores;
};

CoreMap BuildCoreToCpuMap(int logicalCores) {
    CoreMap map;
    for (int i = 0; i < logicalCores; ++i) {
        const std::string base =
            "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/topology/";
        uint64_t pkg = 0, core = 0;
        if (!ReadUInt64(base + "physical_package_id", pkg)) continue;
        if (!ReadUInt64(base + "core_id", core)) continue;
        const long long key = static_cast<long long>(pkg) * 1000000LL +
                              static_cast<long long>(core);
        if (map.byPackageCore.find(key) != map.byPackageCore.end()) continue;
        map.byPackageCore[key] = i;
        // coretemp numbers cores in ascending logical-CPU order within a
        // package, skipping siblings.
        map.orderedPackageCores[pkg].push_back(i);
    }
    return map;
}

void ReadCpuTemperatures(CpuSnapshot& snap) {
    const int n = snap.logicalCores;
    snap.coreTempC.assign(static_cast<size_t>(n), 0.0);

    const CoreMap coreToCpu = BuildCoreToCpuMap(n);
    bool havePackage = false;
    double packageTemp = 0.0;

    for (const std::string& hw : ListDir("/sys/class/hwmon", "hwmon")) {
        const std::string base = "/sys/class/hwmon/" + hw;
        const std::string name = ReadLine(base + "/name");
        if (!IsCpuHwmon(name)) continue;

        std::map<int, double> inputs;
        std::map<int, std::string> labels;
        for (const std::string& file : ListDir(base)) {
            if (file.compare(0, 4, "temp") != 0) continue;
            const size_t underscore = file.find('_', 4);
            if (underscore == std::string::npos) continue;
            int index = 0;
            try {
                index = std::stoi(file.substr(4, underscore - 4));
            } catch (...) {
                continue;
            }
            const std::string suffix = file.substr(underscore);
            if (suffix == "_input") {
                int64_t milli = 0;
                if (ReadInt64(base + "/" + file, milli)) {
                    const double celsius = static_cast<double>(milli) / 1000.0;
                    if (PlausibleTemp(celsius)) inputs[index] = celsius;
                }
            } else if (suffix == "_label") {
                labels[index] = ReadLine(base + "/" + file);
            }
        }

        // coretemp names its package sensor "Package id N"; the Core N labels
        // in the same hwmon device belong to that package.
        int packageId = 0;
        for (const auto& [index, label] : labels) {
            if (label.compare(0, 11, "Package id ") == 0) {
                try {
                    packageId = std::stoi(label.substr(11));
                } catch (...) {
                    packageId = 0;
                }
                break;
            }
        }

        for (const auto& [index, celsius] : inputs) {
            const auto lit = labels.find(index);
            const std::string label = lit == labels.end() ? std::string() : lit->second;

            if (label.compare(0, 11, "Package id ") == 0 ||
                label == "Tctl" || label == "Tdie" || label == "Tccd1" ||
                label == "Tccd2" || label == "Tccd3" || label == "Tccd4" ||
                (label.empty() && name != "coretemp")) {
                // "TccdN" is a CCD, not a core; only Tctl/Tdie drive the
                // package number. Multiple candidates: take the hottest.
                if (label.rfind("Tccd", 0) == 0 && label != "Tccd1") continue;
                if (!havePackage || celsius > packageTemp) packageTemp = celsius;
                havePackage = true;
                continue;
            }

            int cpuIndex = -1;
            if (name == "coretemp" && label.compare(0, 5, "Core ") == 0) {
                int coreId = -1;
                try {
                    coreId = std::stoi(label.substr(5));
                } catch (...) {
                    coreId = -1;
                }
                if (coreId >= 0) {
                    // coretemp numbers cores in the package's CPU-mask order,
                    // which is what orderedPackageCores represents.
                    const auto ordered =
                        coreToCpu.orderedPackageCores.find(static_cast<uint64_t>(packageId));
                    if (ordered != coreToCpu.orderedPackageCores.end() &&
                        coreId < static_cast<int>(ordered->second.size())) {
                        cpuIndex = ordered->second[static_cast<size_t>(coreId)];
                    } else {
                        auto it = coreToCpu.byPackageCore.find(
                            static_cast<long long>(packageId) * 1000000LL + coreId);
                        if (it == coreToCpu.byPackageCore.end()) {
                            it = coreToCpu.byPackageCore.find(coreId);  // single package
                        }
                        if (it != coreToCpu.byPackageCore.end()) cpuIndex = it->second;
                        else if (coreId < n) cpuIndex = coreId;
                    }
                }
            } else if (name == "zenpower" && label.size() >= 2 && label[0] == 'C' &&
                       std::isdigit(static_cast<unsigned char>(label[1]))) {
                try {
                    cpuIndex = std::stoi(label.substr(1));
                } catch (...) {
                    cpuIndex = -1;
                }
                if (cpuIndex >= n) cpuIndex = -1;
            }

            if (cpuIndex >= 0 && cpuIndex < n && snap.coreTempC[cpuIndex] <= 0.0) {
                snap.coreTempC[cpuIndex] = celsius;
            }
        }
    }

    // Fallback: ACPI thermal zones with a CPU-ish type.
    if (!havePackage) {
        for (const std::string& zone : ListDir("/sys/class/thermal", "thermal_zone")) {
            const std::string base = "/sys/class/thermal/" + zone;
            std::string type = ReadLine(base + "/type");
            std::transform(type.begin(), type.end(), type.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            const bool cpuLike = type.find("x86_pkg_temp") != std::string::npos ||
                                 type.find("cpu") != std::string::npos ||
                                 type.find("soc") != std::string::npos ||
                                 type.find("package") != std::string::npos;
            if (!cpuLike) continue;
            int64_t milli = 0;
            if (!ReadInt64(base + "/temp", milli)) continue;
            const double celsius = static_cast<double>(milli) / 1000.0;
            if (!PlausibleTemp(celsius)) continue;
            if (!havePackage || celsius > packageTemp) packageTemp = celsius;
            havePackage = true;
        }
    }

    snap.hasPackageTemp = havePackage;
    snap.packageTempC = havePackage ? packageTemp : 0.0;
}

}  // namespace

CpuSnapshot SampleCpu() {
    CpuSnapshot snap;

    std::ifstream stat("/proc/stat");
    if (!stat.is_open()) {
        static std::atomic<bool> logged{false};
        LogOnce(logged, "LinuxCpu: /proc/stat unreadable; CPU usage unavailable");
        return GetLatestCpu();
    }

    CpuTimes aggregate;
    bool haveAggregate = false;
    std::vector<CpuTimes> cores;
    std::string line;
    while (std::getline(stat, line)) {
        if (line.compare(0, 3, "cpu") != 0) break;  // cpu lines come first
        if (line.size() > 4 && std::isdigit(static_cast<unsigned char>(line[3]))) {
            int index = 0;
            try {
                index = std::stoi(line.substr(3, line.find(' ') - 3));
            } catch (...) {
                continue;
            }
            if (index < 0) continue;
            CpuTimes times;
            if (!ParseCpuLine(line, times)) continue;
            if (static_cast<size_t>(index) >= cores.size()) {
                cores.resize(static_cast<size_t>(index) + 1);
            }
            cores[static_cast<size_t>(index)] = times;
        } else {
            haveAggregate = ParseCpuLine(line, aggregate);
        }
    }

    snap.logicalCores = static_cast<int>(cores.size());
    if (!haveAggregate) {
        for (const auto& times : cores) {
            aggregate.total += times.total;
            aggregate.idle += times.idle;
        }
    }

    const int64_t now = NowMs();
    const int n = snap.logicalCores;
    snap.coreUsage.assign(static_cast<size_t>(n), 0.0);
    snap.coreFreqMHz.assign(static_cast<size_t>(n), 0.0);

    {
        SampleState& state = State();
        std::lock_guard<std::mutex> lock(state.mutex);

        if (state.primed) {
            snap.totalUsage = UsagePercent(aggregate, state.prevAggregate);
            if (state.prevCores.size() == cores.size()) {
                for (int i = 0; i < n; ++i) {
                    snap.coreUsage[static_cast<size_t>(i)] =
                        UsagePercent(cores[static_cast<size_t>(i)],
                                     state.prevCores[static_cast<size_t>(i)]);
                }
            }
            snap.intervalMs = static_cast<double>(now - state.prevMs);
        }

        state.prevAggregate = aggregate;
        state.prevCores = cores;
        state.prevMs = now;
        state.primed = true;
    }

    for (int i = 0; i < n; ++i) {
        const std::string path = "/sys/devices/system/cpu/cpu" +
                                 std::to_string(i) +
                                 "/cpufreq/scaling_cur_freq";
        uint64_t khz = 0;
        if (ReadUInt64(path, khz) && khz > 0) {
            snap.coreFreqMHz[static_cast<size_t>(i)] =
                static_cast<double>(khz) / 1000.0;
        }
    }

    ReadCpuTemperatures(snap);

    {
        SampleState& state = State();
        std::lock_guard<std::mutex> lock(state.mutex);
        state.latest = snap;
    }
    return snap;
}

CpuSnapshot GetLatestCpu() {
    SampleState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.latest;
}

int ReadLogicalCpuCount() {
    std::ifstream cpuinfo("/proc/cpuinfo");
    int count = 0;
    if (cpuinfo.is_open()) {
        std::string line;
        while (std::getline(cpuinfo, line)) {
            if (line.compare(0, 9, "processor") == 0) ++count;
        }
    }
    if (count > 0) return count;
    const long online = ::sysconf(_SC_NPROCESSORS_ONLN);
    return online > 0 ? static_cast<int>(online) : 1;
}

int ReadPhysicalCoreCount() {
    const int logical = ReadLogicalCpuCount();
    std::map<long long, bool> seen;
    for (int i = 0; i < logical; ++i) {
        const std::string base =
            "/sys/devices/system/cpu/cpu" + std::to_string(i) + "/topology/";
        uint64_t pkg = 0, core = 0;
        if (!ReadUInt64(base + "physical_package_id", pkg)) continue;
        if (!ReadUInt64(base + "core_id", core)) continue;
        seen[static_cast<long long>(pkg) * 1000000LL + static_cast<long long>(core)] = true;
    }
    if (!seen.empty()) return static_cast<int>(seen.size());

    // ARM/MIPS kernels may not expose topology. /proc/cpuinfo sometimes has a
    // "cpu cores" line; otherwise logical == physical is the least wrong answer.
    std::ifstream cpuinfo("/proc/cpuinfo");
    if (cpuinfo.is_open()) {
        std::string line;
        int cores = 0;
        while (std::getline(cpuinfo, line)) {
            const size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            if (Trim(line.substr(0, colon)) != "cpu cores") continue;
            try {
                cores = std::max(cores, std::stoi(Trim(line.substr(colon + 1))));
            } catch (...) {
            }
        }
        if (cores > 0) return cores;
    }
    return logical;
}

std::string ReadCpuModel() {
    std::ifstream cpuinfo("/proc/cpuinfo");
    if (cpuinfo.is_open()) {
        std::string line;
        while (std::getline(cpuinfo, line)) {
            const size_t colon = line.find(':');
            if (colon == std::string::npos) continue;
            const std::string key = Trim(line.substr(0, colon));
            if (key != "model name" && key != "Model" && key != "Hardware" &&
                key != "cpu model") {
                continue;
            }
            const std::string value = Trim(line.substr(colon + 1));
            if (!value.empty()) return value;
        }
    }

    // ARM boards often only report the model through the device tree.
    std::string dt;
    if (ReadFile("/sys/firmware/devicetree/base/model", dt)) {
        const size_t nul = dt.find('\0');
        if (nul != std::string::npos) dt.resize(nul);
        dt = Trim(dt);
        if (!dt.empty()) return dt;
    }
    return "Unknown CPU";
}

}  // namespace tcmt::linux
