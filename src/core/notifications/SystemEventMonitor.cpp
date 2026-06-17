#include "SystemEventMonitor.h"
#include "Utils/Logger.h"

// ====================================================================
// Platform detection (mirrors project convention in UserNotifier.cpp)
// ====================================================================
#if !defined(TCMT_WINDOWS) && !defined(TCMT_MACOS) && !defined(TCMT_LINUX)
    #if defined(_WIN32) || defined(_WIN64)
        #define TCMT_WINDOWS
    #elif defined(__APPLE__) && defined(__MACH__)
        #define TCMT_MACOS
    #endif
#endif

// ====================================================================
// Common API
// ====================================================================

SystemEventMonitor::SystemEventMonitor() = default;

SystemEventMonitor::~SystemEventMonitor()
{
    Stop();
}

bool SystemEventMonitor::Start()
{
    if (running_.load()) return true;

#ifdef TCMT_MACOS
    return StartMacOS();
#elif defined(TCMT_WINDOWS)
    return StartWindows();
#else
    Logger::Warn("SystemEventMonitor::Start: not supported on this platform");
    return false;
#endif
}

void SystemEventMonitor::Stop()
{
    if (!running_.exchange(false)) return;

#ifdef TCMT_MACOS
    StopMacOS();
#endif
#ifdef TCMT_WINDOWS
    StopWindows();
#endif
}

bool SystemEventMonitor::IsRunning() const
{
    return running_.load();
}

void SystemEventMonitor::SetPowerCallback(PowerEventCallback cb)
{
    powerCb_ = std::move(cb);
}

void SystemEventMonitor::SetDiskCallback(DiskEventCallback cb)
{
    diskCb_ = std::move(cb);
}

void SystemEventMonitor::SetNetworkCallback(NetworkEventCallback cb)
{
    networkCb_ = std::move(cb);
}

void SystemEventMonitor::SetThermalCallback(ThermalEventCallback cb)
{
    thermalCb_ = std::move(cb);
}

// ====================================================================
// macOS Implementation
//
// Single CFRunLoop thread handles all notification sources:
//   - IOKit IORegisterForSystemPower (sleep/wake)
//   - IOPSNotificationCreateRunLoopSource (AC/battery changes)
//   - DiskArbitration (mount/unmount)
//   - SCDynamicStore (network config changes)
//   - notify API (thermal state)
// ====================================================================

#ifdef TCMT_MACOS

#include <IOKit/IOKitLib.h>
#include <IOKit/IOMessage.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#include <IOKit/ps/IOPowerSources.h>
#include <IOKit/ps/IOPSKeys.h>
#include <DiskArbitration/DiskArbitration.h>
#include <SystemConfiguration/SystemConfiguration.h>
#include <notify.h>
#include <thread>
#include <pthread.h>
#include <CoreFoundation/CoreFoundation.h>

// -------------------------------------------------------------------
// Friend helper struct — all static callbacks have private member access
// -------------------------------------------------------------------

struct MacOSEventCallbacks {
    static void PowerNotification(
        void* refcon, io_service_t /*service*/, natural_t messageType, void* messageArgument)
    {
        auto* self = static_cast<SystemEventMonitor*>(refcon);
        if (!self) return;

        switch (messageType) {
        case kIOMessageSystemWillSleep: {
            io_connect_t root = static_cast<io_connect_t>(
                reinterpret_cast<uintptr_t>(self->powerNotifier_));
            IOAllowPowerChange(root, reinterpret_cast<long>(messageArgument));
            Logger::Debug("SystemEventMonitor: system will sleep");
            break;
        }
        case kIOMessageSystemHasPoweredOn:
            Logger::Debug("SystemEventMonitor: system has powered on");
            if (self->powerCb_) {
                CFTypeRef psInfo = IOPSCopyPowerSourcesInfo();
                CFArrayRef psList = IOPSCopyPowerSourcesList(psInfo);
                bool acOnline = true;
                int batteryPercent = -1;
                if (psList && CFArrayGetCount(psList) > 0) {
                    CFDictionaryRef ps = IOPSGetPowerSourceDescription(
                        psInfo, CFArrayGetValueAtIndex(psList, 0));
                    if (ps) {
                        auto* stateStr = static_cast<CFStringRef>(
                            CFDictionaryGetValue(ps, CFSTR(kIOPSPowerSourceStateKey)));
                        acOnline = stateStr ? CFEqual(stateStr, CFSTR(kIOPSACPowerValue)) != 0 : true;
                        auto* curNum = static_cast<CFNumberRef>(
                            CFDictionaryGetValue(ps, CFSTR(kIOPSCurrentCapacityKey)));
                        auto* maxNum = static_cast<CFNumberRef>(
                            CFDictionaryGetValue(ps, CFSTR(kIOPSMaxCapacityKey)));
                        int cur = 0, maxV = 0;
                        if (curNum && maxNum &&
                            CFNumberGetValue(curNum, kCFNumberIntType, &cur) &&
                            CFNumberGetValue(maxNum, kCFNumberIntType, &maxV) && maxV > 0) {
                            batteryPercent = (cur * 100) / maxV;
                        }
                    }
                }
                if (psList) CFRelease(psList);
                if (psInfo) CFRelease(psInfo);
                self->powerCb_(acOnline, batteryPercent);
            }
            break;
        default:
            break;
        }
    }

    static void PowerSource(void* context)
    {
        auto* self = static_cast<SystemEventMonitor*>(context);
        if (!self || !self->powerCb_) return;

        CFTypeRef psInfo = IOPSCopyPowerSourcesInfo();
        CFArrayRef psList = IOPSCopyPowerSourcesList(psInfo);
        bool acOnline = true;
        int batteryPercent = -1;

        if (psList && CFArrayGetCount(psList) > 0) {
            CFDictionaryRef ps = IOPSGetPowerSourceDescription(
                psInfo, CFArrayGetValueAtIndex(psList, 0));
            if (ps) {
                auto* stateStr = static_cast<CFStringRef>(
                    CFDictionaryGetValue(ps, CFSTR(kIOPSPowerSourceStateKey)));
                acOnline = stateStr ? CFEqual(stateStr, CFSTR(kIOPSACPowerValue)) != 0 : true;
                auto* curNum = static_cast<CFNumberRef>(
                    CFDictionaryGetValue(ps, CFSTR(kIOPSCurrentCapacityKey)));
                auto* maxNum = static_cast<CFNumberRef>(
                    CFDictionaryGetValue(ps, CFSTR(kIOPSMaxCapacityKey)));
                int cur = 0, maxV = 0;
                if (curNum && maxNum &&
                    CFNumberGetValue(curNum, kCFNumberIntType, &cur) &&
                    CFNumberGetValue(maxNum, kCFNumberIntType, &maxV) && maxV > 0) {
                    batteryPercent = (cur * 100) / maxV;
                }
            }
        }

        if (psList) CFRelease(psList);
        if (psInfo) CFRelease(psInfo);

        self->powerCb_(acOnline, batteryPercent);
    }

    static void DiskAppeared(DADiskRef /*disk*/, void* context)
    {
        auto* self = static_cast<SystemEventMonitor*>(context);
        if (self && self->diskCb_) self->diskCb_();
    }

    static void DiskDisappeared(DADiskRef /*disk*/, void* context)
    {
        auto* self = static_cast<SystemEventMonitor*>(context);
        if (self && self->diskCb_) self->diskCb_();
    }

    static void NetworkChanged(SCDynamicStoreRef /*store*/, CFArrayRef /*changedKeys*/, void* context)
    {
        auto* self = static_cast<SystemEventMonitor*>(context);
        if (self && self->networkCb_) self->networkCb_();
    }

    static void ThermalHandler(int token, SystemEventMonitor* self)
    {
        uint64_t stateVal = 0;
        notify_get_state(token, &stateVal);
        if (self && self->thermalCb_) {
            self->thermalCb_(static_cast<int>(stateVal));
        }
    }
};

// -------------------------------------------------------------------
// Start / Stop
// -------------------------------------------------------------------

bool SystemEventMonitor::StartMacOS()
{
    // Spawn dedicated CFRunLoop thread
    auto* t = new std::thread([this]() {
        pthread_setname_np("com.tcmt.systemevent");

        CFRunLoopRef rl = CFRunLoopGetCurrent();

        // ---- (1) IOKit power sleep/wake ----
        IONotificationPortRef powerPort = IONotificationPortCreate(MACH_PORT_NULL);
        if (!powerPort) {
            Logger::Error("SystemEventMonitor: IONotificationPortCreate failed");
            return;
        }
        CFRunLoopSourceRef powerSrc = IONotificationPortGetRunLoopSource(powerPort);
        CFRunLoopAddSource(rl, powerSrc, kCFRunLoopDefaultMode);

        io_service_t rootService = IORegistryEntryFromPath(
            MACH_PORT_NULL, kIOServicePlane ":/IOPowerRootDomain");
        if (rootService) {
            io_connect_t powerRoot = IORegisterForSystemPower(
                this, &powerPort, MacOSEventCallbacks::PowerNotification, &rootService);
            IOObjectRelease(rootService);
            powerNotifier_ = reinterpret_cast<void*>(
                static_cast<uintptr_t>(static_cast<unsigned int>(powerRoot)));
        }

        // ---- (2) IOPS AC/battery changes ----
        CFRunLoopSourceRef psSrc = IOPSNotificationCreateRunLoopSource(
            MacOSEventCallbacks::PowerSource, this);
        if (psSrc) {
            CFRunLoopAddSource(rl, psSrc, kCFRunLoopDefaultMode);
        }

        // ---- (3) DiskArbitration mount/unmount ----
        DASessionRef session = DASessionCreate(kCFAllocatorDefault);
        if (session) {
            diskSession_ = reinterpret_cast<void*>(session);
            DARegisterDiskAppearedCallback(session, nullptr, MacOSEventCallbacks::DiskAppeared, this);
            DARegisterDiskDisappearedCallback(session, nullptr, MacOSEventCallbacks::DiskDisappeared, this);
            DASessionScheduleWithRunLoop(session, rl, kCFRunLoopDefaultMode);
        }

        // ---- (4) SCDynamicStore network changes ----
        SCDynamicStoreContext ctx = { 0, this, nullptr, nullptr, nullptr };
        SCDynamicStoreRef store = SCDynamicStoreCreate(
            kCFAllocatorDefault, CFSTR("TCMT"), MacOSEventCallbacks::NetworkChanged, &ctx);
        if (store) {
            storeRef_ = const_cast<void*>(static_cast<const void*>(store));
            const void* keys[] = { CFSTR("State:/Network/Global/IPv4") };
            CFArrayRef keyArray = CFArrayCreate(
                kCFAllocatorDefault, keys, 1, &kCFTypeArrayCallBacks);
            SCDynamicStoreSetNotificationKeys(store, keyArray, nullptr);
            if (keyArray) CFRelease(keyArray);

            CFRunLoopSourceRef netSrc = SCDynamicStoreCreateRunLoopSource(
                kCFAllocatorDefault, store, 0);
            CFRunLoopAddSource(rl, netSrc, kCFRunLoopDefaultMode);
        }

        // ---- (5) Thermal state via notify API ----
        dispatch_queue_t thermalQueue = dispatch_queue_create(
            "com.tcmt.thermal", DISPATCH_QUEUE_SERIAL);
        int thToken = 0;
        // Use notify_register_dispatch so the callback runs on a serial queue
        notify_register_dispatch("com.apple.system.thermal", &thToken,
            thermalQueue, ^(int /*t*/) {
                MacOSEventCallbacks::ThermalHandler(thToken, this);
            });

        // ---- Signal ready ----
        running_.store(true);
        Logger::Info("SystemEventMonitor: started (macOS CFRunLoop)");

        // ---- Run loop until Stop() ----
        while (running_.load()) {
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 1.0, true);
        }

        // ---- Cleanup all notification sources ----
        Logger::Debug("SystemEventMonitor: cleaning up macOS notification sources");

        powerNotifier_ = nullptr;

        if (psSrc) {
            CFRunLoopRemoveSource(rl, psSrc, kCFRunLoopDefaultMode);
            CFRelease(psSrc);
        }

        if (session) {
            DASessionUnscheduleFromRunLoop(session, rl, kCFRunLoopDefaultMode);
            CFRelease(session);
        }
        diskSession_ = nullptr;

        if (store) {
            CFRelease(store);
        }
        storeRef_ = nullptr;

        if (thToken) notify_cancel(thToken);
        if (thermalQueue) dispatch_release(thermalQueue);

        if (powerPort) {
            CFRunLoopRemoveSource(rl, powerSrc, kCFRunLoopDefaultMode);
            IONotificationPortDestroy(powerPort);
        }
    });

    runLoopThread_ = reinterpret_cast<void*>(t);

    // Wait briefly for the thread to signal running
    for (int retries = 0; retries < 50 && !running_.load(); ++retries) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!running_.load()) {
        Logger::Error("SystemEventMonitor: CFRunLoop thread failed to start");
        if (t->joinable()) t->detach();
        delete t;
        runLoopThread_ = nullptr;
        return false;
    }

    return true;
}

void SystemEventMonitor::StopMacOS()
{
    if (runLoopThread_) {
        auto* t = static_cast<std::thread*>(runLoopThread_);
        if (t->joinable()) t->join();
        delete t;
        runLoopThread_ = nullptr;
    }
}

#endif // TCMT_MACOS

// ====================================================================
// Windows Implementation
//
// Hidden overlapped window + message pump thread:
//   - RegisterPowerSettingNotification (GUID_ACDC_POWER_SOURCE)
//   - WM_DEVICECHANGE with DBT_DEVTYP_VOLUME for disk changes
// Network and thermal are no-ops (covered by ETW in this project).
// ====================================================================

#ifdef TCMT_WINDOWS

#include <winsock2.h>
#include <windows.h>
#include <dbt.h>
#include <thread>
#include <utility>

// GUID for ACDC power source (from Windows SDK, defined here for portability)
// {5D3E9A59-E9D5-4B00-A6BD-FF1FF3E178F9}
static const GUID GUID_TCMT_ACDC_POWER_SOURCE = {
    0x5D3E9A59, 0xE9D5, 0x4B00,
    { 0xA6, 0xBD, 0xFF, 0x1F, 0xF3, 0xE1, 0x78, 0xF9 }
};

// GUID for battery percentage remaining (from Windows SDK)
// {A816EAA3-2A3D-4F34-A5F5-82463D9A0F8B}
static const GUID GUID_TCMT_BATTERY_PERCENTAGE = {
    0xA816EAA3, 0x2A3D, 0x4F34,
    { 0xA5, 0xF5, 0x82, 0x46, 0x3D, 0x9A, 0x0F, 0x8B }
};

// GUID for volume device interface (used with RegisterDeviceNotification)
// {53f5630d-b6bf-11d0-94f2-00a0c91efb8b}
static const GUID GUID_TCMT_DEVINTERFACE_VOLUME = {
    0x53f5630d, 0xb6bf, 0x11d0,
    { 0x94, 0xf2, 0x00, 0xa0, 0xc9, 0x1e, 0xfb, 0x8b }
};

// -------------------------------------------------------------------
// Friend helper struct — all static callbacks have private member access
// -------------------------------------------------------------------

static const wchar_t* g_semWindowClass = L"TCMT_SystemEventMonitor";

struct Win32EventCallbacks {
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        auto* self = reinterpret_cast<SystemEventMonitor*>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            break;
        }

        case WM_POWERBROADCAST:
            if (wParam == PBT_POWERSETTINGCHANGE && self) {
                auto* pbs = reinterpret_cast<POWERBROADCAST_SETTING*>(lParam);
                if (pbs && pbs->PowerSetting == GUID_TCMT_ACDC_POWER_SOURCE &&
                    pbs->DataLength >= sizeof(DWORD)) {
                    bool acOnline = (pbs->Data[0] == 1);
                    if (self->powerCb_) {
                        self->powerCb_(acOnline, -1);
                    }
                    Logger::Debug("SystemEventMonitor: power source changed, AC=" +
                        std::to_string(static_cast<int>(acOnline)));
                }
            }
            return TRUE;

        case WM_DEVICECHANGE:
            if ((wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE) &&
                self && self->diskCb_) {
                auto* hdr = reinterpret_cast<DEV_BROADCAST_HDR*>(lParam);
                if (hdr && hdr->dbch_devicetype == DBT_DEVTYP_VOLUME) {
                    self->diskCb_();
                    Logger::Debug("SystemEventMonitor: disk volume change detected");
                }
            }
            return TRUE;

        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;

        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        }

        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
};

// -------------------------------------------------------------------
// Start / Stop
// -------------------------------------------------------------------

bool SystemEventMonitor::StartWindows()
{
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = Win32EventCallbacks::WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = g_semWindowClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        Logger::Error("SystemEventMonitor: RegisterClassExW failed, error=" +
            std::to_string(GetLastError()));
        return false;
    }

    // Create hidden overlapped window (must be top-level for WM_DEVICECHANGE)
    HWND hwnd = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
        g_semWindowClass, L"TCMT_EventMonitor",
        WS_POPUP,
        0, 0, 0, 0,
        nullptr, nullptr, hInst, this);
    if (!hwnd) {
        Logger::Error("SystemEventMonitor: CreateWindowExW failed, error=" +
            std::to_string(GetLastError()));
        return false;
    }
    notifyHwnd_ = reinterpret_cast<void*>(hwnd);

    // Register for power setting notifications (AC/DC source)
    HPOWERNOTIFY hPower = RegisterPowerSettingNotification(
        hwnd, &GUID_TCMT_ACDC_POWER_SOURCE, DEVICE_NOTIFY_WINDOW_HANDLE);
    if (!hPower) {
        Logger::Warn("SystemEventMonitor: RegisterPowerSettingNotification ACDC failed, error=" +
            std::to_string(GetLastError()));
        // Continue without power notification — non-fatal
    }
    powerNotify_ = reinterpret_cast<void*>(hPower);

    // Register for volume device notifications (disk changes)
    DEV_BROADCAST_DEVICEINTERFACE_W dbci = {};
    dbci.dbcc_size       = sizeof(dbci);
    dbci.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
    dbci.dbcc_classguid  = GUID_TCMT_DEVINTERFACE_VOLUME;
    HDEVNOTIFY hVolNotify = RegisterDeviceNotificationW(
        hwnd, &dbci, DEVICE_NOTIFY_WINDOW_HANDLE);
    if (!hVolNotify) {
        Logger::Warn("SystemEventMonitor: RegisterDeviceNotification volume failed, error=" +
            std::to_string(GetLastError()));
        // Continue without volume notification — non-fatal
    }

    // Spawn message pump thread
    auto* t = new std::thread([this, hwnd, hPower, hVolNotify]() {
        running_.store(true);
        Logger::Info("SystemEventMonitor: started (Windows message pump)");

        MSG msg = {};
        while (GetMessageW(&msg, nullptr, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }

        // Cleanup
        if (hVolNotify) UnregisterDeviceNotification(hVolNotify);
        if (hPower) UnregisterPowerSettingNotification(hPower);
        DestroyWindow(hwnd);
    });

    notifyThread_ = reinterpret_cast<void*>(t);

    // Wait briefly for the thread to signal running
    for (int retries = 0; retries < 50 && !running_.load(); ++retries) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (!running_.load()) {
        Logger::Error("SystemEventMonitor: message pump thread failed to start");
        if (t->joinable()) t->detach();
        delete t;
        notifyThread_ = nullptr;
        notifyHwnd_   = nullptr;
        powerNotify_  = nullptr;
        return false;
    }

    return true;
}

void SystemEventMonitor::StopWindows()
{
    // Signal message pump to exit
    if (notifyHwnd_) {
        PostMessageW(static_cast<HWND>(notifyHwnd_), WM_CLOSE, 0, 0);
    }

    if (notifyThread_) {
        auto* t = static_cast<std::thread*>(notifyThread_);
        if (t->joinable()) t->join();
        delete t;
        notifyThread_ = nullptr;
    }

    powerNotify_ = nullptr;
    notifyHwnd_  = nullptr;
}

#endif // TCMT_WINDOWS
