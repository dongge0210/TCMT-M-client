// ProcessMonitor.h — Windows top-process sampler (PID, memory, CPU%).
// Windows counterpart of ProcessTop (macOS). Uses Toolhelp32 + GetProcessTimes
// deltas for CPU% and working set for memory.

#pragma once

#ifdef TCMT_WINDOWS

#include "ProcessTop.h"

#include <chrono>
#include <unordered_map>
#include <vector>

class ProcessMonitor {
public:
    void Refresh();
    const std::vector<ProcessTopEntry>& GetTop() const { return entries_; }

private:
    struct ProcSample {
        uint64_t totalTime = 0;   // kernel+user, 100ns units
        uint64_t createTime = 0;  // process creation time, 100ns since 1601
    };

    std::unordered_map<unsigned long, ProcSample> prevSamples_;
    std::chrono::steady_clock::time_point prevTime_{};
    bool firstSample_ = true;
    std::vector<ProcessTopEntry> entries_;
};

#elif defined(TCMT_LINUX)

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

#endif  // TCMT_WINDOWS / TCMT_LINUX
