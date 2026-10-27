#pragma once
#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <string>
#include "DataStruct/DataStruct.h"
#include "Utils/JThreadCompat.h"

// Forward declaration only — Snapshot takes TuiData by reference, so this header has no
// business pulling in ../../tui/TuiApp.h. That include was the last remaining edge from
// src/core to src/tui, and it dragged the whole TUI into every file including this header.
// The definition lives in TuiApp.h, which ModuleCoordinator.cpp includes.
namespace tcmt { struct TuiData; }
#include "etw/EtwMonitor.h"
#include "notifications/SystemEventMonitor.h"
#include "power/PowerMonitor.h"

// First-sample readiness bits, OR-ed by each collection loop. The TUI turns
// them into a startup LOADING percentage so the first seconds do not look
// like broken panels. One bit per coordinator loop.
enum : unsigned {
    kReadyCpu         = 1u << 0,
    kReadyMemory      = 1u << 1,
    kReadyDisk        = 1u << 2,
    kReadyNetwork     = 1u << 3,
    kReadyTemperature = 1u << 4,
    kReadyPower       = 1u << 5,
    kReadyLoopCount   = 6,
};

struct ModuleData {
    // CPU (500ms) — scalars use atomic
    // First-sample readiness (see kReady* above).
    std::atomic<unsigned> readyMask{0};
    std::atomic<double> cpuUsage{0.0};
    std::atomic<double> pCoreFreq{0.0};
    std::atomic<double> eCoreFreq{0.0};
    std::atomic<double> pCoreMaxFreq{0.0};
    std::atomic<double> eCoreMaxFreq{0.0};
    std::atomic<double> gpuFreq{0.0};
    std::atomic<double> gpuMaxFreq{0.0};
    // Memory (1s)
    std::atomic<uint64_t> totalMemory{0};
    std::atomic<uint64_t> usedMemory{0};
    std::atomic<uint64_t> availableMemory{0};
    std::atomic<uint64_t> compressedMemory{0};
    std::atomic<uint64_t> swapUsed{0};
    std::atomic<uint64_t> swapTotal{0};
    uint32_t ramSpeed = 0;
    std::string ramType;  // non-atomic — set once at startup
    int ramSlotsTotal = 0;   // SMBIOS physical memory array; 0 when soldered
    int ramSlotsUsed = 0;
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
    // mac = 硬件地址；currentMac = 当前生效的链路地址（macOS 的私有 Wi-Fi 地址会轮换）
    // 三个 uint64 要带默认值：文件里其它成员全都有，就这三个没有，
    // 静态分析（C26495）会一直报未初始化。运行时多半被 vector 的零初始化盖住了，
    // 但让"结构体自己保证有初值"比依赖调用方怎么构造它更省心。
    struct NetSlot {
        std::string name, ip, mac, currentMac, type;
        uint64_t speed = 0, dl = 0, ul = 0;
    };
    std::vector<NetSlot> adapters;
    // Temperature (2s)
    std::mutex tempMutex;
    std::vector<std::pair<std::string, double>> temperatures;
    double cpuTemperature{0.0};
    double cpuPcoreTemperature{0.0};
    double cpuEcoreTemperature{0.0};
    double gpuTemperature{0.0};
    // Power (2s)
    std::atomic<int> batteryPercent{-1};
    std::atomic<bool> acOnline{false};
    // 电池明细（PowerLoop）。拿不到的项留 0，界面按「这台机器没有这一项」处理，不估。
    // SystemInfo 侧的对应字段一直存在、schema 也一直在发，只是 Windows 上从来没人填。
    std::atomic<bool> batteryPresent{false};
    std::atomic<int>  batteryCycleCount{0};
    std::atomic<int>  batteryDesignCapacity{0};   // mWh
    std::atomic<int>  batteryFullCapacity{0};     // mWh（进 SystemInfo::batteryMaxCapacity）
    std::atomic<int>  batteryVoltage{0};          // mV
    std::atomic<double> batteryHealthPercent{0};  // 0-100
    std::atomic<double> cpuPower{0.0};
    std::atomic<double> gpuPower{0.0};
    std::atomic<double> anePower{0.0};
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

    // 采样基周期（毫秒），设置页「采样间隔」改的就是它。默认值与
    // `tcmt::kSampleIntervalDefaultMs`（src/tui/TuiApp.h）一致 —— 那边是设置页的
    // 取值范围，取值时已经夹过一次，这里只是给个初值。
    //
    // 六个采集循环各自按倍率从它派生周期（CPU 1×、内存与网络 2×、温度与电源 4×、磁盘 10×，
    // 默认 500ms 时正好等于改动前的写死值）。以前这个值只喂主循环的 sleep，
    // 采集周期全写死，所以改设置只看得出界面刷新变快，读数纹丝不动。
    std::atomic<int> sampleIntervalMs{500};
};

// 从一个采集循环的倍率算出该睡多久。floorMs 是下限：温度/电源/磁盘那几路单次采集不便宜，
// 间隔调小时不能让它们跟着一起变得很密。
inline int ScaledSampleIntervalMs(const ModuleData& data, int multiplier, int floorMs) {
    int base = data.sampleIntervalMs.load();
    if (base <= 0) base = 500;
    const int v = base * multiplier;
    return v < floorMs ? floorMs : v;
}

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
    // 设置页改了采样间隔就调这个（线程安全，六个采集循环下一轮生效）。
    // 传进来的值应当已经夹过取值范围，这里只做一次防御性下限。
    void SetSampleIntervalMs(int ms) {
        data_.sampleIntervalMs.store(ms > 0 ? ms : 500);
    }
    void Snapshot(SystemInfo& sysInfo, tcmt::TuiData& tuiData);
    // Per-core CPU frequencies, P cores first then E cores (order matches the
    // kernel's logical CPU numbering). Returns the number written; 0 when the
    // platform has no per-core source. `out` entries are -1 when unknown.
    int GetPerCoreFreqs(double* out, int max);
    static void SleepFor(tcmt::compat::StopToken st, int ms);
    // Bits of the collection loops that have completed at least one pass.
    unsigned ReadyMask() const { return data_.readyMask.load(std::memory_order_relaxed); }
    bool IsWifiDirty() { return data_.wifiDirty.exchange(false); }
    bool IsBtDirty()   { return data_.btDirty.exchange(false); }
    bool IsUsbDirty()  { return data_.usbDirty.exchange(false); }
private:
    ModuleData data_;
    std::atomic<bool> running_{false};
    tcmt::compat::JThread cpuThread_, memoryThread_, diskThread_, netThread_, tempThread_, powerThread_;
    tcmt::etw::EtwMonitor etwMonitor_;
    SystemEventMonitor sysEventMonitor_;
    PowerMonitor powerMonitor_;
    void DiskLoop(tcmt::compat::StopToken st);
    void NetworkLoop(tcmt::compat::StopToken st);
    void TemperatureLoop(tcmt::compat::StopToken st);
};
