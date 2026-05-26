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
            Logger::Info("CpuTempReader: PawnIO device not available");
            return -1.0;
        }
        auto data = LoadResource(INTEL_MSR_RES);
        Logger::Info(std::string("CpuTempReader: IntelMSR resource ") +
                     (data.empty() ? "NOT FOUND" : "loaded " + std::to_string(data.size()) + " bytes"));
        if (!data.empty() && s_pa.LoadModuleFromMemory(data.data(), data.size(), "ioctl_read_msr")) {
            s_intel = true;
            Logger::Info("CpuTempReader: Intel MSR module loaded");
        }
        if (!s_intel) {
            Logger::Info("CpuTempReader: no CPU temp module loaded");
            return -1.0;
        }
    }

    if (!s_intel) return -1.0;

    // Read TjMax
    uint64_t tjIn[1] = { MSR_IA32_TEMPERATURE_TARGET };
    uint64_t tjOut[1] = { 0 };
    static int dbgN = 0;
    if (!s_pa.Execute("ioctl_read_msr", tjIn, 1, tjOut, 1, nullptr)) {
        if (dbgN < 5) { Logger::Info("CpuTempReader: MSR 0x1A2 read FAILED"); dbgN++; }
        return -1.0;
    }
    uint32_t tjRaw = (uint32_t)tjOut[0];
    uint32_t tjMax = (tjRaw >> 16) & 0xFF;

    // Read digital readout
    uint64_t rdIn[1] = { MSR_IA32_THERM_STATUS };
    uint64_t rdOut[1] = { 0 };
    if (!s_pa.Execute("ioctl_read_msr", rdIn, 1, rdOut, 1, nullptr)) {
        if (dbgN < 5) { Logger::Info("CpuTempReader: MSR 0x19C read FAILED"); dbgN++; }
        return -1.0;
    }
    uint32_t rdRaw = (uint32_t)rdOut[0];
    uint32_t digReadout = (rdRaw >> 16) & 0x7F;

    if (dbgN < 5) {
        Logger::Info(std::string("CpuTempReader: TjMax raw=") + std::to_string(tjRaw) +
                     " tjMax=" + std::to_string(tjMax) +
                     " therm raw=" + std::to_string(rdRaw) +
                     " dig=" + std::to_string(digReadout) +
                     " temp=" + std::to_string(tjMax - digReadout) + "C");
        dbgN++;
    }

    double tempC = static_cast<double>(tjMax) - static_cast<double>(digReadout);
    if (tempC < 0 || tempC > 125) return -1.0;
    return tempC;
}
#endif
