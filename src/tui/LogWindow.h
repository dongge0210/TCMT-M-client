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
#include <vector>

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
    // Geometry + visible-window info for hit-testing and painting.
    struct LogView {
        int rowHeight = 1;
        int visibleRows = 1;
        int start = 0;        // first visible line (absolute index)
        int count = 0;        // visible line count
        int charWidth = 8;    // average character width (px)
        int maxChars = 1;     // max chars per line that fit the client width
        int total = 0;        // lines in the recent buffer window
        std::vector<std::string> lines;  // UTF-8, oldest -> newest
    };

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    LogView ComputeView(HWND hwnd);
    bool HitTest(const LogView& view, int x, int y, int& line, int& col) const;
    std::wstring BuildSelectionText(const LogView& view) const;
    std::wstring BuildAllText(const LogView& view) const;
    void CopyToClipboard(const std::wstring& text);
    void OnPaint(HWND hwnd);

    HWND hwnd_ = nullptr;
    LogBuffer* buffer_ = nullptr;
    std::thread thread_;

    std::atomic<bool> follow_{true};  // auto-scroll to newest line
    int scrollOffset_ = 0;            // lines scrolled back from newest
    size_t lastRenderCount_ = 0;      // LogBuffer size at last paint (change detection)
    HFONT font_ = nullptr;

    // Text selection (absolute line indexes into the recent buffer window).
    int selAnchorLine_ = -1;
    int selAnchorCol_ = 0;
    int selActiveLine_ = -1;
    int selActiveCol_ = 0;
    bool selecting_ = false;
};

}  // namespace tcmt
