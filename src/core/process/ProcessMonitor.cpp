#ifdef TCMT_WINDOWS

#include "ProcessMonitor.h"

#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>

#include <algorithm>
#include <unordered_set>

void ProcessMonitor::Refresh() {
    const auto now = std::chrono::steady_clock::now();
    const double dtSec = firstSample_
                             ? 0.0
                             : std::chrono::duration<double>(now - prevTime_).count();
    prevTime_ = now;

    SYSTEM_INFO si = {};
    GetSystemInfo(&si);
    const unsigned int coreCount = (std::max)(1u, static_cast<unsigned int>(si.dwNumberOfProcessors));

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return;
    }

    std::vector<ProcessTopEntry> collected;
    std::unordered_set<unsigned long> livePids;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        livePids.insert(pe.th32ProcessID);

        HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!hProc) {
            continue;
        }

        double cpuPct = 0.0;
        FILETIME create{}, exit{}, kernel{}, user{};
        if (GetProcessTimes(hProc, &create, &exit, &kernel, &user)) {
            ULARGE_INTEGER k, u;
            k.LowPart = kernel.dwLowDateTime;
            k.HighPart = kernel.dwHighDateTime;
            u.LowPart = user.dwLowDateTime;
            u.HighPart = user.dwHighDateTime;
            const uint64_t total = k.QuadPart + u.QuadPart;  // 100ns units
            const auto it = prevSamples_.find(pe.th32ProcessID);
            if (!firstSample_ && it != prevSamples_.end() && dtSec > 0.0) {
                const uint64_t delta = total - it->second.totalTime;
                // delta[100ns] -> seconds, then percent of one core, then /coreCount
                cpuPct = static_cast<double>(delta) * 1e-7 / dtSec / coreCount * 100.0;
                if (cpuPct < 0.0) cpuPct = 0.0;
            }
            prevSamples_[pe.th32ProcessID] = ProcSample{total};
        }

        PROCESS_MEMORY_COUNTERS pmc = {};
        uint64_t mem = 0;
        if (K32GetProcessMemoryInfo(hProc, &pmc, sizeof(pmc))) {
            mem = pmc.WorkingSetSize;
        }
        CloseHandle(hProc);

        ProcessTopEntry e;
        e.pid = static_cast<pid_t>(pe.th32ProcessID);
        e.memoryBytes = mem;
        e.cpuPercent = cpuPct;
        char nameBuf[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1,
                            nameBuf, static_cast<int>(sizeof(nameBuf)) - 1, nullptr, nullptr);
        e.name = nameBuf;
        collected.push_back(std::move(e));
    }
    CloseHandle(snap);

    // Drop CPU-time tracking for processes that no longer exist.
    for (auto it = prevSamples_.begin(); it != prevSamples_.end();) {
        if (!livePids.count(it->first)) {
            it = prevSamples_.erase(it);
        } else {
            ++it;
        }
    }

    // Top by memory, capped to the number of rows the TUI shows.
    std::sort(collected.begin(), collected.end(), [](const ProcessTopEntry& a, const ProcessTopEntry& b) {
        return a.memoryBytes > b.memoryBytes;
    });
    const size_t kTop = 8;
    if (collected.size() > kTop) {
        collected.resize(kTop);
    }

    entries_ = std::move(collected);
    firstSample_ = false;
}

#endif  // TCMT_WINDOWS
