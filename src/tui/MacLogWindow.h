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

    bool IsActive() const { return controller_ != nullptr; }

    // Run the main run loop for a short slice so the log window can refresh
    // and process events. Call periodically from the main thread's loop.
    static void PumpRunLoop(double seconds);

private:
    // TCMTLogWindowController* (kept opaque so the header stays C++).
    void* controller_ = nullptr;
};

} // namespace mac
} // namespace tcmt
