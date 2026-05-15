#pragma once
#include <string>
#include <cstdint>

#ifdef TCMT_WINDOWS
// winsock2.h must be before windows.h
#include <winsock2.h>
#include <windows.h>
#else
// Define DWORD for non-Windows platforms
typedef uint32_t DWORD;
#endif

class WinUtils {
public:
    // UTF-8 conversion (cross-platform)
    static std::string WstringToUtf8(const std::wstring& wstr);
    static std::wstring Utf8ToWstring(const std::string& str);
    static std::wstring StringToWstring(const std::string& str);
    static std::string WstringToString(const std::wstring& wstr);
    static bool IsLikelyUtf8(const std::string& s);

    // Windows specific (declarations on all platforms)
    static bool EnablePrivilege(const std::wstring& privilegeName, bool enable = true);
    static bool CheckPrivilege(const std::wstring& privilegeName);
    static bool IsRunAsAdmin();
    static std::string FormatWindowsErrorMessage(DWORD errorCode);
    static std::string GetExecutableDirectory();
};