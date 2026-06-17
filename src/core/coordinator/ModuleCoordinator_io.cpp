// ModuleCoordinator_io.cpp — Disk and Network collector thread loops
#include "ModuleCoordinator.h"
#include "disk/DiskInfo.h"
#include "network/NetworkAdapter.h"
#include "Utils/Logger.h"

#ifdef TCMT_WINDOWS
#include "Utils/WmiManager.h"
#endif

// ============================================================================
// DiskLoop — 5-second cadence
//   Each cycle: create DiskInfo, call GetDisks(), lock and copy to data_.
// ============================================================================
void ModuleCoordinator::DiskLoop(tcmt::compat::StopToken st) {
    Logger::Debug("DiskLoop: started");

    while (!st.stop_requested()) {
        try {
            DiskInfo diskInfo;
            diskInfo.Refresh();
            auto disks = diskInfo.GetDisks();

            {
                std::lock_guard<std::mutex> lock(data_.diskMutex);
                data_.disks = std::move(disks);
            }
        } catch (const std::exception& e) {
            Logger::Error("DiskLoop exception: " + std::string(e.what()));
        } catch (...) {
            Logger::Error("DiskLoop: unknown exception");
        }

        // Skip sleep if a system event triggered a re-scan
        if (!data_.diskDirty.exchange(false)) {
            SleepFor(st, 5000);
        }
    }

    Logger::Debug("DiskLoop: exited");
}

// ============================================================================
// NetworkLoop — 1-second cadence
//   Each cycle: create NetworkAdapter, call GetAdapters(), filter to connected
//   adapters (max 4), lock and copy into data_.
//
//   Platform note:
//     Windows: NetworkAdapter needs a WmiManager ref. A static WmiManager is
//              used inside the loop so COM/WMI is initialized once.
//     macOS:   NetworkAdapter() default constructor is fine.
// ============================================================================
void ModuleCoordinator::NetworkLoop(tcmt::compat::StopToken st) {
    Logger::Debug("NetworkLoop: started");

    while (!st.stop_requested()) {
        try {
#ifdef TCMT_WINDOWS
            // Static WmiManager ensures COM/WMI is initialised only once
            static WmiManager wmi;
            NetworkAdapter adapter(wmi);
#else
            NetworkAdapter adapter;
#endif
            const auto& adapters = adapter.GetAdapters();

            {
                std::lock_guard<std::mutex> lock(data_.netMutex);
                data_.adapters.clear();
                data_.adapters.reserve((std::min)(adapters.size(), size_t{4}));

                int count = 0;
                for (const auto& a : adapters) {
                    // Skip adapters without an IP (not connected)
                    if (a.ip.empty()) continue;
                    if (count >= 4) break;

                    ModuleData::NetSlot ns;
                    ns.name  = a.name;
                    ns.ip    = a.ip;
                    ns.mac   = a.mac;
                    ns.type  = a.adapterType;
                    ns.speed = a.speed;
                    ns.dl    = a.downloadSpeed;
                    ns.ul    = a.uploadSpeed;

                    data_.adapters.push_back(std::move(ns));
                    ++count;
                }
            }
        } catch (const std::exception& e) {
            Logger::Error("NetworkLoop exception: " + std::string(e.what()));
        } catch (...) {
            Logger::Error("NetworkLoop: unknown exception");
        }

        // Skip sleep if a system/ETW event triggered a re-scan
        if (!data_.networkDirty.exchange(false)) {
            SleepFor(st, 1000);
        }
    }

    Logger::Debug("NetworkLoop: exited");
}
