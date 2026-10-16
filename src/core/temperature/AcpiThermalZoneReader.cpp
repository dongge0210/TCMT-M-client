#ifdef TCMT_WINDOWS
#include "AcpiThermalZoneReader.h"
#include "../Utils/Logger.h"

// winsock2.h 必须排在 windows.h 前面。这个文件自己管住包含顺序，不依赖调用方的。
#include <winsock2.h>
#include <windows.h>
#include <wbemidl.h>
#include <comdef.h>

#include <chrono>

namespace {

// 热区变化很慢，而每次查询要起一次 COM 会话加一次 WMI 查询，不值当每 2 秒跑一遍。
constexpr int kCacheSeconds = 30;

std::string WideToUtf8(const wchar_t* w) {
    if (!w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    return out;
}

// "ACPI\\ThermalZone\\TZ00_0" 取最后一段，够认出是哪块就行
std::string ShortZoneName(const std::string& instanceName) {
    const size_t pos = instanceName.find_last_of('\\');
    return pos == std::string::npos ? instanceName : instanceName.substr(pos + 1);
}

} // namespace

std::vector<AcpiThermalZone> AcpiThermalZoneReader::ReadAll() {
    static std::vector<AcpiThermalZone> cache;
    static std::chrono::steady_clock::time_point lastRead{};

    const auto now = std::chrono::steady_clock::now();
    const bool first = (lastRead.time_since_epoch().count() == 0);
    if (!first &&
        std::chrono::duration_cast<std::chrono::seconds>(now - lastRead).count() < kCacheSeconds) {
        return cache;
    }
    lastRead = now;
    cache.clear();

    const HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // S_FALSE 表示本线程已经初始化过，仍然要配一次 CoUninitialize；
    // RPC_E_CHANGED_MODE 表示别人用别的方式初始化过，这时候不能碰引用计数。
    const bool ownCom = SUCCEEDED(hrCom);
    if (FAILED(hrCom) && hrCom != RPC_E_CHANGED_MODE) {
        Logger::Debug("ACPI thermal zones: COM init failed");
        return cache;
    }

    IWbemLocator* pLoc = nullptr;
    IWbemServices* pSvc = nullptr;
    IEnumWbemClassObject* pEnum = nullptr;

    do {
        if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_IWbemLocator, reinterpret_cast<void**>(&pLoc)))) {
            break;
        }
        if (FAILED(pLoc->ConnectServer(_bstr_t(L"ROOT\\WMI"), nullptr, nullptr, nullptr,
                                       0, nullptr, nullptr, &pSvc))) {
            break;
        }
        if (FAILED(CoSetProxyBlanket(pSvc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                                     RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE,
                                     nullptr, EOAC_NONE))) {
            break;
        }
        if (FAILED(pSvc->ExecQuery(_bstr_t(L"WQL"),
                                   _bstr_t(L"SELECT * FROM MSAcpi_ThermalZoneTemperature"),
                                   WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
                                   nullptr, &pEnum))) {
            // 非管理员在这台机器上会走到这里；属于能力缺失，不是故障
            Logger::Debug("ACPI thermal zones: query unavailable (needs elevation)");
            break;
        }

        for (;;) {
            IWbemClassObject* obj = nullptr;
            ULONG got = 0;
            if (pEnum->Next(WBEM_INFINITE, 1, &obj, &got) != S_OK || got == 0) break;

            // CurrentTemperature 是十分之一开尔文
            bool have = false;
            double celsius = 0.0;
            VARIANT vTemp;
            VariantInit(&vTemp);
            if (SUCCEEDED(obj->Get(L"CurrentTemperature", 0, &vTemp, nullptr, nullptr))) {
                long deci = 0;
                if (vTemp.vt == VT_I4) {
                    deci = vTemp.lVal;
                    have = true;
                } else if (vTemp.vt == VT_UI4) {
                    deci = static_cast<long>(vTemp.ulVal);
                    have = true;
                }
                if (have) celsius = deci / 10.0 - 273.15;
            }
            VariantClear(&vTemp);

            std::string label = "ACPI Thermal Zone";
            VARIANT vName;
            VariantInit(&vName);
            if (SUCCEEDED(obj->Get(L"InstanceName", 0, &vName, nullptr, nullptr)) &&
                vName.vt == VT_BSTR) {
                const std::string full = WideToUtf8(vName.bstrVal);
                if (!full.empty()) label = ShortZoneName(full);
            }
            VariantClear(&vName);
            obj->Release();

            // ACPI 里 0 是「没实现」；越界的读数直接丢，宁可少一行也不摆个假数
            if (have && celsius > -20.0 && celsius < 150.0) {
                cache.push_back({label, celsius});
            }
        }
    } while (0);

    if (pEnum) pEnum->Release();
    if (pSvc)  pSvc->Release();
    if (pLoc)  pLoc->Release();
    if (ownCom) CoUninitialize();

    if (cache.empty()) {
        Logger::Debug("ACPI thermal zones: none reported");
    } else {
        Logger::Debug("ACPI thermal zones: " + std::to_string(cache.size()) + " zone(s)");
    }
    return cache;
}

#endif // TCMT_WINDOWS
