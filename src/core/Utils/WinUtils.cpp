#include "WinUtils.h"

#ifdef TCMT_WINDOWS
// NOTE: winsock2.h must be included BEFORE windows.h
#include <winsock2.h>
#include <windows.h>
#include <sstream>
#pragma comment(lib, "advapi32.lib")

bool WinUtils::EnablePrivilege(const std::wstring& privilegeName, bool enable) {
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
        return false;
    LUID luid;
    if (!LookupPrivilegeValueW(nullptr, privilegeName.c_str(), &luid)) {
        CloseHandle(token); return false;
    }
    TOKEN_PRIVILEGES tp;
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = enable ? SE_PRIVILEGE_ENABLED : 0;
    if (!AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr)) {
        CloseHandle(token); return false;
    }
    CloseHandle(token);
    return (GetLastError() == ERROR_SUCCESS);
}

bool WinUtils::CheckPrivilege(const std::wstring& privilegeName) {
    HANDLE hToken;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) return false;
    LUID luid;
    if (!LookupPrivilegeValue(NULL, privilegeName.c_str(), &luid)) {
        CloseHandle(hToken); return false;
    }
    PRIVILEGE_SET ps;
    ps.PrivilegeCount = 1;
    ps.Control = PRIVILEGE_SET_ALL_NECESSARY;
    ps.Privilege[0].Luid = luid;
    ps.Privilege[0].Attributes = 0;
    BOOL result = FALSE;
    PrivilegeCheck(hToken, &ps, &result);
    CloseHandle(hToken);
    return result != FALSE;
}

bool WinUtils::IsRunAsAdmin() {
    BOOL isAdmin = FALSE;
    PSID adminGroupSid = NULL;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;
    if (!AllocateAndInitializeSid(&ntAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID,
        DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &adminGroupSid))
        return false;
    if (!CheckTokenMembership(NULL, adminGroupSid, &isAdmin)) isAdmin = FALSE;
    FreeSid(adminGroupSid);
    return isAdmin != FALSE;
}

std::string WinUtils::FormatWindowsErrorMessage(DWORD errorCode) {
    LPWSTR buffer = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, errorCode, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    if (!buffer) return "Unknown error: " + std::to_string(errorCode);
    std::wstring wideMsg(buffer);
    LocalFree(buffer);
    return std::string(wideMsg.begin(), wideMsg.end());
}

std::string WinUtils::GetExecutableDirectory() {
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    std::string fullPath(path);
    size_t pos = fullPath.find_last_of("\\");
    if (pos != std::string::npos) return fullPath.substr(0, pos);
    return fullPath;
}

#elif defined(TCMT_MACOS)
// ======================== macOS Implementation ========================
#include <unistd.h>
#include <mach-o/dyld.h>

bool WinUtils::EnablePrivilege(const std::wstring&, bool) { return false; }
bool WinUtils::CheckPrivilege(const std::wstring&) { return false; }
bool WinUtils::IsRunAsAdmin() { return geteuid() == 0; }
std::string WinUtils::FormatWindowsErrorMessage(DWORD) { return "N/A on macOS"; }
std::string WinUtils::GetExecutableDirectory() {
    char path[1024];
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0) {
        std::string s(path);
        size_t pos = s.find_last_of('/');
        if (pos != std::string::npos) return s.substr(0, pos);
    }
    return ".";
}

#else
// ======================== Linux / Fallback ========================
bool WinUtils::EnablePrivilege(const std::wstring&, bool) { return false; }
bool WinUtils::CheckPrivilege(const std::wstring&) { return false; }
bool WinUtils::IsRunAsAdmin() { return geteuid() == 0; }
std::string WinUtils::FormatWindowsErrorMessage(DWORD) { return "N/A"; }
std::string WinUtils::GetExecutableDirectory() { return "."; }
#endif

// ======================== Shared UTF-8 conversion (cross-platform) ========================

std::string WinUtils::WstringToUtf8(const std::wstring& wstr) {
    if (wstr.empty()) return {};
#ifdef TCMT_WINDOWS
    int size_needed = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), nullptr, 0, nullptr, nullptr);
    if (size_needed <= 0) return {};
    std::string out(size_needed, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), (int)wstr.size(), &out[0], size_needed, nullptr, nullptr);
    return out;
#elif defined(TCMT_MACOS) || defined(TCMT_LINUX)
    // Direct code-point conversion (avoids locale issues)
    std::string result;
    for (wchar_t wc : wstr) {
        if (wc <= 0x7F) {
            result.push_back(static_cast<char>(wc));
        } else if (wc <= 0x7FF) {
            result.push_back(static_cast<char>(0xC0 | (wc >> 6)));
            result.push_back(static_cast<char>(0x80 | (wc & 0x3F)));
        } else if (wc <= 0xFFFF) {
            result.push_back(static_cast<char>(0xE0 | (wc >> 12)));
            result.push_back(static_cast<char>(0x80 | ((wc >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (wc & 0x3F)));
        } else if (wc <= 0x10FFFF) {
            result.push_back(static_cast<char>(0xF0 | (wc >> 18)));
            result.push_back(static_cast<char>(0x80 | ((wc >> 12) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | ((wc >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (wc & 0x3F)));
        }
    }
    return result;
#endif
}

std::wstring WinUtils::Utf8ToWstring(const std::string& str) {
    if (str.empty()) return {};
#ifdef TCMT_WINDOWS
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), nullptr, 0);
    if (size_needed <= 0) return {};
    std::wstring out(size_needed, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &out[0], size_needed);
    return out;
#elif defined(TCMT_MACOS) || defined(TCMT_LINUX)
    std::wstring result;
    const char* ptr = str.c_str();
    size_t len = str.size();
    size_t i = 0;
    while (i < len) {
        unsigned char c = static_cast<unsigned char>(ptr[i]);
        wchar_t wc = 0;
        if ((c & 0x80) == 0) { wc = c; i += 1; }
        else if ((c & 0xE0) == 0xC0 && i + 1 < len) {
            wc = ((c & 0x1F) << 6) | (static_cast<unsigned char>(ptr[i+1]) & 0x3F); i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < len) {
            wc = ((c & 0x0F) << 12)
               | ((static_cast<unsigned char>(ptr[i+1]) & 0x3F) << 6)
               | (static_cast<unsigned char>(ptr[i+2]) & 0x3F);
            i += 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < len) {
            // 4-byte: simplified - replace with U+FFFD
            wc = 0xFFFD; i += 4;
        } else { wc = L'?'; i += 1; }
        result.push_back(wc);
    }
    return result;
#endif
}

std::wstring WinUtils::StringToWstring(const std::string& str) { return Utf8ToWstring(str); }
std::string WinUtils::WstringToString(const std::wstring& wstr) { return WstringToUtf8(wstr); }

bool WinUtils::IsLikelyUtf8(const std::string& s) {
    const unsigned char* bytes = reinterpret_cast<const unsigned char*>(s.data());
    size_t i = 0, len = s.size();
    while (i < len) {
        unsigned char c = bytes[i];
        if (c < 0x80) { ++i; continue; }
        size_t seqLen = 0;
        if ((c & 0xE0) == 0xC0) seqLen = 2;
        else if ((c & 0xF0) == 0xE0) seqLen = 3;
        else if ((c & 0xF8) == 0xF0) seqLen = 4;
        else return false;
        if (i + seqLen > len) return false;
        for (size_t k = 1; k < seqLen; ++k)
            if ((bytes[i + k] & 0xC0) != 0x80) return false;
        i += seqLen;
    }
    return true;
}
