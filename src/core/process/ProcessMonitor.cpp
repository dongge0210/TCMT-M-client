#ifdef TCMT_WINDOWS

#include "ProcessMonitor.h"

#include <windows.h>
#include <winternl.h>

#include <algorithm>
#include <chrono>
#include <unordered_set>
#include <vector>

#ifndef STATUS_INFO_LENGTH_MISMATCH
#define STATUS_INFO_LENGTH_MISMATCH ((NTSTATUS)0xC0000004L)
#endif

namespace {

using NtQuerySystemInformationFn =
    NTSTATUS (NTAPI *)(SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);

// winternl.h exposes only a prefix of the kernel's SYSTEM_PROCESS_INFORMATION
// (CreateTime/UserTime/KernelTime/Threads and the I/O counters are hidden), so
// define the real layout here. This is the documented x64 structure; the
// kernel appends NumberOfThreads entries immediately after Threads[0].
struct SystemThreadInformationRecord {
    LARGE_INTEGER KernelTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER CreateTime;
    ULONG WaitTime;
    PVOID StartAddress;
    CLIENT_ID ClientId;
    KPRIORITY Priority;
    LONG BasePriority;
    ULONG ContextSwitches;
    ULONG ThreadState;
    ULONG WaitReason;
};

struct SystemProcessInformationRecord {
    ULONG NextEntryOffset;
    ULONG NumberOfThreads;
    LARGE_INTEGER WorkingSetPrivateSize;
    ULONG HardFaultCount;
    ULONG NumberOfThreadsHighWatermark;
    ULONGLONG CycleTime;
    LARGE_INTEGER CreateTime;
    LARGE_INTEGER UserTime;
    LARGE_INTEGER KernelTime;
    UNICODE_STRING ImageName;
    KPRIORITY BasePriority;
    HANDLE UniqueProcessId;
    HANDLE InheritedFromUniqueProcessId;
    ULONG HandleCount;
    ULONG SessionId;
    ULONG_PTR UniqueProcessKey;
    SIZE_T PeakVirtualSize;
    SIZE_T VirtualSize;
    ULONG PageFaultCount;
    SIZE_T PeakWorkingSetSize;
    SIZE_T WorkingSetSize;
    SIZE_T QuotaPagedPoolUsage;
    SIZE_T QuotaNonPagedPoolUsage;
    SIZE_T PagefileUsage;
    SIZE_T PeakPagefileUsage;
    SIZE_T PrivatePageCount;
    LARGE_INTEGER ReadOperationCount;
    LARGE_INTEGER WriteOperationCount;
    LARGE_INTEGER OtherOperationCount;
    LARGE_INTEGER ReadTransferCount;
    LARGE_INTEGER WriteTransferCount;
    LARGE_INTEGER OtherTransferCount;
    SystemThreadInformationRecord Threads[1];
};

NtQuerySystemInformationFn GetNtQuerySystemInformation() {
    static auto fn = reinterpret_cast<NtQuerySystemInformationFn>(
        ::GetProcAddress(::GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation"));
    return fn;
}

std::string WideToUtf8(const WCHAR* data, int chars) {
    if (!data || chars <= 0) return {};
    const int bytes = ::WideCharToMultiByte(CP_UTF8, 0, data, chars,
                                            nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string out(static_cast<size_t>(bytes), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, data, chars, out.data(), bytes, nullptr, nullptr);
    return out;
}

// Owning user of the process, as UTF-8. Empty when the token cannot be read
// (protected processes); the TUI renders that as N/A.
std::string ProcessUserUtf8(DWORD pid) {
    HANDLE proc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return {};

    HANDLE token = nullptr;
    std::string result;
    if (::OpenProcessToken(proc, TOKEN_QUERY, &token)) {
        DWORD needed = 0;
        ::GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
        if (needed > 0) {
            std::vector<BYTE> buf(needed);
            if (::GetTokenInformation(token, TokenUser, buf.data(), needed, &needed)) {
                auto* user = reinterpret_cast<TOKEN_USER*>(buf.data());
                WCHAR name[256] = {};
                WCHAR domain[256] = {};
                DWORD nameLen = 256;
                DWORD domainLen = 256;
                SID_NAME_USE use{};
                if (::LookupAccountSidW(nullptr, user->User.Sid, name, &nameLen,
                                        domain, &domainLen, &use)) {
                    result = WideToUtf8(name, static_cast<int>(nameLen));
                }
            }
        }
        ::CloseHandle(token);
    }
    ::CloseHandle(proc);
    return result;
}

// Task Manager-style state derived from the thread states in the same
// SYSTEM_PROCESS_INFORMATION record. KTHREAD_STATE:
// 0 initialized, 1 ready, 2 running, 3 standby, 4 terminated, 5 waiting,
// 6 transition, 7 deferred-ready. WaitReason 5 is Suspended.
std::string ProcessState(const SystemProcessInformationRecord& p) {
    if (p.NumberOfThreads == 0) return "Unknown";

    bool anyRunning = false;
    bool anyReady = false;
    bool allWaiting = true;
    bool suspended = false;

    for (ULONG i = 0; i < p.NumberOfThreads; ++i) {
        const ULONG state = p.Threads[i].ThreadState;
        if (state == 2) anyRunning = true;
        if (state == 1 || state == 3 || state == 6 || state == 7) anyReady = true;
        if (state != 5) allWaiting = false;
        if (state == 5 && p.Threads[i].WaitReason == 5) suspended = true;
    }

    // Keep these within the 7-column TUI cell. A process with no running,
    // ready or waiting threads is between states (initializing or exiting),
    // so call it Idle instead of showing "Unknown" for half the list.
    if (anyRunning) return "Running";
    if (suspended) return "Susp";
    if (allWaiting) return "Sleep";
    if (anyReady) return "Ready";
    return "Idle";
}

}  // namespace

void ProcessMonitor::Refresh() {
    const auto now = std::chrono::steady_clock::now();
    const double dtSec = firstSample_
                             ? 0.0
                             : std::chrono::duration<double>(now - prevTime_).count();
    prevTime_ = now;

    SYSTEM_INFO si = {};
    ::GetSystemInfo(&si);
    const unsigned int coreCount =
        (std::max)(1u, static_cast<unsigned int>(si.dwNumberOfProcessors));

    const auto query = GetNtQuerySystemInformation();
    if (!query) return;

    // The table grows while it is being queried; retry with the size the
    // kernel reports until one call fits.
    std::vector<BYTE> buffer(1u << 20);
    ULONG needed = 0;
    NTSTATUS status = STATUS_INFO_LENGTH_MISMATCH;
    for (int attempt = 0; attempt < 8; ++attempt) {
        status = query(SystemProcessInformation, buffer.data(),
                       static_cast<ULONG>(buffer.size()), &needed);
        if (status != STATUS_INFO_LENGTH_MISMATCH) break;
        buffer.resize(static_cast<size_t>(needed) + 64u * 1024u);
    }
    if (status != 0) return;

    std::vector<ProcessTopEntry> collected;
    std::unordered_set<unsigned long> livePids;

    auto* spi = reinterpret_cast<SystemProcessInformationRecord*>(buffer.data());
    for (;;) {
        const DWORD pid = static_cast<DWORD>(
            reinterpret_cast<uintptr_t>(spi->UniqueProcessId));
        livePids.insert(pid);

        const uint64_t userTime = static_cast<uint64_t>(spi->UserTime.QuadPart);
        const uint64_t kernelTime = static_cast<uint64_t>(spi->KernelTime.QuadPart);
        const uint64_t totalTime = userTime + kernelTime;
        const uint64_t createTime = static_cast<uint64_t>(spi->CreateTime.QuadPart);

        double cpuPct = 0.0;
        const auto it = prevSamples_.find(pid);
        if (!firstSample_ && it != prevSamples_.end() &&
            it->second.createTime == createTime && dtSec > 0.0 &&
            totalTime >= it->second.totalTime) {
            const uint64_t delta = totalTime - it->second.totalTime;
            // 100ns units -> seconds, then percent of all logical CPUs.
            cpuPct = static_cast<double>(delta) * 1e-7 / dtSec / coreCount * 100.0;
            if (cpuPct < 0.0) cpuPct = 0.0;
        }
        prevSamples_[pid] = ProcSample{totalTime, createTime};

        ProcessTopEntry e;
        e.pid = static_cast<pid_t>(pid);
        e.name = WideToUtf8(spi->ImageName.Buffer,
                            static_cast<int>(spi->ImageName.Length / sizeof(WCHAR)));
        if (e.name.empty()) {
            if (pid == 0) e.name = "System Idle Process";
            else if (pid == 4) e.name = "System";
            else e.name = "PID " + std::to_string(pid);
        }

        e.memoryBytes = static_cast<uint64_t>(spi->WorkingSetSize);
        e.cpuPercent = cpuPct;
        e.threads = static_cast<int>(spi->NumberOfThreads);
        e.cpuTimeMs = totalTime / 10000;           // 100ns -> ms
        e.kernelTimeMs = kernelTime / 10000;
        e.userTimeMs = userTime / 10000;
        e.state = ProcessState(*spi);
        e.user = ProcessUserUtf8(pid);
        e.ppid = static_cast<int>(
            reinterpret_cast<uintptr_t>(spi->InheritedFromUniqueProcessId));
        e.handles = static_cast<int>(spi->HandleCount);
        e.sessionId = spi->SessionId;
        e.basePriority = spi->BasePriority;
        e.privateBytes = static_cast<uint64_t>(spi->PrivatePageCount);
        e.virtualBytes = static_cast<uint64_t>(spi->VirtualSize);
        e.pageFaults = static_cast<uint64_t>(spi->PageFaultCount);
        e.readBytes = static_cast<uint64_t>(spi->ReadTransferCount.QuadPart);
        e.writeBytes = static_cast<uint64_t>(spi->WriteTransferCount.QuadPart);
        e.readOps = static_cast<uint64_t>(spi->ReadOperationCount.QuadPart);
        e.writeOps = static_cast<uint64_t>(spi->WriteOperationCount.QuadPart);
        e.otherOps = static_cast<uint64_t>(spi->OtherOperationCount.QuadPart);
        e.createTimeMs =
            (createTime > 116444736000000000ULL)
                ? (createTime - 116444736000000000ULL) / 10000ULL
                : 0ULL;

        collected.push_back(std::move(e));

        if (spi->NextEntryOffset == 0) break;
        spi = reinterpret_cast<SystemProcessInformationRecord*>(
            reinterpret_cast<BYTE*>(spi) + spi->NextEntryOffset);
    }

    // Drop CPU-time tracking for processes that no longer exist.
    for (auto it = prevSamples_.begin(); it != prevSamples_.end();) {
        if (!livePids.count(it->first)) it = prevSamples_.erase(it);
        else ++it;
    }

    // Top by memory, capped to the number of rows the TUI shows.
    std::sort(collected.begin(), collected.end(),
              [](const ProcessTopEntry& a, const ProcessTopEntry& b) {
                  return a.memoryBytes > b.memoryBytes;
              });
    const size_t kTop = 128;
    if (collected.size() > kTop) collected.resize(kTop);

    entries_ = std::move(collected);
    firstSample_ = false;
}


#elif defined(TCMT_LINUX)
// ======================== Linux Implementation ========================
#include "ProcessMonitor.h"
#include "../Utils/Sysfs.h"

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

#endif  // TCMT_WINDOWS / TCMT_LINUX
