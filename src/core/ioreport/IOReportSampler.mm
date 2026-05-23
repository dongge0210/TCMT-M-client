// IOReportSampler.mm — Apple Silicon IOReport power/frequency/state sampling engine
// Provides IOReport subscription, delta sampling, channel iteration, and data extraction.
// Used by PowerMonitor module. Compiled only on macOS.

#include "power/PowerMonitor.h"
#include "Utils/Logger.h"

#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <thread>
#include <cstring>
#include <cstdint>
#include <chrono>
#include <sys/sysctl.h>

static FILE* dbg_ = nullptr;

// ====================================================================
// IOReport private API — loaded via dlopen("/usr/lib/libIOReport.dylib")
// ====================================================================

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

static bool LoadIOReport() {
    if (g_lib) return true;
    g_lib = dlopen("/usr/lib/libIOReport.dylib", RTLD_LAZY | RTLD_LOCAL);
    if (!g_lib) {
        Logger::Warn("PowerMonitor: /usr/lib/libIOReport.dylib not found");
        return false;
    }
#define DLSYM_OR_FAIL(name_) do { \
        g_##name_ = reinterpret_cast<decltype(g_##name_)>(dlsym(g_lib, #name_)); \
        if (!g_##name_) { Logger::Error("PowerMonitor: dlsym(" #name_ ") failed"); \
            dlclose(g_lib); g_lib = nullptr; return false; } \
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
    g_GetInt     = reinterpret_cast<FnIOReportSimpleGetIntegerValue>(dlsym(g_lib, "IOReportSimpleGetIntegerValue"));
    g_GetGroup   = reinterpret_cast<FnIOReportChannelGetGroup>(dlsym(g_lib, "IOReportChannelGetGroup"));
    g_GetSubGroup = reinterpret_cast<FnIOReportChannelGetSubGroup>(dlsym(g_lib, "IOReportChannelGetSubGroup"));
    g_GetChanName = reinterpret_cast<FnIOReportChannelGetChannelName>(dlsym(g_lib, "IOReportChannelGetChannelName"));
    g_StateCount  = reinterpret_cast<FnIOReportStateGetCount>(dlsym(g_lib, "IOReportStateGetCount"));
    g_StateName   = reinterpret_cast<FnIOReportStateGetNameForIndex>(dlsym(g_lib, "IOReportStateGetNameForIndex"));
    g_StateRes    = reinterpret_cast<FnIOReportStateGetResidency>(dlsym(g_lib, "IOReportStateGetResidency"));
#undef DLSYM_OR_FAIL
    return true;
}

// ====================================================================
// Helpers
// ====================================================================

static const CFStringRef kChannelsKey      = CFSTR("IOReportChannels");
static const CFStringRef kChannelUnitKey   = CFSTR("IOReportChannelUnit");
static const CFStringRef kChannelValuesKey = CFSTR("IOReportChannelValues");

// Read DVFS frequency table from pmgr. Fills outFreqs[] with MHz values.
// Returns count, 0 on failure. Each entry: uint32 pair [freq, voltage].
static int ReadPmgrFreqTable(const char* propName, double* outFreqs, int maxCount) {
    mach_port_t mainPort;
    if (@available(macOS 12.0, *)) {
        mainPort = kIOMainPortDefault;
    } else {
        mainPort = kIOMasterPortDefault;
    }
    io_service_t pmgr = IOServiceGetMatchingService(mainPort, IOServiceNameMatching("pmgr"));
    if (pmgr == IO_OBJECT_NULL) return 0;
    CFMutableDictionaryRef props = nullptr;
    kern_return_t kr = IORegistryEntryCreateCFProperties(pmgr, &props, kCFAllocatorDefault, 0);
    IOObjectRelease(pmgr);
    if (kr != KERN_SUCCESS || !props) return 0;
    CFDataRef data = (CFDataRef)CFDictionaryGetValue(props,
        CFStringCreateWithCString(kCFAllocatorDefault, propName, kCFStringEncodingASCII));
    int count = 0;
    if (data && CFGetTypeID(data) == CFDataGetTypeID()) {
        CFIndex len = CFDataGetLength(data);
        const uint8_t* bytes = CFDataGetBytePtr(data);
        int nPairs = static_cast<int>(len / 8);
        if (nPairs > maxCount) nPairs = maxCount;
        bool isHz = false;
        for (int i = 0; i < nPairs && outFreqs; ++i) {
            uint32_t f = 0;
            memcpy(&f, bytes + i * 8, sizeof(f));
            if (f > 5000000u) isHz = true;
            outFreqs[i] = static_cast<double>(f) / (isHz ? 1000000.0 : 1000.0);
        }
        count = nPairs;
    }
    CFRelease((CFTypeRef)props);
    return count;
}

// ====================================================================
// PowerMonitor implementation (macOS)
// ====================================================================

PowerMonitor::PowerMonitor() {}
PowerMonitor::~PowerMonitor() { Stop(); }

bool PowerMonitor::Start() {
    if (running_.exchange(true)) return true;

    if (!LoadIOReport()) {
        running_.store(false);
        Logger::Info("PowerMonitor: IOReport unavailable, will use powermetrics fallback");
        return false;
    }

    // Fetch channels for Energy Model, CPU Stats, GPU Stats
    auto mkStr = [](const char* s) {
        return CFStringCreateWithCString(kCFAllocatorDefault, s, kCFStringEncodingUTF8);
    };
    CFStringRef gEnergy = mkStr("Energy Model");
    CFStringRef gCpu    = mkStr("CPU Stats");
    CFStringRef gGpu    = mkStr("GPU Stats");
    CFStringRef gAne    = mkStr("ANE");
    CFDictionaryRef eChan = g_CopyGroup(gEnergy, nullptr, 0, 0, 0);
    CFDictionaryRef cChan = g_CopyGroup(gCpu,    nullptr, 0, 0, 0);
    CFDictionaryRef gChan = g_CopyGroup(gGpu,    nullptr, 0, 0, 0);
    CFDictionaryRef aChan = g_CopyGroup(gAne,    nullptr, 0, 0, 0);
    CFRelease(gEnergy); CFRelease(gCpu); CFRelease(gGpu); CFRelease(gAne);

    CFDictionaryRef merged = nullptr;
    if (g_MergeChan && eChan) {
        merged = eChan;
        if (cChan) { g_MergeChan(merged, cChan, nullptr); CFRelease(cChan); }
        if (gChan) { g_MergeChan(merged, gChan, nullptr); CFRelease(gChan); }
        if (aChan) { g_MergeChan(merged, aChan, nullptr); CFRelease(aChan); }
    } else if (eChan) {
        merged = eChan;
        if (cChan) CFRelease(cChan);
        if (gChan) CFRelease(gChan);
        if (aChan) CFRelease(aChan);
    }
    if (!merged) {
        typedef CFDictionaryRef (*FnCopyAll)(uint64_t, uint64_t);
        FnCopyAll f = reinterpret_cast<FnCopyAll>(dlsym(g_lib, "IOReportCopyAllChannels"));
        merged = f ? f(0, 0) : nullptr;
    }
    if (!merged) {
        Logger::Error("PowerMonitor: no IOReport channels available");
        running_.store(false);
        return false;
    }
    chan_ = static_cast<void*>(const_cast<__CFDictionary*>(merged));

    void* sub = nullptr;
    void* subResult = g_CreateSub(nullptr, merged, &sub, 0, nullptr);
    if (!subResult || !sub) {
        Logger::Error("PowerMonitor: IOReportCreateSubscription failed");
        CFRelease((CFTypeRef)merged);
        chan_ = nullptr;
        running_.store(false);
        return false;
    }
    subs_ = subResult;

    // Pre-cache DVFS frequency tables
    pFreqCount_ = ReadPmgrFreqTable("voltage-states5-sram", pFreqTable_, 32);
    eFreqCount_ = ReadPmgrFreqTable("voltage-states1-sram", eFreqTable_, 32);
    gpuFreqCount_ = ReadPmgrFreqTable("voltage-states9", gpuFreqTable_, 32);

    dbg_ = fopen("/tmp/ioreport_debug.log", "w");
    if (dbg_) setbuf(dbg_, nullptr); else dbg_ = stderr;

    // DEBUG: dump DVFS tables
    fprintf(dbg_, "[IOReport] DVFS tables: P-core=%d E-core=%d GPU=%d\n",
            pFreqCount_, eFreqCount_, gpuFreqCount_);
    auto dumpTable = [](const char* label, double* ft, int n) {
        fprintf(dbg_, "[IOReport] %s:", label);
        for (int i = 0; i < n; i++) fprintf(dbg_, " %.0f", ft[i]);
        fprintf(dbg_, "\n");
    };
    if (pFreqCount_ > 0) dumpTable("P-core", pFreqTable_, pFreqCount_);
    if (eFreqCount_ > 0) dumpTable("E-core", eFreqTable_, eFreqCount_);
    if (gpuFreqCount_ > 0) dumpTable("GPU   ", gpuFreqTable_, gpuFreqCount_);
    // Scan ALL voltage-states* properties in pmgr
    {
        double alt[64];
        const char* vsNames[] = {
            "voltage-states0","voltage-states1","voltage-states2",
            "voltage-states3","voltage-states4","voltage-states5",
            "voltage-states6","voltage-states7","voltage-states8",
            "voltage-states9","voltage-states10","voltage-states11",
            "voltage-states12","voltage-states13",
            "voltage-states0-sram","voltage-states1-sram","voltage-states2-sram",
            "voltage-states3-sram","voltage-states4-sram","voltage-states5-sram",
            "voltage-states6-sram","voltage-states7-sram","voltage-states8-sram",
            "voltage-states9-sram","voltage-states10-sram","voltage-states11-sram",
            "voltage-states12-sram","voltage-states13-sram",
        };
        for (auto n : vsNames) {
            int c = ReadPmgrFreqTable(n, alt, 64);
            if (c > 0) {
                fprintf(dbg_, "[IOReport] VS '%s': %d entries", n, c);
                for (int i = 0; i < c; i++) fprintf(dbg_, " %.0f", alt[i]);
                fprintf(dbg_, "\n");
            }
        }
    }
    // END DEBUG
    if (pFreqCount_ > 0) {
        pCoreFreq_.store(pFreqTable_[pFreqCount_-1]);
        pCoreMaxFreq_.store(pFreqTable_[pFreqCount_-1]);
    }
    if (eFreqCount_ > 0) {
        eCoreFreq_.store(eFreqTable_[eFreqCount_-1]);
        eCoreMaxFreq_.store(eFreqTable_[eFreqCount_-1]);
    }
    if (gpuFreqCount_ > 0) {
        gpuFreq_.store(gpuFreqTable_[gpuFreqCount_-1]);
        gpuMaxFreq_.store(gpuFreqTable_[gpuFreqCount_-1]);
    }

    directMode_.store(true);
    running_.store(true);

    // Spawn sample thread
    auto* t = new std::thread([this]() { SampleLoop(); });
    thread_ = reinterpret_cast<void*>(t);
    return true;
}

void PowerMonitor::Stop() {
    running_.store(false);
    if (thread_) {
        auto* t = static_cast<std::thread*>(thread_);
        if (t->joinable()) t->join();
        delete t; thread_ = nullptr;
    }
    if (subs_) { CFRelease(static_cast<CFTypeRef>(subs_)); subs_ = nullptr; }
    if (chan_) { CFRelease(static_cast<CFDictionaryRef>(chan_)); chan_ = nullptr; }
    directMode_.store(false);
    if (dbg_ && dbg_ != stderr) { fclose(dbg_); dbg_ = nullptr; }
}

// ====================================================================
// SampleLoop + ParsePowerDelta
// ====================================================================

void PowerMonitor::SampleLoop() {
    Logger::Info("PowerMonitor: sample thread started");
    CFDictionaryRef prevSample = g_CreateSamples(subs_, static_cast<CFDictionaryRef>(chan_), nullptr);
    if (!prevSample) {
        Logger::Error("PowerMonitor: first IOReportCreateSamples failed");
        return;
    }
    while (running_.load()) {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (running_.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (!running_.load()) break;
        CFDictionaryRef nextSample = g_CreateSamples(subs_, static_cast<CFDictionaryRef>(chan_), nullptr);
        if (!nextSample) break;
        CFDictionaryRef delta = g_CreateDelta(prevSample, nextSample, nullptr);
        if (delta) { ParsePowerDelta((void*)delta); CFRelease((CFTypeRef)delta); }
        CFRelease((CFTypeRef)prevSample);
        prevSample = nextSample;
    }
    CFRelease((CFTypeRef)prevSample);
    Logger::Info("PowerMonitor: sample thread stopped");
}

void PowerMonitor::ParsePowerDelta(void* deltaV) {
    auto* delta = static_cast<CFDictionaryRef>(deltaV);
    CFArrayRef channels = (CFArrayRef)CFDictionaryGetValue(delta, kChannelsKey);
    if (!channels || CFGetTypeID(channels) != CFArrayGetTypeID()) return;

    static int logCount = 0;
    cpuPower_.store(0.0);
    gpuPower_.store(0.0);
    anePower_.store(0.0);
    double gpuFreqSum = 0.0; int gpuFreqN = 0;
    double pCoreFreqSum = 0.0; int pCoreFreqN = 0;
    double eCoreFreqSum = 0.0; int eCoreFreqN = 0;
    CFIndex count = CFArrayGetCount(channels);
    for (CFIndex i = 0; i < count; ++i) {
        auto* channel = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(channels, i));
        if (!channel || CFGetTypeID(channel) != CFDictionaryGetTypeID()) continue;

        char group[64]={}, sub[64]={}, name[64]={};
        if (g_GetGroup) { CFStringRef s = g_GetGroup(channel);
            if (s) CFStringGetCString(s, group, sizeof(group), kCFStringEncodingUTF8); }
        if (g_GetSubGroup) { CFStringRef s = g_GetSubGroup(channel);
            if (s) CFStringGetCString(s, sub, sizeof(sub), kCFStringEncodingUTF8); }
        if (g_GetChanName) { CFStringRef s = g_GetChanName(channel);
            if (s) CFStringGetCString(s, name, sizeof(name), kCFStringEncodingUTF8); }
        if (group[0] == '\0') continue;

        // CPU/GPU Stats: state residency → dynamic frequency
        if (strcmp(group, "CPU Stats") == 0 || strcmp(group, "GPU Stats") == 0) {
            if (g_StateCount && g_StateName && g_StateRes) {
                bool isGpu = strcmp(group, "GPU Stats") == 0;
                if (isGpu) {
                    // Only use GPUPH channels; skip AFRSTATE (display refresh) and BSTGPUPH (boost phase)
                    if (strncmp(name, "GPUPH", 5) != 0) continue;
                }
                if (!isGpu && strcmp(sub, "CPU Core Performance States") != 0) continue;
                int nStates = g_StateCount((CFDictionaryRef)channel);
                if (nStates <= 1) continue;
                // Determine frequency table
                int freqCount;
                double* ft;
                if (isGpu) {
                    freqCount = gpuFreqCount_; ft = gpuFreqTable_;
                }
                else if (strncmp(name, "PCPU", 4) == 0) { freqCount = pFreqCount_; ft = pFreqTable_; }
                else if (strncmp(name, "ECPU", 4) == 0) { freqCount = eFreqCount_; ft = eFreqTable_; }
                else continue;
                if (freqCount <= 0) continue;
                // isIdle filter (below) handles OFF/IDLE/DOWN — don't offset freqBase
                double wsum = 0.0; int64_t tres = 0;
                // DEBUG: state-by-state dump (first 3 deltas for each type)
                static int dbgPCore = 0, dbgECore = 0, dbgGpu = 0;
                bool doDump = false;
                if (isGpu)      { if (dbgGpu   < 3) { doDump = true; dbgGpu++;   } }
                else if (strncmp(name, "PCPU", 4) == 0) { if (dbgPCore < 3) { doDump = true; dbgPCore++; } }
                else { if (dbgECore < 3) { doDump = true; dbgECore++; } }
                if (doDump) {
                    fprintf(dbg_, "[IOReport] chan=%s nStates=%d freqCount=%d\n",
                            name, nStates, freqCount);
                }
                // END DEBUG
                for (int si = 0; si < nStates; ++si) {
                    double freq;
                    if (si < freqCount)
                        freq = ft[si];  // direct mapping: IOReport state i → DVFS[i]
                    else
                        freq = ft[freqCount - 1];  // overflow: use max freq (last table entry)
                    int64_t r = g_StateRes((CFDictionaryRef)channel, si);
                    // Skip IDLE/DOWN/OFF states (macmon behavior: these are not active frequency states)
                    char snameBuf[64] = {};
                    if (g_StateName) {
                        CFStringRef sn = g_StateName((CFDictionaryRef)channel, si);
                        if (sn) { CFStringGetCString(sn, snameBuf, sizeof(snameBuf), kCFStringEncodingUTF8); }
                    }
                    bool isIdle = (strncmp(snameBuf, "IDLE", 4) == 0 ||
                                   strncmp(snameBuf, "DOWN", 4) == 0 ||
                                   strncmp(snameBuf, "OFF", 3) == 0);
                    if (doDump) {
                        fprintf(dbg_, "  [%d] state=%s res=%lld -> %.0f MHz (idx=%d)%s\n",
                                si, snameBuf, (long long)r, freq,
                                si < freqCount ? si : -1,
                                isIdle ? " SKIP" : "");
                    }
                    if (r <= 0) continue;
                    if (isIdle) continue;
                    tres += r; wsum += freq * static_cast<double>(r);
                }
                if (tres > 0) {
                    double mhz = wsum / static_cast<double>(tres);
                    if (doDump) fprintf(dbg_, "  => weighted avg = %.1f MHz (wsum=%.0f tres=%lld)\n", mhz, wsum, (long long)tres);
                    if (isGpu) { gpuFreqSum += mhz; gpuFreqN++; }
                    else if (strncmp(name, "PCPU", 4) == 0) { pCoreFreqSum += mhz; pCoreFreqN++; }
                    else { eCoreFreqSum += mhz; eCoreFreqN++; }
                }
                if (isGpu) continue;
            }
            continue;
        }

        int64_t value = ExtractChannelValue((void*)channel);
        if (value <= 0 || value == INT64_MIN) continue;

        if (strcmp(group, "Energy Model") == 0) {
            double power = EnergyToPower((void*)channel, value) * 1000.0;
            if (power > 0.1 && power < 50000.0) {
                if ((strncmp(name, "ECPU", 4) == 0 || strncmp(name, "PCPU", 4) == 0) && !strstr(name, "SRAM"))
                    cpuPower_.store(cpuPower_.load() + power);
                else if (strcmp(name, "GPU") == 0) gpuPower_.store(power);
                else if (strcmp(name, "ANE") == 0) anePower_.store(power);
            }
        }
        else if (strcmp(group, "ANE") == 0) {
            double power = EnergyToPower((void*)channel, value) * 1000.0;
            if (power >= 0.0 && power < 50000.0) anePower_.store(power);
        }
    }
    if (logCount < 1) ++logCount;
    if (gpuFreqN > 0) gpuFreq_.store(gpuFreqSum / static_cast<double>(gpuFreqN));
    if (pCoreFreqN > 0) pCoreFreq_.store(pCoreFreqSum / static_cast<double>(pCoreFreqN));
    if (eCoreFreqN > 0) eCoreFreq_.store(eCoreFreqSum / static_cast<double>(eCoreFreqN));

    // DEBUG: print computed frequencies (first 10 deltas only)
    {
        static int debugN = 0;
        if (debugN < 10) {
            fprintf(dbg_, "[IOReport] delta#%d: P-core=%.0f MHz  E-core=%.0f MHz  GPU=%.0f MHz\n",
                    debugN, pCoreFreq_.load(), eCoreFreq_.load(), gpuFreq_.load());
            debugN++;
        }
    }
    // END DEBUG
}

// ====================================================================
// ExtractChannelValue + EnergyToPower
// ====================================================================

int64_t PowerMonitor::ExtractChannelValue(void* channelV) {
    auto* channel = static_cast<CFDictionaryRef>(channelV);
    if (g_GetInt) {
        int64_t v = g_GetInt(channel, 0);
        if (v != 0) return v;
    }
    CFArrayRef values = (CFArrayRef)CFDictionaryGetValue(channel, kChannelValuesKey);
    if (values && CFGetTypeID(values) == CFArrayGetTypeID() && CFArrayGetCount(values) > 0) {
        CFNumberRef num = (CFNumberRef)CFArrayGetValueAtIndex(values, 0);
        if (num && CFGetTypeID(num) == CFNumberGetTypeID()) {
            int64_t v = 0;
            CFNumberGetValue(num, kCFNumberSInt64Type, &v);
            return v;
        }
    }
    return 0;
}

double PowerMonitor::EnergyToPower(void* channelV, int64_t energyDelta) {
    auto* channel = static_cast<CFDictionaryRef>(channelV);
    // Default to mJ (most IOReport energy channels report in millijoules)
    double scale = 1e3;
    CFStringRef unitStr = (CFStringRef)CFDictionaryGetValue(channel, kChannelUnitKey);
    if (unitStr && CFGetTypeID(unitStr) == CFStringGetTypeID()) {
        char u[16] = {};
        CFStringGetCString(unitStr, u, sizeof(u), kCFStringEncodingUTF8);
        if (strcmp(u, "mJ") == 0) scale = 1e3;
        else if (strcmp(u, "uJ") == 0) scale = 1e6;
        else if (strcmp(u, "nJ") == 0) scale = 1e9;
    }
    return static_cast<double>(energyDelta) / scale;
}
