// tcmt_sensor_helper.c — SMJobBless privileged helper for BMI284 accelerometer
// Runs as root via launchd. Creates POSIX shared memory, writes XYZ at HID rate.
//
// Uses IOHIDDeviceRegisterInputReportWithTimeStampCallback (interrupt-driven input
// report path — NOT blocked by macOS 15 motionRestrictedService) and wakes the
// AppleSPUHIDDriver by setting SensorPropertyReportingState=1, etc.
//
// Compile: cc -o tcmt_sensor_helper tcmt_sensor_helper.c \
//              -framework IOKit -framework CoreFoundation
// Link with: -Wl,-sectcreate,__TEXT,__info_plist,com.tcmt.sensorhelper.plist

#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDDevice.h>
#include <CoreFoundation/CoreFoundation.h>
#include <signal.h>
#include <string.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <mach/mach_time.h>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static const char*     kShmName      = "tcmt-accel";
static const int       kShmSize      = 64;
static const uint64_t  kShmMagic     = 0x54434D544143434CULL;  // "TCMTACCL"

static const uint32_t  kUsagePage    = 0xFF00;
static const uint32_t  kUsageAccel   = 3;
static const int       kReportLen    = 22;
static const int       kDataOffset   = 6;
static const int       kReportBufSz  = 64;

// ---------------------------------------------------------------------------
// Shared memory layout
// ---------------------------------------------------------------------------
typedef struct __attribute__((packed)) {
    uint64_t            magic;
    volatile uint64_t   updateCount;
    int32_t             x, y, z;
    double              timestamp;
} AccelShm;

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------
static IOHIDDeviceRef  g_hidDev    = NULL;
static int             g_shmFd     = -1;
static AccelShm*       g_shm       = NULL;
static volatile sig_atomic_t g_done = 0;

// ---------------------------------------------------------------------------
// Signal handler
// ---------------------------------------------------------------------------
static void handle_signal(int sig) {
    fprintf(stderr, "tcmt_sensor_helper: received signal %d\n", sig);
    g_done = 1;
    CFRunLoopRef rl = CFRunLoopGetCurrent();
    if (rl) CFRunLoopStop(rl);
}

// ---------------------------------------------------------------------------
// HID input report callback — timestamped variant (not blocked by
// motionRestrictedService on macOS 15+)
// ---------------------------------------------------------------------------
static void on_input_report_ts(void* ctx, IOReturn result, void* sender,
                                IOHIDReportType type, uint32_t reportID,
                                uint8_t* report, CFIndex len,
                                uint64_t timestamp)
{
    (void)ctx; (void)result; (void)sender; (void)type; (void)reportID;
    (void)timestamp;
    if (len < (CFIndex)(kDataOffset + 12) || !g_shm) return;

    int32_t rx, ry, rz;
    memcpy(&rx, report + kDataOffset,      sizeof(rx));
    memcpy(&ry, report + kDataOffset + 4,  sizeof(ry));
    memcpy(&rz, report + kDataOffset + 8,  sizeof(rz));

    g_shm->x         = rx;
    g_shm->y         = ry;
    g_shm->z         = rz;
    g_shm->timestamp = (double)mach_absolute_time() * 1e-9;
    __sync_synchronize();
    g_shm->updateCount++;
}

// ---------------------------------------------------------------------------
// Wake AppleSPUHIDDriver by setting sensor properties.
// Without this the driver may never start delivering interrupt reports.
// ---------------------------------------------------------------------------
static void wake_spu_driver(void) {
    CFMutableDictionaryRef matching = IOServiceMatching("AppleSPUHIDDriver");
    if (!matching) return;

    io_iterator_t iter;
    kern_return_t kr = IOServiceGetMatchingServices(kIOMasterPortDefault, matching, &iter);
    if (kr != KERN_SUCCESS) return;

    io_object_t driver;
    while ((driver = IOIteratorNext(iter)) != 0) {
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
            if (uRef && CFGetTypeID(uRef) == CFNumberGetTypeID())
                CFNumberGetValue((CFNumberRef)uRef,  kCFNumberSInt32Type, &u);

            if ((uint32_t)up == kUsagePage && (uint32_t)u == kUsageAccel) {
                int32_t one = 1;
                int32_t interval = 1000;  // 1000 µs
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

                fprintf(stderr, "tcmt_sensor_helper: woken SPU driver (reporting=1, power=1, interval=1000)\n");
            }
            CFRelease(props);
        }
        IOObjectRelease(parent);
        IOObjectRelease(driver);
    }
    IOObjectRelease(iter);
}

// ---------------------------------------------------------------------------
// Find accelerometer service
// ---------------------------------------------------------------------------
static io_service_t find_accel_service(void) {
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
// Setup shared memory
// ---------------------------------------------------------------------------
static int setup_shm(void) {
    g_shmFd = shm_open(kShmName, O_CREAT | O_RDWR, 0644);
    if (g_shmFd < 0) { perror("shm_open"); return -1; }

    fchmod(g_shmFd, 0644);

    if (ftruncate(g_shmFd, kShmSize) < 0) {
        if (errno == EINVAL) {
            shm_unlink(kShmName);
            close(g_shmFd);
            g_shmFd = shm_open(kShmName, O_CREAT | O_RDWR, 0644);
            if (g_shmFd >= 0) fchmod(g_shmFd, 0644);
        }
        if (g_shmFd < 0 || ftruncate(g_shmFd, kShmSize) < 0) {
            perror("ftruncate"); return -1;
        }
    }

    g_shm = (AccelShm*)mmap(NULL, kShmSize, PROT_READ | PROT_WRITE,
                            MAP_SHARED, g_shmFd, 0);
    if (g_shm == MAP_FAILED) { perror("mmap"); return -1; }

    memset(g_shm, 0, kShmSize);
    g_shm->magic = kShmMagic;
    return 0;
}

// ---------------------------------------------------------------------------
// Cleanup
// ---------------------------------------------------------------------------
static void cleanup(void) {
    g_done = 1;
    if (g_hidDev) {
        IOHIDDeviceUnscheduleFromRunLoop(g_hidDev,
            CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
        IOHIDDeviceClose(g_hidDev, 0);
        CFRelease(g_hidDev);
        g_hidDev = NULL;
    }
    if (g_shm) {
        munmap(g_shm, kShmSize);
        g_shm = NULL;
    }
    if (g_shmFd >= 0) {
        close(g_shmFd);
        shm_unlink(kShmName);
        g_shmFd = -1;
    }
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------
int main(int argc, const char* argv[]) {
    (void)argc; (void)argv;

    // V2: Ignore SIGTERM (launchd/system noise), stay alive for HID.
    signal(SIGTERM, SIG_IGN);
    signal(SIGINT,  handle_signal);
    signal(SIGHUP,  SIG_IGN);

    fprintf(stderr, "tcmt_sensor_helper: v3 starting (timestamped callback)\n");

    // 0. Wake the SPU driver — critical for interrupt reports
    wake_spu_driver();

    // 1. Find accelerometer service
    io_service_t service = find_accel_service();
    if (!service) {
        fprintf(stderr, "tcmt_sensor_helper: no BMI284 accelerometer found\n");
        return 1;
    }

    // 2. Create IOHIDDevice
    g_hidDev = IOHIDDeviceCreate(kCFAllocatorDefault, service);
    IOObjectRelease(service);
    if (!g_hidDev) {
        fprintf(stderr, "tcmt_sensor_helper: IOHIDDeviceCreate failed\n");
        return 1;
    }

    // 3. Open HID device
    IOReturn kr = IOHIDDeviceOpen(g_hidDev, 0);
    if (kr != kIOReturnSuccess) {
        fprintf(stderr, "tcmt_sensor_helper: IOHIDDeviceOpen failed (0x%x)\n", kr);
        CFRelease(g_hidDev);
        g_hidDev = NULL;
        return 1;
    }

    // 4. Setup shared memory
    if (setup_shm() != 0) {
        IOHIDDeviceClose(g_hidDev, 0);
        CFRelease(g_hidDev);
        g_hidDev = NULL;
        return 1;
    }

    // 5. Register timestamped input report callback
    //    Uses IOHIDReportWithTimeStampCallback — this routes through the
    //    interrupt-driven kernel path which is NOT blocked by
    //    motionRestrictedService on macOS 15+.
    uint8_t* reportBuf = (uint8_t*)malloc(kReportBufSz);
    if (!reportBuf) { cleanup(); return 1; }

    IOHIDDeviceRegisterInputReportWithTimeStampCallback(g_hidDev, reportBuf, kReportBufSz,
                                                         on_input_report_ts, NULL);

    // 6. Schedule on run loop
    IOHIDDeviceScheduleWithRunLoop(g_hidDev,
                                   CFRunLoopGetCurrent(),
                                   kCFRunLoopDefaultMode);

    fprintf(stderr, "tcmt_sensor_helper: started (shm=%s, callback=timestamped)\n", kShmName);

    // 7. Run loop — let the async callback do the work
    while (!g_done) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, false);
    }

    // 8. Clean exit
    free(reportBuf);
    cleanup();
    fprintf(stderr, "tcmt_sensor_helper: exited\n");
    return 0;
}
