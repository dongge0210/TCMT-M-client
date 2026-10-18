// LinuxPower.cpp — battery/AC/RAPL collection for Linux.
//
// ABI notes:
//   power_supply energy_* is microwatt-hours, charge_* is microamp-hours,
//   voltage_now is microvolts, current_now is microamps, power_now is
//   microwatts, temp is tenths of a degree Celsius. Not every driver exposes
//   every file; absence is treated as unknown, never as zero.

#include "LinuxPower.h"
#include "LinuxSysfs.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

namespace tcmt::linux {
namespace {

std::string PowerSupplyPath(const std::string& name) {
    return "/sys/class/power_supply/" + name;
}

std::vector<std::string> ListSupplyNames() {
    return ListDir("/sys/class/power_supply");
}

uint64_t ReadU64OrZero(const std::string& path) {
    uint64_t value = 0;
    ReadUInt64(path, value);
    return value;
}

}  // namespace

std::vector<std::string> ListBatteryPaths() {
    std::vector<std::string> paths;
    for (const std::string& name : ListSupplyNames()) {
        const std::string base = PowerSupplyPath(name);
        if (ReadLine(base + "/type") != "Battery") continue;
        paths.push_back(base);
    }
    return paths;
}

BatteryDetail ReadBatteryDetail(const std::string& basePath) {
    BatteryDetail detail;
    detail.present = true;
    detail.name = basePath.substr(basePath.find_last_of('/') + 1);

    detail.status = ReadLine(basePath + "/status");
    if (detail.status == "Charging") detail.charging = true;
    else if (detail.status == "Discharging") detail.discharging = true;
    else if (detail.status == "Full") detail.full = true;

    int64_t capacity = 0;
    if (ReadInt64(basePath + "/capacity", capacity) && capacity >= 0 &&
        capacity <= 100) {
        detail.percent = static_cast<int>(capacity);
    }

    detail.energyNowUWh = ReadU64OrZero(basePath + "/energy_now");
    detail.energyFullUWh = ReadU64OrZero(basePath + "/energy_full");
    detail.energyDesignUWh = ReadU64OrZero(basePath + "/energy_full_design");
    detail.chargeNowUAh = ReadU64OrZero(basePath + "/charge_now");
    detail.chargeFullUAh = ReadU64OrZero(basePath + "/charge_full");
    detail.chargeDesignUAh = ReadU64OrZero(basePath + "/charge_full_design");
    detail.voltageUV = ReadU64OrZero(basePath + "/voltage_now");

    int64_t current = 0;
    if (ReadInt64(basePath + "/current_now", current)) {
        detail.currentNegative = current < 0;
        detail.currentUA = static_cast<uint64_t>(current < 0 ? -current : current);
    }

    detail.powerUW = ReadU64OrZero(basePath + "/power_now");
    int64_t cycles = 0;
    if (ReadInt64(basePath + "/cycle_count", cycles) && cycles > 0) {
        detail.cycleCount = static_cast<int>(cycles);
    }

    // temp is in tenths of a degree Celsius.
    int64_t tempTenths = 0;
    if (ReadInt64(basePath + "/temp", tempTenths)) {
        const double celsius = static_cast<double>(tempTenths) / 10.0;
        if (celsius > -40.0 && celsius < 150.0) {
            detail.temperatureC = celsius;
            detail.temperatureValid = true;
        }
    }

    // Capacity in mWh.
    if (detail.energyFullUWh > 0) {
        detail.currentCapacityMWh = static_cast<uint32_t>(detail.energyNowUWh / 1000ULL);
        detail.fullCapacityMWh = static_cast<uint32_t>(detail.energyFullUWh / 1000ULL);
        detail.designCapacityMWh = static_cast<uint32_t>(detail.energyDesignUWh / 1000ULL);
    } else if (detail.chargeFullUAh > 0 && detail.voltageUV > 0) {
        // µAh * µV = 1e-12 Wh; convert to mWh: /1e9.
        const auto toMWh = [](uint64_t chargeUAh, uint64_t voltageUV) -> uint32_t {
            const long double wh =
                static_cast<long double>(chargeUAh) * static_cast<long double>(voltageUV) /
                1.0e9L;
            return static_cast<uint32_t>(wh);
        };
        detail.currentCapacityMWh = toMWh(detail.chargeNowUAh, detail.voltageUV);
        detail.fullCapacityMWh = toMWh(detail.chargeFullUAh, detail.voltageUV);
        detail.designCapacityMWh = toMWh(detail.chargeDesignUAh, detail.voltageUV);
    }

    if (detail.percent < 0) {
        if (detail.energyFullUWh > 0) {
            detail.percent = static_cast<int>(std::min<uint64_t>(
                100, detail.energyNowUWh * 100 / detail.energyFullUWh));
        } else if (detail.chargeFullUAh > 0) {
            detail.percent = static_cast<int>(std::min<uint64_t>(
                100, detail.chargeNowUAh * 100 / detail.chargeFullUAh));
        }
    }

    if (detail.powerUW > 0) {
        detail.watts = static_cast<double>(detail.powerUW) / 1.0e6;
        detail.powerValid = true;
    } else if (detail.voltageUV > 0 && detail.currentUA > 0) {
        detail.watts = (static_cast<double>(detail.voltageUV) / 1.0e6) *
                       (static_cast<double>(detail.currentUA) / 1.0e6);
        detail.powerValid = true;
    }

    return detail;
}

BatteryDetail ReadPrimaryBattery() {
    const std::vector<std::string> batteries = ListBatteryPaths();
    if (batteries.empty()) return {};
    return ReadBatteryDetail(batteries.front());
}

bool ReadAcOnline() {
    for (const std::string& name : ListSupplyNames()) {
        const std::string base = PowerSupplyPath(name);
        if (ReadLine(base + "/type") == "Battery") continue;
        int64_t online = 0;
        if (ReadInt64(base + "/online", online) && online == 1) return true;
    }
    return false;
}

double SampleCpuPackagePowerMilliwatts() {
    struct State {
        std::mutex mutex;
        bool primed = false;
        int64_t prevUs = 0;
        std::map<std::string, uint64_t> prevEnergy;
        double lastMilliwatts = -1.0;
    };
    static State state;

    struct Package {
        std::string path;
        uint64_t energy = 0;
        uint64_t maxEnergy = 0;
    };
    std::vector<Package> packages;

    for (const std::string& entry : ListDir("/sys/class/powercap")) {
        const bool intel = entry.compare(0, 10, "intel-rapl") == 0;
        const bool amd = entry.compare(0, 8, "amd-rapl") == 0;
        if (!intel && !amd) continue;
        // "intel-rapl:0" is a package; "intel-rapl:0:0" is a subdomain.
        if (entry.find(':', entry.find(':') + 1) != std::string::npos) continue;

        const std::string base = "/sys/class/powercap/" + entry;
        const std::string name = ReadLine(base + "/name");
        if (name.compare(0, 7, "package") != 0) continue;

        Package package;
        package.path = base;
        if (!ReadUInt64(base + "/energy_uj", package.energy)) continue;
        ReadUInt64(base + "/max_energy_range_uj", package.maxEnergy);
        packages.push_back(std::move(package));
    }

    if (packages.empty()) {
        static std::atomic<bool> logged{false};
        LogOnce(logged,
                "LinuxPower: RAPL energy_uj unavailable (root-only by default); "
                "CPU power reported as unavailable");
        return -1.0;
    }

    const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();

    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.primed) {
        for (const auto& package : packages) state.prevEnergy[package.path] = package.energy;
        state.prevUs = nowUs;
        state.primed = true;
        return -1.0;  // no delta yet
    }

    // The power loop runs at >= 1 s; ignore re-entrant calls from the UI path.
    if (nowUs - state.prevUs < 900000) return state.lastMilliwatts;

    const uint64_t deltaUs = static_cast<uint64_t>(nowUs - state.prevUs);
    long double totalMicroJoules = 0.0L;
    for (const auto& package : packages) {
        const auto it = state.prevEnergy.find(package.path);
        const uint64_t prev = it == state.prevEnergy.end() ? package.energy : it->second;
        uint64_t delta = 0;
        if (package.energy >= prev) {
            delta = package.energy - prev;
        } else if (package.maxEnergy > 0) {
            // Counter wrapped.
            delta = package.maxEnergy - prev + package.energy;
        }
        totalMicroJoules += static_cast<long double>(delta);
        state.prevEnergy[package.path] = package.energy;
    }
    state.prevUs = nowUs;

    // µJ / µs == W; convert to mW.
    const double milliwatts =
        static_cast<double>(totalMicroJoules) * 1000.0 / static_cast<double>(deltaUs);
    state.lastMilliwatts = milliwatts;
    return milliwatts;
}

}  // namespace tcmt::linux
