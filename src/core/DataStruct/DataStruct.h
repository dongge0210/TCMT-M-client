// DataStruct.h
#pragma once
#include <cstdio>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

// App version string (override via -DTCMT_VERSION_STR="x.y.z" in CMake/MSBuild).
// 兜底值，正常构建时由 CMake 用根 CMakeLists 的 TCMT_VERSION_DISPLAY 覆盖。
// 改版本时六处一起改，清单见 AvaloniaUI/Core/Constants.cs 上面。
#ifndef TCMT_VERSION_STR
#define TCMT_VERSION_STR "ALPHA_0.0.79"
#endif

// Platform abstraction
#include "../Platform/Platform.h"

// Cross-platform fixed-width wide character (always 2 bytes, UTF-16)
// On Windows: wchar_t is already 2 bytes (UTF-16)
// On macOS/Linux: wchar_t is 4 bytes (UTF-32), force 2-byte with char16_t
#if defined(TCMT_MACOS) || defined(TCMT_LINUX)
using WCHAR = char16_t;
#else
using WCHAR = wchar_t;
#endif

// 温度传感器打包成一行 JSON，给 IPC 的 sensors/json 槽用。
//
// 为什么不按槽位一个个注册字段：那要给每个传感器占两个 schema 字段，而 schema 的总字段数
// 有硬上限（IPC_MAX_FIELDS = 300，客户端超了会静默截断）。之前开的是 10 个槽位，而采集端
// 实际拿到 20 个传感器 —— 一半直接没了，而且槽位一满就没救。
// SMART 属性表早就是用同一个办法装的（PhysDiskSlot::attrsJson），这里沿用。
// 把字符串里的 " \ 和控制字符转义成 JSON 能装的样子。
// 内容本身已经是 UTF-8，原样带走，不做编码转换。
inline std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

inline std::string SensorsToJson(const std::vector<std::pair<std::string, double>>& sensors) {
    std::ostringstream js;
    js << "[";
    for (size_t i = 0; i < sensors.size(); ++i) {
        if (i > 0) js << ",";
        js << "{\"name\":\"" << JsonEscape(sensors[i].first) << "\",\"value\":" << sensors[i].second << "}";
    }
    js << "]";
    return js.str();
}

// 进程列表打包成一行 JSON，形状 [{"pid":…,"name":"…","mem":…,"cpu":…}, …]。
//
// 模板是为了同时吃两种元素类型：macOS 的写入点拿的是 TuiData 的条目，Windows 拿的是
// SystemInfo::ProcData，两边同名字段（pid / name / memoryBytes / cpuPercent）一样，
// 没必要为此造一个中立结构体再逐字段搬一遍。
template <typename Proc>
inline std::string ProcessesToJson(const std::vector<Proc>& procs) {
    std::ostringstream js;
    js << "[";
    for (size_t i = 0; i < procs.size(); ++i) {
        if (i > 0) js << ",";
        js << "{\"pid\":" << procs[i].pid
           << ",\"name\":\"" << JsonEscape(procs[i].name) << "\""
           << ",\"mem\":" << procs[i].memoryBytes
           << ",\"cpu\":" << procs[i].cpuPercent << "}";
    }
    js << "]";
    return js.str();
}

// 逐核传感器打包成一行 JSON，形状 [{"i":0,"temp":52.3,"freq":3200}, …]。
// 见 IPCData.h 的 cpuCoresJson。拿不到的项照写 0，不省略键 —— 解析方按固定键读更省事。
inline std::string CoresToJson(const float* temps, const float* freqs, int count) {
    std::ostringstream js;
    js << "[";
    for (int i = 0; i < count; ++i) {
        if (i > 0) js << ",";
        js << "{\"i\":" << i
           << ",\"temp\":" << temps[i]
           << ",\"freq\":" << freqs[i] << "}";
    }
    js << "]";
    return js.str();
}

// GPU 列表打包成一行 JSON，形状见 IPCData.h 的 gpuJson。
// 与 ProcessesToJson 同理用模板：写入点吃的是 SystemInfo::GpuSpec，字段名一致。
// 从 GPU 名字里认厂商。Windows 有 GetGpuBrand（吃 wstring，见 main.cpp），这份给窄串用，
// macOS 侧没有对应实现，所以两边现在都从这里出。
inline std::string GpuBrandFromName(const std::string& name) {
    if (name.find("NVIDIA") != std::string::npos) return "NVIDIA";
    if (name.find("AMD") != std::string::npos ||
        name.find("Radeon") != std::string::npos) return "AMD";
    if (name.find("Intel") != std::string::npos) return "Intel";
    if (name.find("Apple") != std::string::npos) return "Apple";
    return "Unknown";
}

template <typename Gpu>
inline std::string GpusToJson(const std::vector<Gpu>& gpus) {
    std::ostringstream js;
    js << "[";
    for (size_t i = 0; i < gpus.size(); ++i) {
        if (i > 0) js << ",";
        js << "{\"name\":\"" << JsonEscape(gpus[i].name) << "\""
           << ",\"brand\":\"" << JsonEscape(gpus[i].brand) << "\""
           << ",\"memory\":" << gpus[i].memory
           << ",\"memoryPercent\":" << gpus[i].memoryPercent
           << ",\"coreClock\":" << gpus[i].coreClock
           << ",\"usage\":" << gpus[i].usage
           << ",\"temperature\":" << gpus[i].temperature
           << ",\"isVirtual\":" << (gpus[i].isVirtual ? "true" : "false")
           << ",\"unified\":" << (gpus[i].isUnified ? "true" : "false")
           << ",\"coreCount\":" << gpus[i].coreCount
           << ",\"maxClock\":" << gpus[i].maxClock
           << ",\"power\":" << gpus[i].powerMw
           << ",\"powerLimit\":" << gpus[i].powerLimitMw
           << ",\"fanPercent\":" << gpus[i].fanPercent << "}";
    }
    js << "]";
    return js.str();
}

#pragma pack(push, 1) // Ensure memory alignment

// SMART attribute info
struct SmartAttributeData {
    uint8_t id;                    // Attribute ID
    uint8_t flags;                 // Status flags
    uint8_t current;               // Current value
    uint8_t worst;                 // Worst value
    uint8_t threshold;             // Threshold
    uint64_t rawValue;             // Raw value
    WCHAR name[64];                // Attribute name
    WCHAR description[128];        // Attribute description
    bool isCritical;               // Critical attribute flag
    double physicalValue;          // Physical value (converted)
    WCHAR units[16];               // Units
};

// Physical disk SMART info
struct PhysicalDiskSmartData {
    int physicalIndex = -1;        // Windows: Win32_DiskDrive.Index (used for SMART re-reads)
    WCHAR model[128];              // Disk model
    WCHAR serialNumber[64];        // Serial number
    WCHAR firmwareVersion[32];     // Firmware version
    WCHAR interfaceType[32];       // Interface type (SATA/NVMe/etc)
    WCHAR diskType[16];            // Disk type (SSD/HDD)
    uint64_t capacity;             // Total capacity (bytes)
    double temperature;            // Temperature
    uint8_t healthPercentage;      // Health percentage
    bool isSystemDisk;             // Is system disk
    bool smartEnabled;             // SMART enabled
    bool smartSupported;           // SMART supported

    // SMART attributes array (up to 32 common attributes)
    SmartAttributeData attributes[32];
    int attributeCount;            // Actual attribute count

    // Key health indicators
    uint64_t powerOnHours;         // Power-on time (hours)
    uint64_t powerCycleCount;      // Power cycle count
    uint64_t reallocatedSectorCount; // Reallocated sector count
    uint64_t currentPendingSector; // Current pending sector
    uint64_t uncorrectableErrors;  // Uncorrectable errors
    double wearLeveling;           // Wear leveling (SSD)
    uint64_t totalBytesWritten;    // Total bytes written
    uint64_t totalBytesRead;       // Total bytes read

    // Linux: live IO deltas from /proc/diskstats. 0 when unavailable.
    // Sector counts there are 512-byte units regardless of the device's
    // logical block size.
    uint64_t readBytesPerSec = 0;
    uint64_t writeBytesPerSec = 0;
    uint64_t readIops = 0;
    uint64_t writeIops = 0;

    // Associated logical drives
    char logicalDriveLetters[8];   // Associated drive letters
    int logicalDriveCount;         // Associated drive count

    // Partition volume labels
    WCHAR partitionLabels[8][32]; // Volume label for each partition
    // SMART attributes serialized as JSON array (pipe-delimited for C# parse)
    char attrsJson[4096];

    PlatformSystemTime lastScanTime;       // Last scan time
};

// TPM Info
struct TpmInfo {
    WCHAR manufacturer[32];           // TPM manufacturer name
    uint16_t vendorId;                  // Vendor ID
    WCHAR firmwareVersion[32];        // Firmware version
    uint8_t firmwareVersionMajor;
    uint8_t firmwareVersionMinor;
    uint8_t firmwareVersionBuild;
    uint32_t supportedAlgorithms;       // Supported algorithms
    uint32_t activeAlgorithms;          // Active algorithms
    uint8_t status;                     // TPM status (0=OK, 1=ERROR, 2=DISABLED)
    uint8_t selfTestStatus;             // Self-test status
    uint64_t totalVotes;                // Total votes
    bool isPresent;                     // TPM present
    bool isEnabled;                     // TPM enabled
    bool isActive;                      // TPM active
};

// GPU information
//
// 名字用 std::string 而不是 WCHAR[]：这份现在只喂 IPC 的 gpu/json（见 IPCData.h 的 gpuJson），
// 那边要的是 UTF-8 窄串；多卡机器上整份列表都从这里出去，第 0 项就是主卡。
// memory 是字节数，isUnified 为真时它是统一内存总量、不是显存（Apple Silicon）。
struct GpuSpec {
    std::string name;     // GPU name
    std::string brand;    // Brand
    std::string driver;   // Graphics API / driver release ("Metal 4", "560.94")
    uint64_t memory = 0;      // VRAM (bytes)
    double memoryPercent = 0; // VRAM usage (0-100)
    double coreClock = 0;     // Core clock (MHz)
    double temperature = 0;   // Celsius
    bool isVirtual = false;   // Is virtual GPU
    bool isUnified = false;   // memory 报的其实是统一内存
    double usage = 0;         // GPU usage (0-100)
    // ---- NVIDIA via NVML (Windows). Other platforms leave these at zero. ----
    int nvmlIndex = -1;       // device index for live refresh; -1 = not NVIDIA
    int coreCount = 0;        // shader cores
    int maxClock = 0;         // MHz, graphics clock ceiling
    int memClock = 0;         // MHz, memory clock
    double powerMw = -1;      // current draw, milliwatts; -1 = unreadable
    double powerLimitMw = 0;  // enforced limit, milliwatts
    int fanPercent = -1;      // % of max, -1 when the card reports none
    std::string serial;       // board serial
    std::string vbios;        // VBIOS version
    std::string cudaVersion;  // CUDA UMD version, e.g. "13.4"
    unsigned int pcieGen = 0; // current link generation
    unsigned int pcieWidth = 0;  // current link width (lanes)
    unsigned int maxPcieGen = 0;   // negotiated ceiling for this slot + card
    unsigned int maxPcieWidth = 0;
};

// Network adapter info
struct NetworkAdapterData {
    WCHAR name[128];    // Adapter name
    WCHAR mac[32];      // 硬件 MAC
    WCHAR macCurrent[32]; // 当前生效的链路地址（Windows 上同 mac）
    WCHAR ipAddress[64]; // IP address
    WCHAR adapterType[32]; // Adapter type (wireless/wired)
    uint64_t speed;       // Link speed (bps)
    uint64_t downloadSpeed; // Download throughput (bytes/sec)
    uint64_t uploadSpeed;   // Upload throughput (bytes/sec)
};

// Disk information
struct DiskData {
    char letter;          // Drive letter (e.g. 'C') — Windows only
    // 挂载点。Win32 把盘符、卷 GUID 路径、挂载文件夹都算 volume mount point，
    // macOS 的 "/" 与 "/Volumes/X" 同样落得进去，所以一个字段两边都装得下。
    // 见 https://learn.microsoft.com/windows/win32/fileio/naming-a-volume
    std::string mountPoint;
    std::string label;    // Volume label
    std::string fileSystem;// File system
    uint64_t totalSize = 0; // Total capacity (bytes)
    uint64_t usedSpace = 0; // Used space (bytes)
    uint64_t freeSpace = 0; // Free space (bytes)
};

// Temperature sensor info
struct TemperatureData {
    WCHAR sensorName[64]; // Sensor name
    double temperature;     // Temperature (celsius)
};

// System info struct
struct SystemInfo {
    std::string cpuName;
    int physicalCores;
    int logicalCores;
    double cpuUsage;      // Ensure double type is used
    int performanceCores;
    int efficiencyCores;
    double performanceCoreFreq;
    double efficiencyCoreFreq;
    double cpuBaseFreq = 0;       // Nominal base frequency (MHz) from WMI
    bool hyperThreading;
    bool virtualization;
    uint64_t totalMemory;
    uint64_t usedMemory;
    uint64_t availableMemory;
    uint64_t compressedMemory;
    uint64_t swapUsed = 0;
    uint64_t swapTotal = 0;
    uint32_t ramSpeed = 0;          // RAM frequency in MHz (e.g., 6400)
    char ramType[32] = {};         // DDR generation (e.g., "DDR5", "LPDDR5")
    // 识别到的全部 GPU（第 0 项为主卡）。整份进 IPC 的 gpu/json —— 单值的
    // gpuName/gpuBrand/… 只装得下主卡，多卡机器上其余几块以前是直接消失的。
    std::vector<GpuSpec> gpus;
    std::vector<NetworkAdapterData> adapters;
    std::vector<DiskData> disks;
    std::vector<PhysicalDiskSmartData> physicalDisks; // Physical disk SMART data
    std::vector<std::pair<std::string, double>> temperatures;
    std::vector<TpmInfo> tpms;           // TPM info
    std::string osVersion;
    int batteryPercent = -1;        // -1 = no battery
    bool acOnline = false;
    double cpuPower = 0.0;          // CPU power in mW
    double gpuPower = 0.0;          // GPU power in mW
    double anePower = 0.0;          // ANE power in mW
    bool powerAvailable = false;    // true when a live power source is present
    double gpuFreq = 0.0;           // GPU frequency in MHz
    std::string hardwareModel;      // Hardware model (e.g. "Mac14,2")
    std::string gpuName;            // Added
    std::string gpuBrand;           // Added
    uint64_t gpuMemory;             // Added
    double gpuCoreFreq;             // Added
    double gpuUsage;                // GPU usage
    double gpuMemoryPercent = 0.0;  // VRAM usage % (NVML); 0 = unknown
    bool gpuIsVirtual;              // Is virtual GPU
    std::string networkAdapterName; // Added
    std::string networkAdapterMac;  // Added
    std::string networkAdapterIp;   // Network adapter IP address
    std::string networkAdapterType; // Network adapter type (wireless/wired)
    uint64_t networkAdapterSpeed;   // Added
    double cpuTemperature; // CPU temperature
    double cpuPcoreTemperature = 0.0; // P-core cluster temperature
    double cpuEcoreTemperature = 0.0; // E-core cluster temperature
    double gpuTemperature; // GPU temperature
    double cpuUsageSampleIntervalMs = 0.0; // CPU usage sample interval (ms)
    // WiFi
    bool wifiPowerOn = false;
    bool wifiIsConnected = false;
    std::string wifiSSID;
    int wifiRSSI = 0;
    int wifiChannel = 0;
    std::string wifiSecurity;
    std::string wifiBand;
    std::string wifiGen;

    // Bluetooth
    bool btPowerOn = false;
    int btDeviceCount = 0;
    // 已连接设备名拼成的一行（见 BluetoothInfo.h 的 JoinConnectedDeviceNames）。
    // macOS 的写入点能直接拿到采集结果，不走这里；Windows 侧经 SharedMemoryManager
    // 传递，只能在 SystemInfo 上过一手。
    std::string btDeviceName;

    // 鈹€鈹€鈹€ New fields (feature #4-9) 鈹€鈹€鈹€
    // 4. Fan speeds
    struct FanData {
        std::string name;
        float rpm = 0;
    };
    std::vector<FanData> fans;

    // 5. Process top
    struct ProcData {
        int32_t pid = 0;
        std::string name;
        uint64_t memoryBytes = 0;
        float cpuPercent = 0;
    };
    std::vector<ProcData> topProcesses;

    // 6. Per-core sensors
    float perCoreTemp[16] = {};
    float perCoreFreq[16] = {};
    // Linux-only for now: /proc/stat per-logical-CPU usage. TuiData has no
    // counterpart, so this does not reach the TUI yet.
    float perCoreUsage[16] = {};
    // 1 = efficiency core, from the CPU topology (Windows hybrid parts).
    // perCorePEKnown is false when the machine does not report a split.
    uint8_t perCoreIsE[16] = {};
    bool perCorePEKnown = false;
    int perCoreCount = 0;

    // 7. Battery detail (extended)
    int batteryCycleCount = 0;
    int batteryDesignCapacity = 0;
    int batteryMaxCapacity = 0;
    float batteryHealthPercent = 0;
    float batteryTemp = 0;
    int batteryAmperage = 0;
    int batteryVoltage = 0;
    float batteryChargerWatts = 0;
    bool batteryIsCharging = false;
    bool batteryIsPresent = false;

    // System info
    float loadAvg1 = 0;
    float loadAvg5 = 0;
    float loadAvg15 = 0;
    int processCount = 0;
    uint64_t uptimeSeconds = 0;

    // App version (e.g. "0.14.0")
    std::string appVersion = TCMT_VERSION_STR;

    // Display monitors
    struct DisplayData {
        std::string name;
        int width = 0;          // pixels
        int height = 0;         // pixels
        int refreshRate = 0;    // Hz
        bool isHDR = false;
        bool isBuiltin = false;
        double backingScale = 1.0;
    };
    std::vector<DisplayData> displays;
    bool displayDirty = false;   // set when display config changes

    // Thermal state (macOS NSProcessInfoThermalState)
    int thermalState = 0;        // 0=nominal, 1=fairlySerious, 2=critical

    PlatformSystemTime lastUpdate;
};

#pragma pack(pop)
