#pragma once

#ifdef TCMT_WINDOWS
#include <string>
#include <vector>
#include <cstdint>

// IntelOOBMSM (Out-Of-Band Management Services Module) reader
// Works on Intel Core Ultra (Meteor Lake+) — probes PCI 00:0A.0
// Maps BAR0 and reads Intel Platform Monitoring Technology (PMT) telemetry.
// Provides PCH temperature and SoC uncore sensor data not available via MSR.
struct OobmsmTemp {
    std::string name;
    double temperature;
};

class IntelOobmsmReader {
public:
    static std::vector<OobmsmTemp> ReadAll();
    static bool IsAvailable();
};
#endif
