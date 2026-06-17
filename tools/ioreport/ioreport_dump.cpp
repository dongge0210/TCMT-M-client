// Dump IOReport channels with REAL values via subscription + delta sampling
// Build: clang++ -std=c++20 ioreport_dump.cpp -o ioreport_dump -framework IOKit -framework CoreFoundation
// Run:   ./ioreport_dump [output_file]
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <cstdio>
#include <csignal>
#include <unistd.h>

typedef void*        (*FnCreateSub)(void*, CFDictionaryRef, void**, uint64_t, void*);
typedef CFDictionaryRef (*FnCreateSamples)(void*, CFDictionaryRef, void*);
typedef CFDictionaryRef (*FnCreateDelta)(CFDictionaryRef, CFDictionaryRef, void*);
typedef int64_t      (*FnGetInt)(CFDictionaryRef, int);
typedef CFStringRef  (*FnGetGroup)(CFDictionaryRef);
typedef CFStringRef  (*FnGetSub)(CFDictionaryRef);
typedef CFStringRef  (*FnGetName)(CFDictionaryRef);
typedef CFStringRef  (*FnGetUnit)(CFDictionaryRef);
typedef int          (*FnGetFormat)(CFDictionaryRef);
typedef int          (*FnStateGetCount)(CFDictionaryRef);
typedef uint64_t     (*FnStateGetResidency)(CFDictionaryRef, int);
typedef CFStringRef  (*FnStateGetName)(CFDictionaryRef, int);

static bool g_running = true;
static void on_sigint(int) { g_running = false; }

int main(int argc, char** argv) {
    signal(SIGINT, on_sigint);

    const char* outPath = argc > 1 ? argv[1] : "ioreport_channels.txt";
    FILE* out = fopen(outPath, "w");
    if (!out) { fprintf(stderr, "Cannot open %s\n", outPath); return 1; }

    void* lib = dlopen("/usr/lib/libIOReport.dylib", RTLD_LAZY);
    if (!lib) { fprintf(stderr, "dlopen failed\n"); fclose(out); return 1; }

    typedef CFDictionaryRef (*FnCopyAll)(uint64_t, uint64_t);
    auto CopyAll = (FnCopyAll)dlsym(lib, "IOReportCopyAllChannels");
    auto CreateSub=(FnCreateSub)dlsym(lib, "IOReportCreateSubscription");
    auto Samples = (FnCreateSamples)dlsym(lib, "IOReportCreateSamples");
    auto Delta   = (FnCreateDelta)dlsym(lib, "IOReportCreateSamplesDelta");
    auto GetInt  = (FnGetInt)  dlsym(lib, "IOReportSimpleGetIntegerValue");
    auto GetGroup= (FnGetGroup)dlsym(lib, "IOReportChannelGetGroup");
    auto GetSub  = (FnGetSub)  dlsym(lib, "IOReportChannelGetSubGroup");
    auto GetName = (FnGetName) dlsym(lib, "IOReportChannelGetChannelName");
    auto GetUnit = (FnGetUnit) dlsym(lib, "IOReportChannelGetUnitLabel");
    auto GetFmt  = (FnGetFormat)dlsym(lib, "IOReportChannelGetFormat");
    auto StCnt   = (FnStateGetCount)dlsym(lib, "IOReportStateGetCount");
    auto StRes   = (FnStateGetResidency)dlsym(lib, "IOReportStateGetResidency");
    auto StName  = (FnStateGetName)dlsym(lib, "IOReportStateGetNameForIndex");

    if (!CopyAll || !CreateSub || !Samples || !Delta || !GetGroup) {
        fprintf(stderr, "dlsym failed\n"); fclose(out); return 1;
    }

    // Query ALL channels
    CFDictionaryRef channels = CopyAll(0, 0);
    if (!channels) { fprintf(stderr, "No channels\n"); fclose(out); return 1; }

    // Subscribe
    void* subOut = nullptr;
    void* sub = CreateSub(nullptr, channels, &subOut, 0, nullptr);
    if (!sub) { fprintf(stderr, "Subscription failed\n"); fclose(out); return 1; }

    // Single sample: baseline → sleep(1s) → sample → delta → dump
    fprintf(stderr, "Sampling (1s)...\n");
    CFDictionaryRef s1 = Samples(sub, channels, nullptr);
    if (!s1) { fprintf(stderr, "First sample failed\n"); fclose(out); return 1; }

    sleep(1);
    CFDictionaryRef s2 = Samples(sub, channels, nullptr);
    if (!s2) { fprintf(stderr, "Second sample failed\n"); CFRelease(s1); fclose(out); return 1; }

    CFDictionaryRef delta = Delta(s1, s2, nullptr);
    CFRelease(s1);
    CFRelease(s2);

    if (!delta) { fprintf(stderr, "Delta failed\n"); fclose(out); return 1; }

    CFArrayRef chArr = (CFArrayRef)CFDictionaryGetValue(delta, CFSTR("IOReportChannels"));
    if (!chArr) { fprintf(stderr, "no channels\n"); CFRelease(delta); fclose(out); return 1; }

    CFIndex count = CFArrayGetCount(chArr);
    fprintf(out, "Total channels: %ld\n\n", count);
    for (CFIndex i = 0; i < count; ++i) {
        auto* ch = (CFDictionaryRef)CFArrayGetValueAtIndex(chArr, i);
        if (!ch) continue;

        char g[128]={}, s[128]={}, n[128]={}, u[128]={};
        if (GetGroup(ch)) CFStringGetCString(GetGroup(ch), g, sizeof(g), kCFStringEncodingUTF8);
        if (GetSub(ch))   CFStringGetCString(GetSub(ch), s, sizeof(s), kCFStringEncodingUTF8);
        if (GetName(ch))  CFStringGetCString(GetName(ch), n, sizeof(n), kCFStringEncodingUTF8);
        if (GetUnit(ch))  CFStringGetCString(GetUnit(ch), u, sizeof(u), kCFStringEncodingUTF8);

        int fmt = GetFmt ? GetFmt(ch) : 0;
        if (fmt == 1) { // Simple report
            int64_t v = GetInt(ch, 0);
            fprintf(out, "%ld|%s|%s|%s|%s|%lld\n", i, g, s, n, u, v);
        } else if (fmt == 2 && StCnt && StRes && StName) { // State report
            int nstates = StCnt(ch);
            fprintf(out, "%ld|STATE|%s|%s|%s|%s|nstates=%d\n", i, g, s, n, u, nstates);
            for (int st = 0; st < nstates; st++) {
                CFStringRef sn = StName(ch, st);
                char stName[64] = {};
                if (sn) CFStringGetCString(sn, stName, sizeof(stName), kCFStringEncodingUTF8);
                uint64_t val = StRes(ch, st);
                fprintf(out, "  state[%d] %s = %llu\n", st, stName, val);
            }
        } else {
            fprintf(out, "%ld|fmt=%d|%s|%s|%s|%s\n", i, fmt, g, s, n, u);
        }
    }
    CFRelease(delta);
    CFRelease(channels);
    dlclose(lib);
    fclose(out);
    fprintf(stderr, "Done.\n");
    return 0;
}
