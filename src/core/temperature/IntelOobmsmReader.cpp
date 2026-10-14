#ifdef TCMT_WINDOWS
#include "IntelOobmsmReader.h"
#include "../memory/PawnIOWrapper.h"
#include "../Utils/Logger.h"

static const wchar_t* OOBMSM_RES = L"INTEL_OOBMSM";

static std::vector<uint8_t> LoadRes(const wchar_t* name) {
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

// PCIe capability IDs
static constexpr uint16_t PCI_EXT_CAP_ID_DVSEC = 0x0023;
static constexpr uint16_t INTEL_DVSEC_VENDOR_ID = 0x8086;
static constexpr uint16_t INTEL_PMT_DVSEC_ID = 0x000B;

// Read a DWORD from PCI extended config space
static uint32_t PciConfigRead(PawnIOWrapper& pa, uint32_t offset) {
    uint64_t in[1] = { offset };
    uint64_t out[1] = { 0 };
    if (!pa.Execute("ioctl_pci_config_read_dword", in, 1, out, 1, nullptr))
        return 0xFFFFFFFF;
    return (uint32_t)(out[0] & 0xFFFFFFFF);
}

// Read a DWORD from BAR0
static uint32_t Bar0Read(PawnIOWrapper& pa, uint32_t offset) {
    uint64_t in[1] = { offset };
    uint64_t out[1] = { 0 };
    if (!pa.Execute("ioctl_read_dword", in, 1, out, 1, nullptr))
        return 0xFFFFFFFF;
    return (uint32_t)(out[0] & 0xFFFFFFFF);
}

bool IntelOobmsmReader::IsAvailable() {
    return PawnIOWrapper::IsInstalled();
}

std::vector<OobmsmTemp> IntelOobmsmReader::ReadAll() {
    std::vector<OobmsmTemp> result;
    static PawnIOWrapper s_pa;
    static bool s_probed = false;
    static bool s_ok = false;
    static uint64_t s_bar0Base = 0;
    static uint64_t s_bar0Size = 0;
    static bool s_hasPmt = false;

    if (!s_probed) {
        s_probed = true;
        if (!s_pa.Open()) { Logger::Debug("IntelOobmsm: PawnIO not available"); return result; }
        auto data = LoadRes(OOBMSM_RES);
        if (data.empty()) { Logger::Debug("IntelOobmsm: resource not found"); return result; }
        if (!s_pa.LoadModuleFromMemory(data.data(), data.size(), "IntelOOBMSM")) {
            Logger::Debug("IntelOobmsm: module load failed"); return result;
        }

        uint64_t idOut[4] = {0};
        if (!s_pa.Execute("ioctl_identity", nullptr, 0, idOut, 4, nullptr)) {
            Logger::Debug("IntelOobmsm: identity probe failed"); return result;
        }
        uint16_t vid = (uint16_t)(idOut[0] & 0xFFFF);
        uint16_t did = (uint16_t)((idOut[0] >> 16) & 0xFFFF);
        s_bar0Base = idOut[2];
        s_bar0Size = idOut[3];
        Logger::Debug(std::string("IntelOobmsm: VID=0x") + std::to_string(vid) +
                     " DID=0x" + std::to_string(did) +
                     " BAR0=0x" + std::to_string(s_bar0Base));

        // Walk PCIe extended capabilities for Intel PMT DVSEC (0x000B)
        uint32_t capOff = 0x100;
        for (int i = 0; i < 128 && capOff >= 0x100 && capOff < 0x1000; i++) {
            uint32_t capHdr = PciConfigRead(s_pa, capOff);
            uint16_t capId = (uint16_t)(capHdr & 0xFFFF);
            uint16_t capVer = (uint16_t)((capHdr >> 16) & 0xF);
            uint32_t nextOff = (capHdr >> 20) & 0xFFC;

            if (capId == PCI_EXT_CAP_ID_DVSEC && capVer >= 1) {
                uint32_t dvsec1 = PciConfigRead(s_pa, capOff + 4);
                if ((uint16_t)(dvsec1 & 0xFFFF) == INTEL_DVSEC_VENDOR_ID &&
                    (uint16_t)((dvsec1 >> 16) & 0xFFFF) == INTEL_PMT_DVSEC_ID) {
                    Logger::Debug(std::string("IntelOobmsm: PMT DVSEC at 0x") + std::to_string(capOff));
                    s_hasPmt = true;
                    break;
                }
            }
            if (nextOff == 0 || nextOff <= capOff) break;
            capOff = nextOff;
        }
        s_ok = true;
    }

    if (!s_ok || s_bar0Size == 0) return result;

    // Verify PMT magic in BAR0
    uint32_t magic0 = Bar0Read(s_pa, 0);
    uint32_t magic1 = Bar0Read(s_pa, 4);
    char magic[9] = {};
    for (int i = 0; i < 4; i++) { magic[i] = (char)((magic0 >> (i * 8)) & 0xFF); }
    for (int i = 0; i < 4; i++) { magic[i + 4] = (char)((magic1 >> (i * 8)) & 0xFF); }
    magic[8] = '\0';
    Logger::Debug(std::string("IntelOobmsm: BAR0 magic=\"") + magic + "\"");

    // ── PMT telemetry: why this stops short ────────────────────────────────────
    // Deliberately not decoded. Two things are missing and neither may be guessed:
    //
    //  1. The OOBMSM BAR0 discovery-table layout. The Linux kernel's path does not
    //     transfer: there each entry's header (base_offset / size / guid /
    //     access_type — drivers/platform/x86/intel/pmt/class.h) is its OWN resource,
    //     and num_entries / entry_size come from the PCI DVSEC, not from a table
    //     inside BAR0. OOBMSM is a BAR0-based block with its own spec, which we have
    //     not got. An intermediate version of this comment asserted a
    //     "guid | num_entries | entry_size" header at BAR0+0/+12/+16; that was
    //     inferred from the kernel, not verified against OOBMSM, and was removed.
    //  2. A guid/offset → Celsius map. A telemetry entry is a raw counter, and the
    //     guid names a namespace (PUNIT, crashlog, …), not a sensor. The kernel does
    //     not decode it either — it exposes the raw buffer to userspace.
    //
    // Inventing either one would put manufactured degrees into the temperature list.
    // That is the exact failure this codebase keeps fixing: Platform_macOS.cpp
    // fabricated 45 °C / 55 °C until 2026-10-14. A missing temperature renders as N/A
    // and prompts a question; a fabricated one looks like a reading and gets believed.

    Logger::Debug("IntelOobmsm: PMT telemetry not decoded (no OOBMSM table layout, "
                  "no guid->temperature map) — reporting no temperatures");
    return result;
}
#endif
