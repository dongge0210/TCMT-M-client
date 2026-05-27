#ifdef TCMT_WINDOWS
#include "BoardTempReader.h"
#include "../memory/PawnIOWrapper.h"
#include "../Utils/Logger.h"

static const wchar_t* LPCIO_RES = L"LPC_IO";

// NCT6796D temperature registers at HW monitor base
static const uint8_t TEMP_REGS[][2] = {
    {0x73, 0x74}, // PECI_0: register, bank
    {0x75, 0x76}, // CPUTIN
    {0x77, 0x78}, // SYSTIN
    {0x79, 0x7A}, // AUXTIN0
    {0x7B, 0x7C}, // AUXTIN1
};
static const char* TEMP_NAMES[] = {"PECI_0", "CPUTIN", "SYSTIN", "AUXTIN0", "AUXTIN1"};

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
        if (!s_pa.LoadModuleFromMemory(data.data(), data.size(), "ioctl_pio_inb")) {
            Logger::Info("BoardTemp: LpcIO module failed to load"); return result;
        }

        // Select register port (0=0x2E, 1=0x4E)
        uint64_t slotIn[1] = { 0 };
        s_pa.Execute("ioctl_select_slot", slotIn, 1, nullptr, 0, nullptr);

        // Find Super I/O bars (LpcPort calls this first, without entering config mode)
        if (!s_pa.Execute("ioctl_find_bars", nullptr, 0, nullptr, 0, nullptr)) {
            Logger::Info("BoardTemp: find_bars failed");
            return result;
        }

        // Now enter config mode and read HW monitor base
        uint64_t enter87[2] = { 0x2E, 0x87 };
        s_pa.Execute("ioctl_pio_outb", enter87, 2, nullptr, 0, nullptr);
        s_pa.Execute("ioctl_pio_outb", enter87, 2, nullptr, 0, nullptr);

        // Read chip ID
        uint64_t chipIn[1] = { 0x20 }, chipOut[1] = {0};
        if (s_pa.Execute("ioctl_superio_inb", chipIn, 1, chipOut, 1, nullptr)) {
            Logger::Info(std::string("BoardTemp: chip=0x") + std::to_string((int)(chipOut[0] & 0xFF)));
        }

        // Select HW monitor (logical device 0x0B)
        uint64_t devSel[2] = { 0x07, 0x0B };
        s_pa.Execute("ioctl_superio_outb", devSel, 2, nullptr, 0, nullptr);

        // Read base address (reg 0x60-0x61)
        uint64_t r60[1] = { 0x60 }, r61[1] = { 0x61 };
        uint64_t hi[1] = {0}, lo[1] = {0};
        if (s_pa.Execute("ioctl_superio_inb", r60, 1, hi, 1, nullptr) &&
            s_pa.Execute("ioctl_superio_inb", r61, 1, lo, 1, nullptr)) {
            s_base = (uint16_t)(((hi[0] & 0xFF) << 8) | (lo[0] & 0xFF));
            Logger::Info(std::string("BoardTemp: base=0x") + std::to_string(s_base));
        }

        // Exit config mode
        uint64_t exitAA[2] = { 0x2E, 0xAA };
        s_pa.Execute("ioctl_pio_outb", exitAA, 2, nullptr, 0, nullptr);
    }

    if (s_base == 0) return result;

    // Read temperature from each register
    for (int i = 0; i < 5; i++) {
        uint8_t reg = TEMP_REGS[i][0];
        uint8_t bank = TEMP_REGS[i][1]; (void)bank; // bank switching not yet implemented

        // Try bank write: set bank bit
        uint64_t bankIn[2] = { 0x4E, 0x80 }; // bank register + bit to set
        s_pa.Execute("ioctl_superio_outb", bankIn, 2, nullptr, 0, nullptr);

        // Read temperature value using port IO
        uint64_t portIn[1] = { (uint64_t)(s_base + reg) };
        uint64_t portOut[1] = { 0 };
        if (!s_pa.Execute("ioctl_pio_inb", portIn, 1, portOut, 1, nullptr))
            continue;

        uint8_t raw = (uint8_t)(portOut[0] & 0xFF);
        if (raw > 0 && raw < 128) {
            BoardTemp bt;
            bt.name = TEMP_NAMES[i];
            bt.temperature = (double)(int8_t)raw; // signed 8-bit
            result.push_back(bt);
        }
    }

    return result;
}
#endif
