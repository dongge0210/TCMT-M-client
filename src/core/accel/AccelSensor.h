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

    // Direct async HID path (works on macOS 15+, no root needed)
    bool asyncStarted_ = false;
    std::thread asyncThread_;
    std::atomic<uint64_t> asyncUpdateCount_{0};
    std::atomic<int32_t> ax_{0}, ay_{0}, az_{0};
    bool StartAsyncHid();
    void StopAsyncHid();
    bool ReadAsyncSample();
};

static const uint32_t  kUsagePage  = 0xFF00;
static const uint32_t  kUsageAccel = 3;
static const int       kReportLen  = 22;
static const int       kDataOffset = 6;
static const double    kScale      = 1.0 / 65536.0;  // Q16 fixed-point -> g
