#ifdef TCMT_WINDOWS
#include "IntelMchbarReader.h"
#include "../memory/PawnIOWrapper.h"
#include "../Utils/Logger.h"

static const wchar_t* MCHBAR_RES = L"INTEL_MCHBAR";

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

bool IntelMchbarReader::IsAvailable() {
    return PawnIOWrapper::IsInstalled();
}

std::vector<MchbarSensor> IntelMchbarReader::ReadAll() {
    std::vector<MchbarSensor> result;
    static PawnIOWrapper s_pa;
    static bool s_probed = false;
    static bool s_ok = false;

    if (!s_probed) {
        s_probed = true;
        if (!s_pa.Open()) {
            Logger::Debug("IntelMchbarReader: PawnIO not available");
            return result;
        }
        auto data = LoadRes(MCHBAR_RES);
        if (data.empty()) {
            Logger::Info("IntelMchbarReader: module resource not found");
            return result;
        }
        if (!s_pa.LoadModuleFromMemory(data.data(), data.size(), "IntelMCHBAR")) {
            Logger::Info("IntelMchbarReader: module load failed (not an Intel platform?)");
            return result;
        }

        // Verify MCHBAR is accessible
        uint64_t addrOut[1] = {0};
        if (!s_pa.Execute("ioctl_get_mchbar_addr", nullptr, 0, addrOut, 1, nullptr)) {
            Logger::Info("IntelMchbarReader: MCHBAR not accessible");
            return result;
        }
        Logger::Info(std::string("IntelMchbarReader: MCHBAR at 0x") + std::to_string(addrOut[0]));
        s_ok = true;
    }

    if (!s_ok) return result;

    // TODO: MCHBAR register decoding — target offsets for:
    // - PCODE mailbox (platform telemetry queries)
    // - IMC thermal status (memory thermal throttling)
    // - Uncore frequency / ring ratio
    // Reference: Linux kernel i915/intel-gtt drivers

    return result;
}
#endif
