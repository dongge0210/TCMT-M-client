#pragma once

#include <atomic>

// Platform detection (mirrors project convention)
#if !defined(TCMT_WINDOWS) && !defined(TCMT_MACOS) && !defined(TCMT_LINUX)
    #if defined(_WIN32) || defined(_WIN64)
        #define TCMT_WINDOWS
    #elif defined(__APPLE__) && defined(__MACH__)
        #define TCMT_MACOS
    #endif
#endif

// ====================================================================
// DeviceChangeNotifier
//
// Lightweight, no-window event-based device-change notification for
// USB and Bluetooth.
//
// Windows:  CM_Register_Notification (Win10 1703+) via dynamic load.
//           Falls back to always-true polling on older Windows.
// macOS:    IOKit IOServiceAddMatchingNotification on background
//           CFRunLoop thread.
//
// Usage:
//   static DeviceChangeNotifier usbNotify(DeviceChangeNotifier::USB);
//   if (usbNotify.Poll()) { /* call UsbInfo::Detect() */ }
// ====================================================================

#ifdef TCMT_WINDOWS
// ======================== Windows Implementation ========================
#include <winsock2.h>
#include <windows.h>
#include <usbiodef.h>   // GUID_DEVINTERFACE_USB_DEVICE

#ifndef GUID_BTHPORT_DEVICE_INTERFACE
// {0850302A-B4EC-4a43-B415-46BD3050484B} — from bthdef.h
static const GUID GUID_BTHPORT_DEVICE_INTERFACE = {
    0x0850302A, 0xB4EC, 0x4a43,
    { 0xB4, 0x15, 0x46, 0xBD, 0x30, 0x50, 0x48, 0x4B }
};
#endif

// CM_Register_Notification — available since Windows 10 1703.
// Declare types manually to avoid SDK version dependency.

#ifndef CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE
#define CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE 0
#endif

#ifndef CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL
#define CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL 0
#define CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL  1
#endif

struct DEVCHG_DEVICE_INTERFACE_FILTER {
    GUID ClassGuid;
};

struct DEVCHG_FILTER {
    DWORD cbSize;
    DWORD Flags;
    DWORD FilterType;
    DWORD Reserved;
    DEVCHG_DEVICE_INTERFACE_FILTER DeviceInterface;
};

typedef DWORD (WINAPI *FnCM_Register_Notification)(
    void* pFilter, void* pContext,
    DWORD (WINAPI *pCallback)(void*, void*, DWORD, void*, DWORD),
    void** pNotifyContext);

typedef DWORD (WINAPI *FnCM_Unregister_Notification)(void* hNotify);

class DeviceChangeNotifier {
public:
    enum DeviceClass { USB, Bluetooth, USB_Hub };

    explicit DeviceChangeNotifier(DeviceClass devClass)
        : devClass_(devClass), fallback_(false) {

        const GUID* guidPtr;
        if (devClass == USB_Hub)
            guidPtr = &GUID_DEVINTERFACE_USB_HUB;
        else if (devClass == USB)
            guidPtr = &GUID_DEVINTERFACE_USB_DEVICE;
        else
            guidPtr = &GUID_BTHPORT_DEVICE_INTERFACE;

        HMODULE hCfg = GetModuleHandleW(L"cfgmgr32.dll");
        if (!hCfg) hCfg = LoadLibraryW(L"cfgmgr32.dll");
        if (!hCfg) { fallback_ = true; return; }

        auto pfnRegister = reinterpret_cast<FnCM_Register_Notification>(
            GetProcAddress(hCfg, "CM_Register_Notification"));

        if (!pfnRegister) { fallback_ = true; return; }

        DEVCHG_FILTER filter = {};
        filter.cbSize = sizeof(DEVCHG_FILTER);
        filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
        filter.DeviceInterface.ClassGuid = *guidPtr;

        if (pfnRegister(&filter, this, &DeviceChangeNotifier::Callback,
                        &notifyHandle_) != 0 /* ERROR_SUCCESS */) {
            fallback_ = true;
        }
    }

    ~DeviceChangeNotifier() {
        if (!fallback_ && notifyHandle_) {
            HMODULE hCfg = GetModuleHandleW(L"cfgmgr32.dll");
            if (hCfg) {
                auto pfnUnreg = reinterpret_cast<FnCM_Unregister_Notification>(
                    GetProcAddress(hCfg, "CM_Unregister_Notification"));
                if (pfnUnreg) pfnUnreg(notifyHandle_);
            }
        }
    }

    // Returns true if device change was detected since last Poll().
    // On platforms where notifications aren't available, always returns true
    // (polling fallback — no change in behavior).
    bool Poll() {
        return changed_.exchange(false) || fallback_;
    }

private:
    static DWORD WINAPI Callback(
        void* /*hNotify*/, void* context, DWORD action,
        void* /*eventData*/, DWORD /*eventDataSize*/) {
        if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL ||
            action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL) {
            static_cast<DeviceChangeNotifier*>(context)->changed_.store(true);
        }
        return 0;
    }

    DeviceClass devClass_;
    std::atomic<bool> changed_{true};  // true so first Poll() triggers
    std::atomic<bool> fallback_{false};
    void* notifyHandle_{nullptr};
};

#elif defined(TCMT_MACOS)
// ======================== macOS Implementation ========================
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <thread>
#include <string>

class DeviceChangeNotifier {
public:
    enum DeviceClass { USB, Bluetooth, USB_Hub };

    explicit DeviceChangeNotifier(DeviceClass devClass)
        // USB_Hub == USB on macOS (IOUSBHostDevice covers both devices and hubs)
        : className_((devClass == USB || devClass == USB_Hub) ? "IOUSBHostDevice"
                      : "IOBluetoothHostController") {
        changed_.store(true);

        runLoopThread_ = std::thread([this]() {
            CFRunLoopRef rl = CFRunLoopGetCurrent();
            // kIOMasterPortDefault deprecated on 12+; kIOMainPortDefault requires 12+;
            // IONotificationPortCreate(0) works identically on all versions with zero warnings.
            notifyPort_ = IONotificationPortCreate(0);
            if (!notifyPort_) return;

            CFRunLoopSourceRef src = IONotificationPortGetRunLoopSource(notifyPort_);
            CFRunLoopAddSource(rl, src, kCFRunLoopDefaultMode);

            CFMutableDictionaryRef matching = IOServiceMatching(className_.c_str());
            if (matching) {
                IOServiceAddMatchingNotification(
                    notifyPort_, kIOFirstMatchNotification, matching,
                    &DeviceChangeNotifier::IOCallback, this, &iterator_);
                DeviceChangeNotifier::IOCallback(this, iterator_);
            }
            CFMutableDictionaryRef termMatching = IOServiceMatching(className_.c_str());
            if (termMatching) {
                IOServiceAddMatchingNotification(
                    notifyPort_, kIOTerminatedNotification, termMatching,
                    &DeviceChangeNotifier::IOCallback, this, &termIterator_);
            }

            while (!stopFlag_.load()) {
                CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.5, true);
            }

            if (notifyPort_) {
                CFRunLoopRemoveSource(rl, src, kCFRunLoopDefaultMode);
                IONotificationPortDestroy(notifyPort_);
                notifyPort_ = nullptr;
            }
        });
    }

    ~DeviceChangeNotifier() {
        stopFlag_.store(true);
        if (runLoopThread_.joinable()) runLoopThread_.join();
        if (iterator_) IOObjectRelease(iterator_);
        if (termIterator_) IOObjectRelease(termIterator_);
    }

    bool Poll() {
        return changed_.exchange(false);
    }

private:
    static void IOCallback(void* refcon, io_iterator_t iterator) {
        auto* self = static_cast<DeviceChangeNotifier*>(refcon);
        io_object_t obj;
        while ((obj = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
            IOObjectRelease(obj);
        }
        self->changed_.store(true);
    }

    std::string className_;
    std::atomic<bool> changed_{true};
    std::atomic<bool> stopFlag_{false};
    std::thread runLoopThread_;
    IONotificationPortRef notifyPort_{nullptr};
    io_iterator_t iterator_{0};
    io_iterator_t termIterator_{0};
};

#else
// ======================== Fallback ========================
class DeviceChangeNotifier {
public:
    enum DeviceClass { USB, Bluetooth, USB_Hub };
    explicit DeviceChangeNotifier(DeviceClass) {}
    bool Poll() { return true; }
};
#endif
