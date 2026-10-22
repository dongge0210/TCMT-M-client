#ifdef TCMT_WINDOWS
#include "BoardTempReader.h"
#include "../memory/PawnIOWrapper.h"
#include "../Utils/Logger.h"

static const wchar_t* LPCIO_RES = L"LPC_IO";

// NCT6798D temperature source data (sourced from PawnIO via LpcIO kernel module)
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
    // Names are what the Temps panel prints. "Aux N" is deliberate: the board
    // vendor decides what those pins are wired to (VRM, chipset, a header),
    // and guessing "VRM" from the register number mislabeles half the boards.
    {"CPU PECI",               0x0073, 0x0074, 7},
    {"CPU Socket",             0x0075, 0x0076, 7},
    {"System",                 0x0077, 0x0078, 7},
    {"Aux 0",                  0x0079, 0x007A, 7},
    {"Aux 1",                  0x007B, 0x007C, 7},
    {"Aux 2",                  0x007D, 0x007E, 7},
    {"Aux 3",                  0x04A0, 0x049E, 6},
    {"Aux 4",                  0x0027, 0,      0},
    {"Board",                  0x04A2, 0x04A1, 7},
    {"SMBus 0",                0x0150, 0x0151, 7},
    {"SMBus 1",                0x0670, 0,      0},
    {"PECI 1",                 0x0672, 0,      0},
    {"PCH Max",                0x0674, 0,      0},
    {"PCH",                    0x0676, 0,      0},
    {"PCH CPU",                0x0678, 0,      0},
    {"PCH MCH",                0x067A, 0,      0},
    {"DIMM A1",                0x0405, 0,      0},
    {"DIMM A2",                0x0406, 0,      0},
    {"DIMM B1",                0x0407, 0,      0},
    {"DIMM B2",                0x0408, 0,      0},
    {"Temp 0",                 0x0419, 0,      0},
    {"Temp 1",                 0x041A, 0,      0},
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

    // The bank select is sticky, so it has to be re-issued for bank 0 as well.
    // Skipping it made the bank-0 reads that follow a bank-4 read land on the
    // wrong register: the first call (chip still on bank 0 after config mode)
    // looked fine, every later call started on bank 4 and lost those sensors.
    args[0] = port + 5; args[1] = 0x4E;
    pa.Execute("ioctl_pio_outb", args, 2, nullptr, 0, nullptr);
    args[0] = port + 6; args[1] = bank;
    pa.Execute("ioctl_pio_outb", args, 2, nullptr, 0, nullptr);

    args[0] = port + 5; args[1] = reg;
    pa.Execute("ioctl_pio_outb", args, 2, nullptr, 0, nullptr);

    args[0] = port + 6;
    if (!pa.Execute("ioctl_pio_inb", args, 1, out, 1, nullptr))
        return 0;
    return (uint8_t)(out[0] & 0xFF);
}

// 0xFFFF is the Super I/O's "unassigned" erase value for a base-address
// register, not an address; 0 and anything below 0x100 would collide with the
// ISA/LPC legacy ports.
static bool ValidIoBase(uint16_t b) { return b >= 0x100 && b != 0xFFFF; }

// The Super I/O config ports (0x2E/0x2F) are shared with every other hardware
// monitor on the machine. OpenHardwareMonitor established the name below and
// HWiNFO, LibreHardwareMonitor and nwinfo all honour it; without it the two
// sides interleave config-index writes and the reads come back as 0xFFFF.
static HANDLE IsaIoMutex() {
    static HANDLE h = [] {
        HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, L"Global\\Access_ISABUS.HTP.Method");
        if (!m) m = CreateMutexW(nullptr, FALSE, L"Global\\Access_ISABUS.HTP.Method");
        return m;
    }();
    return h;
}

struct IsaIoLock {
    HANDLE h = nullptr;
    bool held = false;
    IsaIoLock() {
        h = IsaIoMutex();
        held = h && WaitForSingleObject(h, 2000) == WAIT_OBJECT_0;
    }
    ~IsaIoLock() { if (held) ReleaseMutex(h); }
    IsaIoLock(const IsaIoLock&) = delete;
    IsaIoLock& operator=(const IsaIoLock&) = delete;
};

std::vector<BoardTemp> BoardTempReader::ReadAll() {
    std::vector<BoardTemp> result;
    static PawnIOWrapper s_pa;
    static uint16_t s_base = 0;
    static bool s_ready = false;

    // Held for the whole detection + read sequence: the config index port is
    // a single hardware register shared by every monitoring agent.
    IsaIoLock isaLock;
    static uint8_t s_cr28 = 0xFF;
    if (!isaLock.held) {
        static bool loggedNoMutex = false;
        if (!loggedNoMutex) {
            loggedNoMutex = true;
            Logger::Debug("BoardTemp: ISABUS mutex unavailable, accessing unserialized");
        }
    }

    // Full Super I/O detection. Can be re-run: the chip's IO-space lock and
    // base address are not guaranteed to survive forever (another user of the
    // same chip, a suspend/resume, a driver reload), and after that every
    // register read returns 0xFF and the sensor list silently empties.
    static bool firstDetect = true;
    auto detect = [&]() -> bool {
        const bool verbose = firstDetect;
        firstDetect = false;
        if (!s_pa.Open()) return false;
        auto data = LoadResource(LPCIO_RES);
        if (data.empty()) { Logger::Debug("BoardTemp: LPCIO resource not found"); return false; }
        if (!s_pa.LoadModuleFromMemory(data.data(), data.size(), "LpcIO")) {
            Logger::Debug("BoardTemp: LpcIO module failed to load"); return false;
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
        {
            char info[64];
            snprintf(info, sizeof(info), "BoardTemp: chip=0x%02X rev=0x%02X", chipId, chipRev);
            if (verbose) Logger::Info(info);
            else Logger::Debug(info);
            // The register table below is NCT6798D's; any other Nuvoton part
            // may answer at the same ports with a different map.
            if (chipId != 0xD4)
                Logger::Debug("BoardTemp: unexpected Super I/O ID, values may be wrong");
        }

        // Step 4: FindBars (must be in config mode)
        if (!s_pa.Execute("ioctl_find_bars", nullptr, 0, nullptr, 0, nullptr)) {
            if (verbose) Logger::Info("BoardTemp: find_bars failed (chip may not be supported)");
            else Logger::Debug("BoardTemp: find_bars failed");
            uint64_t exitAA[2] = { 0x2E, 0xAA };
            s_pa.Execute("ioctl_pio_outb", exitAA, 2, nullptr, 0, nullptr);
            return false;
        }
        if (verbose) Logger::Info("BoardTemp: find_bars succeeded");
        else Logger::Debug("BoardTemp: find_bars succeeded");

        // Step 5: Select HW monitor logical device (0x0B)
        uint64_t devSel[2] = { 0x07, 0x0B };
        s_pa.Execute("ioctl_superio_outb", devSel, 2, nullptr, 0, nullptr);

        // Step 6: Read base address via ReadWord (register 0x60).
        // The value comes back 0xFFFF when the logical-device selection did not
        // stick, which happens when another monitoring agent (HWiNFO, a vendor
        // control centre) is walking the same config index port at the same
        // time. Re-select the device and read again before believing it.
        uint16_t base = 0;
        for (int attempt = 0; attempt < 5; ++attempt) {
            if (attempt > 0) Sleep(5);
            s_pa.Execute("ioctl_superio_outb", devSel, 2, nullptr, 0, nullptr);
            // Read the selector back: when another agent moved the index port
            // between the write and the read, the selection silently did not
            // stick and 0x60 returns the erase value.
            regIn[0] = 0x07; regOut[0] = 0;
            if (!s_pa.Execute("ioctl_superio_inb", regIn, 1, regOut, 1, nullptr) ||
                (uint8_t)(regOut[0] & 0xFF) != 0x0B) {
                continue;
            }
            uint64_t wOut[1] = {0};
            regIn[0] = 0x60;
            if (s_pa.Execute("ioctl_superio_inw", regIn, 1, wOut, 1, nullptr)) {
                base = (uint16_t)(wOut[0] & 0xFFFF);
                if (ValidIoBase(base)) break;
            }
        }
        s_base = base;
        {
            char info[48];
            snprintf(info, sizeof(info), "BoardTemp: base=0x%04X", s_base);
            // A valid base on a later attempt means the chip came back; that
            // is worth an INFO line even when this is not the first detect.
            if (verbose || ValidIoBase(s_base)) Logger::Info(info);
            else Logger::Debug(info);
        }

        // Step 7: NuvotonDisableIOSpaceLock (register 0x28) for NCT6798D
        regIn[0] = 0x28; regOut[0] = 0;
        if (s_pa.Execute("ioctl_superio_inb", regIn, 1, regOut, 1, nullptr)) {
            uint8_t options = (uint8_t)(regOut[0] & 0xFF);
            s_cr28 = options;
            if (options & 0x10) {
                uint64_t unlockArgs[2] = { 0x28, (uint64_t)(options & ~0x10) };
                s_pa.Execute("ioctl_superio_outb", unlockArgs, 2, nullptr, 0, nullptr);
                // Read it back: if the lock is still set, every HW-monitor read
                // returns 0xFF no matter how often detection is repeated.
                regIn[0] = 0x28; regOut[0] = 0;
                if (s_pa.Execute("ioctl_superio_inb", regIn, 1, regOut, 1, nullptr))
                    s_cr28 = (uint8_t)(regOut[0] & 0xFF);
                if (verbose) Logger::Info("BoardTemp: disabled IO space lock");
                else Logger::Debug("BoardTemp: disabled IO space lock");
            }
        }

        // Step 8: Exit config mode
        uint64_t exitAA[2] = { 0x2E, 0xAA };
        s_pa.Execute("ioctl_pio_outb", exitAA, 2, nullptr, 0, nullptr);
        return ValidIoBase(s_base);
    };

    // A missing/erased base is retried on the next pass: the chip may be held
    // by another agent right now, and giving up permanently is what left the
    // panel stuck on the eight sensors that come from other readers.
    if (!s_ready || !ValidIoBase(s_base)) {
        s_ready = true;
        detect();
    }

    if (!ValidIoBase(s_base)) return result;

    // Read temperature sources using NCT6798D protocol. Two attempts: if the
    // whole pass comes back dead, re-detect once and read again.
    const int count = sizeof(TEMP_SOURCES) / sizeof(TEMP_SOURCES[0]);
    int rawValid = 0;
    uint8_t head[4] = {0};
    for (int attempt = 0; attempt < 2; ++attempt) {
        result.clear();
        rawValid = 0;
        for (int i = 0; i < count; i++) {
            uint8_t raw = HwMonReadByte(s_pa, s_base, TEMP_SOURCES[i].addr);
            if (i < 4) head[i] = raw;
            if (raw == 0xFF || raw == 0x00) continue;
            ++rawValid;

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
        static bool loggedDead = false;
        if (rawValid > 0) {
            if (loggedDead) Logger::Info("BoardTemp: registers answering again");
            loggedDead = false;
            break;
        }
        if (attempt == 0) {
            if (!loggedDead) {
                loggedDead = true;
                Logger::Info("BoardTemp: no readable register, re-detecting Super I/O");
            }
            if (!detect()) break;
        }
    }

    // Sensor count changing between reads means the register reads are
    // unstable, not that the board lost a sensor; log the transitions once.
    static int lastCount = -1;
    if (static_cast<int>(result.size()) != lastCount) {
        lastCount = static_cast<int>(result.size());
        Logger::Info("BoardTemp: " + std::to_string(result.size()) + " readable sensor(s)");
    }
    // First few passes only: distinguishes "registers stopped answering"
    // (raw drops to 0) from "values fell out of range" (raw stays, kept drops).
    static int passes = 0;
    if (++passes <= 8) {
        char sample[96];
        snprintf(sample, sizeof(sample),
                 " base=0x%04X cr28=0x%02X%s raw=0x%02X,0x%02X,0x%02X,0x%02X",
                 s_base, s_cr28, isaLock.held ? "" : "(noisa)",
                 head[0], head[1], head[2], head[3]);
        Logger::Info("BoardTemp: pass " + std::to_string(passes) +
                     " valid=" + std::to_string(rawValid) +
                     " kept=" + std::to_string(result.size()) + sample);
    }

    return result;
}
#endif
