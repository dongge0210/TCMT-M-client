#pragma once

#include <functional>
#include <string>
#include <atomic>
#include <memory>

namespace tcmt { namespace compat { class JThread; } }

namespace tcmt { namespace etw {

using PowerEventCallback   = std::function<void(bool acOnline)>;
using BatteryEventCallback = std::function<void(int percent)>;
using NetworkEventCallback = std::function<void()>;
using WifiEventCallback    = std::function<void()>;
using BluetoothEventCallback = std::function<void()>;
using UsbEventCallback     = std::function<void()>;
using CpuFreqEventCallback = std::function<void()>;

#if defined(TCMT_WINDOWS)

class EtwMonitor {
public:
    EtwMonitor();
    ~EtwMonitor();
    EtwMonitor(const EtwMonitor&) = delete;
    EtwMonitor& operator=(const EtwMonitor&) = delete;

    bool Start();
    void Stop();
    bool IsRunning() const { return running_.load(std::memory_order_acquire); }
    std::string GetLastError() const { return lastError_; }

    void SetPowerCallback(PowerEventCallback cb)      { powerCb_ = std::move(cb); }
    void SetBatteryCallback(BatteryEventCallback cb)  { batteryCb_ = std::move(cb); }
    void SetNetworkCallback(NetworkEventCallback cb)  { networkCb_ = std::move(cb); }
    void SetWifiCallback(WifiEventCallback cb)        { wifiCb_ = std::move(cb); }
    void SetBluetoothCallback(BluetoothEventCallback cb) { btCb_ = std::move(cb); }
    void SetUsbCallback(UsbEventCallback cb)          { usbCb_ = std::move(cb); }
    void SetCpuFreqCallback(CpuFreqEventCallback cb)  { cpuFreqCb_ = std::move(cb); }

private:
    void DispatchEvent(const void* pEvent);
    static unsigned short GetEventId(const void* pEvent);
    static std::string FormatProviderName(const void* pEvent);

    void* sessionHandle_ = nullptr;
    void* traceHandle_   = nullptr;
    std::atomic<bool> running_{false};
    std::string lastError_;
    std::wstring sessionName_;
    std::unique_ptr<tcmt::compat::JThread> traceThread_;

    PowerEventCallback    powerCb_;
    BatteryEventCallback  batteryCb_;
    NetworkEventCallback  networkCb_;
    WifiEventCallback     wifiCb_;
    BluetoothEventCallback btCb_;
    UsbEventCallback      usbCb_;
    CpuFreqEventCallback  cpuFreqCb_;
};

#else

class EtwMonitor {
public:
    EtwMonitor() = default;
    ~EtwMonitor() = default;
    EtwMonitor(const EtwMonitor&) = delete;
    EtwMonitor& operator=(const EtwMonitor&) = delete;

    bool Start() { lastError_ = "ETW not available"; return false; }
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
