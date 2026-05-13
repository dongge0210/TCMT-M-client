#pragma once
#include <string>

struct BoardInfo {
    std::string manufacturer;
    std::string product;
    std::string serialNumber;
    std::string version;
    std::string biosVendor;
    std::string biosVersion;
    std::string biosDate;
    std::string systemUuid;
    std::string chassisType;

    void Detect();
};
