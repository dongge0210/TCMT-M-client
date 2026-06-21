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

// PCODE mailbox registers (offsets within MCHBAR MMIO)
static constexpr uint64_t PCODE_MBOX_CMD  = 0x8000;
static constexpr uint64_t PCODE_MBOX_DATA = 0x8004;
static constexpr uint64_t PCODE_MBOX_BUSY = 0x80000000;

static uint32_t PcodeRead(PawnIOWrapper& pa, uint64_t cmdAddr, uint64_t cmd) {
    uint64_t in[1], out[1];
    // Write command to mailbox
    in[0] = cmdAddr | cmd;
    pa.Execute("ioctl_read_dword", in, 1, out, 1, nullptr);
    // Poll BUSY bit
    for (int i = 0; i < 1000; i++) {
        in[0] = PCODE_MBOX_CMD;
        if (!pa.Execute("ioctl_read_dword", in, 1, out, 1, nullptr))
            return 0xFFFFFFFF;
        if (!((uint32_t)out[0] & PCODE_MBOX_BUSY)) break;
    }
    // Read data
    in[0] = PCODE_MBOX_DATA;
    if (!pa.Execute("ioctl_read_dword", in, 1, out, 1, nullptr))
        return 0xFFFFFFFF;
    return (uint32_t)(out[0] & 0xFFFFFFFF);
}

bool IntelMchbarReader::IsAvailable() {
    return PawnIOWrapper::IsInstalled();
}

std::vector<MchbarSensor> IntelMchbarReader::ReadAll() {
    std::vector<MchbarSensor> result;
    static PawnIOWrapper s_pa;
    static bool s_probed = false;
    static bool s_ok = false;
    static uint64_t s_mchbarAddr = 0;

    if (!s_probed) {
        s_probed = true;
        if (!s_pa.Open()) { Logger::Debug("IntelMchbar: PawnIO not available"); return result; }
        auto data = LoadRes(MCHBAR_RES);
        if (data.empty()) { Logger::Info("IntelMchbar: resource not found"); return result; }
        if (!s_pa.LoadModuleFromMemory(data.data(), data.size(), "IntelMCHBAR")) {
            Logger::Info("IntelMchbar: module load failed"); return result;
        }
        uint64_t addrOut[1] = {0};
        if (!s_pa.Execute("ioctl_get_mchbar_addr", nullptr, 0, addrOut, 1, nullptr)) {
            Logger::Info("IntelMchbar: MCHBAR not accessible"); return result;
        }
        s_mchbarAddr = addrOut[0];
        Logger::Info(std::string("IntelMchbar: MCHBAR at 0x") + std::to_string(s_mchbarAddr));
        s_ok = true;
    }

    if (!s_ok) return result;

    // Read IMC temperature via PCODE mailbox command 0x01
    uint32_t imcRaw = PcodeRead(s_pa, s_mchbarAddr + PCODE_MBOX_CMD, 0x01);
    if (imcRaw != 0xFFFFFFFF && imcRaw != 0) {
        double temp = (double)(imcRaw & 0xFF);
        if (temp > 10 && temp < 125)
            result.push_back({"IMC Temp (PCODE)", temp});
    }

    // Try SoC temperature via PCODE mailbox command 0x2A
    uint32_t socRaw = PcodeRead(s_pa, s_mchbarAddr + PCODE_MBOX_CMD, 0x2A);
    if (socRaw != 0xFFFFFFFF && socRaw != 0) {
        double temp = (double)(socRaw & 0xFF);
        if (temp > 10 && temp < 125)
            result.push_back({"SoC Temp (PCODE)", temp});
    }

    return result;
}
#endif
