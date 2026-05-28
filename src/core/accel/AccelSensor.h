// AccelSensor.h — BMI284 accelerometer via async HID callback + SHM fallback
#pragma once

#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <thread>
#include <atomic>
#include <cstdint>

struct AccelData {
    double x = 0.0, y = 0.0, z = 0.0;  // acceleration in g
    bool valid = false;                 // true if a sample was read
    bool hasDevice = false;             // true if accelerometer present
};

class AccelSensor {
public:
    AccelSensor();
    ~AccelSensor();
    void Refresh();
    const AccelData& GetData() const { return data_; }

private:
    AccelData data_;

    // SHM path (legacy SMJobBless helper, if installed)
    int shmFd_ = -1;
    void* shmPtr_ = nullptr;
    bool TryShmRead();
    void CloseShm();

    // Direct async HID path (works on macOS 15+, no root needed)
    bool asyncStarted_ = false;
    std::thread asyncThread_;
    std::atomic<uint64_t> asyncUpdateCount_{0};
    std::atomic<int32_t> ax_{0}, ay_{0}, az_{0};
    bool StartAsyncHid();
    void StopAsyncHid();
    void AsyncHidThread();
    bool ReadAsyncSample();
};

// Shared memory layout (must match helper)
#pragma pack(push, 1)
struct AccelShm {
    uint64_t            magic;
    volatile uint64_t   updateCount;
    int32_t             x, y, z;
    double              timestamp;
};
#pragma pack(pop)

static const char*     kShmName   = "tcmt-accel";
static const uint64_t  kShmMagic  = 0x54434D544143434CULL;  // "TCMTACCL"
static const int       kShmSize   = 64;
static const uint32_t  kUsagePage = 0xFF00;
static const uint32_t  kUsageAccel = 3;
static const int       kReportLen  = 22;
static const int       kDataOffset = 6;
static const double    kScale      = 1.0 / (65536.0 * 9.80665);  // Q16 -> g
