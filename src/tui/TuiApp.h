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

#include "core/Utils/LogBuffer.h"
#include "core/I18n.h"
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

// Hardware sampling cadence (ms). These bounds mirror the "sampling.intervalMs"
// rule in ConfigManager — keep the two in sync.
inline constexpr int kSampleIntervalDefaultMs = 500;
inline constexpr int kSampleIntervalMinMs = 200;
inline constexpr int kSampleIntervalMaxMs = 5000;

// Server push settings, editable from the TUI settings page (press S).
// The TUI renders these; on save it invokes the handler with the edited
// values, and main applies them (persist + restart the probe) and reports
// the resulting state back via `status`.
struct ServerSettings {
    bool enabled = false;
    std::string url = "http://127.0.0.1:8080";
    bool insecure = false;
    int intervalSec = 2;       // upload cadence, hard bounds 1..60
    int sampleIntervalMs = kSampleIntervalDefaultMs; // hardware sampling cadence
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
    // Startup progress: collection units that have completed one pass.
    // 0 total means "do not show the indicator" (producers that do not
    // report readiness yet).
    uint8_t loadDone = 0;
    uint8_t loadTotal = 0;

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
    int ramSlotsTotal = 0;   // SMBIOS slot count; 0 = soldered / unknown
    int ramSlotsUsed = 0;    // installed modules

    // GPU
    std::string gpuName;
    uint64_t gpuMemory = 0;
    // gpuMemory 报的是统一内存而不是独立显存（Apple Silicon）
    bool gpuUnified = false;
    int gpuCoreCount = 0;          // Apple Silicon: gpu-core-count (0 = unknown)
    // Every detected GPU. Empty means the scalar gpu* fields above are the
    // only source (single-GPU platforms). The scalars always describe the
    // first GPU, because the IPC/JSON paths report that one.
    struct GpuEntry {
        std::string name;
        std::string driver;          // "Metal 4" / "560.94" (empty when unknown)
        double usage = 0.0;
        uint64_t memory = 0;         // dedicated VRAM (or unified total when unified)
        double memoryPercent = 0.0;
        bool unified = false;
        bool isVirtual = false;
        int coreCount = 0;
        // Live per-card readings. NVML fills every one on Windows NVIDIA;
        // macOS has temperature and clock only, the rest stay at zero.
        double temperature = 0.0;    // C
        double clockMhz = 0.0;       // current graphics clock
        int maxClock = 0;            // MHz ceiling
        int memClockMhz = 0;         // memory clock
        double powerMw = -1.0;       // current draw; -1 = unreadable
        double powerLimitMw = 0.0;   // enforced limit
        int fanPercent = -1;         // -1 when the card reports no fan
        int fanCount = 0;
        unsigned int pcieGen = 0;
        unsigned int pcieWidth = 0;
        unsigned int maxPcieGen = 0;
        unsigned int maxPcieWidth = 0;
        std::string serial;
        std::string vbios;
        std::string cudaVersion;     // CUDA UMD version, NVIDIA only
    };
    std::vector<GpuEntry> gpus;
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
        char letter = 0;        // Windows 盘符；macOS 恒为 0
        std::string mountPoint; // 平台中性的落点："C:\" / "/System/Volumes/Data"
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
        int index = -1;          // physical index (Windows) / diskN (macOS)
        std::vector<char> driveLetters;  // Windows: "C", "D" living on this disk
        uint64_t readBps = 0;    // live read throughput, bytes/s
        uint64_t writeBps = 0;   // live write throughput, bytes/s
        std::vector<SmAttributeInfo> attributes;
    };
    std::vector<PhysicalDiskInfo> physicalDisks;

    // Network
    struct NetInfo {
        std::string name;
        std::string ip;
        std::string mac;         // 硬件 MAC，不随网络变
        std::string currentMac;  // 当前生效的链路地址；macOS 上按 SSID 轮换
        std::string type;
        uint64_t speed = 0;
        uint64_t downloadSpeed = 0;
        uint64_t uploadSpeed = 0;
    };
    std::vector<NetInfo> adapters;

    // OS
    std::string osVersion;
    std::string hardwareModel;
    // Login identity (see core/Utils/UserIdentity.h): shown in the status
    // row so a sudo shell is visible at a glance instead of being inferred
    // from data that silently stopped working.
    std::string loginUser;
    bool processElevated = false;   // effective user is root / elevated
    bool elevatedViaSudo = false;   // root, but invoked by a normal user
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
    // AdapterDetails of the connected charger (empty/0 when unplugged).
    std::string chargerName;
    std::string chargerManufacturer;
    std::string chargerModel;
    std::string chargerSerial;
    int chargerVoltageMv = 0;        // negotiated voltage
    int chargerCurrentMa = 0;        // negotiated current
    bool chargerWireless = false;
    bool batteryIsCharging = false;
    int batteryTimeRemainingMin = 0;  // 0 = still estimating / unknown
    bool batteryFullyCharged = false;

    double cpuPower = 0.0;
    double gpuPower = 0.0;
    double anePower = 0.0;
    // Power availability (IOReport energy). Unprivileged processes cannot
    // read energy on macOS 27 (confirmed system limit); the root helper
    // tcmt-powerd supplies it via shared memory when installed. Filled by
    // ModuleCoordinator::Snapshot from PowerMonitor::IsPowerAvailable();
    // when false, cpuPower/gpuPower/anePower stay 0 and the UI renders N/A.
    bool powerAvailable = false;

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
    bool hasWiFi = false;         // a Wi-Fi adapter exists (radio state is wifiPowerOn)
    bool wifiPowerOn = false;     // radio on; false with hasWiFi means "switched off"
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
    bool wifiLocationRoot = false;   // macOS: running as root; Location can never be granted
    int wifiLocationStatus = 0;      // 0=not determined, 1=denied, 2=authorized
    std::string wifiChipset;         // "BCM4387" (macOS system_profiler card type)
    // Bluetooth (optional)
    bool hasBluetooth = false;
    bool btPowerOn = false;
    int btDeviceCount = 0;           // connected devices
    struct BtDeviceInfo {
        std::string name;
        std::string type;            // "Headphones" / "Mouse" / ...
        std::string battery;         // compact "L27 R100 C68" / "87%" / empty
        int rssi = 0;
        bool connected = false;
    };
    std::vector<BtDeviceInfo> btDevices;  // paired devices, connected first in input order
    std::string btChipset;                // "BCM_4387"
    std::string btFirmware;               // "24.1.640.5755"

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
    // Hardware sampling cadence shown on the dashboard header (ms); 0 means the
    // platform entry point does not report one (Windows/Linux today), in which
    // case the header omits the item rather than showing a made-up default.
    int sampleIntervalMs = 0;
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
        // Row context the list shows next to the process. Filled by the
        // collector for the top N only; unknown stays -1 / empty and the row
        // renders N/A.
        int threads = -1;
        uint64_t cpuTimeMs = 0;
        std::string state;
        std::string user;
    };
    std::vector<ProcessTopEntry> topProcesses;

    // Per-core sensor data (up to 16 cores)
    float perCoreTemp[16] = {};
    float perCoreFreq[16] = {};
    float perCoreUsage[16] = {};    // percent; < 0 = not sampled yet
    uint8_t perCoreIsE[16] = {};    // 1 = efficiency core, when the topology says so
    uint8_t perCoreCount = 0;       // cores with temperature/frequency data
    uint8_t perCoreUsageCount = 0;  // cores with usage data (macOS samples these alone)
    uint8_t perCoreFreqCount = 0;   // cores with frequency data (macOS per-core IOReport)
    bool perCorePEKnown = false;    // P/E split known (macOS: P first; Windows: perCoreIsE)

    // Network traffic sparkline history (last 40 samples)
    static constexpr int NET_HISTORY_MAX = 40;
    uint64_t dlHistory[NET_HISTORY_MAX] = {};
    uint64_t ulHistory[NET_HISTORY_MAX] = {};
    int dlHistoryPos = 0;
    int dlHistoryLen = 0;

    // One history ring per adapter, index-aligned with `adapters`. The
    // details page draws a column per wireless adapter; the single ring above
    // stays as the total/primary series and the fallback when this is empty.
    struct AdapterHistory {
        uint64_t dl[NET_HISTORY_MAX] = {};
        uint64_t ul[NET_HISTORY_MAX] = {};
        int pos = 0;
        int len = 0;
    };
    std::vector<AdapterHistory> adapterHistory;

    // One history ring per physical disk, index-aligned with `physicalDisks`.
    // The details page turns it into read/write charts; empty until the
    // second throughput sample.
    struct DiskHistory {
        uint64_t rd[NET_HISTORY_MAX] = {};
        uint64_t wr[NET_HISTORY_MAX] = {};
        int pos = 0;
        int len = 0;
    };
    std::vector<DiskHistory> diskHistory;

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

    // Hardware sampling interval (ms), shown on the settings page (press S).
    void SetSampleIntervalMs(int ms) {
        std::lock_guard<std::mutex> lock(dataMutex_);
        serverSettings_.sampleIntervalMs = ms;
    }
    // Invoked on the TUI thread when the interval is saved (Enter).
    void SetSampleIntervalHandler(std::function<void(int)> h) {
        sampleIntervalHandler_ = std::move(h);
    }

    // UI language. SetConfiguredLanguage seeds the startup choice from config:
    // TCMT_LANG still wins, then this, then the system locale. The handler fires
    // on the TUI thread when the settings page saves a new language, so main can
    // persist it (ui.language in system_monitor.json).
    void SetConfiguredLanguage(std::string lang) { configuredLang_ = std::move(lang); }
    void SetLanguageHandler(std::function<void(Lang)> h) {
        languageHandler_ = std::move(h);
    }

    // Inject external log buffer (e.g. from Logger) for the in-TUI log page.
    // Inert on platforms without the inline page (caps_.inlineLogPage).
    void SetLogBuffer(LogBuffer* buf);

private:
    void Run();
    void RenderSettingsPage(int rows, int cols, int ch);
    void RenderLogPage(int rows, int cols, int ch);
    void RenderHelpPage(int rows, int cols, int ch);
    void RenderConnectionsList(int rows, int cols, int ch);
    void RenderCpuDetails(int rows, int cols, int ch);
    void RenderGpuDetails(int rows, int cols, int ch);
    void RenderDiskDetails(int rows, int cols, int ch);
    void RenderBatteryDetails(int rows, int cols, int ch);
    void RenderNetworkDetails(int rows, int cols, int ch);
    // Shared rate chart (network adapter traffic, disk read/write): header,
    // value axis, block columns, time axis. Returns the row after the axis.
    int DrawRateChart(int cx, int cy, int colW, int rows, int bottom,
                      const char* label, const uint64_t* hist, int pos, int len,
                      uint64_t now, uint64_t scale, int pair, double colSec);
    // Full-screen splash while the first collection round is still landing.
    void RenderLoadingScreen(int rows, int cols, int done, int total);
    // Minimal mouse support: translate clicks/wheel into the equivalent
    // synthetic keys so every handler stays keyboard-driven (B direction).
    int MapMouseEvent(int rows, int cols);

    // Dashboard panels are focusable surfaces: Tab / Shift+Tab cycle the
    // panels actually drawn this frame, and a click inside a panel focuses
    // it. The focused panel's title chip is the active-surface cue.
    enum class PanelId {
        Cpu, Gpu, Memory, Process, Core,
        Disk, PhysicalDisk, Network, WifiBluetooth,
        Temp, Power, Display, Accel, Tpm,
    };
    struct PanelHit {
        PanelId id;
        int y;
        int x;
        int w;
        int h;
    };
    void RegisterPanel(PanelId id, int y, int x, int w, int h);
    void CyclePanelFocus(int dir);
    bool IsPanelFocused(PanelId id) const { return focusedPanel_ == id; }

    // GPU selection for the panel and the detail overlay. The index lives on
    // the TUI side because TuiData is rebuilt by the producer every frame.
    int GpuIndexFor(const TuiData& data) const;              // clamped, 0 when empty
    const TuiData::GpuEntry* CurrentGpu(const TuiData& data) const;  // null when empty

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
    static std::string FormatSize(uint64_t bytes);      // storage: binary (1024), "MB" = MiB
    static std::string FormatSpeed(uint64_t bps);       // link speed: decimal (1000), "Mbps"
    static std::string FormatRate(uint64_t bytesPerSec); // throughput: decimal (1000), "MB/s"
    static std::string TrimRight(const std::string& s, int maxLen);

    // One sparkline glyph (level 0..7). sparkChars_ packs multibyte block
    // characters, so indexing it by byte would slice a UTF-8 sequence in half;
    // ASCII mode packs one byte per level and is indexed directly.
    std::string SparkGlyph(int level) const;
    // Vertical-block glyph for a bar chart cell: 0 = blank, 1..7 = partial
    // fill (▁..▇), 8 = full block (█, '#' in ASCII mode).
    std::string BlockGlyph(int level) const;

    // Row layout helpers (#6): every value line on a panel shares one
    // right-aligned value column (label left), and usage bars draw a
    // colored fill over a dim track instead of same-colored '='/'-'.
    void DrawUsageBarRow(WINDOW* win, int y, int x0, int maxW,
                         const char* label, double pct, const std::string& value);
    void DrawLabeledValue(WINDOW* win, int y, int x0, int maxW,
                          const char* label, const std::string& value, int pair = -1);

    // GUI-style panel title strip: full-width reverse bar (works on any
    // terminal theme and under NO_COLOR). `focused` adds bold so the
    // active panel reads at a glance (design doc, tier-1 chrome).
    void DrawPanelTitle(WINDOW* win, int y, int x0, int maxW,
                        const char* title, bool focused = false);

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
    std::string segShade_ = "\xe2\x96\x91";   // ░ title-strip remainder (B · powerline; ASCII '-')
    std::string barFill_ = "\xe2\x96\x88";    // █ full block (ASCII '=')
    std::string barTrack_ = "\xe2\x96\x91";   // ░ light shade (ASCII '-')
    // Bottom status row: the glyph set in use and, on Windows, which console
    // backend PDCurses picked ("UTF-8 / Windows Terminal", "ASCII / conhost").
    std::string consoleMode_;

    TuiData data_;
    mutable std::mutex dataMutex_;

    std::function<void()> locationRequestHandler_;
    std::function<void()> updateRequestHandler_;

    // Freshness watchdog: microseconds (steady clock) of the last UpdateData
    // snapshot; the dashboard flags stale Ns when it stops advancing.
    std::atomic<int64_t> lastUpdateUs_{0};

    // Mouse hit-testing cache (filled by DrawProcessPanel each frame):
    // screen region of the Top Processes list, plus last-click state for
    // the double-click-to-details gesture.
    int procListY_ = -1;        // row of the panel title (or -1 when hidden)
    int procListCount_ = 0;     // visible process rows below the title
    int procListX0_ = 0;
    int procListW_ = 0;

    // Panel focus state. Only the TUI thread touches it (input + draw are
    // the same loop), so no lock is needed. Default = Top Processes: that
    // surface was keyboard-addressable before panels could be focused, and
    // keeping it keeps the arrow keys live from the first frame.
    PanelId focusedPanel_ = PanelId::Process;
    std::vector<PanelHit> panelHits_;   // panels drawn last frame, in draw order

    // Settings page state (press S): framed interactive form.
    ServerSettings serverSettings_;        // current values (from main)
    ServerSettings draftSettings_;         // edits in progress
    std::function<void(const ServerSettings&)> settingsHandler_;
    std::function<void(int)> sampleIntervalHandler_;   // settings page: sampling interval
    std::function<void(Lang)> languageHandler_;        // settings page: UI language
    std::string configuredLang_;                       // ui.language from config ("" = unset)
    bool settingsPage_ = false;
    bool helpPage_ = false;                // ? key-reference overlay (modal, like settings)
    bool connListPage_ = false;            // connections list overlay ('c' on the dashboard)
    bool cpuPage_ = false;                 // CPU detail overlay (Space on, or double-click, the CPU panel)
    bool gpuPage_ = false;                 // GPU detail overlay (same gestures on the GPU panel)
    bool diskPage_ = false;                // disk detail overlay (same gestures on the Disks panel)
    bool batteryPage_ = false;             // battery/charger overlay (same gestures on the Power panel)
    bool networkPage_ = false;             // network/WiFi/BT overlay (same gestures on the Network panel)
    int selPid_ = -1;                      // selected process pid on the dashboard (-1 = none)
    int gpuIndex_ = 0;                     // GPU shown by the panel/details (Left/Right switches)
    int settingsFocus_ = 0;                // 0=enable, 1=url, 2=insecure, 3=upload interval,
                                           // 4=sample interval, 5=language
    int urlCursor_ = 0;                    // cursor position inside URL field
    Lang draftLang_ = Lang::En;            // language draft in the settings form
    Lang langOnOpen_ = Lang::En;           // language when the form opened (Esc restores)

    // Window dimensions
    int termRows_ = 0;
    int termCols_ = 0;

    // Ensure endwin() is called exactly once
    std::atomic<bool> cursesActive_{false};
};

} // namespace tcmt
