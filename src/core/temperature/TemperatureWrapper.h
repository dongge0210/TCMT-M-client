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
