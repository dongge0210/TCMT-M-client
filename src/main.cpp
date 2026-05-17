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
#include "core/Utils/Logger.h"
#include "core/Utils/TimeUtils.h"
#include "core/Utils/WinUtils.h"
#include "core/Utils/WmiManager.h"
#include "core/disk/DiskInfo.h"
#include "core/Utils/TpmBridge.h"
#include "core/DataStruct/DataStruct.h"
#include "core/DataStruct/SharedMemoryManager.h"
#include "core/IPC/IPCServer.h"
#include "core/history/HistoryLogger.h"
#include "core/usb/UsbInfo.h"
#include "core/wifi/WiFiInfo.h"
#include "core/bluetooth/BluetoothInfo.h"
#include "core/DeviceChangeNotifier.h"
#include "core/MCP/MCPServer.h"
#include "core/IPC/IPCClient.h"
#include "core/temperature/TemperatureWrapper.h"
#include "core/ModuleCoordinator.h"
#include "tui/TuiApp.h"
#include "core/Config/ConfigManager.h"
#include <fstream>
#include <cstdio>

#pragma comment(lib, "kernel32.lib")
#pragma comment(lib, "user32.lib")

std::atomic<bool> g_shouldExit{false};
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
        Logger::Info("Received shutdown signal, exiting safely...");
        g_shouldExit = true;
        SafeConsoleOutput("Exiting program...\n", 14);
        return TRUE;
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
                Logger::Warn("Time format result length abnormal: " + std::to_string(result.length()));
            }
        } else {
            Logger::Error("localtime_s call failed");
        }
    }
    catch (const std::exception& e) {
        Logger::Error("Exception during time formatting: " + std::string(e.what()));
    }
    catch (...) {
        Logger::Error("Exception during time formatting - unknown exception");
    }
    return "Time Formatting Failed";
}

std::string FormatFrequency(double value) {
    try {
        // Parameter validation
        if (std::isnan(value) || std::isinf(value)) {
            Logger::Warn("Invalid frequency value: " + std::to_string(value));
            return "N/A";
        }
        
        if (value < 0) {
            Logger::Warn("Frequency value is negative: " + std::to_string(value));
            return "N/A";
        }
        
        // Reasonableness check - frequency typically not exceeding 10GHz
        if (value > 10000) {
            Logger::Warn("Frequency value abnormal: " + std::to_string(value) + "MHz");
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
        Logger::Error("Exception during frequency formatting: " + std::string(e.what()));
        return "Formatting Failed";
    }
    catch (...) {
        Logger::Error("Exception during frequency formatting - unknown exception");
        return "Formatting Failed";
    }
}

std::string FormatPercentage(double value) {
    try {
        // Parameter validation
        if (std::isnan(value) || std::isinf(value)) {
            Logger::Warn("Percentage value invalid: " + std::to_string(value));
            return "N/A";
        }
        
        // Reasonableness check - percentage typically between 0-100
        if (value < -1.0 || value > 105.0) {
            Logger::Warn("Percentage value abnormal: " + std::to_string(value));
        }
        
        // Limit to reasonable range
        if (value < 0) value = 0;
        if (value > 100) value = 100;
        
        std::stringstream ss;
        ss << std::fixed << std::setprecision(1) << value << "%";
        return ss.str();
    }
    catch (const std::exception& e) {
        Logger::Error("Exception during percentage formatting: " + std::string(e.what()));
        return "Formatting Failed";
    }
    catch (...) {
        Logger::Error("Exception during percentage formatting - unknown exception");
        return "Formatting Failed";
    }
}

std::string FormatTemperature(double value) {
    try {
        // Parameter validation
        if (std::isnan(value) || std::isinf(value)) {
            Logger::Warn("Temperature value invalid: " + std::to_string(value));
            return "N/A";
        }
        
        // Reasonableness check - temperature typically between -50C to 150C
        if (value < -50.0 || value > 150.0) {
            Logger::Warn("Temperature value abnormal: " + std::to_string(value) + "°C");
            if (value < -50.0) return "Too Low";
            if (value > 150.0) return "Too High";
        }
        
        std::stringstream ss;
        ss << static_cast<int>(value) << "°C";  // Display integer temperature
        return ss.str();
    }
    catch (const std::exception& e) {
        Logger::Error("Exception during temperature formatting: " + std::string(e.what()));
        return "Formatting Failed";
    }
    catch (...) {
        Logger::Error("Exception during temperature formatting - unknown exception");
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
            Logger::Warn("Bytes at maximum value, may indicate error");
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
        Logger::Error("Exception during size formatting: " + std::string(e.what()));
        return "Formatting Failed";
    }
    catch (...) {
        Logger::Error("Exception during size formatting - unknown exception");
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

// Main function
bool IsRunAsAdmin() {
    BOOL isAdmin = FALSE;
    PSID adminGroup = NULL;
    SID_IDENTIFIER_AUTHORITY NtAuthority = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&NtAuthority, 2,
        SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
        0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(NULL, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin == TRUE;
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
    
    bool IsInitialized() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return initialized_;
    }
};

// ======================== MCP Helpers ========================

// Build schema describing SharedMemoryBlock fields (for IPCServer + ConnectDirect)
static void BuildWindowsIpcSchema(tcmt::ipc::SchemaHeader& schemaHdr,
                                   std::vector<tcmt::ipc::FieldDef>& fields) {
    schemaHdr.totalSize = sizeof(SharedMemoryBlock);
    auto addField = [&](const char* name, uint32_t offset, uint16_t size,
                        uint8_t type = (uint8_t)tcmt::ipc::FieldType::Float64,
                        uint32_t count = 0) {
        tcmt::ipc::FieldDef f{};
        f.offset = offset; f.size = size; f.type = type; f.count = count;
        std::strncpy(f.name, name, tcmt::ipc::IPC_FIELD_NAME_LEN - 1);
        fields.push_back(f);
    };
    using FT = tcmt::ipc::FieldType;
    addField("cpu/name", offsetof(SharedMemoryBlock, cpuName), 128 * sizeof(WCHAR), (uint8_t)FT::WString);
    addField("cpu/cores/physical", offsetof(SharedMemoryBlock, physicalCores), 4, (uint8_t)FT::Int32);
    addField("cpu/cores/logical", offsetof(SharedMemoryBlock, logicalCores), 4, (uint8_t)FT::Int32);
    addField("cpu/usage", offsetof(SharedMemoryBlock, cpuUsage), 8);
    addField("cpu/cores/performance", offsetof(SharedMemoryBlock, performanceCores), 4, (uint8_t)FT::Int32);
    addField("cpu/cores/efficiency", offsetof(SharedMemoryBlock, efficiencyCores), 4, (uint8_t)FT::Int32);
    addField("cpu/freq/pCore", offsetof(SharedMemoryBlock, pCoreFreq), 8);
    addField("cpu/freq/eCore", offsetof(SharedMemoryBlock, eCoreFreq), 8);
    addField("cpu/freq/base", offsetof(SharedMemoryBlock, cpuBaseFreq), 8);
    addField("cpu/hyperThreading", offsetof(SharedMemoryBlock, hyperThreading), 1, (uint8_t)FT::Bool);
    addField("cpu/virtualization", offsetof(SharedMemoryBlock, virtualization), 1, (uint8_t)FT::Bool);
    addField("cpu/temperature", offsetof(SharedMemoryBlock, cpuTemperature), 8);
    addField("memory/total", offsetof(SharedMemoryBlock, totalMemory), 8, (uint8_t)FT::UInt64);
    addField("memory/used", offsetof(SharedMemoryBlock, usedMemory), 8, (uint8_t)FT::UInt64);
    addField("memory/available", offsetof(SharedMemoryBlock, availableMemory), 8, (uint8_t)FT::UInt64);
    addField("memory/compressed", offsetof(SharedMemoryBlock, compressedMemory), 8, (uint8_t)FT::UInt64);
    addField("gpu/temperature", offsetof(SharedMemoryBlock, gpuTemperature), 8);
    for (int i = 0; i < 2; i++) {
        char prefix[32]; snprintf(prefix, sizeof(prefix), "gpu/%d/", i);
        uint32_t base = offsetof(SharedMemoryBlock, gpus) + i * sizeof(GPUData);
        addField((std::string(prefix)+"name").c_str(), base + offsetof(GPUData, name), 128*(int)sizeof(WCHAR), (uint8_t)FT::WString);
        addField((std::string(prefix)+"brand").c_str(), base + offsetof(GPUData, brand), 64*(int)sizeof(WCHAR), (uint8_t)FT::WString);
        addField((std::string(prefix)+"memory").c_str(), base + offsetof(GPUData, memory), 8, (uint8_t)FT::UInt64);
        addField((std::string(prefix)+"usage").c_str(), base + offsetof(GPUData, usage), 8);
        addField((std::string(prefix)+"isVirtual").c_str(), base + offsetof(GPUData, isVirtual), 1, (uint8_t)FT::Bool);
        addField((std::string(prefix)+"memoryPercent").c_str(), base + offsetof(GPUData, coreClock), 8);
        addField((std::string(prefix)+"temperature").c_str(), offsetof(SharedMemoryBlock, gpuTemperature), 8);
    }
    addField("battery/percent", offsetof(SharedMemoryBlock, batteryPercent), 4, (uint8_t)FT::Int32);
    addField("battery/acOnline", offsetof(SharedMemoryBlock, acOnline), 1, (uint8_t)FT::Bool);
    addField("os/version", offsetof(SharedMemoryBlock, osVersion), 128*(int)sizeof(WCHAR), (uint8_t)FT::WString);

    // Network adapters (up to 4)
    for (int i = 0; i < 4; i++) {
        char pfx[32]; snprintf(pfx, sizeof(pfx), "net/%d/", i);
        uint32_t base = offsetof(SharedMemoryBlock, adapters) + i * sizeof(NetworkAdapterData);
        addField((std::string(pfx)+"name").c_str(), base + offsetof(NetworkAdapterData, name), 128*(int)sizeof(WCHAR), (uint8_t)FT::WString);
        addField((std::string(pfx)+"ip").c_str(),   base + offsetof(NetworkAdapterData, ipAddress), 64*(int)sizeof(WCHAR), (uint8_t)FT::WString);
        addField((std::string(pfx)+"mac").c_str(),  base + offsetof(NetworkAdapterData, mac), 32*(int)sizeof(WCHAR), (uint8_t)FT::WString);
        addField((std::string(pfx)+"type").c_str(), base + offsetof(NetworkAdapterData, adapterType), 32*(int)sizeof(WCHAR), (uint8_t)FT::WString);
        addField((std::string(pfx)+"speed").c_str(),  base + offsetof(NetworkAdapterData, speed), 8, (uint8_t)FT::UInt64);
        addField((std::string(pfx)+"downloadSpeed").c_str(), base + offsetof(NetworkAdapterData, downloadSpeed), 8, (uint8_t)FT::UInt64);
        addField((std::string(pfx)+"uploadSpeed").c_str(),   base + offsetof(NetworkAdapterData, uploadSpeed), 8, (uint8_t)FT::UInt64);
    }

    // Disks (up to 8)
    for (int i = 0; i < 8; i++) {
        char pfx[32]; snprintf(pfx, sizeof(pfx), "disk/%d/", i);
        uint32_t base = offsetof(SharedMemoryBlock, disks) + i * sizeof(SharedMemoryBlock::SharedDiskData);
        addField((std::string(pfx)+"letter").c_str(), base + offsetof(SharedMemoryBlock::SharedDiskData, letter), 1, (uint8_t)FT::UInt8);
        addField((std::string(pfx)+"label").c_str(), base + offsetof(SharedMemoryBlock::SharedDiskData, label), 128*(int)sizeof(WCHAR), (uint8_t)FT::WString);
        addField((std::string(pfx)+"fs").c_str(),    base + offsetof(SharedMemoryBlock::SharedDiskData, fileSystem), 32*(int)sizeof(WCHAR), (uint8_t)FT::WString);
        addField((std::string(pfx)+"total").c_str(), base + offsetof(SharedMemoryBlock::SharedDiskData, totalSize), 8, (uint8_t)FT::UInt64);
        addField((std::string(pfx)+"used").c_str(),  base + offsetof(SharedMemoryBlock::SharedDiskData, usedSpace), 8, (uint8_t)FT::UInt64);
        addField((std::string(pfx)+"free").c_str(),  base + offsetof(SharedMemoryBlock::SharedDiskData, freeSpace), 8, (uint8_t)FT::UInt64);
    }

    // Temperature sensors (up to 10)
    for (int i = 0; i < 10; i++) {
        char pfx[32]; snprintf(pfx, sizeof(pfx), "sensor/%d/", i);
        uint32_t base = offsetof(SharedMemoryBlock, temperatures) + i * sizeof(TemperatureData);
        addField((std::string(pfx)+"name").c_str(), base + offsetof(TemperatureData, sensorName), 64*(int)sizeof(WCHAR), (uint8_t)FT::WString);
        addField((std::string(pfx)+"value").c_str(), base + offsetof(TemperatureData, temperature), 8);
    }

    // Physical disks (SMART) (up to 8)
    for (int i = 0; i < 8; i++) {
        char pfx[32]; snprintf(pfx, sizeof(pfx), "phys/%d/", i);
        uint32_t base = offsetof(SharedMemoryBlock, physicalDisks) + i * sizeof(PhysicalDiskSmartData);
        addField((std::string(pfx)+"model").c_str(),       base + offsetof(PhysicalDiskSmartData, model), 128*(int)sizeof(WCHAR), (uint8_t)FT::WString);
        addField((std::string(pfx)+"serial").c_str(),      base + offsetof(PhysicalDiskSmartData, serialNumber), 64*(int)sizeof(WCHAR), (uint8_t)FT::WString);
        addField((std::string(pfx)+"capacity").c_str(),    base + offsetof(PhysicalDiskSmartData, capacity), 8, (uint8_t)FT::UInt64);
        addField((std::string(pfx)+"interface").c_str(),   base + offsetof(PhysicalDiskSmartData, interfaceType), 32*(int)sizeof(WCHAR), (uint8_t)FT::WString);
        addField((std::string(pfx)+"temperature").c_str(), base + offsetof(PhysicalDiskSmartData, temperature), 8);
        addField((std::string(pfx)+"health").c_str(),      base + offsetof(PhysicalDiskSmartData, healthPercentage), 1, (uint8_t)FT::UInt8);
        addField((std::string(pfx)+"smartSupported").c_str(), base + offsetof(PhysicalDiskSmartData, smartSupported), 1, (uint8_t)FT::Bool);
        // logical drive letters (up to 8 letters, stored as individual bytes + count)
        for (int j = 0; j < 8; j++) {
            char fn[64]; snprintf(fn, sizeof(fn), "%sletter%d", pfx, j);
            addField(fn, base + offsetof(PhysicalDiskSmartData, logicalDriveLetters) + j, 1, (uint8_t)FT::UInt8);
        }
        addField((std::string(pfx)+"letterCount").c_str(), base + offsetof(PhysicalDiskSmartData, logicalDriveCount), 4, (uint8_t)FT::Int32);
    }

    // WiFi
    addField("wifi/ssid",     offsetof(SharedMemoryBlock, wifi.ssid), 32*(int)sizeof(WCHAR), (uint8_t)FT::WString);
    addField("wifi/rssi",     offsetof(SharedMemoryBlock, wifi.rssi), 4, (uint8_t)FT::Int32);
    addField("wifi/channel",  offsetof(SharedMemoryBlock, wifi.channel), 4, (uint8_t)FT::Int32);
    addField("wifi/security", offsetof(SharedMemoryBlock, wifi.security), 16*(int)sizeof(WCHAR), (uint8_t)FT::WString);
    addField("wifi/band",     offsetof(SharedMemoryBlock, wifi.band), 8*(int)sizeof(WCHAR), (uint8_t)FT::WString);
    addField("wifi/gen",      offsetof(SharedMemoryBlock, wifi.wifiGen), 12*(int)sizeof(WCHAR), (uint8_t)FT::WString);
    addField("wifi/powerOn",  offsetof(SharedMemoryBlock, wifi.powerOn), 1, (uint8_t)FT::Bool);
    addField("wifi/isConnected", offsetof(SharedMemoryBlock, wifi.isConnected), 1, (uint8_t)FT::Bool);
    // Bluetooth
    addField("bluetooth/powerOn",     offsetof(SharedMemoryBlock, bluetooth.powerOn), 1, (uint8_t)FT::Bool);
    addField("bluetooth/deviceCount", offsetof(SharedMemoryBlock, bluetooth.deviceCount), 4, (uint8_t)FT::Int32);
    addField("bluetooth/name",        offsetof(SharedMemoryBlock, bluetooth.name), 64*(int)sizeof(WCHAR), (uint8_t)FT::WString);
}

int main(int argc, char* argv[]) {
    _set_se_translator(SEHTranslator);
    
    std::set_new_handler([]() {
        Logger::Fatal("Memory allocation failed - system out of memory");
        throw std::bad_alloc();
    });
    
    SetConsoleCP(65001);
    SetConsoleOutputCP(65001);
    
    setlocale(LC_ALL, "en_US.UTF-8");
    
    SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
    
    try {
        try {
            Logger::EnableConsoleOutput(true);
            Logger::Initialize("system_monitor.log");
            Logger::SetLogLevel(LOG_INFO);
            Logger::Info("Program started");
        }
        catch (const std::exception& e) {
            printf("Logging system initialization failed: %s\n", e.what());
            return 1;
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


            // Build JSON via ConfigManager, then dump to stdout via temp file
            char tmpBuf[MAX_PATH];
            GetTempPathA(MAX_PATH, tmpBuf);
            std::string tmpPath = std::string(tmpBuf) + "tcmt_export.json";
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
                GpuInfo gpuInfo(*wmiManager);
                const auto& gpus = gpuInfo.GetGpuData();
                if (!gpus.empty()) {
                    cfg.SetString("gpu.name", WinUtils::WstringToString(gpus[0].name));
                    cfg.SetUint64("gpu.dedicatedMemory", gpus[0].dedicatedMemory);
                    cfg.SetDouble("gpu.usage", gpus[0].usage);
                }
            } catch (...) {}

            // Network
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
            CoUninitialize();
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
#ifdef _WIN32
            _setmode(_fileno(stdout), _O_BINARY);
            _setmode(_fileno(stdin), _O_BINARY);
#endif
            tcmt::mcp::MCPServer server;

            // Try IPC client first — like Avalonia, reuse running TCMT instance
            tcmt::ipc::IPCClient ipc;
            bool useIpc = ipc.Connect();
            if (useIpc) {
                Logger::Info("MCP: connected to running TCMT via IPC");
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
                    j["usage"] = ipc.ReadFloat64("gpu/0/usage").value_or(0.0);
                    j["memory"] = ipc.ReadUInt64("gpu/0/memory").value_or(0);
                } else {
                    // GPU fallback requires WmiManager — skip on Windows
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

        if (!IsRunAsAdmin()) {
            wchar_t szPath[MAX_PATH];
            GetModuleFileNameW(NULL, szPath, MAX_PATH);

            SHELLEXECUTEINFOW sei = { sizeof(sei) };
            sei.lpVerb = L"runas";
            sei.lpFile = szPath;
            sei.hwnd = NULL;
            sei.nShow = SW_NORMAL;

            if (ShellExecuteExW(&sei)) {
                exit(0);
            } else {
                MessageBoxW(NULL, L"Auto elevation failed, please right-click and run as administrator.", L"Insufficient Privileges", MB_OK | MB_ICONERROR);
                SafeExit(1);
            }
        }

        try {
            HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            if (FAILED(hr)) {
                if (hr == RPC_E_CHANGED_MODE) {
                    Logger::Warn("COM initialization mode conflict: thread already initialized in different mode, trying single-threaded mode");
                    hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                    if (FAILED(hr)) {
                        Logger::Error("COM initialization failed: 0x" + std::to_string(hr));
                        return -1;
                    }
                }
                else {
                    Logger::Error("COM initialization failed: 0x" + std::to_string(hr));
                    return -1;
                }
            }
            g_comInitialized = true;
            Logger::Debug("COM initialized successfully");
        }
        catch (const std::exception& e) {
            Logger::Error("Exception during COM initialization: " + std::string(e.what()));
            return -1;
        }

#ifdef _WIN32
        std::unique_ptr<tcmt::ipc::IPCServer> ipcServer;
#endif
        try {
            if (!SharedMemoryManager::InitSharedMemory()) {
                std::string error = SharedMemoryManager::GetLastError();
                Logger::Error("Shared memory initialization failed: " + error);
                
                Logger::Info("Attempting to reinitialize shared memory...");
                std::this_thread::sleep_for(std::chrono::milliseconds(1000));
                
                if (!SharedMemoryManager::InitSharedMemory()) {
                    Logger::Critical("Shared memory reinitialization failed, program cannot continue");
                    SafeExit(1);
                }
            }
            Logger::Info("Shared memory initialized successfully");

            // IPC server — sends schema to C# Avalonia (Named Pipe on Windows, UDS on macOS)
#ifdef _WIN32
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
#endif
        }
        catch (const std::exception& e) {
            Logger::Error("Exception during shared memory initialization: " + std::string(e.what()));
            SafeExit(1);
        }

        // Create and initialize WMI manager - enhanced memory allocation exception handling
        std::unique_ptr<WmiManager> wmiManager;
        try {
            wmiManager = std::make_unique<WmiManager>();
            if (!wmiManager) {
                Logger::Fatal("WMI manager object creation failed - memory allocation returned null");
                SafeExit(1);
            }
            if (!wmiManager->IsInitialized()) {
                Logger::Error("WMI initialization failed");
                MessageBoxA(NULL, "WMI initialization failed, cannot retrieve system information.", "Error", MB_OK | MB_ICONERROR);
                SafeExit(1);
            }
            Logger::Debug("WMI manager initialized successfully");
        }
        catch (const std::bad_alloc& e) {
            Logger::Fatal("WMI manager creation failed - memory allocation failed: " + std::string(e.what()));
            SafeExit(1);
        }
        catch (const std::exception& e) {
            Logger::Error("WMI manager creation failed: " + std::string(e.what()));
            SafeExit(1);
        }
        catch (...) {
            Logger::Fatal("WMI manager creation failed - unknown exception");
            SafeExit(1);
        }

        // Initialize hardware monitoring bridge (LHM for temperature sensors)
        try {
            TemperatureWrapper::Initialize();
            Logger::Info("Hardware monitoring bridge (LHM) initialized");
        }
        catch (const std::exception& e) {
            Logger::Error("Hardware monitoring bridge initialization failed: " + std::string(e.what()));
            // Do not exit program, continue running but temperature data may not be available
        }

        Logger::Info("Program startup complete");
        
        // Start TUI (Windows version)
        tcmt::TuiApp tuiApp;
        tuiApp.SetLogBuffer(&Logger::GetTuiBuffer());
        tuiApp.Start();
        Logger::Info("TUI started");

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

        // History logger (SQLite)
        HistoryLogger historyLogger;
        historyLogger.SetRetentionDays(30);
        {
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
            if (historyLogger.Initialize(dbPath))
                Logger::Info("HistoryLogger: " + dbPath);
            else
                Logger::Warn("HistoryLogger failed to initialize");
        }

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

        while (!g_shouldExit.load()) {
            static DeviceChangeNotifier s_usbNotify(DeviceChangeNotifier::USB);
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
                sysInfo.compressedMemory = 0; // Windows: no compressed memory

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
                    sysInfo.gpuIsVirtual = cachedGpuIsVirtual;
                    sysInfo.gpuUsage = cachedGpuUsage;

                    // Override with NVML real-time data (NVIDIA GPUs only)
                    double nvmlUsage  = GpuInfo::GetGpuUsage();
                    double nvmlTemp   = GpuInfo::GetGpuTemperature();
                    double nvmlVramPct = GpuInfo::GetVramUsagePercent();
                    if (nvmlUsage  >= 0) sysInfo.gpuUsage = nvmlUsage;
                    if (nvmlTemp   >= 0) sysInfo.gpuTemperature = nvmlTemp;
                    if (nvmlVramPct >= 0) sysInfo.gpuCoreFreq = nvmlVramPct; // VRAM % via coreClock slot

                    // Fix GPU array population - add data validation and cleanup
                    sysInfo.gpus.clear();
                    if (!cachedGpuName.empty() && cachedGpuName != "No GPU detected") {
                        GPUData gpu;
                        
                        // Initialize GPU struct to avoid garbage data
                        memset(&gpu, 0, sizeof(GPUData));
                        
                        // Safely copy GPU name and brand to wchar_t arrays
                        std::wstring gpuNameW = WinUtils::StringToWstring(cachedGpuName);
                        std::wstring gpuBrandW = WinUtils::StringToWstring(cachedGpuBrand);
                        
                        // Limit string length to prevent buffer overflow
                        if (gpuNameW.length() >= sizeof(gpu.name)/sizeof(wchar_t)) {
                            gpuNameW = gpuNameW.substr(0, sizeof(gpu.name)/sizeof(wchar_t) - 1);
                        }
                        if (gpuBrandW.length() >= sizeof(gpu.brand)/sizeof(wchar_t)) {
                            gpuBrandW = gpuBrandW.substr(0, sizeof(gpu.brand)/sizeof(wchar_t) - 1);
                        }
                        
                        wcsncpy_s(gpu.name, sizeof(gpu.name)/sizeof(wchar_t), gpuNameW.c_str(), _TRUNCATE);
                        wcsncpy_s(gpu.brand, sizeof(gpu.brand)/sizeof(wchar_t), gpuBrandW.c_str(), _TRUNCATE);
                        
                        // Validate and clean GPU data - avoid abnormal values
                        gpu.memory = (cachedGpuMemory > 0 && cachedGpuMemory < UINT64_MAX) ? cachedGpuMemory : 0;
                        
                        // Fix GPU core clock - ensure it's in reasonable range
                        if (cachedGpuCoreFreq > 0 && cachedGpuCoreFreq < 10000) {
                            gpu.coreClock = cachedGpuCoreFreq;
                        } else {
                            gpu.coreClock = 0; // Set to 0 instead of abnormal value
                            if (isFirstRun && cachedGpuCoreFreq > 10000) {
                                Logger::Warn("GPU core clock abnormal: " + std::to_string(cachedGpuCoreFreq) + "MHz, reset to 0");
                            }
                        }
                        
                        gpu.isVirtual = cachedGpuIsVirtual;
                        gpu.usage = cachedGpuUsage;  // GPU usage
                        
                        sysInfo.gpus.push_back(gpu);
                        
                        if (isFirstRun) {
                            Logger::Debug("Added GPU to array: " + cachedGpuName + 
                                         " (Memory: " + FormatSize(cachedGpuMemory) + 
                                         ", Clock: " + std::to_string(gpu.coreClock) + "MHz" +
                                         ", Virtual: " + (cachedGpuIsVirtual ? "Yes" : "No") + ")");
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
                    // Collect physical disks (cached — WMI is slow, re-query every 60s)
                    static std::vector<PhysicalDiskSmartData> cachedPhysDisks;
                    static auto lastPhysQuery = std::chrono::steady_clock::now() - std::chrono::seconds(61);
                    auto now = std::chrono::steady_clock::now();
                    if (std::chrono::duration_cast<std::chrono::seconds>(now - lastPhysQuery).count() >= 60) {
                        if (wmiManager) {
                            DiskInfo::CollectPhysicalDisks(*wmiManager, sysInfo.disks, sysInfo);
                            if (!sysInfo.physicalDisks.empty())
                                cachedPhysDisks = sysInfo.physicalDisks;
                            lastPhysQuery = now; // always update — avoid retry flood on empty result
                        }
                    }
                    if (!cachedPhysDisks.empty())
                        sysInfo.physicalDisks = cachedPhysDisks;

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

                // Write to shared memory - enhanced exception handling
                try {
                    // Shared memory write moved after TUI block (WiFi/BT data)
                    if (false && SharedMemoryManager::GetBuffer()) {
                        SharedMemoryManager::WriteToSharedMemory(sysInfo);
                        if (isDetailedLogging) {
                            Logger::Debug("Successfully updated shared memory");
                        }
                    } else if (false) {
                        Logger::Error("Shared memory buffer unavailable");
                        if (SharedMemoryManager::InitSharedMemory()) {
                            SharedMemoryManager::WriteToSharedMemory(sysInfo);
                            if (isDetailedLogging) {
                                Logger::Info("Reinitialized and updated shared memory");
                            }
                        } else {
                            Logger::Error("Failed to reinitialize shared memory: " + SharedMemoryManager::GetLastError());
                        }
                    }
                    
                    if (isDetailedLogging) {
                        Logger::Debug("System info updated to shared memory");
                    }
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
                    tuiData.cpuName = sysInfo.cpuName;
                    tuiData.cpuUsage = sysInfo.cpuUsage;
                    tuiData.physicalCores = sysInfo.physicalCores;
                    tuiData.performanceCores = sysInfo.performanceCores;
                    tuiData.efficiencyCores = sysInfo.efficiencyCores;
                    tuiData.pCoreFreq = sysInfo.performanceCoreFreq;
                    tuiData.eCoreFreq = sysInfo.efficiencyCoreFreq;
                    tuiData.cpuBaseFreq = sysInfo.cpuBaseFreq;
                    tuiData.cpuTemp = sysInfo.cpuTemperature;
                    tuiData.totalMemory = sysInfo.totalMemory;
                    tuiData.usedMemory = sysInfo.usedMemory;
                    tuiData.availableMemory = sysInfo.availableMemory;
                    
                    if (!sysInfo.gpus.empty()) {
                        tuiData.gpuName = sysInfo.gpuName;
                        tuiData.gpuMemory = sysInfo.gpuMemory;
                        tuiData.gpuUsage = sysInfo.gpuUsage;
                        tuiData.gpuMemoryPercent = sysInfo.gpuCoreFreq; // NVML VRAM % (set above)
                    }
                    tuiData.gpuTemp = sysInfo.gpuTemperature;
                    
                    // Disks
                    for (const auto& disk : sysInfo.disks) {
                        tcmt::TuiData::DiskInfo di;
                        di.letter = disk.letter;
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
                    tuiData.connectionCount = ipcServer ? ipcServer->GetClientCount() : 0;
                    if (ipcServer) {
                        auto ct = ipcServer->GetClientTypes();
                        tuiData.clientTypes.clear();
                        for (auto t : ct) tuiData.clientTypes.push_back(static_cast<uint8_t>(t));
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
                      tuiData.wifiSSID = wd.ssid;
                      tuiData.wifiRSSI = wd.rssi;
                      tuiData.wifiChannel = wd.channel;
                      tuiData.wifiSecurity = wd.security;
                      tuiData.wifiBand = wd.band;
                      tuiData.wifiGen = wd.wifiGen;
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
                      tuiData.btDeviceCount = static_cast<int>(bd.devices.size());
                      sysInfo.btPowerOn = bd.adapter.powerOn;
                      sysInfo.btDeviceCount = static_cast<int>(bd.devices.size());

                      // Write WiFi & Bluetooth to shared memory block
                      if (auto* buf = SharedMemoryManager::GetBuffer()) {
                          memset(&buf->wifi, 0, sizeof(buf->wifi));
                          buf->wifi.powerOn = wd.powerOn;
                          buf->wifi.isConnected = wd.isConnected;
                          buf->wifi.rssi = wd.rssi;
                          buf->wifi.channel = wd.channel;
                          wcsncpy_s(buf->wifi.ssid, 32, WinUtils::StringToWstring(wd.ssid).c_str(), _TRUNCATE);
                          wcsncpy_s(buf->wifi.security, 16, WinUtils::StringToWstring(wd.security).c_str(), _TRUNCATE);

                          memset(&buf->bluetooth, 0, sizeof(buf->bluetooth));
                          buf->bluetooth.powerOn = bd.adapter.powerOn;
                          buf->bluetooth.deviceCount = static_cast<int32_t>(bd.devices.size());
                          wcsncpy_s(buf->bluetooth.name, 64, WinUtils::StringToWstring(bd.adapter.name).c_str(), _TRUNCATE);
                      }
                    }

                    tuiData.timestamp = FormatDateTime(std::chrono::system_clock::now());

                    tuiApp.UpdateData(tuiData);

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
                    
                    // 1 second cycle time
                    int targetCycleTime = 1000;
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
        
        Logger::Info("Program received exit signal, starting cleanup");
        
        // Stop TUI before cleanup
        try {
            tuiApp.Stop();
            Logger::Debug("TUI stopped");
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


