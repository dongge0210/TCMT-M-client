// DataStruct.h
#pragma once
#include <string>
#include <vector>

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
    bool hyperThreading;
    bool virtualization;
    uint64_t totalMemory;
    uint64_t usedMemory;
    uint64_t availableMemory;
    uint64_t compressedMemory;
    std::vector<GPUData> gpus;
    std::vector<NetworkAdapterData> adapters;
    std::vector<DiskData> disks;
    std::vector<PhysicalDiskSmartData> physicalDisks; // Physical disk SMART data
    std::vector<std::pair<std::string, double>> temperatures;
    std::vector<TpmInfo> tpms;           // TPM info
    std::string osVersion;
    int batteryPercent = -1;        // -1 = no battery
    bool acOnline = false;
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
    double gpuTemperature; // GPU temperature
    double cpuUsageSampleIntervalMs = 0.0; // CPU usage sample interval (ms)
    PlatformSystemTime lastUpdate;
};

// Shared memory main struct
struct SharedMemoryBlock {
    uint32_t writeSequence;   // seqlock: odd=write in progress, even=complete

    WCHAR cpuName[128];       // CPU name - WCHAR array
    int physicalCores;        // Physical cores
    int logicalCores;         // Logical cores
    double cpuUsage;          // Changed to double type, improved precision
    int performanceCores;     // Performance cores
    int efficiencyCores;      // Efficiency cores
    double pCoreFreq;         // Performance core frequency (MHz)
    double eCoreFreq;         // Efficiency core frequency (MHz)
    bool hyperThreading;      // Hyperthreading enabled
    bool virtualization;      // Virtualization enabled
    uint64_t totalMemory;     // Total memory (bytes)
    uint64_t usedMemory;      // Used memory (bytes)
    uint64_t availableMemory; // Available memory (bytes)
    uint64_t compressedMemory; // Compressed memory (bytes)
    double cpuTemperature; // CPU temperature
    double gpuTemperature; // GPU temperature
    double cpuUsageSampleIntervalMs; // CPU usage sample interval (ms)

    // GPU information (up to 2 GPUs supported)
    GPUData gpus[2];

    // Network adapter (up to 4 adapters supported)
    NetworkAdapterData adapters[4];

    // Logical disk information (up to 8 disks supported)
    struct SharedDiskData {
        char letter;             // Drive letter (e.g. 'C')
        WCHAR label[128];      // Volume label - Using WCHAR array for shared memory
        WCHAR fileSystem[32];  // File system - Using WCHAR array for shared memory
        uint64_t totalSize;      // Total capacity (bytes)
        uint64_t usedSpace;      // Used space (bytes)
        uint64_t freeSpace;      // Free space (bytes)
    } disks[8];

    // Physical disk SMART info (up to 8 physical disks supported)
    PhysicalDiskSmartData physicalDisks[8];

    // Temperature data (up to 10 sensors supported)
    TemperatureData temperatures[10];

    int adapterCount;
    int tempCount;
    int gpuCount;
    int diskCount;
    int physicalDiskCount;       // Physical disk count

    // TPM info (1 TPM supported)
    TpmInfo tpm;
    uint8_t tpmCount;               // TPM count

    // Battery / power info
    int batteryPercent;             // 0-100, -1 = no battery
    bool acOnline;                  // AC power connected

    // OS version info
    WCHAR osVersion[128];           // e.g. "macOS 15.6 (MacBookPro18,1)"

    PlatformSystemTime lastUpdate;
    PlatformCriticalSection lock;
};
#pragma pack(pop)
