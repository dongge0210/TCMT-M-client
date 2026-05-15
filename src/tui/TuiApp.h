// TuiApp.h — ncurses/PDCurses Text User Interface for TCMT
//
// The TUI is the PRIMARY user interface and runs as part of the C++ core process.
// It reads hardware data directly via the same data structures the main loop
// populates (TuiData / SystemInfo), NOT through IPC. This means:
//
//   - No IPC overhead: the TUI shares the process address space with the data
//     collection loop. It renders from memory, not from a serialized protocol.
//   - No dependency on a running GUI or schema pipeline: the TUI works even if
//     IPCServer, Avalonia, or MCP are unavailable.
//   - Ideal for headless/SSH/tmux environments where a graphical desktop or
//     .NET runtime is not present.
//
// The TUI is NOT a "fallback" — it is the authoritative console interface for
// the hardware monitor. Avalonia and MCP are IPC clients that connect to the
// same core process and read from shared memory.
//
#pragma once

#include "LogBuffer.h"
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>
#include <chrono>

// Forward declarations for ncurses (skip if PDCurses already included)
#ifndef __PDCURSES__
struct _win_st;
typedef struct _win_st WINDOW;
#endif

namespace tcmt {

// Data snapshot for TUI rendering (filled by main thread)
struct TuiData {
    // CPU
    std::string cpuName;
    double cpuUsage = 0.0;
    int physicalCores = 0;
    int performanceCores = 0;
    int efficiencyCores = 0;
    double pCoreFreq = 0.0;
    double eCoreFreq = 0.0;
    double cpuTemp = 0.0;

    // Memory
    uint64_t totalMemory = 0;
    uint64_t usedMemory = 0;
    uint64_t availableMemory = 0;
    uint64_t compressedMemory = 0;

    // GPU
    std::string gpuName;
    uint64_t gpuMemory = 0;
    double gpuUsage = 0.0;
    double gpuMemoryPercent = 0.0;
    double gpuTemp = 0.0;

    // Disk
    struct DiskInfo {
        char letter = 0;        // Drive letter (e.g. 'C')
        std::string label;
        uint64_t totalSize = 0;
        uint64_t usedSpace = 0;
        std::string fileSystem;
    };
    std::vector<DiskInfo> disks;

    // Network
    struct NetInfo {
        std::string name;
        std::string ip;
        std::string mac;
        std::string type;
        uint64_t speed = 0;
        uint64_t downloadSpeed = 0;
        uint64_t uploadSpeed = 0;
    };
    std::vector<NetInfo> adapters;

    // OS
    std::string osVersion;
    int batteryPercent = -1;  // -1 = no battery
    bool acOnline = false;

    // Connections
    int connectionCount = 0;
    std::string connectionSince;
    std::vector<uint8_t> clientTypes;  // ClientType values per connection

    // TPM
    std::string tpmInfo;

    // Temperatures
    std::vector<std::pair<std::string, double>> temperatures;

    // WiFi (optional — only if WiFiInfo::Detect() was called)
    bool hasWiFi = false;
    std::string wifiSSID;
    std::string wifiBSSID;
    int wifiRSSI = 0;
    int wifiChannel = 0;
    std::string wifiSecurity;
    double wifiTxRate = 0;
    // Bluetooth (optional)
    bool hasBluetooth = false;
    bool btPowerOn = false;
    int btDeviceCount = 0;

    // Timestamp
    std::string timestamp;
};

class TuiApp {
public:
    TuiApp();
    ~TuiApp();

    // Start/stop the TUI (runs in its own thread)
    void Start();
    void Stop();
    bool IsRunning() const;

    // Update data from main thread (thread-safe)
    void UpdateData(const TuiData& data);

    // Get the log buffer for Logger to write into
    LogBuffer& GetLogBuffer();

    // Inject external log buffer (e.g. from Logger)
    void SetLogBuffer(LogBuffer* buf);

private:
    void Run();
    void SafeEndwin();
    void InitColors();
    void DrawHeader(WINDOW* win, const TuiData& data);
    int DrawCpuPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW);
    int DrawMemoryPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW);
    int DrawGpuPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW);
    int DrawDiskPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW);
    int DrawNetworkPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW);
    int DrawTpmPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW);
    int DrawTempPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW);
    int DrawWifiBluetoothPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW);

    // Utility
    static std::string FormatSize(uint64_t bytes);
    static std::string FormatSpeed(uint64_t bps);
    static std::string FormatBar(double pct, int width);
    static std::string TrimRight(const std::string& s, size_t maxLen);

    std::thread thread_;
    std::atomic<bool> running_{false};

    TuiData data_;
    mutable std::mutex dataMutex_;

    // Internal buffer (fallback), or use external via SetLogBuffer()
    LogBuffer defaultBuffer_;
    // Points to either &defaultBuffer_ or an external buffer
    LogBuffer* logBuf_ = nullptr;

    // Window dimensions
    int termRows_ = 0;
    int termCols_ = 0;

    // Ensure endwin() is called exactly once
    std::atomic<bool> cursesActive_{false};
};

} // namespace tcmt
