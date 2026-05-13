#pragma once

#include <string>
#include <vector>
#include <cstdint>

#ifdef TCMT_WINDOWS
#include <winsock2.h>
#include <windows.h>
#endif

struct UsbDevice {
    std::string name;
    std::string manufacturer;
    uint16_t vid;
    uint16_t pid;
    std::string serialNumber;
    std::string protocolVersion;
    bool isSelfPowered;
    uint32_t maxPower;
};

class UsbInfo {
public:
    UsbInfo();
    ~UsbInfo();

    const std::vector<UsbDevice>& GetDevices() const;
    void Detect();

private:
    std::vector<UsbDevice> devices_;
};
