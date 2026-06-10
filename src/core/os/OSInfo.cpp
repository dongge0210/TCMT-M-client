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
    char modelBuf[128] = {0};
    len = sizeof(modelBuf);
    if (sysctlbyname("hw.model", modelBuf, &len, nullptr, 0) == 0) {
        model = modelBuf;
        osVersion += " (" + model + ")";
    }
}

#elif defined(TCMT_LINUX)
// ======================== Linux Implementation ========================
#include <sys/utsname.h>
#include <fstream>
#include <sstream>
#include <string>
#include <unistd.h>

OSInfo::OSInfo() {
    // Kernel version via uname()
    struct utsname buf;
    std::string kernelVersion;
    if (uname(&buf) == 0) {
        kernelVersion = std::string(buf.sysname) + " " + buf.release;
    } else {
        kernelVersion = "Unknown";
    }

    // Distro name from /etc/os-release
    std::string distroName = "Linux";
    std::ifstream osRelease("/etc/os-release");
    if (osRelease.is_open()) {
        std::string line;
        while (std::getline(osRelease, line)) {
            if (line.substr(0, 12) == "PRETTY_NAME=") {
                size_t start = line.find('"');
                size_t end = line.rfind('"');
                if (start != std::string::npos && end > start) {
                    distroName = line.substr(start + 1, end - start - 1);
                }
                break;
            }
        }
    } else {
        // Fallback: try /etc/lsb-release
        std::ifstream lsbRelease("/etc/lsb-release");
        if (lsbRelease.is_open()) {
            std::string line;
            while (std::getline(lsbRelease, line)) {
                if (line.substr(0, 11) == "DISTRIB_DESCRIPTION=") {
                    size_t start = line.find('"');
                    size_t end = line.rfind('"');
                    if (start != std::string::npos && end > start) {
                        distroName = line.substr(start + 1, end - start - 1);
                    }
                    break;
                }
            }
        }
    }

    osVersion = distroName + " (" + kernelVersion + ")";

    // Machine model from DMI
    std::ifstream dmiProduct("/sys/devices/virtual/dmi/id/product_name");
    if (dmiProduct.is_open()) {
        std::getline(dmiProduct, model);
        dmiProduct.close();
    }
    if (model.empty()) {
        model = "Unknown Linux Machine";
    }
}

#else
#error "Unsupported platform"
#endif

bool OSInfo::HasTpm() {
#if defined(TCMT_WINDOWS)
    return true; // Actual detection via TpmBridge
#elif defined(TCMT_LINUX)
    return (access("/sys/class/tpm/tpm0/", F_OK) == 0) || (access("/dev/tpm0", F_OK) == 0);
#else
    return false; // No TPM support on macOS
#endif
}

std::string OSInfo::GetVersion() const {
    return osVersion;
}

std::string OSInfo::GetModel() const {
    return model;
}

void OSInfo::Initialize() {
    // Nothing to initialize - everything done in constructor
}
