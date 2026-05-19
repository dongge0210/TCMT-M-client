// main_mac.cpp - macOS entry point for TCMT
// Conditionally compiled via CMake (not for Windows)

#ifndef TCMT_MACOS
#error "This file should only be compiled for macOS (TCMT_MACOS defined)"
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
#include <mach/mach_time.h>
#include <mach/mach.h>
#include <mach/vm_statistics.h>
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
#include "core/wifi/WiFiInfo.h"
#include "core/bluetooth/BluetoothInfo.h"
#include "core/DeviceChangeNotifier.h"
#include "core/MCP/MCPServer.h"
#include "core/IPC/IPCClient.h"
#include "core/DataStruct/DataStruct.h"
#include "core/IPC/IPCServer.h"
#include "core/temperature/TemperatureWrapper.h"
#include "core/ModuleCoordinator.h"
#include "core/Utils/Logger.h"
#include "tui/TuiApp.h"

// Config management (wraps CPP-parsers / nlohmann/json internally)
#include "core/Config/ConfigManager.h"
#include <fstream>
#include <cstdio>

// ======================== Signal Handling ========================
static std::atomic<bool> g_shouldExit{false};

static void SignalHandler(int sig) {
    (void)sig;
    g_shouldExit = true;
    // Force fast exit on second Ctrl+C
    static std::atomic<int> sigCount{0};
    if (++sigCount >= 2) {
        _exit(1);
    }
}

// Build the IPCDataBlock field schema shared between TUI and MCP paths.
static void BuildIPCDataBlockSchema(tcmt::ipc::SchemaHeader& header,
                                     std::vector<tcmt::ipc::FieldDef>& fields) {
    using FT = tcmt::ipc::FieldType;
    header.totalSize = sizeof(tcmt::ipc::IPCDataBlock);
    auto add = [&](const char* name, uint32_t offset, uint16_t size, FT type, uint32_t count = 0) {
        tcmt::ipc::FieldDef f{};
        f.offset = offset; f.size = size; f.count = count;
        f.type = static_cast<uint8_t>(type);
        std::strncpy(f.name, name, tcmt::ipc::IPC_FIELD_NAME_LEN - 1);
        fields.push_back(f);
    };
    auto addS  = [&](const char* n, uint32_t o, uint16_t s) { add(n, o, s, FT::String); };
    auto addB  = [&](const char* n, uint32_t o) { add(n, o, 1, FT::Bool); };
    auto addI  = [&](const char* n, uint32_t o) { add(n, o, 4, FT::Int32); };
    auto addU8 = [&](const char* n, uint32_t o) { add(n, o, 1, FT::UInt8); };
    auto addU64= [&](const char* n, uint32_t o) { add(n, o, 8, FT::UInt64); };
    auto addF  = [&](const char* n, uint32_t o) { add(n, o, 4, FT::Float32); };
    auto addF64= [&](const char* n, uint32_t o) { add(n, o, 8, FT::Float64); };
    using B = tcmt::ipc::IPCDataBlock;

    // CPU
    addS("cpu/name",              offsetof(B, cpuName), 64);
    addU8("cpu/cores/physical",    offsetof(B, physicalCores));
    addU8("cpu/cores/logical",     offsetof(B, logicalCores));
    addU8("cpu/cores/performance", offsetof(B, performanceCores));
    addU8("cpu/cores/efficiency",  offsetof(B, efficiencyCores));
    addF("cpu/usage",             offsetof(B, cpuUsage));
    addF("cpu/freq/pCore",        offsetof(B, pCoreFreq));
    addF("cpu/freq/eCore",        offsetof(B, eCoreFreq));
    addF("cpu/temperature",       offsetof(B, cpuTemp));
    addB("cpu/hyperThreading",    offsetof(B, hyperThreading));
    addB("cpu/virtualization",    offsetof(B, virtualization));
    addF("cpu/sampleIntervalMs",  offsetof(B, cpuSampleIntervalMs));

    // Memory
    addU64("memory/total",        offsetof(B, totalMemory));
    addU64("memory/used",         offsetof(B, usedMemory));
    addU64("memory/available",    offsetof(B, availableMemory));
    addU64("memory/compressed",   offsetof(B, compressedMemory));

    // Battery / Power
    addI("battery/percent",       offsetof(B, batteryPercent));
    addB("battery/acOnline",      offsetof(B, acOnline));

    // OS
    addS("os/version",            offsetof(B, osVersion), 128);

    // GPU
    addS("gpu/0/name",            offsetof(B, gpuName), 48);
    addS("gpu/0/brand",           offsetof(B, gpuBrand), 32);
    addU64("gpu/0/memory",        offsetof(B, gpuMemory));
    addF("gpu/0/memoryPercent",   offsetof(B, gpuMemoryPercent));
    addF("gpu/0/usage",           offsetof(B, gpuUsage));
    addF("gpu/0/temperature",     offsetof(B, gpuTemp));
    addB("gpu/0/isVirtual",       offsetof(B, gpuIsVirtual));

    // Disks (up to 4)
    for (int i = 0; i < 4; ++i) {
        char p[32]; snprintf(p, sizeof(p), "disk/%d/", i);
        uint32_t base = offsetof(B, disks) + i * sizeof(B::DiskSlot);
        addS((std::string(p)+"label").c_str(), base + offsetof(B::DiskSlot, label), 32);
        addU64((std::string(p)+"total").c_str(), base + offsetof(B::DiskSlot, totalSize));
        addU64((std::string(p)+"used").c_str(),  base + offsetof(B::DiskSlot, usedSpace));
        addU64((std::string(p)+"free").c_str(),  base + offsetof(B::DiskSlot, freeSpace));
        addS((std::string(p)+"fs").c_str(),      base + offsetof(B::DiskSlot, fs), 16);
    }

    // Network adapters (up to 4)
    for (int i = 0; i < 4; ++i) {
        char p[32]; snprintf(p, sizeof(p), "net/%d/", i);
        uint32_t base = offsetof(B, adapters) + i * sizeof(B::NetSlot);
        addS((std::string(p)+"name").c_str(),   base + offsetof(B::NetSlot, name), 32);
        addS((std::string(p)+"ip").c_str(),     base + offsetof(B::NetSlot, ip), 16);
        addS((std::string(p)+"mac").c_str(),    base + offsetof(B::NetSlot, mac), 18);
        addS((std::string(p)+"type").c_str(),   base + offsetof(B::NetSlot, type), 16);
        addU64((std::string(p)+"speed").c_str(),  base + offsetof(B::NetSlot, speed));
        addU64((std::string(p)+"downloadSpeed").c_str(), base + offsetof(B::NetSlot, downloadSpeed));
        addU64((std::string(p)+"uploadSpeed").c_str(),   base + offsetof(B::NetSlot, uploadSpeed));
    }

    // Temperatures (up to 10)
    for (int i = 0; i < 10; ++i) {
        char p[32]; snprintf(p, sizeof(p), "sensor/%d/", i);
        uint32_t base = offsetof(B, temperatures) + i * sizeof(B::TempSlot);
        addS((std::string(p)+"name").c_str(), base + offsetof(B::TempSlot, name), 64);
        addF((std::string(p)+"value").c_str(), base + offsetof(B::TempSlot, value));
    }

    // Physical disks (up to 8)
    for (int i = 0; i < 8; ++i) {
        char p[32]; snprintf(p, sizeof(p), "phys/%d/", i);
        uint32_t base = offsetof(B, physicalDisks) + i * sizeof(B::PhysDiskSlot);
        addS((std::string(p)+"model").c_str(),       base + offsetof(B::PhysDiskSlot, model), 64);
        addS((std::string(p)+"serial").c_str(),      base + offsetof(B::PhysDiskSlot, serial), 64);
        addU64((std::string(p)+"capacity").c_str(),   base + offsetof(B::PhysDiskSlot, capacity));
        addS((std::string(p)+"interface").c_str(),    base + offsetof(B::PhysDiskSlot, interfaceType), 16);
        addF((std::string(p)+"temperature").c_str(),  base + offsetof(B::PhysDiskSlot, temperature));
        addF((std::string(p)+"health").c_str(),       base + offsetof(B::PhysDiskSlot, healthPercent));
        addB((std::string(p)+"smartSupported").c_str(), base + offsetof(B::PhysDiskSlot, smartSupported));
    }

    // WiFi
    addS("wifi/ssid",        offsetof(B, wifi.ssid), 32);
    addI("wifi/rssi",        offsetof(B, wifi.rssi));
    addI("wifi/channel",     offsetof(B, wifi.channel));
    addS("wifi/security",    offsetof(B, wifi.security), 16);
    addB("wifi/powerOn",     offsetof(B, wifi.powerOn));
    addB("wifi/isConnected", offsetof(B, wifi.isConnected));
    // Bluetooth
    addB("bluetooth/powerOn",     offsetof(B, bluetooth.powerOn));
    addI("bluetooth/deviceCount", offsetof(B, bluetooth.deviceCount));
    addS("bluetooth/name",        offsetof(B, bluetooth.name), 64);
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

// ======================== Main ========================
int main(int argc, char* argv[]) {

    // Setup signal handlers
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);
    std::signal(SIGHUP, SignalHandler);

    try {
        Logger::Initialize("system_monitor.log");
        Logger::EnableConsoleOutput(false);  // TUI takes over console
        Logger::SetLogLevel(LOG_INFO);
        Logger::Info("TCMT macOS Client starting (TUI mode)...");
    } catch (const std::exception& e) {
        std::cerr << "Logger init failed: " << e.what() << std::endl;
        return 1;
    }

    // Load application config (config.json)
    {
        ConfigManager cfg("system_monitor.json");
        if (cfg.Load()) {
            Logger::Info("Config loaded: " + cfg.GetPath());
            // Apply config-driven settings
            std::string logLevel = cfg.GetString("logging.level", "info");
            if (logLevel == "debug")
                Logger::SetLogLevel(LOG_DEBUG);
            else if (logLevel == "warning")
                Logger::SetLogLevel(LOG_WARNING);

            int refreshRate = cfg.GetInt("display.refreshRate", 500);
            // (used later for sleep interval)
            (void)refreshRate;
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
        // One-shot JSON output for scripting
        TemperatureWrapper::Initialize();

        // Build JSON via ConfigManager, then dump to stdout via temp file
        const std::string tmpPath = "/tmp/tcmt_export.json";
        std::remove(tmpPath.c_str());  // ensure clean start
        ConfigManager cfg(tmpPath);
        cfg.Load();  // starts with empty json::object

        // OS
        try {
            OSInfo os;
            cfg.SetString("os.version", os.GetVersion());
        } catch (...) {}

        // CPU
        try {
            auto cpu = std::make_unique<CpuInfo>();
            cfg.SetString("cpu.name", cpu->GetName());
            cfg.SetInt("cpu.cores.physical", cpu->GetLargeCores() + cpu->GetSmallCores());
            cfg.SetInt("cpu.cores.logical", cpu->GetTotalCores());
            cfg.SetDouble("cpu.usage", cpu->GetUsage());
        } catch (...) {}

        // Memory
        try {
            MemoryInfo mem;
            cfg.SetUint64("memory.total", mem.GetTotalPhysical());
            cfg.SetUint64("memory.available", mem.GetAvailablePhysical());
            cfg.SetUint64("memory.used", mem.GetTotalPhysical() - mem.GetAvailablePhysical());
        } catch (...) {}

        // GPU
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

        // Network
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

        // Disks
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

        // Temperatures
        try {
            auto temps = TemperatureWrapper::GetTemperatures();
            nlohmann::json tempObj = nlohmann::json::object();
            for (const auto& t : temps) {
                tempObj[t.first] = t.second;
            }
            cfg.SetJson("temperatures", std::move(tempObj));
        } catch (...) {}

        // Save to temp file, read back, print to stdout
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

        // Try IPC client first — like Avalonia, reuse running TCMT instance
        tcmt::ipc::IPCClient ipc;
        bool useIpc = ipc.Connect();
        if (useIpc) {
            Logger::Info("MCP: connected to running TCMT-M via IPC");
            ipc.ClosePipe(); // free server slot for other clients (Avalonia)
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
                for (int i = 0; i < 8; i++) {
                    auto model = ipc.ReadString("phys/" + std::to_string(i) + "/model");
                    if (!model || model->empty()) continue;
                    nlohmann::json pj;
                    pj["model"] = *model;
                    pj["capacity"] = ipc.ReadUInt64("phys/" + std::to_string(i) + "/capacity").value_or(0);
                    pj["smart"] = ipc.ReadBool("phys/" + std::to_string(i) + "/smartSupported").value_or(false);
                    j["physical"].push_back(pj);
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

    // Initialize temperature wrapper (macOS: limited support)
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

    // Initialize IPC server (schema-driven pipeline for C# Avalonia)
    tcmt::ipc::IPCServer ipcServer;
    {
        tcmt::ipc::SchemaHeader schemaHdr;
        std::vector<tcmt::ipc::FieldDef> fields;
        BuildIPCDataBlockSchema(schemaHdr, fields);
        ipcServer.UpdateSchema(schemaHdr, fields);
    }
    if (ipcServer.Start()) {
        Logger::Info("IPC server started on " + std::string(tcmt::ipc::IPC_SOCK_PATH));
    } else {
        Logger::Warn("IPC server start failed: " + ipcServer.GetLastError());
    }

    // Initial USB detection (startup scan)
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
    std::string dbPath = getenv("HOME") ? std::string(getenv("HOME")) + "/.tcmt/history.db" : "/tmp/tcmt_history.db";
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
            data.connectionCount = ipcServer.GetClientCount();
            auto ct = ipcServer.GetClientTypes();
            data.clientTypes.clear();
            for (auto t : ct) data.clientTypes.push_back(static_cast<uint8_t>(t));
            static bool hadConn = false;
            static std::string connSinceStr;
            if (data.connectionCount > 0 && !hadConn) {
                auto t = std::time(nullptr);
                char buf[16];
                std::strftime(buf, sizeof(buf), "%H:%M:%S", std::localtime(&t));
                connSinceStr = buf;
                hadConn = true;
            } else if (data.connectionCount == 0) {
                hadConn = false;
                connSinceStr.clear();
            }
            data.connectionSince = connSinceStr;
            data.cpuName = cachedCpuName;
            data.physicalCores = cachedTotalCores;
            data.performanceCores = cachedPCores;
            data.efficiencyCores = cachedECores;

            // Coordinator snapshot fills CPU, Memory, Disk, Network, Temperature, Power
            {
                SystemInfo sysInfo;
                coordinator.Snapshot(sysInfo, data);

                // Copy remaining fields that Snapshot doesn't set directly on TuiData
                data.cpuUsage = sysInfo.cpuUsage;
                data.pCoreFreq = sysInfo.performanceCoreFreq;
                data.eCoreFreq = sysInfo.efficiencyCoreFreq;
                data.totalMemory = sysInfo.totalMemory;
                data.usedMemory = sysInfo.usedMemory;
                data.availableMemory = sysInfo.availableMemory;
                data.compressedMemory = sysInfo.compressedMemory;
            }

            // GPU (coordinator doesn't have a GPU loop — keep inline)
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

            // WiFi & Bluetooth (every ~3 seconds)
            static int wbCtr = 0;
            static WiFiInfo s_wifi;
            static DeviceChangeNotifier s_usbNotify(DeviceChangeNotifier::USB);
            static DeviceChangeNotifier s_btNotify(DeviceChangeNotifier::Bluetooth);
            static BluetoothInfo s_bt;
            if (++wbCtr >= 6) { wbCtr = 0;
                try { s_wifi.Detect(); } catch (...) {}
                if (s_btNotify.Poll()) { try { s_bt.Detect(); } catch (...) {} }
            }
            {
              const auto& wd = s_wifi.GetData();
              data.hasWiFi = wd.powerOn;
              data.wifiSSID = wd.ssid;
              data.wifiBSSID = wd.bssid;
              data.wifiRSSI = wd.rssi;
              data.wifiChannel = wd.channel;
              data.wifiSecurity = wd.security;
              data.wifiTxRate = wd.txRate;
              const auto& bd = s_bt.GetData();
              data.hasBluetooth = bd.adapter.detected; // show if adapter hardware detected
              data.btPowerOn = bd.adapter.powerOn;
              data.btDeviceCount = static_cast<int>(bd.devices.size());
            }

            // Timestamp
            auto now = std::chrono::system_clock::now();
            auto time = std::chrono::system_clock::to_time_t(now);
            std::tm tm;
            localtime_r(&time, &tm);
            char buf[64];
            std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
            data.timestamp = buf;

            // Update TUI
            tuiApp.UpdateData(data);

            // Write to IPC shared memory (schema-driven, for C# Avalonia)
                if (ipcServer.IsRunning()) {
                    auto* b = static_cast<tcmt::ipc::IPCDataBlock*>(ipcServer.GetShmPtr());
                    if (b) {
                        // seqlock: mark write in progress (odd)
                        b->writeSequence++;
                        std::atomic_thread_fence(std::memory_order_release);
                        // CPU
                        std::strncpy(b->cpuName, data.cpuName.c_str(), 63);
                        b->cpuName[63] = '\0';
                        b->physicalCores = static_cast<uint8_t>(data.physicalCores);
                        b->logicalCores = static_cast<uint8_t>(data.physicalCores); // Apple Silicon: logical=physical
                        b->performanceCores = static_cast<uint8_t>(data.performanceCores);
                        b->efficiencyCores = static_cast<uint8_t>(data.efficiencyCores);
                        b->cpuUsage = static_cast<float>(data.cpuUsage);
                        b->pCoreFreq = static_cast<float>(data.pCoreFreq);
                        b->eCoreFreq = static_cast<float>(data.eCoreFreq);
                        b->cpuTemp = static_cast<float>(data.cpuTemp);
                        b->hyperThreading = false;
                        b->virtualization = false;
                        b->cpuSampleIntervalMs = 500.0f;
                        // Memory
                        b->totalMemory = data.totalMemory;
                        b->usedMemory = data.usedMemory;
                        b->availableMemory = data.availableMemory;
                        b->compressedMemory = data.compressedMemory;
                        // Battery / power
                        b->batteryPercent = data.batteryPercent;
                        b->acOnline = data.acOnline;
                        // OS
                        std::strncpy(b->osVersion, data.osVersion.c_str(), 127);
                        b->osVersion[127] = '\0';
                        // GPU
                        std::strncpy(b->gpuName, data.gpuName.c_str(), 47);
                        b->gpuName[47] = '\0';
                        b->gpuMemory = data.gpuMemory;
                        b->gpuMemoryPercent = static_cast<float>(data.gpuMemoryPercent);
                        b->gpuUsage = static_cast<float>(data.gpuUsage);
                        b->gpuTemp = static_cast<float>(data.gpuTemp);
                        b->gpuIsVirtual = false;
                        // Disks (up to 4)
                        b->diskCount = 0;
                        for (size_t di = 0; di < std::min(data.disks.size(), size_t(4)); ++di) {
                            auto& d = b->disks[di];
                            std::strncpy(d.label, data.disks[di].label.c_str(), 31);
                            d.label[31] = '\0';
                            d.totalSize = data.disks[di].totalSize;
                            d.usedSpace = data.disks[di].usedSpace;
                            d.freeSpace = data.disks[di].totalSize - data.disks[di].usedSpace;
                            std::strncpy(d.fs, data.disks[di].fileSystem.c_str(), 15);
                            d.fs[15] = '\0';
                            b->diskCount++;
                        }
                        // Network adapters (up to 4, only connected)
                        b->adapterCount = 0;
                        for (size_t ai = 0; ai < data.adapters.size() && b->adapterCount < 4; ++ai) {
                            if (data.adapters[ai].ip.empty()) continue;
                            auto& n = b->adapters[b->adapterCount];
                            std::strncpy(n.name, data.adapters[ai].name.c_str(), 31);
                            std::strncpy(n.ip,   data.adapters[ai].ip.c_str(), 15);
                            std::strncpy(n.mac,  data.adapters[ai].mac.c_str(), 17);
                            std::strncpy(n.type, data.adapters[ai].type.c_str(), 15);
                            n.speed = data.adapters[ai].speed;
                            n.downloadSpeed = data.adapters[ai].downloadSpeed;
                            n.uploadSpeed = data.adapters[ai].uploadSpeed;
                            b->adapterCount++;
                        }
                        // Temperatures (up to 10)
                        b->tempCount = 0;
                        for (size_t ti = 0; ti < std::min(data.temperatures.size(), size_t(10)); ++ti) {
                            std::strncpy(b->temperatures[ti].name, data.temperatures[ti].first.c_str(), 63);
                            b->temperatures[ti].name[63] = '\0';
                            b->temperatures[ti].value = static_cast<float>(data.temperatures[ti].second);
                            b->tempCount++;
                        }
                        // Physical disks (SMART) — cached once at startup
                        static std::vector<PhysicalDiskSmartData> cachedSmart;
                        static bool smartDone = false;
                        if (!smartDone) {
                            try {
                                SystemInfo tmp;
                                DiskInfo().CollectSmartData(tmp);
                                cachedSmart = std::move(tmp.physicalDisks);
                                smartDone = true;
                                Logger::Info("SMART collected " + std::to_string(cachedSmart.size()) + " physical disks");
                            } catch (...) {
                                Logger::Error("SMART collection threw exception (need root?)");
                            }
                        }
                        b->physDiskCount = 0;
                        for (size_t pi = 0; pi < std::min(cachedSmart.size(), size_t(8)); ++pi) {
                            auto& pd = b->physicalDisks[pi];
                            const auto& src = cachedSmart[pi];
                            for (size_t k = 0; k < 63 && src.model[k] != u'\0'; ++k)
                                pd.model[k] = static_cast<char>(src.model[k]);
                            pd.model[63] = '\0';
                            for (size_t k = 0; k < 63 && src.serialNumber[k] != u'\0'; ++k)
                                pd.serial[k] = static_cast<char>(src.serialNumber[k]);
                            pd.serial[63] = '\0';
                            pd.capacity = src.capacity;
                            for (size_t k = 0; k < 15 && src.interfaceType[k] != u'\0'; ++k)
                                pd.interfaceType[k] = static_cast<char>(src.interfaceType[k]);
                            pd.interfaceType[15] = '\0';
                            pd.temperature = static_cast<float>(src.temperature);
                            pd.healthPercent = static_cast<float>(src.healthPercentage);
                            pd.smartSupported = src.smartSupported;
                            b->physDiskCount++;
                        }
                        // TUI physical disk snapshot (from same cached data)
                        data.physicalDisks.clear();
                        data.physicalDisks.reserve(cachedSmart.size());
                        for (const auto& src : cachedSmart) {
                            tcmt::TuiData::PhysicalDiskInfo pi;
                            // Convert WCHAR model to narrow (strip trailing nulls, take up to 63 chars)
                            for (int k = 0; k < 63 && src.model[k] != u'\0'; ++k)
                                pi.model += static_cast<char>(src.model[k]);
                            for (int k = 0; k < 63 && src.serialNumber[k] != u'\0'; ++k)
                                pi.serial += static_cast<char>(src.serialNumber[k]);
                            pi.capacity = src.capacity;
                            pi.temperature = src.temperature;
                            pi.healthPct = src.healthPercentage;
                            pi.smartSupported = src.smartSupported;
                            pi.powerOnHours = src.powerOnHours;
                            pi.wearLeveling = src.wearLeveling;
                            // diskType
                            for (int k = 0; k < 15 && src.diskType[k] != u'\0'; ++k)
                                pi.diskType += static_cast<char>(src.diskType[k]);
                            // interfaceType
                            for (int k = 0; k < 15 && src.interfaceType[k] != u'\0'; ++k)
                                pi.interfaceType += static_cast<char>(src.interfaceType[k]);
                            // attributes
                            pi.attributes.clear();
                            pi.attributes.reserve(src.attributeCount);
                            for (int ai = 0; ai < src.attributeCount; ++ai) {
                                tcmt::TuiData::SmAttributeInfo ai2;
                                ai2.id = src.attributes[ai].id;
                                ai2.current = src.attributes[ai].current;
                                ai2.worst = src.attributes[ai].worst;
                                ai2.rawValue = src.attributes[ai].rawValue;
                                for (int k = 0; k < 63 && src.attributes[ai].name[k] != u'\0'; ++k)
                                    ai2.name += static_cast<char>(src.attributes[ai].name[k]);
                                pi.attributes.push_back(std::move(ai2));
                            }
                            data.physicalDisks.push_back(std::move(pi));
                        }
                    }
                    // WiFi
                    const auto& wd2 = s_wifi.GetData();
                    std::strncpy(b->wifi.ssid, wd2.ssid.c_str(), 31);
                    b->wifi.ssid[31] = '\0';
                    b->wifi.rssi = wd2.rssi;
                    b->wifi.channel = wd2.channel;
                    std::strncpy(b->wifi.security, wd2.security.c_str(), 15);
                    b->wifi.security[15] = '\0';
                    b->wifi.powerOn = wd2.powerOn;
                    b->wifi.isConnected = wd2.isConnected;
                    // Bluetooth
                    const auto& bd2 = s_bt.GetData();
                    b->bluetooth.powerOn = bd2.adapter.powerOn;
                    b->bluetooth.deviceCount = static_cast<int32_t>(bd2.devices.size());
                    std::strncpy(b->bluetooth.name, bd2.adapter.name.c_str(), 63);
                    b->bluetooth.name[63] = '\0';

                    // seqlock: mark write complete (even)
                    std::atomic_thread_fence(std::memory_order_release);
                    b->writeSequence++;
                }

            // Sleep
            auto loopEnd = std::chrono::high_resolution_clock::now();
            int loopMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                loopEnd - loopStart).count();
            int sleepMs = std::max(500 - loopMs, 50);  // 2 Hz update
            // Sleep with responsive exit (check every 50ms)
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

            // USB detection (every 10 seconds)
            static int usbCheckCounter = 0;
            if (++usbCheckCounter >= 20) {
                usbCheckCounter = 0;
                if (s_usbNotify.Poll()) {
                try {
                    UsbInfo usb;
                    usb.Detect();
                    const auto& devs = usb.GetDevices();
                    if (!devs.empty()) {
                        Logger::Info("USB: " + std::to_string(devs.size()) + " device(s)");
                        for (size_t di = 0; di < std::min(devs.size(), size_t(8)); ++di)
                            Logger::Debug("  " + devs[di].name + " VID:" + std::to_string(devs[di].vid)
                                        + " PID:" + std::to_string(devs[di].pid));
                    }
                } catch (...) {}
                } // s_usbNotify.Poll()
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
    ipcServer.Stop();
    Logger::Info("IPC server stopped");
    Logger::Info("Done.");
    return 0;
}
