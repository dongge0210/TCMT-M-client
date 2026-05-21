#pragma once
#include <string>
#include <vector>
#include <utility>

class TemperatureWrapper {
public:
    static void Initialize();
    static void Cleanup();
    static std::vector<std::pair<std::string, double> > GetTemperatures();
    static bool IsInitialized();

private:
    static bool initialized;
};

// powermetrics frequency/power data (Apple Silicon — no ETW/PDH)
double GetPmPCoreFreq();
double GetPmECoreFreq();
double GetPmGpuFreq();
double GetPmCpuPower();
double GetPmGpuPower();
double GetPmAnePower();
