#pragma once

#ifdef TCMT_WINDOWS
#include <string>
#include <vector>

// SPD temperature reading via PawnIO + SMBus
struct DimmTempInfo {
    std::string name;       // e.g. "DDR5 DIMM #1"
    double temperature;     // Celsius, -1 if unavailable
};

class MemoryTempReader {
public:
    // Attempt to read all DIMM temperatures via PawnIO/SMBus.
    // Returns empty if PawnIO not installed or no DIMMs found.
    static std::vector<DimmTempInfo> ReadAll();

    // Check if PawnIO is available on this system
    static bool IsAvailable();
};
#endif // TCMT_WINDOWS
