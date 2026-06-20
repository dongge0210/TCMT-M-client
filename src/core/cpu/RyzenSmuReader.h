#pragma once

#ifdef TCMT_WINDOWS
#include <string>
#include <vector>
#include <cstdint>

// RyzenSMU (System Management Unit) reader — AMD Ryzen Zen 1–6
// Accesses the SMU power management table via PCI config IO ports 0xC4/0xC8
// on the CPU host bridge (00:00.0). Provides CPU package power, SMU-reported
// temperatures, and various internal telemetry.
struct RyzenSensor {
    std::string name;
    double value;       // temperature (C), power (W), or raw value
    std::string unit;   // "C", "W", "A", "MHz", or empty for raw
};

class RyzenSmuReader {
public:
    static std::vector<RyzenSensor> ReadAll();
    static bool IsAvailable();
};
#endif
