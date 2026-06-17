#pragma once

#include <string>
#include <vector>
#include <cstdint>

struct DisplayInfoData {
    std::string name;           // "Built-in Retina Display", etc.
    int width = 0;              // pixels (physical)
    int height = 0;             // pixels (physical)
    int refreshRate = 0;        // Hz (e.g. 120, 60)
    bool isHDR = false;         // HDR capable (EDR support)
    bool isBuiltin = false;     // Built-in display (laptop panel)
    double backingScale = 1.0;  // 2.0 for Retina/HiDPI
};

class DisplayInfo {
public:
    DisplayInfo() = default;
    ~DisplayInfo() = default;

    void Detect();
    const std::vector<DisplayInfoData>& GetDisplays() const;

private:
    std::vector<DisplayInfoData> displays_;
    void Clear();
};
