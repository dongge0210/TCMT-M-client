// LinuxProcess.h — top-process sampling from /proc.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tcmt::linux {

struct ProcessSample {
    int32_t pid = 0;
    std::string name;
    uint64_t memoryBytes = 0;
    double cpuPercent = 0.0;

    // Extra row context for the TUI. Unknown stays -1 / empty.
    int threads = -1;
    uint64_t cpuTimeMs = 0;
    std::string state;
    std::string user;
};

// Returns up to maxCount processes, highest CPU first (memory when no CPU
// delta is available yet, i.e. on the first call).
// Processes that cannot be read (hidepid, other users) are skipped silently.
std::vector<ProcessSample> CollectTopProcesses(int maxCount = 12);

}  // namespace tcmt::linux
