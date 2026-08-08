// LogWindow.h — standalone log window (Win32) owned by the TCMT process.
//
// Displays the in-process Logger ring buffer in its own top-level window.
// Same process, no IPC, no log file, no child process: the window thread only
// reads the shared tcmt::LogBuffer on a refresh timer.

#pragma once

#include <windows.h>

#include <atomic>
#include <future>
#include <string>
#include <thread>

namespace tcmt {

class LogBuffer;

class LogWindow {
public:
    LogWindow() = default;
    ~LogWindow();

    LogWindow(const LogWindow&) = delete;
    LogWindow& operator=(const LogWindow&) = delete;

    // Create the window and start its message loop thread.
    bool Create(LogBuffer* buffer, const std::wstring& title = L"TCMT - Log");

    // Close the window and join the message loop thread.
    void Shutdown();

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    void OnPaint(HWND hwnd);

    HWND hwnd_ = nullptr;
    LogBuffer* buffer_ = nullptr;
    std::thread thread_;

    std::atomic<bool> follow_{true};  // auto-scroll to newest line
    int scrollOffset_ = 0;            // lines scrolled back from newest
    HFONT font_ = nullptr;
};

}  // namespace tcmt
