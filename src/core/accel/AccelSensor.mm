// AccelSensor.mm — BMI284 accelerometer on Apple Silicon
//
// Primary: SMJobBless helper (running as root) writes XYZ to POSIX SHM.
// Fallback: direct IOKit HID read (may fail with kIOReturnNotPermitted on macOS 15).
//
// The helper binary is embedded in the .app bundle:
//   Contents/Library/LaunchServices/com.tcmt.sensorhelper

#import <IOKit/IOKitLib.h>
#import <IOKit/hid/IOHIDDevice.h>
#import <IOKit/hid/IOHIDManager.h>
#import <CoreFoundation/CoreFoundation.h>
#import <ServiceManagement/ServiceManagement.h>
#import <Security/AuthorizationDB.h>
#import <Security/Authorization.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>

#include "AccelSensor.h"
#include "core/Utils/Logger.h"

// ---------------------------------------------------------------------------
// Constants (must match helper)
// ---------------------------------------------------------------------------
static const uint32_t kUsagePage  = 0xFF00;
static const uint32_t kUsageAccel = 3;
static const int      kReportLen  = 22;
static const int      kDataOffset = 6;

static const double kScale = 1.0 / (65536.0 * 9.80665);  // Q16 -> g

// ---------------------------------------------------------------------------
// Shared memory helpers
// ---------------------------------------------------------------------------
bool AccelSensor::TryShmRead() {
    if (shmPtr_ == nullptr) return false;

    AccelShm* shm = (AccelShm*)shmPtr_;
    if (shm->magic != kShmMagic) return false;

    // Volatile read — updateCount is the consistency frontier
    uint64_t uc = shm->updateCount;
    __sync_synchronize();
    if (uc == 0) return false;

    // Scale raw Q16 to g: 1 Q16 = 1/65536 of a unit, and BMI284 reports m/s²
    // so Q16 / 65536 / 9.80665 = g
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
// Direct IOKit read (user-space fallback)
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
// Constructor / Destructor
// ---------------------------------------------------------------------------
AccelSensor::AccelSensor() {
    // 1. Try opening existing SHM (helper already running)
    shmFd_ = shm_open(kShmName, O_RDONLY, 0);
    if (shmFd_ >= 0) {
        shmPtr_ = mmap(nullptr, kShmSize, PROT_READ, MAP_SHARED, shmFd_, 0);
        if (shmPtr_ && shmPtr_ != MAP_FAILED) {
            AccelShm* shm = (AccelShm*)shmPtr_;
            if (shm->magic == kShmMagic && shm->updateCount > 0) {
                data_.hasDevice = true;
                data_.valid = true;
                TryShmRead();
                return;  // SHM works — done
            }
        } else {
            shmPtr_ = nullptr;
        }
    }

    // 2. Try direct IOKit read (user-space — may be blocked on macOS 15)
    io_service_t service = FindAccelService();
    if (!service) return;  // no accelerometer hardware

    data_.hasDevice = true;
    IOHIDDeviceRef dev = IOHIDDeviceCreate(kCFAllocatorDefault, service);
    IOObjectRelease(service);
    if (!dev) return;

    IOReturn kr = IOHIDDeviceOpen(dev, 0);
    if (kr == kIOReturnSuccess) {
        uint8_t buf[64] = {};
        CFIndex len = sizeof(buf);
        kr = IOHIDDeviceGetReport(dev, kIOHIDReportTypeInput, 0, buf, &len);
        if (kr == kIOReturnSuccess && len >= (CFIndex)(kDataOffset + 12)) {
            int32_t rx, ry, rz;
            memcpy(&rx, buf + kDataOffset,      sizeof(rx));
            memcpy(&ry, buf + kDataOffset + 4,  sizeof(ry));
            memcpy(&rz, buf + kDataOffset + 8,  sizeof(rz));
            data_.x = (double)rx * kScale;
            data_.y = (double)ry * kScale;
            data_.z = (double)rz * kScale;
            data_.valid = true;
        }
        IOHIDDeviceClose(dev, 0);
    }
    CFRelease(dev);
}

AccelSensor::~AccelSensor() {
    CloseShm();
}

void AccelSensor::Refresh() {
    // Try SHM first (fast, no privileges needed)
    if (TryShmRead()) return;

    // If SHM exists but is stale, helper might not be running — trigger install
    if (shmPtr_ != nullptr) {
        // SHM is open but no data → helper might have died, re-install
        BlessHelper();
        return;
    }

    // No SHM at all — try to open it (helper may have started in background)
    shmFd_ = shm_open(kShmName, O_RDONLY, 0);
    if (shmFd_ >= 0) {
        shmPtr_ = mmap(nullptr, kShmSize, PROT_READ, MAP_SHARED, shmFd_, 0);
        if (shmPtr_ && shmPtr_ != MAP_FAILED) {
            TryShmRead();
            return;
        }
        shmPtr_ = nullptr;
    }
}

// ---------------------------------------------------------------------------
// SMJobBless — install & start the privileged helper
// ---------------------------------------------------------------------------
bool AccelSensor::BlessHelper() {
    CFStringRef helperID = CFSTR("com.tcmt.sensorhelper");

    // Avoid popping root dialog on every launch — check if helper already installed.
    // SMJobBless copies binary to /Library/PrivilegedHelperTools/ + registers with launchd.
    // Once installed, launchd auto-restarts it on crash/boot; SHM will appear shortly.
    {
        struct stat st;
        if (stat("/Library/PrivilegedHelperTools/com.tcmt.sensorhelper", &st) == 0) {
            return true;  // already installed, launchd manages lifecycle
        }
    }

    // Set up authorization for privileged helper install (first time only)
    AuthorizationRef authRef = NULL;
    AuthorizationItem authItem = {
        kSMRightBlessPrivilegedHelper, 0, NULL, 0
    };
    AuthorizationRights authRights = { 1, &authItem };
    AuthorizationFlags authFlags =
        kAuthorizationFlagDefaults
        | kAuthorizationFlagInteractionAllowed
        | kAuthorizationFlagExtendRights;

    OSStatus status = AuthorizationCreate(&authRights,
                                           kAuthorizationEmptyEnvironment,
                                           authFlags, &authRef);
    if (status != errAuthorizationSuccess) return false;

    CFErrorRef error = NULL;
    Boolean result = SMJobBless(kSMDomainSystemLaunchd,
                                 helperID, authRef, &error);

    if (error) {
        CFStringRef desc = CFErrorCopyDescription(error);
        if (desc) {
            char buf[256] = {};
            CFStringGetCString(desc, buf, sizeof(buf), kCFStringEncodingUTF8);
            Logger::Warn(std::string("SMJobBless failed: ") + buf);
            CFRelease(desc);
        }
        CFRelease(error);
    }

    AuthorizationFree(authRef, kAuthorizationFlagDefaults);

    if (result) {
        Logger::Info("SMJobBless: helper installed/started");
        return true;
    }
    return false;
}
