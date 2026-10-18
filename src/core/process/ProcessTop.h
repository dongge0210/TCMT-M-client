// ProcessTop.h — Top processes by memory (macOS)
#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <chrono>
#include <unordered_map>

#ifdef _WIN32
typedef int pid_t;
#endif

struct ProcessTopEntry {
    pid_t pid = 0;
    std::string name;
    uint64_t memoryBytes = 0;  // RSS
    double cpuPercent = 0.0;   // estimated from delta
    // Row context, filled for the top N only (see Refresh): the proc_taskinfo
    // call that loop already makes supplies threads and CPU time, and one
    // bsdinfo per row supplies state + user. Anything unreadable stays
    // empty / -1 and the list renders it as N/A.
    int threads = -1;          // -1 = unknown
    uint64_t cpuTimeMs = 0;    // user + system, milliseconds
    std::string state;         // short state word (idle/run/sleep/stop/zombie)
    std::string user;          // owning user name

    // Everything else NtQuerySystemInformation reports for a process. These
    // do not all have a TUI column yet; they are collected so the process
    // details view and IPC consumers do not have to re-query the kernel.
    int ppid = -1;
    int handles = -1;
    uint32_t sessionId = 0;
    int basePriority = 0;
    uint64_t privateBytes = 0;
    uint64_t virtualBytes = 0;
    uint64_t pageFaults = 0;
    uint64_t kernelTimeMs = 0;
    uint64_t userTimeMs = 0;
    uint64_t readBytes = 0;
    uint64_t writeBytes = 0;
    uint64_t readOps = 0;
    uint64_t writeOps = 0;
    uint64_t otherOps = 0;
    uint64_t createTimeMs = 0;  // Unix epoch, milliseconds
};

// Everything the details page wants about one pid. Fetched on demand — only
// while a details view is open, never as part of the top-N scan. Fields the
// system refuses to hand over stay empty (or -1) and the page renders them as
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
