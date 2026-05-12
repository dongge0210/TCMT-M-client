#pragma once

#include <string>
#include <vector>

struct BluetoothAdapterData {
    bool powerOn = false;       // Adapter enabled
    bool detected = false;      // Adapter hardware detected
    std::string address;        // "AA:BB:CC:DD:EE:FF"
    std::string name;           // Adapter name
};

struct BluetoothDeviceData {
    std::string name;           // Device name
    std::string address;        // "AA:BB:CC:DD:EE:FF"
    int rssi = 0;               // Signal strength in dBm (0 if unavailable)
    bool connected = false;
    bool remembered = false;    // Paired/remembered
};

struct BluetoothData {
    BluetoothAdapterData adapter;
    std::vector<BluetoothDeviceData> devices;
};

class BluetoothInfo {
public:
    BluetoothInfo() = default;
    ~BluetoothInfo() = default;

    void Detect();
    const BluetoothData& GetData() const;

private:
    BluetoothData data_;
    void Clear();
};
