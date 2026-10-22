#include "ModuleCoordinator.h"
// Snapshot() writes TuiData members, so this translation unit needs the full definition.
// The header above only forward-declares it — that keeps the include out of every file
// that includes ModuleCoordinator.h.
#include "../../tui/TuiApp.h"
#include "temperature/TemperatureWrapper.h"
#include "Utils/Logger.h"
#include "Utils/WinUtils.h"
#include "memory/MemoryInfo.h"
#include <algorithm>
#include <cctype>
#ifdef TCMT_MACOS
#include <sys/sysctl.h>
#endif
#include "notifications/UserNotifier.h"

#ifdef TCMT_LINUX
#include "cpu/CpuInfo.h"
#include "gpu/GpuInfo.h"
#include "power/PowerInfo.h"
#include "process/ProcessMonitor.h"
#include "Utils/Sysfs.h"
#include <chrono>
#include <sys/types.h>
#include <utility>
#endif

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
        // 能力探测，不是故障 —— powermetrics 那条路照样能出功耗。
        // 真拿不到数据时界面会自己说（"Power unavailable"），日志里不必留一条 INFO。
        Logger::Debug("ModuleCoordinator: PowerMonitor direct mode unavailable, using powermetrics fallback");
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
// Per-core frequencies are a macOS-only source today: IOReport publishes one
// channel per core and the channel name carries the index (PCPU0.., ECPU0..).
// The kernel numbers logical CPUs P first, then E, which is the order written
// here. A non-direct power monitor (powermetrics fallback) only has cluster
// values, so it reports nothing rather than guessing.
int ModuleCoordinator::GetPerCoreFreqs(double* out, int max) {
#ifdef TCMT_MACOS
    if (out == nullptr || max <= 0) return 0;
    if (!powerMonitor_.IsDirectMode()) return 0;
    static const int pCores = [] {
        int v = 0;
        size_t len = sizeof(v);
        return sysctlbyname("hw.perflevel0.logicalcpu", &v, &len, nullptr, 0) == 0 ? v : 0;
    }();
    static const int eCores = [] {
        int v = 0;
        size_t len = sizeof(v);
        return sysctlbyname("hw.perflevel1.logicalcpu", &v, &len, nullptr, 0) == 0 ? v : 0;
    }();
    int n = 0;
    for (int i = 0; i < pCores && n < max; ++i)
        out[n++] = powerMonitor_.GetPCoreFreqByIdx(i);
    for (int i = 0; i < eCores && n < max; ++i)
        out[n++] = powerMonitor_.GetECoreFreqByIdx(i);
    return n;
#else
    (void)out;
    (void)max;
    return 0;
#endif
}

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
    sysInfo.swapUsed = data_.swapUsed.load();
    sysInfo.swapTotal = data_.swapTotal.load();
    tuiData.compressedMemory = data_.compressedMemory.load();
    tuiData.swapUsed = data_.swapUsed.load();
    tuiData.swapTotal = data_.swapTotal.load();
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
        sysInfo.cpuPcoreTemperature = data_.cpuPcoreTemperature;
        sysInfo.cpuEcoreTemperature = data_.cpuEcoreTemperature;
        sysInfo.gpuTemperature = data_.gpuTemperature;
        tuiData.temperatures = data_.temperatures;
    }
    tuiData.cpuTemp = sysInfo.cpuTemperature;
    tuiData.cpuPcoreTemp = sysInfo.cpuPcoreTemperature;
    tuiData.cpuEcoreTemp = sysInfo.cpuEcoreTemperature;
    tuiData.gpuTemp = sysInfo.gpuTemperature;

    // Disk
    {
        std::lock_guard<std::mutex> lock(data_.diskMutex);
        sysInfo.disks = data_.disks;
        for (const auto& d : data_.disks) {
            tcmt::TuiData::DiskInfo di;
            di.letter = d.letter;
            di.mountPoint = d.mountPoint;
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
            wcsncpy_s(sysAd.macCurrent,
                      WinUtils::StringToWstring(n.currentMac.empty() ? n.mac : n.currentMac).c_str(),
                      _TRUNCATE);
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
            ni.currentMac = n.currentMac;
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
    // 电池明细：schema 里那批 battery/* 字段一直在发，Windows 侧以前从来没人填。
    sysInfo.batteryIsPresent = data_.batteryPresent.load();
    sysInfo.batteryCycleCount = data_.batteryCycleCount.load();
    sysInfo.batteryDesignCapacity = data_.batteryDesignCapacity.load();
    sysInfo.batteryMaxCapacity = data_.batteryFullCapacity.load();
    sysInfo.batteryVoltage = data_.batteryVoltage.load();
    sysInfo.batteryHealthPercent = static_cast<float>(data_.batteryHealthPercent.load());

    // PowerMonitor (Apple Silicon — CPU/GPU/ANE power in mW)
    const bool pwrAvail = powerMonitor_.IsPowerAvailable();
    tuiData.cpuPower = data_.cpuPower.load();
    tuiData.gpuPower = data_.gpuPower.load();
    tuiData.anePower = data_.anePower.load();
    tuiData.powerAvailable = pwrAvail;
    tuiData.gpuFreq = data_.gpuFreq.load();
    tuiData.gpuMaxFreq = data_.gpuMaxFreq.load();
    sysInfo.cpuPower = data_.cpuPower.load();
    sysInfo.gpuPower = data_.gpuPower.load();
    sysInfo.anePower = data_.anePower.load();
    sysInfo.powerAvailable = pwrAvail;
    sysInfo.gpuFreq = data_.gpuFreq.load();

    // ─── 4. Fan speeds (extracted from temperature list by "Fan" name) ───
    sysInfo.fans.clear();
    {
        std::lock_guard<std::mutex> lock(data_.tempMutex);
        for (const auto& [name, temp] : data_.temperatures) {
            if (name.find("Fan") != std::string::npos ||
                name.find("fan") != std::string::npos) {
                SystemInfo::FanData fd;
                fd.name = name;
                fd.rpm = static_cast<float>(temp); // temp field reused for RPM
                sysInfo.fans.push_back(fd);
            }
        }
    }

    // ─── 5+7+6: Process top, battery detail, per-core — populated by caller (main_mac/main) ───
    // These are filled by the main loop, not by ModuleCoordinator

#ifdef TCMT_LINUX
    // -------------------------------------------------------------------
    // Linux: fill the fields the other entry points populate themselves.
    // Everything below is cached; Snapshot() runs at the UI cadence and must
    // not scan /proc or call NVML on every frame.
    // -------------------------------------------------------------------
    using SteadyClock = std::chrono::steady_clock;
    const auto now = SteadyClock::now();

    // Per-core CPU (collected by CpuLoop through LinuxCpu).
    const tcmt::linux::CpuSnapshot cpu = tcmt::linux::GetLatestCpu();
    const int coreCount = (std::min)(cpu.logicalCores, 16);
    sysInfo.perCoreCount = coreCount;
    tuiData.perCoreCount = static_cast<uint8_t>(coreCount);
    for (int i = 0; i < coreCount; ++i) {
        const size_t index = static_cast<size_t>(i);
        const float freq = index < cpu.coreFreqMHz.size()
                               ? static_cast<float>(cpu.coreFreqMHz[index])
                               : 0.0f;
        const float temp = index < cpu.coreTempC.size()
                               ? static_cast<float>(cpu.coreTempC[index])
                               : 0.0f;
        const float usage = index < cpu.coreUsage.size()
                                ? static_cast<float>(cpu.coreUsage[index])
                                : 0.0f;
        sysInfo.perCoreFreq[i] = freq;
        sysInfo.perCoreTemp[i] = temp;
        sysInfo.perCoreUsage[i] = usage;
        tuiData.perCoreFreq[i] = freq;
        tuiData.perCoreTemp[i] = temp;
        tuiData.perCoreUsage[i] = usage;
    }
    sysInfo.cpuUsageSampleIntervalMs = cpu.intervalMs;
    tuiData.sampleIntervalMs = static_cast<int>(cpu.intervalMs);

    // The coretemp/k10temp package sensor is a better CPU temperature than
    // the first non-GPU entry in the generic sensor list.
    if (cpu.hasPackageTemp) {
        sysInfo.cpuTemperature = cpu.packageTempC;
        tuiData.cpuTemp = cpu.packageTempC;
    }

    // Fans are RPM, not temperatures, so they are not in the sensor list.
    // Refresh them separately (every four seconds is plenty for a fan ramp).
    static bool fanPrimed = false;
    static SteadyClock::time_point fanAt{};
    static std::vector<std::pair<std::string, double>> fanCache;
    if (!fanPrimed || now - fanAt > std::chrono::seconds(4)) {
        fanCache.clear();
        tcmt::linux::ReadHwmonFans(fanCache);
        fanAt = now;
        fanPrimed = true;
    }
    sysInfo.fans.clear();
    sysInfo.fans.reserve(fanCache.size());
    for (const auto& [name, rpm] : fanCache) {
        SystemInfo::FanData fan;
        fan.name = name;
        fan.rpm = static_cast<float>(rpm);
        sysInfo.fans.push_back(std::move(fan));
    }

    // GPU: NVML + DRM sysfs. One second of cache is enough for the UI.
    static bool gpuPrimed = false;
    static SteadyClock::time_point gpuAt{};
    static std::vector<tcmt::linux::GpuSnapshot> gpuCache;
    if (!gpuPrimed || now - gpuAt > std::chrono::seconds(1)) {
        gpuCache = tcmt::linux::CollectGpus();
        gpuAt = now;
        gpuPrimed = true;
    }
    sysInfo.gpus.clear();
    sysInfo.gpus.reserve(gpuCache.size());
    for (const auto& gpu : gpuCache) {
        GpuSpec spec;
        spec.name = gpu.name;
        spec.brand = gpu.brand.empty() ? GpuBrandFromName(gpu.name) : gpu.brand;
        spec.memory = gpu.memoryBytes;
        spec.memoryPercent = gpu.memoryPercent;
        spec.coreClock = gpu.coreClockMHz;
        spec.temperature = gpu.temperatureValid ? gpu.temperatureC : 0.0;
        spec.isVirtual = gpu.isVirtual;
        spec.isUnified = gpu.isUnified;
        spec.usage = gpu.usagePercent;
        sysInfo.gpus.push_back(std::move(spec));
    }
    if (!sysInfo.gpus.empty()) {
        const GpuSpec& primary = sysInfo.gpus.front();
        sysInfo.gpuName = primary.name;
        sysInfo.gpuBrand = primary.brand;
        sysInfo.gpuMemory = primary.memory;
        sysInfo.gpuCoreFreq = primary.coreClock;
        sysInfo.gpuUsage = primary.usage;
        sysInfo.gpuIsVirtual = primary.isVirtual;
        sysInfo.gpuFreq = primary.coreClock;
        tuiData.gpuFreq = primary.coreClock;
        tuiData.gpuName = primary.name;
        tuiData.gpuMemory = primary.memory;
        tuiData.gpuMemoryPercent = primary.memoryPercent;
        tuiData.gpuUsage = primary.usage;
        tuiData.gpuUnified = primary.isUnified;
        if (primary.temperature > 0.0) {
            sysInfo.gpuTemperature = primary.temperature;
            tuiData.gpuTemp = primary.temperature;
        }
    }

    // Power: RAPL is the only CPU power source; absence means unavailable,
    // never an estimate from load.
    const double cpuMilliwatts = data_.cpuPower.load();
    sysInfo.cpuPower = cpuMilliwatts;
    tuiData.cpuPower = cpuMilliwatts;
    sysInfo.powerAvailable = cpuMilliwatts > 0.0;
    tuiData.powerAvailable = sysInfo.powerAvailable;
    sysInfo.anePower = 0.0;
    tuiData.anePower = 0.0;
    if (!gpuCache.empty() && gpuCache.front().powerValid) {
        sysInfo.gpuPower = gpuCache.front().powerMilliwatts;
        tuiData.gpuPower = gpuCache.front().powerMilliwatts;
    }

    // Battery detail (voltage/current/temperature/power) every two seconds.
    static bool batteryPrimed = false;
    static SteadyClock::time_point batteryAt{};
    static tcmt::linux::BatteryDetail battery;
    if (!batteryPrimed || now - batteryAt > std::chrono::seconds(2)) {
        battery = tcmt::linux::ReadPrimaryBattery();
        batteryAt = now;
        batteryPrimed = true;
    }
    if (battery.present) {
        sysInfo.batteryIsPresent = true;
        sysInfo.batteryIsCharging = battery.charging;
        tuiData.batteryIsCharging = battery.charging;
        if (battery.temperatureValid) {
            sysInfo.batteryTemp = static_cast<float>(battery.temperatureC);
            tuiData.batteryTemp = battery.temperatureC;
        }
        int milliamps = 0;
        if (battery.currentUA > 0) {
            milliamps = static_cast<int>(battery.currentUA / 1000ULL);
            if (battery.currentNegative) milliamps = -milliamps;
        } else if (battery.powerValid && battery.voltageUV > 0) {
            // Some drivers expose power_now but not current_now. Derive the
            // current so the TUI's power row (mA * mV) stays populated. Sign
            // follows the charge direction, matching the TUI convention.
            const double volts = static_cast<double>(battery.voltageUV) / 1.0e6;
            milliamps = static_cast<int>(battery.watts * 1000.0 / volts);
            if (battery.discharging) milliamps = -milliamps;
        }
        if (milliamps != 0) {
            sysInfo.batteryAmperage = milliamps;
            tuiData.batteryAmperage = milliamps;
        }
        if (battery.voltageUV > 0) {
            const int millivolts = static_cast<int>(battery.voltageUV / 1000ULL);
            sysInfo.batteryVoltage = millivolts;
            tuiData.batteryVoltage = millivolts;
        }
        // chargerWatts means "rated charger" in the TUI; power_supply only
        // reports the battery's actual power, so it is deliberately not set.
    }

    // Top processes every two seconds.
    static bool procPrimed = false;
    static SteadyClock::time_point procAt{};
    static std::vector<tcmt::linux::ProcessSample> procCache;
    if (!procPrimed || now - procAt > std::chrono::seconds(2)) {
        procCache = tcmt::linux::CollectTopProcesses(12);
        procAt = now;
        procPrimed = true;
    }
    sysInfo.topProcesses.clear();
    sysInfo.topProcesses.reserve(procCache.size());
    tuiData.topProcesses.clear();
    tuiData.topProcesses.reserve(procCache.size());
    for (const auto& sample : procCache) {
        SystemInfo::ProcData proc;
        proc.pid = sample.pid;
        proc.name = sample.name;
        proc.memoryBytes = sample.memoryBytes;
        proc.cpuPercent = static_cast<float>(sample.cpuPercent);
        sysInfo.topProcesses.push_back(std::move(proc));

        tcmt::TuiData::ProcessTopEntry row;
        row.pid = static_cast<pid_t>(sample.pid);
        row.name = sample.name;
        row.memoryBytes = sample.memoryBytes;
        row.cpuPercent = sample.cpuPercent;
        row.threads = sample.threads;
        row.cpuTimeMs = sample.cpuTimeMs;
        row.state = sample.state;
        row.user = sample.user;
        tuiData.topProcesses.push_back(std::move(row));
    }
#endif  // TCMT_LINUX
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
            // Log the transitions, not every pass: a count that drops from 15
            // to 8 after startup is the sensor list losing entries, which the
            // Temps panel then shows as "the extra sensors vanished".
            static std::size_t lastTempCount = static_cast<std::size_t>(-1);
            if (temps.size() != lastTempCount) {
                lastTempCount = temps.size();
                Logger::Info("TemperatureLoop: " + std::to_string(temps.size()) + " sensors");
            }
            data_.cpuTemperature = 0.0;
            data_.cpuPcoreTemperature = 0.0;
            data_.cpuEcoreTemperature = 0.0;
            data_.gpuTemperature = 0.0;

            for (const auto& [name, temp] : temps) {
                std::string lower = name;
                std::transform(lower.begin(), lower.end(), lower.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

                // Battery and power entries are not thermal sensors
                if (lower.find("battery") != std::string::npos) continue;
                if (lower.find("power") != std::string::npos) continue;
#ifdef TCMT_LINUX
                // On Linux fans share the sensor list (value = RPM) and disk
                // hwmon entries are not CPU/GPU temperatures. Both still reach
                // the sensor list; only the CPU/GPU attribution skips them.
                if (lower.find("fan") != std::string::npos) continue;
                if (lower.find("nvme") != std::string::npos ||
                    lower.find("drivetemp") != std::string::npos) {
                    continue;
                }
#endif

                bool isGpu = (lower.find("gpu") != std::string::npos ||
                              lower.find("tg") != std::string::npos ||
                              lower.find("graphics") != std::string::npos);

                if (isGpu) {
                    if (data_.gpuTemperature == 0.0)
                        data_.gpuTemperature = temp;
                    continue;
                }

                // P-core / E-core cluster temperature detection
                // Apple Silicon SMC reports separate sensors for each cluster:
                //   "P-core", "pACC", "P cluster", "performance core", etc.
                //   "E-core", "eACC", "E cluster", "efficiency core", etc.
                bool isPcore = (lower.find("pcore") != std::string::npos ||
                                lower.find("p_core") != std::string::npos ||
                                lower.find("p-core") != std::string::npos ||
                                lower.find("performance") != std::string::npos ||
                                (lower.find("p") != std::string::npos && lower.find("acc") != std::string::npos));
                bool isEcore = (lower.find("ecore") != std::string::npos ||
                                lower.find("e_core") != std::string::npos ||
                                lower.find("e-core") != std::string::npos ||
                                lower.find("efficiency") != std::string::npos ||
                                (lower.find("e") != std::string::npos && lower.find("acc") != std::string::npos));

                if (isPcore && data_.cpuPcoreTemperature == 0.0) {
                    data_.cpuPcoreTemperature = temp;
                } else if (isEcore && data_.cpuEcoreTemperature == 0.0) {
                    data_.cpuEcoreTemperature = temp;
                } else if (data_.cpuTemperature == 0.0) {
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
                // powermetrics globals (all mW — same unit as data_.*Power). Without
                // these the UI showed 0 W whenever direct IOReport mode was unavailable.
                double cp = GetPmCpuPower();
                double gp = GetPmGpuPower();
                double ap = GetPmAnePower();
                if (cp > 0) data_.cpuPower.store(cp);
                if (gp > 0) data_.gpuPower.store(gp);
                if (ap > 0) data_.anePower.store(ap);
            }
#endif
        } catch (const std::exception& e) {
            Logger::Error("TemperatureLoop: " + std::string(e.what()));
        }

        // 4× 采样基周期，下限 1000ms —— 温度那一路单次采集不便宜
        data_.readyMask.fetch_or(kReadyTemperature, std::memory_order_relaxed);
        SleepFor(st, ScaledSampleIntervalMs(data_, 4, 1000));
    }
}
