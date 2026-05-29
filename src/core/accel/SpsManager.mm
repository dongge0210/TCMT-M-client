// SpsManager.mm — Apple SPU unified sensor monitor
#import <CoreFoundation/CoreFoundation.h>
#import <IOKit/IOKitLib.h>
#import <IOKit/hid/IOHIDDevice.h>
#include "SpsManager.h"
#include "core/Utils/Logger.h"
#include <cstdio>
#include <cstring>
#include <cmath>

// ── Known sensor identities ──────────────────────────────────────────────

static constexpr uint32_t PAGE_VENDOR  = 0xFF00;
static constexpr uint32_t PAGE_SENSOR  = 0x0020;
static constexpr uint32_t PAGE_APPLE2  = 0xFF0C; // Apple private vendor page

// ── Construction / destruction ───────────────────────────────────────────

SpsManager::SpsManager() = default;
SpsManager::~SpsManager() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

// ── Identify sensor type by usage page+usage ────────────────────────────

SpsType SpsManager::Identify(uint32_t page, uint32_t usage) const {
    if (page == PAGE_VENDOR) {
        switch (usage) {
            case 3:  return SpsType::Gravity;
            case 9:  return SpsType::Gyro;
            case 4:  return SpsType::ALS;
            case 5:  return SpsType::Temp; // BMI284 die temperature or IMU ambient
            default: return SpsType::Unknown;
        }
    }
    if (page == PAGE_SENSOR && usage == 138) return SpsType::LidAngle;
    if (page == PAGE_APPLE2) {
        // AppleHIDUsageTables: 0xFF0C = kHIDPage_AppleVendorMotion
        //   usage 1 = kHIDUsage_AppleVendorMotion_Motion (heartbeat)
        //   usage 5 = kHIDUsage_AppleVendorMotion_DeviceMotion6 (fusion)
        switch (usage) {
            case 1:  return SpsType::ApplePriv1;
            case 5:  return SpsType::ApplePriv5;
            default: return SpsType::Unknown;
        }
    }
    return SpsType::Unknown;
}

static const char* SensorLabel(SpsType t) {
    switch (t) {
        case SpsType::Gravity:   return "Gravity";
        case SpsType::Gyro:      return "Gyro";
        case SpsType::ALS:       return "ALS";
        case SpsType::Temp:      return "Temp";
        case SpsType::LidAngle:  return "LidAngle";
        case SpsType::ApplePriv1: return "ApplePriv1";
        case SpsType::ApplePriv5: return "ApplePriv5";
        default:                 return "Unknown";
    }
}

// ── Wake ALL AppleSPUHIDDriver instances ─────────────────────────────────

void SpsManager::WakeAllDrivers() {
    CFMutableDictionaryRef matching = IOServiceMatching("AppleSPUHIDDriver");
    if (!matching) return;

    io_iterator_t iter;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMasterPortDefault, matching, &iter);
    if (kr != KERN_SUCCESS) return;

    io_object_t driver;
    while ((driver = IOIteratorNext(iter)) != 0) {
        int32_t one = 1, interval = 1000;
        CFNumberRef val;

        val = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &one);
        IORegistryEntrySetCFProperty(driver, CFSTR("SensorPropertyReportingState"), val);
        CFRelease(val);

        val = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &one);
        IORegistryEntrySetCFProperty(driver, CFSTR("SensorPropertyPowerState"), val);
        CFRelease(val);

        val = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &interval);
        IORegistryEntrySetCFProperty(driver, CFSTR("ReportInterval"), val);
        CFRelease(val);

        IOObjectRelease(driver);
    }
    IOObjectRelease(iter);
    Logger::Debug("SpsManager: woken all AppleSPUHIDDriver instances");
}

// ── Static HID callback ─────────────────────────────────────────────────

void SpsManager::HidCallback(void* ctx, IOReturn result, void* sender,
                              IOHIDReportType type, uint32_t reportID,
                              uint8_t* report, CFIndex len, uint64_t timestamp)
{
    (void)sender; (void)type; (void)reportID; (void)timestamp;
    if (result != kIOReturnSuccess || !ctx) return;
    auto* s = (SpsSensor*)ctx;

    switch (s->type) {
    case SpsType::Gravity:
    case SpsType::Gyro: {
        if (len < 18) return;
        int32_t v1, v2, v3;
        memcpy(&v1, report + 6, 4);
        memcpy(&v2, report + 10, 4);
        memcpy(&v3, report + 14, 4);
        s->v1.store(v1, std::memory_order_relaxed);
        s->v2.store(v2, std::memory_order_relaxed);
        s->v3.store(v3, std::memory_order_relaxed);
        s->updateCount.fetch_add(1, std::memory_order_release);
        break;
    }
    case SpsType::ALS: {
        if (len < 4) return;
        int32_t raw;
        memcpy(&raw, report, 4);
        s->sv.store(raw, std::memory_order_relaxed);
        s->updateCount.fetch_add(1, std::memory_order_release);
        break;
    }
    case SpsType::Temp: {
        // 14-byte report: byte0=seq, byte1=frameCnt, bytes 3-4 = LE int16 Q8.8 °C
        //   recorded: 0x1E80 → 30.5°C (stable IMU die temp at idle)
        if (len < 5) return;
        int16_t raw;
        memcpy(&raw, report + 3, 2);
        s->sv.store(raw, std::memory_order_relaxed); // store as Q8.8 fixed-point * 256
        s->updateCount.fetch_add(1, std::memory_order_release);
        break;
    }
    case SpsType::LidAngle: {
        if (len < 3 || report[0] != 1) return;
        uint16_t raw = (uint16_t)report[1] | ((uint16_t)report[2] << 8);
        s->sv.store(raw & 0x1FF, std::memory_order_relaxed);
        s->updateCount.fetch_add(1, std::memory_order_release);
        break;
    }
    case SpsType::ApplePriv1: {
        // kHIDUsage_AppleVendorMotion_Motion — SPU fusion pipeline heartbeat
        // report[4] = monotonic heartbeat counter (increments ~2.5s independent of motion)
        // report[0] = event flag (0x03=pair start, 0x02=pair end, 0x50=init)
        if (len < 5) return;
        s->sv.store(report[4], std::memory_order_relaxed);
        s->v1.store(report[0], std::memory_order_relaxed); // event flag
        s->v2.store(report[1], std::memory_order_relaxed); // event sub-type
        s->updateCount.fetch_add(1, std::memory_order_release);
        break;
    }
    case SpsType::ApplePriv5: {
        // kHIDUsage_AppleVendorMotion_DeviceMotion6 — 100-byte CMDeviceMotion fusion
        // Only fires when a CoreMotion consumer (e.g. startDeviceMotionUpdates) is active.
        // For now: count reports and store first 4 bytes as raw int32.
        if (len >= 4) {
            int32_t raw; memcpy(&raw, report, 4);
            s->sv.store(raw, std::memory_order_relaxed);
            s->updateCount.fetch_add(1, std::memory_order_release);
        }
        break;
    }
    case SpsType::Unknown: {
        // Dump first 5 raw reports for unknown sensors
        static std::atomic<int> dumpCnt[32];
        int idx = ((uintptr_t)s >> 4) & 0x1F;
        if (dumpCnt[idx].fetch_add(1, std::memory_order_relaxed) < 5) {
            char hex[256]; int pos = 0;
            for (CFIndex i = 0; i < len && pos < 250; i++)
                pos += snprintf(hex + pos, sizeof(hex) - pos, "%02x ", (unsigned)report[i]);
            Logger::Info(std::string("SpsRaw [") + s->name + "] len=" + std::to_string(len) +
                         " hex=" + hex);
        }
        // Also store first 4 bytes as int32
        if (len >= 4) {
            int32_t raw; memcpy(&raw, report, 4);
            s->sv.store(raw, std::memory_order_relaxed);
            s->updateCount.fetch_add(1, std::memory_order_release);
        }
        break;
    }
    }
}

// ── Enumerate, open, start background thread ────────────────────────────

bool SpsManager::Start() {
    WakeAllDrivers();

    // Enumerate AppleSPUHIDDevice
    CFMutableDictionaryRef matching = IOServiceMatching("AppleSPUHIDDevice");
    if (!matching) { Logger::Error("SpsManager: IOServiceMatching failed"); return false; }

    io_iterator_t iter;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMasterPortDefault, matching, &iter);
    if (kr != KERN_SUCCESS) { Logger::Error("SpsManager: no SPU devices"); return false; }

    io_object_t svc;
    while ((svc = IOIteratorNext(iter)) != 0) {
        CFMutableDictionaryRef props = nullptr;
        kr = IORegistryEntryCreateCFProperties(svc, &props, kCFAllocatorDefault, 0);
        if (kr != KERN_SUCCESS || !props) { IOObjectRelease(svc); continue; }

        auto cfint = [&](const char* key) -> uint32_t {
            CFStringRef cfk = CFStringCreateWithCString(kCFAllocatorDefault, key, kCFStringEncodingUTF8);
            if (!cfk) return 0;
            CFTypeRef ref = CFDictionaryGetValue(props, cfk);
            CFRelease(cfk);
            if (!ref || CFGetTypeID(ref) != CFNumberGetTypeID()) return 0;
            int32_t v = 0; CFNumberGetValue((CFNumberRef)ref, kCFNumberSInt32Type, &v);
            return (uint32_t)v;
        };

        uint32_t page = cfint("PrimaryUsagePage");
        uint32_t usage = cfint("PrimaryUsage");

        auto s = std::make_unique<SpsSensor>();
        s->usagePage = page;
        s->usage = usage;
        s->type = Identify(page, usage);

        // Build label
        char label[64];
        if (s->type != SpsType::Unknown)
            snprintf(label, sizeof(label), "%s (0x%X/%u)", SensorLabel(s->type), page, usage);
        else
            snprintf(label, sizeof(label), "Unknown (0x%X/%u)", page, usage);
        s->name = label;

        // Open HID device
        IOHIDDeviceRef dev = IOHIDDeviceCreate(kCFAllocatorDefault, svc);
        CFRelease(props);
        IOObjectRelease(svc);
        if (!dev) { Logger::Warn("SpsManager: IOHIDDeviceCreate failed for " + s->name); continue; }

        kr = IOHIDDeviceOpen(dev, 0);
        if (kr != kIOReturnSuccess) {
            Logger::Warn("SpsManager: IOHIDDeviceOpen failed for " + s->name + " kr=" + std::to_string(kr));
            CFRelease(dev);
            continue;
        }

        s->dev = dev;

        // Allocate report buffer
        CFIndex bufSize = 128;
        s->reportBuf = new uint8_t[bufSize];
        s->reportLen = bufSize;

        Logger::Info("SpsManager: opened " + s->name);
        sensors_.push_back(std::move(s));
    }
    IOObjectRelease(iter);

    if (sensors_.empty()) {
        Logger::Warn("SpsManager: no sensors opened");
        return false;
    }

    // Background thread with CFRunLoop
    running_ = true;
    thread_ = std::thread([this]() {
        pthread_setname_np("tcmt-sps");
        for (auto& s : sensors_) {
            if (!s->dev) continue;
            IOHIDDeviceRegisterInputReportWithTimeStampCallback(
                s->dev, s->reportBuf, s->reportLen, HidCallback, (void*)s.get());
            IOHIDDeviceScheduleWithRunLoop(s->dev, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
        }
        Logger::Info("SpsManager: background thread running (" + std::to_string(sensors_.size()) + " sensors)");
        while (running_)
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.5, false);
        // cleanup
        for (auto& s : sensors_) {
            if (s->dev) {
                IOHIDDeviceUnscheduleFromRunLoop(s->dev, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
                IOHIDDeviceClose(s->dev, 0);
                CFRelease(s->dev);
                s->dev = nullptr;
            }
            delete[] s->reportBuf;
            s->reportBuf = nullptr;
        }
        Logger::Debug("SpsManager: thread exited");
    });
    thread_.detach();
    return true;
}

// ── Refresh ─────────────────────────────────────────────────────────────

void SpsManager::Refresh() {
    for (auto& s : sensors_) {
        if (s->updateCount.load(std::memory_order_acquire) == 0) continue;

        switch (s->type) {
        case SpsType::Gravity: {
            int32_t v1 = s->v1.load(std::memory_order_relaxed);
            int32_t v2 = s->v2.load(std::memory_order_relaxed);
            int32_t v3 = s->v3.load(std::memory_order_relaxed);
            double scale = 1.0 / 65536.0;
            double x = (double)v1 * scale;
            double y = (double)v2 * scale;
            double z = (double)v3 * scale;
            // Dead zone: stationary axes (X/Y when flat) can show ±0.06g noise from DMP
            if (fabs(x) < 0.015) x = 0.0;
            if (fabs(y) < 0.015) y = 0.0;
            // Don't dead-zone Z (~1g at rest)
            s->sample3 = { x, y, z, true };
            break;
        }
        case SpsType::Gyro: {
            int32_t v1 = s->v1.load(std::memory_order_relaxed);
            int32_t v2 = s->v2.load(std::memory_order_relaxed);
            int32_t v3 = s->v3.load(std::memory_order_relaxed);
            double scale = 1.0 / 65536.0;
            double x = (double)v1 * scale;
            double y = (double)v2 * scale;
            double z = (double)v3 * scale;
            // Dead zone: BMI284 gyro LSB is 0.061°/s (±2000°/s range, 16-bit)
            // At rest the quantization noise shows as ±0.06 steps
            double deadZone = 0.1; // one LSB = 0.061, so 0.1 cleans it up
            if (fabs(x) < deadZone) x = 0.0;
            if (fabs(y) < deadZone) y = 0.0;
            if (fabs(z) < deadZone) z = 0.0;
            s->sample3 = { x, y, z, true };
            break;
        }
        case SpsType::Temp: {
            int32_t raw = s->sv.load(std::memory_order_relaxed);
            s->sample1 = { (double)raw / 256.0, true }; // Q8.8 fixed point → °C
            break;
        }
        case SpsType::LidAngle: {
            int32_t raw = s->sv.load(std::memory_order_relaxed);
            s->sample1 = { (double)raw, true }; // raw lid angle (0-511 → 0-360°?)
            break;
        }
        case SpsType::ALS: {
            int32_t raw = s->sv.load(std::memory_order_relaxed);
            s->sample1 = { (double)raw, true }; // raw ALS
            break;
        }
        case SpsType::ApplePriv1: {
            int32_t sv = s->sv.load(std::memory_order_relaxed);
            s->sample1 = { (double)sv, true }; // sv = heartbeat counter byte
            break;
        }
        case SpsType::ApplePriv5: {
            int32_t sv = s->sv.load(std::memory_order_relaxed);
            s->sample1 = { (double)sv, true }; // raw fusion data first 4 bytes
            break;
        }
        default:
            break;
        }
    }
}
