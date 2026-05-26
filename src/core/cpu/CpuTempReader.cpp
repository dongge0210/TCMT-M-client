#ifdef TCMT_WINDOWS
#include "CpuTempReader.h"
#include "../memory/PawnIOWrapper.h"
#include "../Utils/Logger.h"

static const uint32_t MSR_IA32_TEMPERATURE_TARGET = 0x1A2;
static const uint32_t MSR_IA32_THERM_STATUS       = 0x19C;
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
    static bool s_ready = false;

    if (!s_ready) {
        if (!s_pa.Open()) return -1.0;
        auto data = LoadResource(INTEL_MSR_RES);
        if (data.empty()) return -1.0;
        if (!s_pa.LoadModuleFromMemory(data.data(), data.size(), "ioctl_read_msr"))
            return -1.0;
        s_ready = true;
        Logger::Info("CpuTempReader: Intel MSR module ready");
    }

    uint64_t tjOut[1] = {0}, rdOut[1] = {0};
    uint64_t tjIn[1] = { MSR_IA32_TEMPERATURE_TARGET };
    uint64_t rdIn[1] = { MSR_IA32_THERM_STATUS };

    if (!s_pa.Execute("ioctl_read_msr", tjIn, 1, tjOut, 1, nullptr))
        return -1.0;
    uint32_t tjMax = ((uint32_t)tjOut[0] >> 16) & 0xFF;

    if (!s_pa.Execute("ioctl_read_msr", rdIn, 1, rdOut, 1, nullptr))
        return -1.0;
    uint32_t rdEax = (uint32_t)rdOut[0];
    if ((rdEax & 0x80000000) == 0) return -1.0;  // VALID bit must be set
    uint32_t dig = (rdEax >> 16) & 0x7F;

    int tempC = (int)tjMax - (int)dig;
    if (tempC < 0 || tempC > 125) return -1.0;

    static int lastTemp = -1;
    if (tempC != lastTemp) {
        Logger::Info(std::string("CpuTempReader: ") + std::to_string(tempC) +
                     "C (TjMax=" + std::to_string(tjMax) + " dig=" + std::to_string(dig) + ")");
        lastTemp = tempC;
    }
    return (double)tempC;
}
#endif
