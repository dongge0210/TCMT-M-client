#ifdef TCMT_WINDOWS
#include "RyzenSmuReader.h"
#include "../memory/PawnIOWrapper.h"
#include "../Utils/Logger.h"

static const wchar_t* RYZEN_SMU_RES = L"RYZEN_SMU";

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

bool RyzenSmuReader::IsAvailable() {
    return PawnIOWrapper::IsInstalled();
}

std::vector<RyzenSensor> RyzenSmuReader::ReadAll() {
    std::vector<RyzenSensor> result;
    static PawnIOWrapper s_pa;
    static bool s_probed = false;
    static bool s_ok = false;
    static uint32_t s_smuVersion = 0;
    static std::string s_codeName;

    if (!s_probed) {
        s_probed = true;
        if (!s_pa.Open()) {
            Logger::Debug("RyzenSmuReader: PawnIO not available");
            return result;
        }
        auto data = LoadRes(RYZEN_SMU_RES);
        if (data.empty()) {
            Logger::Info("RyzenSmuReader: module resource not found");
            return result;
        }
        if (!s_pa.LoadModuleFromMemory(data.data(), data.size(), "RyzenSMU")) {
            Logger::Info("RyzenSmuReader: module load failed (not an AMD platform?)");
            return result;
        }

        // Probe SMU version and codename
        uint64_t verOut[1] = {0};
        if (s_pa.Execute("ioctl_get_smu_version", nullptr, 0, verOut, 1, nullptr)) {
            s_smuVersion = (uint32_t)verOut[0];
        }
        uint64_t nameOut[1] = {0};
        if (s_pa.Execute("ioctl_get_code_name", nullptr, 0, nameOut, 1, nullptr)) {
            s_codeName = std::to_string(nameOut[0]);
        }
        Logger::Info(std::string("RyzenSmuReader: SMU v") + std::to_string(s_smuVersion) +
                     " codename=" + s_codeName);

        // Resolve and update PM table
        uint64_t resolveOut[2] = {0};
        if (s_pa.Execute("ioctl_resolve_pm_table", nullptr, 0, resolveOut, 2, nullptr)) {
            uint32_t pmVersion = (uint32_t)resolveOut[0];
            uint32_t tableBase = (uint32_t)resolveOut[1];
            Logger::Info(std::string("RyzenSmuReader: PM table v") + std::to_string(pmVersion) +
                         " base=0x" + std::to_string(tableBase));
        }
        // Update the PM table from SMU to DRAM before reading
        s_pa.Execute("ioctl_update_pm_table", nullptr, 0, nullptr, 0, nullptr);

        s_ok = true;
    }

    if (!s_ok) return result;

    // TODO: PM table decoding — the table layout is codename-specific.
    // Each entry encodes a sensor type (temperature, power, current, frequency)
    // and value. The decoding logic is in LibreHardwareMonitor's RyzenSmu.cs.
    //
    // We read a fixed number of entries each cycle. The table can be up to
    // several hundred entries on modern platforms.
    constexpr int PM_TABLE_SIZE = 128;
    uint64_t pmOut[128] = {0};
    if (!s_pa.Execute("ioctl_read_pm_table", nullptr, 0, pmOut, PM_TABLE_SIZE, nullptr))
        return result;

    // Placeholder: scan for plausible temperature entries (30–100°C range)
    // Real decoding requires codename-specific layout tables.
    int sensorCount = 0;
    for (int i = 0; i < PM_TABLE_SIZE && sensorCount < 16; i++) {
        uint64_t raw = pmOut[i];
        if (raw == 0 || raw == 0xFFFFFFFFFFFFFFFFULL) continue;
        // Heuristic: values in 30–100 range could be temperatures
        double val = static_cast<double>(raw & 0xFFFFFFFF);
        if (val >= 30.0 && val <= 100.0) {
            result.push_back({std::string("SMU_Sensor_") + std::to_string(i), val, "C"});
            sensorCount++;
        }
    }

    return result;
}
#endif
