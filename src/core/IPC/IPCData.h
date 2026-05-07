#pragma once
#include <cstdint>
#include <cstring>

namespace tcmt::ipc {

// === Protocol Constants ===
constexpr uint32_t IPC_MAGIC          = 0x54434D54; // "TCMT"
constexpr uint8_t  IPC_VERSION        = 1;
constexpr uint32_t IPC_MAX_FIELDS     = 120;
constexpr uint32_t IPC_FIELD_NAME_LEN = 32;
constexpr uint32_t IPC_FIELD_UNITS_LEN = 16;
constexpr uint32_t IPC_SCHEMA_HEADER_SIZE = 16;
constexpr uint32_t IPC_FIELD_DEF_SIZE = 80;
constexpr int      IPC_MAX_CLIENTS    = 8;
constexpr const char* IPC_SHM_PATH   = "/tcmt_ipc_shm";
constexpr const char* IPC_SOCK_PATH  = "/tmp/tcmt_ipc.sock";

// === Client types (sent in HELLO payload) ===
enum class ClientType : uint8_t {
    Unknown  = 0,
    Avalonia = 1,  // C# Avalonia GUI
    MCP      = 2,  // C++ MCP server (--mcp mode)
};

// === Pipe message types (after schema handshake) ===
enum class PipeMsgType : uint8_t {
    Hello      = 0x01,  // C# → C++: client intro
    HelloAck   = 0x02,  // C++ → C#:  server intro + schema follows
    Ack        = 0x03,  // C# → C++:  schema accepted
    Ping       = 0x04,  // C# → C++:  keep-alive
    Pong       = 0x05,  // C++ → C#:  keep-alive response
    Bye        = 0x06,  // C# → C++:  client disconnecting
    Shutdown   = 0x07,  // C++ → C#:  server shutting down (SIGINT/SIGTERM)
    SchemaUpdate = 0x08, // C++ → C#: schema changed, re-open shared memory
};

#pragma pack(push, 1)
struct PipeMessage {
    uint8_t  type = 0;       // PipeMsgType
    uint8_t  version = IPC_VERSION;
    uint16_t payloadSize = 0; // Size of payload after header
    // Followed by payload
};
static constexpr uint32_t PIPE_MSG_HEADER_SIZE = 4;
#pragma pack(pop)

// === Wire Types ===
enum class FieldType : uint8_t {
    UInt8   = 1,
    Int8    = 2,
    UInt16  = 3,
    Int16   = 4,
    UInt32  = 5,
    Int32   = 6,
    UInt64  = 7,
    Int64   = 8,
    Float32 = 9,
    Float64 = 10,
    Bool    = 11,
    String  = 12,
    WString = 13,
};

#pragma pack(push, 1)

struct SchemaHeader {
    uint32_t magic      = IPC_MAGIC;
    uint8_t  version    = IPC_VERSION;
    uint8_t  flags      = 0;
    uint16_t fieldCount = 0;
    uint32_t totalSize  = 0;
    uint32_t stringBlockSize = 0;
};

struct FieldDef {
    uint32_t id       = 0;
    uint8_t  type     = 0;      // FieldType
    uint8_t  reserved = 0;
    uint16_t size     = 0;
    uint32_t offset   = 0;
    uint32_t count    = 0;
    uint32_t strOffset = 0;
    uint32_t flags    = 0;
    float    minVal   = 0;
    float    maxVal   = 0;
    char     name[IPC_FIELD_NAME_LEN]  = {};
    char     units[IPC_FIELD_UNITS_LEN] = {};
};

// IPC data block -- shared via mmap file, read by C# AvaloniaUI
struct IPCDataBlock {
    // CPU
    char     cpuName[64]             = {};
    uint8_t  physicalCores           = 0;
    uint8_t  logicalCores            = 0;
    uint8_t  performanceCores        = 0;
    uint8_t  efficiencyCores         = 0;
    float    cpuUsage                = 0;
    float    pCoreFreq               = 0;
    float    eCoreFreq               = 0;
    float    cpuTemp                 = 0;
    bool     hyperThreading          = false;
    bool     virtualization          = false;
    float    cpuSampleIntervalMs     = 500;
    char     timestamp[20]           = {};

    // Memory
    uint64_t totalMemory             = 0;
    uint64_t usedMemory              = 0;
    uint64_t availableMemory         = 0;
    uint64_t compressedMemory        = 0;

    // Battery / power
    int32_t  batteryPercent          = -1;
    bool     acOnline                = false;

    // OS
    char     osVersion[128]          = {};

    // GPU
    char     gpuName[48]             = {};
    char     gpuBrand[32]            = {};
    uint64_t gpuMemory               = 0;
    float    gpuMemoryPercent        = 0;
    float    gpuUsage                = 0;
    float    gpuTemp                 = 0;
    bool     gpuIsVirtual            = false;

    // Disks (up to 4)
    struct DiskSlot {
        char     label[32]           = {};
        uint64_t totalSize           = 0;
        uint64_t usedSpace           = 0;
        uint64_t freeSpace           = 0;
        char     fs[16]              = {};
    };
    DiskSlot disks[4]                = {};
    uint8_t  diskCount               = 0;

    // Network adapters (up to 4)
    struct NetSlot {
        char     name[32]            = {};
        char     ip[16]              = {};
        char     mac[18]             = {};
        char     type[16]            = {};
        uint64_t speed               = 0;
        uint64_t downloadSpeed       = 0;
        uint64_t uploadSpeed         = 0;
    };
    NetSlot  adapters[4]             = {};
    uint8_t  adapterCount            = 0;

    // Temperatures (up to 10)
    struct TempSlot {
        char     name[64]            = {};
        float    value               = 0;
    };
    TempSlot temperatures[10]        = {};
    uint8_t  tempCount               = 0;

    // Physical disks + SMART (up to 2)
    struct PhysDiskSlot {
        char     model[64]           = {};
        char     serial[64]          = {};
        uint64_t capacity            = 0;
        char     interfaceType[16]   = {};
        float    temperature         = 0;
        float    healthPercent       = 0;
        bool     smartSupported      = false;
    };
    PhysDiskSlot physicalDisks[2]    = {};
    uint8_t  physDiskCount           = 0;
};

#pragma pack(pop)

static_assert(sizeof(FieldDef) == IPC_FIELD_DEF_SIZE, "FieldDef size mismatch");
static_assert(sizeof(SchemaHeader) == IPC_SCHEMA_HEADER_SIZE, "SchemaHeader size mismatch");

} // namespace tcmt::ipc
