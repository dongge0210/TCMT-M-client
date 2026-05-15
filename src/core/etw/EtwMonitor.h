#pragma once

#include <functional>
#include <string>
#include <atomic>

namespace tcmt { namespace etw {

// Callback types — called from ETW background thread on Windows.
// Must not block.
using PowerEventCallback   = std::function<void(bool acOnline)>;
using BatteryEventCallback = std::function<void(int percent)>;
using NetworkEventCallback = std::function<void()>;
using WifiEventCallback    = std::function<void()>;
using BluetoothEventCallback = std::function<void()>;
using UsbEventCallback     = std::function<void()>;
using CpuFreqEventCallback = std::function<void()>;

#if defined(TCMT_WINDOWS)

// ======================== Windows — real ETW implementation ========================
class EtwMonitor {
public:
    EtwMonitor();
    ~EtwMonitor();
    EtwMonitor(const EtwMonitor&) = delete;
    EtwMonitor& operator=(const EtwMonitor&) = delete;

    bool Start();
    void Stop();
    bool IsRunning() const;
    std::string GetLastError() const;

    void SetPowerCallback(PowerEventCallback cb);
    void SetBatteryCallback(BatteryEventCallback cb);
    void SetNetworkCallback(NetworkEventCallback cb);
    void SetWifiCallback(WifiEventCallback cb);
    void SetBluetoothCallback(BluetoothEventCallback cb);
    void SetUsbCallback(UsbEventCallback cb);
    void SetCpuFreqCallback(CpuFreqEventCallback cb);

private:
    void DispatchEvent(const void* pEvent);

    void* sessionHandle_ = nullptr;
    void* traceHandle_   = nullptr;
    std::atomic<bool> running_{false};
    std::string lastError_;
    std::wstring sessionName_;

    struct JThreadDeleter { void operator()(void*) const; };
    std::unique_ptr<void, JThreadDeleter> traceThread_;

    PowerEventCallback    powerCb_;
    BatteryEventCallback  batteryCb_;
    NetworkEventCallback  networkCb_;
    WifiEventCallback     wifiCb_;
    BluetoothEventCallback btCb_;
    UsbEventCallback      usbCb_;
    CpuFreqEventCallback  cpuFreqCb_;
};

#else

// ======================== Non-Windows stub ========================
class EtwMonitor {
public:
    EtwMonitor() = default;
    ~EtwMonitor() = default;
    EtwMonitor(const EtwMonitor&) = delete;
    EtwMonitor& operator=(const EtwMonitor&) = delete;

    bool Start() { lastError_ = "ETW not available on this platform"; return false; }
    void Stop() {}
    bool IsRunning() const { return false; }
    std::string GetLastError() const { return lastError_; }

    void SetPowerCallback(PowerEventCallback) {}
    void SetBatteryCallback(BatteryEventCallback) {}
    void SetNetworkCallback(NetworkEventCallback) {}
    void SetWifiCallback(WifiEventCallback) {}
    void SetBluetoothCallback(BluetoothEventCallback) {}
    void SetUsbCallback(UsbEventCallback) {}
    void SetCpuFreqCallback(CpuFreqEventCallback) {}

private:
    std::string lastError_;
};

#endif

} // namespace etw
} // namespace tcmt
