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

namespace {

struct TempReaderState {
    PawnIOWrapper pa;
    bool ready = false;
    uint32_t tjMax = 100;
    int logicalCount = 0;
};

// One PawnIO session shared by the aggregate and per-core readers; opening
// and loading the kernel module twice would be wasteful and can fail.
TempReaderState& TempState() {
    static TempReaderState st;
    static bool tried = false;
    if (!tried) {
        tried = true;
        if (!st.pa.Open()) return st;
        auto data = LoadResource(INTEL_MSR_RES);
        if (data.empty()) return st;
        if (!st.pa.LoadModuleFromMemory(data.data(), data.size(), "ioctl_read_msr"))
            return st;

        // Read TjMax once (same for all cores)
        uint64_t tjOut[1] = {0};
        uint64_t tjIn[1] = { MSR_IA32_TEMPERATURE_TARGET };
        if (st.pa.Execute("ioctl_read_msr", tjIn, 1, tjOut, 1, nullptr))
            st.tjMax = ((uint32_t)tjOut[0] >> 16) & 0xFF;

        st.logicalCount = GetProcessorCount();
        st.ready = true;
        Logger::Info(std::string("CpuTempReader: ready, TjMax=") + std::to_string(st.tjMax) +
                     "C, " + std::to_string(st.logicalCount) + " logical CPUs");
    }
    return st;
}

// First logical processor of every physical core, in enumeration order
// (P cores before E cores on hybrid parts). The sibling thread shares the
// same die sensor, so one read per core is enough.
std::vector<int> PhysicalCoreFirstThreads() {
    std::vector<int> first;
    // The sizing call fails with ERROR_INSUFFICIENT_BUFFER by design; only the
    // length it writes is meaningful. Treating that FALSE as a failure used to
    // drop the whole enumeration and fall back to the first 16 logical CPUs,
    // which on a hybrid part are all P-core threads.
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    if (len == 0)
        return first;

    std::vector<unsigned char> buf(len);
    auto* base = reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(buf.data());
    if (!GetLogicalProcessorInformationEx(RelationProcessorCore, base, &len))
        return first;

    for (DWORD off = 0; off < len; ) {
        const auto* info = reinterpret_cast<const PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(
            buf.data() + off);
        if (info->Relationship == RelationProcessorCore) {
            const KAFFINITY mask = info->Processor.GroupMask[0].Mask;
            for (int bit = 0; bit < 64; ++bit) {
                if (mask & (KAFFINITY(1) << bit)) {
                    first.push_back(bit);
                    break;
                }
            }
        }
        off += info->Size;
    }
    return first;
}

}  // namespace

std::vector<CpuCoreTemp> CpuTempReader::ReadAll() {
    std::vector<CpuCoreTemp> result;
    TempReaderState& st = TempState();
    if (!st.ready) return result;

    // Read all cores for max + average
    double coreMax = 0, coreSum = 0, packageTemp = 0;
    int validCores = 0;
    for (int i = 0; i < st.logicalCount && i < 32; i++) {
        uint64_t val = 0;
        if (!ReadMsrOnCore(st.pa, MSR_IA32_THERM_STATUS, i, &val))
            continue;
        uint32_t eax = (uint32_t)val;
        if ((eax & 0x80000000) == 0) continue;
        uint32_t dig = (eax >> 16) & 0x7F;
        int tempC = (int)st.tjMax - (int)dig;
        if (tempC < 0 || tempC > 125) continue;

        if (i == 0) packageTemp = tempC;
        if (tempC > coreMax) coreMax = tempC;
        coreSum += tempC;
        validCores++;
    }
    if (validCores == 0) return result;

    result.push_back({"CPU Package", packageTemp});
    result.push_back({"CPU Core Max", coreMax});
    result.push_back({"CPU Core Average", coreSum / validCores});
    return result;
}

std::vector<double> CpuTempReader::ReadPerCore() {
    std::vector<double> result;
    TempReaderState& st = TempState();
    if (!st.ready) return result;

    std::vector<int> logical = PhysicalCoreFirstThreads();
    if (logical.empty()) {
        // No topology info: fall back to logical processors, capped at the
        // panel's 16 columns.
        for (int i = 0; i < st.logicalCount && i < 16; ++i) logical.push_back(i);
    }

    result.reserve(logical.size());
    for (int lp : logical) {
        uint64_t val = 0;
        double tempC = 0.0;
        if (ReadMsrOnCore(st.pa, MSR_IA32_THERM_STATUS, lp, &val)) {
            const uint32_t eax = (uint32_t)val;
            if ((eax & 0x80000000) != 0) {
                const int t = (int)st.tjMax - (int)((eax >> 16) & 0x7F);
                if (t >= 0 && t <= 125) tempC = t;
            }
        }
        result.push_back(tempC);
    }
    return result;
}
#endif
