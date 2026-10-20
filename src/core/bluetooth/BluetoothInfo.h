#pragma once

#include <cstddef>
#include <string>
#include <utility>
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
    // macOS (system_profiler): "Minor Type" — Headphones, Mouse, Car Stereo...
    std::string type;
    // Battery readings. One entry for single-battery devices ("Battery"),
    // three for AirPods-style devices ("Left"/"Right"/"Case"). Percent.
    std::vector<std::pair<std::string, int>> batteries;
};

// "Case 68% Left 23% Right 100%" for AirPods-style devices, "87%" for the
// single-battery ones. The labels are spelled out because "R100" read as
// gibberish without a legend. Empty when the device reports no battery.
inline std::string CompactBatteryText(const BluetoothDeviceData& d) {
    std::string out;
    for (const auto& [label, pct] : d.batteries) {
        if (!out.empty()) out += ' ';
        if (!label.empty()) {
            out += label;
            out += ' ';
        }
        out += std::to_string(pct);
        out += '%';
    }
    return out;
}

struct BluetoothData {
    BluetoothAdapterData adapter;
    std::vector<BluetoothDeviceData> devices;
};

// 把已连接设备名拼成一行，供 IPC 的 bluetooth.name 槽使用。
// 为什么是设备名而不是适配器名：适配器名是"这台机器自己叫什么"，对用户没意义；
// 设计稿要显示的是"当前连着哪些设备"。采集端一直取到了这份名单，之前只送了数量。
// 逐个拼、装不下就停 —— 不在 UTF-8 字符中间截断。两个平台的写入点共用这一份逻辑。
inline std::string JoinConnectedDeviceNames(const BluetoothData& data, size_t maxBytes) {
    std::string out;
    for (const auto& dev : data.devices) {
        if (!dev.connected || dev.name.empty()) continue;
        std::string next = out.empty() ? dev.name : (out + " · " + dev.name);
        if (next.size() > maxBytes) break;
        out = std::move(next);
    }
    return out;
}

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
