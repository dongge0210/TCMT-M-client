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

    static bool IsAvailable();
};
#endif
