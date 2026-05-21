// PowerMonitor.cpp — Apple Silicon IOReport power/frequency sampler
// Uses Apple's private IOReport framework (dlopen'd at runtime) to read
// CPU/GPU/ANE energy counters and IOKit pmgr DVFS properties for frequencies.
// No sudo required.  Falls back gracefully when IOReport is unavailable.

#include "PowerMonitor.h"
#include "../Utils/Logger.h"

#ifdef TCMT_MACOS

// ---------------------------------------------------------------------------
// macOS — IOReport + IOKit direct implementation (Apple Silicon only)
// ---------------------------------------------------------------------------
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <thread>
#include <cstring>
#include <cstdint>
#include <chrono>
#include <sys/sysctl.h>

// ============================================================================
// IOReport private API — loaded via dlopen("/usr/lib/libIOReport.dylib")
// ============================================================================

// Function pointer types for the private IOReport C API
typedef CFDictionaryRef (*FnIOReportCopyChannelsInGroup)(CFStringRef, CFStringRef, uint64_t, uint64_t, uint64_t);
typedef void         (*FnIOReportMergeChannels)(CFDictionaryRef, CFDictionaryRef, void*);
typedef void*  (*FnIOReportCreateSubscription)(void*, CFDictionaryRef, void**, uint64_t, void*);
typedef CFDictionaryRef (*FnIOReportCreateSamples)(void*, CFDictionaryRef, void*);
typedef CFDictionaryRef (*FnIOReportCreateSamplesDelta)(CFDictionaryRef, CFDictionaryRef, void*);
typedef int64_t (*FnIOReportSimpleGetIntegerValue)(CFDictionaryRef, int);
typedef CFStringRef (*FnIOReportChannelGetGroup)(CFDictionaryRef);
typedef CFStringRef (*FnIOReportChannelGetSubGroup)(CFDictionaryRef);
typedef CFStringRef (*FnIOReportChannelGetChannelName)(CFDictionaryRef);
typedef int32_t  (*FnIOReportStateGetCount)(CFDictionaryRef);
typedef CFStringRef (*FnIOReportStateGetNameForIndex)(CFDictionaryRef, int32_t);
typedef int64_t  (*FnIOReportStateGetResidency)(CFDictionaryRef, int32_t);

// Function pointers (loaded once; all instances share them)
static void* g_lib = nullptr;
static FnIOReportCopyChannelsInGroup g_CopyGroup = nullptr;
static FnIOReportMergeChannels       g_MergeChan = nullptr;
static FnIOReportCreateSubscription   g_CreateSub = nullptr;
static FnIOReportCreateSamples        g_CreateSamples = nullptr;
static FnIOReportCreateSamplesDelta   g_CreateDelta = nullptr;
static FnIOReportSimpleGetIntegerValue g_GetInt = nullptr;
static FnIOReportChannelGetGroup       g_GetGroup = nullptr;
static FnIOReportChannelGetSubGroup    g_GetSubGroup = nullptr;
static FnIOReportChannelGetChannelName g_GetChanName = nullptr;
static FnIOReportStateGetCount         g_StateCount = nullptr;
static FnIOReportStateGetNameForIndex  g_StateName  = nullptr;
static FnIOReportStateGetResidency     g_StateRes   = nullptr;

// Load all IOReport symbols.  Returns true once.
static bool LoadIOReport() {
    if (g_lib) return true;

    g_lib = dlopen("/usr/lib/libIOReport.dylib", RTLD_LAZY | RTLD_LOCAL);
    if (!g_lib) {
        Logger::Warn("PowerMonitor: /usr/lib/libIOReport.dylib not found");
        return false;
    }

#define DLSYM_OR_FAIL(name_)                                          \
    do {                                                               \
        g_##name_ = reinterpret_cast<decltype(g_##name_)>(            \
            dlsym(g_lib, #name_));                                     \
        if (!g_##name_) {                                              \
            Logger::Error("PowerMonitor: dlsym(" #name_ ") failed");   \
            dlclose(g_lib); g_lib = nullptr;                           \
            return false;                                              \
        }                                                              \
    } while (0)

    g_CopyGroup    = reinterpret_cast<FnIOReportCopyChannelsInGroup>( dlsym(g_lib, "IOReportCopyChannelsInGroup"));
    g_MergeChan    = reinterpret_cast<FnIOReportMergeChannels>(       dlsym(g_lib, "IOReportMergeChannels"));
    g_CreateSub     = reinterpret_cast<FnIOReportCreateSubscription>(  dlsym(g_lib, "IOReportCreateSubscription"));
    g_CreateSamples = reinterpret_cast<FnIOReportCreateSamples>(       dlsym(g_lib, "IOReportCreateSamples"));
    g_CreateDelta   = reinterpret_cast<FnIOReportCreateSamplesDelta>(  dlsym(g_lib, "IOReportCreateSamplesDelta"));
    if (!g_CopyGroup || !g_CreateSub || !g_CreateSamples || !g_CreateDelta) {
        Logger::Error("PowerMonitor: required IOReport symbols missing");
        dlclose(g_lib); g_lib = nullptr;
        return false;
    }

    // These are optional — not all macOS versions export them
    g_GetInt     = reinterpret_cast<FnIOReportSimpleGetIntegerValue>(
                       dlsym(g_lib, "IOReportSimpleGetIntegerValue"));
    g_GetGroup   = reinterpret_cast<FnIOReportChannelGetGroup>(
                       dlsym(g_lib, "IOReportChannelGetGroup"));
    g_GetSubGroup = reinterpret_cast<FnIOReportChannelGetSubGroup>(
                       dlsym(g_lib, "IOReportChannelGetSubGroup"));
    g_GetChanName = reinterpret_cast<FnIOReportChannelGetChannelName>(
                       dlsym(g_lib, "IOReportChannelGetChannelName"));
    g_StateCount  = reinterpret_cast<FnIOReportStateGetCount>(
                       dlsym(g_lib, "IOReportStateGetCount"));
    g_StateName   = reinterpret_cast<FnIOReportStateGetNameForIndex>(
                       dlsym(g_lib, "IOReportStateGetNameForIndex"));
    g_StateRes    = reinterpret_cast<FnIOReportStateGetResidency>(
                       dlsym(g_lib, "IOReportStateGetResidency"));

#undef DLSYM_OR_FAIL

    Logger::Info("PowerMonitor: loaded libIOReport.dylib");
    return true;
}

// ============================================================================
// IOReport dictionary key constants (CFSTR is evaluated at compile time)
// ============================================================================
static const CFStringRef kChannelsKey     = CFSTR("IOReportChannels");
static const CFStringRef kGroupKey        = CFSTR("IOReportGroupName");
static const CFStringRef kChannelNameKey  = CFSTR("IOReportChannelName");
static const CFStringRef kChannelInfoKey  = CFSTR("IOReportChannelInfo");
static const CFStringRef kChannelUnitKey  = CFSTR("IOReportChannelUnit");
static const CFStringRef kChannelValuesKey = CFSTR("IOReportChannelValues");

// ============================================================================
// Helpers
// ============================================================================

// Read cluster max frequency from pmgr IOKit node via IOServiceNameMatching.
// property:  "voltage-states1-sram" (E-cluster), "voltage-states5-sram" (P-cluster)
// Returns MHz or 0 on failure.
static double ReadPmgrFreq(const char* propName) {
    mach_port_t mainPort;
    if (@available(macOS 12.0, *)) {
        mainPort = kIOMainPortDefault;
    } else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        mainPort = kIOMasterPortDefault;
#pragma clang diagnostic pop
    }
    io_service_t pmgr = IOServiceGetMatchingService(
        mainPort,
        IOServiceNameMatching("pmgr"));
    if (pmgr == IO_OBJECT_NULL) return 0.0;

    CFMutableDictionaryRef props = nullptr;
    kern_return_t kr = IORegistryEntryCreateCFProperties(pmgr, &props,
                        kCFAllocatorDefault, 0);
    IOObjectRelease(pmgr);
    if (kr != KERN_SUCCESS || !props) return 0.0;

    CFDataRef data = (CFDataRef)CFDictionaryGetValue(
                        props, CFStringCreateWithCString(
                            kCFAllocatorDefault, propName,
                            kCFStringEncodingASCII));
    double freqMHz = 0.0;
    if (data && CFGetTypeID(data) == CFDataGetTypeID()) {
        CFIndex len   = CFDataGetLength(data);
        const uint8_t* bytes = CFDataGetBytePtr(data);
        size_t nPairs = static_cast<size_t>(len) / 8;  // each: [freq:4, volt:4]

        if (nPairs > 0) {
            uint32_t maxFreq = 0;
            for (size_t i = 0; i < nPairs; ++i) {
                uint32_t f = 0;
                memcpy(&f, bytes + i * 8, sizeof(f));
                if (f > maxFreq) maxFreq = f;
            }

            if (maxFreq > 0) {
                // M1-M3: freq in Hz  (values ~600000000..3504000000)
                // M4+:   freq in kHz (values ~600000..3504000)
                // Detect by magnitude: > 5000000 means Hz
                if (maxFreq > 5000000u)
                    freqMHz = static_cast<double>(maxFreq) / 1000000.0;
                else
                    freqMHz = static_cast<double>(maxFreq) / 1000.0;
            }
        }
    }
    CFRelease(props);
    return freqMHz;
}

// ============================================================================
// PowerMonitor implementation
// ============================================================================

PowerMonitor::PowerMonitor() {}

PowerMonitor::~PowerMonitor() { Stop(); }

bool PowerMonitor::Start() {
    if (running_.exchange(true)) return true;

    // -- 1. Load IOReport library --
    if (!LoadIOReport()) {
        running_.store(false);
        Logger::Info("PowerMonitor: IOReport unavailable, will use powermetrics fallback");
        return false;
    }

    // -- 2. Fetch channels for power/freq groups (names from LLM analysis of IOReport dump) --
    auto makeCFStr = [](const char* s) -> CFStringRef {
        return CFStringCreateWithCString(kCFAllocatorDefault, s, kCFStringEncodingUTF8);
    };

    CFStringRef gEnergy = makeCFStr("Energy Model");
    CFStringRef gCpu    = makeCFStr("CPU Stats");
    CFStringRef gGpu    = makeCFStr("GPU Stats");

    CFDictionaryRef eChan = g_CopyGroup(gEnergy, nullptr, 0, 0, 0);
    CFDictionaryRef cChan = g_CopyGroup(gCpu,    nullptr, 0, 0, 0);
    CFDictionaryRef gChan = g_CopyGroup(gGpu,    nullptr, 0, 0, 0);

    CFRelease(gEnergy); CFRelease(gCpu); CFRelease(gGpu);

    if (!eChan && !cChan && !gChan) {
        Logger::Error("PowerMonitor: no channels for Energy Model/CPU Stats/GPU Stats");
        running_.store(false);
        return false;
    }

    // Merge into eChan (first non-null), or use CopyAll fallback
    CFDictionaryRef merged = nullptr;
    if (g_MergeChan && eChan) {
        merged = eChan;
        if (cChan) { g_MergeChan(merged, cChan, nullptr); CFRelease(cChan); }
        if (gChan) { g_MergeChan(merged, gChan, nullptr); CFRelease(gChan); }
    } else if (eChan) {
        merged = eChan;
        if (cChan) CFRelease(cChan);
        if (gChan) CFRelease(gChan);
    }
    // Fallback: if no specific groups, try CopyAllChannels
    if (!merged) {
        typedef CFDictionaryRef (*FnCopyAll)(uint64_t, uint64_t);
        FnCopyAll g_CopyAll = reinterpret_cast<FnCopyAll>(dlsym(g_lib, "IOReportCopyAllChannels"));
        merged = g_CopyAll ? g_CopyAll(0, 0) : nullptr;
    }
    if (!merged) {
        Logger::Error("PowerMonitor: no IOReport channels available");
        running_.store(false);
        return false;
    }
    chan_ = static_cast<void*>(const_cast<__CFDictionary*>(merged));

    // -- 3. Create subscription --
    void* sub = nullptr;
    void* subResult = g_CreateSub(nullptr, merged, &sub, 0, nullptr);
    if (!subResult || !sub) {
        Logger::Error("PowerMonitor: IOReportCreateSubscription failed");
        CFRelease(merged);
        chan_ = nullptr;
        running_.store(false);
        return false;
    }
    subs_ = subResult;
    // chan_ owns merged — released in Stop()

    directMode_.store(true);

    // -- 4. Pre-cache pmgr frequencies (stable — read once) --
    {
        double pf = ReadPmgrFreq("voltage-states5-sram");
        double ef = ReadPmgrFreq("voltage-states1-sram");
        double gf = ReadPmgrFreq("voltage-states9");
        if (pf > 0) { pCoreFreq_.store(pf); Logger::Info("PowerMonitor: P-cluster freq " + std::to_string(pf) + " MHz"); }
        if (ef > 0) { eCoreFreq_.store(ef); Logger::Info("PowerMonitor: E-cluster freq " + std::to_string(ef) + " MHz"); }
        if (gf > 0) { gpuFreq_.store(gf);   Logger::Info("PowerMonitor: GPU freq " + std::to_string(gf) + " MHz"); }
    }

    // -- 5. Start background sampling thread --
    thread_ = static_cast<void*>(new std::thread([this]() { SampleLoop(); }));

    Logger::Info("PowerMonitor: direct IOReport mode active");
    return true;
}

void PowerMonitor::Stop() {
    if (!running_.exchange(false)) return;

    // Join background thread
    if (thread_) {
        auto* t = static_cast<std::thread*>(thread_);
        if (t->joinable()) t->join();
        delete t;
        thread_ = nullptr;
    }

    // Release IOReport handles
    if (subs_) { CFRelease(static_cast<CFTypeRef>(subs_)); subs_ = nullptr; }
    if (chan_) { CFRelease(static_cast<CFDictionaryRef>(chan_)); chan_ = nullptr; }

    directMode_.store(false);
    Logger::Info("PowerMonitor: stopped");
}

// ============================================================================
// SampleLoop — background thread for IOReport delta sampling (~1 s interval)
// ============================================================================
void PowerMonitor::SampleLoop() {
    Logger::Info("PowerMonitor: sample thread started");

    // Acquire first sample as baseline
    CFDictionaryRef prevSample = g_CreateSamples(
        subs_, static_cast<CFDictionaryRef>(chan_), nullptr);
    if (!prevSample) {
        Logger::Error("PowerMonitor: first IOReportCreateSamples failed, "
                      "stopping sample thread");
        running_.store(false);
        return;
    }

    while (running_.load()) {
        // Sleep ~1 second between samples
        {
            auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::seconds(1);
            while (running_.load()
                   && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
        }
        if (!running_.load()) break;

        // Acquire next sample
        CFDictionaryRef nextSample = g_CreateSamples(
            subs_, static_cast<CFDictionaryRef>(chan_), nullptr);
        if (!nextSample) {
            Logger::Error("PowerMonitor: IOReportCreateSamples failed");
            continue;
        }

        // Compute delta and parse power values
        CFDictionaryRef delta = g_CreateDelta(prevSample, nextSample, nullptr);
        if (delta) {
            ParsePowerDelta((void*)delta);
            CFRelease(delta);
        }

        CFRelease(prevSample);
        prevSample = nextSample;  // advance baseline
    }

    CFRelease(prevSample);
    Logger::Info("PowerMonitor: sample thread stopped");
}

// ============================================================================
// ParsePowerDelta — iterate IOReport channel array in the delta result,
// extract energy counters and convert them to average power in Watts.
// ============================================================================
void PowerMonitor::ParsePowerDelta(void* deltaV) {
    auto* delta = static_cast<CFDictionaryRef>(deltaV);

    CFArrayRef channels = (CFArrayRef)CFDictionaryGetValue(delta, kChannelsKey);
    if (!channels || CFGetTypeID(channels) != CFArrayGetTypeID()) return;

    static int logCount = 0;
    CFIndex count = CFArrayGetCount(channels);
    for (CFIndex i = 0; i < count; ++i) {
        auto* channel = static_cast<CFDictionaryRef>(
            CFArrayGetValueAtIndex(channels, i));
        if (!channel || CFGetTypeID(channel) != CFDictionaryGetTypeID()) continue;

        // Use IOReport API functions to read metadata (not CFDictionaryGetValue)
        char group[64] = {0}, sub[64] = {0}, name[64] = {0};
        if (g_GetGroup) {
            CFStringRef s = g_GetGroup(channel);
            if (s) CFStringGetCString(s, group, sizeof(group), kCFStringEncodingUTF8);
        }
        if (g_GetSubGroup) {
            CFStringRef s = g_GetSubGroup(channel);
            if (s) CFStringGetCString(s, sub, sizeof(sub), kCFStringEncodingUTF8);
        }
        if (g_GetChanName) {
            CFStringRef s = g_GetChanName(channel);
            if (s) CFStringGetCString(s, name, sizeof(name), kCFStringEncodingUTF8);
        }

        if (group[0] == '\0') continue;

        // CPU Stats: state residency (before value check — no simple int value)
        if (strcmp(group, "CPU Stats") == 0) {
            if (g_StateCount && g_StateName && g_StateRes) {
                // "CPU Complex Voltage States" has V0-V16 (voltages) — skip
                // "CPU Core Performance States" has freq names like "660 MHz"
                if (strcmp(sub, "CPU Core Performance States") != 0) { continue; }

                // Aggregate per-cluster: accumulate all PCPU*/ECPU* channels
                int nStates = g_StateCount((CFDictionaryRef)channel);
                if (nStates > 1) {
                    double weightedSum = 0.0;
                    int64_t totalRes = 0;
                    static bool firstLog = true;
                    for (int si = 0; si < nStates; ++si) {
                        int64_t r = g_StateRes((CFDictionaryRef)channel, si);
                        CFStringRef sn = g_StateName((CFDictionaryRef)channel, si);
                        char sname[32] = {};
                        if (sn) CFStringGetCString(sn, sname, sizeof(sname), kCFStringEncodingUTF8);
                        if (firstLog && strncmp(name, "PCPU", 4) == 0 && si == 0)
                            Logger::Info("PowerMonitor: perf state[0]='" + std::string(sname) + "' r=" + std::to_string(r));
                        if (r <= 0) continue;
                        totalRes += r;
                        double mhz = atof(sname);
                        // State names like "660 MHz" or "3504 MHz"
                        if (mhz > 100 && mhz < 10000)
                            weightedSum += mhz * static_cast<double>(r);
                    }
                    if (strncmp(name, "PCPU", 4) == 0) firstLog = false;
                    if (totalRes > 0) {
                        double activeMHz = weightedSum / static_cast<double>(totalRes);
                        if (strncmp(name, "PCPU", 4) == 0) pCoreFreq_.store(activeMHz);
                        else if (strncmp(name, "ECPU", 4) == 0) eCoreFreq_.store(activeMHz);
                    }
                }
            }
            continue;
        }

        int64_t value = ExtractChannelValue((void*)channel);
        if (value <= 0 || value == INT64_MIN) continue;

        // Power: "Energy Model" group — energy in mJ/uJ/nJ over ~1s → mW
        if (strcmp(group, "Energy Model") == 0) {
            double power = EnergyToPower((void*)channel, value) * 1000.0;
            if (power > 0.1 && power < 50000.0) {
                if (strcmp(sub, "CPU Energy") == 0) cpuPower_.store(power);
                else if (strcmp(sub, "GPU") == 0) gpuPower_.store(power);
                else if (strcmp(sub, "ANE") == 0) anePower_.store(power);
            }
        }
    }
    if (logCount < 1) ++logCount;
}

// ============================================================================
// ExtractChannelValue — obtain the int64 value from an IOReport channel dict.
// Tries IOReportSimpleGetIntegerValue first, falls back to manual array read.
// ============================================================================
int64_t PowerMonitor::ExtractChannelValue(void* channelV) {
    auto* channel = static_cast<CFDictionaryRef>(channelV);

    // Preferred path: IOReportSimpleGetIntegerValue (fast, direct)
    if (g_GetInt) {
        int64_t v = g_GetInt(channel, 0);
        if (v != 0) return v;
    }

    // Fallback: read "IOReportChannelValues" array index 0
    CFArrayRef values = (CFArrayRef)CFDictionaryGetValue(channel,
                                                         kChannelValuesKey);
    if (values && CFGetTypeID(values) == CFArrayGetTypeID()) {
        CFIndex n = CFArrayGetCount(values);
        if (n > 0) {
            auto* num = static_cast<CFNumberRef>(
                CFArrayGetValueAtIndex(values, 0));
            if (num && CFGetTypeID(num) == CFNumberGetTypeID()) {
                int64_t v = 0;
                if (CFNumberGetValue(num, kCFNumberSInt64Type, &v) && v > 0)
                    return v;
            }
        }
    }

    return 0;
}

// ============================================================================
// EnergyToPower — convert energy delta to power (Watts).
// Reads the channel unit from the "IOReportChannelInfo" sub-dictionary to
// determine the scale (nJ / uJ / mJ / J) and divides by 1 second.
// ============================================================================
double PowerMonitor::EnergyToPower(void* channelV, int64_t energyDelta) {
    auto* channel = static_cast<CFDictionaryRef>(channelV);

    // Default scale: assume Joules (divide by 1)
    double scale = 1.0;

    // Read unit from "IOReportChannelInfo" → "IOReportChannelUnit"
    auto* info = static_cast<CFDictionaryRef>(
        CFDictionaryGetValue(channel, kChannelInfoKey));
    if (info && CFGetTypeID(info) == CFDictionaryGetTypeID()) {
        auto* unitNum = static_cast<CFNumberRef>(
            CFDictionaryGetValue(info, kChannelUnitKey));
        if (unitNum && CFGetTypeID(unitNum) == CFNumberGetTypeID()) {
            int64_t unit = 0;
            CFNumberGetValue(unitNum, kCFNumberSInt64Type, &unit);
            // IOReportChannelUnit is a bitfield encoding the unit.
            // Common values observed on Apple Silicon:
            //   unit & 0xFF == 12 → nanojoules   (1e-9)
            //   unit & 0xFF == 11 → microjoules  (1e-6)
            //   unit & 0xFF == 10 → millijoules  (1e-3)
            //   unit & 0xFF == 9  → joules       (1)
            int64_t unitCode = unit & 0xFF;
            if      (unitCode >= 12) scale = 1e9;   // nJ
            else if (unitCode == 11) scale = 1e6;   // uJ
            else if (unitCode == 10) scale = 1e3;   // mJ
            // else unitCode == 9: scale = 1.0 (J)
        }
    }

    // Power (W) = energy_delta (J) / time (s)
    // Our sampling interval is ~1 second; a more accurate approach would
    // measure actual elapsed time, but 1 s is sufficient for average power.
    double powerW = static_cast<double>(energyDelta) / scale;
    return powerW;
}

#else  // !TCMT_MACOS
// ============================================================================
// Windows / other platforms — stub: Start() always returns false so the
// caller falls back to its existing polling mechanism.
// ============================================================================

PowerMonitor::PowerMonitor() {}
PowerMonitor::~PowerMonitor() { Stop(); }

bool PowerMonitor::Start() {
    // IOReport is not available on this platform.
    return false;
}

void PowerMonitor::Stop() {
    // Nothing to clean up.
}

#endif // TCMT_MACOS
