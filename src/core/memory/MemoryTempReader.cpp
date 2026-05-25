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

// PawnIO ioctl_smbus_xfer constants
static const uint64_t I2C_SMBUS_READ             = 1;
static const uint64_t I2C_SMBUS_I2C_BLOCK_DATA   = 8;

static double ReadSpdTemp(PawnIOWrapper& pa, const char* funcName,
                          uint8_t smbusAddr, uint8_t reg) {
    // DDR5 SPD uses I2C block read: send register address, read 2 bytes
    // ioctl_smbus_xfer input: [addr, read_write, command, protocol, byte_count]
    uint64_t inBuf[5] = { smbusAddr, I2C_SMBUS_READ, reg,
                          I2C_SMBUS_I2C_BLOCK_DATA, 2 };
    uint64_t outBuf[2] = { 0, 0 };
    uint32_t retSize = 0;

    if (!pa.Execute(funcName, inBuf, 5, outBuf, 2, &retSize)) {
        static int failLog = 0;
        if (failLog < 2) {
            Logger::Debug(std::string("PawnIO Execute failed addr=0x") +
                          std::to_string(smbusAddr));
            failLog++;
        }
        return -1.0;
    }

    uint8_t* bytes = (uint8_t*)outBuf;
    uint8_t count = bytes[0];
    if (count < 1) return -1.0;

    // DDR5 SPD Hub Temperature (MR49-MR50): 11-bit signed, 0.25°C LSB
    // MR49 (0x31) = bits [7:0], MR50 (0x32) = bits [10:8] + flags
    uint8_t tlo = count >= 1 ? bytes[1] : 0;
    uint8_t thi = count >= 2 ? bytes[2] : 0;
    int16_t tempRaw = ((thi & 0x07) << 8) | tlo;
    if (tempRaw & 0x0400) tempRaw |= 0xF800;  // sign-extend 11-bit
    double tempC = tempRaw * 0.25;
    return (tempC > -10.0 && tempC < 120.0) ? tempC : -1.0;
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

    // Probe SPD addresses
    for (uint8_t addr = SPD_ADDR_BEGIN; addr <= SPD_ADDR_END; addr++) {
        double t = ReadSpdTemp(s_pa, s_funcName.c_str(), addr, SPD_TEMP_REG);
        if (t >= 0.0) {
            DimmTempInfo info;
            info.temperature = t;
            char nameBuf[32];
            snprintf(nameBuf, sizeof(nameBuf), "DIMM %02X", addr);
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
