#ifdef TCMT_WINDOWS
#include "CpuPowerReader.h"
#include "../memory/PawnIOWrapper.h"
#include "../Utils/Logger.h"

#include <chrono>
#include <cstdint>
#include <vector>

namespace {

constexpr uint32_t kMsrRaplPowerUnit   = 0x606;  // 能量单位 ESU 在 [12:8]
constexpr uint32_t kMsrPkgEnergyStatus = 0x611;  // 包能量计数器
const wchar_t* kIntelMsrResource = L"INTEL_MSR"; // 与 CpuTempReader 同一份 PawnIO 模块

std::vector<uint8_t> LoadModuleBytes(const wchar_t* name) {
    HMODULE hMod = GetModuleHandleW(nullptr);
    HRSRC hRes = FindResourceW(hMod, name, RT_RCDATA);
    if (!hRes) return {};
    HGLOBAL hGlobal = LoadResource(hMod, hRes);
    if (!hGlobal) return {};
    const DWORD size = SizeofResource(hMod, hRes);
    const uint8_t* data = static_cast<const uint8_t*>(LockResource(hGlobal));
    if (!data || size == 0 || size > 65536) return {};
    return std::vector<uint8_t>(data, data + size);
}

} // namespace

bool CpuPowerReader::IsAvailable() {
    return PawnIOWrapper::IsInstalled();
}

double CpuPowerReader::ReadPackagePowerW() {
    // 自己开一个 PawnIO 会话：CpuTempReader 的那个是它函数内的局部静态，拿不到。
    // 两个会话各自加载一份只读的 MSR 模块，代价可忽略，换来的是不必去动已经在跑的
    // 温度那条路。
    static PawnIOWrapper pa;
    static bool tried = false;
    static bool ready = false;
    static double joulePerCount = 1.0 / 16384.0;   // ESU 缺省按 14 位算
    static uint32_t lastEnergy = 0;
    static std::chrono::steady_clock::time_point lastAt{};

    if (!tried) {
        tried = true;
        if (!pa.Open()) {
            Logger::Debug("CPU power: PawnIO not available, skipping RAPL");
            return -1.0;
        }
        const auto data = LoadModuleBytes(kIntelMsrResource);
        if (data.empty() ||
            !pa.LoadModuleFromMemory(data.data(), data.size(), "ioctl_read_msr")) {
            Logger::Debug("CPU power: INTEL_MSR module unavailable");
            return -1.0;
        }
        uint64_t unitIn[1]  = { kMsrRaplPowerUnit };
        uint64_t unitOut[1] = { 0 };
        uint32_t esu = 14;
        if (pa.Execute("ioctl_read_msr", unitIn, 1, unitOut, 1, nullptr)) {
            const uint32_t read = static_cast<uint32_t>((unitOut[0] >> 8) & 0x1F);
            if (read > 0 && read < 32) esu = read;
        } else {
            Logger::Debug("CPU power: RAPL power-unit MSR unreadable, assuming 2^-14 J");
        }
        joulePerCount = 1.0 / static_cast<double>(uint64_t{1} << esu);
        Logger::Info("CPU power: RAPL enabled, energy unit 2^-" + std::to_string(esu) + " J");
        ready = true;
    }
    if (!ready) return -1.0;

    uint64_t in[1]  = { kMsrPkgEnergyStatus };
    uint64_t out[1] = { 0 };
    if (!pa.Execute("ioctl_read_msr", in, 1, out, 1, nullptr)) return -1.0;
    const uint32_t energy = static_cast<uint32_t>(out[0] & 0xFFFFFFFFULL);

    const auto now = std::chrono::steady_clock::now();
    if (lastAt.time_since_epoch().count() == 0) {
        // 第一次只建立基线，没有差值可算
        lastAt = now;
        lastEnergy = energy;
        return -1.0;
    }

    const double dtSec = std::chrono::duration<double>(now - lastAt).count();
    if (dtSec < 0.05) return -1.0;   // 太密的两次采样算出来的数没有意义

    // 计数器是 32 位的，会回绕；按无符号取差
    const uint32_t delta = energy - lastEnergy;
    lastAt = now;
    lastEnergy = energy;

    const double watts = static_cast<double>(delta) * joulePerCount / dtSec;
    // 回绕、虚拟化、计数器没启用都会给出离谱的数，宁可报「没有」
    if (!(watts >= 0.0) || watts > 2000.0) return -1.0;
    return watts;
}

#endif // TCMT_WINDOWS
