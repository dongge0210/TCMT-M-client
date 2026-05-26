#pragma once

#ifdef TCMT_WINDOWS
#include <cstdint>

class CpuTempReader {
public:
    // Returns CPU package temperature in Celsius, or -1 if unavailable
    static double ReadPackageTemp();

    // Check if Intel MSR or AMD SMU module was loaded
    static bool IsAvailable();
};
#endif
