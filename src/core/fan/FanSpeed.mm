// FanSpeed.mm — macOS fan speed via IOKit AppleSMC (Intel) / AppleFan (AS)
#include "FanSpeed.h"
#include "../Utils/Logger.h"

#import <IOKit/IOKitLib.h>
#import <CoreFoundation/CoreFoundation.h>

#if defined(__x86_64__)
// ===== Intel Mac: AppleSMC direct access (FNum, F{n}Ac) =====
// Uses the same SMC mechanism as TemperatureWrapper

static io_connect_t g_smc_fan_conn = 0;

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

enum {
    KSmcReadKeyInfo  = 2,
    KSmcReadKeyValue = 5,
};

static kern_return_t open_smc(io_connect_t* conn) {
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

static kern_return_t smc_call(io_connect_t conn, SmcKeyData_t* in, SmcKeyData_t* out) {
    size_t outSize = sizeof(SmcKeyData_t);
    return IOConnectCallStructMethod(conn, 2, in, sizeof(SmcKeyData_t), out, &outSize);
}

static uint32_t smc_strkey(const char* key) {
    uint32_t k = 0;
    for (int i = 0; i < 4 && key[i]; i++)
        k = (k << 8) | (uint8_t)key[i];
    return k;
}

static float smc_read_rpm(io_connect_t conn, const char* key) {
    SmcKeyData_t in = {};
    in.key = smc_strkey(key);
    in.data8 = KSmcReadKeyInfo;
    SmcKeyData_t out = {};
    if (smc_call(conn, &in, &out) != kIOReturnSuccess) return 0;
    // Read actual value
    in.key = smc_strkey(key);
    in.data8 = KSmcReadKeyValue;
    memset(&out, 0, sizeof(out));
    if (smc_call(conn, &out, &out) != kIOReturnSuccess) return 0;
    // RPM value is stored in first 2 bytes as big-endian uint16
    float rpm = ((uint8_t)out.data[0] << 8) | (uint8_t)out.data[1];
    return rpm;
}

static int smc_read_fan_count(io_connect_t conn) {
    SmcKeyData_t in = {};
    in.key = smc_strkey("FNum");
    in.data8 = KSmcReadKeyValue;
    SmcKeyData_t out = {};
    if (smc_call(conn, &in, &out) != kIOReturnSuccess) return 0;
    return (uint8_t)out.data[0];
}

#endif // __x86_64__

void FanSpeed::Detect() {
    fans_.clear();

#if defined(__x86_64__)
    // Intel Mac: read fan speeds via AppleSMC
    static bool smcOpened = false;
    if (!smcOpened) {
        if (open_smc(&g_smc_fan_conn) == kIOReturnSuccess) {
            smcOpened = true;
            Logger::Info("FanSpeed: AppleSMC opened for fan reading");
        } else {
            Logger::Debug("FanSpeed: AppleSMC not available for fan reading");
            return;
        }
    }

    int fanCount = smc_read_fan_count(g_smc_fan_conn);
    if (fanCount < 0 || fanCount > 6) fanCount = 0;

    for (int i = 0; i < fanCount; i++) {
        char key[5];
        snprintf(key, sizeof(key), "F%dAc", i);
        float rpm = smc_read_rpm(g_smc_fan_conn, key);
        FanSpeedEntry e;
        e.name = "Fan " + std::to_string(i);
        e.rpm = rpm;
        fans_.push_back(e);
    }
#else
    // Apple Silicon: SMC hardware-locked, fans not accessible without root.
    // On AS Macs, IOReport framework may expose fan data under energy model
    // but this requires deep private framework integration.
    // For now, return empty — fan data only available on Intel Macs.
    (void)0;
#endif
}
