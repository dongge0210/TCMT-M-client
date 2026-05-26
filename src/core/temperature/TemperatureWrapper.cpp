#include "TemperatureWrapper.h"
#include "../Utils/Logger.h"

#ifdef TCMT_WINDOWS
#pragma managed
#include "../Utils/LibreHardwareMonitorBridge.h"
#include "../memory/MemoryTempReader.h"
#include "../cpu/CpuTempReader.h"
#pragma unmanaged

bool TemperatureWrapper::initialized = false;

void TemperatureWrapper::Initialize() {
    try {
        LibreHardwareMonitorBridge::Initialize();
        initialized = true;
        Logger::Debug("TemperatureWrapper: LibreHardwareMonitor initialized");
    } catch (...) {
        initialized = false;
        Logger::Error("TemperatureWrapper: LibreHardwareMonitor initialization failed");
    }
    if (MemoryTempReader::IsAvailable())
        Logger::Info("PawnIO: installed, DIMM temperature reading enabled");
    else
        Logger::Info("PawnIO: not installed, DIMM temperature unavailable");
}

void TemperatureWrapper::Cleanup() {
    if (initialized) {
        LibreHardwareMonitorBridge::Cleanup();
        initialized = false;
    }
}

std::vector<std::pair<std::string, double>> TemperatureWrapper::GetTemperatures() {
    std::vector<std::pair<std::string, double>> temps;
    if (initialized) {
        try {
            temps = LibreHardwareMonitorBridge::GetTemperatures();
        } catch (...) {
            Logger::Warn("TemperatureWrapper::GetTemperatures: unknown exception caught");
        }
    }

    // Add DIMM temperatures via PawnIO/SMBus (no-op if PawnIO not installed)
    size_t lhmCount = temps.size();
    try {
        auto dimms = MemoryTempReader::ReadAll();
        for (const auto& d : dimms) {
            if (d.temperature > 0)
                temps.push_back({d.name, d.temperature});
        }
    } catch (...) {
        Logger::Debug("MemoryTempReader: exception (no PawnIO?)");
    }

    // Add CPU package temperature via PawnIO/MSR (independent of LHM)
    double cpuTemp = CpuTempReader::ReadPackageTemp();
    if (cpuTemp > 0)
        temps.push_back({"CPU Package (PawnIO)", cpuTemp});

    // One-time dump: compare LHM vs PawnIO temperature sources
    static int dumpN = 0;
    if (dumpN < 1) {
        Logger::Info("=== Temperature sources (LHM=" + std::to_string(lhmCount) +
                     " + PawnIO=" + std::to_string(temps.size() - lhmCount) +
                     " = " + std::to_string(temps.size()) + " total) ===");
        for (size_t i = 0; i < temps.size(); i++) {
            Logger::Info(std::string(i < lhmCount ? "  [LHM] " : "  [PawnIO] ") +
                         temps[i].first + " = " + std::to_string(temps[i].second) + " C");
        }
        dumpN++;
    }

    return temps;
}

bool TemperatureWrapper::IsInitialized() { return initialized; }

#elif defined(TCMT_MACOS)
// ======================== macOS Implementation ========================
// macOS temperature access options (no root):
//   1. Apple Silicon SMC (AppleSMCKeysEndpoint, selector 2, works on AS)
//   2. Intel SMC (AppleSMC, selector 5, works on Intel)
//   3. IOKit IOHIDEventService (fallback, limited sensors)
//   4. powermetrics (needs root, caches via background thread)
//
// Priority: SMC (platform-appropriate) > IOKit HID > powermetrics cache

#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <unordered_map>

// =====================================================================
// SMC Reader — Direct IOKit access to AppleSMC
// =====================================================================
// SMC selectors (selector 5, subfunction in data8 field):
//   9  = getTotalNumber   → total key count
//   7  = getKeyFromIndex  → key string by index
//   2  = readKeyInfo      → key metadata (type, size)
//   5  = readKeyValue      → actual value data

// SMC is Intel-only — hardware locked on Apple Silicon
#ifdef __x86_64__

enum {
    KSmcReadKeyInfo  = 2,
    KSmcReadKeyValue = 5,
    KSmcGetTotalNum  = 9,
};

typedef struct {
    uint32_t key;
    uint32_t vers;
    uint32_t pLimitData;
    uint32_t keyInfo;
    uint32_t result;
    uint32_t status;
    uint32_t data8;
    uint32_t data32;
    char data[32];
} SmcKeyData_t;

// Suppress missing field initializer warnings for SMC structs
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-field-initializers"

typedef struct {
    char vers[8];
    char flags;
    uint8_t num;
    uint8_t cpuType;
    uint16_t cpuSubType;
    uint32_t cacheSize;
    uint32_t cpuCacheSize;
    uint32_t busSpeed;
    uint32_t cpuSpeedMax;
    uint32_t cpuSpeedMin;
    uint32_t cpuSpeedCur;
    uint32_t fab;
} SmcVersion_t;

typedef struct {
    uint32_t dataSize;
    char dataType[5];
    uint8_t dataAttributes;
} SmcKeyInfoVal_t;

#ifdef __x86_64__
static io_connect_t g_smc_conn = 0;
static std::mutex   g_smc_mutex;
#endif

// Open/close AppleSMC service
static kern_return_t open_smc_service(io_connect_t* conn) {
    // Use kIOMasterPortDefault for compatibility with macOS 11+
    // kIOMainPortDefault is just a rename in macOS 12+ but functionally identical
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    mach_port_t masterPort = kIOMasterPortDefault;
#pragma clang diagnostic pop
    io_service_t svc = IOServiceGetMatchingService(
        masterPort, IOServiceMatching("AppleSMC"));
    if (!svc) return kIOReturnNotFound;
    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, conn);
    IOObjectRelease(svc);
    return kr;
}

// Read SMC key info (selector 5, subfn 2)
static kern_return_t smc_read_key_info(io_connect_t conn, uint32_t key,
                                       SmcKeyInfoVal_t* info) {
    SmcKeyData_t in = {};
    SmcKeyData_t out = {};
    in.key   = key;
    in.data8 = KSmcReadKeyInfo;
    size_t sz = sizeof(out);
    kern_return_t kr = IOConnectCallStructMethod(conn, 5, &in, sizeof(in), &out, &sz);
    if (kr != kIOReturnSuccess) return kr;
    std::memset(info, 0, sizeof(*info));
    // dataSize from keyInfo (varies by SMC version)
    info->dataSize = static_cast<uint32_t>(out.keyInfo & 0xFFFF);
    if (info->dataSize == 0) info->dataSize = static_cast<uint32_t>((out.keyInfo >> 16) & 0xFFFF);
    if (info->dataSize == 0) info->dataSize = static_cast<uint32_t>(out.status & 0xFFFF);
    // Copy dataType from response (first 4 bytes of data[] contain type string)
    std::memcpy(info->dataType, out.data, std::min(sizeof(info->dataType), sizeof(out.data)));
    return kIOReturnSuccess;
}

// Read SMC key value (selector 5, subfn 5)
static kern_return_t smc_read_key_value(io_connect_t conn, uint32_t key,
                                         uint32_t dataSize,
                                         char* outBuf, size_t maxLen) {
    SmcKeyData_t in = {};
    SmcKeyData_t out = {};
    in.key    = key;
    in.data8  = KSmcReadKeyValue;
    in.keyInfo = dataSize;
    size_t sz = sizeof(out);
    kern_return_t kr = IOConnectCallStructMethod(conn, 5, &in, sizeof(in), &out, &sz);
    if (kr == kIOReturnSuccess && dataSize > 0 && dataSize <= 32) {
        size_t n = std::min(static_cast<size_t>(dataSize), maxLen);
        std::memcpy(outBuf, out.data, n);
    }
    return kr;
}

#pragma clang diagnostic pop

// Decode temperature from raw SMC bytes
// Common types: 'sp78' (signed 7.8 fixed-point), 'flt' (IEEE754 float),
//              'fpe2' (IEEE11073 float), 'fp79'/'fp88' (half/float variants)
static double smc_decode_temp(const char* bytes, size_t size, const char* type) {
    if (size == 0 || !bytes) return 0.0;

    // SP78: signed 7-bit integer + 8-bit fractional
    if (type[0] == 's' && type[1] == 'p') {
        if (size >= 2) {
            int8_t  intPart  = static_cast<int8_t>(bytes[0]);
            uint8_t fracPart = static_cast<uint8_t>(bytes[1]);
            return intPart + fracPart / 256.0;
        }
    }
    // IEEE754 float
    if (type[0] == 'f' && type[1] == 'l' && type[2] == 't') {
        if (size >= 4) {
            float f; std::memcpy(&f, bytes, 4); return f;
        }
    }
    // FPE2: unsigned 14.2 fixed-point
    if (type[0] == 'f' && type[1] == 'p' && type[2] == 'e') {
        if (size >= 2) {
            uint16_t val = static_cast<uint8_t>(bytes[0]) | (static_cast<uint8_t>(bytes[1]) << 8);
            return val / 4.0;
        }
    }
    // fp79: 16-bit half-precision (signed)
    if (type[0] == 'f' && type[1] == 'p' && type[2] == '7') {
        if (size >= 2) {
            uint16_t bits = static_cast<uint8_t>(bytes[0]) | (static_cast<uint8_t>(bytes[1]) << 8);
            int sign = (bits >> 15) & 1;
            int exp  = (bits >> 10) & 0x1F;
            int mant = bits & 0x3FF;
            if (exp == 0) return sign ? -0.0f : 0.0f;
            if (exp == 31) return sign ? -1e9f : 1e9f;
            float f = (1.0f + mant / 1024.0f) * std::powf(2.0f, static_cast<float>(exp - 15));
            return sign ? -f : f;
        }
    }
    // Fallback: single signed byte
    if (size >= 1) return static_cast<double>(static_cast<int8_t>(bytes[0]));
    return 0.0;
}

// Probe SMC connection and test read
static bool probe_smc(void) {
    if (g_smc_conn != 0) return true;

    kern_return_t kr = open_smc_service(&g_smc_conn);
    if (kr != kIOReturnSuccess) {
        Logger::Info("TemperatureWrapper: AppleSMC not accessible (kr=0x"
                     + std::to_string(kr) + ")");
        return false;
    }

    // Test with a known CPU temperature key
    uint32_t key = 0;
    std::memcpy(&key, "TC0P", 4);
    // Apple Silicon byte order: already big-endian from memcpy
    double testTemp = 0;
    char val[32] = {0};
    SmcKeyInfoVal_t info{};
    if (smc_read_key_info(g_smc_conn, key, &info) == kIOReturnSuccess && info.dataSize > 0) {
        if (smc_read_key_value(g_smc_conn, key, info.dataSize, val, sizeof(val)) == kIOReturnSuccess) {
            testTemp = smc_decode_temp(val, info.dataSize, "sp78");
        }
    }

    if (testTemp > 10 && testTemp < 150) {
        Logger::Info("TemperatureWrapper: AppleSMC open, TC0P="
                     + std::to_string(testTemp) + "C");
    } else {
        Logger::Debug("TemperatureWrapper: AppleSMC TC0P probe returned 0 (expected on Apple Silicon)");
    }
    return true;
}

// Read a named SMC temperature key
static bool smc_read_temp(const char* keyStr, const char* type, double* tempOut) {
    std::lock_guard<std::mutex> lock(g_smc_mutex);
    if (!g_smc_conn) return false;

    uint32_t key = 0;
    std::memcpy(&key, keyStr, 4);

    SmcKeyInfoVal_t info;
    if (smc_read_key_info(g_smc_conn, key, &info) != kIOReturnSuccess || info.dataSize == 0)
        return false;
    if (info.dataSize > 32) info.dataSize = 32;

    char val[32] = {0};
    if (smc_read_key_value(g_smc_conn, key, info.dataSize, val, sizeof(val)) != kIOReturnSuccess)
        return false;

    *tempOut = smc_decode_temp(val, info.dataSize, type ? type : "sp78");
    return (*tempOut > 0 && *tempOut < 150);
}

// Enumerate all SMC keys, filter temperature-type sensors.
// Called once; caller may hold g_smc_mutex or not (we re-lock internally).
static std::vector<std::string> smc_enumerate_temp_keys(void) {
    std::vector<std::string> keys;
    std::lock_guard<std::mutex> lock(g_smc_mutex);
    if (!g_smc_conn) return keys;

    SmcKeyData_t in = {};
    SmcKeyData_t out = {};
    in.data8 = KSmcGetTotalNum;
    size_t sz = sizeof(out);
    if (IOConnectCallStructMethod(g_smc_conn, 5, &in, sizeof(in), &out, &sz) != kIOReturnSuccess)
        return keys;

    uint32_t total = out.keyInfo;
    if (total == 0 || total > 65535) return keys;

    for (uint32_t i = 0; i < total && keys.size() < 64; i++) {
        SmcKeyData_t kin = {}, kout = {};
        kin.data8 = 7;  // getKeyFromIndex
        kin.keyInfo = i;
        size_t ksz = sizeof(kout);
        if (IOConnectCallStructMethod(g_smc_conn, 5, &kin, sizeof(kin), &kout, &ksz) != kIOReturnSuccess)
            continue;

        char name[5] = {};
        std::memcpy(name, &kout.key, 4);
        SmcKeyInfoVal_t info{};
        if (smc_read_key_info(g_smc_conn, kout.key, &info) != kIOReturnSuccess || info.dataSize == 0)
            continue;
        if (info.dataSize > 32) continue;

        char type[5] = {};
        std::memcpy(type, info.dataType, 4);
        bool isTemp = (type[0] == 's' && type[1] == 'p') ||
                       (type[0] == 'f' && type[1] == 'l') ||
                       (type[0] == 'f' && type[1] == 'p');
        if (isTemp)
            keys.push_back(std::string(name, 4) + ":" + std::string(type, 4));
    }
    return keys;
}

#endif // __x86_64__ — end of Intel SMC block

// =====================================================================
// Apple Silicon SMC — selector 2 via AppleSMCKeysEndpoint (no root needed)
// Reference: macmon (https://github.com/vladkens/macmon)
// =====================================================================

typedef struct {
    uint32_t dataSize;
    uint32_t dataType;
    uint8_t  dataAttributes;
    uint8_t  _pad[3];   // natural alignment: KeyInfo = 12 bytes
} AsKeyInfo;

typedef struct {
    uint32_t   key;
    uint8_t    vers[8];
    uint8_t    pLimitData[16];
    AsKeyInfo  keyInfo;
    uint8_t    result;
    uint8_t    status;
    uint8_t    data8;
    uint8_t    _pad;     // align data32 to 4-byte boundary
    uint32_t   data32;
    uint8_t    bytes[32];
} AsKeyData;  // total: 80 bytes

static io_connect_t g_as_smc_conn = 0;
static std::mutex  g_as_smc_mutex;
static std::vector<std::string> g_as_smc_cpu_keys;
static std::vector<std::string> g_as_smc_gpu_keys;
static std::unordered_map<std::string, AsKeyInfo> g_as_smc_key_info_cache;
static bool g_as_smc_ready = false;

static bool as_smc_open(void) {
    if (g_as_smc_conn != 0) return true;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    mach_port_t masterPort = kIOMasterPortDefault;
#pragma clang diagnostic pop

    io_iterator_t iter = 0;
    CFDictionaryRef match = IOServiceMatching("AppleSMC");
    if (!match) {
        // DEBUG REMOVED
        // fprintf(stderr, "[temp] AS SMC: IOServiceMatching returned NULL\n");
        return false;
    }
    kern_return_t kr = IOServiceGetMatchingServices(masterPort, match, &iter);
    if (kr != KERN_SUCCESS) {
        // DEBUG REMOVED
        // fprintf(stderr, "[temp] AS SMC: IOServiceGetMatchingServices failed kr=%d\n", kr);
        return false;
    }

    // Also try parent iteration to find nested children
    io_iterator_t childIter = 0;
    io_registry_entry_t entry;
    int foundCount = 0;

    // First pass: iterate top-level AppleSMC entries
    while ((entry = IOIteratorNext(iter)) != 0) {
        foundCount++;
        char nameBuf[128] = {};
        // Try IORegistryEntryGetName first (always works)
        IORegistryEntryGetName(entry, nameBuf);
        if (strcmp(nameBuf, "AppleSMCKeysEndpoint") == 0) {
            kr = IOServiceOpen(entry, mach_task_self(), 0, &g_as_smc_conn);
            IOObjectRelease(entry);
            break;
        }

        // Check children of this entry
        if (IORegistryEntryGetChildIterator(entry, kIOServicePlane, &childIter) == KERN_SUCCESS) {
            io_registry_entry_t child;
            int childIdx = 0;
            while ((child = IOIteratorNext(childIter)) != 0) {
                childIdx++;
                char cname[128] = {};
                IORegistryEntryGetName(child, cname);
                // DEBUG REMOVED
        // fprintf(stderr, "[temp] AS SMC:   child #%d name='%s'\n", childIdx, cname);
                if (strcmp(cname, "AppleSMCKeysEndpoint") == 0) {
                    kr = IOServiceOpen(child, mach_task_self(), 0, &g_as_smc_conn);
                    // DEBUG REMOVED
        // fprintf(stderr, "[temp] AS SMC: child IOServiceOpen kr=%d conn=%u\n", kr, g_as_smc_conn);
                    IOObjectRelease(child);
                    break;
                }
                IOObjectRelease(child);
            }
            IOObjectRelease(childIter);
            if (g_as_smc_conn != 0) { IOObjectRelease(entry); break; }
        }
        IOObjectRelease(entry);
    }
    IOObjectRelease(iter);

    // SMC scan done
    if (kr != KERN_SUCCESS || g_as_smc_conn == 0) {
        // DEBUG REMOVED
        // fprintf(stderr, "[temp] AS SMC: failed (kr=%d conn=%u)\n", kr, g_as_smc_conn);
        return false;
    }
    Logger::Info("TemperatureWrapper: AppleSMCKeysEndpoint opened for Apple Silicon");
    return true;
}

static kern_return_t as_smc_call(AsKeyData* in, AsKeyData* out) {
    size_t isz = sizeof(AsKeyData), osz = sizeof(AsKeyData);
    static int dbgOnce = 0;
    if (dbgOnce == 0) {
        // DEBUG REMOVED
        // fprintf(stderr, "[temp] as_smc_call: sizeof(AsKeyData)=%zu\n", isz);
        dbgOnce = 1;
    }
    return IOConnectCallStructMethod(g_as_smc_conn, 2, in, isz, out, &osz);
}

// Read key info (data8=9): fills keyInfo (dataSize, dataType, dataAttributes)
static bool as_smc_read_key_info(uint32_t keyWord, AsKeyInfo* info) {
    AsKeyData in = {};
    AsKeyData out = {};
    in.key = keyWord;
    in.data8 = 9;
    kern_return_t kr = as_smc_call(&in, &out);
    static int dbgCnt = 0;
    if (dbgCnt < 3) {
        dbgCnt++;
    }
    if (kr != KERN_SUCCESS || out.result != 0)
        return false;
    memcpy(info, &out.keyInfo, sizeof(AsKeyInfo));
    return (info->dataSize > 0 && info->dataSize <= 32);
}

// Read sensor value (data8=5): raw bytes in out.bytes[0..dataSize]
static bool as_smc_read_val(uint32_t keyWord, const AsKeyInfo* info,
                            uint8_t* outBytes, size_t outMax) {
    AsKeyData in = {};
    AsKeyData out = {};
    in.key = keyWord;
    in.data8 = 5;
    memcpy(&in.keyInfo, info, sizeof(AsKeyInfo));
    if (as_smc_call(&in, &out) != KERN_SUCCESS || out.result != 0)
        return false;
    size_t n = std::min(static_cast<size_t>(info->dataSize), outMax);
    memcpy(outBytes, out.bytes, n);
    return true;
}

// Get key name by index (data8=8, data32=index)
static bool as_smc_key_by_index(uint32_t index, char keyOut[5]) {
    AsKeyData in = {};
    AsKeyData out = {};
    in.data8 = 8;
    in.data32 = index;
    if (as_smc_call(&in, &out) != KERN_SUCCESS || out.result != 0)
        return false;
    uint32_t kw = out.key;  // key is in Big Endian
    keyOut[0] = (kw >> 24) & 0xFF;
    keyOut[1] = (kw >> 16) & 0xFF;
    keyOut[2] = (kw >>  8) & 0xFF;
    keyOut[3] =  kw        & 0xFF;
    keyOut[4] = '\0';
    return true;
}

// Decode temperature from raw bytes (assumes float32 big-endian, or sp78 fixed-point)
static double as_smc_decode_temp(const uint8_t* bytes, const AsKeyInfo* info) {
    if (!bytes || !info || info->dataSize < 2) return 0.0;
    // Try float32 big-endian first (macmon approach)
    uint32_t dt = info->dataType;
    char typeStr[5] = {};
    typeStr[0] = (dt >> 24) & 0xFF;
    typeStr[1] = (dt >> 16) & 0xFF;
    typeStr[2] = (dt >>  8) & 0xFF;
    typeStr[3] =  dt        & 0xFF;
    // "flt " = float32 little-endian (macmon: f32::from_le_bytes)
    if (strcmp(typeStr, "flt ") == 0 && info->dataSize >= 4) {
        float f;
        memcpy(&f, bytes, sizeof(f));  // direct memcpy on little-endian (ARM64)
        if (f > 10.0 && f < 150.0) return static_cast<double>(f);
    }
    // "sp78" = signed 16-bit 7.8 fixed-point big-endian (common for Intel SMC)
    if (strcmp(typeStr, "sp78") == 0 && info->dataSize >= 2) {
        int16_t raw = ((int16_t)bytes[0] << 8) | bytes[1];
        double val = static_cast<double>(raw) / 256.0;
        if (val > 0 && val < 150.0) return val;
    }
    // Generic fallback: try as float32 little-endian
    if (info->dataSize >= 4) {
        float f;
        memcpy(&f, bytes, sizeof(f));
        if (f > 10 && f < 150.0) return static_cast<double>(f);
    }
    return 0.0;
}

// Enumerate all SMC temperature keys (called once at init)
static void as_smc_enumerate(void) {
    std::lock_guard<std::mutex> lock(g_as_smc_mutex);
    if (g_as_smc_ready) return;

    // Read #KEY to get total key count
    AsKeyInfo keyInfo;
    uint32_t hashKey = 0x234b4559; // "#KEY" as FourCC
    // reading #KEY
    if (!as_smc_read_key_info(hashKey, &keyInfo)) {
        // DEBUG REMOVED
        // fprintf(stderr, "[temp] AS SMC: #KEY read_key_info FAILED\n");
        return;
    }
    // #KEY info read ok

    uint8_t keyBytes[32] = {};
    if (!as_smc_read_val(hashKey, &keyInfo, keyBytes, sizeof(keyBytes))) {
        // DEBUG REMOVED
        // fprintf(stderr, "[temp] AS SMC: #KEY read_val FAILED\n");
        return;
    }

    uint32_t totalKeys = ((uint32_t)keyBytes[0] << 24) | ((uint32_t)keyBytes[1] << 16)
                       | ((uint32_t)keyBytes[2] <<  8) | (uint32_t)keyBytes[3];
    Logger::Info("TemperatureWrapper: AS SMC has " + std::to_string(totalKeys) + " keys");
    if (totalKeys == 0 || totalKeys > 65535) return;

    g_as_smc_cpu_keys.clear();
    g_as_smc_gpu_keys.clear();

    int dbgLimit = 20;
    for (uint32_t i = 0; i < totalKeys; i++) {
        char k[5] = {};
        if (!as_smc_key_by_index(i, k)) continue;

        AsKeyInfo ki;
        uint32_t kw = ((uint32_t)(uint8_t)k[0] << 24) | ((uint32_t)(uint8_t)k[1] << 16)
                    | ((uint32_t)(uint8_t)k[2] <<  8) | (uint32_t)(uint8_t)k[3];
        if (!as_smc_read_key_info(kw, &ki)) continue;

        char dt[5] = {};
        dt[0] = (ki.dataType >> 24) & 0xFF;
        dt[1] = (ki.dataType >> 16) & 0xFF;
        dt[2] = (ki.dataType >>  8) & 0xFF;
        dt[3] =  ki.dataType        & 0xFF;

        if (dbgLimit > 0 && k[0] >= ' ' && k[1] >= ' ') {
            dbgLimit--;
        }

        if (ki.dataSize != 4) continue;

        // Tp=performance core, Te=efficiency core, Ts=super core (M5+), Tg=GPU
        if (k[0] == 'T' && k[1] == 'g') {
            g_as_smc_gpu_keys.push_back(std::string(k));
            g_as_smc_key_info_cache[std::string(k)] = ki;
        } else if (k[0] == 'T' && (k[1] == 'p' || k[1] == 'e' || k[1] == 's')) {
            g_as_smc_cpu_keys.push_back(std::string(k));
            g_as_smc_key_info_cache[std::string(k)] = ki;
        }
    }

    g_as_smc_ready = true;
    Logger::Info("TemperatureWrapper: AS SMC CPU keys="
                 + std::to_string(g_as_smc_cpu_keys.size())
                 + " GPU keys=" + std::to_string(g_as_smc_gpu_keys.size()));
}

// Read temperatures from Apple Silicon SMC (with cache for reliability)
static std::vector<std::pair<std::string, double>> g_as_smc_cached_temps;

static std::vector<std::pair<std::string, double>> as_smc_read_temps(void) {
    std::vector<std::pair<std::string, double>> temps;
    if (!g_as_smc_ready) return g_as_smc_cached_temps; // return last known good data

    std::lock_guard<std::mutex> lock(g_as_smc_mutex);

    auto readKey = [&](const std::string& k) -> double {
        uint32_t kw = ((uint32_t)(uint8_t)k[0] << 24) | ((uint32_t)(uint8_t)k[1] << 16)
                    | ((uint32_t)(uint8_t)k[2] <<  8) | (uint32_t)(uint8_t)k[3];
        auto it = g_as_smc_key_info_cache.find(k);
        if (it == g_as_smc_key_info_cache.end()) return 0.0;
        AsKeyData in = {}, out = {};
        in.key = kw; in.data8 = 5;
        memcpy(&in.keyInfo, &it->second, sizeof(AsKeyInfo));
        if (as_smc_call(&in, &out) != KERN_SUCCESS || out.result != 0) return 0.0;
        return as_smc_decode_temp(out.bytes, &it->second);
    };
    double cpuSum = 0.0; int cpuN = 0;
    double gpuSum = 0.0; int gpuN = 0;
    for (const auto& k : g_as_smc_cpu_keys) {
        double v = readKey(k);
        if (v > 10.0 && v < 150.0) { cpuSum += v; cpuN++; }
    }
    for (const auto& k : g_as_smc_gpu_keys) {
        double v = readKey(k);
        if (v > 10.0 && v < 150.0) { gpuSum += v; gpuN++; }
    }
    if (cpuN > 0) temps.push_back({"CPU Die", cpuSum / cpuN});
    if (gpuN > 0) temps.push_back({"GPU Die", gpuSum / gpuN});

    if (temps.size() >= 1)
        g_as_smc_cached_temps = temps;
    else if (!g_as_smc_cached_temps.empty())
        temps = g_as_smc_cached_temps;

    return temps;
}

// =====================================================================
// Background powermetrics caching thread (Intel only — needs root)
// powermetrics --samplers thermal -i N -n 1 outputs CPU/GPU die temps.
// =====================================================================
static std::vector<std::pair<std::string, double>> g_pm_temps;
static std::mutex  g_pm_mutex;
static std::atomic<bool> g_pm_running{false};
static std::atomic<bool> g_pm_available{false};
static std::thread g_pm_thread;
std::atomic<double> g_pm_pCoreFreq{0.0};
std::atomic<double> g_pm_eCoreFreq{0.0};
std::atomic<double> g_pm_gpuFreq{0.0};
std::atomic<double> g_pm_cpuPower{0.0};
std::atomic<double> g_pm_gpuPower{0.0};
std::atomic<double> g_pm_anePower{0.0};

double GetPmPCoreFreq() { return g_pm_pCoreFreq.load(); }
double GetPmECoreFreq() { return g_pm_eCoreFreq.load(); }
double GetPmGpuFreq()   { return g_pm_gpuFreq.load(); }
double GetPmCpuPower()  { return g_pm_cpuPower.load(); }
double GetPmGpuPower()  { return g_pm_gpuPower.load(); }
double GetPmAnePower()  { return g_pm_anePower.load(); }

// Parse the first floating-point number from a line (works for "XXX MHz", "XXX mW", etc.)
static double parse_number_line(const char* line) {
    if (!line) return 0;
    while (*line) {
        if (*line == '-' || *line == '.' || (*line >= '0' && *line <= '9')) {
            const char* start = line;
            while (*line == '-' || *line == '.' || (*line >= '0' && *line <= '9')) ++line;
            std::string s(start, static_cast<size_t>(line - start));
            double v = std::atof(s.c_str());
            if (v > 0) return v;
            continue;
        }
        ++line;
    }
    return 0;
}

// Parse "XX.YY C" or "XX.YY°C" from a line
static double parse_temp_line(const char* line) {
    if (!line) return 0;
    // Look for patterns: " <number> C" or " <number> °C"
    // UTF-8 degree sign: 0xC2 0xB0
    const char* p = line;
    while (*p) {
        // Skip whitespace
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;

        // Check if next non-space char is digit or minus
        if ((*p == '-' || (*p >= '0' && *p <= '9'))) {
            const char* numStart = p;
            // Skip to end of number
            while (*p == '-' || *p == '.' || (*p >= '0' && *p <= '9')) ++p;
            // Skip whitespace
            while (*p == ' ' || *p == '\t') ++p;
            // Check for "C"
            if (*p == 'C' && p > numStart) {
                std::string s(numStart, static_cast<size_t>(p - numStart));
                double v = std::atof(s.c_str());
                if (v > 0 && v < 200) return v;
            }
            // Check for "°C" (UTF-8: 0xC2 0xB0)
            if (p[0] == '\xC2' && p[1] == '\xB0' && p[2] == 'C' && p > numStart) {
                std::string s(numStart, static_cast<size_t>(p - numStart));
                double v = std::atof(s.c_str());
                if (v > 0 && v < 200) return v;
            }
        }
        ++p;
    }
    return 0;
}

static void powermetrics_thread_func(void) {
    Logger::Debug("TemperatureWrapper: powermetrics thread started");

    // Warm up: wait 5s before first run so system settles
    std::this_thread::sleep_for(std::chrono::seconds(5));

    // SIGALRM timeout helper: interrupt a blocking read
    // Signal is process-wide but safe since only this thread uses it
    sig_t oldAlrm = signal(SIGALRM, [](int) {});
    signal(SIGALRM, oldAlrm);  // restore; just want to test signal works

    while (g_pm_running.load()) {
        // Run powermetrics with default samplers (includes frequency + power on AS)
        // --samplers thermal is Intel-only; Apple Silicon uses default output
        alarm(15);
        FILE* fp = popen("/usr/bin/powermetrics -i 10000 -n 1 2>/dev/null", "r");
        if (!fp) {
            alarm(0);
            std::this_thread::sleep_for(std::chrono::seconds(30));
            continue;
        }

        std::vector<std::pair<std::string, double>> batch;
        char line[512] = {0};

        while (fgets(line, sizeof(line), fp)) {
            // Frequency: "E-Cluster HW active frequency: 1405 MHz"
            if (std::strstr(line, "E-Cluster HW active frequency:")) {
                double v = parse_number_line(line);
                if (v > 0) g_pm_eCoreFreq.store(v);
            }
            else if (std::strstr(line, "P-Cluster HW active frequency:")) {
                double v = parse_number_line(line);
                if (v > 0) g_pm_pCoreFreq.store(v);
            }
            // GPU frequency: "GPU HW active frequency: 482 MHz"
            else if (std::strstr(line, "GPU HW active frequency:")) {
                double v = parse_number_line(line);
                if (v > 0) g_pm_gpuFreq.store(v);
            }
            // Power: "CPU Power: 941 mW" / "GPU Power: 591 mW" / "ANE Power: 0 mW"
            else if (std::strstr(line, "CPU Power:")) {
                double v = parse_number_line(line);
                if (v > 0) { g_pm_cpuPower.store(v); batch.push_back({"CPU Power", v}); }
            }
            else if (std::strstr(line, "GPU Power:")) {
                double v = parse_number_line(line);
                if (v > 0) { g_pm_gpuPower.store(v); batch.push_back({"GPU Power", v}); }
            }
            else if (std::strstr(line, "ANE Power:")) {
                double v = parse_number_line(line);
                if (v >= 0) g_pm_anePower.store(v);
            }
            // Temperature (thermal sampler on Intel, ARM PMU on AS)
            else if (std::strstr(line, "CPU Die Temperature") ||
                     std::strstr(line, "GPU Die Temperature")) {
                double t = parse_temp_line(line);
                if (t > 10 && t < 150) {
                    std::string label = (std::strstr(line, "GPU")) ? "GPU Die" : "CPU Die";
                    batch.push_back({label, t});
                }
            }
        }
        alarm(0);
        pclose(fp);

        if (!batch.empty()) {
            std::lock_guard<std::mutex> lock(g_pm_mutex);
            g_pm_temps.swap(batch);
            g_pm_available = true;
            Logger::Debug("TemperatureWrapper: powermetrics cached "
                         + std::to_string(g_pm_temps.size()) + " sensors");
        }

        if (g_pm_running.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
        }
    }
    Logger::Debug("TemperatureWrapper: powermetrics thread stopped");
}

static void start_powermetrics_thread(void) {
    if (g_pm_running.load()) return;
    // Check if powermetrics is available
    static bool checked = false;
    static bool available = false;
    if (!checked) {
        FILE* fp = popen("/usr/bin/which powermetrics 2>/dev/null", "r");
        char buf[128] = {0};
        if (fp && fgets(buf, sizeof(buf), fp)) {
            if (std::strstr(buf, "powermetrics")) available = true;
        }
        if (fp) pclose(fp);
        checked = true;
    }
    if (!available) {
        Logger::Info("TemperatureWrapper: powermetrics not available (needs root)");
        return;
    }

    g_pm_running = true;
    g_pm_thread  = std::thread(powermetrics_thread_func);
}

// =====================================================================
// IOKit thermal sensors fallback (no root needed, but limited coverage)
// =====================================================================
static std::vector<std::pair<std::string, double>> iokit_hid_temps(void) {
    std::vector<std::pair<std::string, double>> temps;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    mach_port_t masterPort = kIOMasterPortDefault;
#pragma clang diagnostic pop

    io_iterator_t iter = 0;
    if (IOServiceGetMatchingServices(masterPort,
                                     IOServiceMatching("IOHIDEventService"),
                                     &iter) != KERN_SUCCESS)
        return temps;

    io_registry_entry_t entry;
    while ((entry = IOIteratorNext(iter)) != 0) {
        CFTypeRef ref = IORegistryEntryCreateCFProperty(
            entry, CFSTR("Temperature"), kCFAllocatorDefault, 0);
        if (ref) {
            if (CFGetTypeID(ref) == CFNumberGetTypeID()) {
                double tC = 0;
                if (CFNumberGetValue((CFNumberRef)ref, kCFNumberDoubleType, &tC)
                    && tC > 10 && tC < 150) {
                    temps.push_back({"HID Temperature", tC});
                }
            }
            CFRelease(ref);
        }
        IOObjectRelease(entry);
    }
    IOObjectRelease(iter);
    return temps;
}

// =====================================================================
// Apple Silicon ARM PMU Temp Sensors — no root needed
// AppleARMPMUTempSensor exposes CPU die temperature via IOKit
// =====================================================================
static std::vector<std::pair<std::string, double>> iokit_arm_temp_sensors(void) {
    std::vector<std::pair<std::string, double>> temps;

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    mach_port_t masterPort = kIOMasterPortDefault;
#pragma clang diagnostic pop

    io_iterator_t iter = 0;
    kern_return_t kr = IOServiceGetMatchingServices(
        masterPort,
        IOServiceMatching("AppleARMPMUTempSensor"),
        &iter);
    if (kr != KERN_SUCCESS) {
        Logger::Debug("TemperatureWrapper: AppleARMPMUTempSensor not found (kr="
                      + std::to_string(kr) + ")");
        return temps;
    }

    io_registry_entry_t entry;
    int idx = 0;
    while ((entry = IOIteratorNext(iter)) != 0) {
        // Try reading Temperature property
        CFTypeRef ref = IORegistryEntryCreateCFProperty(
            entry, CFSTR("Temperature"), kCFAllocatorDefault, 0);
        if (ref) {
            if (CFGetTypeID(ref) == CFNumberGetTypeID()) {
                double tC = 0;
                if (CFNumberGetValue((CFNumberRef)ref, kCFNumberDoubleType, &tC)
                    && tC > 10 && tC < 150) {
                    std::string label = (idx == 0) ? "CPU Die" : "CPU Die #" + std::to_string(idx);
                    temps.push_back({label, tC});
                }
            }
            CFRelease(ref);
        }
        // Also try reading via IOHIDEventService approach
        if (temps.empty()) {
            CFTypeRef svcRef = IORegistryEntryCreateCFProperty(
                entry, CFSTR("IOHIDEventService"), kCFAllocatorDefault, 0);
            if (svcRef) CFRelease(svcRef);
        }
        IOObjectRelease(entry);
        idx++;
    }
    IOObjectRelease(iter);
    return temps;
}

// =====================================================================
// Temperature keys to probe (Intel SMC only)
// =====================================================================
#ifdef __x86_64__
static const char* kCpuKeys[] = {
    // CPU proximity / package
    "TC0P", "TC0D", "TC0H", "TC0C",
    "TC1P", "TC2P", "TC3P", "TC4P",
    "TC5P", "TC6P", "TC7P", "TC8P",
    "TCFC", "TCGC", "TCSA", "TCXC",
    "TCXS", "TC0S", "TC1S",
    // Palm rest / ambient
    "Ts0P", "Ts1P", "Ts2P",
    // RAM / rail
    "Rp0T", "Rp1T", "Rp2T",
    // CPU efficiency / performance
    "TCED", "TC0E", "TC1E",
    NULL
};

static const char* kGpuKeys[] = {
    "TG0P", "TG0D", "TG0H",
    "TG1P", "TG1D",
    "TGDD", "TGSD",
    "GgTp", "GgTs",
    NULL
};
#endif // __x86_64__

// =====================================================================
// Battery temperature via AppleSmartBattery (no root needed)
// Temperature value is in centidegrees (×100 °C), e.g. 3018 → 30.18°C
// =====================================================================
static double iokit_battery_temp(void) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    mach_port_t masterPort = kIOMasterPortDefault;
#pragma clang diagnostic pop

    io_service_t svc = IOServiceGetMatchingService(
        masterPort, IOServiceMatching("AppleSmartBattery"));
    if (!svc) return 0.0;

    double result = 0.0;
    CFTypeRef ref = IORegistryEntryCreateCFProperty(
        svc, CFSTR("Temperature"), kCFAllocatorDefault, 0);
    if (ref && CFGetTypeID(ref) == CFNumberGetTypeID()) {
        SInt64 raw = 0;
        if (CFNumberGetValue((CFNumberRef)ref, kCFNumberSInt64Type, &raw)) {
            result = raw / 100.0;
        }
        if (ref) CFRelease(ref);
    }
    IOObjectRelease(svc);
    return result;
}

// =====================================================================
// Public interface
// =====================================================================
bool TemperatureWrapper::initialized = false;

void TemperatureWrapper::Initialize() {
    if (initialized) return;
    initialized = true;

    // Step 1: SMC (Intel or Apple Silicon, platform-appropriate)
#ifdef __x86_64__
    probe_smc();
#else
    if (as_smc_open()) {
        as_smc_enumerate();
        Logger::Info("TemperatureWrapper: AS SMC ready, CPU="
                     + std::to_string(g_as_smc_cpu_keys.size())
                     + " GPU=" + std::to_string(g_as_smc_gpu_keys.size()) + " keys");
    } else {
        // DEBUG REMOVED
        // fprintf(stderr, "[temp] AS SMC open failed\n");
    }
#endif

    // Step 2: Start powermetrics background thread (needs root)
    start_powermetrics_thread();

    Logger::Info("TemperatureWrapper: initialized");
}

void TemperatureWrapper::Cleanup() {
    if (!initialized) return;

    // Stop powermetrics thread
    if (g_pm_running.load()) {
        g_pm_running = false;
        if (g_pm_thread.joinable()) g_pm_thread.join();
    }

    // Close SMC
#ifdef __x86_64__
    if (g_smc_conn) {
        IOServiceClose(g_smc_conn);
        g_smc_conn = 0;
    }
#else
    if (g_as_smc_conn) {
        std::lock_guard<std::mutex> lock(g_as_smc_mutex);
        g_as_smc_ready = false;
        g_as_smc_cpu_keys.clear();
        g_as_smc_gpu_keys.clear();
        g_as_smc_key_info_cache.clear();
        g_as_smc_cached_temps.clear();
        IOServiceClose(g_as_smc_conn);
        g_as_smc_conn = 0;
    }
#endif

    initialized = false;
}

std::vector<std::pair<std::string, double>> TemperatureWrapper::GetTemperatures() {
    std::vector<std::pair<std::string, double>> temps;

    if (!initialized) return temps;

#ifdef __x86_64__
    // Priority 1: Intel SMC hardcoded keys
    for (int i = 0; kCpuKeys[i]; i++) {
        double t = 0;
        if (smc_read_temp(kCpuKeys[i], "sp78", &t))
            temps.push_back({std::string(kCpuKeys[i]) + " (CPU)", t});
    }
    for (int i = 0; kGpuKeys[i]; i++) {
        double t = 0;
        if (smc_read_temp(kGpuKeys[i], "sp78", &t))
            temps.push_back({std::string(kGpuKeys[i]) + " (GPU)", t});
    }
#else
    // Priority 1: Apple Silicon SMC (dynamically enumerated keys, no root)
    {
        auto as = as_smc_read_temps();
        temps.insert(temps.end(), as.begin(), as.end());
        // ARM PMU fallback if SMC gave nothing
        if (as.empty()) {
            auto arm = iokit_arm_temp_sensors();
            temps.insert(temps.end(), arm.begin(), arm.end());
        }
    }
#endif

    // IOKit HID (supplement if still few sensors)
    if (temps.size() < 2) {
        auto hid = iokit_hid_temps();
        temps.insert(temps.end(), hid.begin(), hid.end());
    }

    // powermetrics cache (needs root, supplements SMC)
    {
        std::lock_guard<std::mutex> lock(g_pm_mutex);
        if (!g_pm_temps.empty()) {
            temps.insert(temps.end(), g_pm_temps.begin(), g_pm_temps.end());
        }
    }

    // Battery temperature (always available on laptops)
    double batTemp = iokit_battery_temp();
    if (batTemp > 10 && batTemp < 80) {
        temps.push_back({"Battery", batTemp});
    }

    static int totalDbg = 0;
    if (totalDbg < 3) {
        // DEBUG REMOVED
        // fprintf(stderr, "[temp] GetTemperatures total: %zu sensors\n", temps.size());
        totalDbg++;
    }

    return temps;
}

bool TemperatureWrapper::IsInitialized() { return initialized; }

#else
#error "Unsupported platform"
#endif
