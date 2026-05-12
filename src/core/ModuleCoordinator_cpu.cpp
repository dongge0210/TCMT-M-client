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

#ifdef TCMT_MACOS
#include <mach/mach.h>
#include <mach/vm_statistics.h>
#endif

// =====================================================================
// CpuLoop  —  500 ms cadence
// Creates one CpuInfo object (calls the constructor once) and reuses it
// across loop iterations.  Each cycle collects usage, P-core freq, and
// E-core freq then sleeps for 500 ms (minus the elapsed collection time).
// =====================================================================
void CpuLoop(ModuleData& data, tcmt::compat::StopToken st) {
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
            data.pCoreFreq.store(cpuInfo->GetLargeCoreSpeed());
            data.eCoreFreq.store(cpuInfo->GetSmallCoreSpeed());
        } catch (const std::exception& e) {
            Logger::Error("CpuLoop: collection error - "
                          + std::string(e.what()));
        }

        // Sleep for the remainder of the 500 ms cycle
        auto elapsed = std::chrono::steady_clock::now() - cycleStart;
        auto elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                .count();
        int sleepMs = 500 - static_cast<int>(elapsedMs);
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
            } else {
                data.compressedMemory.store(0);
            }
#endif
        } catch (const std::exception& e) {
            Logger::Error("MemoryLoop: " + std::string(e.what()));
        }

        ModuleCoordinator::SleepFor(st, 1000);
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

            data.acOnline.store(powerInfo.acOnline);

            // Use the first battery's charge percentage; -1 means no battery
            int batPct = -1;
            if (!powerInfo.batteries.empty()) {
                batPct = static_cast<int>(
                    powerInfo.batteries[0].chargePercent);
            }
            data.batteryPercent.store(batPct);
        } catch (const std::exception& e) {
            Logger::Error("PowerLoop: " + std::string(e.what()));
        }

        ModuleCoordinator::SleepFor(st, 2000);
    }

    Logger::Info("PowerLoop: stopped");
}
