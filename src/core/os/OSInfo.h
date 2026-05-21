#pragma once
#include <string>

#ifdef TCMT_WINDOWS
// winsock2.h must be before windows.h
#include <winsock2.h>
#include <windows.h>
#elif defined(TCMT_MACOS)
// macOS headers - no special includes needed for OSInfo
#endif

class OSInfo {
public:
    OSInfo();
    std::string GetVersion() const;
    std::string GetModel() const;       // e.g. "Mac14,2"
    void Initialize();
    static bool HasTpm();
private:
    std::string osVersion;
    std::string model;
#ifdef TCMT_WINDOWS
    DWORD majorVersion;
    DWORD minorVersion;
    DWORD buildNumber;
#endif
};
