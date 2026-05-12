#include "OSInfo.h"
#include "../Utils/Logger.h"

#ifdef TCMT_WINDOWS
// ======================== Windows Implementation ========================
// NOTE: winsock2.h must be included BEFORE windows.h
#include <winsock2.h>
#include <windows.h>
#include "../Utils/WinUtils.h"
#include <winternl.h>
#include <ntstatus.h>
#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS ((NTSTATUS)0x00000000L)
#endif

typedef NTSTATUS(WINAPI* RtlGetVersionPtr)(PRTL_OSVERSIONINFOW);

OSInfo::OSInfo() {
    RTL_OSVERSIONINFOW osvi = { sizeof(osvi) };
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        RtlGetVersionPtr RtlGetVersion = (RtlGetVersionPtr)GetProcAddress(ntdll, "RtlGetVersion");
        if (RtlGetVersion && RtlGetVersion(&osvi) == STATUS_SUCCESS) {
            osVersion = WinUtils::WstringToString(
                std::wstring(L"Windows ") +
                std::to_wstring(osvi.dwMajorVersion) + L"." +
                std::to_wstring(osvi.dwMinorVersion) +
                L" (Build " + std::to_wstring(osvi.dwBuildNumber) + L")"
            );
        } else {
            osVersion = "Unknown OS Version";
        }
    } else {
        osVersion = "Unknown OS Version";
    }
}

#elif defined(TCMT_MACOS)
// ======================== macOS Implementation ========================
#include <sys/sysctl.h>
#include <string>

OSInfo::OSInfo() {
    // macOS version via sysctl "kern.osproductversion"
    char version[128] = {0};
    size_t len = sizeof(version);
    if (sysctlbyname("kern.osproductversion", version, &len, nullptr, 0) == 0) {
        osVersion = std::string("macOS ") + version;
    } else {
        // Fallback: kern.osrelease
        char release[128] = {0};
        len = sizeof(release);
        if (sysctlbyname("kern.osrelease", release, &len, nullptr, 0) == 0) {
            osVersion = std::string("macOS (Darwin ") + release + ")";
        } else {
            osVersion = "Unknown macOS Version";
        }
    }

    // Append machine model if available (e.g. "MacBookPro17,1")
    char model[128] = {0};
    len = sizeof(model);
    if (sysctlbyname("hw.model", model, &len, nullptr, 0) == 0) {
        osVersion += " (" + std::string(model) + ")";
    }
}

#else
#error "Unsupported platform"
#endif

bool OSInfo::HasTpm() {
#if defined(TCMT_WINDOWS)
    return true; // Actual detection via TpmBridge
#else
    return false; // No TPM support on macOS/Linux
#endif
}

std::string OSInfo::GetVersion() const {
    return osVersion;
}

void OSInfo::Initialize() {
    // Nothing to initialize - everything done in constructor
}
