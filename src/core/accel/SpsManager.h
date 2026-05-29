// SpsManager.h — Unified Apple SPU sensor monitor
// Reads ALL sensor HID devices under AppleSPUHIDDevice:
//   0xFF00/3  → Gravity/attitude vector     (3×int32 Q16 → g)
//   0xFF00/9  → Gyroscope angular velocity  (3×int32 Q16 → deg/s)
//   0xFF00/4  → Ambient light sensor (ALS)
//   0x0020/138 → Lid angle                  (3-byte → degrees)
//   0xFF00/5  → BMI284 die temperature      (bytes 3-4 LE int16 Q8.8 → °C)
//   0xFF0C/1  → AppleVendorMotion_Motion    (5-byte SPU fusion heartbeat ~0.4Hz)
//   0xFF0C/5  → AppleVendorMotion_DeviceMotion6 (100-byte CMDeviceMotion, needs CoreMotion)
#pragma once

#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <thread>
#include <atomic>
#include <vector>
#include <memory>
#include <string>
#include <cstdint>

// ── Per-sensor data ──────────────────────────────────────────────────────

struct SpsSample3Axis {
    double x = 0.0, y = 0.0, z = 0.0;
    bool valid = false;
};

struct SpsSampleScalar {
    double value = 0.0;
    bool valid = false;
};

// ── Sensor descriptor and live state ────────────────────────────────────

enum class SpsType : uint8_t {
    Gravity,    // 0xFF00/3  Q16 → g
    Gyro,       // 0xFF00/9  Q16 → deg/s
    ALS,        // 0xFF00/4  122-byte raw
    Temp,       // 0xFF00/5  14-byte, temperature (BMI284 die or IMU ambient)
    LidAngle,   // 0x0020/138 3-byte → degrees
    ApplePriv1, // 0xFF0C/1  5-byte, AppleVendorMotion_Motion (SPU fusion heartbeat ~0.4Hz)
    ApplePriv5, // 0xFF0C/5  100-byte, AppleVendorMotion_DeviceMotion6 (needs CoreMotion)
    Unknown,    // fallback — raw hex dump
};

struct SpsSensor {
    SpsType     type        = SpsType::Unknown;
    uint32_t    usagePage   = 0;
    uint32_t    usage       = 0;
    std::string name;       // human-readable label

    // HID state
    IOHIDDeviceRef  dev         = nullptr;
    uint8_t*        reportBuf   = nullptr;
    CFIndex         reportLen   = 0;

    // Live sample (updated by async callback)
    std::atomic<uint64_t> updateCount{0};

    // 3-axis sensors
    std::atomic<int32_t> v1{0}, v2{0}, v3{0};

    // Scalar sensors
    std::atomic<int32_t> sv{0};

    SpsSample3Axis  sample3;   // decoded 3-axis value (after Refresh)
    SpsSampleScalar sample1;   // decoded scalar value (after Refresh)
};

// ── Manager ──────────────────────────────────────────────────────────────

class SpsManager {
public:
    SpsManager();
    ~SpsManager();

    /// Enumerate & open all SPU HID sensors; start the background CFRunLoop
    bool Start();

    /// Read latest samples from all sensors (fast, atomic loads)
    void Refresh();

    /// Access sensors
    const std::vector<std::unique_ptr<SpsSensor>>& Sensors() const { return sensors_; }

private:
    std::vector<std::unique_ptr<SpsSensor>> sensors_;
    bool running_ = false;
    std::thread thread_;

    // ── internal helpers ──
    void WakeAllDrivers();
    SpsType Identify(uint32_t page, uint32_t usage) const;
    static void HidCallback(void* ctx, IOReturn result, void* sender,
                            IOHIDReportType type, uint32_t reportID,
                            uint8_t* report, CFIndex len, uint64_t timestamp);
};
