#pragma once

#include <string>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <functional>

// ────────────────────────────────────────────────────────────────────────────
// LogPipe — dedicated log channel between the dashboard process and the
// standalone --tui-log viewer process.
//
// This is intentionally SEPARATE from the schema-driven monitoring IPC
// (IPCData / SharedMemory / NamedPipe "TCMT_IPC_Pipe"). It only carries
// formatted log lines and is invisible to Avalonia/MCP clients.
//
//   Windows: named pipe  \\.\pipe\TCMT_Log_Pipe
//   macOS/Linux: Unix domain socket /tmp/tcmt_log.sock
//
// Server side (dashboard process): Start() + WriteLine().
// Client side (--tui-log process):  StartClient(callback).
// ────────────────────────────────────────────────────────────────────────────

class LogPipe {
public:
    using OnLine = std::function<void(const std::string&)>;

    static LogPipe& Instance();

    // ── Server (dashboard) ──
    // Create the endpoint and accept the log viewer connection in background.
    bool Start();
    void Stop();
    bool IsRunning() const { return running_.load(); }

    // Non-blocking best-effort write. Lines are queued (bounded) and dropped
    // when no viewer is connected or the queue is full, so logging never blocks.
    void WriteLine(const std::string& line);

    // ── Client (--tui-log) ──
    // Connect to the dashboard's log pipe with retry in a background thread,
    // delivering each line (without trailing newline) to onLine.
    bool StartClient(OnLine onLine);

private:
    LogPipe() = default;
    ~LogPipe();
    LogPipe(const LogPipe&) = delete;
    LogPipe& operator=(const LogPipe&) = delete;

    void ServerLoop();
    void WriterLoop();
    void ClientLoop(OnLine onLine);

    std::atomic<bool> running_{false};

    // Server-side state
    void* clientHandle_ = nullptr;  // Windows: HANDLE
    int   clientFd_     = -1;       // POSIX: accepted client fd
    int   listenFd_     = -1;       // POSIX: listening fd
    std::mutex clientMutex_;

    // Write queue
    std::deque<std::string> queue_;
    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    static constexpr size_t kMaxQueuedLines = 4096;

    std::thread serverThread_;
    std::thread writerThread_;
    std::thread clientThread_;
};
