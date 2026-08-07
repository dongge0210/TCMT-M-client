#include "LogPipe.h"
#include "Logger.h"

#ifdef _WIN32
// ======================== Windows Implementation (Named Pipe) ========================
#include <windows.h>

namespace {
const wchar_t* kLogPipeName = L"\\\\.\\pipe\\TCMT_Log_Pipe";
}

LogPipe& LogPipe::Instance() {
    static LogPipe instance;
    return instance;
}

LogPipe::~LogPipe() {
    Stop();
}

bool LogPipe::Start() {
    if (running_.exchange(true)) return true;

    serverThread_ = std::thread(&LogPipe::ServerLoop, this);
    writerThread_ = std::thread(&LogPipe::WriterLoop, this);
    return true;
}

void LogPipe::Stop() {
    if (!running_.exchange(false)) return;

    // Wake a blocked ConnectNamedPipe by connecting to our own instance.
    HANDLE wake = CreateFileW(kLogPipeName, GENERIC_READ | GENERIC_WRITE, 0,
                              nullptr, OPEN_EXISTING, 0, nullptr);
    if (wake != INVALID_HANDLE_VALUE) CloseHandle(wake);

    {
        std::lock_guard<std::mutex> lock(clientMutex_);
        if (clientHandle_) {
            CancelIoEx(static_cast<HANDLE>(clientHandle_), nullptr);
            CloseHandle(static_cast<HANDLE>(clientHandle_));
            clientHandle_ = nullptr;
        }
    }
    queueCv_.notify_all();

    if (serverThread_.joinable()) serverThread_.join();
    if (writerThread_.joinable()) writerThread_.join();
}

void LogPipe::ServerLoop() {
    while (running_.load()) {
        HANDLE pipe = CreateNamedPipeW(kLogPipeName,
                                       PIPE_ACCESS_DUPLEX,
                                       PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
                                       1, 65536, 65536, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            Sleep(500);
            continue;
        }

        BOOL connected = ConnectNamedPipe(pipe, nullptr);
        if (!connected && GetLastError() != ERROR_PIPE_CONNECTED) {
            CloseHandle(pipe);
            Sleep(100);
            continue;
        }
        if (!running_.load()) {
            CloseHandle(pipe);
            break;
        }

        {
            std::lock_guard<std::mutex> lock(clientMutex_);
            if (clientHandle_) CloseHandle(static_cast<HANDLE>(clientHandle_));
            clientHandle_ = pipe;
        }
        Logger::Info("LogPipe: log viewer connected");

        // Wait until the viewer disconnects (or we stop)
        while (running_.load()) {
            Sleep(200);
            DWORD avail = 0;
            if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr)) break;
        }

        {
            std::lock_guard<std::mutex> lock(clientMutex_);
            if (clientHandle_ == pipe) clientHandle_ = nullptr;
        }
        CloseHandle(pipe);
        Logger::Info("LogPipe: log viewer disconnected");
    }
}

void LogPipe::WriterLoop() {
    while (running_.load()) {
        std::string line;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCv_.wait_for(lock, std::chrono::milliseconds(200),
                              [this] { return !queue_.empty() || !running_.load(); });
            if (queue_.empty()) continue;
            line = std::move(queue_.front());
            queue_.pop_front();
        }

        HANDLE pipe = nullptr;
        {
            std::lock_guard<std::mutex> lock(clientMutex_);
            pipe = static_cast<HANDLE>(clientHandle_);
        }
        if (!pipe) continue;  // no viewer connected — drop

        // Never block: if the viewer isn't draining and the pipe buffer would
        // overflow, drop the line instead. (Single writer, so avail is accurate.)
        DWORD avail = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr)) {
            // Pipe broken — server loop will notice and reconnect
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (avail + line.size() > 65536) continue;

        DWORD written = 0;
        WriteFile(pipe, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    }
}

void LogPipe::WriteLine(const std::string& line) {
    if (!running_.load()) return;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (queue_.size() >= kMaxQueuedLines) queue_.pop_front();
        queue_.push_back(line);
    }
    queueCv_.notify_one();
}

bool LogPipe::StartClient(OnLine onLine) {
    if (!onLine) return false;
    if (running_.exchange(true)) return true;

    clientThread_ = std::thread([this, cb = std::move(onLine)]() mutable {
        ClientLoop(std::move(cb));
    });
    return true;
}

void LogPipe::ClientLoop(OnLine onLine) {
    HANDLE pipe = INVALID_HANDLE_VALUE;

    // Connect with retry until the dashboard appears (or we quit)
    while (running_.load()) {
        pipe = CreateFileW(kLogPipeName, GENERIC_READ | GENERIC_WRITE, 0,
                           nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (!running_.load()) return;

    // Non-blocking reads so Stop() can unblock cleanly
    DWORD pipeMode = PIPE_NOWAIT;
    SetNamedPipeHandleState(pipe, &pipeMode, nullptr, nullptr);

    char buf[8192];
    std::string pending;
    DWORD n = 0;
    while (running_.load()) {
        BOOL ok = ReadFile(pipe, buf, sizeof(buf), &n, nullptr);
        if (!ok) {
            DWORD err = GetLastError();
            if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED) {
                onLine("[主进程已退出，日志通道关闭 — 按 q 退出]");
                break;
            }
            // ERROR_NO_DATA (or transient) — no data available yet
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }
        if (n == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }
        pending.append(buf, n);
        size_t pos;
        while ((pos = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, pos);
            pending.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            onLine(line);
        }
    }
    CloseHandle(pipe);
    running_.store(false);
}

#else
// ======================== POSIX stub (macOS/Linux — TODO) ========================

LogPipe& LogPipe::Instance() {
    static LogPipe instance;
    return instance;
}

LogPipe::~LogPipe() {
    Stop();
}

bool LogPipe::Start() { return false; }
void LogPipe::Stop() { running_.store(false); }
void LogPipe::WriteLine(const std::string&) {}
bool LogPipe::StartClient(OnLine) { return false; }

#endif // _WIN32
