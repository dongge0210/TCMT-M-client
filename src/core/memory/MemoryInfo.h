#pragma once
#include <cstdint>

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

private:
#ifdef TCMT_WINDOWS
    MEMORYSTATUSEX memStatus;
#elif defined(TCMT_MACOS)
    uint64_t totalPhysical;
    uint64_t availablePhysical;
    uint64_t totalVirtual;
#endif
};
