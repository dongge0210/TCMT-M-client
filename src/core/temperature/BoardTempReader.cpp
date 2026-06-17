#ifdef TCMT_WINDOWS
#include "BoardTempReader.h"
#include "../memory/PawnIOWrapper.h"
#include "../Utils/Logger.h"

static const wchar_t* LPCIO_RES = L"LPC_IO";

// NCT6798D temperature source data (full LHM Group D set)
// addr: 16-bit register address (upper byte = bank, lower byte = register)
// halfAddr: 16-bit half-register address (0 if no fractional)
// halfBit: bit position for fractional (0 if no fractional)
// Temperature = ((signed_byte << 1) | halfBit) * 0.5°C
static const struct {
    const char* name;
    uint16_t addr;
    uint16_t halfAddr;
    int halfBit;
} TEMP_SOURCES[] = {
    {"MB_PECI_0",              0x0073, 0x0074, 7},
    {"MB_CPUTIN",              0x0075, 0x0076, 7},
    {"MB_SYSTIN",              0x0077, 0x0078, 7},
    {"MB_AUXTIN0",             0x0079, 0x007A, 7},
    {"MB_AUXTIN1",             0x007B, 0x007C, 7},
    {"MB_AUXTIN2",             0x007D, 0x007E, 7},
    {"MB_AUXTIN3",             0x04A0, 0x049E, 6},
    {"MB_AUXTIN4",             0x0027, 0,      0},
    {"MB_TSENSOR",             0x04A2, 0x04A1, 7},
    {"MB_SMBUSMASTER0",        0x0150, 0x0151, 7},
    {"MB_SMBUSMASTER1",        0x0670, 0,      0},
    {"MB_PECI_1",              0x0672, 0,      0},
    {"MB_PCH_CHIP_CPU_MAX",    0x0674, 0,      0},
    {"MB_PCH_CHIP",            0x0676, 0,      0},
    {"MB_PCH_CPU",             0x0678, 0,      0},
    {"MB_PCH_MCH",             0x067A, 0,      0},
    {"MB_AGENT0_DIMM0",        0x0405, 0,      0},
    {"MB_AGENT0_DIMM1",        0x0406, 0,      0},
    {"MB_AGENT1_DIMM0",        0x0407, 0,      0},
    {"MB_AGENT1_DIMM1",        0x0408, 0,      0},
    {"MB_BYTE_TEMP0",          0x0419, 0,      0},
    {"MB_BYTE_TEMP1",          0x041A, 0,      0},
};

static std::vector<uint8_t> LoadResource(const wchar_t* name) {
    HMODULE hMod = GetModuleHandleW(nullptr);
    HRSRC hRes = FindResourceW(hMod, name, RT_RCDATA);
    if (!hRes) return {};
    HGLOBAL hGlobal = LoadResource(hMod, hRes);
    if (!hGlobal) return {};
    DWORD size = SizeofResource(hMod, hRes);
    const uint8_t* data = static_cast<const uint8_t*>(LockResource(hGlobal));
    if (!data || size == 0 || size > 65536) return {};
    return std::vector<uint8_t>(data, data + size);
}

bool BoardTempReader::IsAvailable() {
    return PawnIOWrapper::IsInstalled();
}

// Read a byte from HW monitor register via banked access (NCT6798D protocol)
// addr: 16-bit address (upper byte = bank, lower byte = register)
// Writes bank select (0x4E) to port+5, bank value to port+6
// Writes register to port+5, reads value from port+6
static uint8_t HwMonReadByte(PawnIOWrapper& pa, uint16_t port, uint16_t addr) {
    uint8_t bank = (uint8_t)(addr >> 8);
    uint8_t reg = (uint8_t)(addr & 0xFF);
    uint64_t args[2];
    uint64_t out[1] = {0};

    if (bank != 0) {
        args[0] = port + 5; args[1] = 0x4E;
        pa.Execute("ioctl_pio_outb", args, 2, nullptr, 0, nullptr);
        args[0] = port + 6; args[1] = bank;
        pa.Execute("ioctl_pio_outb", args, 2, nullptr, 0, nullptr);
    }

    args[0] = port + 5; args[1] = reg;
    pa.Execute("ioctl_pio_outb", args, 2, nullptr, 0, nullptr);

    args[0] = port + 6;
    if (!pa.Execute("ioctl_pio_inb", args, 1, out, 1, nullptr))
        return 0;
    return (uint8_t)(out[0] & 0xFF);
}

std::vector<BoardTemp> BoardTempReader::ReadAll() {
    std::vector<BoardTemp> result;
    static PawnIOWrapper s_pa;
    static uint16_t s_base = 0;
    static bool s_ready = false;

    if (!s_ready) {
        s_ready = true;
        if (!s_pa.Open()) return result;
        auto data = LoadResource(LPCIO_RES);
        if (data.empty()) { Logger::Info("BoardTemp: LPCIO resource not found"); return result; }
        if (!s_pa.LoadModuleFromMemory(data.data(), data.size(), "LpcIO")) {
            Logger::Info("BoardTemp: LpcIO module failed to load"); return result;
        }

        // Step 1: SelectSlot — matches LpcPort constructor
        uint64_t slotIn[1] = { 0 }; // 0=0x2E, 1=0x4E
        s_pa.Execute("ioctl_select_slot", slotIn, 1, nullptr, 0, nullptr);

        // Step 2: Enter config mode (WinbondNuvotonFintekEnter)
        uint64_t enter87[2] = { 0x2E, 0x87 };
        s_pa.Execute("ioctl_pio_outb", enter87, 2, nullptr, 0, nullptr);
        s_pa.Execute("ioctl_pio_outb", enter87, 2, nullptr, 0, nullptr);

        // Step 3: Read chip ID (register 0x20) and revision (register 0x21)
        uint64_t regIn[1], regOut[1] = {0};
        regIn[0] = 0x20;
        s_pa.Execute("ioctl_superio_inb", regIn, 1, regOut, 1, nullptr);
        uint8_t chipId = (uint8_t)(regOut[0] & 0xFF);
        regIn[0] = 0x21; regOut[0] = 0;
        s_pa.Execute("ioctl_superio_inb", regIn, 1, regOut, 1, nullptr);
        uint8_t chipRev = (uint8_t)(regOut[0] & 0xFF);
        Logger::Info(std::string("BoardTemp: chip=0x") + std::to_string((int)chipId) +
                     " rev=0x" + std::to_string((int)chipRev));

        // Step 4: FindBars (must be in config mode)
        if (!s_pa.Execute("ioctl_find_bars", nullptr, 0, nullptr, 0, nullptr)) {
            Logger::Info("BoardTemp: find_bars failed (chip may not be supported)");
            uint64_t exitAA[2] = { 0x2E, 0xAA };
            s_pa.Execute("ioctl_pio_outb", exitAA, 2, nullptr, 0, nullptr);
            return result;
        }
        Logger::Info("BoardTemp: find_bars succeeded");

        // Step 5: Select HW monitor logical device (0x0B)
        uint64_t devSel[2] = { 0x07, 0x0B };
        s_pa.Execute("ioctl_superio_outb", devSel, 2, nullptr, 0, nullptr);

        // Step 6: Read base address via ReadWord (register 0x60)
        uint64_t wOut[1] = {0};
        regIn[0] = 0x60;
        if (s_pa.Execute("ioctl_superio_inw", regIn, 1, wOut, 1, nullptr)) {
            s_base = (uint16_t)(wOut[0] & 0xFFFF);
            Logger::Info(std::string("BoardTemp: base=0x") + std::to_string(s_base));
        }

        // Step 7: NuvotonDisableIOSpaceLock (register 0x28) for NCT6798D
        regIn[0] = 0x28; regOut[0] = 0;
        if (s_pa.Execute("ioctl_superio_inb", regIn, 1, regOut, 1, nullptr)) {
            uint8_t options = (uint8_t)(regOut[0] & 0xFF);
            if (options & 0x10) {
                uint64_t unlockArgs[2] = { 0x28, (uint64_t)(options & ~0x10) };
                s_pa.Execute("ioctl_superio_outb", unlockArgs, 2, nullptr, 0, nullptr);
                Logger::Info("BoardTemp: disabled IO space lock");
            }
        }

        // Step 8: Exit config mode
        uint64_t exitAA[2] = { 0x2E, 0xAA };
        s_pa.Execute("ioctl_pio_outb", exitAA, 2, nullptr, 0, nullptr);
    }

    if (s_base == 0 || s_base < 0x100) return result;

    // Read temperature sources using NCT6798D protocol
    int count = sizeof(TEMP_SOURCES) / sizeof(TEMP_SOURCES[0]);
    for (int i = 0; i < count; i++) {
        uint8_t raw = HwMonReadByte(s_pa, s_base, TEMP_SOURCES[i].addr);
        if (raw == 0xFF || raw == 0x00) continue;

        // NCT6798D formula: (signed_byte << 1) | halfBit, then * 0.5
        int value = (int8_t)raw << 1;
        if (TEMP_SOURCES[i].halfBit > 0) {
            uint8_t halfRaw = HwMonReadByte(s_pa, s_base, TEMP_SOURCES[i].halfAddr);
            value |= (halfRaw >> TEMP_SOURCES[i].halfBit) & 0x1;
        }
        double temp = value * 0.5;
        if (temp <= -55.0 || temp >= 125.0) continue;

        BoardTemp bt;
        bt.name = TEMP_SOURCES[i].name;
        bt.temperature = temp;
        result.push_back(bt);
    }

    return result;
}
#endif
