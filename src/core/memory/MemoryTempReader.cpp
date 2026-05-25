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

static const uint64_t I2C_SMBUS_READ      = 1;
static const uint64_t I2C_SMBUS_BYTE_DATA = 2;
static const uint64_t I2C_SMBUS_WORD_DATA = 3;

static double ReadSpdTemp(PawnIOWrapper& pa, const char* funcName,
                          uint8_t smbusAddr, uint8_t reg) {
    // Try WORD_DATA first (reads reg and reg+1 in one transaction)
    uint64_t inBuf[4] = { smbusAddr, I2C_SMBUS_READ, reg, I2C_SMBUS_WORD_DATA };
    uint64_t outBuf[1] = { 0 };
    uint32_t retSize = 0;

    if (!pa.Execute(funcName, inBuf, 4, outBuf, 1, &retSize))
        return -1.0;

    // SPD Hub temperature: register 0x31 is MR49 (TS_READ)
    // The WORD_DATA gives us whatever is at that SMBus command offset.
    // DDR5 SPD Hub returns temperature in Celsius (8-bit unsigned at MR49).
    uint8_t t = (uint8_t)(outBuf[0] & 0xFF);
    if (t > 10 && t < 120) return (double)t;
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

    // One-time probe: compare Hub MR vs NVM at same address
    static bool firstProbe = true;
    if (firstProbe && s_funcName.size()) {
        firstProbe = false;
        uint8_t probeAddr = SPD_ADDR_BEGIN;
        uint64_t pOut[1] = {0};
        // MR0 (MemReg=0, register 0) — should be vendor/device ID
        uint64_t pIn[4] = { probeAddr, I2C_SMBUS_READ, 0x00, I2C_SMBUS_BYTE_DATA };
        if (s_pa.Execute(s_funcName.c_str(), pIn, 4, pOut, 1, nullptr))
            Logger::Info("Hub MR0(reg0)=0x" + std::to_string((int)(pOut[0] & 0xFF)));
        // NVM byte 0 (MemReg=1, addr 0x80) — should be SPD byte 0
        pIn[2] = 0x80;
        if (s_pa.Execute(s_funcName.c_str(), pIn, 4, pOut, 1, nullptr))
            Logger::Info("NVM byte0(0x80)=0x" + std::to_string((int)(pOut[0] & 0xFF)));
        // MR49 (MemReg=0, reg 0x31) — temperature
        pIn[2] = 0x31;
        if (s_pa.Execute(s_funcName.c_str(), pIn, 4, pOut, 1, nullptr))
            Logger::Info("Hub MR49(reg0x31)=0x" + std::to_string((int)(pOut[0] & 0xFF)));
        // NVM byte 0x31 (MemReg=1, addr 0xB1)
        pIn[2] = 0xB1;
        if (s_pa.Execute(s_funcName.c_str(), pIn, 4, pOut, 1, nullptr))
            Logger::Info("NVM byte0x31(0xB1)=0x" + std::to_string((int)(pOut[0] & 0xFF)));
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
