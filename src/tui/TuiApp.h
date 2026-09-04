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
#include <functional>
#include <string>
// pid_t: POSIX on macOS/Linux, need explicit definition on Windows
#ifdef _WIN32
typedef int pid_t;
#endif
#include <vector>
#include <mutex>
#include <atomic>
#include <thread>

// Forward declarations for ncurses (skip if PDCurses already included)
#ifndef __PDCURSES__
struct _win_st;
typedef struct _win_st WINDOW;
#endif

namespace tcmt {

// Server push settings, editable from the TUI settings page (press S).
// The TUI renders these; on save it invokes the handler with the edited
// values, and main applies them (persist + restart the probe) and reports
// the resulting state back via `status`.
struct ServerSettings {
    bool enabled = false;
    std::string url = "http://127.0.0.1:8080";
    bool insecure = false;
    int intervalSec = 2;       // upload cadence, hard bounds 1..60
    std::string status = "";   // probe state reported by main ("running" etc.)
};

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
    int gpuFanSpeed = -1;            // kept for compat, see gpuFans
    struct GpuFanInfo {
        unsigned int index = 0;
        int speedRpm = 0;
        bool isRpm = false;
    };
    std::vector<GpuFanInfo> gpuFans;

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
    int httpClientCount = 0;           // local motion HTTP server clients (macOS)

    // Server push (tcmt-server upload; filled by the monitor loop)
    bool serverPushEnabled = false;
    std::string serverUrl;
    std::string serverStatus;   // "running" / "disabled" / "error: ..."
    int64_t lastPushMs = 0;     // last successful snapshot post (0 = never)

    // Self-update (filled by the monitor loop)
    std::string updateStatus;
    int updateState = 0;   // 0 idle 1 checking 2 available 3 downloading 4 verifying 5 ready 6 failed

    // TPM
    std::string tpmInfo;

    // Temperatures
    std::vector<std::pair<std::string, double>> temperatures;

    // WiFi (optional — only if WiFiInfo::Detect() was called)
    bool hasWiFi = false;
    bool wifiConnected = false;   // adapter on + associated (vs. merely present)
    std::string wifiSSID;
    std::string wifiBSSID;
    int wifiRSSI = 0;
    int wifiChannel = 0;
    std::string wifiSecurity;
    std::string wifiBand;
    std::string wifiGen;
    double wifiTxRate = 0;
    bool wifiLocationDenied = false; // macOS 15+: SSID blocked by Location Services
    int wifiLocationStatus = 0;      // 0=not determined, 1=denied, 2=authorized
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

    // Per-core sensor data (up to 16 cores)
    float perCoreTemp[16] = {};
    float perCoreFreq[16] = {};
    uint8_t perCoreCount = 0;

    // Network traffic sparkline history (last 40 samples)
    static constexpr int NET_HISTORY_MAX = 40;
    uint64_t dlHistory[NET_HISTORY_MAX] = {};
    uint64_t ulHistory[NET_HISTORY_MAX] = {};
    int dlHistoryPos = 0;
    int dlHistoryLen = 0;

    // Timestamp
    std::string timestamp;
};

// Capability report — the single place the TUI answers "what does this
// build actually support", so key bindings, page availability and guidance
// text are runtime data instead of #ifdefs sprinkled through the render
// code. Each platform builds its report once (DetectPlatformCaps in
// TuiApp.cpp); everything else reads these flags.
struct PlatformCaps {
    bool inlineLogPage = false;        // in-TUI log page (L/Tab). Windows pairs the dashboard with its own window instead.
    bool nativeLogWindow = false;      // OS-native log window exists (Win32 LogWindow / AppKit MacLogWindow).
    bool resizableTerminal = false;    // terminal reports live resize events (PDCurses is_termresized).
    bool wifiLocationServices = false; // macOS Location Services SSID flow (R key + guidance text).
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

    // Optional handler invoked when the user presses R on the dashboard
    // (e.g. request macOS Location Services for WiFi SSID).
    void SetLocationRequestHandler(std::function<void()> handler) {
        locationRequestHandler_ = std::move(handler);
    }

    // Optional handler invoked when the user presses U (start the update
    // download) — main wires this to the Updater.
    void SetUpdateRequestHandler(std::function<void()> h) {
        updateRequestHandler_ = std::move(h);
    }

    // Server push settings shown on the settings page (press S).
    void SetServerSettings(const ServerSettings& s) {
        std::lock_guard<std::mutex> lock(dataMutex_);
        serverSettings_ = s;
    }
    // Invoked on the TUI thread when the user saves the settings page.
    void SetServerSettingsHandler(std::function<void(const ServerSettings&)> h) {
        settingsHandler_ = std::move(h);
    }

    // Inject external log buffer (e.g. from Logger) for the in-TUI log page.
    // Inert on platforms without the inline page (caps_.inlineLogPage).
    void SetLogBuffer(LogBuffer* buf);

private:
    void Run();
    void RenderSettingsPage(int rows, int cols, int ch);
    void RenderLogPage(int rows, int cols, int ch);
    void RenderHelpPage(int rows, int cols, int ch);
    void RenderProcessDetails(int rows, int cols, int ch);
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
    int DrawCorePanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW);
    int DrawNetGraphPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW);

    // Utility
    static std::string FormatSize(uint64_t bytes);      // storage: binary (1024), "MB" = MiB
    static std::string FormatSpeed(uint64_t bps);       // link speed: decimal (1000), "Mbps"
    static std::string FormatRate(uint64_t bytesPerSec); // throughput: decimal (1000), "MB/s"
    static std::string TrimRight(const std::string& s, size_t maxLen);

    // Row layout helpers (#6): every value line on a panel shares one
    // right-aligned value column (label left), and usage bars draw a
    // colored fill over a dim track instead of same-colored '='/'-'.
    void DrawUsageBarRow(WINDOW* win, int y, int x0, int maxW,
                         const char* label, double pct, const std::string& value);
    void DrawLabeledValue(WINDOW* win, int y, int x0, int maxW,
                          const char* label, const std::string& value, int pair = -1);

    std::thread thread_;
    std::atomic<bool> running_{false};

    // Capabilities of this platform/build (see DetectPlatformCaps).
    PlatformCaps caps_;

    // Page state: Dashboard (hardware panels) or Log (scrolling log page)
    bool logPage_ = false;
    int logScrollOffset_ = 0;   // lines scrolled up from bottom
    bool logFollow_ = true;     // auto-follow newest lines
    bool logHomePending_ = false;  // HOME pressed; park at oldest on next draw
    uint64_t lastLogVer_ = 0;      // LogBuffer version at last log-page paint
    int lastLogRows_ = 0;          // terminal size at last log-page paint
    int lastLogCols_ = 0;

    // Internal buffer (fallback), or use external via SetLogBuffer()
    LogBuffer defaultBuffer_;
    // Points to either &defaultBuffer_ or an external buffer
    LogBuffer* logBuf_ = nullptr;

    // Downgrade switches (detected once in Run, then fixed for the session):
    // NO_COLOR skips color initialization entirely; TCMT_ASCII=1 (or a
    // non-UTF-8 locale) swaps Unicode glyphs for ASCII ones.
    bool noColor_ = false;
    bool asciiMode_ = false;
    std::string sparkChars_ = " \xe2\x96\x81\xe2\x96\x82\xe2\x96\x83\xe2\x96\x84"
                              "\xe2\x96\x85\xe2\x96\x86\xe2\x96\x87\xe2\x96\x88"; // ▁..█, index 0 = empty
    std::string degSuffixTemp_ = "\xc2\xb0";  // suffix after a core-temp value (° / C)
    std::string degSuffixAngle_ = "\xc2\xb0"; // suffix after the lid angle (° / deg)
    std::string upArrow_ = "\xe2\x86\x91";    // ↑ (^ in ASCII mode)
    std::string downArrow_ = "\xe2\x86\x93";  // ↓ (v)

    TuiData data_;
    mutable std::mutex dataMutex_;

    std::function<void()> locationRequestHandler_;
    std::function<void()> updateRequestHandler_;

    // Freshness watchdog: microseconds (steady clock) of the last UpdateData
    // snapshot; the dashboard flags stale Ns when it stops advancing.
    std::atomic<int64_t> lastUpdateUs_{0};

    // Settings page state (press S): framed interactive form.
    ServerSettings serverSettings_;        // current values (from main)
    ServerSettings draftSettings_;         // edits in progress
    std::function<void(const ServerSettings&)> settingsHandler_;
    bool settingsPage_ = false;
    bool helpPage_ = false;                // ? key-reference overlay (modal, like settings)
    bool detailsPage_ = false;             // process-details overlay (Enter on a selected row)
    int selPid_ = -1;                      // selected process pid on the dashboard (-1 = none)
    int settingsFocus_ = 0;                // 0=enable, 1=url, 2=insecure
    int urlCursor_ = 0;                    // cursor position inside URL field

    // Window dimensions
    int termRows_ = 0;
    int termCols_ = 0;

    // Ensure endwin() is called exactly once
    std::atomic<bool> cursesActive_{false};
};

} // namespace tcmt
