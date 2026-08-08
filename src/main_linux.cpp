// main_linux.cpp - Linux entry point for TCMT

#ifndef TCMT_LINUX
#error "This file should only be compiled for Linux (TCMT_LINUX defined)"
#endif

#include <chrono>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <thread>
#include <atomic>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <sys/types.h>
#include <sys/sysinfo.h>
#include <dirent.h>
#include <cstdio>
#include <fstream>
#include <sys/stat.h>

#include "core/cpu/CpuInfo.h"
#include "core/gpu/GpuInfo.h"
#include "core/memory/MemoryInfo.h"
#include "core/network/NetworkAdapter.h"
#include "core/os/OSInfo.h"
#include "core/power/PowerInfo.h"
#include "core/disk/DiskInfo.h"
#include "core/history/HistoryLogger.h"
#include "core/usb/UsbInfo.h"
#include "core/notifications/DeviceChangeNotifier.h"
#include "core/notifications/UserNotifier.h"
#include "core/MCP/MCPServer.h"
#include "core/IPC/IPCClient.h"
#include "core/DataStruct/DataStruct.h"
#include "core/temperature/TemperatureWrapper.h"
#include "core/coordinator/ModuleCoordinator.h"
#include "core/Utils/Logger.h"
#include "tui/TuiApp.h"

#include "core/Config/ConfigManager.h"
#include "core/wifi/WiFiInfo.h"
#include "core/bluetooth/BluetoothInfo.h"

// ======================== Signal Handling ========================
static std::atomic<bool> g_shouldExit{false};

static void SignalHandler(int sig) {
    (void)sig;
    g_shouldExit = true;
    static std::atomic<int> sigCount{0};
    if (++sigCount >= 2) {
        _exit(1);
    }
}

// ======================== Formatting Helpers ========================
static std::string FormatSize(uint64_t bytes) {
    const double gb = 1024.0 * 1024.0 * 1024.0;
    const double mb = 1024.0 * 1024.0;
    std::stringstream ss;
    ss << std::fixed << std::setprecision(1);
    if (bytes >= (uint64_t)gb) ss << (bytes / gb) << " GB";
    else if (bytes >= (uint64_t)mb) ss << (bytes / mb) << " MB";
    else ss << bytes << " B";
    return ss.str();
}

static int GetProcessCount() {
    DIR* proc = opendir("/proc");
    if (!proc) return 0;
    int count = 0;
    struct dirent* entry;
    while ((entry = readdir(proc)) != nullptr) {
        if (entry->d_type == DT_DIR) {
            bool isNum = true;
            for (const char* p = entry->d_name; *p; ++p) {
                if (*p < '0' || *p > '9') { isNum = false; break; }
            }
            if (isNum) count++;
        }
    }
    closedir(proc);
    return count;
}

// ======================== Main ========================
int main(int argc, char* argv[]) {

    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);

    try {
        Logger::Initialize("system_monitor.log");
        Logger::EnableConsoleOutput(false);
        Logger::SetLogLevel(LOG_INFO);
        Logger::Info("TCMT Linux Client starting (TUI mode)...");
    } catch (const std::exception& e) {
        std::cerr << "Logger init failed: " << e.what() << std::endl;
        return 1;
    }

    {
        ConfigManager cfg("system_monitor.json");
        if (cfg.Load()) {
            Logger::Info("Config loaded: " + cfg.GetPath());
            std::string logLevel = cfg.GetString("logging.level", "info");
            if (logLevel == "debug")
                Logger::SetLogLevel(LOG_DEBUG);
            else if (logLevel == "warning")
                Logger::SetLogLevel(LOG_WARNING);
        } else {
            Logger::Warn("No config file found, using defaults");
        }
    }

    // ======================== --json Mode ========================
    bool jsonMode = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--json") {
            jsonMode = true;
            break;
        }
    }

    if (jsonMode) {
        TemperatureWrapper::Initialize();

        const std::string tmpPath = "/tmp/tcmt_export.json";
        std::remove(tmpPath.c_str());
        ConfigManager cfg(tmpPath);
        cfg.Load();

        try {
            OSInfo os;
            cfg.SetString("os.version", os.GetVersion());
        } catch (...) {}

        try {
            auto cpu = std::make_unique<CpuInfo>();
            cfg.SetString("cpu.name", cpu->GetName());
            cfg.SetInt("cpu.cores.physical", cpu->GetLargeCores() + cpu->GetSmallCores());
            cfg.SetInt("cpu.cores.logical", cpu->GetTotalCores());
            cfg.SetDouble("cpu.usage", cpu->GetUsage());
        } catch (...) {}

        try {
            MemoryInfo mem;
            cfg.SetUint64("memory.total", mem.GetTotalPhysical());
            cfg.SetUint64("memory.available", mem.GetAvailablePhysical());
            cfg.SetUint64("memory.used", mem.GetTotalPhysical() - mem.GetAvailablePhysical());
        } catch (...) {}

        try {
            auto gpu = std::make_unique<GpuInfo>();
            const auto& gpus = gpu->GetGpuData();
            if (!gpus.empty()) {
                cfg.SetString("gpu.name",
                    std::string(gpus[0].name.begin(), gpus[0].name.end()));
                cfg.SetUint64("gpu.dedicatedMemory", gpus[0].dedicatedMemory);
                cfg.SetDouble("gpu.usage", gpus[0].usage);
            }
        } catch (...) {}

        try {
            NetworkAdapter net;
            const auto& adapters = net.GetAdapters();
            for (const auto& a : adapters) {
                nlohmann::json na;
                na["name"] = a.name;
                na["ip"] = a.ip;
                na["mac"] = a.mac;
                na["type"] = a.adapterType;
                na["speed"] = a.speed;
                na["downloadSpeed"] = a.downloadSpeed;
                na["uploadSpeed"] = a.uploadSpeed;
                cfg.AppendToArray("network.adapters", std::move(na));
            }
        } catch (...) {}

        try {
            DiskInfo disk;
            auto volumes = disk.GetDisks();
            for (const auto& v : volumes) {
                nlohmann::json dj;
                dj["label"] = v.label;
                dj["fileSystem"] = v.fileSystem;
                dj["total"] = v.totalSize;
                dj["used"] = v.usedSpace;
                cfg.AppendToArray("disks", std::move(dj));
            }
        } catch (...) {}

        try {
            auto temps = TemperatureWrapper::GetTemperatures();
            nlohmann::json tempObj = nlohmann::json::object();
            for (const auto& t : temps) {
                tempObj[t.first] = t.second;
            }
            cfg.SetJson("temperatures", std::move(tempObj));
        } catch (...) {}

        if (cfg.Save()) {
            std::ifstream in(tmpPath);
            if (in) {
                std::cout << in.rdbuf();
            }
        }
        std::cout << std::endl;
        std::remove(tmpPath.c_str());

        TemperatureWrapper::Cleanup();
        return 0;
    }

    // ======================== --mcp Mode ========================
    bool mcpMode = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--mcp") {
            mcpMode = true;
            break;
        }
    }
    if (mcpMode) {
        tcmt::mcp::MCPServer server;

        tcmt::ipc::IPCClient ipc;
        bool useIpc = ipc.Connect();
        if (useIpc) {
            Logger::Info("MCP: connected to running TCMT-M via IPC");
            ipc.ClosePipe();
        } else {
            Logger::Info("MCP: IPC unavailable, using direct hardware reads");
        }

        server.RegisterTool("get_cpu_status", "CPU usage, cores, frequency, temperature",
        [&ipc, useIpc]() -> nlohmann::json {
            nlohmann::json j;
            if (useIpc) {
                j["name"]    = ipc.ReadString("cpu/name").value_or("");
                j["usage"]   = ipc.ReadFloat64("cpu/usage").value_or(0.0);
                j["cores"]["physical"]    = ipc.ReadInt32("cpu/cores/physical").value_or(0);
                j["cores"]["performance"] = ipc.ReadInt32("cpu/cores/performance").value_or(0);
                j["cores"]["efficiency"]  = ipc.ReadInt32("cpu/cores/efficiency").value_or(0);
                j["frequencies"]["pCore"] = ipc.ReadFloat64("cpu/freq/pCore").value_or(0.0);
                j["frequencies"]["eCore"] = ipc.ReadFloat64("cpu/freq/eCore").value_or(0.0);
                j["temperature"] = ipc.ReadFloat64("cpu/temperature").value_or(0.0);
            } else {
                CpuInfo cpu;
                j["name"] = cpu.GetName();
                j["usage"] = cpu.GetUsage();
                j["cores"]["physical"] = cpu.GetLargeCores() + cpu.GetSmallCores();
                j["cores"]["performance"] = cpu.GetLargeCores();
                j["cores"]["efficiency"] = cpu.GetSmallCores();
                j["frequencies"]["pCore"] = cpu.GetLargeCoreSpeed();
                j["frequencies"]["eCore"] = cpu.GetSmallCoreSpeed();
                auto temps = TemperatureWrapper::GetTemperatures();
                for (const auto& [n, t] : temps)
                    if (n.find("CPU") != std::string::npos || n.find("cpu") != std::string::npos)
                        { j["temperature"] = t; break; }
            }
            return j;
        });

        server.RegisterTool("get_memory", "System memory statistics",
        [&ipc, useIpc]() -> nlohmann::json {
            nlohmann::json j;
            if (useIpc) {
                j["total"]     = ipc.ReadUInt64("memory/total").value_or(0);
                j["available"] = ipc.ReadUInt64("memory/available").value_or(0);
                j["used"]      = ipc.ReadUInt64("memory/used").value_or(0);
                j["compressed"] = ipc.ReadUInt64("memory/compressed").value_or(0);
            } else {
                MemoryInfo mem;
                j["total"] = mem.GetTotalPhysical();
                j["available"] = mem.GetAvailablePhysical();
                j["used"] = mem.GetTotalPhysical() - mem.GetAvailablePhysical();
            }
            return j;
        });

        server.RegisterTool("get_gpu_status", "GPU usage and memory",
        [&ipc, useIpc]() -> nlohmann::json {
            nlohmann::json j;
            if (useIpc) {
                j["name"]  = ipc.ReadString("gpu/0/name").value_or("");
                j["usage"] = ipc.ReadFloat64("gpu/0/usage").value_or(0.0);
                j["memory"] = ipc.ReadUInt64("gpu/0/memory").value_or(0);
                j["temperature"] = ipc.ReadFloat64("gpu/0/temperature").value_or(0.0);
            } else {
                GpuInfo gpu;
                const auto& gpus = gpu.GetGpuData();
                if (!gpus.empty()) {
                    j["name"] = std::string(gpus[0].name.begin(), gpus[0].name.end());
                    j["usage"] = gpus[0].usage;
                    j["memory"] = gpus[0].dedicatedMemory;
                }
                auto temps = TemperatureWrapper::GetTemperatures();
                for (const auto& [n, t] : temps)
                    if (n.find("GPU") != std::string::npos || n.find("gpu") != std::string::npos)
                        { j["temperature"] = t; break; }
            }
            return j;
        });

        server.RegisterTool("get_disk_health", "Logical volumes + physical disk SMART",
        [&ipc, useIpc]() -> nlohmann::json {
            nlohmann::json j;
            if (useIpc) {
                for (int i = 0; i < 8; i++) {
                    auto label = ipc.ReadString("disk/" + std::to_string(i) + "/label");
                    if (!label || label->empty()) continue;
                    nlohmann::json dv;
                    dv["label"] = *label;
                    dv["fs"]    = ipc.ReadString("disk/" + std::to_string(i) + "/fs").value_or("");
                    dv["total"] = ipc.ReadUInt64("disk/" + std::to_string(i) + "/total").value_or(0);
                    dv["used"]  = ipc.ReadUInt64("disk/" + std::to_string(i) + "/used").value_or(0);
                    j["volumes"].push_back(dv);
                }
            } else {
                DiskInfo disk;
                for (const auto& v : disk.GetDisks()) {
                    nlohmann::json dv;
                    dv["label"] = v.label;
                    dv["fs"] = v.fileSystem;
                    dv["total"] = v.totalSize;
                    dv["used"] = v.usedSpace;
                    j["volumes"].push_back(dv);
                }
            }
            return j;
        });

        server.RegisterTool("get_battery_info", "Battery percentage and AC status",
        [&ipc, useIpc]() -> nlohmann::json {
            nlohmann::json j;
            if (useIpc) {
                j["percent"]  = ipc.ReadInt32("battery/percent").value_or(-1);
                j["acOnline"] = ipc.ReadBool("battery/acOnline").value_or(false);
            } else {
                PowerInfo power;
                power.Detect();
                j["acOnline"] = power.acOnline;
                j["percent"] = !power.batteries.empty() ? (int)power.batteries[0].chargePercent : -1;
            }
            return j;
        });

        server.RegisterTool("get_network_info", "Network adapters and throughput",
        [&ipc, useIpc]() -> nlohmann::json {
            nlohmann::json j;
            if (useIpc) {
                for (int i = 0; i < 4; i++) {
                    auto name = ipc.ReadString("net/" + std::to_string(i) + "/name");
                    if (!name || name->empty()) continue;
                    nlohmann::json na;
                    na["name"] = *name;
                    na["ip"]   = ipc.ReadString("net/" + std::to_string(i) + "/ip").value_or("");
                    na["mac"]  = ipc.ReadString("net/" + std::to_string(i) + "/mac").value_or("");
                    na["type"] = ipc.ReadString("net/" + std::to_string(i) + "/type").value_or("");
                    na["speed"] = ipc.ReadUInt64("net/" + std::to_string(i) + "/speed").value_or(0);
                    na["downloadSpeed"] = ipc.ReadUInt64("net/" + std::to_string(i) + "/downloadSpeed").value_or(0);
                    na["uploadSpeed"]   = ipc.ReadUInt64("net/" + std::to_string(i) + "/uploadSpeed").value_or(0);
                    j["adapters"].push_back(na);
                }
            } else {
                NetworkAdapter net;
                for (const auto& a : net.GetAdapters()) {
                    if (a.ip.empty()) continue;
                    nlohmann::json na;
                    na["name"] = a.name;
                    na["ip"] = a.ip;
                    na["mac"] = a.mac;
                    na["type"] = a.adapterType;
                    na["speed"] = a.speed;
                    na["downloadSpeed"] = a.downloadSpeed;
                    na["uploadSpeed"] = a.uploadSpeed;
                    j["adapters"].push_back(na);
                }
            }
            return j;
        });

        server.RegisterTool("get_temperatures", "All temperature sensors",
        [&ipc, useIpc]() -> nlohmann::json {
            nlohmann::json j;
            if (useIpc) {
                for (int i = 0; i < 10; i++) {
                    auto name = ipc.ReadString("sensor/" + std::to_string(i) + "/name");
                    if (!name || name->empty()) continue;
                    auto val = ipc.ReadFloat32("sensor/" + std::to_string(i) + "/value");
                    if (val) j["sensors"].push_back({{"name", *name}, {"value", *val}});
                }
            } else {
                for (const auto& [name, value] : TemperatureWrapper::GetTemperatures())
                    j["sensors"].push_back({{"name", name}, {"value", value}});
            }
            return j;
        });

        server.RegisterTool("get_system_info", "OS version and hardware summary",
        [&ipc, useIpc]() -> nlohmann::json {
            nlohmann::json j;
            if (useIpc) {
                j["os"]   = ipc.ReadString("os/version").value_or("");
                j["cpu"]  = ipc.ReadString("cpu/name").value_or("");
                j["cores"] = ipc.ReadInt32("cpu/cores/physical").value_or(0);
                j["memoryTotal"] = ipc.ReadUInt64("memory/total").value_or(0);
            } else {
                OSInfo os; CpuInfo cpu; MemoryInfo mem;
                j["os"] = os.GetVersion();
                j["cpu"] = cpu.GetName();
                j["cores"] = cpu.GetTotalCores();
                j["memoryTotal"] = mem.GetTotalPhysical();
            }
            return j;
        });

        server.Run();
        TemperatureWrapper::Cleanup();
        return 0;
    }

    // Initialize temperature wrapper
    TemperatureWrapper::Initialize();
    Logger::Debug("TemperatureWrapper initialized");

    // Cache static system info
    OSInfo os;
    Logger::Debug("OS: " + os.GetVersion());

    std::unique_ptr<CpuInfo> cpuInfo;
    try {
        cpuInfo = std::make_unique<CpuInfo>();
        Logger::Debug("CPU: " + cpuInfo->GetName()
                   + " (" + std::to_string(cpuInfo->GetTotalCores()) + " cores)");
    } catch (const std::exception& e) {
        Logger::Error("CpuInfo init failed: " + std::string(e.what()));
    }

    std::unique_ptr<GpuInfo> gpuInfo;
    try {
        gpuInfo = std::make_unique<GpuInfo>();
        for (const auto& gpu : gpuInfo->GetGpuData()) {
            std::string gname(gpu.name.begin(), gpu.name.end());
            Logger::Debug("GPU: " + gname + " mem=" + FormatSize(gpu.dedicatedMemory));
        }
    } catch (const std::exception& e) {
        Logger::Error("GpuInfo init failed: " + std::string(e.what()));
    }

    // Initial USB detection
    try {
        UsbInfo usb;
        usb.Detect();
        const auto& devs = usb.GetDevices();
        Logger::Info("USB: initial scan — " + std::to_string(devs.size()) + " device(s) found");
        for (size_t di = 0; di < std::min(devs.size(), size_t(5)); ++di)
            Logger::Debug("  " + devs[di].name + " VID:" + std::to_string(devs[di].vid)
                        + " PID:" + std::to_string(devs[di].pid));
    } catch (...) { Logger::Debug("USB: initial scan failed"); }

    // Start TUI
    tcmt::TuiApp tuiApp;
    tuiApp.SetLogBuffer(&Logger::GetTuiBuffer());
    tuiApp.Start();

    // Start history logger (SQLite)
    HistoryLogger historyLogger;
    historyLogger.SetRetentionDays(30);
    std::string dbPath = std::string(getenv("HOME") ? getenv("HOME") : "/tmp") + "/.tcmt/history.db";
    {
        std::string dir = dbPath.substr(0, dbPath.find_last_of('/'));
        mkdir(dir.c_str(), 0755);
    }
    if (historyLogger.Initialize(dbPath)) {
        Logger::Info("HistoryLogger started: " + dbPath);
    } else {
        Logger::Warn("HistoryLogger failed to initialize");
    }
    Logger::Info("TUI started");

    // Static info caching
    static std::string cachedCpuName;
    static int cachedTotalCores = 0;
    static int cachedPCores = 0;
    static int cachedECores = 0;
    static bool cachedHT = false;
    static bool cachedVirt = false;
    if (cpuInfo) {
        cachedCpuName = cpuInfo->GetName();
        cachedTotalCores = cpuInfo->GetTotalCores();
        cachedPCores = cpuInfo->GetLargeCores();
        cachedECores = cpuInfo->GetSmallCores();
        cachedHT = cpuInfo->IsHyperThreadingEnabled();
        cachedVirt = cpuInfo->IsVirtualizationEnabled();
    }

    // Start ModuleCoordinator background collection threads
    ModuleCoordinator coordinator;
    coordinator.Start();

    int loopCounter = 1;

    while (!g_shouldExit.load() && tuiApp.IsRunning()) {
        try {
            auto loopStart = std::chrono::high_resolution_clock::now();

            // === Build TuiData snapshot ===
            tcmt::TuiData data;
            data.osVersion = os.GetVersion();
            data.hardwareModel = os.GetModel();
            data.cpuName = cachedCpuName;
            data.physicalCores = cachedTotalCores;
            data.performanceCores = cachedPCores;
            data.efficiencyCores = cachedECores;

            // Coordinator snapshot fills CPU, Memory, Disk, Network, Temperature, Power
            {
                SystemInfo sysInfo;
                coordinator.Snapshot(sysInfo, data);

                data.cpuUsage = sysInfo.cpuUsage;
                data.pCoreFreq = sysInfo.performanceCoreFreq;
                data.eCoreFreq = sysInfo.efficiencyCoreFreq;
                data.totalMemory = sysInfo.totalMemory;
                data.usedMemory = sysInfo.usedMemory;
                data.availableMemory = sysInfo.availableMemory;
                data.compressedMemory = sysInfo.compressedMemory;
                data.swapUsed = sysInfo.swapUsed;
                data.swapTotal = sysInfo.swapTotal;
                data.ramSpeed = sysInfo.ramSpeed;
                strncpy(data.ramType, sysInfo.ramType, sizeof(data.ramType) - 1);
            }

            // GPU
            if (gpuInfo) {
                gpuInfo->RefreshUsage();
                const auto& gpus = gpuInfo->GetGpuData();
                for (const auto& gpu : gpus) {
                    if (data.gpuName.empty()) {
                        std::string gn(gpu.name.begin(), gpu.name.end());
                        data.gpuName = gn;
                        data.gpuMemory = gpu.dedicatedMemory;
                    }
                    if (data.gpuUsage == 0.0 && gpu.usage > 0.0) {
                        data.gpuUsage = gpu.usage;
                    }
                }
            }

            // USB detection (every 10 seconds)
            static int usbCheckCounter = 0;
            static size_t prevUsbCount = 0;
            static DeviceChangeNotifier s_usbNotify(DeviceChangeNotifier::USB);
            static DeviceChangeNotifier s_hubNotify(DeviceChangeNotifier::USB_Hub);
            if (s_usbNotify.Poll() || s_hubNotify.Poll()) usbCheckCounter = 20;
            if (++usbCheckCounter >= 20) {
                usbCheckCounter = 0;
                try {
                    UsbInfo usb;
                    usb.Detect();
                    const auto& devs = usb.GetDevices();
                    Logger::Debug("USB: scan — " + std::to_string(devs.size()) + " device(s)");
                    if (devs.size() != prevUsbCount) {
                        if (devs.empty())
                            Logger::Info("USB: all devices removed");
                        else
                            Logger::Info("USB: " + std::to_string(devs.size()) + " device(s)");
                        prevUsbCount = devs.size();
                    }
                } catch (...) {}
            }

            // WiFi & Bluetooth (every ~3 seconds)
            static int wifiBtCounter = 0;
            static WiFiInfo s_wifi;
            static BluetoothInfo s_bt;
            if (++wifiBtCounter >= 6) {
                wifiBtCounter = 0;
                try { s_wifi.Detect(); } catch (...) {}
                try { s_bt.Detect(); } catch (...) {}
            }
            {
                const auto& wd = s_wifi.GetData();
                data.hasWiFi = wd.powerOn || wd.isConnected;
                data.wifiSSID = wd.ssid;
                data.wifiBSSID = wd.bssid;
                data.wifiRSSI = wd.rssi;
                data.wifiChannel = wd.channel;
                data.wifiSecurity = wd.security;
                data.wifiBand = wd.band;
                data.wifiTxRate = wd.txRate;
            }
            {
                const auto& bd = s_bt.GetData();
                data.hasBluetooth = bd.adapter.detected;
                data.btPowerOn = bd.adapter.powerOn;
                data.btDeviceCount = static_cast<int>(bd.devices.size());
            }

            // System uptime / load / process count
            {
                struct sysinfo si;
                if (sysinfo(&si) == 0) {
                    data.uptimeSeconds = si.uptime;
                }
                double load[3];
                if (getloadavg(load, 3) == 3) {
                    data.loadAvg1 = load[0];
                    data.loadAvg5 = load[1];
                    data.loadAvg15 = load[2];
                }
                data.processCount = GetProcessCount();
            }

            // Timestamp
            auto now = std::chrono::system_clock::now();
            auto time = std::chrono::system_clock::to_time_t(now);
            std::tm tm;
            localtime_r(&time, &tm);
            char buf[64];
            std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
            data.timestamp = buf;

            // Physical disks (SMART) — refresh every 60 seconds
            static std::vector<PhysicalDiskSmartData> cachedSmart;
            static auto lastSmartRefresh = std::chrono::steady_clock::now() - std::chrono::seconds(61);
            auto nowSt = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(nowSt - lastSmartRefresh).count() >= 60) {
                try {
                    SystemInfo tmp;
                    DiskInfo().CollectSmartData(tmp);
                    if (!tmp.physicalDisks.empty())
                        cachedSmart = std::move(tmp.physicalDisks);
                    lastSmartRefresh = nowSt;
                } catch (...) {}
            }
            data.physicalDisks.clear();
            data.physicalDisks.reserve(cachedSmart.size());
            auto w2u = [](const WCHAR* s, int maxLen) -> std::string {
                std::string out;
                for (int i = 0; i < maxLen && s[i] != u'\0'; ++i) {
                    uint16_t cp = static_cast<uint16_t>(s[i]);
                    if (cp < 0x80) out += (char)cp;
                    else if (cp < 0x800) { out += (char)(0xC0|(cp>>6)); out += (char)(0x80|(cp&0x3F)); }
                    else { out += (char)(0xE0|(cp>>12)); out += (char)(0x80|((cp>>6)&0x3F)); out += (char)(0x80|(cp&0x3F)); }
                }
                return out;
            };
            for (const auto& src : cachedSmart) {
                tcmt::TuiData::PhysicalDiskInfo pi;
                pi.model = w2u(src.model, 63);
                pi.serial = w2u(src.serialNumber, 63);
                pi.capacity = src.capacity;
                pi.temperature = src.temperature;
                pi.healthPct = src.healthPercentage;
                pi.smartSupported = src.smartSupported;
                pi.powerOnHours = src.powerOnHours;
                pi.wearLeveling = src.wearLeveling;
                pi.diskType = w2u(src.diskType, 15);
                pi.interfaceType = w2u(src.interfaceType, 15);
                pi.attributes.clear();
                pi.attributes.reserve(src.attributeCount);
                for (int ai = 0; ai < src.attributeCount; ++ai) {
                    tcmt::TuiData::SmAttributeInfo ai2;
                    ai2.id = src.attributes[ai].id;
                    ai2.current = src.attributes[ai].current;
                    ai2.worst = src.attributes[ai].worst;
                    ai2.rawValue = src.attributes[ai].rawValue;
                    ai2.name = w2u(src.attributes[ai].name, 63);
                    pi.attributes.push_back(std::move(ai2));
                }
                data.physicalDisks.push_back(std::move(pi));
            }
            for (size_t di = 0; di < cachedSmart.size(); ++di) {
                if (cachedSmart[di].temperature > 0) {
                    std::string label = w2u(cachedSmart[di].model, 63);
                    if (label.empty()) label = "Disk";
                    data.temperatures.push_back({label, cachedSmart[di].temperature});
                }
            }

            // Update TUI
            tuiApp.UpdateData(data);

            // Sleep
            auto loopEnd = std::chrono::high_resolution_clock::now();
            int loopMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                loopEnd - loopStart).count();
            int sleepMs = std::max(500 - loopMs, 50);
            for (int s = 0; s < 10 && !g_shouldExit.load(); ++s) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }

            // Push sensor snapshots to history logger
            if (historyLogger.IsRunning()) {
                auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                std::vector<SensorSnapshot> snapshots;
                snapshots.push_back({"cpu/usage", data.cpuUsage, "%", (uint64_t)nowMs});
                snapshots.push_back({"cpu/temperature", data.cpuTemp, "C", (uint64_t)nowMs});
                snapshots.push_back({"gpu/usage", data.gpuUsage, "%", (uint64_t)nowMs});
                snapshots.push_back({"gpu/temperature", data.gpuTemp, "C", (uint64_t)nowMs});
                if (data.totalMemory > 0) {
                    double memPct = 100.0 * data.usedMemory / data.totalMemory;
                    snapshots.push_back({"memory/percent", memPct, "%", (uint64_t)nowMs});
                }
                if (data.batteryPercent >= 0) {
                    snapshots.push_back({"battery/percent", (double)data.batteryPercent, "%", (uint64_t)nowMs});
                }
                historyLogger.WriteBatch(snapshots);
            }

            loopCounter++;
        }
        catch (const std::exception& e) {
            Logger::Error("Loop error: " + std::string(e.what()));
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        catch (...) {
            Logger::Error("Loop unknown error");
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    Logger::Info("Exiting, cleaning up...");
    tuiApp.Stop();
    TemperatureWrapper::Cleanup();
    historyLogger.Shutdown();
    Logger::Info("HistoryLogger stopped");
    Logger::Info("Done.");
    return 0;
}
