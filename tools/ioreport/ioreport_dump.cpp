// Dump IOReport channels with REAL values via subscription + delta sampling
// Build: clang++ -std=c++20 ioreport_dump.cpp -o ioreport_dump -framework IOKit -framework CoreFoundation
// Run:   ./ioreport_dump [output_file]
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>
#include <cstdio>
#include <csignal>
#include <unistd.h>

typedef CFDictionaryRef (*FnCopyCh)(CFStringRef, CFStringRef, uint64_t, uint64_t, uint64_t);
typedef void         (*FnMerge)(CFDictionaryRef, CFDictionaryRef, void*);
typedef void*        (*FnCreateSub)(void*, CFDictionaryRef, void**, uint64_t, void*);
typedef CFDictionaryRef (*FnCreateSamples)(void*, CFDictionaryRef, void*);
typedef CFDictionaryRef (*FnCreateDelta)(CFDictionaryRef, CFDictionaryRef, void*);
typedef int64_t      (*FnGetInt)(CFDictionaryRef, int);
typedef CFStringRef  (*FnGetGroup)(CFDictionaryRef);
typedef CFStringRef  (*FnGetSub)(CFDictionaryRef);
typedef CFStringRef  (*FnGetName)(CFDictionaryRef);
typedef CFStringRef  (*FnGetUnit)(CFDictionaryRef);

static bool g_running = true;
static void on_sigint(int) { g_running = false; }

int main(int argc, char** argv) {
    signal(SIGINT, on_sigint);

    const char* outPath = argc > 1 ? argv[1] : "ioreport_channels.txt";
    FILE* out = fopen(outPath, "w");
    if (!out) { fprintf(stderr, "Cannot open %s\n", outPath); return 1; }

    void* lib = dlopen("/usr/lib/libIOReport.dylib", RTLD_LAZY);
    if (!lib) { fprintf(stderr, "dlopen failed\n"); fclose(out); return 1; }

    auto CopyCh = (FnCopyCh)   dlsym(lib, "IOReportCopyChannelsInGroup");
    auto Merge   = (FnMerge)   dlsym(lib, "IOReportMergeChannels");
    auto CreateSub=(FnCreateSub)dlsym(lib, "IOReportCreateSubscription");
    auto Samples = (FnCreateSamples)dlsym(lib, "IOReportCreateSamples");
    auto Delta   = (FnCreateDelta)dlsym(lib, "IOReportCreateSamplesDelta");
    auto GetInt  = (FnGetInt)  dlsym(lib, "IOReportSimpleGetIntegerValue");
    auto GetGroup= (FnGetGroup)dlsym(lib, "IOReportChannelGetGroup");
    auto GetSub  = (FnGetSub)  dlsym(lib, "IOReportChannelGetSubGroup");
    auto GetName = (FnGetName) dlsym(lib, "IOReportChannelGetChannelName");
    auto GetUnit = (FnGetUnit) dlsym(lib, "IOReportChannelGetUnitLabel");

    if (!CopyCh || !CreateSub || !Samples || !Delta || !GetGroup) {
        fprintf(stderr, "dlsym failed\n"); fclose(out); return 1;
    }

    // Get channels for key groups
    auto mkStr = [](const char* s) {
        return CFStringCreateWithCString(kCFAllocatorDefault, s, kCFStringEncodingUTF8);
    };
    CFStringRef gEn = mkStr("Energy Model");
    CFStringRef gCpu = mkStr("CPU Stats");
    CFStringRef gGpu = mkStr("GPU Stats");

    CFDictionaryRef eCh = CopyCh(gEn, nullptr, 0, 0, 0);
    CFDictionaryRef cCh = CopyCh(gCpu, nullptr, 0, 0, 0);
    CFDictionaryRef gCh = CopyCh(gGpu, nullptr, 0, 0, 0);

    CFRelease(gEn); CFRelease(gCpu); CFRelease(gGpu);

    CFDictionaryRef merged = eCh ? eCh : (cCh ? cCh : gCh);
    if (Merge) {
        if (eCh && cCh && cCh != merged) { Merge(merged, cCh, nullptr); CFRelease(cCh); }
        if (gCh && gCh != merged) { Merge(merged, gCh, nullptr); CFRelease(gCh); }
    }
    if (!merged) { fprintf(stderr, "No channels\n"); fclose(out); return 1; }

    // Subscribe
    void* subOut = nullptr;
    void* sub = CreateSub(nullptr, merged, &subOut, 0, nullptr);
    if (!sub) { fprintf(stderr, "Subscription failed\n"); fclose(out); return 1; }

    // Single sample: baseline → sleep(1s) → sample → delta → dump
    fprintf(stderr, "Sampling (1s)...\n");
    CFDictionaryRef s1 = Samples(sub, merged, nullptr);
    if (!s1) { fprintf(stderr, "First sample failed\n"); fclose(out); return 1; }

    sleep(1);
    CFDictionaryRef s2 = Samples(sub, merged, nullptr);
    if (!s2) { fprintf(stderr, "Second sample failed\n"); CFRelease(s1); fclose(out); return 1; }

    CFDictionaryRef delta = Delta(s1, s2, nullptr);
    CFRelease(s1);
    CFRelease(s2);

    if (!delta) { fprintf(stderr, "Delta failed\n"); fclose(out); return 1; }

    CFArrayRef channels = (CFArrayRef)CFDictionaryGetValue(delta, CFSTR("IOReportChannels"));
    if (!channels) { fprintf(stderr, "no channels\n"); CFRelease(delta); fclose(out); return 1; }

    CFIndex count = CFArrayGetCount(channels);
    fprintf(out, "Total channels: %ld\n\n", count);
    for (CFIndex i = 0; i < count; ++i) {
        auto* ch = (CFDictionaryRef)CFArrayGetValueAtIndex(channels, i);
        if (!ch) continue;

        char g[128]={}, s[128]={}, n[128]={}, u[128]={};
        if (GetGroup(ch)) CFStringGetCString(GetGroup(ch), g, sizeof(g), kCFStringEncodingUTF8);
        if (GetSub(ch))   CFStringGetCString(GetSub(ch), s, sizeof(s), kCFStringEncodingUTF8);
        if (GetName(ch))  CFStringGetCString(GetName(ch), n, sizeof(n), kCFStringEncodingUTF8);
        if (GetUnit(ch))  CFStringGetCString(GetUnit(ch), u, sizeof(u), kCFStringEncodingUTF8);
        int64_t v = GetInt(ch, 0);

        fprintf(out, "%ld|%s|%s|%s|%s|%lld\n", i, g, s, n, u, v);
    }
    CFRelease(delta);
    CFRelease(merged);
    dlclose(lib);
    fclose(out);
    fprintf(stderr, "Done.\n");
    return 0;
}
