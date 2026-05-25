#ifdef TCMT_WINDOWS
#include "MemoryTempReader.h"
#include "PawnIOWrapper.h"
#include "../Utils/Logger.h"

#include <algorithm>

// DDR5 SPD Hub temperature register: offset 0x31 (TS_READ), 10-bit signed, 0.25°C/LSB
static const uint8_t SPD_ADDR_BEGIN = 0x50;
static const uint8_t SPD_ADDR_END   = 0x53;
static const uint8_t SPD_TEMP_REG   = 0x31;

// Embedded .bin resource names (from resources.rc)
struct SmbusModule {
    const wchar_t* resName;   // RCDATA resource name
    const char*    funcName;  // PawnIO function name to call
};
static const SmbusModule SMBUS_MODULES[] = {
    { L"SMBUS_I801",    "ioctl_smbus_xfer" },
    { L"SMBUS_PIIX4",   "ioctl_smbus_xfer" },
    { L"SMBUS_NCT6793", "ioctl_smbus_xfer" },
    { L"SMBUS_SKYLAKE", "ioctl_smbus_xfer" },
};

// Load embedded .bin resource
static std::vector<uint8_t> LoadBinResource(const wchar_t* resName) {
    HMODULE hMod = GetModuleHandleW(nullptr);
    HRSRC hRes = FindResourceW(hMod, resName, RT_RCDATA);
    if (!hRes) return {};
    HGLOBAL hGlobal = LoadResource(hMod, hRes);
    if (!hGlobal) return {};
    DWORD size = SizeofResource(hMod, hRes);
    const uint8_t* data = static_cast<const uint8_t*>(LockResource(hGlobal));
    if (!data || size == 0 || size > 65536) return {};
    return std::vector<uint8_t>(data, data + size);
}

// PawnIO ioctl_smbus_xfer I2C_SMBUS constants
static const uint64_t I2C_SMBUS_READ       = 1;
static const uint64_t I2C_SMBUS_WORD_DATA  = 3;

static double ReadSpdTemp(PawnIOWrapper& pa, const char* funcName,
                          uint8_t smbusAddr, uint8_t reg) {
    // ioctl_smbus_xfer input: [addr, read_write, command, protocol]
    uint64_t inBuf[4] = { smbusAddr, I2C_SMBUS_READ, reg, I2C_SMBUS_WORD_DATA };
    uint64_t outBuf[1] = { 0 };
    uint32_t retSize = 0;

    if (!pa.Execute(funcName, inBuf, 4, outBuf, 1, &retSize)) {
        static int failLog = 0;
        if (failLog < 2) {
            Logger::Debug(std::string("PawnIO Execute failed for ") + funcName +
                          " addr=0x" + std::to_string(smbusAddr));
            failLog++;
        }
        return -1.0;
    }

    // DDR5 SPD: 10-bit signed, LSB = 0.25°C
    uint16_t raw = (uint16_t)(outBuf[0] & 0xFFFF);
    int16_t tempRaw = (int16_t)(raw & 0x03FF);
    if (tempRaw & 0x0200) tempRaw |= 0xFC00;
    double tempC = tempRaw * 0.25;
    return (tempC > -10.0 && tempC < 120.0) ? tempC : -1.0;
}

bool MemoryTempReader::IsAvailable() {
    return PawnIOWrapper::IsInstalled();
}

std::vector<DimmTempInfo> MemoryTempReader::ReadAll() {
    std::vector<DimmTempInfo> result;
    PawnIOWrapper pa;

    if (!pa.Open()) {
        Logger::Debug("MemoryTempReader: PawnIO device not available, skipping DIMM temps");
        return result;
    }

    // Load embedded SMBus modules
    struct LoadedMod { std::string func; bool ok; };
    std::vector<LoadedMod> mods;
    for (auto& m : SMBUS_MODULES) {
        auto data = LoadBinResource(m.resName);
        if (data.empty()) {
            Logger::Debug(std::string("PawnIO: resource not found: ") +
                          std::to_string((int)m.resName[0]));
            continue;
        }
        Logger::Info(std::string("PawnIO: loading module from resource, ") +
                     std::to_string(data.size()) + " bytes");
        if (pa.LoadModuleFromMemory(data.data(), data.size(), m.funcName)) {
            mods.push_back({ m.funcName, true });
            // Probe known function names on first load
            static bool probed = false;
            if (!probed) {
                probed = true;
                const char* probeFuncs[] = {
                    "ioctl_smbus_xfer", "ioctl_identity", "ioctl_clock_freq",
                    "main", "i801_init", "i2c_smbus_read_word_data",
                };
                Logger::Info("PawnIO: probing module functions:");
                for (auto fn : probeFuncs) {
                    uint64_t dummy[1] = {0};
                    uint32_t rs = 0;
                    bool ok = pa.Execute(fn, dummy, 0, dummy, 0, &rs);
                    Logger::Info(std::string("  ") + fn + (ok ? " -> OK" : " -> FAIL"));
                }
            }
            break;
        } else {
            Logger::Info("PawnIO: LoadModuleFromMemory failed");
        }
    }

    if (mods.empty()) {
        static bool loggedOnce = false;
        if (!loggedOnce) {
            Logger::Info("MemoryTempReader: no SMBus modules loaded (embedded resources missing?)");
            loggedOnce = true;
        }
        return result;
    }

    // Probe SPD addresses
    for (uint8_t addr = SPD_ADDR_BEGIN; addr <= SPD_ADDR_END; addr++) {
        for (auto& m : mods) {
            double t = ReadSpdTemp(pa, m.func.c_str(), addr, SPD_TEMP_REG);
            if (t >= 0.0) {
                DimmTempInfo info;
                info.temperature = t;
                char nameBuf[32];
                snprintf(nameBuf, sizeof(nameBuf), "DIMM %02X", addr);
                info.name = nameBuf;
                result.push_back(info);
                break;
            }
        }
    }

    static int logCount = 0;
    if (logCount < 3) {
        Logger::Info(std::string("MemoryTempReader: found ") +
                     std::to_string(result.size()) + " DIMMs");
        logCount++;
    }
    return result;
}
#endif
