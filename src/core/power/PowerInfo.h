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
