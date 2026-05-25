#ifdef TCMT_WINDOWS
#include "MemoryTempReader.h"
#include "PawnIOWrapper.h"
#include "../Utils/Logger.h"

#include <algorithm>

// DDR5 SPD Hub temperature register: offset 0x31 (TS_READ), 10-bit signed, 0.25°C/LSB
// SPD address range: 0x50-0x57 (one per DIMM slot)
static const uint8_t SPD_ADDR_BEGIN = 0x50;
static const uint8_t SPD_ADDR_END   = 0x53; // Probe first 4 slots
static const uint8_t SPD_TEMP_REG   = 0x31;
static const uint8_t SPD_TEMP_LEN   = 2;    // 2 bytes (10-bit value)

// SMBus module filenames (loaded from PawnIO executable directory or bundled resources)
static const wchar_t* SMBUS_MODULES[] = {
    L"SmbusI801.bin",
    L"SmbusPIIX4.bin",
};

// Try to find a .bin file in common locations
static std::vector<uint8_t> LoadBinFile(const wchar_t* name) {
    const wchar_t* searchPaths[] = {
        L"Resources/PawnIo/",           // LHM submodule relative
        L"src/third_party/LibreHardwareMonitor/LibreHardwareMonitorLib/Resources/PawnIo/",
        L"",                            // current directory
    };

    for (auto dir : searchPaths) {
        std::wstring path(dir);
        path += name;

        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD sz = GetFileSize(h, nullptr);
            if (sz > 0 && sz < 65536) {
                std::vector<uint8_t> buf(sz);
                DWORD rd;
                if (ReadFile(h, buf.data(), sz, &rd, nullptr) && rd == sz) {
                    CloseHandle(h);
                    return buf;
                }
            }
            CloseHandle(h);
        }
    }
    return {};
}

static double ReadSpdTemp(PawnIOWrapper& pa, const char* moduleFunc,
                          uint8_t smbusAddr, uint8_t reg) {
    // SMBus read word: input = { smbusAddr << 8 | reg }
    // The .bin module's SMBus read function signature varies by module
    // i2c_smbus_read_word_data(addr, reg) → returns 16-bit value
    uint64_t inBuf[2] = { smbusAddr, reg };
    uint64_t outBuf[1] = { 0 };
    uint32_t retSize = 0;

    if (!pa.Execute(moduleFunc, inBuf, 2, outBuf, 1, &retSize))
        return -1.0;

    // Decode DDR5 SPD temperature: 10-bit signed value, LSB = 0.25°C
    uint16_t raw = (uint16_t)(outBuf[0] & 0xFFFF);
    // DDR5: bits [9:0] are temperature * 4, sign-extend from bit 9
    int16_t tempRaw = (int16_t)(raw & 0x03FF);
    if (tempRaw & 0x0200) tempRaw |= 0xFC00; // sign-extend 10-bit
    double tempC = tempRaw * 0.25;

    if (tempC < -10.0 || tempC > 120.0) return -1.0;
    return tempC;
}

bool MemoryTempReader::IsAvailable() {
    return PawnIOWrapper::IsInstalled();
}

std::vector<DimmTempInfo> MemoryTempReader::ReadAll() {
    std::vector<DimmTempInfo> result;
    PawnIOWrapper pa;

    if (!pa.Open()) return result;

    // Load all SMBus modules
    struct LoadedMod { std::string func; bool ok; };
    std::vector<LoadedMod> mods;
    for (auto modPath : SMBUS_MODULES) {
        auto data = LoadBinFile(modPath);
        if (data.empty()) continue;

        // The function name for SMBus read_word embedded in the .bin
        // PawnIO modules use standardized function names
        std::string modFunc = "i2c_smbus_read_word_data";
        if (pa.LoadModuleFromMemory(data.data(), data.size(), modFunc.c_str())) {
            mods.push_back({ modFunc, true });
            break; // One SMBus module is enough
        }
    }

    if (mods.empty()) {
        Logger::Debug("MemoryTempReader: no SMBus modules loaded");
        return result;
    }

    // Probe each SPD address for temperature
    for (uint8_t addr = SPD_ADDR_BEGIN; addr <= SPD_ADDR_END; addr++) {
        for (auto& m : mods) {
            if (!m.ok) continue;
            double t = ReadSpdTemp(pa, m.func.c_str(), addr, SPD_TEMP_REG);
            if (t >= 0.0) {
                DimmTempInfo info;
                info.temperature = t;
                char nameBuf[32];
                snprintf(nameBuf, sizeof(nameBuf), "DIMM %02X", addr);
                info.name = nameBuf;
                result.push_back(info);
                break; // found temp for this slot, try next address
            }
        }
    }

    Logger::Info(std::string("MemoryTempReader: found ") +
                 std::to_string(result.size()) + " DIMMs with temperature sensors");
    return result;
}
#endif // TCMT_WINDOWS
