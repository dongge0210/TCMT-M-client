// LinuxProcess.cpp — /proc/<pid> top-process sampler.
//
// Same contract as the Windows ProcessMonitor: CPU% is computed from
// utime+stime deltas between two calls, so the first call reports 0 and the
// list is ordered by memory until a delta exists.
//
// Cross-user reads fail under hidepid=2 and similar policies; those PIDs are
// skipped without logging, because it is a normal configuration, not an error.

#include "LinuxProcess.h"
#include "LinuxSysfs.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <map>
#include <mutex>

#include <pwd.h>
#include <sstream>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace tcmt::linux {
namespace {

struct SampleState {
    std::mutex mutex;
    bool primed = false;
    int64_t prevMs = 0;
    std::map<int32_t, uint64_t> prevTicks;
};

SampleState& State() {
    static SampleState state;
    return state;
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool IsNumericName(const char* name) {
    if (!name || !*name) return false;
    for (const char* p = name; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
    }
    return true;
}

struct ProcStat {
    char state = '?';
    uint64_t utime = 0;
    uint64_t stime = 0;
    int threads = -1;
    uint64_t rssPages = 0;
};

// /proc/<pid>/stat: comm may contain spaces and parentheses, so the fields are
// parsed after the last ')'. Field indexes below are relative to the token
// right after ") ".
bool ReadProcStat(int32_t pid, ProcStat& out) {
    std::ifstream ifs("/proc/" + std::to_string(pid) + "/stat");
    if (!ifs.is_open()) return false;
    std::string text;
    std::getline(ifs, text);
    const size_t open = text.find('(');
    const size_t close = text.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open) {
        return false;
    }
    std::istringstream ss(text.substr(close + 1));
    std::vector<std::string> fields;
    std::string token;
    while (ss >> token) fields.push_back(std::move(token));
    // state(0) ppid(1) pgrp(2) session(3) tty_nr(4) tpgid(5) flags(6)
    // minflt(7) cminflt(8) majflt(9) cmajflt(10) utime(11) stime(12)
    // cutime(13) cstime(14) priority(15) nice(16) num_threads(17)
    // itrealvalue(18) starttime(19) vsize(20) rss(21)
    if (fields.size() < 22) return false;
    out.state = fields[0].empty() ? '?' : fields[0][0];
    try {
        out.utime = std::stoull(fields[11]);
        out.stime = std::stoull(fields[12]);
        out.threads = std::stoi(fields[17]);
        out.rssPages = std::stoull(fields[21]);
    } catch (...) {
        return false;
    }
    return true;
}

std::string UidToUser(uint32_t uid, std::map<uint32_t, std::string>& cache) {
    const auto it = cache.find(uid);
    if (it != cache.end()) return it->second;

    std::string result;
    struct passwd pwd {};
    struct passwd* found = nullptr;
    char buffer[1024] = {};
    if (::getpwuid_r(static_cast<uid_t>(uid), &pwd, buffer, sizeof(buffer), &found) == 0 &&
        found && found->pw_name) {
        result = found->pw_name;
    }
    cache[uid] = result;
    return result;
}

// /proc/<pid>/statm: size resident shared text lib data dt (pages).
uint64_t ReadStatmResidentPages(int32_t pid) {
    std::ifstream ifs("/proc/" + std::to_string(pid) + "/statm");
    if (!ifs.is_open()) return 0;
    uint64_t sizePages = 0, residentPages = 0;
    if (!(ifs >> sizePages >> residentPages)) return 0;
    return residentPages;
}

// Name, UID and a VmRSS fallback from /proc/<pid>/status.
void ReadProcStatus(int32_t pid, std::string& name, std::string& user,
                    uint64_t& vmRssBytes) {
    std::ifstream ifs("/proc/" + std::to_string(pid) + "/status");
    if (!ifs.is_open()) return;

    static std::map<uint32_t, std::string> userCache;
    std::string line;
    while (std::getline(ifs, line)) {
        if (line.compare(0, 5, "Name:") == 0) {
            name = Trim(line.substr(5));
        } else if (line.compare(0, 4, "Uid:") == 0) {
            std::istringstream ss(line.substr(4));
            uint64_t uid = 0;
            if (ss >> uid) user = UidToUser(static_cast<uint32_t>(uid), userCache);
        } else if (line.compare(0, 6, "VmRSS:") == 0) {
            std::istringstream ss(line.substr(6));
            uint64_t kb = 0;
            if (ss >> kb) vmRssBytes = kb * 1024ULL;
        }
    }
}

}  // namespace

std::vector<ProcessSample> CollectTopProcesses(int maxCount) {
    if (maxCount <= 0) return {};

    const long clkTck = ::sysconf(_SC_CLK_TCK);
    const long pageSize = ::sysconf(_SC_PAGESIZE);
    const double ticksPerSecond = clkTck > 0 ? static_cast<double>(clkTck) : 100.0;
    const uint64_t bytesPerPage =
        pageSize > 0 ? static_cast<uint64_t>(pageSize) : 4096ULL;

    DIR* proc = ::opendir("/proc");
    if (!proc) {
        static std::atomic<bool> logged{false};
        LogOnce(logged, "LinuxProcess: /proc unreadable; process list unavailable");
        return {};
    }

    SampleState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);

    const int64_t now = NowMs();
    const double dtSeconds =
        state.primed && now > state.prevMs
            ? static_cast<double>(now - state.prevMs) / 1000.0
            : 0.0;

    std::vector<ProcessSample> samples;
    std::map<int32_t, uint64_t> currentTicks;
    int readErrors = 0;

    struct dirent* entry = nullptr;
    while ((entry = ::readdir(proc)) != nullptr) {
        if (!IsNumericName(entry->d_name)) continue;
        int32_t pid = 0;
        try {
            pid = std::stoi(entry->d_name);
        } catch (...) {
            continue;
        }

        ProcStat stat;
        if (!ReadProcStat(pid, stat)) {
            ++readErrors;
            continue;
        }

        ProcessSample sample;
        sample.pid = pid;
        sample.state = std::string(1, stat.state);
        sample.threads = stat.threads;
        sample.cpuTimeMs = static_cast<uint64_t>(
            (stat.utime + stat.stime) * 1000ULL /
            static_cast<uint64_t>(ticksPerSecond > 0 ? ticksPerSecond : 100.0));

        uint64_t vmRssBytes = 0;
        ReadProcStatus(pid, sample.name, sample.user, vmRssBytes);
        if (sample.name.empty()) sample.name = "pid " + std::to_string(pid);
        // statm is the documented RSS source; stat field 24 and VmRSS are
        // fallbacks for kernels/configurations that hide it.
        const uint64_t statmPages = ReadStatmResidentPages(pid);
        if (statmPages > 0) {
            sample.memoryBytes = statmPages * bytesPerPage;
        } else {
            sample.memoryBytes = stat.rssPages * bytesPerPage;
        }
        if (sample.memoryBytes == 0 && vmRssBytes > 0) sample.memoryBytes = vmRssBytes;

        const uint64_t ticks = stat.utime + stat.stime;
        currentTicks[pid] = ticks;
        if (dtSeconds > 0.0) {
            const auto prev = state.prevTicks.find(pid);
            if (prev != state.prevTicks.end() && ticks >= prev->second) {
                const double deltaTicks = static_cast<double>(ticks - prev->second);
                sample.cpuPercent =
                    100.0 * deltaTicks / ticksPerSecond / dtSeconds;
                if (sample.cpuPercent < 0.0) sample.cpuPercent = 0.0;
            }
        }
        samples.push_back(std::move(sample));
    }
    ::closedir(proc);

    state.prevTicks = std::move(currentTicks);
    state.prevMs = now;
    state.primed = true;
    if (readErrors > 0 && samples.empty()) {
        static std::atomic<bool> logged{false};
        LogOnce(logged,
                "LinuxProcess: no process could be read (hidepid or permissions)");
    }

    const bool anyCpu = std::any_of(samples.begin(), samples.end(),
                                    [](const ProcessSample& p) { return p.cpuPercent > 0.0; });
    std::stable_sort(samples.begin(), samples.end(),
                     [anyCpu](const ProcessSample& a, const ProcessSample& b) {
        if (anyCpu && a.cpuPercent != b.cpuPercent) return a.cpuPercent > b.cpuPercent;
        if (a.memoryBytes != b.memoryBytes) return a.memoryBytes > b.memoryBytes;
        return a.pid < b.pid;
    });

    if (static_cast<int>(samples.size()) > maxCount) samples.resize(maxCount);
    return samples;
}

}  // namespace tcmt::linux
