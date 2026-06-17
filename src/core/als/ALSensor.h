// ALSensor.h — macOS Ambient Light Sensor via IOKit AppleSPUVD6286
#pragma once

struct ALSData {
    double lux = 0.0;          // Current ambient light in lux
    bool valid = false;        // true if sensor was read successfully
};

class ALSensor {
public:
    void Detect();
    const ALSData& GetData() const { return data_; }
private:
    ALSData data_;
};
