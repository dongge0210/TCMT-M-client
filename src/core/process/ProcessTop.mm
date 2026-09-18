// ProcessTop.mm — macOS top processes via proc_listallpids + proc_pidinfo
#include "ProcessTop.h"
#include "../Utils/Logger.h"
#include <cstring>
#include <pwd.h>

#import <libproc.h>
#import <mach/mach_time.h>
#import <mach/mach.h>

// The state word shown next to each process in the list, straight from
// pbi_status. It stays English by design — status short words are part of the
// label layer, like stale/FOLLOW/N/A.
static const char* ProcStateWord(uint32_t status) {
    switch (status) {
        case 1:  return "idle";    // SIDL
        case 2:  return "run";     // SRUN
        case 3:  return "sleep";   // SSLEEP
        case 4:  return "stop";    // SSTOP
        case 5:  return "zombie";  // SZOMB
        default: return "";
    }
}

void ProcessTop::Refresh() {
    // Mach timebase (numerator/denominator for converting to nanoseconds)
    if (timebase_ == 0.0) {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        timebase_ = (double)tb.numer / (double)tb.denom;
    }

    auto now = std::chrono::steady_clock::now();
    double wallNs = 0.0;
    if (!firstSample_) {
        auto delta = now - prevTime_;
        wallNs = std::chrono::duration_cast<std::chrono::nanoseconds>(delta).count();
    }

    // Collect all PIDs
    pid_t pids[4096];
    int count = proc_listallpids(pids, sizeof(pids));
    if (count <= 0) return;

    std::vector<std::pair<pid_t, uint64_t>> byMem;  // pid, memory
    std::unordered_map<pid_t, uint64_t> memMap;
    std::unordered_map<pid_t, uint64_t> cpuTimeMap;  // total (user+system) in ns

    for (int i = 0; i < count; i++) {
        // Read task info
        proc_taskinfo taskInfo;
        int ret = proc_pidinfo(pids[i], PROC_PIDTASKINFO, 0,
                               &taskInfo, sizeof(taskInfo));
        if (ret != sizeof(taskInfo)) continue;

        uint64_t rss = taskInfo.pti_resident_size;
        if (rss == 0) continue;

        byMem.push_back({pids[i], rss});
        memMap[pids[i]] = rss;

        uint64_t cpuAbs = taskInfo.pti_total_user + taskInfo.pti_total_system;
        cpuTimeMap[pids[i]] = (uint64_t)(cpuAbs * timebase_);  // ns
    }

    // Sort by memory descending, take top 7
    std::sort(byMem.begin(), byMem.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    int topN = std::min(7, (int)byMem.size());

    // If we have a previous sample, compute CPU%
    std::unordered_map<pid_t, double> cpuPct;
    if (!firstSample_ && wallNs > 1e6) {  // need at least 1ms
        for (int i = 0; i < topN; i++) {
            pid_t pid = byMem[i].first;
            auto& m = cpuTimeMap[pid];
            auto it = prevSamples_.find(pid);
            if (it != prevSamples_.end()) {
                // Convert previous CPU time (abs) to ns using timebase
                uint64_t prevNs = (uint64_t)((it->second.totalUser + it->second.totalSystem) * timebase_);
                uint64_t deltaNs = (m > prevNs) ? (m - prevNs) : 0;
                cpuPct[pid] = (double)deltaNs / wallNs * 100.0;
                if (cpuPct[pid] > 100.0) cpuPct[pid] = 100.0;  // clamp
            } else {
                cpuPct[pid] = 0.0;
            }
        }
    }

    // Store previous samples for next cycle, and collect the per-row context
    // the list shows. Threads and CPU time come from the proc_taskinfo call this
    // loop already makes; state and user cost one bsdinfo per top row (≤7 calls
    // — never a scan-wide addition). Unreadable rows keep empty/-1 and the list
    // renders N/A.
    prevSamples_.clear();
    std::vector<int> thr(topN, -1);
    std::vector<uint64_t> cpuMs(topN, 0);
    std::vector<std::string> stateOf(topN), userOf(topN);
    for (int i = 0; i < topN; i++) {
        pid_t pid = byMem[i].first;
        proc_taskinfo taskInfo;
        int ret = proc_pidinfo(pid, PROC_PIDTASKINFO, 0,
                               &taskInfo, sizeof(taskInfo));
        if (ret == sizeof(taskInfo)) {
            ProcSample& ps = prevSamples_[pid];
            ps.totalUser = taskInfo.pti_total_user;
            ps.totalSystem = taskInfo.pti_total_system;
            thr[i] = static_cast<int>(taskInfo.pti_threadnum);
            const uint64_t ticks = taskInfo.pti_total_user + taskInfo.pti_total_system;
            cpuMs[i] = static_cast<uint64_t>(static_cast<double>(ticks) * timebase_ / 1e6);
        }
        proc_bsdinfo bi;
        if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, sizeof(bi)) == sizeof(bi)) {
            stateOf[i] = ProcStateWord(bi.pbi_status);
            // uid -> name, cached: a handful of uids repeat across the rows and
            // getpwuid_r can block on a directory service (LDAP/AD) on each call.
            static std::unordered_map<uid_t, std::string> uidNames;
            auto cached = uidNames.find(bi.pbi_uid);
            if (cached == uidNames.end()) {
                char pwBuf[256] = {};
                struct passwd pwd;
                struct passwd* res = nullptr;
                std::string nm;
                if (getpwuid_r(bi.pbi_uid, &pwd, pwBuf, sizeof(pwBuf), &res) == 0 &&
                    res != nullptr && res->pw_name != nullptr) {
                    nm = res->pw_name;
                }
                cached = uidNames.emplace(bi.pbi_uid, std::move(nm)).first;
            }
            if (!cached->second.empty()) userOf[i] = cached->second;
        }
    }
    prevTime_ = now;
    firstSample_ = false;

    // Build result
    entries_.clear();
    for (int i = 0; i < topN; i++) {
        pid_t pid = byMem[i].first;
        ProcessTopEntry e;
        e.pid = pid;
        e.memoryBytes = memMap[pid];
        e.cpuPercent = cpuPct[pid];
        e.threads = thr[i];
        e.cpuTimeMs = cpuMs[i];
        e.state = stateOf[i];
        e.user = userOf[i];
        // Get process name
        char nameBuf[PROC_PIDPATHINFO_MAXSIZE] = {};
        int nameLen = proc_name(pid, nameBuf, sizeof(nameBuf));
        if (nameLen > 0) {
            e.name = std::string(nameBuf, nameLen);
        } else {
            e.name = "(" + std::to_string(pid) + ")";
        }
        entries_.push_back(e);
    }
}

// ── Per-row context for the top N ────────────────────────────────────
