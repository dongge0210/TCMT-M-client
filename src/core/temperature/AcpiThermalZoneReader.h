#pragma once

#ifdef TCMT_WINDOWS
#include <string>
#include <vector>

struct AcpiThermalZone {
    std::string name;
    double temperature;   // Celsius
};

// ACPI 热区温度（WMI MSAcpi_ThermalZoneTemperature）。
//
// 这是 Windows 上少数不需要驱动就能拿到的温度源：不依赖 PawnIO，也不走 SMART。
// 代价是覆盖很差 —— 很多机器只报一两个区，台式机常常一个都不报，非管理员还可能直接
// 拿到 ACCESS_DENIED。拿不到就返回空，**不占位造数**（界面宁愿少一行，也不要一个假的 0）。
class AcpiThermalZoneReader {
public:
    static std::vector<AcpiThermalZone> ReadAll();
};
#endif
