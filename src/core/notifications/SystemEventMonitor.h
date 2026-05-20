#pragma once
#include <functional>
#include <atomic>

// Platform detection (mirrors project convention in DeviceChangeNotifier.h)
#if !defined(TCMT_WINDOWS) && !defined(TCMT_MACOS) && !defined(TCMT_LINUX)
    #if defined(_WIN32) || defined(_WIN64)
        #define TCMT_WINDOWS
    #elif defined(__APPLE__) && defined(__MACH__)
        #define TCMT_MACOS
    #endif
#endif

// ====================================================================
// SystemEventMonitor
//
// OS-level event callbacks to replace polling for hardware state changes.
//
// macOS: CFRunLoop thread manages IOKit power/sleep/wake, IOPS battery,
//        DiskArbitration mount/unmount, SCDynamicStore network, and
//        notify API thermal state callbacks.
// Windows: Hidden-window message pump thread handles power notifications
//          (RegisterPowerSettingNotification) and disk volume changes.
//
// Usage:
//   SystemEventMonitor monitor;
//   monitor.SetPowerCallback([](bool ac, int pct) { ... });
//   monitor.SetThermalCallback([](int state) { ... });
//   monitor.Start();
//   ...
//   monitor.Stop();
// ====================================================================

// Callback types
using PowerEventCallback   = std::function<void(bool acOnline, int batteryPercent)>;
using DiskEventCallback    = std::function<void()>;
using NetworkEventCallback = std::function<void()>;
using ThermalEventCallback = std::function<void(int state)>;  // 0=nominal,1=fair,2=serious,3=critical

// Forward declarations for friend helper structs (defined in .cpp)
#ifdef TCMT_MACOS
struct MacOSEventCallbacks;
#endif
#ifdef TCMT_WINDOWS
struct Win32EventCallbacks;
#endif

class SystemEventMonitor {
public:
    SystemEventMonitor();
    ~SystemEventMonitor();

    // Non-copyable
    SystemEventMonitor(const SystemEventMonitor&) = delete;
    SystemEventMonitor& operator=(const SystemEventMonitor&) = delete;

    // Register callbacks before Start()
    void SetPowerCallback(PowerEventCallback cb);
    void SetDiskCallback(DiskEventCallback cb);
    void SetNetworkCallback(NetworkEventCallback cb);
    void SetThermalCallback(ThermalEventCallback cb);

    // Start/stop monitoring. Returns false if platform not supported.
    bool Start();
    void Stop();
    bool IsRunning() const;

private:
    // Friend structs — static callbacks defined in .cpp need private member access
#ifdef TCMT_MACOS
    friend struct MacOSEventCallbacks;
#endif
#ifdef TCMT_WINDOWS
    friend struct Win32EventCallbacks;
#endif

    // Platform implementation
    bool StartMacOS();
    void StopMacOS();
    bool StartWindows();
    void StopWindows();

    // Callbacks
    PowerEventCallback   powerCb_;
    DiskEventCallback    diskCb_;
    NetworkEventCallback networkCb_;
    ThermalEventCallback thermalCb_;

    std::atomic<bool> running_{false};

    // Platform handles (opaque)
#ifdef TCMT_MACOS
    void* powerNotifier_{nullptr};    // io_connect_t  (IOPowerRootDomain connect)
    void* diskSession_{nullptr};      // DASessionRef
    void* storeRef_{nullptr};         // SCDynamicStoreRef
    void* runLoopThread_{nullptr};    // std::thread*
#endif
#ifdef TCMT_WINDOWS
    void* powerNotify_{nullptr};      // HPOWERNOTIFY
    void* notifyHwnd_{nullptr};       // HWND (hidden power-notify window)
    void* notifyThread_{nullptr};     // std::thread*
#endif
};
