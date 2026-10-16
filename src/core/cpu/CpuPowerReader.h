#pragma once

#ifdef TCMT_WINDOWS
// Intel RAPL 包功耗。
//
// 用户态拿不到 VRM 的电压和电流，所以 CPU 功耗没法用 V×I 算；能用的只有 RAPL 的能量计数器：
// 读 MSR_PKG_ENERGY_STATUS（0x611），两次采样取能量差再除以时间。单位由 MSR_RAPL_POWER_UNIT
// （0x606）的 ESU 位段决定。
//
// 走 PawnIO 的 MSR 通道（和 CpuTempReader 同一种做法），没装 PawnIO 就返回 -1。
// AMD 不走这里 —— 那边是 RyzenSmuReader 读 SMU 表。
class CpuPowerReader {
public:
    // 瓦特。首次调用（还没有基线）或取不到时返回 -1，调用方按「没有」处理，不要当 0 用。
    static double ReadPackagePowerW();
    static bool IsAvailable();
};
#endif
