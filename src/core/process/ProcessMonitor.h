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
        uint64_t totalTime = 0;  // kernel+user, 100ns units
    };

    std::unordered_map<unsigned long, ProcSample> prevSamples_;
    std::chrono::steady_clock::time_point prevTime_{};
    bool firstSample_ = true;
    std::vector<ProcessTopEntry> entries_;
};

#endif  // TCMT_WINDOWS
