// sensor_recorder.mm — record ALL Apple SPU HID sensors for N seconds
// Compile:
//   clang++ -std=c++17 -framework IOKit -framework CoreFoundation \
//           -o sensor_recorder sensor_recorder.mm
// Run:
//   ./sensor_recorder [duration_seconds=15]
//
// Actions to perform while recording:
// 1. First 3s: MacBook flat on table
// 2. Next 3s:  tilt to portrait (90°)
// 3. Next 3s:  tilt to 45°
// 4. Next 3s:  rotate in horizontal plane
// 5. Last 3s:  open/close lid slowly

#import <CoreFoundation/CoreFoundation.h>
#import <IOKit/IOKitLib.h>
#import <IOKit/hid/IOHIDDevice.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>
#include <vector>
#include <string>
#include <algorithm>

// ── Known usages ──
static constexpr uint32_t PAGE_VENDOR = 0xFF00;
static constexpr uint32_t PAGE_SENSOR = 0x0020;
static constexpr uint32_t PAGE_APPLE2 = 0xFF0C;

static const char* sensor_label(uint32_t page, uint32_t usage) {
    if (page == PAGE_VENDOR) {
        switch (usage) {
            case 3: return "Gravity";
            case 9: return "Gyro";
            case 4: return "ALS";
            case 5: return "Temp";
            default: return "Unknown";
        }
    }
    if (page == PAGE_SENSOR && usage == 138) return "LidAngle";
    if (page == PAGE_APPLE2 && usage == 1) return "ApplePriv1";
    if (page == PAGE_APPLE2 && usage == 5) return "ApplePriv5";
    return "???";
}

// ── Per-sensor state ──
struct Sensor {
    IOHIDDeviceRef dev = nullptr;
    uint8_t* reportBuf = nullptr;
    CFIndex reportLen = 0;
    uint32_t page = 0, usage = 0;
    char label[64];
    FILE* out = nullptr;
};

static std::vector<Sensor> g_sensors;
static volatile bool g_running = true;
static double g_startTime = 0;

static double now_sec() {
    return CFAbsoluteTimeGetCurrent() + kCFAbsoluteTimeIntervalSince1970;
}

// ── HID callback ──
static void on_report(void* ctx, IOReturn result, void* sender,
                      IOHIDReportType type, uint32_t reportID,
                      uint8_t* report, CFIndex len, uint64_t timestamp)
{
    (void)sender; (void)type; (void)reportID;
    if (result != kIOReturnSuccess || !ctx) return;
    auto* s = (Sensor*)ctx;
    double t = now_sec() - g_startTime;

    // Build hex dump
    char hex[512]; int pos = 0;
    for (CFIndex i = 0; i < len && pos < 500; i++)
        pos += snprintf(hex + pos, sizeof(hex) - pos, "%02x ", (unsigned)report[i]);

    // Parse known formats
    char parsed[256] = "";
    if (s->page == PAGE_VENDOR && (s->usage == 3 || s->usage == 9) && len >= 18) {
        int32_t v1, v2, v3;
        memcpy(&v1, report + 6, 4);
        memcpy(&v2, report + 10, 4);
        memcpy(&v3, report + 14, 4);
        double scale = 1.0 / 65536.0;
        const char* unit = (s->usage == 3) ? "g" : "deg/s";
        snprintf(parsed, sizeof(parsed), " => %+8.4f %+8.4f %+8.4f %s  (raw %d %d %d)",
                 v1*scale, v2*scale, v3*scale, unit, v1, v2, v3);
    }
    else if (s->page == PAGE_VENDOR && s->usage == 5 && len >= 10) {
        // Temp: bytes 8-9 = Q8.8 °C
        int16_t raw;
        memcpy(&raw, report + 8, 2);
        snprintf(parsed, sizeof(parsed), " => %.2f C  (seq=%d)", raw / 256.0, report[0]);
    }
    else if (s->page == PAGE_SENSOR && s->usage == 138 && len >= 3 && report[0] == 1) {
        uint16_t raw = (uint16_t)report[1] | ((uint16_t)report[2] << 8);
        snprintf(parsed, sizeof(parsed), " => lid=%u deg (raw 0x%04x)", raw & 0x1FF, raw);
    }

    // Timestamp + label + hex + parsed
    fprintf(s->out, "%8.4f  %-12s %s%s\n", t, s->label, hex, parsed);
    fflush(s->out);
}

// ── Wake drivers ──
static void wake_drivers() {
    CFMutableDictionaryRef matching = IOServiceMatching("AppleSPUHIDDriver");
    if (!matching) return;
    io_iterator_t iter;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iter) != KERN_SUCCESS) return;
    io_object_t drv;
    while ((drv = IOIteratorNext(iter)) != 0) {
        int32_t one = 1, interval = 1000;
        CFNumberRef v;
        v = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &one);
        IORegistryEntrySetCFProperty(drv, CFSTR("SensorPropertyReportingState"), v); CFRelease(v);
        v = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &one);
        IORegistryEntrySetCFProperty(drv, CFSTR("SensorPropertyPowerState"), v); CFRelease(v);
        v = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &interval);
        IORegistryEntrySetCFProperty(drv, CFSTR("ReportInterval"), v); CFRelease(v);
        IOObjectRelease(drv);
    }
    IOObjectRelease(iter);
}

int main(int argc, char** argv) {
    double duration = 15.0;
    if (argc > 1) duration = atof(argv[1]);
    if (duration < 1) duration = 15;

    printf("=== SPU Sensor Recorder ===\n");
    printf("Recording for %.0f seconds...\n", duration);
    printf("Perform actions as prompted.\n\n");

    wake_drivers();

    // Enumerate devices
    CFMutableDictionaryRef matching = IOServiceMatching("AppleSPUHIDDevice");
    if (!matching) { fprintf(stderr, "IOServiceMatching failed\n"); return 1; }
    io_iterator_t iter;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, matching, &iter) != KERN_SUCCESS) {
        fprintf(stderr, "No AppleSPUHIDDevice found\n"); return 1;
    }

    // Open output file
    FILE* out = fopen("/tmp/sensor_recording.txt", "w");
    if (!out) { perror("fopen"); return 1; }
    fprintf(out, "# SPU Sensor Recording - %.0f seconds\n", duration);
    fprintf(out, "# Time(s)  Sensor        Raw Hex\n");
    fprintf(out, "# Columns: time, label, hex_bytes, => parsed_value\n\n");

    io_object_t svc;
    while ((svc = IOIteratorNext(iter)) != 0) {
        CFMutableDictionaryRef props = nullptr;
        if (IORegistryEntryCreateCFProperties(svc, &props, kCFAllocatorDefault, 0) != KERN_SUCCESS || !props) {
            IOObjectRelease(svc); continue;
        }

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

        Sensor s;
        s.page = page;
        s.usage = usage;
        snprintf(s.label, sizeof(s.label), "%s(0x%X/%u)", sensor_label(page, usage), page, usage);

        IOHIDDeviceRef dev = IOHIDDeviceCreate(kCFAllocatorDefault, svc);
        CFRelease(props);
        IOObjectRelease(svc);
        if (!dev) { printf("  SKIP %s (create failed)\n", s.label); continue; }

        // Skip the top-level device (usage 255) — not a data sensor
        if (page == PAGE_VENDOR && usage == 255) { CFRelease(dev); continue; }

        IOReturn kr = IOHIDDeviceOpen(dev, 0);
        if (kr != kIOReturnSuccess) {
            printf("  SKIP %s (open failed kr=0x%x)\n", s.label, kr);
            CFRelease(dev);
            continue;
        }

        CFIndex bufSize = 256;
        s.reportBuf = new uint8_t[bufSize];
        s.reportLen = bufSize;
        s.dev = dev;
        s.out = out; // all write to same file

        g_sensors.push_back(s);
        printf("  OPEN %s\n", s.label);
    }
    IOObjectRelease(iter);

    if (g_sensors.empty()) {
        fprintf(stderr, "No sensors opened\n");
        fclose(out);
        return 1;
    }

    // Start CFRunLoop on main thread
    g_startTime = now_sec();
    for (auto& s : g_sensors) {
        IOHIDDeviceRegisterInputReportWithTimeStampCallback(
            s.dev, s.reportBuf, s.reportLen, on_report, (void*)&s);
        IOHIDDeviceScheduleWithRunLoop(s.dev, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
    }

    printf("\nRecording... %d sensors active\n\n", (int)g_sensors.size());
    puts("=== ACTIONS TIMELINE ===");
    puts("  0-3s  : MacBook flat on table (lid open ~140°)");
    puts("  3-6s  : Tilt to portrait 90° (lift left edge)");
    puts("  6-9s  : Tilt to 45° angle");
    puts("  9-12s : Rotate in horizontal plane");
    puts(" 12-15s: Open/close lid slowly");
    puts("");

    // Run for the duration
    double endTime = now_sec() + duration;
    CFRunLoopRef rl = CFRunLoopGetCurrent();
    while (now_sec() < endTime) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
    }

    // Cleanup
    fprintf(out, "\n# End of recording\n");
    fclose(out);

    for (auto& s : g_sensors) {
        if (s.dev) {
            IOHIDDeviceUnscheduleFromRunLoop(s.dev, CFRunLoopGetCurrent(), kCFRunLoopDefaultMode);
            IOHIDDeviceClose(s.dev, 0);
            CFRelease(s.dev);
        }
        delete[] s.reportBuf;
    }

    printf("\nDone! Recording saved to: /tmp/sensor_recording.txt\n");
    printf("Share the contents of that file with me for analysis.\n");
    return 0;
}
