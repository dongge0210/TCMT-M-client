#pragma once

#ifdef TCMT_WINDOWS
#include <cstdint>
#include <string>
#include <vector>

struct CpuCoreTemp {
    std::string name;
    double temperature;
};

class CpuTempReader {
public:
    // Returns all core temperatures + package, or empty if unavailable
    static std::vector<CpuCoreTemp> ReadAll();

    // Die temperature of every physical core, one entry per core in
    // enumeration order (P cores first on hybrid parts). 0 marks a core whose
    // MSR read failed. Empty when PawnIO/MSR is unavailable.
    static std::vector<double> ReadPerCore();

    static bool IsAvailable();
};
#endif
