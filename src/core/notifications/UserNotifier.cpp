#include "UserNotifier.h"
#include "Utils/Logger.h"

// Platform detection (mirrors project convention in DeviceChangeNotifier.h)
#if !defined(TCMT_WINDOWS) && !defined(TCMT_MACOS) && !defined(TCMT_LINUX)
    #if defined(_WIN32) || defined(_WIN64)
        #define TCMT_WINDOWS
    #elif defined(__APPLE__) && defined(__MACH__)
        #define TCMT_MACOS
    #endif
#endif

#ifdef TCMT_WINDOWS
#include <winsock2.h>
#include <windows.h>
#include <wtsapi32.h>
#pragma comment(lib, "wtsapi32.lib")
#endif

#ifdef TCMT_MACOS
#include <cstdio>   // popen, pclose
#endif

// ====================================================================
// Public API
// ====================================================================

bool UserNotifier::ShowNotification(const std::string& title, const std::string& message)
{
#ifdef TCMT_MACOS
    return ShowMacOS(title, message);
#elif defined(TCMT_WINDOWS)
    return ShowWindows(title, message);
#else
    (void)title;
    (void)message;
    Logger::Warn("UserNotifier::ShowNotification: notifications not supported on this platform");
    return false;
#endif
}

bool UserNotifier::IsAvailable()
{
#ifdef TCMT_MACOS
    return true;
#elif defined(TCMT_WINDOWS)
    return true;
#else
    return false;
#endif
}

// ====================================================================
// macOS — osascript Notification Center banner
// ====================================================================

#ifdef TCMT_MACOS

bool UserNotifier::ShowMacOS(const std::string& title, const std::string& message)
{
    // Escape for AppleScript: backslash and double-quote must be escaped
    auto escapeForAS = [](const std::string& s) -> std::string {
        std::string result;
        result.reserve(s.size() + 8);
        for (char c : s) {
            switch (c) {
                case '\\': result += "\\\\"; break;
                case '"':  result += "\\\""; break;
                default:   result += c;       break;
            }
        }
        return result;
    };

    // Escape for shell: single-quote inside a single-quoted string
    // must end the quote, add escaped quote, and restart
    auto escapeForSh = [](const std::string& s) -> std::string {
        std::string result;
        result.reserve(s.size() + 8);
        for (char c : s) {
            if (c == '\'')
                result += "'\\''";
            else
                result += c;
        }
        return result;
    };

    std::string safeTitle = escapeForSh(escapeForAS(title));
    std::string safeMsg   = escapeForSh(escapeForAS(message));

    // osascript -e 'display notification "MESSAGE" with title "TITLE"'
    std::string cmd = "osascript -e 'display notification \"";
    cmd += safeMsg;
    cmd += "\" with title \"";
    cmd += safeTitle;
    cmd += "\"'";

    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        Logger::Error("UserNotifier: failed to launch osascript");
        return false;
    }

    int status = pclose(pipe);
    if (status != 0) {
        Logger::Warn("UserNotifier: osascript exited with status " + std::to_string(status));
        return false;
    }

    Logger::Debug("UserNotifier: macOS notification sent: '" + title + "'");
    return true;
}

#endif // TCMT_MACOS

// ====================================================================
// Windows — WTSSendMessage with MessageBox fallback
// ====================================================================

#ifdef TCMT_WINDOWS

bool UserNotifier::ShowWindows(const std::string& title, const std::string& message)
{
    // Convert UTF-8 strings to wide strings for the Windows API
    int titleLen = MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, nullptr, 0);
    int msgLen   = MultiByteToWideChar(CP_UTF8, 0, message.c_str(), -1, nullptr, 0);

    std::wstring wTitle(static_cast<size_t>(titleLen), L'\0');
    std::wstring wMsg(static_cast<size_t>(msgLen), L'\0');

    MultiByteToWideChar(CP_UTF8, 0, title.c_str(), -1, &wTitle[0], titleLen);
    MultiByteToWideChar(CP_UTF8, 0, message.c_str(), -1, &wMsg[0], msgLen);

    // Try WTSSendMessage first (shows in the active user session)
    DWORD response = 0;
    BOOL wtsResult = WTSSendMessageW(
        WTS_CURRENT_SERVER_HANDLE,
        WTS_CURRENT_SESSION,
        &wTitle[0],
        static_cast<DWORD>((wTitle.size()) * sizeof(wchar_t)),
        &wMsg[0],
        static_cast<DWORD>((wMsg.size()) * sizeof(wchar_t)),
        MB_OK | MB_ICONINFORMATION,
        10 * 1000,  // 10-second timeout
        &response,
        FALSE       // non-blocking (fire and forget)
    );

    if (wtsResult) {
        Logger::Debug("UserNotifier: WTS notification sent: '" + title + "'");
        return true;
    }

    // Fallback: show a plain message box in the current process
    Logger::Warn("UserNotifier: WTSSendMessage failed, falling back to MessageBox");

    MessageBoxW(nullptr,
                wMsg.c_str(),
                wTitle.c_str(),
                MB_OK | MB_ICONINFORMATION);

    Logger::Debug("UserNotifier: MessageBox fallback sent: '" + title + "'");
    return true;
}

#endif // TCMT_WINDOWS
