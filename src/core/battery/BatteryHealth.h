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
    double chargerWatts = 0.0;   // connected charger rated wattage (0 = unknown)
    // AdapterDetails of the connected charger (all empty/0 when unplugged):
    // the marketing name ("30W USB-C Power Adapter"), the negotiated voltage
    // and current, and the identifiers the adapter reports.
    std::string chargerName;
    std::string chargerManufacturer;
    std::string chargerModel;
    std::string chargerSerial;
    int chargerVoltageMv = 0;    // negotiated adapter voltage
    int chargerCurrentMa = 0;    // negotiated adapter current
    bool chargerWireless = false;
};

class BatteryHealth {
public:
    void Detect();
    const BatteryHealthData& GetData() const { return data_; }
private:
    BatteryHealthData data_;
};
