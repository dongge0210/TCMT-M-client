#ifdef TCMT_WINDOWS
#include "RyzenSmuReader.h"
#include "../memory/PawnIOWrapper.h"
#include "../Utils/Logger.h"
#include <cmath>
#include <map>
#include <cstring>

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

// PM table sensor descriptor — ported from LHM's SmuSensorType
// Type: 0=temp, 1=power, 2=current, 3=voltage, 4=clock
struct SmuSensorDef {
    const char* name;
    int type;     // 0=temp(C), 1=power(W), 2=current(A), 3=voltage(V), 4=clock(MHz)
    float scale;
};

// Convert PM table index to float pointer (4 bytes per entry in the raw buffer)
static float PmEntry(const uint64_t* raw, uint32_t index) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(raw);
    float val;
    std::memcpy(&val, bytes + index * 4, sizeof(float));
    return val;
}

// PM table sensor definitions for known Zen platforms (key = PM table version)
// Source: LibreHardwareMonitor.LibreHardwareMonitorLib.Hardware.RyzenSMU
static const std::map<uint32_t, std::map<uint32_t, SmuSensorDef>> kPmSensorMap = {
    // Zen 2 (Matisse)
    {0x00240903, {
        {15, {"TDC",          2, 1.0f}},
        {21, {"EDC",          2, 1.0f}},
        {48, {"Fabric Clock", 4, 1.0f}},
        {50, {"Uncore Clock", 4, 1.0f}},
        {51, {"Memory Clock", 4, 1.0f}},
        {115,{"SoC Temp",     0, 1.0f}},
    }},
    // Zen 3 (Vermeer)
    {0x00380805, {
        {3,  {"TDC",          2, 1.0f}},
        {48, {"Fabric Clock", 4, 1.0f}},
        {50, {"Uncore Clock", 4, 1.0f}},
        {51, {"Memory Clock", 4, 1.0f}},
        {127,{"SoC Temp",     0, 1.0f}},
    }},
    // Zen 4 (Raphael / Granite Ridge)
    {0x00540004, {
        {3,  {"CPU PPT",      1, 1.0f}},
        {11, {"Package Temp", 0, 1.0f}},
        {20, {"Core Power",   1, 1.0f}},
        {21, {"SoC Power",    1, 1.0f}},
        {22, {"Misc Power",   1, 1.0f}},
        {26, {"Total Power",  1, 1.0f}},
        {47, {"VDDCR",        3, 1.0f}},
        {48, {"TDC",          2, 1.0f}},
        {49, {"EDC",          2, 1.0f}},
        {52, {"VDDCR SoC",    3, 1.0f}},
        {57, {"VDD Misc",     3, 1.0f}},
        {70, {"Fabric Clock", 4, 1.0f}},
        {74, {"Uncore Clock", 4, 1.0f}},
        {78, {"Memory Clock", 4, 1.0f}},
        {211,{"IOD Hotspot",  0, 1.0f}},
        {539,{"L3 CCD1 Temp", 0, 1.0f}},
        {540,{"L3 CCD2 Temp", 0, 1.0f}},
        {268,{"LDO VDD",      3, 1.0f}},
    }},
    // Zen Raven Ridge APU
    {0x001E0004, {
        {7,  {"TDC",          2, 1.0f}},
        {11, {"EDC",          2, 1.0f}},
        {66, {"SoC Current",  2, 1.0f}},
        {67, {"SoC Power",    1, 1.0f}},
        {108,{"Core #1 Temp", 0, 1.0f}},
        {109,{"Core #2 Temp", 0, 1.0f}},
        {110,{"Core #3 Temp", 0, 1.0f}},
        {111,{"Core #4 Temp", 0, 1.0f}},
        {151,{"GFX Temp",     0, 1.0f}},
        {156,{"GFX Load",     5, 1.0f}},  // 5 = load %
    }},
};

// PM table sizes by codename + version
// Returns size in bytes, or 0 if unknown.
static uint32_t GetPmTableSize(int64_t codeName, uint32_t pmVersion) {
    // Codename values from LHM's CpuCodeName enum (GetCodeName returns these)
    // Matisse=402, Vermeer=410, Raphael=415, Cezanne=412, Renoir=400
    // RavenRidge=405, RavenRidge2=406, Picasso=401
    switch (codeName) {
    case 402: // Matisse (Zen 2)
        switch (pmVersion) {
        case 0x240902: return 0x514;
        case 0x240903: return 0x518;
        case 0x240802: return 0x7E0;
        case 0x240803: return 0x7E4;
        default: return 0x518; // fallback
        }
    case 410: // Vermeer (Zen 3)
        switch (pmVersion) {
        case 0x2D0903: return 0x594;
        case 0x380904: return 0x5A4;
        case 0x380905: return 0x5D0;
        case 0x2D0803: return 0x894;
        case 0x380804: return 0x8A4;
        case 0x380805: return 0x8F0;
        default: return 0x8F0; // fallback
        }
    case 415: case 416: // Raphael, GraniteRidge (Zen 4)
        switch (pmVersion) {
        case 0x00540004: return 0x948;
        case 0x00540104: return 0x950;
        default: return 0x948; // fallback
        }
    case 400: // Renoir
        return 0x8AC;
    case 412: // Cezanne
        return 0x944;
    case 405: case 406: case 401: // RavenRidge/Picasso
        return 0x608 + 0xA4;
    default:
        return 0; // unknown — will use heuristic fallback
    }
}

static const char* TypeToUnit(int type) {
    switch (type) { case 0: return "C"; case 1: return "W"; case 2: return "A";
        case 3: return "V"; case 4: return "MHz"; default: return ""; }
}

bool RyzenSmuReader::IsAvailable() {
    return PawnIOWrapper::IsInstalled();
}

std::vector<RyzenSensor> RyzenSmuReader::ReadAll() {
    std::vector<RyzenSensor> result;
    static PawnIOWrapper s_pa;
    static bool s_probed = false;
    static bool s_ok = false;
    static uint32_t s_pmVersion = 0;
    static uint32_t s_pmTableSize = 0;
    static int64_t s_codeNameId = -1;
    static const std::map<uint32_t, SmuSensorDef>* s_sensorDefs = nullptr;

    if (!s_probed) {
        s_probed = true;
        if (!s_pa.Open()) { Logger::Debug("RyzenSmu: PawnIO not available"); return result; }
        auto data = LoadRes(RYZEN_SMU_RES);
        if (data.empty()) { Logger::Info("RyzenSmu: resource not found"); return result; }
        if (!s_pa.LoadModuleFromMemory(data.data(), data.size(), "RyzenSMU")) {
            Logger::Info("RyzenSmu: module load failed"); return result;
        }

        uint64_t verOut[1] = {0};
        s_pa.Execute("ioctl_get_smu_version", nullptr, 0, verOut, 1, nullptr);

        uint64_t nameOut[1] = {0};
        s_pa.Execute("ioctl_get_code_name", nullptr, 0, nameOut, 1, nullptr);
        s_codeNameId = (int64_t)nameOut[0];

        uint64_t resolveOut[2] = {0};
        if (s_pa.Execute("ioctl_resolve_pm_table", nullptr, 0, resolveOut, 2, nullptr)) {
            s_pmVersion = (uint32_t)resolveOut[0];
            uint32_t tableBase = (uint32_t)resolveOut[1];
            Logger::Info(std::string("RyzenSmu: codename=") + std::to_string(s_codeNameId) +
                         " pmVer=0x" + std::to_string(s_pmVersion) +
                         " base=0x" + std::to_string(tableBase));
        }

        s_pmTableSize = GetPmTableSize(s_codeNameId, s_pmVersion);
        auto it = kPmSensorMap.find(s_pmVersion);
        if (it != kPmSensorMap.end()) {
            s_sensorDefs = &it->second;
            Logger::Info(std::string("RyzenSmu: ") + std::to_string(it->second.size()) +
                         " known sensors");
        }
        if (s_pmTableSize == 0 && s_sensorDefs == nullptr) {
            Logger::Info("RyzenSmu: unknown platform, using heuristic fallback");
        }

        s_ok = true;
    }

    if (!s_ok) return result;

    // Update PM table from SMU DRAM buffer
    s_pa.Execute("ioctl_update_pm_table", nullptr, 0, nullptr, 0, nullptr);

    // Read PM table into uint64 buffer
    uint32_t entryCount = s_pmTableSize > 0 ? (s_pmTableSize + 7) / 8 : 256;
    if (entryCount > 1024) entryCount = 1024;
    std::vector<uint64_t> raw(entryCount, 0);
    if (!s_pa.Execute("ioctl_read_pm_table", nullptr, 0, raw.data(), (uint32_t)raw.size(), nullptr))
        return result;

    // Decode known sensors
    if (s_sensorDefs && s_pmTableSize > 0) {
        for (const auto& [idx, def] : *s_sensorDefs) {
            if (idx >= s_pmTableSize / 4) continue;
            float val = PmEntry(raw.data(), idx) * def.scale;
            if (val > -1000.f && val < 100000.f)
                result.push_back({def.name, (double)val, TypeToUnit(def.type)});
        }
        return result;
    }

    // Heuristic fallback: scan for plausible temperature entries (30–100°C)
    int maxEntries = (int)std::min((size_t)256, raw.size());
    int sensorCount = 0;
    for (int i = 0; i < maxEntries && sensorCount < 16; i++) {
        float val = PmEntry(raw.data(), i);
        if (val >= 30.0f && val <= 100.0f && std::isfinite(val)) {
            result.push_back({std::string("SMU_Sensor_") + std::to_string(i), (double)val, "C"});
            sensorCount++;
        }
    }
    return result;
}
#endif
