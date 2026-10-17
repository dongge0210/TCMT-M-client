// FanSpeed.h — Cross-platform fan speed monitoring
#pragma once
#include <string>
#include <vector>

struct FanSpeedEntry {
    std::string name;
    float rpm = 0;
};

class FanSpeed {
public:
    void Detect();
    const std::vector<FanSpeedEntry>& GetFans() const { return fans_; }

private:
    std::vector<FanSpeedEntry> fans_;
};
