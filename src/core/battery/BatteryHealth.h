// BatteryHealth.h — macOS battery health monitoring via IOKit AppleSmartBattery
#pragma once
#include <string>

struct BatteryHealthData {
    int cycleCount = 0;
    int designCapacity = 0;      // mAh (original design)
    int maxCapacity = 0;         // mAh (current maximum, degraded)
    double healthPercent = 0.0;  // maxCapacity / designCapacity * 100
    double temperature = 0.0;    // Celsius
    bool isCharging = false;
    bool externalConnected = false;
    bool present = false;
    int amperage = 0;            // mA (positive=charging, negative=discharging)
    int voltage = 0;             // mV
};

class BatteryHealth {
public:
    void Detect();
    const BatteryHealthData& GetData() const { return data_; }
private:
    BatteryHealthData data_;
};
