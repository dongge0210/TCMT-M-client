#pragma once
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <string>
#include "DataStruct/DataStruct.h"
#include "../tui/TuiApp.h"
#include "Utils/JThreadCompat.h"
#include "etw/EtwMonitor.h"
#include "SystemEventMonitor.h"

struct ModuleData {
    // CPU (500ms) — scalars use atomic
    std::atomic<double> cpuUsage{0.0};
    std::atomic<double> pCoreFreq{0.0};
    std::atomic<double> eCoreFreq{0.0};
    // Memory (1s)
    std::atomic<uint64_t> totalMemory{0};
    std::atomic<uint64_t> usedMemory{0};
    std::atomic<uint64_t> availableMemory{0};
    std::atomic<uint64_t> compressedMemory{0};
    // GPU (500ms)
    std::atomic<double> gpuUsage{0.0};
    std::atomic<uint64_t> gpuMemory{0};
    std::atomic<double> gpuTemp{0.0};
    std::atomic<bool> gpuIsVirtual{false};
    std::mutex gpuMutex;
    std::string gpuName;
    std::string gpuBrand;
    // Disk (5s)
    std::mutex diskMutex;
    std::vector<DiskData> disks;
    // Network (1s)
    std::mutex netMutex;
    struct NetSlot { std::string name, ip, mac, type; uint64_t speed, dl, ul; };
    std::vector<NetSlot> adapters;
    // Temperature (2s)
    std::mutex tempMutex;
    std::vector<std::pair<std::string, double>> temperatures;
    double cpuTemperature{0.0};
    double gpuTemperature{0.0};
    // Power (2s)
    std::atomic<int> batteryPercent{-1};
    std::atomic<bool> acOnline{false};
    // ETW dirty flags — set by callbacks, checked by loops
    std::atomic<bool> powerDirty{false};
    std::atomic<bool> networkDirty{false};
    std::atomic<bool> wifiDirty{false};
    std::atomic<bool> btDirty{false};
    std::atomic<bool> usbDirty{false};
    std::atomic<bool> cpuFreqDirty{false};
    // SystemEventMonitor dirty flags
    std::atomic<bool> diskDirty{false};
    std::atomic<bool> sysPowerDirty{false};  // sleep/wake events
    std::atomic<bool> thermalDirty{false};
};

// Free function thread loops (implemented in ModuleCoordinator_cpu.cpp)
void CpuLoop(ModuleData& data, tcmt::compat::StopToken st);
void MemoryLoop(ModuleData& data, tcmt::compat::StopToken st);
void PowerLoop(ModuleData& data, tcmt::compat::StopToken st);

class ModuleCoordinator {
public:
    ModuleCoordinator();
    ~ModuleCoordinator();
    void Start();
    void Stop();
    void Snapshot(SystemInfo& sysInfo, tcmt::TuiData& tuiData);
    static void SleepFor(tcmt::compat::StopToken st, int ms);
    bool IsWifiDirty() { return data_.wifiDirty.exchange(false); }
    bool IsBtDirty()   { return data_.btDirty.exchange(false); }
    bool IsUsbDirty()  { return data_.usbDirty.exchange(false); }
private:
    ModuleData data_;
    std::atomic<bool> running_{false};
    tcmt::compat::JThread cpuThread_, memoryThread_, diskThread_, netThread_, tempThread_, powerThread_;
    tcmt::etw::EtwMonitor etwMonitor_;
    SystemEventMonitor sysEventMonitor_;
    void DiskLoop(tcmt::compat::StopToken st);
    void NetworkLoop(tcmt::compat::StopToken st);
    void TemperatureLoop(tcmt::compat::StopToken st);
};
