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

#endif  // TCMT_WINDOWS
