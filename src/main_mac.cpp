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
#include <mutex>
#include <atomic>
#include <cstring>
#include <csignal>
#include <unistd.h>
#include <sys/types.h>
#include <cstdlib>
#include <mach/mach_time.h>
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <libproc.h>
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
#include "core/display/DisplayInfo.h"
#include "core/battery/BatteryHealth.h"
#include "core/fan/FanSpeed.h"
#include "core/notifications/DeviceChangeNotifier.h"
#include "core/notifications/UserNotifier.h"
#include "core/MCP/MCPServer.h"
#include "core/IPC/IPCClient.h"
#include "core/DataStruct/DataStruct.h"
#include "core/IPC/IPCServer.h"
#include "core/temperature/TemperatureWrapper.h"
#include "core/process/ProcessTop.h"
#include "core/als/ALSensor.h"
#include "core/accel/SpsManager.h"
#include "core/coordinator/ModuleCoordinator.h"
#include "core/Utils/Logger.h"
#include "core/Utils/UserIdentity.h"
#include "tui/TuiApp.h"
#include "tui/MacLogWindow.h"

// Config management (JSON config via nlohmann/json)
#include "core/Config/ConfigManager.h"
#include "core/Updater.h"
#include "core/HTTPServer/MotionHTTPServer.h"
#include "core/ServerProbe.h"
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
    addF("cpu/pcore/temperature", offsetof(B, cpuPcoreTemp));
    addF("cpu/ecore/temperature", offsetof(B, cpuEcoreTemp));
    addB("cpu/hyperThreading",    offsetof(B, hyperThreading));
    addB("cpu/virtualization",    offsetof(B, virtualization));
    addF("cpu/sampleIntervalMs",  offsetof(B, cpuSampleIntervalMs));
    addF("cpu/freq/base",         offsetof(B, cpuBaseFreq));

    // Memory
    addU64("memory/total",        offsetof(B, totalMemory));
    addU64("memory/used",         offsetof(B, usedMemory));
    addU64("memory/available",    offsetof(B, availableMemory));
    addU64("memory/compressed",   offsetof(B, compressedMemory));
    addU64("memory/swapUsed",     offsetof(B, swapUsed));
    addU64("memory/swapTotal",    offsetof(B, swapTotal));
    add("memory/ramSpeed", offsetof(B, ramSpeed), 4, FT::UInt32);
    add("memory/ramType",  offsetof(B, ramType), (uint16_t)sizeof(B::ramType), FT::String);

    // Battery / Power
    addI("battery/percent",       offsetof(B, batteryPercent));
    addB("battery/acOnline",      offsetof(B, acOnline));
    addF("power/cpu",             offsetof(B, cpuPower));
    addF("power/gpu",             offsetof(B, gpuPower));
    addF("power/ane",             offsetof(B, anePower));

    // OS
    addS("os/version",            offsetof(B, osVersion), 128);
    addS("os/model",              offsetof(B, hardwareModel), 128);
    addS("app/version",           offsetof(B, appVersion), 32);

    // GPU
    addS("gpu/0/name",            offsetof(B, gpuName), 48);
    addS("gpu/0/brand",           offsetof(B, gpuBrand), 32);
    addU64("gpu/0/memory",        offsetof(B, gpuMemory));
    addF("gpu/0/memoryPercent",   offsetof(B, gpuMemoryPercent));
    addF("gpu/0/usage",           offsetof(B, gpuUsage));
    addF("gpu/0/temperature",     offsetof(B, gpuTemp));
    addF("gpu/freq",              offsetof(B, gpuFreq));
    addB("gpu/0/isVirtual",       offsetof(B, gpuIsVirtual));
    addB("gpu/0/unified",         offsetof(B, gpuUnified));
    // 整份 GPU 列表：gpu/0/* 只有主卡，多卡机器上其余的卡走这里
    addS("gpu/json",              offsetof(B, gpuJson), sizeof(B::gpuJson));

    // Disks (up to 4)
    for (int i = 0; i < 4; ++i) {
        char p[32]; snprintf(p, sizeof(p), "disk/%d/", i);
        uint32_t base = offsetof(B, disks) + i * sizeof(B::DiskSlot);
        addS((std::string(p)+"label").c_str(), base + offsetof(B::DiskSlot, label), 32);
        // 挂载点。字段名上限 31 字符（IPC_FIELD_NAME_LEN=32），"disk/3/mountPoint" 是 18，够。
        addS((std::string(p)+"mountPoint").c_str(), base + offsetof(B::DiskSlot, mountPoint), 64);
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
        addS((std::string(p)+"macCurrent").c_str(), base + offsetof(B::NetSlot, macCurrent), 18);
        addS((std::string(p)+"type").c_str(),   base + offsetof(B::NetSlot, type), 24);
        addU64((std::string(p)+"speed").c_str(),  base + offsetof(B::NetSlot, speed));
        addU64((std::string(p)+"downloadSpeed").c_str(), base + offsetof(B::NetSlot, downloadSpeed));
        addU64((std::string(p)+"uploadSpeed").c_str(),   base + offsetof(B::NetSlot, uploadSpeed));
    }

    // 温度传感器：一个字段装完整份，形状见 IPCData.h 的 sensorsJson
    addS("sensors/json", offsetof(B, sensorsJson), 4096);

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
        addI((std::string(p)+"attrCount").c_str(),     base + offsetof(B::PhysDiskSlot, attrCount));
        addS((std::string(p)+"attrsJson").c_str(),     base + offsetof(B::PhysDiskSlot, attrsJson), 4096);
    }

    // WiFi
    addS("wifi/ssid",        offsetof(B, wifi.ssid), 32);
    addI("wifi/rssi",        offsetof(B, wifi.rssi));
    addI("wifi/channel",     offsetof(B, wifi.channel));
    addS("wifi/security",    offsetof(B, wifi.security), 16);
    // band / gen 采集端一直在收（WiFiInfo.mm），Windows 侧也一直有注册，
    // 只有 macOS 这边漏了两行 —— 结果是 C# 用 ?? "" 读到空串，界面上频段和代数永远空着。
    addS("wifi/band",        offsetof(B, wifi.band), 8);
    addS("wifi/gen",         offsetof(B, wifi.wifiGen), 12);
    addB("wifi/powerOn",     offsetof(B, wifi.powerOn));
    addB("wifi/isConnected", offsetof(B, wifi.isConnected));
    // Bluetooth
    addB("bluetooth/powerOn",     offsetof(B, bluetooth.powerOn));
    addI("bluetooth/deviceCount", offsetof(B, bluetooth.deviceCount));
    addS("bluetooth/name",        offsetof(B, bluetooth.name), 64);

    // ─── 4. Fan speeds (up to 6) ───
    for (int i = 0; i < 6; ++i) {
        char p[32]; snprintf(p, sizeof(p), "fan/%d/", i);
        uint32_t base = offsetof(B, fanSpeeds) + i * sizeof(B::FanSlot);
        addS((std::string(p)+"name").c_str(), base + offsetof(B::FanSlot, name), 32);
        addF((std::string(p)+"rpm").c_str(),  base + offsetof(B::FanSlot, rpm));
    }
    addU8("fan/count", offsetof(B, fanCount));

    // ─── 5. 进程列表：一个字段装完整份，形状见 IPCData.h 的 processesJson ───
    addS("proc/json", offsetof(B, processesJson), 16384);

    // ─── 7. Battery detail ───
    addI("battery/cycleCount",       offsetof(B, batteryCycleCount));
    addI("battery/designCapacity",   offsetof(B, batteryDesignCapacity));
    addI("battery/maxCapacity",      offsetof(B, batteryMaxCapacity));
    addF("battery/healthPercent",    offsetof(B, batteryHealthPercent));
    addF("battery/temperature",      offsetof(B, batteryTemp));
    addI("battery/amperage",         offsetof(B, batteryAmperage));
    addI("battery/voltage",          offsetof(B, batteryVoltage));
    addF("battery/chargerWatts",     offsetof(B, batteryChargerWatts));
    addB("battery/isCharging",       offsetof(B, batteryIsCharging));
    addB("battery/isPresent",        offsetof(B, batteryIsPresent));

    // ─── 6. 逐核传感器：一个字段装完整份，形状见 IPCData.h 的 cpuCoresJson ───
    addS("cpu/cores/json", offsetof(B, cpuCoresJson), sizeof(B::cpuCoresJson));

    // ─── System info (load avg, process count, uptime) ───
    addF("system/loadAvg1",    offsetof(B, loadAvg1));
    addF("system/loadAvg5",    offsetof(B, loadAvg5));
    addF("system/loadAvg15",   offsetof(B, loadAvg15));
    addI("system/processCount", offsetof(B, processCount));
    addU64("system/uptime",     offsetof(B, uptimeSeconds));
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

// Sanitize strings embedded in the hand-built snapshot JSON (strip quotes).
static std::string JsonSafe(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c == '"' || c == '\\') c = '_';
    }
    return out;
}

// Loop stall diagnostics: the stage marker is set before each major block of
// the monitor loop so a hung sensor call can be located after the fact
// (slow-iteration warning + `sample` while frozen).
static std::atomic<int> g_loopStage{0};
static Updater s_updater;

// Hardware sampling cadence in ms, editable on the TUI settings page (press S)
// and persisted to system_monitor.json (sampling.intervalMs). The monitor loop
// reads it every iteration; a change applies from the next frame on.
static std::atomic<int> g_sampleIntervalMs{tcmt::kSampleIntervalDefaultMs};

// SMART attributes → JSON for IPC consumers. Built once per SMART refresh
// (60s) instead of on every monitor-loop frame — the per-attribute UTF-8
// conversion + stringstream formatting cost ~150ms per frame otherwise.
static std::string SmartAttrsToJson(const PhysicalDiskSmartData& src) {
    if (src.attributeCount <= 0) return std::string();
    auto toUtf8 = [](const WCHAR* s, int maxLen) -> std::string {
        std::string out;
        for (int i = 0; i < maxLen && s[i] != u'\0'; ++i) {
            uint16_t cp = static_cast<uint16_t>(s[i]);
            if (cp == '"' || cp == '\\') { out += '\\'; out += (char)cp; }
            else if (cp < 0x80) out += (char)cp;
            else if (cp < 0x800) {
                out += (char)(0xC0 | (cp >> 6));
                out += (char)(0x80 | (cp & 0x3F));
            } else {
                out += (char)(0xE0 | (cp >> 12));
                out += (char)(0x80 | ((cp >> 6) & 0x3F));
                out += (char)(0x80 | (cp & 0x3F));
            }
        }
        return out;
    };
    std::ostringstream js;
    js << "[";
    for (int ai = 0; ai < src.attributeCount; ++ai) {
        if (ai > 0) js << ",";
        const auto& a = src.attributes[ai];
        js << "{\"id\":" << (int)a.id
           << ",\"cur\":" << (int)a.current
           << ",\"worst\":" << (int)a.worst
           << ",\"raw\":" << a.rawValue
           << ",\"name\":\"" << toUtf8(a.name, 63) << "\""
           << ",\"desc\":\"" << toUtf8(a.description, 127) << "\""
           << "}";
    }
    js << "]";
    return js.str();
}

// ======================== Server push settings ========================
// No CLI flags: the server connection (enable / URL / TLS verify) is edited
// on the TUI settings page (press S) and persisted to system_monitor.json
// (server.*). The TUI handler stores a pending copy here; the monitor loop
// applies it (restart the probe) on its own thread.
static ConfigManager g_cfg("system_monitor.json");
static std::mutex s_settingsMutex;
static bool s_settingsPending = false;
static tcmt::ServerSettings s_pendingSettings;

// ── IPC 配置通道：允许客户端改的键 ────────────────────────────────────────
//
// 白名单是**必须**的，不是保守。g_cfg 里除了用户配置，还存着采集端自己每个周期
// 写进去的运行时快照（os.version / cpu.name / memory.total …）和一堆内部键。
// 没有白名单，客户端一个请求就能把它们写坏 —— 而且写进去的还是合法 JSON，
// 要等下次重启读配置才发现不对。
//
// 每个键带类型，是因为 JSON 里的 true / 1 / "1" 必须落到 ConfigManager 不同的
// Set* 上。类型猜错的后果比不生效更糟：值写进去了、界面也显示改了，但它永远不生效。
namespace {

enum class CfgType { Bool, Int, Str };

struct CfgKey {
    const char* key;
    CfgType     type;
};

constexpr CfgKey kClientConfigKeys[] = {
    { "server.enabled",      CfgType::Bool },
    { "server.url",          CfgType::Str  },
    { "server.insecure",     CfgType::Bool },
    { "server.intervalSec",  CfgType::Int  },
    { "sampling.intervalMs", CfgType::Int  },
    { "ui.language",         CfgType::Str  },
};

const CfgKey* FindCfgKey(const std::string& key) {
    for (const auto& k : kClientConfigKeys) {
        if (key == k.key) return &k;
    }
    return nullptr;
}

/// 当前配置值，交给 IPCServer 在客户端握手完成后推过去。
std::string BuildConfigSnapshotJson() {
    nlohmann::json j;
    j["server.enabled"]      = g_cfg.GetBool("server.enabled", false);
    j["server.url"]          = g_cfg.GetString("server.url", "");
    j["server.insecure"]     = g_cfg.GetBool("server.insecure", false);
    j["server.intervalSec"]  = g_cfg.GetInt("server.intervalSec", 2);
    j["sampling.intervalMs"] = g_sampleIntervalMs.load();
    j["ui.language"]         = g_cfg.GetString("ui.language", "zh-Hans");
    return j.dump();
}

} // namespace

// ======================== Main ========================
int main(int argc, char* argv[]) {
    // Location Services is granted per login user: a root process can never
    // request it, so sudo turns the WiFi panel into a permanent "unavailable"
    // and hides nothing else that the user gains. Refuse to start instead of
    // running in a degraded state that looks like a bug.
    if (geteuid() == 0) {
        std::cerr << "TCMT-M: unavailable under root. Run without sudo as your login user.\n"
                  << "TCMT-M: root 下不可用，请去掉 sudo 以登录用户运行。\n";
        return 1;
    }

    // Setup signal handlers
    std::signal(SIGINT, SignalHandler);
    std::signal(SIGTERM, SignalHandler);
    std::signal(SIGHUP, SignalHandler);

    try {
        Logger::Initialize("system_monitor.log");
        Logger::EnableConsoleOutput(false);  // TUI takes over console
        // Start at INFO, not WARN: the in-app log page is a diagnostic surface,
        // and a WARN-only gate meant INFO never reached g_tuiLogBuffer at all —
        // the page looked permanently empty. config logging.level (below) and
        // --debug/--verbose refine the level from here.
        Logger::SetLogLevel(LOG_INFO);
        Logger::Info("TCMT macOS Client starting (TUI mode)...");
    } catch (const std::exception& e) {
        std::cerr << "Logger init failed: " << e.what() << std::endl;
        return 1;
    }

    // Keep sampling timers responsive while the window is occluded or the
    // app is backgrounded (App Nap would otherwise coalesce nanosleep and
    // the monitor loop appears to stall).
    tcmt::mac::MacLogWindow::DisableAppNap();

    // Load application config (config.json)
    {
        if (g_cfg.Load()) {
            Logger::Info("Config loaded: " + g_cfg.GetPath());
            // Apply config-driven settings
            std::string logLevel = g_cfg.GetString("logging.level", "info");
            if (logLevel == "debug")
                Logger::SetLogLevel(LOG_DEBUG);
            else if (logLevel == "warning")
                Logger::SetLogLevel(LOG_WARNING);
            else if (logLevel == "error")
                Logger::SetLogLevel(LOG_ERROR);
            else
                Logger::SetLogLevel(LOG_INFO);   // "info", or an unrecognised value

            // Hardware sampling cadence, editable from the TUI settings page
            // (press S). Bounds mirror the form and ConfigManager's rule.
            int sampleMs = g_cfg.GetInt("sampling.intervalMs",
                                        tcmt::kSampleIntervalDefaultMs);
            if (sampleMs < tcmt::kSampleIntervalMinMs)
                sampleMs = tcmt::kSampleIntervalMinMs;
            if (sampleMs > tcmt::kSampleIntervalMaxMs)
                sampleMs = tcmt::kSampleIntervalMaxMs;
            g_sampleIntervalMs.store(sampleMs);
        } else {
            Logger::Warn("No config file found, using defaults");
        }
    }

    // ======================== Flags / server settings ========================
    // Server push has no CLI flags — see the settings block above. The URL
    // defaults to TCMT_SERVER env (if set), otherwise localhost:8080.
    bool jsonMode = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--json") jsonMode = true;
        else if (std::string(argv[i]) == "--debug") Logger::SetLogLevel(LOG_DEBUG);
        else if (std::string(argv[i]) == "--verbose") Logger::SetLogLevel(LOG_INFO);
    }

    // Reported after the CLI pass has settled the level, so it is visible
    // with --verbose (the config path runs before it).
    Logger::Info("Sampling interval: " + std::to_string(g_sampleIntervalMs.load()) +
                 " ms (change it on the TUI settings page, press S)");

    std::string serverUrl = g_cfg.GetString("server.url", "");
    if (serverUrl.empty()) {
        const char* env = std::getenv("TCMT_SERVER");
        serverUrl = (env && env[0] != '\0') ? env : "http://127.0.0.1:8080";
    }
    const bool serverEnabled = g_cfg.GetBool("server.enabled", false);
    const bool serverInsecure = g_cfg.GetBool("server.insecure", false);
    const bool motionHttpEnabled = g_cfg.GetBool("motion_http.enabled", false);
    // Upload cadence — hard bounds 1..60 s (config + TUI settings page).
    int intervalSec = g_cfg.GetInt("server.intervalSec", 2);
    if (intervalSec < 1) intervalSec = 1;
    if (intervalSec > 60) intervalSec = 60;
    int64_t intervalMs = (int64_t)intervalSec * 1000;  // runtime (TUI can change)
    bool probeActive = serverEnabled;   // runtime state (TUI can toggle)
    std::string probeStatus = "disabled";  // shown on the TUI settings page
    int64_t lastPushMs = 0;                // last successful snapshot post

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
            // 可选能力的探测：走直读是正常路径，不是降级，按约定压到 DEBUG
            Logger::Debug("MCP: IPC unavailable, using direct hardware reads");
        }

        server.RegisterTool("get_cpu_status", "CPU usage, cores, frequency, temperature",
        [&ipc, useIpc]() -> nlohmann::json {
            nlohmann::json j;
            if (useIpc) {
                j["name"]    = ipc.ReadString("cpu/name").value_or("");
                j["usage"]   = ipc.ReadFloat("cpu/usage").value_or(0.0);
                j["cores"]["physical"]    = ipc.ReadInt32("cpu/cores/physical").value_or(0);
                j["cores"]["performance"] = ipc.ReadInt32("cpu/cores/performance").value_or(0);
                j["cores"]["efficiency"]  = ipc.ReadInt32("cpu/cores/efficiency").value_or(0);
                j["frequencies"]["pCore"] = ipc.ReadFloat("cpu/freq/pCore").value_or(0.0);
                j["frequencies"]["eCore"] = ipc.ReadFloat("cpu/freq/eCore").value_or(0.0);
                j["temperature"] = ipc.ReadFloat("cpu/temperature").value_or(0.0);
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
                j["usage"] = ipc.ReadFloat("gpu/0/usage").value_or(0.0);
                j["memory"] = ipc.ReadUInt64("gpu/0/memory").value_or(0);
                j["temperature"] = ipc.ReadFloat("gpu/0/temperature").value_or(0.0);
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
                    // 当前生效的链路地址。macOS 上采集端给不出（系统按签名状态限制），这里是空串 ——
                    // 不拿硬件地址冒充，消费方据"空"判断未知即可。
                    na["macCurrent"] = ipc.ReadString("net/" + std::to_string(i) + "/macCurrent").value_or("");
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
                    na["macCurrent"] = a.currentMac;
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
                // sensors/json 是整份打包好的数组，形状与下面本地分支一致。
                // 解析失败就当没有，保持和以前"读不到就不放 sensors 键"一样的行为 ——
                // 这是给 AI 消费的工具，宁可少给也不能给个形状不对的东西。
                const std::string raw = ipc.ReadString("sensors/json").value_or("");
                if (!raw.empty()) {
                    auto parsed = nlohmann::json::parse(raw, nullptr, false);
                    if (!parsed.is_discarded() && parsed.is_array()) {
                        j["sensors"] = std::move(parsed);
                    }
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

    // 客户端（Avalonia 设置页）读写配置的入口。服务端不认识键名，只负责搬 JSON；
    // 认键、校验、落盘、让探针重启，都在这里 —— 只有 main 有 g_cfg 和那套 pending 机制。
    ipcServer.SetConfigBridge(
        [] { return BuildConfigSnapshotJson(); },
        [&ipcServer](const std::string& payload) {
            nlohmann::json in;
            try {
                in = nlohmann::json::parse(payload);
            } catch (const std::exception& e) {
                Logger::Warn(std::string("IPC: ConfigSet 不是合法 JSON，丢弃: ") + e.what());
                return;
            }
            if (!in.is_object()) {
                Logger::Warn("IPC: ConfigSet 不是 JSON 对象，丢弃");
                return;
            }

            int applied = 0;
            for (auto it = in.begin(); it != in.end(); ++it) {
                const CfgKey* k = FindCfgKey(it.key());
                if (!k) {
                    // 不认识的键说出来，不静默忽略：它意味着客户端和服务端的版本对不上，
                    // 而静默会让对面以为改成功了。
                    Logger::Warn("IPC: ConfigSet 含白名单外的键，忽略: " + it.key());
                    continue;
                }
                try {
                    switch (k->type) {
                        case CfgType::Bool:
                            if (!it.value().is_boolean()) throw std::runtime_error("要布尔值");
                            g_cfg.SetBool(k->key, it.value().get<bool>());
                            break;
                        case CfgType::Int:
                            if (!it.value().is_number_integer()) throw std::runtime_error("要整数");
                            g_cfg.SetInt(k->key, it.value().get<int>());
                            break;
                        case CfgType::Str:
                            if (!it.value().is_string()) throw std::runtime_error("要字符串");
                            g_cfg.SetString(k->key, it.value().get<std::string>());
                            break;
                    }
                    applied++;
                } catch (const std::exception& e) {
                    Logger::Warn(std::string("IPC: ConfigSet 的 ") + k->key + " 类型不对（" +
                                 e.what() + "），忽略");
                }
            }

            if (applied == 0) return;

            g_cfg.Save();

            // 采样间隔除了落盘还要喂那个原子量 —— 主循环读的是它，不是文件。
            const int ms = g_cfg.GetInt("sampling.intervalMs", g_sampleIntervalMs.load());
            g_sampleIntervalMs.store(
                std::max(tcmt::kSampleIntervalMinMs, std::min(tcmt::kSampleIntervalMaxMs, ms)));

            // 上传那几项要让探针按新值重启，和 TUI 按 S 保存走的是同一条路。
            tcmt::ServerSettings s;
            s.enabled     = g_cfg.GetBool("server.enabled", false);
            s.url         = g_cfg.GetString("server.url", "");
            s.insecure    = g_cfg.GetBool("server.insecure", false);
            s.intervalSec = g_cfg.GetInt("server.intervalSec", 2);
            {
                std::lock_guard<std::mutex> lock(s_settingsMutex);
                s_pendingSettings = s;
                s_settingsPending = true;
            }

            Logger::Info("IPC: 客户端改了 " + std::to_string(applied) + " 项配置，已保存并生效");

            // 回推一份：改的那个客户端要靠它把界面和落盘的值对齐
            ipcServer.BroadcastConfigSnapshot();
        });

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
    // Location Services request is user-triggered (press R in the TUI):
    // the judgment/guidance comes first, only then does the app ask.
    // Under sudo there is no granted identity to prompt for, so the key is
    // not advertised at all (the WiFi panel explains the root case).
    if (geteuid() != 0) {
        tuiApp.SetLocationRequestHandler([] {
            WiFiInfo::RequestLocationAuthorization();
        });
    }
    // Server push settings (TUI settings page, press S) -> persist + apply.
    tuiApp.SetServerSettings({serverEnabled, serverUrl, serverInsecure, intervalSec,
                              g_sampleIntervalMs.load(), probeStatus});
    tuiApp.SetSampleIntervalHandler([](int ms) {
        if (ms < 200) ms = 200;
        if (ms > 5000) ms = 5000;
        g_sampleIntervalMs.store(ms);
        g_cfg.SetInt("sampling.intervalMs", ms);
        g_cfg.Save();
        Logger::Info("Sampling interval set to " + std::to_string(ms) + " ms");
    });
    // Self-update: startup check + U-key download.
    s_updater.SetExePath(argv[0]);
    s_updater.CheckForUpdate();
    tuiApp.SetUpdateRequestHandler([] { s_updater.StartDownload(); });

    // Language chosen on the settings page survives a restart: the TUI hands the
    // new value back here, and startup feeds the saved one in (TCMT_LANG still wins).
    tuiApp.SetConfiguredLanguage(g_cfg.GetString("ui.language", ""));
    tuiApp.SetLanguageHandler([](tcmt::Lang lang) {
        g_cfg.SetString("ui.language", lang == tcmt::Lang::ZhHans ? "zh-Hans" : "en");
        g_cfg.Save();
    });

    tuiApp.SetServerSettingsHandler([](const tcmt::ServerSettings& s) {
        g_cfg.SetBool("server.enabled", s.enabled);
        g_cfg.SetString("server.url", s.url);
        g_cfg.SetBool("server.insecure", s.insecure);
        g_cfg.SetInt("server.intervalSec", s.intervalSec);
        g_cfg.Save();
        std::lock_guard<std::mutex> lock(s_settingsMutex);
        s_pendingSettings = s;
        s_settingsPending = true;
        Logger::Info("Server settings saved: enabled=" +
                     std::string(s.enabled ? "true" : "false") + " url=" + s.url);
    });
    tuiApp.Start();

    // In-process native log window (AppKit). The default is single-terminal:
    // logs are viewed in the in-TUI log page (press L) — no native window.
    // Set TCMT_LOG_WINDOW=1 to opt into the separate AppKit window instead.
    tcmt::mac::MacLogWindow logWindow;
    const bool wantLogWindow = (std::getenv("TCMT_LOG_WINDOW") != nullptr);
    if (wantLogWindow) {
        if (logWindow.Create(&Logger::GetTuiBuffer())) {
            Logger::Info("Log window opened (in-process AppKit)");
        } else {
            Logger::Warn("TCMT_LOG_WINDOW=1 but window creation failed — using in-TUI log page (L)");
        }
    } else {
        Logger::Info("Single-terminal log: in-TUI log page (press L)");
    }

    // Temperature sensors (SMC/PawnIO) — sampled on heavy frames for the
    // tcmt-server snapshot and available to the MCP tools.
    TemperatureWrapper::Initialize();

    // SPU Sensor Manager — reads ALL AppleSPUHIDDevice sensors via
    // asynchronous IOHIDDevice input report callbacks (no root needed).
    // Includes: gravity/orientation vector, gyroscope, ALS, lid angle.
    // Global motion cache for HTTP server access (atomic swap)
    static std::mutex s_motionMutex;
    static double s_ax = 0, s_ay = 0, s_az = -1;
    static double s_gx = 0, s_gy = 0, s_gz = 0;
    static double s_lidAngle = 0, s_imut = 0;
    static int s_heartbeat = 0, s_heartFlag = 0;
    static bool s_hasAccel = false, s_hasGyro = false;

    static SpsManager s_sps;
    if (!s_sps.Start()) {
        Logger::Warn("SpsManager: no SPU HID sensors found — sensor features disabled");
    } else {
        Logger::Info("SpsManager started with " + std::to_string(s_sps.Sensors().size()) + " sensors");
        for (auto& s : s_sps.Sensors())
            Logger::Info("  " + s->name);
    }

    // Motion HTTP server (enable via motion_http.enabled in config)
    static MotionHTTPServer s_http;
    if (motionHttpEnabled) {
        s_http.Start(9876, [](const std::string& method, const std::string& path) -> std::string {
        if (path == "/sensors/motion" && method == "GET") {
            std::lock_guard<std::mutex> lk(s_motionMutex);
            char buf[512];
            snprintf(buf, sizeof(buf),
                "{\"ax\":%.6f,\"ay\":%.6f,\"az\":%.6f,\"gx\":%.3f,\"gy\":%.3f,\"gz\":%.3f,\"lidAngle\":%.1f,\"hb\":%d,\"imut\":%.1f}",
                s_ax, s_ay, s_az, s_gx, s_gy, s_gz, s_lidAngle, s_heartbeat, s_imut);
            return std::string(buf);
        }
        if (path == "/system/ping" && method == "GET") {
            char buf[128];
            snprintf(buf, sizeof(buf), "{\"status\":\"ok\",\"conns\":%d}", s_http.OpenClients());
            return std::string(buf);
        }
        return "{}";
    });
    } // if (motionHttpEnabled)

    // ServerProbe: push data to tcmt-server (enable/URL from TUI settings)
    static ServerProbe s_probe;
    if (serverEnabled) {
        s_probe.SetInsecure(serverInsecure);
        Logger::Info("ServerProbe: target " + serverUrl);
        if (s_probe.Start(serverUrl)) {
            Logger::Info("ServerProbe: upload started");
            probeStatus = "running";
            probeActive = true;
        } else {
            Logger::Warn("ServerProbe: cannot start (invalid URL or no TLS support)");
            probeStatus = "error: cannot start";
            probeActive = false;
        }
    }

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

    // Run the monitoring loop on a background thread so the main thread can
    // run the AppKit event loop (fully interactive log window). The loop
    // exits when the TUI quits or a signal arrives, then stops the run loop.
    std::thread monitorThread([&] {

    while (!g_shouldExit.load() && tuiApp.IsRunning()) {
        try {
            auto loopStart = std::chrono::high_resolution_clock::now();

            // Apply server settings saved from the TUI settings page.
            {
                tcmt::ServerSettings pending;
                bool havePending = false;
                {
                    std::lock_guard<std::mutex> lock(s_settingsMutex);
                    if (s_settingsPending) {
                        pending = s_pendingSettings;
                        s_settingsPending = false;
                        havePending = true;
                    }
                }
                if (havePending) {
                    serverUrl = pending.url;
                    intervalSec = (std::max)(1, (std::min)(pending.intervalSec, 60));
                    intervalMs = (int64_t)intervalSec * 1000;
                    s_probe.Stop();
                    if (pending.enabled) {
                        s_probe.SetInsecure(pending.insecure);
                        if (s_probe.Start(pending.url)) {
                            probeStatus = "running";
                            probeActive = true;
                            Logger::Info("ServerProbe restarted: " + pending.url);
                        } else {
                            probeStatus = "error: cannot start";
                            probeActive = false;
                            Logger::Warn("ServerProbe restart failed: " + pending.url);
                        }
                    } else {
                        probeStatus = "disabled";
                        probeActive = false;
                        Logger::Info("ServerProbe stopped (disabled in settings)");
                    }
                    tuiApp.SetServerSettings(
                        {pending.enabled, pending.url, pending.insecure, intervalSec,
                         g_sampleIntervalMs.load(), probeStatus});
                }
            }

            // === Build TuiData snapshot ===
            g_loopStage = 1;  // apply-settings + base snapshot fields
            tcmt::TuiData data;
            // Handshake state, visible in the TUI: connecting until the
            // server assigns an id, then uploading with that id attached.
            if (probeActive) {
                probeStatus = s_probe.Token().empty()
                    ? "connecting..."
                    : ("uploading (" + s_probe.DeviceId() + ")");
            }
            data.sampleIntervalMs = g_sampleIntervalMs.load();  // dashboard header
            data.serverPushEnabled = probeActive;
            data.serverUrl = serverUrl;
            data.serverStatus = probeStatus;
            data.lastPushMs = lastPushMs;
            data.updateStatus = s_updater.StatusText();
            data.updateState = static_cast<int>(s_updater.GetState());
            data.osVersion = os.GetVersion();
            data.hardwareModel = os.GetModel();
            // Login identity is fixed for the process lifetime.
            static const tcmt::UserIdentity identity = tcmt::GetUserIdentity();
            data.loginUser = identity.user;
            data.processElevated = identity.isRoot;
            data.elevatedViaSudo = identity.viaSudo;
            data.connectionCount = ipcServer.GetClientCount()
                                 + (motionHttpEnabled ? s_http.ActiveClients() : 0);
            data.httpClientCount = motionHttpEnabled ? s_http.ActiveClients() : 0;
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

            // A frame is "heavy" — sensors sampled, TUI data rebuilt, IPC
            // written — once every g_sampleIntervalMs (TUI settings page,
            // 200..5000 ms; default 500 = 2 Hz). Time-based rather than
            // loop-count-based so the cadence survives slow frames.
            static auto lastHeavyFrame = std::chrono::steady_clock::now() -
                                         std::chrono::minutes(1);  // first frame is heavy
            const auto frameNow = std::chrono::steady_clock::now();
            bool isHeavyFrame = std::chrono::duration_cast<std::chrono::milliseconds>(
                frameNow - lastHeavyFrame).count() >= g_sampleIntervalMs.load();
            if (isHeavyFrame) lastHeavyFrame = frameNow;
            g_loopStage = 2;  // heavy-frame sensor sampling

            // Coordinator snapshot — 2Hz only
            if (isHeavyFrame) {
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
                data.swapUsed = sysInfo.swapUsed;
                data.swapTotal = sysInfo.swapTotal;
                data.ramSpeed = sysInfo.ramSpeed;
                strncpy(data.ramType, sysInfo.ramType, sizeof(data.ramType) - 1);

                // ─── Per-core sensors (macOS: limited availability) ───
                data.perCoreCount = std::min((int)sysInfo.perCoreCount, 16);
                for (int ci = 0; ci < data.perCoreCount; ci++) {
                    data.perCoreTemp[ci] = sysInfo.perCoreTemp[ci];
                    data.perCoreFreq[ci] = sysInfo.perCoreFreq[ci];
                    data.perCoreUsage[ci] = sysInfo.perCoreUsage[ci];
                }
                // macOS has no per-core temperature/frequency source wired in,
                // but CpuInfo samples per-logical-CPU usage from the kernel.
                if (cpuInfo) {
                    cpuInfo->SamplePerCoreUsage();
                    const std::vector<double>& perCore = cpuInfo->GetPerCoreUsage();
                    const int n = (std::min)(static_cast<int>(perCore.size()), 16);
                    for (int i = 0; i < n; ++i)
                        data.perCoreUsage[i] = static_cast<float>(perCore[i]);
                    data.perCoreUsageCount = static_cast<uint8_t>(n);
                }
                // Per-core frequencies from the IOReport channels (PCPU<n> /
                // ECPU<n>); the kernel numbers cores P first, then E, so the
                // P/E split is positional.
                {
                    double freqs[16];
                    const int nf = coordinator.GetPerCoreFreqs(freqs, 16);
                    for (int i = 0; i < nf; ++i)
                        data.perCoreFreq[i] = static_cast<float>(freqs[i]);
                    data.perCoreFreqCount = static_cast<uint8_t>(nf);
                    data.perCorePEKnown = (data.performanceCores > 0);
                }
            }

            // GPU (coordinator doesn't have a GPU loop — keep inline)
            if (gpuInfo) {
                gpuInfo->RefreshUsage();
                const auto& gpus = gpuInfo->GetGpuData();
                // Full list for the panel/details; the scanners below keep
                // describing the first GPU for IPC and JSON.
                data.gpus.clear();
                for (const auto& gpu : gpus) {
                    tcmt::TuiData::GpuEntry e;
                    e.name.assign(gpu.name.begin(), gpu.name.end());
                    e.usage = gpu.usage;
                    e.memory = gpu.dedicatedMemory;
                    e.unified = gpu.isUnified;
                    e.isVirtual = gpu.isVirtual;
                    e.coreCount = gpu.coreCount;
                    data.gpus.push_back(e);
                }
                for (const auto& gpu : gpus) {
                    if (data.gpuName.empty()) {
                        std::string gn(gpu.name.begin(), gpu.name.end());
                        data.gpuName = gn;
                        data.gpuMemory = gpu.dedicatedMemory;
                        data.gpuUnified = gpu.isUnified;
                        data.gpuCoreCount = gpu.coreCount;
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
            static DeviceChangeNotifier s_hubNotify(DeviceChangeNotifier::USB_Hub);
            static DeviceChangeNotifier s_btNotify(DeviceChangeNotifier::Bluetooth);
            static BluetoothInfo s_bt;
            if (++wbCtr >= 6) { wbCtr = 0;
                try { s_wifi.Detect(); } catch (...) {}
                // Poll() only reports change events; Detect() must still run
                // every cycle, otherwise the battery/type profile that lands
                // asynchronously would never reach the panel until the next
                // connect/disconnect.
                s_btNotify.Poll();
                try { s_bt.Detect(); } catch (...) {}
            }
            {
              const auto& wd = s_wifi.GetData();
              data.hasWiFi = wd.powerOn;
              data.wifiConnected = wd.isConnected;
              data.wifiSSID = wd.ssid;
              data.wifiBSSID = wd.bssid;
              data.wifiRSSI = wd.rssi;
              data.wifiChannel = wd.channel;
              data.wifiSecurity = wd.security;
              data.wifiTxRate = wd.txRate;
              data.wifiBand = wd.band;
              data.wifiGen = wd.wifiGen;
              data.wifiLocationDenied = wd.locationDenied;
              data.wifiLocationRoot = wd.locationRoot;
              data.wifiLocationStatus = wd.locationStatus;
              const auto& bd = s_bt.GetData();
              data.hasBluetooth = bd.adapter.detected; // show if adapter hardware detected
              data.btPowerOn = bd.adapter.powerOn;
              data.btDevices.clear();
              {
                  int btConnected = 0;
                  for (const auto& dev : bd.devices) {
                      tcmt::TuiData::BtDeviceInfo d;
                      d.name = dev.name;
                      d.type = dev.type;
                      d.battery = CompactBatteryText(dev);
                      d.rssi = dev.rssi;
                      d.connected = dev.connected;
                      if (dev.connected) ++btConnected;
                      data.btDevices.push_back(std::move(d));
                  }
                  data.btDeviceCount = btConnected;
              }
            }

            // Display monitors, battery health, ALS, accelerometer & thermal state (every ~3 seconds)
            static int displayCtr = 0;
            static DisplayInfo s_display;
            static BatteryHealth s_battery;
            static FanSpeed s_fan;
            static ALSensor s_als;
            if (++displayCtr >= 6) { displayCtr = 0;
                try { s_display.Detect(); } catch (...) {}
                try { s_battery.Detect(); } catch (...) {}
                try { s_fan.Detect(); } catch (...) {}
                try { s_als.Detect(); } catch (...) {}
            } // end heavy frame
            // SPU sensors: SpsManager refreshes all sensors atomically (~500ms)
            try { s_sps.Refresh(); } catch (...) {}

                // Thermal state (available since macOS 10.10.3, well below our 11.0 target)
#ifdef __OBJC__
            NSProcessInfoThermalState ts = [[NSProcessInfo processInfo] thermalState];
            data.thermalState = (int)ts;
#endif
            {
                data.displays.clear();
                for (const auto& d : s_display.GetDisplays()) {
                    tcmt::TuiData::DisplayInfo di;
                    di.name = d.name;
                    di.width = d.width;
                    di.height = d.height;
                    di.refreshRate = d.refreshRate;
                    di.isHDR = d.isHDR;
                    di.isBuiltin = d.isBuiltin;
                    di.backingScale = d.backingScale;
                    data.displays.push_back(di);
                }
            }

            // Battery health
            {
                const auto& bd = s_battery.GetData();
                data.batteryCycleCount = bd.cycleCount;
                data.batteryDesignCapacity = bd.designCapacity;
                data.batteryMaxCapacity = bd.maxCapacity;
                data.batteryHealthPercent = bd.healthPercent;
                data.batteryTemp = bd.temperature;
                data.batteryAmperage = bd.amperage;
                data.batteryVoltage = bd.voltage;
                data.batteryIsCharging = bd.isCharging;
                data.batteryTimeRemainingMin = bd.timeRemainingMin;
                data.batteryFullyCharged = bd.fullyCharged;
                // Charger details; the panel had a Charger row from the start
                // but this assignment was missing, so it never appeared.
                data.chargerWatts = bd.chargerWatts;
                data.chargerName = bd.chargerName;
                data.chargerManufacturer = bd.chargerManufacturer;
                data.chargerModel = bd.chargerModel;
                data.chargerSerial = bd.chargerSerial;
                data.chargerVoltageMv = bd.chargerVoltageMv;
                data.chargerCurrentMa = bd.chargerCurrentMa;
                data.chargerWireless = bd.chargerWireless;
            }

            // ALS (ambient light sensor) — from SPU HID callback (SpsManager)
            // Provides real-time lux + RGBC raw channels at sensor rate (~100Hz).
            // Falls back to IORegistry polling (s_als) if SpsManager ALS is unavailable.
            {
                bool alsFromSps = false;
                for (auto& s : s_sps.Sensors()) {
                    if (s->type == SpsType::ALS && s->sample1.valid) {
                        data.alsValid = true;
                        data.alsLux = s->sample1.value;
                        data.alsChannels.r = s->alsRawR.load(std::memory_order_relaxed);
                        data.alsChannels.g = s->alsRawG.load(std::memory_order_relaxed);
                        data.alsChannels.b = s->alsRawB.load(std::memory_order_relaxed);
                        data.alsChannels.valid = true;
                        alsFromSps = true;
                        break;
                    }
                }
                // Fallback: IORegistry polling (ALSensor)
                if (!alsFromSps) {
                    const auto& ad = s_als.GetData();
                    data.alsValid = ad.valid;
                    data.alsLux = ad.lux;
                }
            }

            // SPU sensors — read latest samples from SpsManager
            {
                data.accel = tcmt::TuiData::AccelInfo{};
                data.gyro = tcmt::TuiData::GyroInfo{};
                data.lidAngle = tcmt::TuiData::LidInfo{};
                bool hasGravitySensor = false;

                for (auto& s : s_sps.Sensors()) {
                    switch (s->type) {
                    case SpsType::Gravity:
                        hasGravitySensor = true;
                        if (s->sample3.valid) {
                            data.accel.hasDevice = true;
                            data.accel.valid = true;
                            data.accel.x = s->sample3.x;
                            data.accel.y = s->sample3.y;
                            data.accel.z = s->sample3.z;
                        }
                        break;
                    case SpsType::Gyro:
                        if (s->sample3.valid) {
                            data.gyro.valid = true;
                            data.gyro.x = s->sample3.x;
                            data.gyro.y = s->sample3.y;
                            data.gyro.z = s->sample3.z;
                        }
                        break;
                    case SpsType::LidAngle:
                        if (s->sample1.valid) {
                            data.lidAngle.valid = true;
                            data.lidAngle.angle = s->sample1.value;
                        }
                        break;
                    case SpsType::Temp:
                        if (s->sample1.valid) {
                            data.spuTemp.valid = true;
                            data.spuTemp.celsius = s->sample1.value;
                        }
                        break;
                    case SpsType::ApplePriv1:
                        if (s->sample1.valid) {
                            data.motionHb.valid = true;
                            data.motionHb.counter = (uint8_t)s->sv.load(std::memory_order_relaxed);
                            data.motionHb.eventFlag = (uint8_t)s->v1.load(std::memory_order_relaxed);
                        }
                        break;
                    case SpsType::ApplePriv5:
                        if (s->sample1.valid) {
                            data.deviceMotion.valid = true;
                            data.deviceMotion.raw = s->sample1.value;
                        }
                        break;
                    default:
                        break;
                    }
                }
                // If HID device exists but no callback data yet, mark available but invalid
                if (hasGravitySensor && !data.accel.valid)
                    data.accel.hasDevice = true;
                // IMU die temp → unified temperature list
                if (data.spuTemp.valid)
                    data.temperatures.push_back({"IMU", data.spuTemp.celsius});
            }

            // Update global motion cache for HTTP server
            {
                std::lock_guard<std::mutex> lk(s_motionMutex);
                s_ax = data.accel.x; s_ay = data.accel.y; s_az = data.accel.z;
                s_gx = data.gyro.x; s_gy = data.gyro.y; s_gz = data.gyro.z;
                s_hasAccel = data.accel.valid;
                s_hasGyro = data.gyro.valid;
                if (data.lidAngle.valid) s_lidAngle = data.lidAngle.angle;
                if (data.spuTemp.valid) s_imut = data.spuTemp.celsius;
                if (data.motionHb.valid) {
                    s_heartbeat = data.motionHb.counter;
                    s_heartFlag = data.motionHb.eventFlag;
                }
            } // motion cache

            // ── Heavy sensors / IPC write: 2Hz only ──
            if (isHeavyFrame) {

            // System uptime / load / process count / top processes
            static ProcessTop s_procTop;      // snapshot read by the loop
            static ProcessTop s_procTopWork;  // refreshed off-thread
            static std::mutex procMutex;
            static std::atomic<bool> procRefreshing{false};
            static int procCtr = 0;
            {
                // Uptime via sysctl kern.boottime
                struct timeval boottime;
                size_t len = sizeof(boottime);
                if (sysctlbyname("kern.boottime", &boottime, &len, NULL, 0) == 0) {
                    data.uptimeSeconds = (uint64_t)(time(NULL) - boottime.tv_sec);
                }
                // Load average
                double load[3];
                if (getloadavg(load, 3) == 3) {
                    data.loadAvg1 = load[0];
                    data.loadAvg5 = load[1];
                    data.loadAvg15 = load[2];
                }
                // Process count
                int n = proc_listallpids(NULL, 0);
                if (n > 0) data.processCount = n;

                // Top processes — refresh every ~3s off-thread: the per-PID
                // proc_pidinfo scan is ~500 syscalls and must not stall the loop.
                if (++procCtr >= 6 && !procRefreshing.load() && !g_shouldExit.load()) {
                    procCtr = 0;
                    procRefreshing.store(true);
                    std::thread([] {
                        try { s_procTopWork.Refresh(); } catch (...) {}
                        {
                            std::lock_guard<std::mutex> lk(procMutex);
                            s_procTop = s_procTopWork;  // top-N + CPU-delta samples
                        }
                        procRefreshing.store(false);
                    }).detach();
                }
                data.topProcesses.clear();
                {
                    std::lock_guard<std::mutex> lk(procMutex);
                    for (const auto& e : s_procTop.GetTop()) {
                        tcmt::TuiData::ProcessTopEntry pe;
                        pe.pid = e.pid;
                        pe.name = e.name;
                        pe.memoryBytes = e.memoryBytes;
                        pe.cpuPercent = e.cpuPercent;
                        // Row context — the collector fills these for the top N
                        // and leaves them empty/-1 otherwise; dropping them here
                        // is what left every context column showing N/A.
                        pe.threads = e.threads;
                        pe.cpuTimeMs = e.cpuTimeMs;
                        pe.state = e.state;
                        pe.user = e.user;
                        data.topProcesses.push_back(pe);
                    }
                }
            }

            // ─── Network traffic history ───
            if (!data.adapters.empty()) {
                uint64_t dl = data.adapters[0].downloadSpeed;
                uint64_t ul = data.adapters[0].uploadSpeed;
                int pos = data.dlHistoryPos;
                data.dlHistory[pos] = dl;
                data.ulHistory[pos] = ul;
                data.dlHistoryPos = (pos + 1) % tcmt::TuiData::NET_HISTORY_MAX;
                if (data.dlHistoryLen < tcmt::TuiData::NET_HISTORY_MAX)
                    data.dlHistoryLen++;
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
            static std::vector<std::string> cachedSmartJson;  // per-disk attrs JSON
            static std::atomic<bool> smartRefreshing{false};
            static std::vector<PhysicalDiskSmartData> smartPending;
            static std::atomic<bool> smartPendingReady{false};
            static std::mutex smartMutex;
            static auto lastSmartRefresh = std::chrono::steady_clock::now() - std::chrono::seconds(61);
            auto nowSt = std::chrono::steady_clock::now();
            // Collect on a background thread: CollectSmartData blocks ~200ms
            // on IOKit and must not stall the monitor loop / TUI update.
            if (std::chrono::duration_cast<std::chrono::seconds>(nowSt - lastSmartRefresh).count() >= 60
                && !smartRefreshing.load() && !g_shouldExit.load()) {
                lastSmartRefresh = nowSt;
                smartRefreshing.store(true);
                std::thread([] {
                    try {
                        SystemInfo tmp;
                        DiskInfo().CollectSmartData(tmp);
                        if (!tmp.physicalDisks.empty()) {
                            std::lock_guard<std::mutex> lk(smartMutex);
                            smartPending = std::move(tmp.physicalDisks);
                            smartPendingReady.store(true);
                        }
                    } catch (...) {}
                    smartRefreshing.store(false);
                }).detach();
            }
            if (smartPendingReady.exchange(false)) {
                std::lock_guard<std::mutex> lk(smartMutex);
                cachedSmart = std::move(smartPending);
                // Rebuild the IPC JSON cache only when SMART data changes.
                cachedSmartJson.clear();
                cachedSmartJson.reserve(cachedSmart.size());
                for (const auto& d : cachedSmart)
                    cachedSmartJson.push_back(SmartAttrsToJson(d));
            }
                data.physicalDisks.clear();
                data.physicalDisks.reserve(cachedSmart.size());
                // WCHAR (char16_t) → UTF-8 helper
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
                // Add physical disk temps to unified temperature list
                for (size_t di = 0; di < cachedSmart.size(); ++di) {
                    if (cachedSmart[di].temperature > 0) {
                        std::string label = w2u(cachedSmart[di].model, 63);
                        if (label.empty()) label = "Disk";
                        data.temperatures.push_back({label, cachedSmart[di].temperature});
                    }
                }

            // Update TUI
            g_loopStage = 3;  // UpdateData + IPC write
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
                        b->cpuPcoreTemp = static_cast<float>(data.cpuPcoreTemp);
                        b->cpuEcoreTemp = static_cast<float>(data.cpuEcoreTemp);
                        b->hyperThreading = false;
                        b->virtualization = false;
                        b->cpuSampleIntervalMs = 500.0f;
                        b->cpuBaseFreq = static_cast<float>(cpuInfo->GetLargeCoreSpeed());
                        // Memory
                        b->totalMemory = data.totalMemory;
                        b->usedMemory = data.usedMemory;
                        b->availableMemory = data.availableMemory;
                        b->compressedMemory = data.compressedMemory;
                        b->swapUsed = data.swapUsed;
                        b->swapTotal = data.swapTotal;
                        b->ramSpeed = data.ramSpeed;
                        std::strncpy(b->ramType, data.ramType, 31);
                        b->ramType[31] = '\0';
                        // Battery / power
                        b->batteryPercent = data.batteryPercent;
                        b->acOnline = data.acOnline;
                        b->cpuPower = static_cast<float>(data.cpuPower);
                        b->gpuPower = static_cast<float>(data.gpuPower);
                        b->anePower = static_cast<float>(data.anePower);
                        // OS
                        std::strncpy(b->osVersion, data.osVersion.c_str(), 127);
                        b->osVersion[127] = '\0';
                        std::strncpy(b->hardwareModel, data.hardwareModel.c_str(), 127);
                        b->hardwareModel[127] = '\0';
                        // App version
                        std::strncpy(b->appVersion, APP_VERSION, 31);
                        b->appVersion[31] = '\0';
                        // GPU
                        std::strncpy(b->gpuName, data.gpuName.c_str(), 47);
                        b->gpuName[47] = '\0';
                        b->gpuMemory = data.gpuMemory;
                        b->gpuMemoryPercent = static_cast<float>(data.gpuMemoryPercent);
                        b->gpuUsage = static_cast<float>(data.gpuUsage);
                        b->gpuTemp = static_cast<float>(data.gpuTemp);
                        b->gpuFreq = static_cast<float>(data.gpuFreq);
                        b->gpuIsVirtual = false;
                        b->gpuUnified = data.gpuUnified;
                        // 整份 GPU 列表（第 0 项为主卡）。上面的单值字段只装得下主卡，
                        // 多卡机器（Intel Mac 的核显 + 独显）其余几块只在这里留得下。
                        {
                            std::vector<GpuSpec> specs;
                            if (gpuInfo) {
                                for (const auto& g : gpuInfo->GetGpuData()) {
                                    GpuSpec s;
                                    s.name = std::string(g.name.begin(), g.name.end());
                                    s.brand = GpuBrandFromName(s.name);
                                    s.memory = g.dedicatedMemory;
                                    s.coreClock = g.coreClock;
                                    s.temperature = static_cast<double>(g.temperature);
                                    s.isVirtual = g.isVirtual;
                                    s.isUnified = g.isUnified;
                                    s.usage = g.usage;
                                    specs.push_back(s);
                                }
                            }
                            if (!specs.empty()) {
                                // 主卡换成这一帧的实时值 —— gpu/0/* 用的是同一批数。
                                // coreClock 尤其要换：GpuInfo 那份里是 0，频率在 PowerMonitor 那边
                                // （data.gpuFreq），不换就等于把「核心频率」这一行弄没了。
                                specs[0].memoryPercent = data.gpuMemoryPercent;
                                specs[0].usage = data.gpuUsage;
                                specs[0].temperature = data.gpuTemp;
                                if (data.gpuFreq > 0) specs[0].coreClock = data.gpuFreq;
                                std::strncpy(b->gpuBrand, specs[0].brand.c_str(),
                                             sizeof(b->gpuBrand) - 1);
                                b->gpuBrand[sizeof(b->gpuBrand) - 1] = '\0';
                            }
                            const std::string gpuListJson = GpusToJson(specs);
                            std::strncpy(b->gpuJson, gpuListJson.c_str(), sizeof(b->gpuJson) - 1);
                            b->gpuJson[sizeof(b->gpuJson) - 1] = '\0';
                        }
                        // Disks (up to 4)
                        b->diskCount = 0;
                        for (size_t di = 0; di < std::min(data.disks.size(), size_t(4)); ++di) {
                            auto& d = b->disks[di];
                            std::strncpy(d.label, data.disks[di].label.c_str(), 31);
                            d.label[31] = '\0';
                            std::strncpy(d.mountPoint, data.disks[di].mountPoint.c_str(), 63);
                            d.mountPoint[63] = '\0';
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
                            // 当前地址：macOS 上采集端就是空的（系统限制，见 NetworkAdapter.cpp 的说明），
                            // 原样写出去，界面据"空"判断取不到 —— 不拿硬件地址冒充。
                            std::strncpy(n.macCurrent, data.adapters[ai].currentMac.c_str(), sizeof(n.macCurrent) - 1);
                            n.macCurrent[sizeof(n.macCurrent) - 1] = '\0';
                            std::strncpy(n.type, data.adapters[ai].type.c_str(), sizeof(n.type) - 1);
                            n.speed = data.adapters[ai].speed;
                            n.downloadSpeed = data.adapters[ai].downloadSpeed;
                            n.uploadSpeed = data.adapters[ai].uploadSpeed;
                            b->adapterCount++;
                        }
                        // 温度传感器：整份打包进一个字段，不再截到 10 个
                        {
                            const std::string sensorsJson = SensorsToJson(data.temperatures);
                            std::strncpy(b->sensorsJson, sensorsJson.c_str(), sizeof(b->sensorsJson) - 1);
                            b->sensorsJson[sizeof(b->sensorsJson) - 1] = '\0';
                        }
                        // Physical disks (SMART) — reuse TUI cache for IPC
                        {
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
                                // SMART attributes JSON — precomputed at SMART
                                // refresh (60s); never rebuilt per frame.
                                if (src.attributeCount > 0 && pi < cachedSmartJson.size()) {
                                    const std::string& jstr = cachedSmartJson[pi];
                                    if (!jstr.empty() && jstr.size() < 4096) {
                                        std::strncpy(pd.attrsJson, jstr.c_str(), 4095);
                                        pd.attrsJson[4095] = '\0';
                                        pd.attrCount = src.attributeCount;
                                    }
                                }
                                b->physDiskCount++;
                            }
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
                    std::strncpy(b->wifi.band, wd2.band.c_str(), sizeof(b->wifi.band) - 1);
                    b->wifi.band[sizeof(b->wifi.band) - 1] = '\0';
                    std::strncpy(b->wifi.wifiGen, wd2.wifiGen.c_str(), sizeof(b->wifi.wifiGen) - 1);
                    b->wifi.wifiGen[sizeof(b->wifi.wifiGen) - 1] = '\0';
                    b->wifi.powerOn = wd2.powerOn;
                    b->wifi.isConnected = wd2.isConnected;
                    // Bluetooth
                    const auto& bd2 = s_bt.GetData();
                    b->bluetooth.powerOn = bd2.adapter.powerOn;
                    b->bluetooth.deviceCount = static_cast<int32_t>(bd2.devices.size());
                    // name 装"已连接设备名"，不装适配器名（原来是后者，而且两端都没人读）。
                    // 拼装逻辑与 Windows 侧共用 BluetoothInfo.h 的 JoinConnectedDeviceNames。
                    const std::string btNames =
                        JoinConnectedDeviceNames(bd2, sizeof(b->bluetooth.name) - 1);
                    std::strncpy(b->bluetooth.name, btNames.c_str(), sizeof(b->bluetooth.name) - 1);
                    b->bluetooth.name[sizeof(b->bluetooth.name) - 1] = '\0';

                    // ─── 4. Fan speeds ───
                    b->fanCount = 0;
                    {
                        const auto& fans = s_fan.GetFans();
                        b->fanCount = static_cast<uint8_t>(std::min(fans.size(), size_t(6)));
                        for (size_t fi = 0; fi < b->fanCount; ++fi) {
                            std::strncpy(b->fanSpeeds[fi].name, fans[fi].name.c_str(), 31);
                            b->fanSpeeds[fi].name[31] = '\0';
                            b->fanSpeeds[fi].rpm = fans[fi].rpm;
                        }
                    }

                    // ─── 5. 进程列表：整份打包进一个字段，不再截到 7 个 ───
                    {
                        const std::string procsJson = ProcessesToJson(data.topProcesses);
                        std::strncpy(b->processesJson, procsJson.c_str(), sizeof(b->processesJson) - 1);
                        b->processesJson[sizeof(b->processesJson) - 1] = '\0';
                    }

                    // ─── 7. Battery detail ───
                    const auto& bd = s_battery.GetData();
                    b->batteryCycleCount = bd.cycleCount;
                    b->batteryDesignCapacity = bd.designCapacity;
                    b->batteryMaxCapacity = bd.maxCapacity;
                    b->batteryHealthPercent = static_cast<float>(bd.healthPercent);
                    b->batteryTemp = static_cast<float>(bd.temperature);
                    b->batteryAmperage = bd.amperage;
                    b->batteryVoltage = bd.voltage;
                    b->batteryChargerWatts = static_cast<float>(bd.chargerWatts);
                    b->batteryIsCharging = bd.isCharging;
                    b->batteryIsPresent = bd.present;

                    // ─── 6. 逐核传感器：整份打包进一个字段 ───
                    // macOS 侧还拿不到逐核数据（要等 TemperatureWrapper 扩出来），这里照实写空数组，
                    // 不占位造数。
                    {
                        const std::string coresJson =
                            CoresToJson(data.perCoreTemp, data.perCoreFreq,
                                        static_cast<int>(data.perCoreCount));
                        std::strncpy(b->cpuCoresJson, coresJson.c_str(),
                                     sizeof(b->cpuCoresJson) - 1);
                        b->cpuCoresJson[sizeof(b->cpuCoresJson) - 1] = '\0';
                    }

                    // ─── System info (load avg, process count, uptime) ───
                    b->loadAvg1 = static_cast<float>(data.loadAvg1);
                    b->loadAvg5 = static_cast<float>(data.loadAvg5);
                    b->loadAvg15 = static_cast<float>(data.loadAvg15);
                    b->processCount = data.processCount;
                    b->uptimeSeconds = data.uptimeSeconds;

                    // seqlock: mark write complete (even)
                    std::atomic_thread_fence(std::memory_order_release);
                    b->writeSequence++;
                }

            } // isHeavyFrame

            // Push snapshot to tcmt-server. Heavy-frame only (~1Hz): cpu/mem/gpu
            // values are only populated on heavy frames, so pushing every loop
            // would upload mostly zeros.
            g_loopStage = 4;  // snapshot push
            // Time-based upload gate (configurable 1..60 s via TUI settings).
            const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            const bool pushDue = lastPushMs == 0 || nowMs - lastPushMs >= intervalMs;
            if (probeActive && isHeavyFrame && pushDue && s_probe.Token().size() > 0) {
                char snap[1024];
                snprintf(snap, sizeof(snap),
                    "\"cpu_usage\":%.1f,\"cpu_temp\":%.1f,"
                    "\"memory_used\":%llu,\"memory_total\":%llu,"
                    "\"gpu_usage\":%.1f,\"gpu_temp\":%.1f,"
                    "\"ax\":%.4f,\"ay\":%.4f,\"az\":%.4f,"
                    "\"gx\":%.2f,\"gy\":%.2f,\"gz\":%.2f,"
                    "\"lidAngle\":%.1f,\"hb\":%d,\"imut\":%.1f,"
                    "\"cpu_name\":\"%s\",\"gpu_name\":\"%s\","
                    "\"os_version\":\"%s\",\"uptime\":%llu",
                    data.cpuUsage, data.cpuTemp,
                    (unsigned long long)data.usedMemory, (unsigned long long)data.totalMemory,
                    data.gpuUsage, data.gpuTemp,
                    s_ax, s_ay, s_az, s_gx, s_gy, s_gz,
                    s_lidAngle, s_heartbeat, s_imut,
                    JsonSafe(cachedCpuName).c_str(), JsonSafe(data.gpuName).c_str(),
                    JsonSafe(data.osVersion).c_str(),
                    (unsigned long long)data.uptimeSeconds);
                std::string body = "{";
                body += snap;
                // Disks (volumes): name + used/total bytes.
                body += ",\"disks\":[";
                for (size_t di = 0; di < data.disks.size(); ++di) {
                    if (di) body += ",";
                    const auto& dsk = data.disks[di];
                    std::string dname = !dsk.label.empty() ? dsk.label
                        : (dsk.letter ? std::string(1, dsk.letter) : std::string("?"));
                    char db[256];
                    snprintf(db, sizeof(db), "{\"name\":\"%s\",\"used\":%llu,\"total\":%llu}",
                             JsonSafe(dname).c_str(),
                             (unsigned long long)dsk.usedSpace,
                             (unsigned long long)dsk.totalSize);
                    body += db;
                }
                body += "]";
                // Network adapters: name + down/up speeds (bytes/s).
                body += ",\"net\":[";
                for (size_t ni = 0; ni < data.adapters.size(); ++ni) {
                    if (ni) body += ",";
                    char nb[256];
                    snprintf(nb, sizeof(nb), "{\"name\":\"%s\",\"down\":%llu,\"up\":%llu}",
                             JsonSafe(data.adapters[ni].name).c_str(),
                             (unsigned long long)data.adapters[ni].downloadSpeed,
                             (unsigned long long)data.adapters[ni].uploadSpeed);
                    body += nb;
                }
                body += "]";
                // Battery (optional — -1 means no battery).
                if (data.batteryPercent >= 0) {
                    char bb[96];
                    snprintf(bb, sizeof(bb),
                             ",\"battery\":{\"percent\":%d,\"charging\":%s,\"health\":%.1f}",
                             data.batteryPercent,
                             data.acOnline ? "true" : "false",
                             data.batteryHealthPercent);
                    body += bb;
                }
                // Full temperature sensor list (SMC etc.) — the viewer shows
                // everything here, not just CPU/GPU.
                auto temps = TemperatureWrapper::GetTemperatures();
                body += ",\"temperatures\":[";
                for (size_t ti = 0; ti < temps.size(); ++ti) {
                    if (ti) body += ",";
                    char tb[192];
                    snprintf(tb, sizeof(tb), "{\"name\":\"%s\",\"value\":%.1f,\"unit\":\"C\"}",
                             temps[ti].first.c_str(), temps[ti].second);
                    body += tb;
                }
                body += "]}";
                s_probe.PostSnapshot(std::move(body));
                lastPushMs = nowMs;
            }

            // Sleep
            auto loopEnd = std::chrono::high_resolution_clock::now();
            int loopMs = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                loopEnd - loopStart).count();
            // Stall diagnostics: if a sensor call hung, the stage marker
            // tells us where. (Then `sample TCMT-M 2` while frozen nails it.)
            if (loopMs > 2000) {
                Logger::Warn("loop iteration slow: " + std::to_string(loopMs) +
                             "ms at stage " + std::to_string(g_loopStage.load()));
            }
            int sleepMs = std::max(33 - loopMs, 5);  // 30 Hz update
            // Sleep with responsive exit (check every 5ms)
            for (int s = 0; s < 7 && !g_shouldExit.load(); ++s) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            // Push sensor snapshots to history logger (heavy-frame only, same reason)
            if (isHeavyFrame && historyLogger.IsRunning()) {
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
            static size_t prevUsbCount = 0;
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

        if (logWindow.IsActive())
            tcmt::mac::MacLogWindow::StopApp();
    });

    // Main thread: run the AppKit event loop so the log window is fully
    // interactive (scrolling, selection, copy). Without a window (headless
    // fallback) just wait for the monitoring thread to finish.
    if (logWindow.IsActive()) {
        logWindow.Run();
    }
    monitorThread.join();

    Logger::Info("Exiting, cleaning up...");
    logWindow.Stop();
    tuiApp.Stop();
    TemperatureWrapper::Cleanup();
    historyLogger.Shutdown();
    Logger::Info("HistoryLogger stopped");
    ipcServer.Stop();
    Logger::Info("IPC server stopped");
    Logger::Info("Done.");
    return 0;
}
