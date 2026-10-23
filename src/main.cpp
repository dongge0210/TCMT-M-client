#pragma unmanaged

#pragma comment(linker, "/STACK:8388608")  // Set stack size

/*
If you see warning MSB8077: Some files are set to C++/CLI but "Enable CLR Support for Single File" property is not defined.
Please ignore this warning - the project structure doesn't support this scenario
*/
// Do NOT include winsock2.h here - it breaks other headers that include windows.h first
// Network headers are included in the platform-specific source files instead
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>

// Manual GUID definition — bypasses LNK2001 for GUID_DEVINTERFACE_USB_HUB
const GUID GUID_DEVINTERFACE_USB_HUB = {
    0xf18a0e88, 0xc30c, 0x11d0, { 0x88, 0x15, 0x00, 0xa0, 0xc9, 0x06, 0xbe, 0xd8 }
};
#include <sddl.h>
#include <Aclapi.h>
#include <conio.h>
#include <eh.h>

#include <chrono>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <utility>
#include <thread>
#include <io.h>
#include <fcntl.h>
#include <algorithm>
#include <vector>
#include <mutex>
#include <atomic>
#include <locale>
#include <new>
#include <memory>
#include <stdexcept>

#include "core/cpu/CpuInfo.h"
#include "core/gpu/GpuInfo.h"
#include "core/memory/MemoryInfo.h"
#include "core/network/NetworkAdapter.h"
#include "core/os/OSInfo.h"
#include "core/process/ProcessMonitor.h"
#include "core/Utils/Logger.h"
#include "core/Utils/UserIdentity.h"
#include "core/Utils/TimeUtils.h"
#include "core/Utils/WinUtils.h"
#include "core/Utils/WmiManager.h"
#include "core/disk/DiskInfo.h"
#include "core/disk/SmartReader.h"
#include "core/Utils/TpmBridge.h"
#include "core/DataStruct/DataStruct.h"
#include "core/DataStruct/SharedMemoryManager.h"
#include "core/IPC/IPCServer.h"
#include "core/history/HistoryLogger.h"
#include "core/usb/UsbInfo.h"
#include "core/wifi/WiFiInfo.h"
#include <USBMonitor.h>
#include "core/bluetooth/BluetoothInfo.h"
#include "core/notifications/DeviceChangeNotifier.h"
#include "core/MCP/MCPServer.h"
#include "core/IPC/IPCClient.h"
#include "core/temperature/TemperatureWrapper.h"
#include "core/cpu/CpuTempReader.h"
#include "core/coordinator/ModuleCoordinator.h"
#include "tui/TuiApp.h"
#include "tui/LogWindow.h"
#include "core/Config/ConfigManager.h"
#include "core/ServerProbe.h"
#include <fstream>
#include <cstdio>

#pragma comment(lib, "kernel32.lib")
#pragma comment(lib, "user32.lib")

std::atomic<bool> g_shouldExit{false};

// ── Server push (tcmt-server) — settings edited in the TUI (press S), ──
// ── persisted to <exe-dir>\system_monitor.json (server.*).              ──
static ConfigManager& GetServerCfg() {
    static ConfigManager cfg(WinUtils::GetExecutableDirectory() + "\\system_monitor.json");
    static const bool loaded = cfg.Load();
    (void)loaded;
    return cfg;
}

struct ServerPushState {
    ServerProbe probe;
    bool enabled = false;
    std::string url = "http://127.0.0.1:8080";
    bool insecure = false;
    int intervalSec = 2;        // hard bounds 1..60
    int64_t intervalMs = 2000;
    bool active = false;
    std::string status = "disabled";
    int64_t lastPushMs = 0;
    std::mutex mutex;
    tcmt::ServerSettings pending;
    bool pendingReady = false;
};
static ServerPushState g_push;

static std::string JsonSafeStr(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c == '"' || c == '\\') c = '_';
    }
    return out;
}
// 采样周期（毫秒）。TUI 设置页那个「采样间隔」改的就是它。
// macOS 那边同样的东西叫 g_sampleIntervalMs，这里是 Windows 的对应物 —— 以前根本没有：
// 主循环里写死 int targetCycleTime = 1000，所以设置页里改采样间隔在这台机器上是空操作。
// 默认值沿用 Windows 原来的 1 秒（不是 macOS 的 500ms —— 那是另一台机器的既有行为，
// 要统一得单独说）。
static std::atomic<int> g_sampleIntervalMs{1000};

// ── IPC ConfigSet：Avalonia 设置页改配置的唯一入口 ──
// 服务端只负责搬 JSON；认键、校验、落盘、生效在这里。没有白名单的话，
// 客户端一个请求就能把 os.version / cpu.name 这类运行时快照写进配置文件。
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

std::string BuildConfigSnapshotJson() {
    auto& cfg = GetServerCfg();
    nlohmann::json j;
    j["server.enabled"]      = cfg.GetBool("server.enabled", false);
    j["server.url"]          = cfg.GetString("server.url", "");
    j["server.insecure"]     = cfg.GetBool("server.insecure", false);
    j["server.intervalSec"]  = cfg.GetInt("server.intervalSec", 2);
    j["sampling.intervalMs"] = g_sampleIntervalMs.load();
    j["ui.language"]         = cfg.GetString("ui.language", "zh-Hans");
    return j.dump();
}

void ApplyClientConfigSet(const std::string& payload, tcmt::ipc::IPCServer& ipcServer) {
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

    auto& cfg = GetServerCfg();
    int applied = 0;
    for (auto it = in.begin(); it != in.end(); ++it) {
        const CfgKey* k = FindCfgKey(it.key());
        if (!k) {
            Logger::Warn("IPC: ConfigSet 含白名单外的键，忽略: " + it.key());
            continue;
        }
        try {
            switch (k->type) {
                case CfgType::Bool:
                    if (!it.value().is_boolean()) throw std::runtime_error("要布尔值");
                    cfg.SetBool(k->key, it.value().get<bool>());
                    break;
                case CfgType::Int:
                    if (!it.value().is_number_integer()) throw std::runtime_error("要整数");
                    cfg.SetInt(k->key, it.value().get<int>());
                    break;
                case CfgType::Str:
                    if (!it.value().is_string()) throw std::runtime_error("要字符串");
                    cfg.SetString(k->key, it.value().get<std::string>());
                    break;
            }
            applied++;
        } catch (const std::exception& e) {
            Logger::Warn(std::string("IPC: ConfigSet 的 ") + k->key + " 类型不对（" +
                         e.what() + "），忽略");
        }
    }

    if (applied == 0) return;
    cfg.Save();

    // 采样间隔除了落盘还要喂那个原子量 —— RunMonitoringLoop 每轮读它，
    // 发现变了才调 coordinator.SetSampleIntervalMs。
    const int ms = cfg.GetInt("sampling.intervalMs", g_sampleIntervalMs.load());
    g_sampleIntervalMs.store(
        (std::max)(tcmt::kSampleIntervalMinMs,
                   (std::min)(tcmt::kSampleIntervalMaxMs, ms)));

    // 上传设置走 TUI 设置页同一条 pending 通道，主循环会重启探针。
    tcmt::ServerSettings s;
    s.enabled     = cfg.GetBool("server.enabled", false);
    s.url         = cfg.GetString("server.url", "");
    s.insecure    = cfg.GetBool("server.insecure", false);
    s.intervalSec = cfg.GetInt("server.intervalSec", 2);
    {
        std::lock_guard<std::mutex> lock(g_push.mutex);
        g_push.pending = s;
        g_push.pendingReady = true;
    }

    Logger::Info("IPC: 客户端改了 " + std::to_string(applied) + " 项配置，已保存并生效");
    ipcServer.BroadcastConfigSnapshot();
}

}  // namespace

static std::atomic<bool> g_monitoringStarted{false};
static std::atomic<bool> g_comInitialized{false};

static std::mutex g_consoleMutex;

bool CheckForKeyPress();
char GetKeyPress();
void SafeExit(int exitCode);

void SEHTranslator(unsigned int u, EXCEPTION_POINTERS* pExp);
std::string GetSEHExceptionName(DWORD exceptionCode);

void SafeConsoleOutput(const std::string& message);
void SafeConsoleOutput(const std::string& message, int color);
BOOL WINAPI ConsoleCtrlHandler(DWORD dwCtrlType) {
    switch (dwCtrlType) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
    {
        // Ctrl+C can be delivered multiple times — only act once, otherwise the
        // exit path gets spammed with "Received shutdown signal" log lines.
        static std::atomic<bool> handled{false};
        if (handled.exchange(true)) return TRUE;
        Logger::Info("Received shutdown signal, exiting safely...");
        g_shouldExit = true;
        SafeConsoleOutput("Exiting program...\n", 14);
        // The cooperative path can still block in a hardware ioctl or a thread
        // join. Never leave the console hung after Ctrl+C: if the process is
        // still alive five seconds from now, end it.
        try {
            std::thread([] {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                TerminateProcess(GetCurrentProcess(), 0);
            }).detach();
        } catch (...) {
        }
        return TRUE;
    }
    }
    return FALSE;
}

// Thread-safe console output function implementation
void SafeConsoleOutput(const std::string& message) {
    std::lock_guard<std::mutex> lock(g_consoleMutex);
    try {
        // Use UTF-8 encoding for output
        HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
        if (hConsole != INVALID_HANDLE_VALUE) {
            // Ensure input string is not empty
            if (message.empty()) {
                return;
            }
            
            // Convert UTF-8 string to UTF-16 (Wide Character)
            int wideLength = MultiByteToWideChar(CP_UTF8, 0, message.c_str(), -1, nullptr, 0);
            if (wideLength > 0) {
                std::vector<wchar_t> wideMessage(wideLength);
                if (MultiByteToWideChar(CP_UTF8, 0, message.c_str(), -1, wideMessage.data(), wideLength)) {
                    // Use WriteConsoleW to output Unicode text directly
                    DWORD written;
                    WriteConsoleW(hConsole, wideMessage.data(), static_cast<DWORD>(wideLength - 1), &written, NULL);
                    return;
                }
            }
            
            // If UTF-8 conversion fails, fall back to ASCII output
            DWORD written;
            WriteConsoleA(hConsole, message.c_str(), static_cast<DWORD>(message.length()), &written, NULL);
        }
    }
    catch (...) {

    }
}

void SafeConsoleOutput(const std::string& message, int color) {
    std::lock_guard<std::mutex> lock(g_consoleMutex);
    try {
        HANDLE hConsole = GetStdHandle(STD_OUTPUT_HANDLE);
        if (hConsole != INVALID_HANDLE_VALUE) {
            // Save original color
            CONSOLE_SCREEN_BUFFER_INFO csbi;
            GetConsoleScreenBufferInfo(hConsole, &csbi);
            WORD originalColor = csbi.wAttributes;
            
            SetConsoleTextAttribute(hConsole, color);
            
            // Ensure input string is not empty
            if (!message.empty()) {
                // Convert UTF-8 string to UTF-16 (Wide Character)
                int wideLength = MultiByteToWideChar(CP_UTF8, 0, message.c_str(), -1, nullptr, 0);
                if (wideLength > 0) {
                    std::vector<wchar_t> wideMessage(wideLength);
                    if (MultiByteToWideChar(CP_UTF8, 0, message.c_str(), -1, wideMessage.data(), wideLength)) {
                        // Use WriteConsoleW to output Unicode text directly
                        DWORD written;
                        WriteConsoleW(hConsole, wideMessage.data(), static_cast<DWORD>(wideLength - 1), &written, NULL);
                    } else {
                        // If conversion fails, fall back to ASCII output
                        DWORD written;
                        WriteConsoleA(hConsole, message.c_str(), static_cast<DWORD>(message.length()), &written, NULL);
                    }
                }
            }
            
            // Restore original color
            SetConsoleTextAttribute(hConsole, originalColor);
        }
    }
    catch (...) {
        // Ignore console output errors to avoid recursive exceptions
    }
}

// Structured exception handling implementation
std::string GetSEHExceptionName(DWORD exceptionCode) {
    switch (exceptionCode) {
        case EXCEPTION_ACCESS_VIOLATION: return "Access Violation";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "Array Bounds Exceeded";
        case EXCEPTION_BREAKPOINT: return "Breakpoint";
        case EXCEPTION_DATATYPE_MISALIGNMENT: return "Data Type Misalignment";
        case EXCEPTION_FLT_DENORMAL_OPERAND: return "Float Denormal Operand";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "Float Divide By Zero";
        case EXCEPTION_FLT_INEXACT_RESULT: return "Float Inexact Result";
        case EXCEPTION_FLT_INVALID_OPERATION: return "Float Invalid Operation";
        case EXCEPTION_FLT_OVERFLOW: return "Float Overflow";
        case EXCEPTION_FLT_STACK_CHECK: return "Float Stack Check";
        case EXCEPTION_FLT_UNDERFLOW: return "Float Underflow";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "Illegal Instruction";
        case EXCEPTION_IN_PAGE_ERROR: return "In Page Error";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "Integer Divide By Zero";
        case EXCEPTION_INT_OVERFLOW: return "Integer Overflow";
        case EXCEPTION_INVALID_DISPOSITION: return "Invalid Disposition";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "Noncontinuable Exception";
        case EXCEPTION_PRIV_INSTRUCTION: return "Privilege Instruction";
        case EXCEPTION_SINGLE_STEP: return "Single Step";
        case EXCEPTION_STACK_OVERFLOW: return "Stack Overflow";
        default: return "Unknown System Exception (0x" + std::to_string(exceptionCode) + ")";
    }
}

void SEHTranslator(unsigned int u, EXCEPTION_POINTERS* pExp) {
    std::string exceptionName = GetSEHExceptionName(u);
    std::stringstream ss;
    ss << "System Exception: " << exceptionName << " (0x" << std::hex << u << ")";
    if (pExp && pExp->ExceptionRecord) {
        ss << " Address: 0x" << std::hex << pExp->ExceptionRecord->ExceptionAddress;
    }
    
    // Try to safely log
    try {
        if (Logger::IsInitialized()) {
            Logger::Fatal(ss.str());
        } else {
            // If logging system not initialized, output directly to console
            SafeConsoleOutput("FATAL: " + ss.str() + "\n");
        }
    } catch (...) {
        // Last resort, direct output
        SafeConsoleOutput("FATAL: " + ss.str() + "\n");
    }
    
    throw std::runtime_error(ss.str());
}

// Safe exit function
void SafeExit(int exitCode) {
    try {
        Logger::Info("Starting cleanup process");
        
        // Set exit flag
        g_shouldExit = true;
        
        // Cleanup hardware monitoring bridge
        try {
            TemperatureWrapper::Cleanup();
            Logger::Debug("Hardware monitoring bridge cleanup complete");
        }
        catch (const std::exception& e) {
            Logger::Error("Error cleaning up hardware monitoring bridge: " + std::string(e.what()));
        }
        
        // Cleanup shared memory
        try {
            SharedMemoryManager::CleanupSharedMemory();
            Logger::Debug("Shared memory cleanup complete");
        }
        catch (const std::exception& e) {
            Logger::Error("Error cleaning up shared memory: " + std::string(e.what()));
        }
        
        // Cleanup COM
        if (g_comInitialized.load()) {
            try {
                CoUninitialize();
                g_comInitialized = false;
                Logger::Debug("COM cleanup complete");
            }
            catch (...) {
                Logger::Error("Unknown error cleaning up COM");
            }
        }
        
        Logger::Info("Cleanup complete, exit code: " + std::to_string(exitCode));
        
        // Give logging system time to complete writes
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    catch (...) {
        // Last exception handling to avoid crashing during cleanup
    }
    
    exit(exitCode);
}

// Helper functions
// Hardware name translation
std::string TranslateHardwareName(const std::string& name) {
    if (name.find("CPU Package") != std::string::npos) return "CPU Temperature";
    if (name.find("GPU Core") != std::string::npos) return "GPU Temperature";
    return name;
}

// Brand detection
std::string GetGpuBrand(const std::wstring& name) {
    if (name.find(L"NVIDIA") != std::wstring::npos) return "NVIDIA";
    if (name.find(L"AMD") != std::wstring::npos) return "AMD";
    if (name.find(L"Intel") != std::wstring::npos) return "Intel";
    return "Unknown";
}

// Network speed unit
std::string FormatNetworkSpeed(double speedBps) {
    std::stringstream ss;
    ss << std::fixed << std::setprecision(1);

    if (speedBps >= 1000000000) {
        ss << (speedBps / 1000000000) << " Gbps";
    }
    else if (speedBps >= 1000000) {
        ss << (speedBps / 1000000) << " Mbps";
    }
    else if (speedBps >= 1000) {
        ss << (speedBps / 1000) << " Kbps";
    }
    else {
        ss << speedBps << " bps";
    }
    return ss.str();
}

// Time formatting - enhanced exception handling
std::string FormatDateTime(const std::chrono::system_clock::time_point& tp) {
    try {
        auto time = std::chrono::system_clock::to_time_t(tp);
        struct tm timeinfo;
        if (localtime_s(&timeinfo, &time) == 0) {  // Check return value
            std::stringstream ss;
            ss << std::put_time(&timeinfo, "%Y-%m-%d %H:%M:%S");
            std::string result = ss.str();
            
            // Verify result reasonableness
            if (result.length() >= 19 && result.length() <= 25) {  // Basic length check
                return result;
            } else {
                Logger::Debug("Time format result length abnormal: " + std::to_string(result.length()));
            }
        } else {
            Logger::Debug("localtime_s call failed");
        }
    }
    catch (const std::exception& e) {
        Logger::Debug("Exception during time formatting: " + std::string(e.what()));
    }
    catch (...) {
        Logger::Debug("Exception during time formatting - unknown exception");
    }
    return "Time Formatting Failed";
}

std::string FormatFrequency(double value) {
    try {
        // Parameter validation
        if (std::isnan(value) || std::isinf(value)) {
            Logger::Debug("Invalid frequency value: " + std::to_string(value));
            return "N/A";
        }

        if (value < 0) {
            Logger::Debug("Frequency value is negative: " + std::to_string(value));
            return "N/A";
        }

        // Reasonableness check - frequency typically not exceeding 10GHz
        if (value > 10000) {
            Logger::Debug("Frequency value abnormal: " + std::to_string(value) + "MHz");
            return "Abnormal Value";
        }
        
        std::stringstream ss;
        ss << std::fixed << std::setprecision(1);

        if (value >= 1000) {
            ss << (value / 1000.0) << " GHz";
        }
        else {
            ss << value << " MHz";
        }
        return ss.str();
    }
    catch (const std::exception& e) {
        Logger::Debug("Exception during frequency formatting: " + std::string(e.what()));
        return "Formatting Failed";
    }
    catch (...) {
        Logger::Debug("Exception during frequency formatting - unknown exception");
        return "Formatting Failed";
    }
}

std::string FormatPercentage(double value) {
    try {
        // Parameter validation
        if (std::isnan(value) || std::isinf(value)) {
            Logger::Debug("Percentage value invalid: " + std::to_string(value));
            return "N/A";
        }

        // Reasonableness check - percentage typically between 0-100
        if (value < -1.0 || value > 105.0) {
            Logger::Debug("Percentage value abnormal: " + std::to_string(value));
        }
        
        // Limit to reasonable range
        if (value < 0) value = 0;
        if (value > 100) value = 100;
        
        std::stringstream ss;
        ss << std::fixed << std::setprecision(1) << value << "%";
        return ss.str();
    }
    catch (const std::exception& e) {
        Logger::Debug("Exception during percentage formatting: " + std::string(e.what()));
        return "Formatting Failed";
    }
    catch (...) {
        Logger::Debug("Exception during percentage formatting - unknown exception");
        return "Formatting Failed";
    }
}

std::string FormatTemperature(double value) {
    try {
        // Parameter validation
        if (std::isnan(value) || std::isinf(value)) {
            Logger::Debug("Temperature value invalid: " + std::to_string(value));
            return "N/A";
        }

        // Reasonableness check - temperature typically between -50C to 150C
        if (value < -50.0 || value > 150.0) {
            Logger::Debug("Temperature value abnormal: " + std::to_string(value) + "°C");
            if (value < -50.0) return "Too Low";
            if (value > 150.0) return "Too High";
        }
        
        std::stringstream ss;
        ss << static_cast<int>(value) << "°C";  // Display integer temperature
        return ss.str();
    }
    catch (const std::exception& e) {
        Logger::Debug("Exception during temperature formatting: " + std::string(e.what()));
        return "Formatting Failed";
    }
    catch (...) {
        Logger::Debug("Exception during temperature formatting - unknown exception");
        return "Formatting Failed";
    }
}

std::string FormatSize(uint64_t bytes, bool useBinary = true) {
    try {
        const double kb = useBinary ? 1024.0 : 1000.0;
        const double mb = kb * kb;
        const double gb = mb * kb;
        const double tb = gb * kb;

        // Parameter validation - check if at maximum value (usually indicates error)
        if (bytes == UINT64_MAX) {
            Logger::Debug("Bytes at maximum value, may indicate error");
            return "N/A";
        }

        std::stringstream ss;
        ss << std::fixed << std::setprecision(1);

        if (bytes >= tb) ss << (bytes / tb) << " TB";
        else if (bytes >= gb) ss << (bytes / gb) << " GB";
        else if (bytes >= mb) ss << (bytes / mb) << " MB";
        else if (bytes >= kb) ss << (bytes / kb) << " KB";
        else ss << bytes << " B";

        return ss.str();
    }
    catch (const std::exception& e) {
        Logger::Debug("Exception during size formatting: " + std::string(e.what()));
        return "Formatting Failed";
    }
    catch (...) {
        Logger::Debug("Exception during size formatting - unknown exception");
        return "Formatting Failed";
    }
}

std::string FormatDiskUsage(uint64_t used, uint64_t total) {
    if (total == 0) return "0%";
    double percentage = (static_cast<double>(used) / total) * 100.0;
    std::stringstream ss;
    ss << std::fixed << std::setprecision(1) << percentage << "%";
    return ss.str();
}

static void PrintSectionHeader(const std::string& title) {
    SafeConsoleOutput("\n=== " + title + " ===\n", 14); // Yellow
}

static void PrintInfoItem(const std::string& label, const std::string& value, int indent = 2) {
    std::string line = std::string(indent, ' ') + label;
    // Format to fixed width
    if (line.length() < 27) {
        line += std::string(27 - line.length(), ' ');
    }
    line += ": " + value + "\n";
    SafeConsoleOutput(line);
}

// GpuInfo::GpuData → SystemInfo::GpuSpec。多卡机器上每一块都要过这一手，
// 所以主卡和其余的卡走的是同一个转换，不会两处写法漂移。
static GpuSpec MakeGpuSpec(const GpuInfo::GpuData& gpu) {
    GpuSpec spec;
    spec.name = WinUtils::WstringToString(gpu.name);
    spec.brand = GetGpuBrand(gpu.name);
    spec.driver = gpu.driver;
    spec.memory = gpu.dedicatedMemory;
    spec.memoryPercent = 0.0;   // 只有主卡能拿到 NVML 的 VRAM 占用，写入端再补
    spec.coreClock = gpu.coreClock;
    spec.temperature = static_cast<double>(gpu.temperature);
    spec.isVirtual = gpu.isVirtual;
    spec.isUnified = gpu.isUnified;
    spec.usage = gpu.usage;
    spec.nvmlIndex = gpu.nvmlIndex;
    spec.coreCount = gpu.coreCount;
    spec.maxClock = gpu.maxCoreClock;
    spec.memClock = gpu.memClock;
    spec.powerMw = static_cast<double>(gpu.powerMw);
    spec.powerLimitMw = static_cast<double>(gpu.powerLimitMw);
    spec.fanPercent = gpu.fanPercent;
    spec.serial = gpu.serial;
    spec.vbios = gpu.vbios;
    spec.cudaVersion = gpu.cudaVersion;
    spec.pcieGen = gpu.pcieGen;
    spec.pcieWidth = gpu.pcieWidth;
    spec.maxPcieGen = gpu.maxPcieGen;
    spec.maxPcieWidth = gpu.maxPcieWidth;
    return spec;
}

// Thread-safe GPU info cache class
class ThreadSafeGpuCache {
private:
    mutable std::mutex mtx_;
    bool initialized_ = false;
    std::string cachedGpuName_ = "No GPU detected";
    std::string cachedGpuBrand_ = "Unknown";
    uint64_t cachedGpuMemory_ = 0;
    uint32_t cachedGpuCoreFreq_ = 0;
    bool cachedGpuIsVirtual_ = false;
    double cachedGpuUsage_ = 0.0;
    // 识别到的全部 GPU，第 0 项为主卡。上面那组单值字段只装得下主卡，
    // 多卡机器上其余的卡以前没有任何出口 —— 现在整份进 IPC 的 gpu/json。
    std::vector<GpuSpec> cachedGpus_;

public:
    void Initialize(WmiManager& wmiManager) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (initialized_) return;
        
        try {
            Logger::Info("Initializing GPU info");
            
            GpuInfo gpuInfo(wmiManager);
            const auto& gpus = gpuInfo.GetGpuData();

            // GpuInfo::DetectGpusViaWmi() already logs each GPU — no need to duplicate here
            Logger::Debug("GPU cache: found " + std::to_string(gpus.size()) + " GPU(s) from WMI");
            
            const GpuInfo::GpuData* selectedGpu = nullptr;
            for (const auto& gpu : gpus) {
                if (!gpu.isVirtual) {
                    selectedGpu = &gpu;
                    break;
                }
            }
            
            if (!selectedGpu && !gpus.empty()) {
                selectedGpu = &gpus[0];
            }
            
            if (selectedGpu) {
                cachedGpuName_ = WinUtils::WstringToString(selectedGpu->name);
                cachedGpuBrand_ = GetGpuBrand(selectedGpu->name);
                cachedGpuMemory_ = selectedGpu->dedicatedMemory;
                cachedGpuCoreFreq_ = static_cast<uint32_t>(selectedGpu->coreClock);
                cachedGpuIsVirtual_ = selectedGpu->isVirtual;
                cachedGpuUsage_ = selectedGpu->usage;  // Save GPU usage

                Logger::Info("Selected primary GPU: " + cachedGpuName_ +
                           " (Virtual: " + (cachedGpuIsVirtual_ ? "Yes" : "No") + ")");

                // 主卡排第 0，其余按原顺序跟上 —— 与 schema 里 gpu/0/* 是主卡的约定一致。
                cachedGpus_.clear();
                cachedGpus_.push_back(MakeGpuSpec(*selectedGpu));
                for (const auto& gpu : gpus) {
                    if (&gpu == selectedGpu) continue;
                    cachedGpus_.push_back(MakeGpuSpec(gpu));
                }
                if (cachedGpus_.size() > 1) {
                    Logger::Info("GPU cache: carrying " + std::to_string(cachedGpus_.size()) +
                                 " GPUs (primary first)");
                }
            } else {
                Logger::Warn("No GPU detected");
            }
            
            initialized_ = true;
            // Logger::Info("GPU info initialization complete, subsequent loops will use cached info");
        }
        catch (const std::exception& e) {
            Logger::Error("GPU info initialization failed: " + std::string(e.what()));
            initialized_ = true;
        }
    }
    
    void GetCachedInfo(std::string& name, std::string& brand, uint64_t& memory, 
                       uint32_t& coreFreq, bool& isVirtual, double& usage) const {
        std::lock_guard<std::mutex> lock(mtx_);
        name = cachedGpuName_;
        brand = cachedGpuBrand_;
        memory = cachedGpuMemory_;
        coreFreq = cachedGpuCoreFreq_;
        isVirtual = cachedGpuIsVirtual_;
        usage = cachedGpuUsage_;
    }

    // 整份 GPU 列表（第 0 项为主卡）。返回副本：加锁期间把引用交出去没有意义。
    std::vector<GpuSpec> GetCachedGpus() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return cachedGpus_;
    }
    
    bool IsInitialized() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return initialized_;
    }
};

// ======================== MCP Helpers ========================

// Build schema describing IPCDataBlock fields (for IPCServer + ConnectDirect)
static void BuildWindowsIpcSchema(tcmt::ipc::SchemaHeader& schemaHdr,
                                   std::vector<tcmt::ipc::FieldDef>& fields) {
    schemaHdr.totalSize = sizeof(tcmt::ipc::IPCDataBlock);
    auto addField = [&](const char* name, uint32_t offset, uint16_t size,
                        uint8_t type = (uint8_t)tcmt::ipc::FieldType::Float32,
                        uint32_t count = 0) {
        tcmt::ipc::FieldDef f{};
        f.offset = offset; f.size = size; f.type = type; f.count = count;
        std::strncpy(f.name, name, tcmt::ipc::IPC_FIELD_NAME_LEN - 1);
        fields.push_back(f);
    };
    using FT = tcmt::ipc::FieldType;
    using B = tcmt::ipc::IPCDataBlock;

    // CPU
    addField("cpu/name", offsetof(B, cpuName), sizeof(B::cpuName), (uint8_t)FT::String);
    addField("cpu/cores/physical", offsetof(B, physicalCores), 1, (uint8_t)FT::UInt8);
    addField("cpu/cores/logical", offsetof(B, logicalCores), 1, (uint8_t)FT::UInt8);
    addField("cpu/usage", offsetof(B, cpuUsage), 4);
    addField("cpu/cores/performance", offsetof(B, performanceCores), 1, (uint8_t)FT::UInt8);
    addField("cpu/cores/efficiency", offsetof(B, efficiencyCores), 1, (uint8_t)FT::UInt8);
    addField("cpu/freq/pCore", offsetof(B, pCoreFreq), 4);
    addField("cpu/freq/eCore", offsetof(B, eCoreFreq), 4);
    addField("cpu/freq/base", offsetof(B, cpuBaseFreq), 4);
    addField("cpu/hyperThreading", offsetof(B, hyperThreading), 1, (uint8_t)FT::Bool);
    addField("cpu/virtualization", offsetof(B, virtualization), 1, (uint8_t)FT::Bool);
    addField("cpu/temperature", offsetof(B, cpuTemp), 4);
    addField("cpu/pcore/temperature", offsetof(B, cpuPcoreTemp), 4);
    addField("cpu/ecore/temperature", offsetof(B, cpuEcoreTemp), 4);
    addField("cpu/sampleIntervalMs", offsetof(B, cpuSampleIntervalMs), 4);

    // Memory
    addField("memory/total", offsetof(B, totalMemory), 8, (uint8_t)FT::UInt64);
    addField("memory/used", offsetof(B, usedMemory), 8, (uint8_t)FT::UInt64);
    addField("memory/available", offsetof(B, availableMemory), 8, (uint8_t)FT::UInt64);
    addField("memory/compressed", offsetof(B, compressedMemory), 8, (uint8_t)FT::UInt64);
    addField("memory/swapUsed", offsetof(B, swapUsed), 8, (uint8_t)FT::UInt64);
    addField("memory/swapTotal", offsetof(B, swapTotal), 8, (uint8_t)FT::UInt64);
    addField("memory/ramSpeed", offsetof(B, ramSpeed), 4, (uint8_t)FT::UInt32);
    addField("memory/ramType", offsetof(B, ramType), sizeof(B::ramType), (uint8_t)FT::String);

    // Battery/Power
    addField("battery/percent", offsetof(B, batteryPercent), 4, (uint8_t)FT::Int32);
    addField("battery/acOnline", offsetof(B, acOnline), 1, (uint8_t)FT::Bool);
    addField("power/cpu", offsetof(B, cpuPower), 4);
    addField("power/gpu", offsetof(B, gpuPower), 4);
    addField("power/ane", offsetof(B, anePower), 4);

    // OS
    addField("os/version", offsetof(B, osVersion), sizeof(B::osVersion), (uint8_t)FT::String);
    addField("os/model", offsetof(B, hardwareModel), sizeof(B::hardwareModel), (uint8_t)FT::String);

    // GPU (single slot, mapped as gpu/0/ for C# compat)
    addField("gpu/0/name", offsetof(B, gpuName), sizeof(B::gpuName), (uint8_t)FT::String);
    addField("gpu/0/brand", offsetof(B, gpuBrand), sizeof(B::gpuBrand), (uint8_t)FT::String);
    addField("gpu/0/memory", offsetof(B, gpuMemory), 8, (uint8_t)FT::UInt64);
    addField("gpu/0/usage", offsetof(B, gpuUsage), 4);
    addField("gpu/0/memoryPercent", offsetof(B, gpuMemoryPercent), 4);
    addField("gpu/0/isVirtual", offsetof(B, gpuIsVirtual), 1, (uint8_t)FT::Bool);
    // Windows 上是独立显存，永远 false；这一项在 macOS 上才有意义（统一内存）
    addField("gpu/0/unified", offsetof(B, gpuUnified), 1, (uint8_t)FT::Bool);
    addField("gpu/0/temperature", offsetof(B, gpuTemp), 4);
    addField("gpu/freq", offsetof(B, gpuFreq), 4);
    // 整份 GPU 列表：gpu/0/* 只有主卡，多卡机器上其余的卡走这里
    addField("gpu/json", offsetof(B, gpuJson), sizeof(B::gpuJson), (uint8_t)FT::String);

    // Network adapters (up to 4)
    for (int i = 0; i < 4; i++) {
        char pfx[32]; snprintf(pfx, sizeof(pfx), "net/%d/", i);
        uint32_t base = offsetof(B, adapters) + i * sizeof(B::NetSlot);
        addField((std::string(pfx)+"name").c_str(), base + offsetof(B::NetSlot, name), sizeof(B::NetSlot::name), (uint8_t)FT::String);
        addField((std::string(pfx)+"macCurrent").c_str(), base + offsetof(B::NetSlot, macCurrent), sizeof(B::NetSlot::macCurrent), (uint8_t)FT::String);
        addField((std::string(pfx)+"ip").c_str(),   base + offsetof(B::NetSlot, ip), sizeof(B::NetSlot::ip), (uint8_t)FT::String);
        addField((std::string(pfx)+"mac").c_str(),  base + offsetof(B::NetSlot, mac), sizeof(B::NetSlot::mac), (uint8_t)FT::String);
        addField((std::string(pfx)+"type").c_str(), base + offsetof(B::NetSlot, type), sizeof(B::NetSlot::type), (uint8_t)FT::String);
        addField((std::string(pfx)+"speed").c_str(),  base + offsetof(B::NetSlot, speed), 8, (uint8_t)FT::UInt64);
        addField((std::string(pfx)+"downloadSpeed").c_str(), base + offsetof(B::NetSlot, downloadSpeed), 8, (uint8_t)FT::UInt64);
        addField((std::string(pfx)+"uploadSpeed").c_str(),   base + offsetof(B::NetSlot, uploadSpeed), 8, (uint8_t)FT::UInt64);
    }

    // Disks (up to 4)
    for (int i = 0; i < 4; i++) {
        char pfx[32]; snprintf(pfx, sizeof(pfx), "disk/%d/", i);
        uint32_t base = offsetof(B, disks) + i * sizeof(B::DiskSlot);
        addField((std::string(pfx)+"letter").c_str(), base + offsetof(B::DiskSlot, letter), 1, (uint8_t)FT::UInt8);
        addField((std::string(pfx)+"mountPoint").c_str(), base + offsetof(B::DiskSlot, mountPoint), sizeof(B::DiskSlot::mountPoint), (uint8_t)FT::String);
        addField((std::string(pfx)+"label").c_str(), base + offsetof(B::DiskSlot, label), sizeof(B::DiskSlot::label), (uint8_t)FT::String);
        addField((std::string(pfx)+"total").c_str(), base + offsetof(B::DiskSlot, totalSize), 8, (uint8_t)FT::UInt64);
        addField((std::string(pfx)+"used").c_str(),  base + offsetof(B::DiskSlot, usedSpace), 8, (uint8_t)FT::UInt64);
        addField((std::string(pfx)+"free").c_str(),  base + offsetof(B::DiskSlot, freeSpace), 8, (uint8_t)FT::UInt64);
        addField((std::string(pfx)+"fs").c_str(),    base + offsetof(B::DiskSlot, fs), sizeof(B::DiskSlot::fs), (uint8_t)FT::String);
    }

    // 温度传感器：一个字段装完整份，形状见 IPCData.h 的 sensorsJson
    addField("sensors/json", offsetof(B, sensorsJson), sizeof(B::sensorsJson), (uint8_t)FT::String);

    // Physical disks (SMART) (up to 8)
    for (int i = 0; i < 8; i++) {
        char pfx[32]; snprintf(pfx, sizeof(pfx), "phys/%d/", i);
        uint32_t base = offsetof(B, physicalDisks) + i * sizeof(B::PhysDiskSlot);
        addField((std::string(pfx)+"model").c_str(),       base + offsetof(B::PhysDiskSlot, model), sizeof(B::PhysDiskSlot::model), (uint8_t)FT::String);
        addField((std::string(pfx)+"serial").c_str(),      base + offsetof(B::PhysDiskSlot, serial), sizeof(B::PhysDiskSlot::serial), (uint8_t)FT::String);
        addField((std::string(pfx)+"capacity").c_str(),    base + offsetof(B::PhysDiskSlot, capacity), 8, (uint8_t)FT::UInt64);
        addField((std::string(pfx)+"interface").c_str(),   base + offsetof(B::PhysDiskSlot, interfaceType), sizeof(B::PhysDiskSlot::interfaceType), (uint8_t)FT::String);
        addField((std::string(pfx)+"temperature").c_str(), base + offsetof(B::PhysDiskSlot, temperature), 4);
        addField((std::string(pfx)+"health").c_str(),      base + offsetof(B::PhysDiskSlot, healthPercent), 4);
        addField((std::string(pfx)+"smartSupported").c_str(), base + offsetof(B::PhysDiskSlot, smartSupported), 1, (uint8_t)FT::Bool);
        addField((std::string(pfx)+"attrCount").c_str(),    base + offsetof(B::PhysDiskSlot, attrCount), 4, (uint8_t)FT::Int32);
        addField((std::string(pfx)+"attrsJson").c_str(),     base + offsetof(B::PhysDiskSlot, attrsJson), sizeof(B::PhysDiskSlot::attrsJson), (uint8_t)FT::String);
        for (int j = 0; j < 8; j++) {
            char fn[64]; snprintf(fn, sizeof(fn), "%sletter%d", pfx, j);
            addField(fn, base + offsetof(B::PhysDiskSlot, logicalDriveLetters) + j, 1, (uint8_t)FT::UInt8);
        }
        addField((std::string(pfx)+"letterCount").c_str(), base + offsetof(B::PhysDiskSlot, logicalDriveCount), 4, (uint8_t)FT::Int32);
    }

    // WiFi
    addField("wifi/ssid",     offsetof(B, wifi.ssid), sizeof(B::WifiSlot::ssid), (uint8_t)FT::String);
    addField("wifi/rssi",     offsetof(B, wifi.rssi), 4, (uint8_t)FT::Int32);
    addField("wifi/channel",  offsetof(B, wifi.channel), 4, (uint8_t)FT::Int32);
    addField("wifi/security", offsetof(B, wifi.security), sizeof(B::WifiSlot::security), (uint8_t)FT::String);
    addField("wifi/band",     offsetof(B, wifi.band), sizeof(B::WifiSlot::band), (uint8_t)FT::String);
    addField("wifi/gen",      offsetof(B, wifi.wifiGen), sizeof(B::WifiSlot::wifiGen), (uint8_t)FT::String);
    addField("wifi/powerOn",  offsetof(B, wifi.powerOn), 1, (uint8_t)FT::Bool);
    addField("wifi/isConnected", offsetof(B, wifi.isConnected), 1, (uint8_t)FT::Bool);

    // Bluetooth
    addField("bluetooth/powerOn",     offsetof(B, bluetooth.powerOn), 1, (uint8_t)FT::Bool);
    addField("bluetooth/deviceCount", offsetof(B, bluetooth.deviceCount), 4, (uint8_t)FT::Int32);
    addField("bluetooth/name",        offsetof(B, bluetooth.name), sizeof(B::BtSlot::name), (uint8_t)FT::String);

    // TPM
    addField("tpm/manufacturer",    offsetof(B, tpm.manufacturer), sizeof(B::TpmSlot::manufacturer), (uint8_t)FT::String);
    addField("tpm/firmwareVersion", offsetof(B, tpm.firmwareVersion), sizeof(B::TpmSlot::firmwareVersion), (uint8_t)FT::String);
    addField("tpm/isPresent",       offsetof(B, tpm.isPresent), 1, (uint8_t)FT::Bool);
    addField("tpm/isEnabled",       offsetof(B, tpm.isEnabled), 1, (uint8_t)FT::Bool);
    addField("tpm/isActive",        offsetof(B, tpm.isActive), 1, (uint8_t)FT::Bool);
    addField("tpm/selfTestStatus",  offsetof(B, tpm.selfTestStatus), 1, (uint8_t)FT::UInt8);
    addField("tpm/status",          offsetof(B, tpm.status), 1, (uint8_t)FT::UInt8);
    addField("tpm/count",           offsetof(B, tpmCount), 1, (uint8_t)FT::UInt8);

    // App version
    addField("app/version", offsetof(B, appVersion), sizeof(B::appVersion), (uint8_t)FT::String);

    // ─── 4. Fan speeds (up to 6) ───
    for (int i = 0; i < 6; i++) {
        char pfx[32]; snprintf(pfx, sizeof(pfx), "fan/%d/", i);
        uint32_t base = offsetof(B, fanSpeeds) + i * sizeof(B::FanSlot);
        addField((std::string(pfx)+"name").c_str(), base + offsetof(B::FanSlot, name), sizeof(B::FanSlot::name), (uint8_t)FT::String);
        addField((std::string(pfx)+"rpm").c_str(),  base + offsetof(B::FanSlot, rpm), sizeof(float), (uint8_t)FT::Float32);
    }
    addField("fan/count", offsetof(B, fanCount), 1, (uint8_t)FT::UInt8);

    // ─── 5. 进程列表：一个字段装完整份，形状见 IPCData.h 的 processesJson ───
    addField("proc/json", offsetof(B, processesJson), sizeof(B::processesJson), (uint8_t)FT::String);

    // ─── 7. Battery detail ───
    addField("battery/cycleCount",     offsetof(B, batteryCycleCount), sizeof(int32_t), (uint8_t)FT::Int32);
    addField("battery/designCapacity", offsetof(B, batteryDesignCapacity), sizeof(int32_t), (uint8_t)FT::Int32);
    addField("battery/maxCapacity",    offsetof(B, batteryMaxCapacity), sizeof(int32_t), (uint8_t)FT::Int32);
    addField("battery/healthPercent",  offsetof(B, batteryHealthPercent), sizeof(float), (uint8_t)FT::Float32);
    addField("battery/temperature",    offsetof(B, batteryTemp), sizeof(float), (uint8_t)FT::Float32);
    addField("battery/amperage",       offsetof(B, batteryAmperage), sizeof(int32_t), (uint8_t)FT::Int32);
    addField("battery/voltage",        offsetof(B, batteryVoltage), sizeof(int32_t), (uint8_t)FT::Int32);
    addField("battery/chargerWatts",   offsetof(B, batteryChargerWatts), sizeof(float), (uint8_t)FT::Float32);
    addField("battery/isCharging",     offsetof(B, batteryIsCharging), 1, (uint8_t)FT::Bool);
    addField("battery/isPresent",      offsetof(B, batteryIsPresent), 1, (uint8_t)FT::Bool);

    // ─── 6. 逐核传感器：一个字段装完整份，形状见 IPCData.h 的 cpuCoresJson ───
    addField("cpu/cores/json", offsetof(B, cpuCoresJson), sizeof(B::cpuCoresJson), (uint8_t)FT::String);

    // ─── System info ───
    addField("system/loadAvg1",    offsetof(B, loadAvg1), sizeof(float), (uint8_t)FT::Float32);
    addField("system/loadAvg5",    offsetof(B, loadAvg5), sizeof(float), (uint8_t)FT::Float32);
    addField("system/loadAvg15",   offsetof(B, loadAvg15), sizeof(float), (uint8_t)FT::Float32);
    addField("system/processCount", offsetof(B, processCount), sizeof(int32_t), (uint8_t)FT::Int32);
    addField("system/uptime",      offsetof(B, uptimeSeconds), sizeof(uint64_t), (uint8_t)FT::UInt64);
}

// ======================== Mode Handlers ========================

// --json: one-shot JSON snapshot to stdout (for scripting / CI)
static int RunJsonMode() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr)) {
        Logger::Error("COM init failed for --json mode");
        return 1;
    }

    auto wmiManager = std::make_unique<WmiManager>();
    if (!wmiManager || !wmiManager->IsInitialized()) {
        Logger::Error("WMI init failed for --json mode");
        CoUninitialize();
        return 1;
    }

    char tmpBuf[MAX_PATH];
    GetTempPathA(MAX_PATH, tmpBuf);
    std::string tmpPath = std::string(tmpBuf) + "tcmt_export.json";
    std::remove(tmpPath.c_str());
    ConfigManager cfg(tmpPath);
    cfg.Load();

    try { OSInfo os; cfg.SetString("os.version", os.GetVersion()); } catch (...) {}
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
        GpuInfo gpuInfo(*wmiManager);
        const auto& gpus = gpuInfo.GetGpuData();
        if (!gpus.empty()) {
            cfg.SetString("gpu.name", WinUtils::WstringToString(gpus[0].name));
            cfg.SetUint64("gpu.dedicatedMemory", gpus[0].dedicatedMemory);
            cfg.SetDouble("gpu.usage", gpus[0].usage);
        }
    } catch (...) {}
    try {
        NetworkAdapter netAdapter(*wmiManager);
        const auto& adapters = netAdapter.GetAdapters();
        for (const auto& a : adapters) {
            nlohmann::json na;
            na["name"] = a.name;
            na["ip"] = a.ip;
            na["mac"] = a.mac;
            na["type"] = a.adapterType;
            na["speed"] = a.speed;
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
        for (const auto& t : temps) tempObj[t.first] = t.second;
        cfg.SetJson("temperatures", std::move(tempObj));
    } catch (...) {}

    if (cfg.Save()) {
        std::ifstream in(tmpPath);
        if (in) std::cout << in.rdbuf();
    }
    std::cout << std::endl;
    std::remove(tmpPath.c_str());
    TemperatureWrapper::Cleanup();
    // Release WMI COM interfaces BEFORE CoUninitialize — otherwise the
    // WmiManager destructor (on scope exit) releases pointers after the
    // COM apartment is torn down → access violation at process exit.
    wmiManager.reset();
    CoUninitialize();
    return 0;
}

// --mcp: JSON-RPC 2.0 MCP server over stdio (for AI agent integration)
static int RunMcpMode() {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    tcmt::mcp::MCPServer server;

    tcmt::ipc::IPCClient ipc;
    bool useIpc = ipc.Connect();
    if (useIpc) {
        Logger::Info("MCP: connected to running TCMT via IPC");
        ipc.ClosePipe();
    } else {
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
            j["temperature"] = 0.0;
        }
        return j;
    });
    server.RegisterTool("get_memory", "System memory statistics",
    [&ipc, useIpc]() -> nlohmann::json {
        nlohmann::json j;
        if (useIpc) {
            j["total"] = ipc.ReadUInt64("memory/total").value_or(0);
            j["used"]  = ipc.ReadUInt64("memory/used").value_or(0);
            j["available"] = ipc.ReadUInt64("memory/available").value_or(0);
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
        }
        return j;
    });
    server.RegisterTool("get_system_info", "OS version and hardware summary",
    [&ipc, useIpc]() -> nlohmann::json {
        nlohmann::json j;
        if (useIpc) {
            j["os"] = ipc.ReadString("os/version").value_or("");
            j["cpu"] = ipc.ReadString("cpu/name").value_or("");
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

// ======================== Startup Helpers ========================

// Initialize COM for the monitoring process (retries single-threaded mode
// when the apartment model conflicts with an earlier initializer).
static bool InitCom() {
    try {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr)) {
            if (hr == RPC_E_CHANGED_MODE) {
                Logger::Warn("COM initialization mode conflict: thread already initialized in different mode, trying single-threaded mode");
                hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                if (FAILED(hr)) {
                    Logger::Error("COM initialization failed: 0x" + std::to_string(hr));
                    return false;
                }
            }
            else {
                Logger::Error("COM initialization failed: 0x" + std::to_string(hr));
                return false;
            }
        }
        g_comInitialized = true;
        Logger::Debug("COM initialized successfully");
        return true;
    }
    catch (const std::exception& e) {
        Logger::Error("Exception during COM initialization: " + std::string(e.what()));
        return false;
    }
}

// Create the shared memory block and start the IPC server (schema broadcast
// to C# Avalonia via Named Pipe on Windows, UDS on macOS).
static bool InitSharedMemoryAndIpc(std::unique_ptr<tcmt::ipc::IPCServer>& ipcServer) {
    try {
        if (!SharedMemoryManager::InitSharedMemory()) {
            std::string error = SharedMemoryManager::GetLastError();
            Logger::Error("Shared memory initialization failed: " + error);

            Logger::Info("Attempting to reinitialize shared memory...");
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));

            if (!SharedMemoryManager::InitSharedMemory()) {
                Logger::Critical("Shared memory reinitialization failed, program cannot continue");
                return false;
            }
        }
        Logger::Info("Shared memory initialized successfully");

        ipcServer = std::make_unique<tcmt::ipc::IPCServer>();
        {
            tcmt::ipc::SchemaHeader schemaHdr;
            std::vector<tcmt::ipc::FieldDef> fields;
            BuildWindowsIpcSchema(schemaHdr, fields);
            ipcServer->UpdateSchema(schemaHdr, fields);
        }
        if (ipcServer->Start()) {
            Logger::Info("IPC server started (named pipe)");
        } else {
            Logger::Warn("IPC server failed: " + ipcServer->GetLastError());
        }

        // Avalonia 设置页的 ConfigSet 入口。macOS 的 main_mac.cpp 一直有这条，
        // Windows 以前没接 —— 设置页写出去的 server.* / sampling.intervalMs
        // 全被丢弃，界面还会被回推的快照打回原值。
        ipcServer->SetConfigBridge(
            [] { return BuildConfigSnapshotJson(); },
            [srv = ipcServer.get()](const std::string& payload) {
                ApplyClientConfigSet(payload, *srv);
            });
        return true;
    }
    catch (const std::exception& e) {
        Logger::Error("Exception during shared memory initialization: " + std::string(e.what()));
        return false;
    }
}

// Create the WMI manager; returns nullptr on fatal failure.
static std::shared_ptr<WmiManager> InitWmiManager() {
    std::shared_ptr<WmiManager> wmiManager;
    try {
        wmiManager = std::make_shared<WmiManager>();
        if (!wmiManager) {
            Logger::Fatal("WMI manager object creation failed - memory allocation returned null");
            return nullptr;
        }
        if (!wmiManager->IsInitialized()) {
            Logger::Error("WMI initialization failed");
            MessageBoxA(NULL, "WMI initialization failed, cannot retrieve system information.", "Error", MB_OK | MB_ICONERROR);
            return nullptr;
        }
        Logger::Debug("WMI manager initialized successfully");
    }
    catch (const std::bad_alloc& e) {
        Logger::Fatal("WMI manager creation failed - memory allocation failed: " + std::string(e.what()));
        return nullptr;
    }
    catch (const std::exception& e) {
        Logger::Error("WMI manager creation failed: " + std::string(e.what()));
        return nullptr;
    }
    catch (...) {
        Logger::Fatal("WMI manager creation failed - unknown exception");
        return nullptr;
    }
    return wmiManager;
}

// Initialize temperature monitoring (PawnIO + NVML on Windows, SMC on macOS,
// sysfs on Linux). Failures are non-fatal: the program continues without temps.
static void InitTemperatureBridge() {
    try {
        TemperatureWrapper::Initialize();
        Logger::Info("Temperature monitoring initialized");
    }
    catch (const std::exception& e) {
        Logger::Error("Hardware monitoring bridge initialization failed: " + std::string(e.what()));
        // Do not exit program, continue running but temperature data may not be available
    }
}

// Open the SQLite sensor history database next to %APPDATA%\TCMT (Windows) or
// ~/.tcmt (POSIX). Returns true when the database is ready for writes.
static bool InitHistoryLogger(HistoryLogger& historyLogger) {
    historyLogger.SetRetentionDays(30);
    std::string dbPath;
#ifdef _WIN32
    char envBuf[MAX_PATH];
    DWORD envLen = GetEnvironmentVariableA("APPDATA", envBuf, sizeof(envBuf));
    if (envLen > 0 && envLen < sizeof(envBuf))
        dbPath = std::string(envBuf) + "\\TCMT\\history.db";
    else
        dbPath = std::string(getenv("TEMP") ? getenv("TEMP") : "C:\\TEMP") + "\\tcmt_history.db";
    // Create directory tree
    std::string dir = dbPath.substr(0, dbPath.find_last_of('\\'));
    for (size_t i = 0; i < dir.size(); i++)
        if (dir[i] == '\\' || dir[i] == '/') {
            dir[i] = '\0';
            CreateDirectoryA(dir.c_str(), nullptr);
            dir[i] = '\\';
        }
    CreateDirectoryA(dir.c_str(), nullptr);
#else
    dbPath = getenv("HOME") ? std::string(getenv("HOME")) + "/.tcmt/history.db" : "/tmp/tcmt_history.db";
    {
        std::string dir = dbPath.substr(0, dbPath.find_last_of('/'));
        mkdir(dir.c_str(), 0755);
    }
#endif
    if (historyLogger.Initialize(dbPath)) {
        Logger::Info("HistoryLogger: " + dbPath);
        return true;
    }
    Logger::Warn("HistoryLogger failed to initialize");
    return false;
}

// ======================== Monitoring Main Loop ========================
// Collects CPU/GPU/memory/disk/temperature snapshots once per second,
// publishes them to the TUI and shared memory, and stores sensor history.
// Returns when the global exit flag is set.
static void RunMonitoringLoop(std::shared_ptr<WmiManager>& wmiManager,
                              std::unique_ptr<tcmt::ipc::IPCServer>& ipcServer,
                              tcmt::TuiApp& tuiApp,
                              HistoryLogger& historyLogger) {
    int loopCounter = 1;
    bool isFirstRun = true;
    
    // Cache static system info (only on first fetch)
    static std::atomic<bool> systemInfoCached{false};
    static std::string cachedOsVersion;
    static std::string cachedCpuName;
    static uint32_t cachedPhysicalCores = 0;
    static uint32_t cachedLogicalCores = 0;
    static uint32_t cachedPerformanceCores = 0;
    static uint32_t cachedEfficiencyCores = 0;
    static bool cachedHyperThreading = false;
    static bool cachedVirtualization = false;
    static double cachedBaseFreq = 0.0;
    // WMI: Win32_Processor.MaxClockSpeed (MHz) — read once
    if (cachedBaseFreq == 0.0 && wmiManager && wmiManager->IsInitialized()) {
        IEnumWbemClassObject* pEnumCpu = nullptr;
        HRESULT hr = wmiManager->GetWmiService()->ExecQuery(
            _bstr_t("WQL"), _bstr_t("SELECT MaxClockSpeed FROM Win32_Processor"),
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &pEnumCpu);
        if (SUCCEEDED(hr) && pEnumCpu) {
            IWbemClassObject* pObj = nullptr;
            ULONG ret = 0;
            while (pEnumCpu->Next(WBEM_INFINITE, 1, &pObj, &ret) == S_OK) {
                VARIANT vt; VariantInit(&vt);
                if (SUCCEEDED(pObj->Get(L"MaxClockSpeed", 0, &vt, 0, 0)) && vt.vt == VT_I4)
                    cachedBaseFreq = (double)vt.intVal;
                VariantClear(&vt);
                pObj->Release();
            }
            pEnumCpu->Release();
        }
    }
    
    std::unique_ptr<CpuInfo> cpuInfo;
    try {
        cpuInfo = std::make_unique<CpuInfo>();
        if (!cpuInfo) {
            Logger::Fatal("CPU info object creation failed - memory allocation returned null");
            SafeExit(1);
        }
        Logger::Debug("CPU info object created successfully");
    }
    catch (const std::bad_alloc& e) {
        Logger::Fatal("CPU info object creation failed - memory allocation failed: " + std::string(e.what()));
        SafeExit(1);
    }
    catch (const std::exception& e) {
        Logger::Error("CPU info object creation failed: " + std::string(e.what()));
        SafeExit(1);
    }
    catch (...) {
        Logger::Fatal("CPU info object creation failed - unknown exception");
        SafeExit(1);
    }
    
    ThreadSafeGpuCache gpuCache;

    // Start ModuleCoordinator background collection threads
    ModuleCoordinator coordinator;
    coordinator.Start();
    coordinator.SetSampleIntervalMs(g_sampleIntervalMs.load());

    // 采样间隔的 handler 必须挂在 coordinator 还活着的作用域里。它以前写在
    // main() 的设置块中，而 coordinator 是 RunMonitoringLoop 的局部变量，
    // MSVC 直接报 C2065 / C3481。启动值上面已经喂过一次，这里只处理运行中修改。
    tuiApp.SetSampleIntervalHandler([&coordinator](int ms) {
        if (ms < tcmt::kSampleIntervalMinMs) ms = tcmt::kSampleIntervalMinMs;
        if (ms > tcmt::kSampleIntervalMaxMs) ms = tcmt::kSampleIntervalMaxMs;
        g_sampleIntervalMs.store(ms);
        // 采集循环也按这个周期跑，否则改了设置只有界面刷新变快
        coordinator.SetSampleIntervalMs(ms);
        auto& cfg = GetServerCfg();
        cfg.SetInt("sampling.intervalMs", ms);
        cfg.Save();
        Logger::Info("Sampling interval set to " + std::to_string(ms) + " ms");
    });

    // Exit when Ctrl+C is received OR the TUI itself quit ('q' / Esc): on
    // Windows the TUI runs in-process, so its shutdown must stop the loop.
    int appliedSampleMs = g_sampleIntervalMs.load();
    while (!g_shouldExit.load() && tuiApp.IsRunning()) {
        // TUI 设置页直接调 setter；Avalonia 经 IPC 改的只是全局原子量。
        // 两条路最终都落到 g_sampleIntervalMs，这里发现变化再喂给采集线程。
        {
            const int wantedMs = g_sampleIntervalMs.load();
            if (wantedMs != appliedSampleMs) {
                coordinator.SetSampleIntervalMs(wantedMs);
                appliedSampleMs = wantedMs;
            }
        }

        // Apply server settings saved from the TUI settings page.
        {
            tcmt::ServerSettings pending;
            bool havePending = false;
            {
                std::lock_guard<std::mutex> lock(g_push.mutex);
                if (g_push.pendingReady) {
                    pending = g_push.pending;
                    g_push.pendingReady = false;
                    havePending = true;
                }
            }
            if (havePending) {
                g_push.probe.Stop();
                g_push.url = pending.url;
                g_push.intervalSec = (std::max)(1, (std::min)(pending.intervalSec, 60));
                g_push.intervalMs = (int64_t)g_push.intervalSec * 1000;
                g_push.enabled = pending.enabled;
                g_push.insecure = pending.insecure;
                if (pending.enabled) {
                    g_push.probe.SetInsecure(pending.insecure);
                    g_push.active = g_push.probe.Start(pending.url);
                    g_push.status = g_push.active ? "connecting..." : "error: cannot start";
                    if (g_push.active) Logger::Info("ServerProbe restarted: " + pending.url);
                    else Logger::Warn("ServerProbe restart failed: " + pending.url);
                } else {
                    g_push.active = false;
                    g_push.status = "disabled";
                    Logger::Info("ServerProbe stopped (disabled in settings)");
                }
                // ServerSettings 有六个成员，这里以前只传五个 —— 09-16 给 macOS 那边加了
                // sampleIntervalMs 之后，这个调用点没跟着改，MSVC 直接 C2664 编不过。
                // 采样间隔走 g_sampleIntervalMs（设置页可改），不是默认值
                tuiApp.SetServerSettings(
                    {g_push.enabled, g_push.url, g_push.insecure, g_push.intervalSec,
                     g_sampleIntervalMs.load(), g_push.status});
            }
        }
        static DeviceChangeNotifier s_usbNotify(DeviceChangeNotifier::USB);
        static DeviceChangeNotifier s_hubNotify(DeviceChangeNotifier::USB_Hub);
        static DeviceChangeNotifier s_btNotify(DeviceChangeNotifier::Bluetooth);
        try {
            auto loopStart = std::chrono::high_resolution_clock::now();
            
            bool isDetailedLogging = (loopCounter % 5 == 1);
            
            if (isDetailedLogging) {
                Logger::Debug("Starting main monitoring loop iteration #" + std::to_string(loopCounter));
            }
            
            if (loopCounter == 5) {
                g_monitoringStarted = true;
                Logger::Info("Program is running stable");
            }
            
            SystemInfo sysInfo{};
            // compressedMemory populated by ModuleCoordinator (PDH \Memory\Modified Page List Bytes)

            try {
                sysInfo.cpuUsage = 0.0;
                sysInfo.performanceCoreFreq = 0.0;
                sysInfo.efficiencyCoreFreq = 0.0;
                sysInfo.totalMemory = 0;
                sysInfo.usedMemory = 0;
                sysInfo.availableMemory = 0;
                sysInfo.gpuMemory = 0;
                sysInfo.gpuCoreFreq = 0.0;
                sysInfo.gpuIsVirtual = false;
                sysInfo.networkAdapterSpeed = 0;
                sysInfo.lastUpdate = Platform::SystemTime::Now();
                
                if (sysInfo.lastUpdate.year < 2020 || sysInfo.lastUpdate.year > 2050) {
                    Logger::Warn("Abnormal system time: " + std::to_string(sysInfo.lastUpdate.year));
                }
            }
            catch (const std::exception& e) {
                Logger::Error("SystemInfo initialization failed: " + std::string(e.what()));
                continue;
            }
            catch (...) {
                Logger::Error("SystemInfo initialization failed - unknown exception");
                continue;
            }

            if (!systemInfoCached.load()) {
                try {
                    Logger::Info("Initializing system information");
                    
                    OSInfo os;
                    cachedOsVersion = os.GetVersion();

                    if (cpuInfo) {
                        cachedCpuName = cpuInfo->GetName();
                        cachedPhysicalCores = cpuInfo->GetLargeCores() + cpuInfo->GetSmallCores();
                        cachedLogicalCores = cpuInfo->GetTotalCores();
                        cachedPerformanceCores = cpuInfo->GetLargeCores();
                        cachedEfficiencyCores = cpuInfo->GetSmallCores();
                        cachedHyperThreading = cpuInfo->IsHyperThreadingEnabled();
                        cachedVirtualization = cpuInfo->IsVirtualizationEnabled();
                    }
                    
                    systemInfoCached = true;
                    Logger::Info("System information initialized");
                }
                catch (const std::exception& e) {
                    Logger::Error("System info initialization failed: " + std::string(e.what()));
                    cachedOsVersion = "Unknown";
                    cachedCpuName = "Unknown";
                    systemInfoCached = true;
                }
            }
            
            sysInfo.osVersion = cachedOsVersion;
            sysInfo.cpuName = cachedCpuName;
            sysInfo.physicalCores = cachedPhysicalCores;
            sysInfo.logicalCores = cachedLogicalCores;
            sysInfo.performanceCores = cachedPerformanceCores;
            sysInfo.efficiencyCores = cachedEfficiencyCores;
            sysInfo.hyperThreading = cachedHyperThreading;
            sysInfo.virtualization = cachedVirtualization;
            sysInfo.cpuBaseFreq = cachedBaseFreq;

            // Coordinator snapshot fills CPU, Memory, Temperature, Power, Disk
            {
                tcmt::TuiData tempTui;
                coordinator.Snapshot(sysInfo, tempTui);
            }

            if (!gpuCache.IsInitialized()) {
                try {
                    gpuCache.Initialize(*wmiManager);
                }
                catch (const std::exception& e) {
                    Logger::Error("GPU cache initialization failed: " + std::string(e.what()));
                }
            }
            
            try {
                std::string cachedGpuName, cachedGpuBrand;
                uint64_t cachedGpuMemory;
                uint32_t cachedGpuCoreFreq;
                bool cachedGpuIsVirtual;
                double cachedGpuUsage;
                
                gpuCache.GetCachedInfo(cachedGpuName, cachedGpuBrand, cachedGpuMemory, 
                                      cachedGpuCoreFreq, cachedGpuIsVirtual, cachedGpuUsage);
                
                sysInfo.gpuName = cachedGpuName;
                sysInfo.gpuBrand = cachedGpuBrand;
                sysInfo.gpuMemory = cachedGpuMemory;
                sysInfo.gpuCoreFreq = cachedGpuCoreFreq;
                sysInfo.gpuFreq = cachedGpuCoreFreq;  // NVML real-time clock → TUI
                sysInfo.gpuIsVirtual = cachedGpuIsVirtual;
                sysInfo.gpuUsage = cachedGpuUsage;

                // NVML 实时读数（只有 NVIDIA 有）。按固定 1 秒采一次，不跟着主循环跑：
                // NVML 的 utilization 报的是「上一段采样窗口内的占用」，窗口由驱动维护（约 1 秒）。
                // 主循环最快 100ms 一轮，跟着它读是把窗口切碎，一次短促的 GPU 突发就会被报成
                // 接近满载。固定 1 秒采一次，报出来的才是完整一秒的平均。
                static double nvmlUsageCached = -1.0;
                static double nvmlTempCached  = -1.0;
                static double nvmlVramCached  = -1.0;
                static double nvmlPowerCached = -1.0;
                static std::chrono::steady_clock::time_point nvmlLastSample{};
                {
                    const auto now = std::chrono::steady_clock::now();
                    const bool first = (nvmlLastSample.time_since_epoch().count() == 0);
                    const bool due = first ||
                        std::chrono::duration_cast<std::chrono::milliseconds>(now - nvmlLastSample).count() >= 1000;
                    if (due) {
                        nvmlLastSample = now;
                        const double u = GpuInfo::GetGpuUsage();
                        const double t = GpuInfo::GetGpuTemperature();
                        const double v = GpuInfo::GetVramUsagePercent();
                        const double p = GpuInfo::GetGpuPower();
                        if (u >= 0) nvmlUsageCached = u;
                        if (t >= 0) nvmlTempCached  = t;
                        if (v >= 0) nvmlVramCached  = v;
                        if (p >= 0) nvmlPowerCached = p;
                    }
                }
                double nvmlUsage  = nvmlUsageCached;
                double nvmlTemp   = nvmlTempCached;
                double nvmlVramPct = nvmlVramCached;
                if (nvmlUsage  >= 0) sysInfo.gpuUsage = nvmlUsage;
                if (nvmlTemp   >= 0) sysInfo.gpuTemperature = nvmlTemp;
                // 显卡功耗（毫瓦）。Windows 上只有 N 卡拿得到 —— 用户态没有别的功耗出口，
                // A 卡/核显留 0，不估。
                if (nvmlPowerCached >= 0) sysInfo.gpuPower = nvmlPowerCached;
                if (nvmlVramPct >= 0) {
                    sysInfo.gpuCoreFreq = nvmlVramPct;       // 老字段被借去装 VRAM%，见下
                    // 显存占用要写进 gpuMemoryPercent —— 以前只写到了 tuiData 上，
                    // sysInfo 这一份从来没人填，IPC 出去的 gpu/0/memoryPercent 恒 0，
                    // 界面上就是一条永远 0.0% 的假读数。
                    sysInfo.gpuMemoryPercent = nvmlVramPct;
                }

                // GPU 列表：整份从缓存搬过来（第 0 项为主卡），再把主卡的实时值刷上去。
                sysInfo.gpus = gpuCache.GetCachedGpus();
                if (!sysInfo.gpus.empty()) {
                    // 主卡时钟只在合理区间里采纳，异常值退回 0 —— 见过 10000+ 的读数
                    if (!(cachedGpuCoreFreq > 0 && cachedGpuCoreFreq < 10000)) {
                        if (isFirstRun && cachedGpuCoreFreq > 10000) {
                            Logger::Warn("GPU core clock abnormal: " + std::to_string(cachedGpuCoreFreq) + "MHz, reset to 0");
                        }
                        sysInfo.gpus[0].coreClock = 0;
                    }
                    // 实时占用/温度/显存百分比只有主卡拿得到（NVML 只开了一块卡的句柄）
                    sysInfo.gpus[0].usage = sysInfo.gpuUsage;
                    sysInfo.gpus[0].temperature = sysInfo.gpuTemperature;
                    if (nvmlVramPct >= 0) sysInfo.gpus[0].memoryPercent = nvmlVramPct;
                    sysInfo.gpus[0].isVirtual = sysInfo.gpuIsVirtual;

                    // Per-GPU live readings, applied after the primary-card
                    // assignments above so each NVIDIA card shows its own
                    // numbers. The cache is the startup snapshot; without this
                    // refresh every card kept reporting its first-second value.
                    static std::chrono::steady_clock::time_point nvmlLiveAt{};
                    static std::vector<GpuInfo::NvidiaLive> nvmlLive;
                    const auto liveNow = std::chrono::steady_clock::now();
                    const bool liveDue = nvmlLiveAt.time_since_epoch().count() == 0 ||
                        std::chrono::duration_cast<std::chrono::milliseconds>(liveNow - nvmlLiveAt).count() >= 1000;
                    if (liveDue) {
                        nvmlLiveAt = liveNow;
                        nvmlLive.clear();
                        nvmlLive.reserve(sysInfo.gpus.size());
                        for (const auto& g : sysInfo.gpus) {
                            nvmlLive.push_back(g.nvmlIndex >= 0
                                ? GpuInfo::GetNvidiaLive(g.nvmlIndex)
                                : GpuInfo::NvidiaLive{});
                        }
                    }
                    for (size_t gi = 0; gi < sysInfo.gpus.size() && gi < nvmlLive.size(); ++gi) {
                        const auto& live = nvmlLive[gi];
                        if (!live.valid) continue;
                        sysInfo.gpus[gi].usage = live.usage;
                        sysInfo.gpus[gi].temperature = live.temperature;
                        sysInfo.gpus[gi].memoryPercent = live.memoryPercent;
                        sysInfo.gpus[gi].coreClock = live.clockMhz;
                        sysInfo.gpus[gi].memClock = static_cast<int>(live.memClockMhz);
                        if (live.powerMw >= 0) sysInfo.gpus[gi].powerMw = live.powerMw;
                        // The PCIe link re-negotiates as the card idles and
                        // wakes, so the width has to be sampled live too.
                        if (live.pcieGen > 0) sysInfo.gpus[gi].pcieGen = live.pcieGen;
                        if (live.pcieWidth > 0) sysInfo.gpus[gi].pcieWidth = live.pcieWidth;
                    }

                    if (isFirstRun) {
                        Logger::Debug("Added " + std::to_string(sysInfo.gpus.size()) +
                                     " GPU(s), primary: " + cachedGpuName +
                                     " (Memory: " + FormatSize(cachedGpuMemory) + ")");
                    }
                } else {
                    if (isFirstRun) {
                        Logger::Debug("No valid GPU detected, skipping GPU data population");
                    }
                }
            }
            catch (const std::bad_alloc& e) {
                Logger::Error("GPU cache info processing failed - Out of memory: " + std::string(e.what()));
                sysInfo.gpus.clear();
                sysInfo.gpuName = "Out of memory";
                sysInfo.gpuBrand = "Unknown";
                sysInfo.gpuMemory = 0;
                sysInfo.gpuCoreFreq = 0;
                sysInfo.gpuIsVirtual = false;
            }
            catch (const std::exception& e) {
                Logger::Error("Failed to get GPU cache info: " + std::string(e.what()));
                sysInfo.gpus.clear();
                sysInfo.gpuName = "Failed to get GPU info";
                sysInfo.gpuBrand = "Unknown";
                sysInfo.gpuMemory = 0;
                sysInfo.gpuCoreFreq = 0;
                sysInfo.gpuIsVirtual = false;
            }
            catch (...) {
                Logger::Error("Failed to get GPU cache info - Unknown exception");
                sysInfo.gpus.clear();
                sysInfo.gpuName = "Unknown exception";
                sysInfo.gpuBrand = "Unknown";
                sysInfo.gpuMemory = 0;
                sysInfo.gpuCoreFreq = 0;
                sysInfo.gpuIsVirtual = false;
            }

            // Initialize network adapter info (avoid crash due to invalid data)
            sysInfo.networkAdapterName = "No network adapter detected";
            sysInfo.networkAdapterMac = "00-00-00-00-00-00";
            sysInfo.networkAdapterSpeed = 0;
            sysInfo.networkAdapterIp = "N/A";
            sysInfo.networkAdapterType = "Unknown";

            // Network adapter info now populated by ModuleCoordinator::Snapshot()
            // (was duplicated here — removed to avoid redundant WMI queries)

            // Temperature data handled by coordinator (TemperatureLoop via TemperatureWrapper)

            // Physical disk (SMART) and TPM collection (coordinator handles logical disks)
            try {
                // Collect physical disks on a background thread. DeviceIoControl SMART
                // reads can block indefinitely on unresponsive drives/controllers (no
                // timeout), so the main loop must never wait on them. Results are cached.
                // The cache is intentionally heap-allocated for the process lifetime so a
                // detached worker can touch it safely even while the program is exiting.
                static auto* physDiskMutex = new std::mutex();
                static auto* cachedPhysDisks = new std::vector<PhysicalDiskSmartData>();
                static std::atomic<bool> physScanInFlight{false};
                static auto lastPhysScanStart = std::chrono::steady_clock::now() - std::chrono::seconds(61);
                auto now = std::chrono::steady_clock::now();
                // A scan that has been running too long is considered stuck (hung drive) —
                // clear the flag so the next refresh window can retry.
                if (physScanInFlight.load() &&
                    std::chrono::duration_cast<std::chrono::seconds>(now - lastPhysScanStart).count() > 120) {
                    physScanInFlight.store(false);
                }
                if (std::chrono::duration_cast<std::chrono::seconds>(now - lastPhysScanStart).count() >= 60 &&
                    wmiManager && !physScanInFlight.exchange(true)) {
                    lastPhysScanStart = now; // throttle only when a scan actually starts
                    auto wmi = wmiManager;   // shared_ptr keeps WmiManager alive for the worker
                    std::thread([wmi]() {
                        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                        bool comInit = SUCCEEDED(hr);
                        SystemInfo tmp;
                        try {
                            // Phase 1: fast single-query WMI disk list — publish immediately
                            // so the TUI shows physical disks even if a SMART read hangs.
                            DiskInfo::CollectPhysicalDiskInfo(*wmi, tmp);
                            if (!tmp.physicalDisks.empty()) {
                                {
                                    std::lock_guard<std::mutex> lock(*physDiskMutex);
                                    *cachedPhysDisks = tmp.physicalDisks;
                                }
                                // Phase 2: SMART per disk on its own thread — a hung drive
                                // never blocks the list or the other disks.
                                auto disks = tmp.physicalDisks;
                                for (const auto& d : disks) {
                                    if (d.physicalIndex < 0) continue;
                                    std::thread([d]() {
                                        PhysicalDiskSmartData out = d;
                                        bool ok = false;
                                        try {
                                            ok = SmartReader::Read(out.physicalIndex, out);
                                        } catch (...) {}
                                        if (ok) {
                                            std::lock_guard<std::mutex> lock(*physDiskMutex);
                                            for (auto& e : *cachedPhysDisks) {
                                                if (e.physicalIndex == out.physicalIndex) {
                                                    e = out;
                                                    break;
                                                }
                                            }
                                        }
                                    }).detach();
                                }
                            }
                        } catch (...) {}
                        physScanInFlight.store(false);
                        if (comInit) CoUninitialize();
                    }).detach();
                }
                {
                    std::lock_guard<std::mutex> lock(*physDiskMutex);
                    sysInfo.physicalDisks = *cachedPhysDisks;
                }

                // Serialize SMART attributes to JSON for each physical disk
                for (auto& pd : sysInfo.physicalDisks) {
                    if (pd.attributeCount > 0 && pd.attributeCount <= 32) {
                        std::string json = "[";
                        for (int ai = 0; ai < pd.attributeCount; ai++) {
                            if (ai > 0) json += ",";
                            auto& a = pd.attributes[ai];
                            // Convert WCHAR name to UTF-8
                            char nameUtf8[256] = {};
                            WideCharToMultiByte(CP_UTF8, 0, a.name, -1,
                                nameUtf8, (int)sizeof(nameUtf8) - 1, nullptr, nullptr);
                            // JSON-escape name
                            std::string nameEscaped;
                            for (const char* p = nameUtf8; *p; p++) {
                                if (*p == '"') nameEscaped += "\\\"";
                                else if (*p == '\\') nameEscaped += "\\\\";
                                else nameEscaped += *p;
                            }
                            // Convert WCHAR desc to UTF-8 and escape
                            char descUtf8[512] = {};
                            WideCharToMultiByte(CP_UTF8, 0, a.description, -1,
                                descUtf8, (int)sizeof(descUtf8) - 1, nullptr, nullptr);
                            std::string descEscaped;
                            for (const char* p = descUtf8; *p; p++) {
                                if (*p == '"') descEscaped += "\\\"";
                                else if (*p == '\\') descEscaped += "\\\\";
                                else descEscaped += *p;
                            }
                            char buf[768];
                            snprintf(buf, sizeof(buf),
                                "{\"id\":%u,\"cur\":%u,\"worst\":%u,\"raw\":%llu,\"name\":\"%s\",\"desc\":\"%s\"}",
                                a.id, a.current, a.worst, a.rawValue, nameEscaped.c_str(), descEscaped.c_str());
                            json += buf;
                        }
                        json += "]";
                        strncpy_s(pd.attrsJson, json.c_str(), sizeof(pd.attrsJson) - 1);
                        pd.attrsJson[sizeof(pd.attrsJson) - 1] = '\0';
                    }
                }

                // Collect TPM data
                {
                    TpmInfo tpmInfo = {};
                    if (TpmBridge::GetTpmInfo(tpmInfo)) {
                        sysInfo.tpms.clear();
                        sysInfo.tpms.push_back(tpmInfo);
                        Logger::Debug("TPM data collected, isPresent=" + std::to_string(tpmInfo.isPresent));
                    }
                }
            }
            catch (const std::bad_alloc& e) {
                Logger::Error("Failed to get physical disk / TPM data - Out of memory: " + std::string(e.what()));
                sysInfo.physicalDisks.clear();
            }
            catch (const std::exception& e) {
                Logger::Error("Failed to get physical disk / TPM data: " + std::string(e.what()));
                sysInfo.physicalDisks.clear();
            }
            catch (...) {
                Logger::Error("Failed to get physical disk / TPM data - Unknown exception");
                sysInfo.physicalDisks.clear();
            }

            // Validate data before writing to shared memory - enhanced data validation
            try {
                // CPU usage validation
                if (sysInfo.cpuUsage < 0.0 || sysInfo.cpuUsage > 100.0) {
                    Logger::Warn("CPU usage data abnormal: " + std::to_string(sysInfo.cpuUsage) + "%, resetting to 0");
                    sysInfo.cpuUsage = 0.0;
                }
                
                if (sysInfo.totalMemory > 0) {
                    if (sysInfo.usedMemory > sysInfo.totalMemory) {
                        Logger::Warn("Used memory exceeds total memory, data abnormal");
                        sysInfo.usedMemory = sysInfo.totalMemory;
                    }
                    if (sysInfo.availableMemory > sysInfo.totalMemory) {
                        Logger::Warn("Available memory exceeds total memory, data abnormal");
                        sysInfo.availableMemory = sysInfo.totalMemory;
                    }
                }
                
                // Frequency data validation
                if (std::isnan(sysInfo.performanceCoreFreq) || std::isinf(sysInfo.performanceCoreFreq)) {
                    sysInfo.performanceCoreFreq = 0.0;
                }
                if (std::isnan(sysInfo.efficiencyCoreFreq) || std::isinf(sysInfo.efficiencyCoreFreq)) {
                    sysInfo.efficiencyCoreFreq = 0.0;
                }
                if (std::isnan(sysInfo.gpuCoreFreq) || std::isinf(sysInfo.gpuCoreFreq)) {
                    sysInfo.gpuCoreFreq = 0.0;
                }
                
                // Temperature data validation
                if (std::isnan(sysInfo.cpuTemperature) || std::isinf(sysInfo.cpuTemperature)) {
                    sysInfo.cpuTemperature = 0.0;
                }
                if (std::isnan(sysInfo.gpuTemperature) || std::isinf(sysInfo.gpuTemperature)) {
                    sysInfo.gpuTemperature = 0.0;
                }
                
                // Network speed validation
                if (sysInfo.networkAdapterSpeed > 1000000000000ULL) {
                    Logger::Warn("Network adapter speed abnormal: " + std::to_string(sysInfo.networkAdapterSpeed));
                    sysInfo.networkAdapterSpeed = 0;
                }
            }
            catch (const std::exception& e) {
                Logger::Error("Exception during data validation: " + std::string(e.what()));
            }
            catch (...) {
                Logger::Error("Unknown exception during data validation");
            }

            // Write to shared memory (moved to TUI block after WiFi/BT populated)
            try {
                // SharedMemory write now handled in TUI update section below
            }
            catch (const std::bad_alloc& e) {
                Logger::Error("Out of memory while processing system info: " + std::string(e.what()));
            }
            catch (const std::exception& e) {
                Logger::Error("Exception while processing system info: " + std::string(e.what()));
            }
            catch (...) {
                Logger::Error("Unknown exception while processing system info");
            }
            
            // Update TUI with current data
            try {
                tcmt::TuiData tuiData;
                // Login identity is fixed for the process lifetime.
                static const tcmt::UserIdentity identity = tcmt::GetUserIdentity();
                tuiData.loginUser = identity.user;
                // Windows: "elevated" means the UAC token check; POSIX: euid 0.
                tuiData.processElevated = WinUtils::IsRunAsAdmin();
                tuiData.elevatedViaSudo = identity.viaSudo;
                tuiData.cpuName = sysInfo.cpuName;
                tuiData.cpuUsage = sysInfo.cpuUsage;
                tuiData.physicalCores = sysInfo.physicalCores;
                tuiData.performanceCores = sysInfo.performanceCores;
                tuiData.efficiencyCores = sysInfo.efficiencyCores;
                tuiData.pCoreFreq = sysInfo.performanceCoreFreq;
                tuiData.eCoreFreq = sysInfo.efficiencyCoreFreq;
                tuiData.cpuBaseFreq = sysInfo.cpuBaseFreq;
                tuiData.cpuTemp = sysInfo.cpuTemperature;
                tuiData.cpuPcoreTemp = sysInfo.cpuPcoreTemperature;
                tuiData.cpuEcoreTemp = sysInfo.cpuEcoreTemperature;
                tuiData.totalMemory = sysInfo.totalMemory;
                tuiData.usedMemory = sysInfo.usedMemory;
                tuiData.availableMemory = sysInfo.availableMemory;
                tuiData.compressedMemory = sysInfo.compressedMemory;
                tuiData.ramSpeed = sysInfo.ramSpeed;
                snprintf(tuiData.ramType, sizeof(tuiData.ramType), "%s", sysInfo.ramType);

                if (!sysInfo.gpus.empty()) {
                    tuiData.gpuName = sysInfo.gpuName;
                    tuiData.gpuMemory = sysInfo.gpuMemory;
                    tuiData.gpuUsage = sysInfo.gpuUsage;
                    tuiData.gpuMemoryPercent = sysInfo.gpuMemoryPercent;
                    // Full list for the panel/details; the scalars above keep
                    // describing the primary GPU for IPC and JSON.
                    tuiData.gpus.clear();
                    for (const auto& g : sysInfo.gpus) {
                        // Virtual display adapters (RDP, Parsec, VMware...) are
                        // not GPUs; listing them made the panel show a software
                        // adapter as if it were the real card.
                        if (g.isVirtual) continue;
                        tcmt::TuiData::GpuEntry e;
                        e.name = g.name;
                        e.driver = g.driver;
                        e.usage = g.usage;
                        e.memory = g.memory;
                        e.memoryPercent = g.memoryPercent;
                        e.unified = g.isUnified;
                        e.isVirtual = g.isVirtual;
                        e.coreCount = g.coreCount;
                        e.temperature = g.temperature;
                        e.clockMhz = g.coreClock;
                        e.maxClock = g.maxClock;
                        e.memClockMhz = g.memClock;
                        e.powerMw = g.powerMw;
                        e.powerLimitMw = g.powerLimitMw;
                        e.fanPercent = g.fanPercent;
                        e.serial = g.serial;
                        e.vbios = g.vbios;
                        e.cudaVersion = g.cudaVersion;
                        e.pcieGen = g.pcieGen;
                        e.pcieWidth = g.pcieWidth;
                        e.maxPcieGen = g.maxPcieGen;
                        e.maxPcieWidth = g.maxPcieWidth;
                        tuiData.gpus.push_back(e);
                    }
                }
                tuiData.gpuTemp = sysInfo.gpuTemperature;
                tuiData.cpuPower = sysInfo.cpuPower;
                tuiData.gpuPower = sysInfo.gpuPower;
                tuiData.anePower = sysInfo.anePower;
                // PowerMonitor is the macOS energy-model implementation; on
                // Windows IsPowerAvailable() is a stub that always returns
                // false, so the panel said "unavailable" even when NVML had
                // GPU power. Any live source means the panel has data.
                sysInfo.powerAvailable =
                    (sysInfo.cpuPower > 0 || sysInfo.gpuPower > 0 || sysInfo.anePower > 0);
                tuiData.powerAvailable = sysInfo.powerAvailable;
                tuiData.gpuFreq = sysInfo.gpuFreq;
                // GPU fans (NVAPI RPM + NVML fallback)
                tuiData.gpuFans.clear();
                for (const auto& gf : GpuInfo::GetGpuFans()) {
                    tcmt::TuiData::GpuFanInfo fi;
                    fi.index = gf.index;
                    fi.speedRpm = gf.speedRpm;
                    fi.isRpm = gf.isRpm;
                    tuiData.gpuFans.push_back(fi);
                }
                // Keep legacy field for backward compat
                tuiData.gpuFanSpeed = tuiData.gpuFans.empty() ? -1 : tuiData.gpuFans[0].speedRpm;
                // PID monitor — top processes by memory (refresh every ~3s)
                { static int procCtr = 0;
                  static ProcessMonitor s_procMon;
                  if (++procCtr >= 3) {
                      procCtr = 0;
                      try { s_procMon.Refresh(); } catch (...) {}
                  }
                  tuiData.topProcesses.clear();
                  // IPC 写出去的是 sysInfo.topProcesses，不是 tuiData 这一份。
                  // 以前只填了 TUI，Avalonia 的进程页因此恒空（proc/json 一直是 []）。
                  sysInfo.topProcesses.clear();
                  for (const auto& e : s_procMon.GetTop()) {
                      tcmt::TuiData::ProcessTopEntry pe;
                      pe.pid = e.pid;
                      pe.name = e.name;
                      pe.memoryBytes = e.memoryBytes;
                      pe.cpuPercent = e.cpuPercent;
                      // Row context — see the macOS bridge in main_mac.cpp.
                      pe.threads = e.threads;
                      pe.cpuTimeMs = e.cpuTimeMs;
                      pe.state = e.state;
                      pe.user = e.user;
                      tuiData.topProcesses.push_back(pe);

                      SystemInfo::ProcData sd;
                      sd.pid = e.pid;
                      sd.name = e.name;
                      sd.memoryBytes = e.memoryBytes;
                      sd.cpuPercent = e.cpuPercent;
                      sysInfo.topProcesses.push_back(std::move(sd));
                  }
                }

                // Disks
                for (const auto& disk : sysInfo.disks) {
                    tcmt::TuiData::DiskInfo di;
                    di.letter = disk.letter;
                    di.mountPoint = disk.mountPoint;
                    di.label = disk.label;
                    di.totalSize = disk.totalSize;
                    di.usedSpace = disk.usedSpace;
                    di.fileSystem = disk.fileSystem;
                    tuiData.disks.push_back(di);
                }

                // Physical disks (SMART)
                for (const auto& pd : sysInfo.physicalDisks) {
                    tcmt::TuiData::PhysicalDiskInfo pi;
                    pi.model = WinUtils::WstringToString(pd.model);
                    pi.serial = WinUtils::WstringToString(pd.serialNumber);
                    pi.interfaceType = WinUtils::WstringToString(pd.interfaceType);
                    pi.diskType = WinUtils::WstringToString(pd.diskType);
                    pi.capacity = pd.capacity;
                    pi.temperature = pd.temperature;
                    pi.healthPct = pd.healthPercentage;
                    pi.smartSupported = pd.smartSupported;
                    pi.powerOnHours = pd.powerOnHours;
                    pi.wearLeveling = pd.wearLeveling;
                    for (int ai = 0; ai < pd.attributeCount && ai < 32; ai++) {
                        const auto& sa = pd.attributes[ai];
                        tcmt::TuiData::SmAttributeInfo ai2;
                        ai2.id = sa.id;
                        ai2.current = sa.current;
                        ai2.worst = sa.worst;
                        ai2.rawValue = sa.rawValue;
                        ai2.name = WinUtils::WstringToString(sa.name);
                        pi.attributes.push_back(ai2);
                    }
                    tuiData.physicalDisks.push_back(pi);
                }
                
                // Network adapters
                for (const auto& adapter : sysInfo.adapters) {
                    tcmt::TuiData::NetInfo ni;
                    // Convert wchar_t[] arrays to std::string
                    ni.name = WinUtils::WstringToString(adapter.name);
                    ni.ip = WinUtils::WstringToString(adapter.ipAddress);
                    ni.mac = WinUtils::WstringToString(adapter.mac);
                    ni.type = WinUtils::WstringToString(adapter.adapterType);
                    ni.speed = adapter.speed;
                    ni.downloadSpeed = adapter.downloadSpeed;
                    ni.uploadSpeed = adapter.uploadSpeed;
                    tuiData.adapters.push_back(ni);
                }
                
                tuiData.osVersion = sysInfo.osVersion;

                // ─── Per-core die temperatures and live per-core state ───
                // MSR and PDH reads need one call per core, so they run on the
                // 1 s cadence instead of every UI frame. SamplePerCore walks
                // the same topology as ReadPerCore, so index i is the same
                // physical core in both arrays.
                {
                    static std::chrono::steady_clock::time_point coreTempAt{};
                    static std::vector<float> coreTemps;
                    static std::vector<CpuInfo::CoreSample> coreSamples;
                    const auto nowT = std::chrono::steady_clock::now();
                    const bool due = coreTempAt.time_since_epoch().count() == 0 ||
                        std::chrono::duration_cast<std::chrono::milliseconds>(nowT - coreTempAt).count() >= 1000;
                    if (due) {
                        coreTempAt = nowT;
                        const auto temps = CpuTempReader::ReadPerCore();
                        coreTemps.assign(temps.begin(), temps.end());
                        if (cpuInfo) coreSamples = cpuInfo->SamplePerCore();
                    }
                    const int n = std::min(static_cast<int>((std::max)(
                        coreTemps.size(), coreSamples.size())), 16);
                    bool hybrid = false;
                    sysInfo.perCoreCount = n;
                    for (int ci = 0; ci < n; ++ci) {
                        sysInfo.perCoreTemp[ci] = ci < static_cast<int>(coreTemps.size())
                            ? coreTemps[ci] : 0.0f;
                        if (ci < static_cast<int>(coreSamples.size())) {
                            sysInfo.perCoreFreq[ci] = static_cast<float>(coreSamples[ci].freqMHz);
                            sysInfo.perCoreUsage[ci] = static_cast<float>(coreSamples[ci].usage);
                            sysInfo.perCoreIsE[ci] = coreSamples[ci].type;
                            if (coreSamples[ci].type == 1) hybrid = true;
                        } else {
                            sysInfo.perCoreFreq[ci] = 0.0f;
                            sysInfo.perCoreUsage[ci] = -1.0f;
                            sysInfo.perCoreIsE[ci] = 0;
                        }
                    }
                    sysInfo.perCorePEKnown = hybrid;
                }

                // ─── Per-core sensors ───
                tuiData.perCoreCount = std::min((int)sysInfo.perCoreCount, 16);
                tuiData.perCoreUsageCount = tuiData.perCoreCount;
                tuiData.perCoreFreqCount = tuiData.perCoreCount;
                tuiData.perCorePEKnown = sysInfo.perCorePEKnown;
                for (int ci = 0; ci < tuiData.perCoreCount; ci++) {
                    tuiData.perCoreTemp[ci] = sysInfo.perCoreTemp[ci];
                    tuiData.perCoreFreq[ci] = sysInfo.perCoreFreq[ci];
                    tuiData.perCoreUsage[ci] = sysInfo.perCoreUsage[ci];
                    tuiData.perCoreIsE[ci] = sysInfo.perCoreIsE[ci];
                }

                // ─── Network traffic history (sparkline) ───
                // Statics: tuiData is rebuilt each pass, so a ring kept inside
                // the struct reset to one sample and never drew a sparkline.
                static uint64_t s_dlHist[tcmt::TuiData::NET_HISTORY_MAX] = {};
                static uint64_t s_ulHist[tcmt::TuiData::NET_HISTORY_MAX] = {};
                static int s_histPos = 0;
                static int s_histLen = 0;
                if (!tuiData.adapters.empty()) {
                    s_dlHist[s_histPos] = tuiData.adapters[0].downloadSpeed;
                    s_ulHist[s_histPos] = tuiData.adapters[0].uploadSpeed;
                    s_histPos = (s_histPos + 1) % tcmt::TuiData::NET_HISTORY_MAX;
                    if (s_histLen < tcmt::TuiData::NET_HISTORY_MAX) ++s_histLen;
                }
                std::memcpy(tuiData.dlHistory, s_dlHist, sizeof(s_dlHist));
                std::memcpy(tuiData.ulHistory, s_ulHist, sizeof(s_ulHist));
                tuiData.dlHistoryPos = s_histPos;
                tuiData.dlHistoryLen = s_histLen;
                tuiData.connectionCount = ipcServer ? ipcServer->GetClientCount() : 0;
                if (ipcServer) {
                    auto ct = ipcServer->GetClientTypes();
                    tuiData.clientTypes.clear();
                    for (auto t : ct) tuiData.clientTypes.push_back(static_cast<uint8_t>(t));
                }
                // 磁盘温度并进统一温度表。要并进 sysInfo 那一份 —— IPC 写的是
                // systemInfo.temperatures（见 SharedMemoryManager_Windows），
                // 以前只 append 给了 tuiData，所以 TUI 里看得到、界面上一条都没有。
                for (const auto& pd : tuiData.physicalDisks) {
                    if (pd.temperature > 0) {
                        std::string label = pd.model;
                        if (label.empty()) label = "Disk";
                        sysInfo.temperatures.push_back({label, pd.temperature});
                    }
                }
                // GPU temperature lives in its own field (NVML); the Temps panel
                // draws systemInfo.temperatures. Add it once if the temperature
                // collector did not already provide a GPU entry.
                if (sysInfo.gpuTemperature > 0) {
                    const bool hasGpuTemp = std::any_of(
                        sysInfo.temperatures.begin(), sysInfo.temperatures.end(),
                        [](const auto& t) { return t.first.find("GPU") != std::string::npos; });
                    if (!hasGpuTemp)
                        sysInfo.temperatures.push_back({"GPU Core", sysInfo.gpuTemperature});
                }
                tuiData.temperatures = sysInfo.temperatures;
                if (!sysInfo.tpms.empty() && sysInfo.tpms[0].isPresent) {
                    auto& tpm = sysInfo.tpms[0];
                    tuiData.tpmInfo = WinUtils::WstringToString(tpm.manufacturer)
                                    + " v" + WinUtils::WstringToString(tpm.firmwareVersion);
                    if (!tpm.isEnabled) tuiData.tpmInfo += " (Disabled)";
                    else if (!tpm.isActive) tuiData.tpmInfo += " (Inactive)";
                } else {
                    tuiData.tpmInfo = "No TPM";
                }

                // WiFi & Bluetooth (every ~3s, or immediate on ETW event)
                { static int wbCtr = 0;
                  static WiFiInfo s_wifi;
                  static BluetoothInfo s_bt;
                  bool forcePoll = coordinator.IsWifiDirty() || coordinator.IsBtDirty();
                  if (++wbCtr >= 3 || forcePoll) { wbCtr = 0;
                      try { s_wifi.Detect(); } catch (...) {}
                      if (s_btNotify.Poll() || forcePoll) { try { s_bt.Detect(); } catch (...) {} }
                  }
                  const auto& wd = s_wifi.GetData();
                  tuiData.hasWiFi = wd.powerOn;
                  tuiData.wifiConnected = wd.isConnected;
                  tuiData.wifiSSID = wd.ssid;
                  tuiData.wifiBSSID = wd.bssid;
                  tuiData.wifiRSSI = wd.rssi;
                  tuiData.wifiChannel = wd.channel;
                  tuiData.wifiSecurity = wd.security;
                  tuiData.wifiBand = wd.band;
                  tuiData.wifiGen = wd.wifiGen;
                  tuiData.wifiTxRate = wd.txRate;
                  tuiData.wifiChipset = wd.chipset;
                  sysInfo.wifiPowerOn = wd.powerOn;
                  sysInfo.wifiIsConnected = wd.isConnected;
                  sysInfo.wifiSSID = wd.ssid;
                  sysInfo.wifiRSSI = wd.rssi;
                  sysInfo.wifiChannel = wd.channel;
                  sysInfo.wifiSecurity = wd.security;
                  sysInfo.wifiBand = wd.band;
                  sysInfo.wifiGen = wd.wifiGen;
                  const auto& bd = s_bt.GetData();
                  tuiData.hasBluetooth = bd.adapter.powerOn || !bd.devices.empty();
                  tuiData.btPowerOn = bd.adapter.powerOn;
                  tuiData.btChipset = bd.adapter.chipset;
                  tuiData.btDevices.clear();
                  int btConnected = 0;
                  for (const auto& dev : bd.devices) {
                      tcmt::TuiData::BtDeviceInfo d;
                      d.name = dev.name;
                      d.type = dev.type;
                      d.battery = CompactBatteryText(dev);
                      d.rssi = dev.rssi;
                      d.connected = dev.connected;
                      if (dev.connected) ++btConnected;
                      tuiData.btDevices.push_back(std::move(d));
                  }
                  tuiData.btDeviceCount = btConnected;
                  sysInfo.btPowerOn = bd.adapter.powerOn;
                  sysInfo.btDeviceCount = btConnected;
                  sysInfo.btDeviceName = JoinConnectedDeviceNames(bd, 63);

                  // Write WiFi & Bluetooth to shared memory block (via WriteToSharedMemory below)
                  sysInfo.wifiPowerOn = wd.powerOn;
                  sysInfo.wifiIsConnected = wd.isConnected;
                }

                tuiData.timestamp = FormatDateTime(std::chrono::system_clock::now());

                tuiApp.UpdateData(tuiData);

                // Live push status for the TUI (handshake visibility).
                if (g_push.active) {
                    g_push.status = g_push.probe.Token().empty()
                        ? "connecting..."
                        : ("uploading (" + g_push.probe.DeviceId() + ")");
                }

                // Time-based upload gate (configurable 1..60 s via TUI).
                {
                    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    const bool due = g_push.lastPushMs == 0 ||
                        nowMs - g_push.lastPushMs >= g_push.intervalMs;
                    if (g_push.active && due && g_push.probe.Token().size() > 0) {
                        std::string body = "{";
                        char buf[512];
                        snprintf(buf, sizeof(buf),
                            "\"cpu_usage\":%.1f,\"cpu_temp\":%.1f,"
                            "\"memory_used\":%llu,\"memory_total\":%llu,"
                            "\"gpu_usage\":%.1f,\"gpu_temp\":%.1f,"
                            "\"cpu_name\":\"%s\",\"gpu_name\":\"%s\","
                            "\"os_version\":\"%s\",\"uptime\":%llu",
                            tuiData.cpuUsage, tuiData.cpuTemp,
                            (unsigned long long)tuiData.usedMemory,
                            (unsigned long long)tuiData.totalMemory,
                            tuiData.gpuUsage, tuiData.gpuTemp,
                            JsonSafeStr(tuiData.cpuName).c_str(),
                            JsonSafeStr(tuiData.gpuName).c_str(),
                            JsonSafeStr(tuiData.osVersion).c_str(),
                            (unsigned long long)tuiData.uptimeSeconds);
                        body += buf;
                        // Disks (volumes): name + used/total bytes.
                        body += ",\"disks\":[";
                        for (size_t di = 0; di < tuiData.disks.size(); ++di) {
                            if (di) body += ",";
                            const auto& dsk = tuiData.disks[di];
                            std::string dname = !dsk.label.empty() ? dsk.label
                                : (dsk.letter ? std::string(1, dsk.letter) : std::string("?"));
                            char db[256];
                            snprintf(db, sizeof(db), "{\"name\":\"%s\",\"used\":%llu,\"total\":%llu}",
                                     JsonSafeStr(dname).c_str(),
                                     (unsigned long long)dsk.usedSpace,
                                     (unsigned long long)dsk.totalSize);
                            body += db;
                        }
                        body += "]";
                        // Network adapters: name + down/up speeds (bytes/s).
                        body += ",\"net\":[";
                        for (size_t ni = 0; ni < tuiData.adapters.size(); ++ni) {
                            if (ni) body += ",";
                            char nb[256];
                            snprintf(nb, sizeof(nb), "{\"name\":\"%s\",\"down\":%llu,\"up\":%llu}",
                                     JsonSafeStr(tuiData.adapters[ni].name).c_str(),
                                     (unsigned long long)tuiData.adapters[ni].downloadSpeed,
                                     (unsigned long long)tuiData.adapters[ni].uploadSpeed);
                            body += nb;
                        }
                        body += "]";
                        // Battery (optional — -1 means no battery).
                        if (tuiData.batteryPercent >= 0) {
                            char bb[96];
                            snprintf(bb, sizeof(bb),
                                     ",\"battery\":{\"percent\":%d,\"charging\":%s,\"health\":%.1f}",
                                     tuiData.batteryPercent,
                                     tuiData.acOnline ? "true" : "false",
                                     tuiData.batteryHealthPercent);
                            body += bb;
                        }
                        auto temps = TemperatureWrapper::GetTemperatures();
                        body += ",\"temperatures\":[";
                        for (size_t ti = 0; ti < temps.size(); ++ti) {
                            if (ti) body += ",";
                            char tb[192];
                            snprintf(tb, sizeof(tb),
                                     "{\"name\":\"%s\",\"value\":%.1f,\"unit\":\"C\"}",
                                     JsonSafeStr(temps[ti].first).c_str(), temps[ti].second);
                            body += tb;
                        }
                        body += "]}";
                        g_push.probe.PostSnapshot(std::move(body));
                        g_push.lastPushMs = nowMs;
                    }
                }

                // Write to shared memory (after WiFi/BT data populated)
                try {
                    if (SharedMemoryManager::GetBuffer()) {
                        SharedMemoryManager::WriteToSharedMemory(sysInfo);
                    }
                } catch (...) {}
            }
            catch (const std::exception& e) {
                Logger::Warn("TUI data update failed: " + std::string(e.what()));
            }

            // Calculate loop execution time and adaptive sleep - optimize refresh speed, enhanced exception handling
            try {
                auto loopEnd = std::chrono::high_resolution_clock::now();
                auto loopDuration = std::chrono::duration_cast<std::chrono::milliseconds>(loopEnd - loopStart);
                
                // 采样周期由设置页控制（原来是写死的 1000ms）
                int targetCycleTime = g_sampleIntervalMs.load();
                int sleepTime = (std::max)(targetCycleTime - static_cast<int>(loopDuration.count()), 100); // Min sleep 100ms
                
                if (isDetailedLogging) {
                    double loopTimeSeconds = loopDuration.count() / 1000.0;
                    double sleepTimeSeconds = sleepTime / 1000.0;
                    
                    if (loopTimeSeconds < 0 || loopTimeSeconds > 60) {
                        Logger::Warn("Loop time calculation abnormal: " + std::to_string(loopTimeSeconds) + " seconds");
                    }
                    
                    std::stringstream ss;
                    ss << std::fixed << std::setprecision(2);
                    ss << "Main monitoring loop #" << loopCounter << " executed in " 
                       << loopTimeSeconds << "s, will sleep for " << sleepTimeSeconds << "s";
                    
                    Logger::Debug(ss.str());
                }
                
                // Check exit flag during sleep - use shorter check interval for better responsiveness
                auto sleepStart = std::chrono::high_resolution_clock::now();
                while (!g_shouldExit.load()) {
                    try {
                        auto now = std::chrono::high_resolution_clock::now();
                        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - sleepStart);
                        if (elapsed.count() >= sleepTime) {
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    }
                    catch (const std::exception& e) {
                        Logger::Warn("Exception during sleep: " + std::string(e.what()));
                        break;
                    }
                    catch (...) {
                        Logger::Warn("Unknown exception during sleep");
                        break;
                    }
                }
            }
            catch (const std::exception& e) {
                Logger::Error("Exception while calculating loop time: " + std::string(e.what()));
                try {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                } catch (...) {
                    Logger::Fatal("System sleep function abnormal");
                }
            }
            catch (...) {
                Logger::Error("Unknown exception while calculating loop time");
                try {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                } catch (...) {
                    Logger::Fatal("System sleep function abnormal");
                }
            }
            
            // Safely increment loop counter
            try {
                // Push sensor history
                if (historyLogger.IsRunning()) {
                    auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count();
                    std::vector<SensorSnapshot> snapshots;
                    snapshots.push_back({"cpu/usage", sysInfo.cpuUsage, "%", (uint64_t)nowMs});
                    snapshots.push_back({"cpu/temperature", sysInfo.cpuTemperature, "C", (uint64_t)nowMs});
                    snapshots.push_back({"gpu/usage", sysInfo.gpuUsage, "%", (uint64_t)nowMs});
                    snapshots.push_back({"gpu/temperature", sysInfo.gpuTemperature, "C", (uint64_t)nowMs});
                    if (sysInfo.totalMemory > 0) {
                        double memPct = 100.0 * sysInfo.usedMemory / sysInfo.totalMemory;
                        snapshots.push_back({"memory/percent", memPct, "%", (uint64_t)nowMs});
                    }
                    if (sysInfo.batteryPercent >= 0)
                        snapshots.push_back({"battery/percent", (double)sysInfo.batteryPercent, "%", (uint64_t)nowMs});
                    // USB detection (every ~10 seconds)
                    static int usbCheckCounter = 0;
                    static size_t prevUsbCount = 0;
                    if (s_usbNotify.Poll() || s_hubNotify.Poll()) usbCheckCounter = 20;
                    if (++usbCheckCounter >= 20) {
                        usbCheckCounter = 0;
                        try {
                            UsbInfo usb;
                            usb.Detect();
                            const auto& devs = usb.GetDevices();
                            if (devs.size() != prevUsbCount) {
                                if (devs.empty())
                                    Logger::Info("USB: all devices removed");
                                else {
                                    Logger::Info("USB: " + std::to_string(devs.size()) + " device(s)");
                                    for (size_t di = 0; di < std::min(devs.size(), size_t(8)); ++di)
                                        Logger::Debug("  " + devs[di].name + " VID:" + std::to_string(devs[di].vid)
                                                    + " PID:" + std::to_string(devs[di].pid));
                                }
                                prevUsbCount = devs.size();
                            }
                        } catch (...) {}
                    }

                    historyLogger.WriteBatch(snapshots);
                    }

                loopCounter++;
                
                if (loopCounter < 0 || loopCounter > 2000000000) {
                    Logger::Warn("Loop counter abnormal, resetting to 1");
                    loopCounter = 1;
                }
            }
            catch (...) {
                Logger::Error("Failed to update loop counter");
                loopCounter = 1;
            }
            
            // Set flag after first run
            if (isFirstRun) {
                isFirstRun = false;
            }
        }
        catch (const std::bad_alloc& e) {
            Logger::Critical("Memory allocation exception in main loop: " + std::string(e.what()));
            try {
                std::this_thread::sleep_for(std::chrono::milliseconds(2000));
            } catch (...) {
                Logger::Fatal("Cannot execute sleep, system severe exception");
            }
            continue;
        }
        catch (const std::exception& e) {
            Logger::Critical("Exception in main loop: " + std::string(e.what()));
            try {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            } catch (...) {
                Logger::Fatal("Cannot execute sleep, system severe exception");
            }
            continue;
        }
        catch (...) {
            Logger::Fatal("Unknown exception in main loop");
            try {
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
            } catch (...) {
                SafeExit(1);
            }
            continue;
        }
    }

    // Staged on purpose: if Ctrl+C ever stops short, the log shows which
    // thread join never returned.
    Logger::Info("Monitoring loop finished, stopping collection threads");
    coordinator.Stop();
    Logger::Info("Collection threads stopped");
}

int main(int argc, char* argv[]) {
    _set_se_translator(SEHTranslator);
    
    std::set_new_handler([]() {
        Logger::Fatal("Memory allocation failed - system out of memory");
        throw std::bad_alloc();
    });
    
    SetConsoleCP(65001);
    SetConsoleOutputCP(65001);
    
    // UCRT's UTF-8 locale is ".UTF-8"; the glibc-style name fails on MSVC and
    // silently leaves the "C" locale. PDCurses is built with PDC_WIDE, so a
    // non-UTF-8 locale makes addstr() decode UTF-8 bytes as the ANSI code
    // page — every Chinese string then renders as mojibake.
    if (!setlocale(LC_ALL, ".UTF-8"))
        setlocale(LC_ALL, "en_US.UTF-8");
    
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);

    try {
        try {
            Logger::EnableConsoleOutput(true);
            // Absolute path next to the exe — the process may be auto-elevated
            // (ShellExecuteEx runas) which changes the working directory, so a
            // relative "system_monitor.log" would land in System32 or elsewhere.
            Logger::Initialize(WinUtils::GetExecutableDirectory() + "\\system_monitor.log");
            Logger::SetLogLevel(LOG_INFO);      // default: INFO+ (matches main_mac/main_linux); --debug lowers it
            Logger::Info("Program started");
        }
        catch (const std::exception& e) {
            printf("Logging system initialization failed: %s\n", e.what());
            return 1;
        }

        // --debug / --verbose: lower the log threshold below the WARNING default
        // (scan any argv position; --json/--mcp keep their argv[1] matching below).
        for (int i = 1; i < argc; ++i) {
            if (std::string(argv[i]) == "--debug") Logger::SetLogLevel(LOG_DEBUG);
            else if (std::string(argv[i]) == "--verbose") Logger::SetLogLevel(LOG_INFO);
        }

        // ======================== --json Mode ========================
        bool jsonMode = (argc > 1 && std::string(argv[1]) == "--json");
        if (jsonMode) return RunJsonMode();

        // ======================== --mcp Mode ========================
        bool mcpMode = (argc > 1 && std::string(argv[1]) == "--mcp");
        if (mcpMode) return RunMcpMode();

        if (!InitCom()) return -1;

        std::unique_ptr<tcmt::ipc::IPCServer> ipcServer;
        if (!InitSharedMemoryAndIpc(ipcServer)) SafeExit(1);

        auto wmiManager = InitWmiManager();
        if (!wmiManager) SafeExit(1);

        InitTemperatureBridge();

        Logger::Info("Program startup complete");

        // The TUI owns the console from here. The Windows logger writes INFO
        // in light green and then resets the console attribute, but PDCurses
        // draws through that same console: the color change and the log text
        // bleed into the screen and leave the UI tinted green. macOS/Linux
        // already disable console output for their TUI; do the same here and
        // keep the in-app log page fed through g_tuiLogBuffer.
        Logger::EnableConsoleOutput(false);

        // Start TUI (Windows version)
        tcmt::TuiApp tuiApp;
        tuiApp.Start();

        // Server push settings (TUI settings page, press S).
        {
            auto& cfg = GetServerCfg();
            g_push.enabled = cfg.GetBool("server.enabled", false);
            g_push.url = cfg.GetString("server.url", "http://127.0.0.1:8080");
            g_push.insecure = cfg.GetBool("server.insecure", false);
            g_push.intervalSec = (std::max)(1, (std::min)(cfg.GetInt("server.intervalSec", 2), 60));
            g_push.intervalMs = (int64_t)g_push.intervalSec * 1000;
            g_push.active = g_push.enabled;
            if (g_push.enabled) {
                g_push.probe.SetInsecure(g_push.insecure);
                if (g_push.probe.Start(g_push.url)) {
                    g_push.status = "connecting...";
                } else {
                    g_push.status = "error: cannot start";
                    g_push.active = false;
                }
            }
        }
        // 采样间隔从配置读（跟 macOS 那边一样），夹到允许区间内。
        // 这是启动时的一次性读取，主循环每次迭代读的是上面那个原子量。
        {
            const int ms = GetServerCfg().GetInt("sampling.intervalMs",
                                                 g_sampleIntervalMs.load());
            g_sampleIntervalMs.store(
                (std::max)(tcmt::kSampleIntervalMinMs,
                           (std::min)(tcmt::kSampleIntervalMaxMs, ms)));
            // 启动值由 RunMonitoringLoop 在其作用域内喂给 coordinator
            Logger::Info("Sampling interval: " + std::to_string(g_sampleIntervalMs.load()) +
                         " ms");
        }

        // 同上：六个成员，别漏 sampleIntervalMs —— 而且这里要传真值，不是默认值
        tuiApp.SetServerSettings(
            {g_push.enabled, g_push.url, g_push.insecure, g_push.intervalSec,
             g_sampleIntervalMs.load(), g_push.status});
        tuiApp.SetServerSettingsHandler([](const tcmt::ServerSettings& s) {
            auto& cfg = GetServerCfg();
            cfg.SetBool("server.enabled", s.enabled);
            cfg.SetString("server.url", s.url);
            cfg.SetBool("server.insecure", s.insecure);
            cfg.SetInt("server.intervalSec", s.intervalSec);
            cfg.Save();
            std::lock_guard<std::mutex> lock(g_push.mutex);
            g_push.pending = s;
            g_push.pendingReady = true;
            Logger::Info("Server settings saved: enabled=" +
                         std::string(s.enabled ? "true" : "false") + " url=" + s.url);
        });
        // 采样间隔的 handler 在 RunMonitoringLoop 里注册 —— coordinator 是那边的局部变量。
        // 语言同样单独一个 handler：设置页选的语言以前只活在本次会话里，重启就回到默认。
        // 启动时把存下来的值喂回去（TCMT_LANG 仍然优先）。
        {
            auto& cfg = GetServerCfg();
            tuiApp.SetConfiguredLanguage(cfg.GetString("ui.language", ""));
        }
        tuiApp.SetLanguageHandler([](tcmt::Lang lang) {
            auto& cfg = GetServerCfg();
            cfg.SetString("ui.language", lang == tcmt::Lang::ZhHans ? "zh-Hans" : "en");
            cfg.Save();
            Logger::Info(std::string("UI language set to ")
                         + (lang == tcmt::Lang::ZhHans ? "zh-Hans" : "en"));
        });
        Logger::Info("TUI started");

        // 独立日志窗口（Win32，同进程）是可选的，跟 macOS 那边一样要 TCMT_LOG_WINDOW=1 才开。
        // 以前 Windows 无条件弹一个额外窗口，而 TUI 自带日志页（按 L），等于每次都多一个。
        tcmt::LogWindow logWindow;
        if (std::getenv("TCMT_LOG_WINDOW") != nullptr) {
            if (logWindow.Create(&Logger::GetTuiBuffer())) {
                Logger::Info("Log window opened (in-process Win32)");
            } else {
                Logger::Warn("TCMT_LOG_WINDOW=1 but window creation failed — using in-TUI log page (L)");
            }
        } else {
            Logger::Info("Single-window log: in-TUI log page (press L)");
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

        // USB flash drive hotplug monitor (USBMonitor-cpp)
        USBMonitor usbFlashMonitor(
            [](UsbState state, std::string path) {
                switch (state) {
                    case UsbState::Inserted:
                        Logger::Info("USB flash drive inserted: " + path);
                        break;
                    case UsbState::Removed:
                        Logger::Info("USB flash drive removed: " + path);
                        break;
                    case UsbState::UpdateReady:
                        Logger::Info("USB update drive detected: " + path);
                        break;
                }
            }
        );
        usbFlashMonitor.startMonitoring();

        // History logger (SQLite)
        HistoryLogger historyLogger;
        InitHistoryLogger(historyLogger);

        RunMonitoringLoop(wmiManager, ipcServer, tuiApp, historyLogger);
        Logger::Info("Program received exit signal, starting cleanup");

        // Close the log window before tearing down the TUI/logger.
        logWindow.Shutdown();
        Logger::Info("Log window closed");

        // Stop TUI before cleanup
        try {
            tuiApp.Stop();
            Logger::Info("TUI stopped");
        }
        catch (const std::exception& e) {
            Logger::Error("Error stopping TUI: " + std::string(e.what()));
        }
        
        historyLogger.Shutdown();
        Logger::Info("HistoryLogger stopped");

        SafeExit(0);
    }
    catch (const std::exception& e) {
        Logger::Fatal("Program fatal error: " + std::string(e.what()));
        SafeExit(1);
    }
    catch (...) {
        Logger::Fatal("Program unknown fatal error");
        SafeExit(1);
    }
}

// Check for key press (non-blocking) - enhanced exception handling
bool CheckForKeyPress() {
    try {
        return _kbhit() != 0;
    }
    catch (const std::exception& e) {
        Logger::Warn("Exception while checking key press: " + std::string(e.what()));
        return false;
    }
    catch (...) {
        Logger::Warn("Unknown exception while checking key press");
        return false;
    }
}

char GetKeyPress() {
    try {
        if (_kbhit()) {
            char key = _getch();
            if (key >= 0 && key <= 127) {
                return key;
            } else {
                Logger::Warn("Detected abnormal key value: " + std::to_string(static_cast<int>(key)));
                return 0;
            }
        }
    }
    catch (const std::exception& e) {
        Logger::Warn("Exception while getting key press: " + std::string(e.what()));
    }
    catch (...) {
        Logger::Warn("Unknown exception while getting key press");
    }
    return 0;
}


