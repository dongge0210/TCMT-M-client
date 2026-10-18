// LinuxDisk.cpp — disk IO counters and temperature lookup for Linux.
//
// /proc/diskstats field layout (after the device name):
//   0 reads completed      1 reads merged      2 sectors read
//   3 ms reading           4 writes completed  5 writes merged
//   6 sectors written      7 ms writing        8 IOs in progress
//   9 ms doing IO         10 weighted ms
// Sector counts are always 512-byte units in this interface.

#include "LinuxDisk.h"
#include "LinuxSysfs.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>

namespace tcmt::linux {
namespace {

struct DiskCounters {
    uint64_t readsCompleted = 0;
    uint64_t sectorsRead = 0;
    uint64_t writesCompleted = 0;
    uint64_t sectorsWritten = 0;
};

struct StatsState {
    std::mutex mutex;
    int64_t prevUs = 0;
    int64_t lastComputeUs = 0;
    std::map<std::string, DiskCounters> prev;
    std::map<std::string, DiskIoDelta> last;
};

StatsState& State() {
    static StatsState state;
    return state;
}

int64_t NowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool StartsWith(const std::string& s, const char* prefix) {
    const size_t n = std::strlen(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

bool AllDigits(const std::string& s, size_t from) {
    if (from >= s.size()) return false;
    for (size_t i = from; i < s.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

bool AllLetters(const std::string& s, size_t from) {
    if (from >= s.size()) return false;
    for (size_t i = from; i < s.size(); ++i) {
        if (!std::islower(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

std::map<std::string, DiskCounters> ReadDiskstats() {
    std::map<std::string, DiskCounters> result;
    std::ifstream ifs("/proc/diskstats");
    if (!ifs.is_open()) return result;

    std::string line;
    while (std::getline(ifs, line)) {
        std::istringstream ss(line);
        uint64_t major = 0, minor = 0;
        std::string name;
        if (!(ss >> major >> minor >> name)) continue;
        uint64_t values[11] = {};
        int count = 0;
        while (count < 11 && (ss >> values[count])) ++count;
        if (count < 7) continue;

        DiskCounters counters;
        counters.readsCompleted = values[0];
        counters.sectorsRead = values[2];
        counters.writesCompleted = values[4];
        counters.sectorsWritten = values[6];
        result[name] = counters;
    }
    return result;
}

double ReadMillidegreeCelsius(const std::string& path) {
    int64_t milli = 0;
    if (!ReadInt64(path, milli)) return 0.0;
    const double celsius = static_cast<double>(milli) / 1000.0;
    return celsius > 0.0 && celsius <= 150.0 ? celsius : 0.0;
}

std::string RealPath(const std::string& path) {
    char buffer[PATH_MAX] = {};
    if (!::realpath(path.c_str(), buffer)) return {};
    return std::string(buffer);
}

}  // namespace

bool IsWholeDisk(const std::string& name) {
    if (StartsWith(name, "nvme")) {
        // nvme0n1; partitions are nvme0n1p1 and never appear in /sys/block.
        const size_t n = name.find('n', 4);
        if (n == std::string::npos || n == 4) return false;
        return AllDigits(name.substr(4, n - 4), 0) && AllDigits(name, n + 1);
    }
    if (StartsWith(name, "mmcblk")) {
        return AllDigits(name, 6);
    }
    if (StartsWith(name, "sd") || StartsWith(name, "vd") || StartsWith(name, "hd") ||
        StartsWith(name, "xvd")) {
        const size_t prefix = StartsWith(name, "xvd") ? 3 : 2;
        return AllLetters(name, prefix);
    }
    return false;
}

std::vector<std::string> ListPhysicalDisks() {
    std::vector<std::string> disks;
    for (const std::string& name : ListDir("/sys/block")) {
        if (IsWholeDisk(name)) disks.push_back(name);
    }
    return disks;
}

std::map<std::string, DiskIoDelta> SampleDiskStats() {
    const auto counters = ReadDiskstats();
    const int64_t nowUs = NowUs();

    StatsState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);

    // Multiple disks are collected in one burst; only the first call of a
    // burst may compute a delta, otherwise the later calls see a ~0 interval.
    if (state.prevUs != 0 && state.lastComputeUs != 0 &&
        nowUs - state.lastComputeUs < 1000000) {
        return state.last;
    }

    std::map<std::string, DiskIoDelta> deltas;
    if (state.prevUs != 0 && nowUs > state.prevUs) {
        const double seconds = static_cast<double>(nowUs - state.prevUs) / 1.0e6;
        for (const auto& [name, current] : counters) {
            if (!IsWholeDisk(name)) continue;
            const auto it = state.prev.find(name);
            if (it == state.prev.end()) continue;
            const DiskCounters& prev = it->second;
            if (current.sectorsRead < prev.sectorsRead ||
                current.sectorsWritten < prev.sectorsWritten ||
                current.readsCompleted < prev.readsCompleted ||
                current.writesCompleted < prev.writesCompleted) {
                continue;  // counter reset (hotplug / overflow)
            }

            DiskIoDelta delta;
            delta.seconds = seconds;
            delta.readBytesPerSec = static_cast<uint64_t>(
                static_cast<double>((current.sectorsRead - prev.sectorsRead) * 512ULL) /
                seconds);
            delta.writeBytesPerSec = static_cast<uint64_t>(
                static_cast<double>((current.sectorsWritten - prev.sectorsWritten) * 512ULL) /
                seconds);
            delta.readIops = static_cast<uint64_t>(
                static_cast<double>(current.readsCompleted - prev.readsCompleted) / seconds);
            delta.writeIops = static_cast<uint64_t>(
                static_cast<double>(current.writesCompleted - prev.writesCompleted) / seconds);
            deltas[name] = delta;
        }
    }

    state.prev = counters;
    state.prevUs = nowUs;
    state.lastComputeUs = nowUs;
    state.last = deltas;
    return deltas;
}

bool ReadDiskTemperatureC(const std::string& device, double& celsius) {
    // NVMe hwmon is registered on the controller (nvme0), not on the
    // namespace (nvme0n1). Depending on kernel and driver, the namespace's
    // "device" symlink points at either one, so try every known location.
    std::vector<std::string> hwmonRoots;
    hwmonRoots.push_back("/sys/block/" + device + "/device/hwmon/");
    // Some kernels link the namespace's "device" at the namespace itself, so
    // the controller is reached through a second "device" hop.
    hwmonRoots.push_back("/sys/block/" + device + "/device/device/hwmon/");

    std::string controllerName;
    if (StartsWith(device, "nvme")) {
        const size_t n = device.find('n', 4);
        if (n != std::string::npos && n > 4) controllerName = device.substr(0, n);
    }
    if (!controllerName.empty()) {
        hwmonRoots.push_back("/sys/class/nvme/" + controllerName + "/hwmon/");
        hwmonRoots.push_back("/sys/class/nvme/" + controllerName + "/device/hwmon/");
    }

    for (const std::string& root : hwmonRoots) {
        for (const std::string& hw : ListDir(root, "hwmon")) {
            const double value = ReadMillidegreeCelsius(root + hw + "/temp1_input");
            if (value > 0.0) {
                celsius = value;
                return true;
            }
        }
    }

    // SATA/SAS: drivetemp registers a hwmon under the SCSI device. NVMe is
    // also matched here for kernels where the namespace/controller symlinks
    // differ from the paths above.
    const std::string deviceReal = RealPath("/sys/block/" + device + "/device");
    const std::string namespaceReal = RealPath("/sys/block/" + device);
    std::string controllerReal;
    if (!namespaceReal.empty()) {
        const size_t slash = namespaceReal.find_last_of('/');
        if (slash != std::string::npos) controllerReal = namespaceReal.substr(0, slash);
    }

    for (const std::string& hw : ListDir("/sys/class/hwmon", "hwmon")) {
        const std::string base = "/sys/class/hwmon/" + hw;
        const std::string name = ReadLine(base + "/name");
        if (name != "drivetemp" && name != "nvme") continue;

        bool matches = false;
        if (PathExists(base + "/device/block/" + device)) {
            matches = true;
        } else {
            const std::string hwmonDevice = RealPath(base + "/device");
            if (!hwmonDevice.empty()) {
                matches = hwmonDevice == deviceReal ||
                          (!controllerReal.empty() && hwmonDevice == controllerReal);
            }
        }
        if (!matches) continue;

        const double value = ReadMillidegreeCelsius(base + "/temp1_input");
        if (value > 0.0) {
            celsius = value;
            return true;
        }
    }
    return false;
}

std::string ReadDiskInterfaceType(const std::string& device) {
    if (StartsWith(device, "nvme")) return "NVMe";
    if (StartsWith(device, "mmcblk")) return "SD/MMC";
    if (StartsWith(device, "vd")) return "VirtIO";
    if (StartsWith(device, "xvd")) return "Xen";
    if (StartsWith(device, "hd")) return "IDE";

    if (StartsWith(device, "sd")) {
        std::string target = RealPath("/sys/block/" + device + "/device");
        std::transform(target.begin(), target.end(), target.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (target.find("/usb") != std::string::npos) return "USB";
        if (target.find("virtio") != std::string::npos) return "VirtIO";
        if (target.find("sas") != std::string::npos) return "SAS";
        if (target.find("/ata") != std::string::npos ||
            target.find("sata") != std::string::npos) {
            return "SATA";
        }
        return "SCSI";
    }
    return "Unknown";
}

}  // namespace tcmt::linux
