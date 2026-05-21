#pragma once

#include <cstdint>

struct PhysicalDiskSmartData;

// Reads SMART attributes from a physical disk via DeviceIoControl.
// Windows-only; no-op stub on other platforms.
class SmartReader {
public:
    // Populate smartData with temperature, health %, power-on hours,
    // wear leveling, and SMART support flag from \\.\PhysicalDrive{diskIndex}.
    // Returns true if any SMART data was read, false on failure.
    static bool Read(int diskIndex, PhysicalDiskSmartData& smartData);
};
