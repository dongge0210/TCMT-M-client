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
static const uint64_t I2C_SMBUS_WORD_DATA = 3;

static double ReadSpdTemp(PawnIOWrapper& pa, const char* funcName,
                          uint8_t smbusAddr, uint8_t reg) {
    // DDR5 SPD Hub MR49:MR50 temperature (JEDEC JESD300-5B)
    // WORD_DATA at register 0x31 reads both MR49 (low) and MR50 (high)
    uint64_t inBuf[4] = { smbusAddr, I2C_SMBUS_READ, reg, I2C_SMBUS_WORD_DATA };
    uint64_t outBuf[1] = { 0 };
    if (!pa.Execute(funcName, inBuf, 4, outBuf, 1, nullptr))
        return -1.0;

    // Decode: raw16 >> 2, sign-extend 10-bit, * 0.25°C
    uint16_t raw = (uint16_t)(outBuf[0] & 0xFFFF);
    int16_t val = (int16_t)(raw >> 2);
    if (val & 0x0200) val |= 0xFC00;  // sign-extend from bit 9 (was bit 11 pre-shift)
    double tempC = val * 0.25;
    if (tempC > 0.0 && tempC < 120.0) return tempC;
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
            if (s_pa.LoadModuleFromMemory(data.data(), data.size(), m.funcName)) {
                s_funcName = m.funcName;
                Logger::Debug("PawnIO: SMBus module loaded");
                break;
            }
        }
        if (s_funcName.empty()) {
            Logger::Info("PawnIO: no SMBus module loaded");
            return result;
        }
    }

    // One-time: enable SPD Hub temperature sensors
    static bool sensorsEnabled = false;
    if (!sensorsEnabled && !s_funcName.empty()) {
        sensorsEnabled = true;
        for (uint8_t addr = SPD_ADDR_BEGIN; addr <= SPD_ADDR_END; addr++) {
            // Read MR26 (0x1A) — temperature config
            uint64_t cfgIn[4] = { addr, I2C_SMBUS_READ, 0x1A, 2 }; // BYTE_DATA
            uint64_t cfgOut[1] = {0};
            if (s_pa.Execute(s_funcName.c_str(), cfgIn, 4, cfgOut, 1, nullptr)) {
                uint8_t cfg = (uint8_t)(cfgOut[0] & 0xFF);
                if (cfg & 0x01) {
                    // Temperature sensor disabled — enable it
                    cfg &= ~0x01;
                    Logger::Info(std::string("PawnIO: enabling temp sensor on DIMM at 0x") +
                                 std::to_string(addr));
                    // MR26 write = BYTE_DATA write: [addr, WRITE=0, reg=0x1A, BYTE_DATA=2, value]
                    uint64_t wrBuf[5] = { addr, 0, 0x1A, 2, cfg };
                    s_pa.Execute(s_funcName.c_str(), wrBuf, 5, nullptr, 0, nullptr);
                }
            }
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

    // Probe SMBus 0x48-0x4F for motherboard temp sensors (LM75 family)
    for (uint8_t addr = 0x48; addr <= 0x4F; addr++) {
        uint64_t probeIn[4] = { addr, I2C_SMBUS_READ, 0x00, I2C_SMBUS_WORD_DATA };
        uint64_t probeOut[1] = {0};
        if (s_pa.Execute(s_funcName.c_str(), probeIn, 4, probeOut, 1, nullptr)) {
            uint16_t raw = (uint16_t)(probeOut[0] & 0xFFFF);
            uint16_t be = ((raw & 0xFF) << 8) | (raw >> 8); // SMBus LE → BE
            int16_t tRaw = (int16_t)(be >> 5);
            if (tRaw & 0x0400) tRaw |= 0xF800;
            double tC = tRaw * 0.125;
            if (tC > 0 && tC < 120) {
                char nameBuf[32];
                snprintf(nameBuf, sizeof(nameBuf), "SMBus 0x%02X", addr);
                result.push_back({nameBuf, tC});
            }
        }
    }

    return result;
}
#endif
