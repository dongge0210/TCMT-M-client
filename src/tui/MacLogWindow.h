// MacLogWindow.h — in-process native macOS log window (AppKit).
// Mirrors the Windows Win32 LogWindow: same process, reads the shared
// in-memory tcmt::LogBuffer, refreshes on a timer. No IPC, no files,
// no child process.
#pragma once

namespace tcmt {
class LogBuffer;
}

namespace tcmt {
namespace mac {

class MacLogWindow {
public:
    MacLogWindow() = default;
    ~MacLogWindow();

    // Must be called on the main thread. Returns false if the window could
    // not be created (e.g. headless session) — callers keep the TUI going.
    bool Create(LogBuffer* buffer);

    // Close the window and release resources. Safe to call multiple times.
    void Stop();

    // Run the AppKit event loop on the main thread until StopApp() is called.
    // Makes the log window fully interactive (scrolling, selection, copy).
    void Run();

    // Ask the AppKit run loop (main thread) to stop. Thread-safe.
    static void StopApp();

    bool IsActive() const { return controller_ != nullptr; }

private:
    // TCMTLogWindowController* (kept opaque so the header stays C++).
    void* controller_ = nullptr;
};

} // namespace mac
} // namespace tcmt
