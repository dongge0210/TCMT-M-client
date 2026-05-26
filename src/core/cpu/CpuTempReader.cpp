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

static int GetProcessorCount() {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors;
}

// Read MSR on a specific logical processor by setting thread affinity
static bool ReadMsrOnCore(PawnIOWrapper& pa, uint32_t msr, int coreIdx, uint64_t* outVal) {
    HANDLE hThread = GetCurrentThread();
    DWORD_PTR origAffinity = SetThreadAffinityMask(hThread, (DWORD_PTR)1 << coreIdx);
    if (origAffinity == 0) return false;

    uint64_t inBuf[1] = { msr };
    uint64_t outBuf[1] = { 0 };
    bool ok = pa.Execute("ioctl_read_msr", inBuf, 1, outBuf, 1, nullptr);

    SetThreadAffinityMask(hThread, origAffinity);
    if (!ok) return false;
    *outVal = outBuf[0];
    return true;
}

bool CpuTempReader::IsAvailable() {
    return PawnIOWrapper::IsInstalled();
}

std::vector<CpuCoreTemp> CpuTempReader::ReadAll() {
    std::vector<CpuCoreTemp> result;
    static PawnIOWrapper s_pa;
    static bool s_ready = false;
    static uint32_t s_tjMax = 100;
    static int s_coreCount = 0;

    if (!s_ready) {
        if (!s_pa.Open()) return result;
        auto data = LoadResource(INTEL_MSR_RES);
        if (data.empty()) return result;
        if (!s_pa.LoadModuleFromMemory(data.data(), data.size(), "ioctl_read_msr"))
            return result;

        // Read TjMax once (same for all cores)
        uint64_t tjOut[1] = {0};
        uint64_t tjIn[1] = { MSR_IA32_TEMPERATURE_TARGET };
        if (s_pa.Execute("ioctl_read_msr", tjIn, 1, tjOut, 1, nullptr))
            s_tjMax = ((uint32_t)tjOut[0] >> 16) & 0xFF;

        s_coreCount = GetProcessorCount();
        s_ready = true;
        Logger::Info(std::string("CpuTempReader: ready, TjMax=") + std::to_string(s_tjMax) +
                     "C, " + std::to_string(s_coreCount) + " cores");
    }

    if (!s_ready) return result;

    // Read per-core temperatures
    double packageSum = 0;
    int validCores = 0;
    for (int i = 0; i < s_coreCount && i < 32; i++) {
        uint64_t val = 0;
        if (!ReadMsrOnCore(s_pa, MSR_IA32_THERM_STATUS, i, &val))
            continue;
        uint32_t eax = (uint32_t)val;
        if ((eax & 0x80000000) == 0) continue;  // VALID bit

        uint32_t dig = (eax >> 16) & 0x7F;
        int tempC = (int)s_tjMax - (int)dig;
        if (tempC < 0 || tempC > 125) continue;

        CpuCoreTemp ct;
        ct.name = "Core #" + std::to_string(i);
        ct.temperature = (double)tempC;
        result.push_back(ct);
        packageSum += tempC;
        validCores++;
    }

    // Add package average if we have core data
    if (validCores > 0) {
        CpuCoreTemp pkg;
        pkg.name = "CPU Package (PawnIO)";
        pkg.temperature = packageSum / validCores;
        result.push_back(pkg);
    }

    return result;
}
#endif
