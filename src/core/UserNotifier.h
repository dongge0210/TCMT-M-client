#pragma once
#include <string>

class UserNotifier {
public:
    // Show a desktop notification. Returns true if notification was sent.
    // On macOS: uses osascript to display notification center banner
    // On Windows: uses WTSSendMessage to show message box in user session
    static bool ShowNotification(const std::string& title, const std::string& message);

    // Check if the current platform supports notifications
    static bool IsAvailable();

private:
    static bool ShowMacOS(const std::string& title, const std::string& message);
    static bool ShowWindows(const std::string& title, const std::string& message);
};
