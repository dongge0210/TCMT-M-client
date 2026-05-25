#ifdef TCMT_WINDOWS
#include "MemoryTempReader.h"
#include "PawnIOWrapper.h"
#include "../Utils/Logger.h"

#include <algorithm>
#include <sstream>
#include <iomanip>

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

    // outBuf[0] = byte count + packed data for first 8 bytes
    // byte 0 = count, byte 1..N = data
    uint8_t* bytes = (uint8_t*)outBuf;
    uint8_t count = bytes[0];
    static int dbgLog = 0;
    if (dbgLog < 4) {
        std::ostringstream oss;
        oss << "PawnIO SMBus addr=0x" << std::hex << (int)smbusAddr
            << " reg=0x" << std::hex << (int)reg
            << " count=" << std::dec << (int)count
            << " data=" << std::hex;
        for (int i = 1; i <= count && i <= 8; i++)
            oss << " " << (int)bytes[i];
        Logger::Info(oss.str());
        dbgLog++;
    }

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
            Logger::Info(std::string("PawnIO: SMBus module loaded, ") +
                         std::to_string(data.size()) + " bytes");
            break;
        } else {
            Logger::Debug("PawnIO: LoadModuleFromMemory failed");
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
