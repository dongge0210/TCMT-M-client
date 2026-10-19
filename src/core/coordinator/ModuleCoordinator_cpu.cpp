// ModuleCoordinator_cpu.cpp
// Free-function thread loops for CPU, Memory, and Power collection.
// Each loop owns its hardware module instance and writes results into
// the shared ModuleData struct via atomics.  SleepFor() checks the
// stop token every 50ms for responsive shutdown.

#include "ModuleCoordinator.h"
#include "cpu/CpuInfo.h"
#include "memory/MemoryInfo.h"
#include "power/PowerInfo.h"
#include "Utils/Logger.h"

#include <thread>
#include <chrono>
#include <memory>

#ifdef TCMT_WINDOWS
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#include "cpu/CpuPowerReader.h"
#endif

#ifdef TCMT_MACOS
#include <mach/mach.h>
#include <mach/vm_statistics.h>
#include <sys/sysctl.h>
#endif

#ifdef TCMT_LINUX
#include "Utils/Sysfs.h"
#include <atomic>
#endif

// =====================================================================
// CpuLoop  —  500 ms cadence
// Creates one CpuInfo object (calls the constructor once) and reuses it
// across loop iterations.  Each cycle collects usage, P-core freq, and
// E-core freq then sleeps for 500 ms (minus the elapsed collection time).
// =====================================================================
void CpuLoop(ModuleData& data, tcmt::compat::StopToken st) {
#ifdef TCMT_LINUX
    // Linux: /proc/stat is the source of truth for both the aggregate and the
    // per-CPU values. SampleCpu() caches the result for Snapshot(), so the UI
    // thread never reads /proc itself.
    Logger::Info("CpuLoop: started (Linux /proc/stat)");

    while (!st.stop_requested()) {
        const auto cycleStart = std::chrono::steady_clock::now();
        try {
            const tcmt::linux::CpuSnapshot snapshot = tcmt::linux::SampleCpu();
            data.cpuUsage.store(snapshot.totalUsage);

            double freqSum = 0.0;
            int freqCount = 0;
            for (double mhz : snapshot.coreFreqMHz) {
                if (mhz > 0.0) {
                    freqSum += mhz;
                    ++freqCount;
                }
            }
            // Linux has no P/E split; the average current frequency is the
            // only honest aggregate. eCoreFreq stays 0.
            if (freqCount > 0) data.pCoreFreq.store(freqSum / freqCount);
            data.eCoreFreq.store(0.0);
        } catch (const std::exception& e) {
            Logger::Error("CpuLoop: collection error - " + std::string(e.what()));
        }

        const auto elapsed = std::chrono::steady_clock::now() - cycleStart;
        const auto elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        const int sleepMs = ScaledSampleIntervalMs(data, 1, 200) -
                            static_cast<int>(elapsedMs);
        if (sleepMs > 0) ModuleCoordinator::SleepFor(st, sleepMs);
    }

    Logger::Info("CpuLoop: stopped");
    return;
#endif  // TCMT_LINUX

    // Create the CpuInfo instance once; if construction fails the thread
    // exits immediately because there is nothing useful to do.
    std::unique_ptr<CpuInfo> cpuInfo;
    try {
        cpuInfo = std::make_unique<CpuInfo>();
    } catch (const std::exception& e) {
        Logger::Error("CpuLoop: failed to create CpuInfo - "
                      + std::string(e.what()));
        return;
    }

    Logger::Info("CpuLoop: started");

    while (!st.stop_requested()) {
        auto cycleStart = std::chrono::steady_clock::now();

        try {
            data.cpuUsage.store(cpuInfo->GetUsage());
            double pf = cpuInfo->GetLargeCoreSpeed();
            double ef = cpuInfo->GetSmallCoreSpeed();
            if (pf > 0) data.pCoreFreq.store(pf);
            if (ef > 0) data.eCoreFreq.store(ef);
        } catch (const std::exception& e) {
            // Loop-body failures repeat every tick while the fault persists;
            // stay at ERROR, but throttling repeated text is a later item
            // (see Logger level conventions in Logger.h).
            Logger::Error("CpuLoop: collection error - "
                          + std::string(e.what()));
        }

        // Sleep for the remainder of the cycle（1× 采样基周期，下限 200ms）
        auto elapsed = std::chrono::steady_clock::now() - cycleStart;
        auto elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                .count();
        int sleepMs = ScaledSampleIntervalMs(data, 1, 200) -
                      static_cast<int>(elapsedMs);
        if (sleepMs > 0) {
            ModuleCoordinator::SleepFor(st, sleepMs);
        }
    }

    Logger::Info("CpuLoop: stopped");
}

// =====================================================================
// MemoryLoop  —  1 s cadence
// Constructs a fresh MemoryInfo object each cycle so that the
// GlobalMemoryStatusEx (Windows) / sysctl + host_statistics64 (macOS)
// values are always up-to-date.  On macOS compressed memory is read
// directly via host_statistics64.
// =====================================================================
void MemoryLoop(ModuleData& data, tcmt::compat::StopToken st) {
    Logger::Info("MemoryLoop: started");

    while (!st.stop_requested()) {
        try {
            MemoryInfo memInfo;

            uint64_t total     = memInfo.GetTotalPhysical();
            uint64_t available = memInfo.GetAvailablePhysical();
            uint64_t used      = total - available;

            data.totalMemory.store(total);
            data.usedMemory.store(used);
            data.availableMemory.store(available);

#ifdef TCMT_MACOS
            // macOS: read compressed memory via Mach host_statistics64
            mach_port_t host = mach_host_self();
            vm_size_t pageSize = vm_kernel_page_size;
            vm_statistics64_data_t vmStats;
            mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;

            if (host_statistics64(host, HOST_VM_INFO64,
                                  (host_info64_t)&vmStats,
                                  &count) == KERN_SUCCESS) {
                data.compressedMemory.store(
                    vmStats.compressor_page_count *
                    static_cast<uint64_t>(pageSize));
                // Swap usage
                struct xsw_usage xsu;
                size_t xsuLen = sizeof(xsu);
                if (sysctlbyname("vm.swapusage", &xsu, &xsuLen, nullptr, 0) == 0) {
                    data.swapUsed.store(xsu.xsu_used);
                    data.swapTotal.store(xsu.xsu_total);
                }
            } else {
                data.compressedMemory.store(0);
            }
#elif defined(TCMT_LINUX)
            // /proc/swaps reports every active swap area; SwapFree from
            // /proc/meminfo alone would miss multiple devices.
            uint64_t swapUsed = 0, swapTotal = 0;
            if (tcmt::linux::ReadSwapUsage(swapUsed, swapTotal)) {
                data.swapUsed.store(swapUsed);
                data.swapTotal.store(swapTotal);
            }
            // Linux has no single kernel counter equivalent to macOS's
            // compressor or Windows' compression store: zswap/zram usage is
            // per-device and not comparable. Report 0 and say so once.
            data.compressedMemory.store(0);
            static std::atomic<bool> compressedLogged{false};
            if (!compressedLogged.exchange(true)) {
                Logger::Info("MemoryLoop: no system-wide compressed-memory counter on Linux; reporting 0");
            }
#elif defined(TCMT_WINDOWS)
            // Windows: estimate compressed memory = CommitTotal - PhysicalUsed - PageFileUsed.
            // The compression store lives in physical memory and counts toward commit.
            {
                PERFORMANCE_INFORMATION pi = { sizeof(PERFORMANCE_INFORMATION) };
                if (GetPerformanceInfo(&pi, sizeof(pi))) {
                    uint64_t commitB = (uint64_t)pi.CommitTotal * pi.PageSize;
                    uint64_t physUsed = (uint64_t)(pi.PhysicalTotal - pi.PhysicalAvailable) * pi.PageSize;
                    // Subtract pagefile-backed commit (approximate)
                    MEMORYSTATUSEX msx = { sizeof(msx) };
                    if (GlobalMemoryStatusEx(&msx)) {
                        uint64_t pagefileUsed = msx.ullTotalPageFile - msx.ullAvailPageFile;
                        uint64_t commitDedup = (commitB > physUsed + pagefileUsed)
                            ? (commitB - physUsed - pagefileUsed) : 0;
                        if (commitDedup > 0 && commitDedup < (uint64_t)32ULL * 1024 * 1024 * 1024)
                            data.compressedMemory.store(commitDedup);
                    }
                }
            }
#endif
        } catch (const std::exception& e) {
            Logger::Error("MemoryLoop: " + std::string(e.what()));
        }

        // 2× 采样基周期，下限 500ms
        ModuleCoordinator::SleepFor(st, ScaledSampleIntervalMs(data, 2, 500));
    }

    Logger::Info("MemoryLoop: stopped");
}

// =====================================================================
// PowerLoop  —  2 s cadence
// Constructs a fresh PowerInfo and calls Detect() each cycle to read
// battery percentage and AC adapter status.
// =====================================================================
void PowerLoop(ModuleData& data, tcmt::compat::StopToken st) {
    Logger::Info("PowerLoop: started");

    while (!st.stop_requested()) {
        try {
            PowerInfo powerInfo;
            powerInfo.Detect();

            // An ETW (Windows) or SystemEventMonitor power callback may have landed
            // while we were reading. Those callbacks store the state the change
            // actually produced, so this read is older by definition: keep theirs and
            // re-read next tick instead of overwriting a fresh value with a stale one.
            // At most one iteration is skipped per event, because the flags are
            // consumed below.
            const bool supersededByEvent = data.powerDirty.load() || data.sysPowerDirty.load();
            if (!supersededByEvent) {
                data.acOnline.store(powerInfo.acOnline);

                // Use the first battery's charge percentage; -1 means no battery
                int batPct = -1;
                if (!powerInfo.batteries.empty()) {
                    const auto& primary = powerInfo.batteries[0];
                    batPct = static_cast<int>(primary.chargePercent);
#ifdef TCMT_LINUX
                    // power_supply has no "capacity" file on some devices and
                    // no energy/charge counters either. 0 would be a made-up
                    // reading, so report "unknown" instead.
                    if (primary.chargePercent == 0 && primary.fullChargeCapacity == 0 &&
                        primary.currentCapacity == 0 && primary.designCapacity == 0) {
                        batPct = -1;
                    }
#endif
                }
                data.batteryPercent.store(batPct);

                // 电池明细。Windows 侧这些数来自电池设备接口（见 PowerInfo.cpp），
                // macOS 侧来自 IOKit。读不出来的项就是 0，不代表真实值。
                if (!powerInfo.batteries.empty()) {
                    const auto& b = powerInfo.batteries[0];
                    data.batteryPresent.store(true);
                    data.batteryCycleCount.store(static_cast<int>(b.cycleCount));
                    data.batteryDesignCapacity.store(static_cast<int>(b.designCapacity));
                    data.batteryFullCapacity.store(static_cast<int>(b.fullChargeCapacity));
                    data.batteryVoltage.store(static_cast<int>(b.voltage));
                    // wearLevel 是磨损比例，健康度是它的补数；两个容量都拿不到时留 0
                    data.batteryHealthPercent.store(
                        (b.designCapacity > 0 && b.fullChargeCapacity > 0)
                            ? (100.0 - b.wearLevel * 100.0)
                            : 0.0);
                } else {
                    data.batteryPresent.store(false);
                }

#ifdef TCMT_WINDOWS
                // CPU 功耗走 RAPL 能量差（Intel + PawnIO 才有）。取不到返回 -1 时保持原值，
                // 不写 0 —— 0 会被界面当成「功耗是 0」这种真实读数。
                {
                    const double watts = CpuPowerReader::ReadPackagePowerW();
                    if (watts >= 0.0) data.cpuPower.store(watts * 1000.0);  // W → mW
                }
#elif defined(TCMT_LINUX)
                // RAPL energy_uj is root-only by default since CVE-2020-8694.
                // -1 means "not readable"; store 0 so powerAvailable stays
                // false instead of keeping a stale reading.
                const double milliwatts = tcmt::linux::SampleCpuPackagePowerMilliwatts();
                data.cpuPower.store(milliwatts >= 0.0 ? milliwatts : 0.0);
#endif
            }
        } catch (const std::exception& e) {
            Logger::Error("PowerLoop: " + std::string(e.what()));
        }

        // Respond faster to ETW/system power events；平时按 4× 采样基周期，下限 1000ms
        if (!data.powerDirty.exchange(false) && !data.sysPowerDirty.exchange(false)) {
            ModuleCoordinator::SleepFor(st, ScaledSampleIntervalMs(data, 4, 1000));
        }
    }

    Logger::Info("PowerLoop: stopped");
}
