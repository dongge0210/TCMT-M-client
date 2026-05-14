#include "ModuleCoordinator.h"
#include "temperature/TemperatureWrapper.h"
#include "Utils/Logger.h"
#include <algorithm>
#include <cctype>

// =====================================================================
// Construction / Destruction
// =====================================================================
ModuleCoordinator::ModuleCoordinator() {}

ModuleCoordinator::~ModuleCoordinator() {
    Stop();
}

// =====================================================================
// Start / Stop
// =====================================================================
void ModuleCoordinator::Start() {
    if (running_.exchange(true)) return;

    cpuThread_     = tcmt::compat::JThread([](tcmt::compat::StopToken st, ModuleCoordinator* self) {
        CpuLoop(self->data_, std::move(st));
    }, this);
    memoryThread_  = tcmt::compat::JThread([](tcmt::compat::StopToken st, ModuleCoordinator* self) {
        MemoryLoop(self->data_, std::move(st));
    }, this);
    diskThread_    = tcmt::compat::JThread([](tcmt::compat::StopToken st, ModuleCoordinator* self) {
        self->DiskLoop(std::move(st));
    }, this);
    netThread_     = tcmt::compat::JThread([](tcmt::compat::StopToken st, ModuleCoordinator* self) {
        self->NetworkLoop(std::move(st));
    }, this);
    tempThread_    = tcmt::compat::JThread([](tcmt::compat::StopToken st, ModuleCoordinator* self) {
        self->TemperatureLoop(std::move(st));
    }, this);
    powerThread_   = tcmt::compat::JThread([](tcmt::compat::StopToken st, ModuleCoordinator* self) {
        PowerLoop(self->data_, std::move(st));
    }, this);

    Logger::Info("ModuleCoordinator: started all collection threads");
}

void ModuleCoordinator::Stop() {
    if (!running_.exchange(false)) return;

    // Request stop on all jthreads (destructor joins automatically)
    if (cpuThread_.joinable())    cpuThread_.request_stop();
    if (memoryThread_.joinable()) memoryThread_.request_stop();
    if (diskThread_.joinable())   diskThread_.request_stop();
    if (netThread_.joinable())    netThread_.request_stop();
    if (tempThread_.joinable())   tempThread_.request_stop();
    if (powerThread_.joinable())  powerThread_.request_stop();

    // Join all threads
    if (cpuThread_.joinable())    cpuThread_.join();
    if (memoryThread_.joinable()) memoryThread_.join();
    if (diskThread_.joinable())   diskThread_.join();
    if (netThread_.joinable())    netThread_.join();
    if (tempThread_.joinable())   tempThread_.join();
    if (powerThread_.joinable())  powerThread_.join();

    Logger::Info("ModuleCoordinator: all collection threads stopped");
}

// =====================================================================
// Sleep helper — checks stop token every 50ms for responsive shutdown
// =====================================================================
void ModuleCoordinator::SleepFor(tcmt::compat::StopToken st, int ms) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!st.stop_requested()) {
        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        auto step = (std::min)(remain, std::chrono::milliseconds(50));
        std::this_thread::sleep_for(step);
    }
}

// =====================================================================
// Snapshot — copy all ModuleData atomics + locked vectors into outputs
// =====================================================================
void ModuleCoordinator::Snapshot(SystemInfo& sysInfo, tcmt::TuiData& tuiData) {
    // CPU
    sysInfo.cpuUsage = data_.cpuUsage.load();
    sysInfo.performanceCoreFreq = data_.pCoreFreq.load();
    sysInfo.efficiencyCoreFreq = data_.eCoreFreq.load();

    // Memory
    sysInfo.totalMemory = data_.totalMemory.load();
    sysInfo.usedMemory = data_.usedMemory.load();
    sysInfo.availableMemory = data_.availableMemory.load();
    sysInfo.compressedMemory = data_.compressedMemory.load();

    // GPU
    sysInfo.gpuUsage = data_.gpuUsage.load();
    sysInfo.gpuMemory = data_.gpuMemory.load();
    sysInfo.gpuIsVirtual = data_.gpuIsVirtual.load();
    tuiData.gpuUsage = data_.gpuUsage.load();
    tuiData.gpuMemory = data_.gpuMemory.load();
    {
        std::lock_guard<std::mutex> lock(data_.gpuMutex);
        sysInfo.gpuName = data_.gpuName;
        sysInfo.gpuBrand = data_.gpuBrand;
        tuiData.gpuName = data_.gpuName;
    }

    // Temperature
    {
        std::lock_guard<std::mutex> lock(data_.tempMutex);
        sysInfo.temperatures = data_.temperatures;
        sysInfo.cpuTemperature = data_.cpuTemperature;
        sysInfo.gpuTemperature = data_.gpuTemperature;
        tuiData.temperatures = data_.temperatures;
    }
    tuiData.cpuTemp = sysInfo.cpuTemperature;
    tuiData.gpuTemp = sysInfo.gpuTemperature;

    // Disk
    {
        std::lock_guard<std::mutex> lock(data_.diskMutex);
        sysInfo.disks = data_.disks;
        for (const auto& d : data_.disks) {
            tcmt::TuiData::DiskInfo di;
            di.letter = d.letter;
            di.label = d.label;
            di.totalSize = d.totalSize;
            di.usedSpace = d.usedSpace;
            di.fileSystem = d.fileSystem;
            tuiData.disks.push_back(di);
        }
    }

    // Network
    {
        std::lock_guard<std::mutex> lock(data_.netMutex);
        for (const auto& n : data_.adapters) {
            tcmt::TuiData::NetInfo ni;
            ni.name = n.name;
            ni.ip = n.ip;
            ni.mac = n.mac;
            ni.type = n.type;
            ni.speed = n.speed;
            ni.downloadSpeed = n.dl;
            ni.uploadSpeed = n.ul;
            tuiData.adapters.push_back(ni);
        }
    }

    // Power
    tuiData.batteryPercent = data_.batteryPercent.load();
    tuiData.acOnline = data_.acOnline.load();
    sysInfo.batteryPercent = data_.batteryPercent.load();
    sysInfo.acOnline = data_.acOnline.load();
}

// =====================================================================
// Phase 2: Temperature thread
// =====================================================================
void ModuleCoordinator::TemperatureLoop(tcmt::compat::StopToken st) {
    while (!st.stop_requested()) {
        try {
            auto temps = TemperatureWrapper::GetTemperatures();

            std::lock_guard<std::mutex> lock(data_.tempMutex);
            data_.temperatures = temps;
            data_.cpuTemperature = 0.0;
            data_.gpuTemperature = 0.0;

            for (const auto& [name, temp] : temps) {
                std::string lower = name;
                std::transform(lower.begin(), lower.end(), lower.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

                bool isGpu = (lower.find("gpu") != std::string::npos ||
                              lower.find("tg") != std::string::npos ||
                              lower.find("graphics") != std::string::npos);

                if (isGpu) {
                    if (data_.gpuTemperature == 0.0)
                        data_.gpuTemperature = temp;
                } else {
                    if (data_.cpuTemperature == 0.0)
                        data_.cpuTemperature = temp;
                }
            }
        } catch (const std::exception& e) {
            Logger::Error("TemperatureLoop: " + std::string(e.what()));
        }

        SleepFor(st, 2000);
    }
}
