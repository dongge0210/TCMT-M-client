// DataStruct.h
#pragma once
#include <string>
#include <vector>

// App version string (override via -DTCMT_VERSION_STR="x.y.z" in CMake/MSBuild)
#ifndef TCMT_VERSION_STR
#define TCMT_VERSION_STR "0.14.0"
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
struct GPUData {
    WCHAR name[128];    // GPU name
    WCHAR brand[64];    // Brand
    uint64_t memory;      // VRAM (bytes)
    double coreClock;     // Core clock (MHz)
    bool isVirtual;       // Is virtual GPU
    double usage;         // GPU usage (0-100)
};

// Network adapter info
struct NetworkAdapterData {
    WCHAR name[128];    // Adapter name
    WCHAR mac[32];      // MAC address
    WCHAR ipAddress[64]; // IP address
    WCHAR adapterType[32]; // Adapter type (wireless/wired)
    uint64_t speed;       // Link speed (bps)
    uint64_t downloadSpeed; // Download throughput (bytes/sec)
    uint64_t uploadSpeed;   // Upload throughput (bytes/sec)
};

// Disk information
struct DiskData {
    char letter;          // Drive letter (e.g. 'C')
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
    std::vector<GPUData> gpus;
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
