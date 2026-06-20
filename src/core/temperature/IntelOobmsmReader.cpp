#ifdef TCMT_WINDOWS
#include "IntelOobmsmReader.h"
#include "../memory/PawnIOWrapper.h"
#include "../Utils/Logger.h"

static const wchar_t* OOBMSM_RES = L"INTEL_OOBMSM";

// PMT (Platform Monitoring Technology) discovery lives in BAR0 extended PCIe caps.
// The telemetry layout is platform-specific; we start with a basic identity probe
// and leave detailed sensor decoding for future work.

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

bool IntelOobmsmReader::IsAvailable() {
    return PawnIOWrapper::IsInstalled();
}

std::vector<OobmsmTemp> IntelOobmsmReader::ReadAll() {
    std::vector<OobmsmTemp> result;
    static PawnIOWrapper s_pa;
    static bool s_probed = false;
    static bool s_ok = false;

    if (!s_probed) {
        s_probed = true;
        if (!s_pa.Open()) {
            Logger::Debug("IntelOobmsmReader: PawnIO not available");
            return result;
        }
        auto data = LoadRes(OOBMSM_RES);
        if (data.empty()) {
            Logger::Info("IntelOobmsmReader: module resource not found");
            return result;
        }
        if (!s_pa.LoadModuleFromMemory(data.data(), data.size(), "IntelOOBMSM")) {
            Logger::Info("IntelOobmsmReader: module load failed (not a Core Ultra platform?)");
            return result;
        }

        // Probe identity — verify the OOBMSM device is present
        uint64_t idOut[4] = {0};
        if (!s_pa.Execute("ioctl_identity", nullptr, 0, idOut, 4, nullptr)) {
            Logger::Info("IntelOobmsmReader: identity probe failed");
            return result;
        }
        uint16_t vid = (uint16_t)(idOut[0] & 0xFFFF);
        uint16_t did = (uint16_t)((idOut[0] >> 16) & 0xFFFF);
        uint64_t bar0 = idOut[2];
        uint64_t bar0Size = idOut[3];
        Logger::Info(std::string("IntelOobmsmReader: VID=") + std::to_string(vid) +
                     " DID=" + std::to_string(did) +
                     " BAR0=0x" + std::to_string(bar0) +
                     " size=0x" + std::to_string(bar0Size));
        s_ok = true;
    }

    if (!s_ok) return result;

    // TODO: PMT telemetry decoding — walk BAR0 for DVSEC 0x000B / TPMI 0x0023
    // and parse per-IP-block temperature/power telemetry entries.
    // Reference: LibreHardwareMonitorLib.PawnIo.IntelOobmsm (LHM submodule)

    return result;
}
#endif
