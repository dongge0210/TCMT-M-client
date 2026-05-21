#include "ModuleCoordinator.h"
#include "temperature/TemperatureWrapper.h"
#include "Utils/Logger.h"
#include "Utils/WinUtils.h"
#include "memory/MemoryInfo.h"
#include <algorithm>
#include <cctype>
#include "notifications/UserNotifier.h"

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

#ifdef TCMT_WINDOWS
    // ETW trace session — kernel event notifications
    etwMonitor_.SetPowerCallback([this](bool acOnline) {
        data_.acOnline.store(acOnline);
        data_.powerDirty.store(true);
    });
    etwMonitor_.SetBatteryCallback([this](int pct) {
        if (pct >= 0 && pct <= 100) data_.batteryPercent.store(pct);
        data_.powerDirty.store(true);
    });
    etwMonitor_.SetNetworkCallback([this]() {
        data_.networkDirty.store(true);
    });
    etwMonitor_.SetWifiCallback([this]() {
        data_.wifiDirty.store(true);
    });
    etwMonitor_.SetBluetoothCallback([this]() {
        data_.btDirty.store(true);
    });
    etwMonitor_.SetUsbCallback([this]() {
        data_.usbDirty.store(true);
    });
    etwMonitor_.SetCpuFreqCallback([this]() {
        data_.cpuFreqDirty.store(true);
    });
    if (!etwMonitor_.Start()) {
        Logger::Warn("ModuleCoordinator: EtwMonitor start failed — " +
                     etwMonitor_.GetLastError() + " (falling back to polling)");
    }
#endif

    // SystemEventMonitor — platform-level power/disk/thermal events
    sysEventMonitor_.SetPowerCallback([this](bool acOnline, int batteryPercent) {
        data_.acOnline.store(acOnline);
        if (batteryPercent >= 0 && batteryPercent <= 100)
            data_.batteryPercent.store(batteryPercent);
        data_.sysPowerDirty.store(true);
    });
    sysEventMonitor_.SetDiskCallback([this]() {
        data_.diskDirty.store(true);
    });
    sysEventMonitor_.SetNetworkCallback([this]() {
        data_.networkDirty.store(true);
    });
    sysEventMonitor_.SetThermalCallback([this](int state) {
        (void)state;
        data_.thermalDirty.store(true);
    });
    if (!sysEventMonitor_.Start()) {
        Logger::Warn("ModuleCoordinator: SystemEventMonitor start failed, using polling fallback");
    }

#ifdef TCMT_MACOS
    // PowerMonitor — direct IOReport (no sudo) with powermetrics fallback
    if (!powerMonitor_.Start()) {
        Logger::Info("ModuleCoordinator: PowerMonitor direct mode unavailable, using powermetrics fallback");
    } else {
        Logger::Info("ModuleCoordinator: PowerMonitor direct mode active (no sudo)");
        // Seed frequency immediately (before TemperatureLoop's first iteration)
        data_.pCoreFreq.store(powerMonitor_.GetPCoreFreq());
        data_.eCoreFreq.store(powerMonitor_.GetECoreFreq());
        data_.pCoreMaxFreq.store(powerMonitor_.GetPCoreMaxFreq());
        data_.eCoreMaxFreq.store(powerMonitor_.GetECoreMaxFreq());
        data_.gpuFreq.store(powerMonitor_.GetGpuFreq());
        data_.gpuMaxFreq.store(powerMonitor_.GetGpuMaxFreq());
    }
#endif

    // Read RAM specs once at startup
    MemoryInfo memInfo;
    data_.ramSpeed = memInfo.GetRamSpeed();
    data_.ramType = memInfo.GetRamType();
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

    powerMonitor_.Stop();
    sysEventMonitor_.Stop();

#ifdef TCMT_WINDOWS
    etwMonitor_.Stop();
#endif
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
    tuiData.pCoreFreq = data_.pCoreFreq.load();
    tuiData.eCoreFreq = data_.eCoreFreq.load();
    tuiData.pCoreMaxFreq = data_.pCoreMaxFreq.load();
    tuiData.eCoreMaxFreq = data_.eCoreMaxFreq.load();

    // Memory
    sysInfo.totalMemory = data_.totalMemory.load();
    sysInfo.usedMemory = data_.usedMemory.load();
    sysInfo.availableMemory = data_.availableMemory.load();
    sysInfo.compressedMemory = data_.compressedMemory.load();
    tuiData.compressedMemory = data_.compressedMemory.load();
    sysInfo.ramSpeed = data_.ramSpeed;
    snprintf(sysInfo.ramType, sizeof(sysInfo.ramType), "%s", data_.ramType.c_str());
    tuiData.ramSpeed = data_.ramSpeed;
    snprintf(tuiData.ramType, sizeof(tuiData.ramType), "%s", data_.ramType.c_str());

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
#ifdef TCMT_WINDOWS
        sysInfo.adapters.clear();
#endif
        for (const auto& n : data_.adapters) {
#ifdef TCMT_WINDOWS
            NetworkAdapterData sysAd;
            memset(&sysAd, 0, sizeof(sysAd));
            wcsncpy_s(sysAd.name, WinUtils::StringToWstring(n.name).c_str(), _TRUNCATE);
            wcsncpy_s(sysAd.mac, WinUtils::StringToWstring(n.mac).c_str(), _TRUNCATE);
            wcsncpy_s(sysAd.ipAddress, WinUtils::StringToWstring(n.ip).c_str(), _TRUNCATE);
            wcsncpy_s(sysAd.adapterType, WinUtils::StringToWstring(n.type).c_str(), _TRUNCATE);
            sysAd.speed = n.speed;
            sysAd.downloadSpeed = n.dl;
            sysAd.uploadSpeed = n.ul;
            sysInfo.adapters.push_back(sysAd);
#endif
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
#ifdef TCMT_WINDOWS
        if (!data_.adapters.empty()) {
            sysInfo.networkAdapterName = data_.adapters[0].name;
            sysInfo.networkAdapterMac = data_.adapters[0].mac;
            sysInfo.networkAdapterIp = data_.adapters[0].ip;
            sysInfo.networkAdapterType = data_.adapters[0].type;
            sysInfo.networkAdapterSpeed = data_.adapters[0].speed;
        }
#endif
    }

    // Power
    tuiData.batteryPercent = data_.batteryPercent.load();
    tuiData.acOnline = data_.acOnline.load();
    sysInfo.batteryPercent = data_.batteryPercent.load();
    sysInfo.acOnline = data_.acOnline.load();

    // PowerMonitor (Apple Silicon — CPU/GPU/ANE power in mW)
    tuiData.cpuPower = data_.cpuPower.load();
    tuiData.gpuPower = data_.gpuPower.load();
    tuiData.anePower = data_.anePower.load();
    tuiData.gpuFreq = data_.gpuFreq.load();
    tuiData.gpuMaxFreq = data_.gpuMaxFreq.load();
    sysInfo.cpuPower = data_.cpuPower.load();
    sysInfo.gpuPower = data_.gpuPower.load();
    sysInfo.anePower = data_.anePower.load();
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

                // Battery and power entries are not thermal sensors
                if (lower.find("battery") != std::string::npos) continue;
                if (lower.find("power") != std::string::npos) continue;

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

#ifdef TCMT_MACOS
            // Frequency + power from PowerMonitor (IOReport, dynamic, no sudo)
            // Falls back to powermetrics globals if PowerMonitor not in direct mode
            if (powerMonitor_.IsDirectMode()) {
                double pf = powerMonitor_.GetPCoreFreq();
                double ef = powerMonitor_.GetECoreFreq();
                if (pf > 0) data_.pCoreFreq.store(pf);
                if (ef > 0) data_.eCoreFreq.store(ef);
                data_.cpuPower.store(powerMonitor_.GetCpuPower());
                data_.gpuPower.store(powerMonitor_.GetGpuPower());
                data_.anePower.store(powerMonitor_.GetAnePower());
                data_.gpuFreq.store(powerMonitor_.GetGpuFreq());
            } else {
                double pf = GetPmPCoreFreq();
                double ef = GetPmECoreFreq();
                if (pf > 0) data_.pCoreFreq.store(pf);
                if (ef > 0) data_.eCoreFreq.store(ef);
            }
#endif
        } catch (const std::exception& e) {
            Logger::Error("TemperatureLoop: " + std::string(e.what()));
        }

        SleepFor(st, 2000);
    }
}
