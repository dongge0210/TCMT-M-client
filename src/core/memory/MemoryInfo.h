#pragma once
#include <cstdint>
#include <string>

#ifdef TCMT_WINDOWS
#include <winsock2.h>
#include <windows.h>
#endif

class MemoryInfo {
public:
    MemoryInfo();
    uint64_t GetTotalPhysical() const;
    uint64_t GetAvailablePhysical() const;
    uint64_t GetTotalVirtual() const;

    // RAM frequency in MHz (e.g., 6400). Returns 0 if unknown.
    uint32_t GetRamSpeed() const;
    // DDR generation string (e.g., "DDR5", "LPDDR5", "Unified LPDDR"). Returns "Unknown" if unknown.
    std::string GetRamType() const;

private:
#ifdef TCMT_WINDOWS
    MEMORYSTATUSEX memStatus;
    uint32_t ramSpeed = 0;
    std::string ramType;
#elif defined(TCMT_MACOS)
    uint64_t totalPhysical;
    uint64_t availablePhysical;
    uint64_t totalVirtual;
    uint32_t ramSpeed = 0;
    std::string ramType;
#endif
};
