#pragma once
#include <string>
#include <vector>
#include <cstdint>

// Charging state
enum class ChargingState {
    Unknown = 0,
    Charging,
    Discharging,
    Full,
    NotCharging
};

struct BatteryInfo {
    std::string name;
    uint32_t designCapacity = 0;        // mWh
    uint32_t fullChargeCapacity = 0;    // mWh
    uint32_t currentCapacity = 0;       // mWh
    uint32_t voltage = 0;               // mV
    uint32_t cycleCount = 0;
    uint32_t chargePercent = 0;         // 0-100
    double wearLevel = 0.0;             // 0.0-1.0 (1.0 = fully worn)
    ChargingState chargingState = ChargingState::Unknown;
    bool acOnline = false;
};

struct PowerInfo {
    std::string powerPlan;               // Active power plan name
    bool acOnline = false;               // AC power status
    std::vector<BatteryInfo> batteries;
    double systemWattage = 0.0;          // Total system power draw (if available)

    void Detect();
};

#ifdef TCMT_LINUX

#include <cstdint>
#include <string>
#include <vector>

namespace tcmt::linux {

// Everything found under one /sys/class/power_supply/BAT* directory.
// Values the kernel does not expose stay 0 / false; no defaults are invented.
struct BatteryDetail {
    bool present = false;
    std::string name;
    std::string status;        // raw "status" file (Charging/Discharging/...)

    int percent = -1;          // -1 when the kernel has no capacity reading
    bool charging = false;
    bool discharging = false;
    bool full = false;

    uint64_t energyNowUWh = 0;
    uint64_t energyFullUWh = 0;
    uint64_t energyDesignUWh = 0;
    uint64_t chargeNowUAh = 0;
    uint64_t chargeFullUAh = 0;
    uint64_t chargeDesignUAh = 0;
    uint64_t voltageUV = 0;
    uint64_t currentUA = 0;    // magnitude; sign is in currentNegative
    bool currentNegative = false;
    uint64_t powerUW = 0;
    int cycleCount = 0;

    bool temperatureValid = false;
    double temperatureC = 0.0;

    bool powerValid = false;
    double watts = 0.0;

    // Capacities normalised to mWh for BatteryInfo / SystemInfo. Charge-based
    // batteries are only converted when voltage_now is available; otherwise
    // these stay 0 rather than pretending mAh is mWh.
    uint32_t currentCapacityMWh = 0;
    uint32_t fullCapacityMWh = 0;
    uint32_t designCapacityMWh = 0;
};

std::vector<std::string> ListBatteryPaths();
BatteryDetail ReadBatteryDetail(const std::string& basePath);
BatteryDetail ReadPrimaryBattery();
bool ReadAcOnline();

// Package energy delta from /sys/class/powercap/*/energy_uj.
// Returns -1 when the interface is missing or unreadable (energy_uj is
// root-only by default since CVE-2020-8694). Never estimates from load.
double SampleCpuPackagePowerMilliwatts();

}  // namespace tcmt::linux

#endif  // TCMT_LINUX
