// ProcessTop.h — Top processes by memory (macOS)
#pragma once
#include <string>
#include <vector>
#include <chrono>
#include <unordered_map>

struct ProcessTopEntry {
    pid_t pid = 0;
    std::string name;
    uint64_t memoryBytes = 0;  // RSS
    double cpuPercent = 0.0;   // estimated from delta
};

class ProcessTop {
public:
    void Refresh();
    const std::vector<ProcessTopEntry>& GetTop() const { return entries_; }

private:
    std::vector<ProcessTopEntry> entries_;

    // Per-process CPU time tracking for delta%
    struct ProcSample {
        uint64_t totalUser = 0;    // mach absolute time units
        uint64_t totalSystem = 0;
    };
    std::unordered_map<pid_t, ProcSample> prevSamples_;
    std::chrono::steady_clock::time_point prevTime_{};
    bool firstSample_ = true;
    double timebase_ = 0.0;  // mach_timebase conversion factor
};
