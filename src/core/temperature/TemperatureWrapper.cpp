#include "TemperatureWrapper.h"
#include "../Utils/Logger.h"

#ifdef TCMT_WINDOWS
#pragma managed
#include "../Utils/LibreHardwareMonitorBridge.h"
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
    return temps;
}

bool TemperatureWrapper::IsInitialized() { return initialized; }

#elif defined(TCMT_MACOS)
// ======================== macOS Implementation ========================
// macOS temperature access options (no root):
//   1. SMC direct reads via IOKit AppleSMC (no root on Intel; Apple Silicon needs root)
//   2. powermetrics (needs root, caches via background thread)
//   3. IOKit IOHIDEventService (few sensors, unreliable)
//
// Priority: SMC > powermetrics cache > IOKit HID
// All three are tried; GetTemperatures() returns whatever succeeded.

#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
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

// =====================================================================
// SMC Reader — Direct IOKit access to AppleSMC
// =====================================================================
// SMC selectors (selector 5, subfunction in data8 field):
//   9  = getTotalNumber   → total key count
//   7  = getKeyFromIndex  → key string by index
//   2  = readKeyInfo      → key metadata (type, size)
//   5  = readKeyValue      → actual value data

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

static io_connect_t g_smc_conn = 0;
static std::mutex   g_smc_mutex;

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
    // dataSize is encoded in the lower 16 bits of keyInfo (varies by SMC version)
    info->dataSize = static_cast<uint32_t>(out.keyInfo & 0xFFFF);
    if (info->dataSize == 0) info->dataSize = static_cast<uint32_t>((out.keyInfo >> 16) & 0xFFFF);
    if (info->dataSize == 0) info->dataSize = static_cast<uint32_t>(out.status & 0xFFFF);
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
    SmcKeyInfoVal_t info;
    if (smc_read_key_info(g_smc_conn, key, &info) == kIOReturnSuccess && info.dataSize > 0) {
        if (smc_read_key_value(g_smc_conn, key, info.dataSize, val, sizeof(val)) == kIOReturnSuccess) {
            testTemp = smc_decode_temp(val, info.dataSize, "sp78");
        }
    }

    if (testTemp > 10 && testTemp < 150) {
        Logger::Info("TemperatureWrapper: AppleSMC open, TC0P="
                     + std::to_string(testTemp) + "C");
    } else {
        // Apple Silicon: SMC temperature keys may need root.
        // Keep connection open anyway; reads will return 0 gracefully.
        Logger::Info("TemperatureWrapper: AppleSMC connected (keys need root on Apple Silicon)");
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

// =====================================================================
// Background powermetrics caching thread
// powermetrics --samplers thermal -i N -n 1 outputs CPU/GPU die temps.
// Runs as a daemon; GetTemperatures() reads the cache. Falls back to
// empty when cache is stale or powermetrics is unavailable (no root).
// =====================================================================
static std::vector<std::pair<std::string, double>> g_pm_temps;
static std::mutex  g_pm_mutex;
static std::atomic<bool> g_pm_running{false};
static std::atomic<bool> g_pm_available{false};
static std::thread g_pm_thread;

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
        // Run powermetrics for 10s sampling interval, output 1 sample
        alarm(12);  // 12s hard timeout — fires if powermetrics blocks
        FILE* fp = popen("/usr/bin/powermetrics --samplers thermal -i 10000 -n 1 2>/dev/null", "r");
        if (!fp) {
            alarm(0);
            std::this_thread::sleep_for(std::chrono::seconds(30));
            continue;
        }

        std::vector<std::pair<std::string, double>> batch;
        char line[512] = {0};
        [[maybe_unused]] bool in_thermal = false;

        while (fgets(line, sizeof(line), fp)) {
            if (std::strstr(line, "Thermal pressure")) {
                in_thermal = true;
            }
            if (std::strstr(line, "CPU Die Temperature") ||
                std::strstr(line, "GPU Die Temperature") ||
                std::strstr(line, "CPU Die") ||
                std::strstr(line, "GPU Die")) {
                double t = parse_temp_line(line);
                if (t > 10 && t < 150) {
                    std::string label = (std::strstr(line, "GPU")) ? "GPU Die" : "CPU Die";
                    batch.push_back({label, t});
                }
            }
            if (std::strlen(line) > 2 && std::strstr(line, "PLimitData") != nullptr) {
                in_thermal = false;
            }
        }
        alarm(0);  // cancel SIGALRM
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
// Temperature keys to probe on Apple Silicon (no root for most keys)
// =====================================================================
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

    // Step 1: Try direct SMC (works without root on Intel; may need root on AS)
    probe_smc();

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
    if (g_smc_conn) {
        IOServiceClose(g_smc_conn);
        g_smc_conn = 0;
    }

    initialized = false;
}

std::vector<std::pair<std::string, double>> TemperatureWrapper::GetTemperatures() {
    std::vector<std::pair<std::string, double>> temps;

    if (!initialized) return temps;

    // Priority 1: Direct SMC reads (works on Intel; needs root on Apple Silicon)
    for (int i = 0; kCpuKeys[i]; i++) {
        double t = 0;
        if (smc_read_temp(kCpuKeys[i], "sp78", &t)) {
            temps.push_back({std::string(kCpuKeys[i]) + " (CPU)", t});
        }
    }
    for (int i = 0; kGpuKeys[i]; i++) {
        double t = 0;
        if (smc_read_temp(kGpuKeys[i], "sp78", &t)) {
            temps.push_back({std::string(kGpuKeys[i]) + " (GPU)", t});
        }
    }

    // Priority 2: powermetrics cache (needs root, filled by background thread)
    if (temps.empty()) {
        std::lock_guard<std::mutex> lock(g_pm_mutex);
        if (!g_pm_temps.empty()) {
            temps = g_pm_temps;
        }
    }

    // Priority 3: Battery temperature (no root, always available on laptops)
    double batTemp = iokit_battery_temp();
    if (batTemp > 10 && batTemp < 80) {
        temps.push_back({"Battery", batTemp});
    }

    // Priority 4: Apple Silicon ARM PMU temp sensors (no root, Apple Silicon only)
    {
        auto arm = iokit_arm_temp_sensors();
        temps.insert(temps.end(), arm.begin(), arm.end());
    }

    // Priority 5: IOKit HID (last resort)
    if (temps.empty()) {
        auto hid = iokit_hid_temps();
        temps.insert(temps.end(), hid.begin(), hid.end());
    }

    return temps;
}

bool TemperatureWrapper::IsInitialized() { return initialized; }

#else
#error "Unsupported platform"
#endif
