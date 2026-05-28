// AccelSensor.mm — BMI284 accelerometer on Apple Silicon
//
// Primary path: async HID callback via IOHIDDeviceRegisterInputReportWithTimeStampCallback.
//   Works on macOS 15+ WITHOUT root — the interrupt-driven input report kernel path
//   is NOT blocked by motionRestrictedService.
//   Requires waking AppleSPUHIDDriver with SensorPropertyReportingState=1 etc.
//
// Fallback: POSIX SHM from SMJobBless helper (legacy; helper may or may not be running).
//
// Architecture backed by research:
//   - olvvier/apple-silicon-accelerometer (MIT) — async HID + driver wake confirmed working
//   - macOS 15 motionRestrictedService blocks only synchronous IOHIDDeviceGetReport

#import <IOKit/IOKitLib.h>
#import <IOKit/hid/IOHIDDevice.h>
#import <CoreFoundation/CoreFoundation.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>

#include "AccelSensor.h"
#include "core/Utils/Logger.h"

// ---------------------------------------------------------------------------
// Async HID callback — fires on a background CFRunLoop for each input report.
// The extra uint64_t timestamp parameter is what distinguishes this from the
// plain IOHIDDeviceRegisterInputReportCallback (which is also blocked).
// ---------------------------------------------------------------------------
struct AsyncHidContext {
    std::atomic<uint64_t>* updateCount;
    std::atomic<int32_t>*  ax;
    std::atomic<int32_t>*  ay;
    std::atomic<int32_t>*  az;
    IOHIDDeviceRef         dev;
    CFRunLoopRef           runLoop;
};

static void on_input_report_ts(void* ctx, IOReturn result, void* sender,
                                IOHIDReportType type, uint32_t reportID,
                                uint8_t* report, CFIndex len,
                                uint64_t timestamp)
{
    (void)sender; (void)type; (void)reportID; (void)timestamp;
    if (result != kIOReturnSuccess) return;
    if (len < (CFIndex)(kDataOffset + 12)) return;

    auto* c = (AsyncHidContext*)ctx;
    int32_t rx, ry, rz;
    memcpy(&rx, report + kDataOffset,      sizeof(rx));
    memcpy(&ry, report + kDataOffset + 4,  sizeof(ry));
    memcpy(&rz, report + kDataOffset + 8,  sizeof(rz));
    c->ax->store(rx, std::memory_order_relaxed);
    c->ay->store(ry, std::memory_order_relaxed);
    c->az->store(rz, std::memory_order_relaxed);
    c->updateCount->fetch_add(1, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Wake the AppleSPUHIDDriver — sets properties that tell the driver to
// start delivering interrupt reports. Without this, the callback may never fire.
// ---------------------------------------------------------------------------
static bool wake_spu_driver(void) {
    CFMutableDictionaryRef matching = IOServiceMatching("AppleSPUHIDDriver");
    if (!matching) return false;

    io_iterator_t iter;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMasterPortDefault, matching, &iter);
    if (kr != KERN_SUCCESS) return false;

    bool woken = false;
    io_object_t driver;
    while ((driver = IOIteratorNext(iter)) != 0) {
        // Get parent AppleSPUHIDDevice to verify this is the accelerometer
        io_service_t parent = 0;
        IORegistryEntryGetParentEntry(driver, kIOServicePlane, &parent);
        if (!parent) { IOObjectRelease(driver); continue; }

        CFMutableDictionaryRef props = NULL;
        kr = IORegistryEntryCreateCFProperties(parent, &props, kCFAllocatorDefault, 0);
        if (kr == KERN_SUCCESS && props) {
            CFTypeRef upRef = CFDictionaryGetValue(props, CFSTR("PrimaryUsagePage"));
            CFTypeRef uRef  = CFDictionaryGetValue(props, CFSTR("PrimaryUsage"));
            int32_t up = 0, u = 0;
            if (upRef && CFGetTypeID(upRef) == CFNumberGetTypeID())
                CFNumberGetValue((CFNumberRef)upRef, kCFNumberSInt32Type, &up);
            if (uRef  && CFGetTypeID(uRef) == CFNumberGetTypeID())
                CFNumberGetValue((CFNumberRef)uRef,  kCFNumberSInt32Type, &u);

            if ((uint32_t)up == kUsagePage && (uint32_t)u == kUsageAccel) {
                // Found accelerometer — wake this driver
                int32_t one = 1;
                int32_t interval = 1000;  // 1000 µs = 1 ms interval
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

                Logger::Debug("Accel: woken AppleSPUHIDDriver (SensorPropertyReportingState=1, ReportInterval=1000)");
                woken = true;
            }
            CFRelease(props);
        }
        IOObjectRelease(parent);
        IOObjectRelease(driver);
        if (woken) break;
    }
    IOObjectRelease(iter);
    return woken;
}

// ---------------------------------------------------------------------------
// Find accelerometer IOService in IORegistry
// ---------------------------------------------------------------------------
static io_service_t FindAccelService() {
    CFMutableDictionaryRef matching = IOServiceMatching("AppleSPUHIDDevice");
    if (!matching) return 0;

    io_iterator_t iter;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMasterPortDefault, matching, &iter);
    if (kr != KERN_SUCCESS) return 0;

    io_service_t found = 0;
    io_object_t svc;
    while ((svc = IOIteratorNext(iter)) != 0) {
        CFMutableDictionaryRef props = NULL;
        kr = IORegistryEntryCreateCFProperties(svc, &props, kCFAllocatorDefault, 0);
        if (kr == KERN_SUCCESS && props) {
            CFTypeRef upRef = CFDictionaryGetValue(props, CFSTR("PrimaryUsagePage"));
            CFTypeRef uRef  = CFDictionaryGetValue(props, CFSTR("PrimaryUsage"));
            if (upRef && uRef &&
                CFGetTypeID(upRef) == CFNumberGetTypeID() &&
                CFGetTypeID(uRef)  == CFNumberGetTypeID())
            {
                int32_t up = 0, u = 0;
                CFNumberGetValue((CFNumberRef)upRef, kCFNumberSInt32Type, &up);
                CFNumberGetValue((CFNumberRef)uRef,  kCFNumberSInt32Type, &u);
                if ((uint32_t)up == kUsagePage && (uint32_t)u == kUsageAccel) {
                    IOObjectRetain(svc);
                    found = svc;
                }
            }
            CFRelease(props);
        }
        IOObjectRelease(svc);
        if (found) break;
    }
    IOObjectRelease(iter);
    return found;
}

// ---------------------------------------------------------------------------
// SHM helpers (legacy SMJobBless path)
// ---------------------------------------------------------------------------
bool AccelSensor::TryShmRead() {
    if (shmPtr_ == nullptr) return false;

    AccelShm* shm = (AccelShm*)shmPtr_;
    if (shm->magic != kShmMagic) return false;

    uint64_t uc = shm->updateCount;
    __sync_synchronize();
    if (uc == 0) return false;

    data_.x = (double)shm->x * kScale;
    data_.y = (double)shm->y * kScale;
    data_.z = (double)shm->z * kScale;
    data_.valid = true;
    return true;
}

void AccelSensor::CloseShm() {
    if (shmPtr_) {
        munmap(shmPtr_, kShmSize);
        shmPtr_ = nullptr;
    }
    if (shmFd_ >= 0) {
        close(shmFd_);
        shmFd_ = -1;
    }
}

// ---------------------------------------------------------------------------
// Async HID: background thread running CFRunLoop for HID callbacks
// ---------------------------------------------------------------------------
bool AccelSensor::StartAsyncHid() {
    // 1. Wake the SPU driver — critical for reports to start flowing
    wake_spu_driver();

    // 2. Find the accelerometer service
    io_service_t service = FindAccelService();
    if (!service) {
        Logger::Debug("Accel: no BMI284 hardware found");
        return false;
    }

    IOHIDDeviceRef dev = IOHIDDeviceCreate(kCFAllocatorDefault, service);
    IOObjectRelease(service);
    if (!dev) return false;

    IOReturn kr = IOHIDDeviceOpen(dev, 0);
    if (kr != kIOReturnSuccess) {
        Logger::Warn("Accel: IOHIDDeviceOpen failed (0x" + 
                     std::to_string(kr) + ")");
        CFRelease(dev);
        return false;
    }

    data_.hasDevice = true;

    // Store in context for the thread
    AsyncHidContext* ctx = new AsyncHidContext;
    ctx->updateCount = &asyncUpdateCount_;
    ctx->ax = &ax_;
    ctx->ay = &ay_;
    ctx->az = &az_;
    ctx->dev = dev;

    // Pre-allocate report buffer
    uint8_t* reportBuf = new uint8_t[kReportLen];

    // Allocate context on heap, thread owns it
    asyncStarted_ = true;
    asyncThread_ = std::thread([this, dev, ctx, reportBuf]() {
        pthread_setname_np("tcmt-accel-async");

        ctx->runLoop = CFRunLoopGetCurrent();

        // Register timestamped callback (NOT the plain one — different kernel path)
        IOHIDDeviceRegisterInputReportWithTimeStampCallback(dev, reportBuf, kReportLen,
                                                             on_input_report_ts, ctx);

        IOHIDDeviceScheduleWithRunLoop(dev, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);

        Logger::Info("Accel: async HID thread started");

        // Run loop — processes HID callbacks
        while (asyncStarted_) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.5, false);
        }

        // Cleanup
        IOHIDDeviceUnscheduleFromRunLoop(dev, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
        IOHIDDeviceClose(dev, 0);
        CFRelease(dev);
        delete[] reportBuf;
        delete ctx;
        Logger::Debug("Accel: async HID thread exited");
    });
    asyncThread_.detach();
    return true;
}

void AccelSensor::StopAsyncHid() {
    asyncStarted_ = false;
    // Thread will exit on next CFRunLoopRunInMode timeout
}

bool AccelSensor::ReadAsyncSample() {
    uint64_t uc = asyncUpdateCount_.load(std::memory_order_acquire);
    if (uc == 0) return false;

    int32_t rx = ax_.load(std::memory_order_relaxed);
    int32_t ry = ay_.load(std::memory_order_relaxed);
    int32_t rz = az_.load(std::memory_order_relaxed);

    data_.x = (double)rx * kScale;
    data_.y = (double)ry * kScale;
    data_.z = (double)rz * kScale;
    data_.valid = true;
    return true;
}

// ---------------------------------------------------------------------------
// Constructor — try SHM first (fast, if helper running), else start async HID
// ---------------------------------------------------------------------------
AccelSensor::AccelSensor() {
    // 1. Try opening existing SHM (helper already running from previous launch)
    shmFd_ = shm_open(kShmName, O_RDONLY, 0);
    if (shmFd_ >= 0) {
        shmPtr_ = mmap(nullptr, kShmSize, PROT_READ, MAP_SHARED, shmFd_, 0);
        if (shmPtr_ && shmPtr_ != MAP_FAILED) {
            AccelShm* shm = (AccelShm*)shmPtr_;
            if (shm->magic == kShmMagic && shm->updateCount > 0) {
                data_.hasDevice = true;
                data_.valid = true;
                TryShmRead();
                Logger::Info("Accel: reading from SHM (helper active)");
                return;
            }
        } else {
            shmPtr_ = nullptr;
        }
    }

    // 2. SHM not available — start direct async HID path (no root needed)
    Logger::Info("Accel: starting async HID path (no SMJobBless required)");
    if (StartAsyncHid()) {
        // Give the callback a moment to fire
        for (int i = 0; i < 20 && !ReadAsyncSample(); ++i) {
            usleep(50000); // 50ms * 20 = 1s total
        }
        if (data_.valid) {
            Logger::Info("Accel: async HID path working");
        } else {
            Logger::Warn("Accel: async HID path started but no data yet");
        }
    }
}

AccelSensor::~AccelSensor() {
    StopAsyncHid();
    CloseShm();
}

// ---------------------------------------------------------------------------
// Refresh — called periodically from main loop
// ---------------------------------------------------------------------------
void AccelSensor::Refresh() {
    // 1. Try SHM (fast, if helper running)
    if (TryShmRead()) return;

    // 2. Try async HID path
    if (asyncStarted_) {
        if (ReadAsyncSample()) return;

        // If async thread started but no data after initial warmup,
        // try re-waking the driver
        static int reWarmCount = 0;
        if (++reWarmCount >= 60) {  // ~30 seconds
            reWarmCount = 0;
            Logger::Debug("Accel: re-waking SPU driver");
            wake_spu_driver();
        }
        return;
    }

    // 3. No path active — try to start async HID (maybe it failed first time)
    //    Only try once per ~10 seconds to avoid log spam
    static int retryCount = 0;
    if (++retryCount >= 20) {
        retryCount = 0;
        Logger::Info("Accel: retrying async HID start");
        StartAsyncHid();
    }
}
