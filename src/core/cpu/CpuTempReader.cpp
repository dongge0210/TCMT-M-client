#ifdef TCMT_WINDOWS
#include "CpuTempReader.h"
#include "../memory/PawnIOWrapper.h"
#include "../Utils/Logger.h"

// Intel MSR registers
static const uint32_t MSR_IA32_TEMPERATURE_TARGET = 0x1A2;
static const uint32_t MSR_IA32_THERM_STATUS       = 0x19C;

// Embedded Intel MSR module resource name
static const wchar_t* INTEL_MSR_RES = L"INTEL_MSR";

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

bool CpuTempReader::IsAvailable() {
    return PawnIOWrapper::IsInstalled();
}

double CpuTempReader::ReadPackageTemp() {
    static PawnIOWrapper s_pa;
    static bool s_loaded = false;
    static bool s_intel = false;

    if (!s_loaded) {
        s_loaded = true;
        if (!s_pa.Open()) {
            Logger::Debug("CpuTempReader: PawnIO not available");
            return -1.0;
        }

        // Try Intel MSR module
        auto data = LoadResource(INTEL_MSR_RES);
        if (!data.empty() && s_pa.LoadModuleFromMemory(data.data(), data.size(), "ioctl_read_msr")) {
            s_intel = true;
        } else {
            // Try AMD module (future)
        }

        if (!s_intel) {
            Logger::Debug("CpuTempReader: no CPU temp module loaded");
            return -1.0;
        }
    }

    if (!s_intel) return -1.0;

    // Read TjMax
    uint64_t tjIn[1] = { MSR_IA32_TEMPERATURE_TARGET };
    uint64_t tjOut[1] = { 0 };
    if (!s_pa.Execute("ioctl_read_msr", tjIn, 1, tjOut, 1, nullptr))
        return -1.0;
    uint32_t tjMax = (uint32_t)((tjOut[0] >> 16) & 0xFF);

    // Read digital readout
    uint64_t rdIn[1] = { MSR_IA32_THERM_STATUS };
    uint64_t rdOut[1] = { 0 };
    if (!s_pa.Execute("ioctl_read_msr", rdIn, 1, rdOut, 1, nullptr))
        return -1.0;
    uint32_t digReadout = (uint32_t)((rdOut[0] >> 16) & 0x7F);

    double tempC = static_cast<double>(tjMax) - static_cast<double>(digReadout);
    if (tempC < 0 || tempC > 125) return -1.0;
    return tempC;
}
#endif
