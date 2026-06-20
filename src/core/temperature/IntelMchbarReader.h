#pragma once

#ifdef TCMT_WINDOWS
#include <string>
#include <vector>
#include <cstdint>

// IntelMCHBAR (Memory Controller Hub BAR) reader
// Maps the MCHBAR MMIO region via PCI 00:00.0 config registers 0x48/0x4C.
// Provides raw access to memory controller internal telemetry:
// PCODE mailbox, IMC thermal status, uncore frequency.
// Supported: Sandy Bridge through Nova Lake.
struct MchbarSensor {
    std::string name;
    double value;
};

class IntelMchbarReader {
public:
    static std::vector<MchbarSensor> ReadAll();
    static bool IsAvailable();
};
#endif
