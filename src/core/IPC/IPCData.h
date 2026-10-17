#pragma once
#include <cstdint>
#include <cstring>

namespace tcmt::ipc {

// === Protocol Constants ===
constexpr uint32_t IPC_MAGIC          = 0x54434D54; // "TCMT"
constexpr uint8_t  IPC_VERSION        = 1;
constexpr uint32_t IPC_MAX_FIELDS     = 300;
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
    Shutdown     = 0x07,  // C++ → C#:  server shutting down (SIGINT/SIGTERM)
    SchemaUpdate = 0x08,  // C++ → C#: schema changed, re-open shared memory
    ConfigUpdate = 0x09,  // C++ → C#: config file changed, client should reload
    ConfigSet      = 0x0A,  // C# → C++: 改配置，payload = JSON 对象，键必须在白名单里
    ConfigSnapshot = 0x0B,  // C++ → C#: 当前配置值，payload = JSON 对象（同一份白名单）
};

// 配置消息的 payload 上限。它是几个键值对，不是数据面 —— 卡住上限是防止
// 对端发一个超大 payloadSize 把服务端的内存拉爆。
static constexpr uint16_t IPC_CONFIG_PAYLOAD_MAX = 8192;

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
    uint32_t writeSequence           = 0; // seqlock: odd=write in progress, even=complete

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
    float    cpuPcoreTemp            = 0;
    float    cpuEcoreTemp            = 0;
    bool     hyperThreading          = false;
    bool     virtualization          = false;
    float    cpuSampleIntervalMs     = 500;
    float    cpuBaseFreq             = 0;     // nominal base clock in MHz
    char     timestamp[20]           = {};

    // Memory
    uint64_t totalMemory             = 0;
    uint64_t usedMemory              = 0;
    uint64_t availableMemory         = 0;
    uint64_t compressedMemory        = 0;
    uint64_t swapUsed                = 0;
    uint64_t swapTotal               = 0;
    uint32_t ramSpeed                = 0;
    char     ramType[32]             = {};

    // Battery / power
    int32_t  batteryPercent          = -1;
    bool     acOnline                = false;
    float    cpuPower                = 0;
    float    gpuPower                = 0;
    float    anePower                = 0;

    // OS
    char     osVersion[128]          = {};
    char     hardwareModel[128]      = {};

    // GPU
    char     gpuName[48]             = {};
    char     gpuBrand[32]            = {};
    uint64_t gpuMemory               = 0;
    float    gpuMemoryPercent        = 0;
    float    gpuUsage                = 0;
    float    gpuTemp                 = 0;
    float    gpuFreq                 = 0;
    bool     gpuIsVirtual            = false;
    // gpuMemory 是统一内存而非独立显存（Apple Silicon），界面据此不画「显存」那一行
    bool     gpuUnified              = false;
    // 上面 gpu/0/* 那份是「主卡」：多卡机器上只挑了其中一块，另外几块完全没有痕迹。
    // 整份 GPU 列表装成一行 JSON，槽位方案要 4 槽 × 8 字段 ≈ 32 个名额，这里只占 1 个。
    // 形状：[{"name":"Apple M2","brand":"Apple","memory":17179869184,"memoryPercent":61.0,
    //        "coreClock":1398,"usage":12.0,"temperature":48.0,"isVirtual":false,"unified":true}, ...]
    // 数组第 0 项就是主卡，与上面的 gpu/0/* 一致。
    char     gpuJson[2048]           = {};

    // Disks (up to 4)
    struct DiskSlot {
        char     letter              = '\0';   // Windows 盘符；macOS 恒为 0
        // 挂载点。Win32 把盘符、卷 GUID 路径、挂载文件夹都算 volume mount point，
        // macOS 的 "/" 与 "/Volumes/X" 同样落得进去 —— 一个字段两边都装得下。
        // 见 https://learn.microsoft.com/windows/win32/fileio/naming-a-volume
        char     mountPoint[64]      = {};
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
        // 硬件 MAC：这块网卡出厂的那个，不随网络变。
        char     mac[18]             = {};
        // 当前生效的链路层地址。macOS 的「私有 Wi-Fi 地址」按 SSID 轮换，所以它会与 mac 不同；
        // Windows / Linux 上通常与 mac 相同。采集端给不出时写空串。
        char     macCurrent[18]      = {};
        // 24 而不是 16 —— 采集端产出的两个标准值 "Wireless adapter" 与 "Ethernet adapter"
        // 都恰好是 16 个字符，装进 char[16] 只剩 15 个位置，界面上显示成 "Wireless adapte"。
        // 缓冲区至少要能装下自己写进去的字符串。
        char     type[24]            = {};
        uint64_t speed               = 0;
        uint64_t downloadSpeed       = 0;
        uint64_t uploadSpeed         = 0;
    };
    NetSlot  adapters[4]             = {};
    uint8_t  adapterCount            = 0;

    // 温度传感器：整份打包成一行 JSON，不再按槽位开字段。
    //
    // 原来开 10 个 TempSlot，每个占 2 个 schema 字段 —— 而采集端实际产出 20 个传感器，
    // 一半被截掉，且槽位一满就没救；schema 总字段数还有硬上限（IPC_MAX_FIELDS）。
    // 装成一个字段后上限消失，schema 也只占 1 个名额。
    // 装法与解析沿用 SMART 属性表那套（PhysDiskSlot::attrsJson）。
    // 形状：[{"name":"CPU P-core","value":68},{"name":"GPU","value":46}, ...]
    char     sensorsJson[4096]       = {};

    // Physical disks + SMART (up to 2)
    struct PhysDiskSlot {
        char     model[64]           = {};
        char     serial[64]          = {};
        uint64_t capacity            = 0;
        char     interfaceType[16]   = {};
        float    temperature         = 0;
        float    healthPercent       = 0;
        bool     smartSupported      = false;
        int32_t  attrCount           = 0;
        char     attrsJson[4096]     = {}; // SMART attributes as JSON array
        char     logicalDriveLetters[8] = {}; // e.g., "CD"
        int32_t  logicalDriveCount   = 0;
    };
    PhysDiskSlot physicalDisks[8]    = {};
    uint8_t  physDiskCount           = 0;

    // WiFi
    struct WifiSlot {
        char     ssid[32]            = {};
        int32_t  rssi                = 0;
        int32_t  channel             = 0;
        char     security[16]        = {};
        char     band[8]             = {};
        char     wifiGen[12]         = {};
        bool     powerOn             = false;
        bool     isConnected         = false;
    };
    WifiSlot wifi                     = {};

    // Bluetooth
    struct BtSlot {
        bool     powerOn             = false;
        int32_t  deviceCount         = 0;
        char     name[64]            = {};
    };
    BtSlot bluetooth                  = {};

    // TPM
    struct TpmSlot {
        char     manufacturer[32]    = {};
        char     firmwareVersion[32] = {};
        uint16_t vendorId            = 0;
        uint8_t  firmwareVersionMajor = 0;
        uint8_t  firmwareVersionMinor = 0;
        uint8_t  firmwareVersionBuild = 0;
        bool     isPresent           = false;
        bool     isEnabled           = false;
        bool     isActive            = false;
        uint8_t  selfTestStatus      = 0;
        uint8_t  status              = 0;
    };
    TpmSlot tpm                       = {};
    uint8_t  tpmCount                 = 0;

    // App version string (e.g. "0.14.0")
    // 32 而不是 16：版本串现在是 ALPHA_0.0.79-<git 短 SHA>，20 个字符。
    // 原来 16（写入截到 15）会把后缀吃掉一半，而且截断后看着还像个正常版本号。
    char     appVersion[32]          = {};

    // ─── 4. Fan speeds (up to 6 fans) ───
    struct FanSlot {
        char     name[32]            = {};
        float    rpm                 = 0;
    };
    FanSlot  fanSpeeds[6]            = {};
    uint8_t  fanCount                = 0;

    // ─── 5. 进程列表：整份打包成一行 JSON，不再按槽位开字段 ───
    //
    // 原来只开 7 个槽位、每槽占 4 个 schema 字段。采集端明明枚举了全部进程，却在写入时被砍到 7，
    // 进程页也就只能显示 7 行；而 schema 总字段数有硬上限（IPC_MAX_FIELDS = 300，超了客户端
    // 静默截断），照槽位往上抬是抬不动的 —— 抬到 64 个进程就要 256 个字段。
    // 装成一个字段后上限消失，schema 反而省下 27 个名额。
    // 形状：[{"pid":1234,"name":"Google Chrome Helper","mem":1620,"cpu":54.1}, …]
    char     processesJson[16384]    = {};

    // ─── 7. Battery detail (health, cycle count, etc.) ───
    int32_t  batteryCycleCount       = 0;
    int32_t  batteryDesignCapacity   = 0;      // mAh
    int32_t  batteryMaxCapacity      = 0;      // mAh (current max)
    float    batteryHealthPercent    = 0;      // 0-100
    float    batteryTemp             = 0;      // Celsius
    int32_t  batteryAmperage         = 0;      // mA
    int32_t  batteryVoltage          = 0;      // mV
    float    batteryChargerWatts     = 0;      // connected charger W
    bool     batteryIsCharging       = false;
    bool     batteryIsPresent        = false;

    // ─── 6. 逐核传感器：整份打包成一行 JSON，不再按槽位开字段 ───
    //
    // 原来 16 槽 × 2 字段 + 1 个计数 = 33 个 schema 名额。Windows 侧的总字段数因此顶到 326，
    // 越过 IPC_MAX_FIELDS = 300，排在后面的 core/* 尾巴和整个 system/* 被客户端静默截掉 ——
    // 连 core/count 都读不到，逐核那一段从来没渲染过。装成一个字段后上限消失，schema 只占 1 个名额。
    // 装法与解析沿用 sensorsJson 那套。
    // 形状：[{"i":0,"temp":52.3,"freq":3200}, ...]，数组长度即核数。
    char     cpuCoresJson[2048]      = {};

    // ─── System info ───
    float    loadAvg1                = 0;
    float    loadAvg5                = 0;
    float    loadAvg15               = 0;
    int32_t  processCount            = 0;
    uint64_t uptimeSeconds           = 0;
};

#pragma pack(pop)

static_assert(sizeof(FieldDef) == IPC_FIELD_DEF_SIZE, "FieldDef size mismatch");
static_assert(sizeof(SchemaHeader) == IPC_SCHEMA_HEADER_SIZE, "SchemaHeader size mismatch");

} // namespace tcmt::ipc
