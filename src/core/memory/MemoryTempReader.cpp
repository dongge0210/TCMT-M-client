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

static const uint64_t I2C_SMBUS_WRITE     = 0;
static const uint64_t I2C_SMBUS_READ      = 1;
static const uint64_t I2C_SMBUS_BYTE_DATA = 2;

// SPD5118 Hub MR write: select MR register by writing to reg 0x00
static bool HubWriteMR(PawnIOWrapper& pa, const char* funcName,
                       uint8_t addr, uint8_t mr) {
    uint64_t wBuf[5] = { addr, I2C_SMBUS_WRITE, 0x00, I2C_SMBUS_BYTE_DATA, mr };
    uint64_t out[1] = {0};
    return pa.Execute(funcName, wBuf, 5, out, 0, nullptr);
}

static double ReadSpdTemp(PawnIOWrapper& pa, const char* funcName,
                          uint8_t smbusAddr, uint8_t mr) {
    // Step 1: Select MR by writing to reg 0 (MR select)
    if (!HubWriteMR(pa, funcName, smbusAddr, mr))
        return -1.0;

    // Step 2: Read the selected MR from reg 0
    uint64_t rdBuf[4] = { smbusAddr, I2C_SMBUS_READ, 0x00, I2C_SMBUS_BYTE_DATA };
    uint64_t rdOut[1] = { 0 };
    if (!pa.Execute(funcName, rdBuf, 4, rdOut, 1, nullptr))
        return -1.0;

    uint8_t t = (uint8_t)(rdOut[0] & 0xFF);
    if (t > 10 && t < 120) return (double)t;  // SPD Hub MR49: 1°C resolution
    return -1.0;
}

bool MemoryTempReader::IsAvailable() {
    return PawnIOWrapper::IsInstalled();
}

std::vector<DimmTempInfo> MemoryTempReader::ReadAll() {
    std::vector<DimmTempInfo> result;
    static PawnIOWrapper s_pa;
    static std::string s_funcName;
    static bool s_probed = false;

    if (!s_probed) {
        s_probed = true;
        if (!s_pa.Open()) {
            Logger::Debug("MemoryTempReader: PawnIO device not available");
            return result;
        }
        for (auto& m : SMBUS_MODULES) {
            auto data = LoadBinResource(m.resName);
            if (data.empty()) continue;
            Logger::Info(std::string("PawnIO: loading SMBus module, ") +
                         std::to_string(data.size()) + " bytes");
            if (s_pa.LoadModuleFromMemory(data.data(), data.size(), m.funcName)) {
                s_funcName = m.funcName;
                Logger::Info("PawnIO: SMBus module loaded");
                break;
            }
        }
        if (s_funcName.empty()) {
            Logger::Info("PawnIO: no SMBus module loaded");
            return result;
        }
    }

    int dimmIdx = 0;
    for (uint8_t addr = SPD_ADDR_BEGIN; addr <= SPD_ADDR_END; addr++) {
        double t = ReadSpdTemp(s_pa, s_funcName.c_str(), addr, SPD_TEMP_REG);
        if (t >= 0.0) {
            DimmTempInfo info;
            info.temperature = t;
            char nameBuf[32];
            snprintf(nameBuf, sizeof(nameBuf), "DIMM %d", ++dimmIdx);
            info.name = nameBuf;
            result.push_back(info);
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
