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
// pid_t: POSIX on macOS/Linux, need explicit definition on Windows
#ifdef _WIN32
typedef int pid_t;
#endif
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
    double pCoreMaxFreq = 0.0;
    double eCoreMaxFreq = 0.0;
    double cpuBaseFreq = 0.0;
    double gpuFreq = 0.0;
    double gpuMaxFreq = 0.0;
    double cpuTemp = 0.0;
    double cpuPcoreTemp = 0.0;
    double cpuEcoreTemp = 0.0;

    // Memory
    uint64_t totalMemory = 0;
    uint64_t usedMemory = 0;
    uint64_t availableMemory = 0;
    uint64_t compressedMemory = 0;
    uint64_t swapUsed = 0;
    uint64_t swapTotal = 0;
    uint32_t ramSpeed = 0;
    char ramType[32] = {};

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

    struct SmAttributeInfo {
        uint8_t id = 0;
        uint8_t current = 0;
        uint8_t worst = 0;
        uint64_t rawValue = 0;
        std::string name;
    };

    struct PhysicalDiskInfo {
        std::string model;
        std::string serial;
        std::string interfaceType;
        std::string diskType;    // "SSD" / "HDD"
        uint64_t capacity = 0;
        double temperature = 0;
        uint8_t healthPct = 0;
        bool smartSupported = false;
        uint64_t powerOnHours = 0;
        double wearLeveling = 0;
        std::vector<SmAttributeInfo> attributes;
    };
    std::vector<PhysicalDiskInfo> physicalDisks;

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
    std::string hardwareModel;
    int batteryPercent = -1;  // -1 = no battery
    bool acOnline = false;

    // Battery health (macOS AppleSmartBattery)
    int batteryCycleCount = 0;
    int batteryDesignCapacity = 0;   // mAh
    int batteryMaxCapacity = 0;      // mAh (actual max, may be degraded)
    double batteryHealthPercent = 0.0;
    double batteryTemp = 0.0;        // Celsius
    int batteryAmperage = 0;         // mA (+=charging, -=discharging)
    int batteryVoltage = 0;          // mV
    double chargerWatts = 0.0;       // connected charger rated wattage
    bool batteryIsCharging = false;

    double cpuPower = 0.0;
    double gpuPower = 0.0;
    double anePower = 0.0;

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
    std::string wifiBand;
    std::string wifiGen;
    double wifiTxRate = 0;
    bool wifiLocationDenied = false; // macOS 15+: SSID blocked by Location Services
    // Bluetooth (optional)
    bool hasBluetooth = false;
    bool btPowerOn = false;
    int btDeviceCount = 0;

    // Display monitors
    struct DisplayInfo {
        std::string name;
        int width = 0;
        int height = 0;
        int refreshRate = 0;    // Hz
        bool isHDR = false;
        bool isBuiltin = false;
        double backingScale = 1.0;
    };
    std::vector<DisplayInfo> displays;

    // Thermal state (macOS NSProcessInfoThermalState)
    int thermalState = 0;       // 0=nominal, 1=fairlySerious, 2=critical

    // System uptime / load / process
    uint64_t uptimeSeconds = 0;
    double loadAvg1 = 0.0;
    double loadAvg5 = 0.0;
    double loadAvg15 = 0.0;
    int processCount = 0;

    // ALS (Ambient Light Sensor)
    struct AlsChannelInfo {
        bool valid = false;
        uint32_t r = 0, g = 0, b = 0;   // RGB raw counts
    };
    bool alsValid = false;
    double alsLux = 0.0;
    AlsChannelInfo alsChannels;

    // Accelerometer / Attitude (BMI284 DMP gravity vector, 0xFF00/3)
    struct AccelInfo {
        bool hasDevice = false;    // BMI284 present on this machine
        bool valid = false;        // current sample valid
        double x = 0.0;           // X axis (g)
        double y = 0.0;           // Y axis (g)
        double z = 0.0;           // Z axis (g, ~1g at rest)
    };
    AccelInfo accel;

    // Gyroscope (BMI284 DMP angular velocity, 0xFF00/9)
    struct GyroInfo {
        bool valid = false;
        double x = 0.0, y = 0.0, z = 0.0; // deg/s
    };
    GyroInfo gyro;

    // Lid angle (hinge angle detection, 0x0020/138)
    struct LidInfo {
        bool valid = false;
        double angle = 0.0; // degrees (0–360)
    };
    LidInfo lidAngle;

    // SPU Temperature (BMI284 die / IMU ambient, 0xFF00/5)
    struct SpuTempInfo {
        bool valid = false;
        double celsius = 0.0;
    };
    SpuTempInfo spuTemp;

    // AppleVendorMotion_Motion heartbeat (0xFF0C/1)
    struct MotionHeartbeatInfo {
        bool valid = false;
        uint8_t counter = 0;   // report[4] — monotonic counter, increments ~every 2.5s
        uint8_t eventFlag = 0; // report[0] — 0x03=pair start, 0x02=pair end
    };
    MotionHeartbeatInfo motionHb;

    // AppleVendorMotion_DeviceMotion6 fusion (0xFF0C/5, needs CoreMotion)
    struct DeviceMotionInfo {
        bool valid = false;
        double raw = 0.0; // first 4 bytes as int32 (placeholder until format known)
    };
    DeviceMotionInfo deviceMotion;

    // Top processes (by memory, top ~7)
    struct ProcessTopEntry {
        pid_t pid = 0;
        std::string name;
        uint64_t memoryBytes = 0;
        double cpuPercent = 0.0;
    };
    std::vector<ProcessTopEntry> topProcesses;

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
    int DrawPhysicalDiskPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW);
    int DrawWifiBluetoothPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW);
    int DrawDisplayPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW);
    int DrawPowerPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW);
    int DrawAccelPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW);
    int DrawProcessPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW);

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
