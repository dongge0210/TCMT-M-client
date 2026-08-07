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
// ======================== POSIX Implementation (Unix Domain Socket) ========================
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cerrno>
#include <cstring>

namespace {
const char* kLogSocketPath = "/tmp/tcmt_log.sock";

// Suppress SIGPIPE on send (Linux: MSG_NOSIGNAL; macOS: SO_NOSIGPIPE).
void SuppressSigpipe(int fd) {
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
}
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

    ::unlink(kLogSocketPath);
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        running_.store(false);
        return false;
    }
    listenFd_ = fd;

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, kLogSocketPath, sizeof(addr.sun_path) - 1);
    if (::bind(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(fd, 4) != 0) {
        ::close(fd);
        listenFd_ = -1;
        running_.store(false);
        return false;
    }
    ::chmod(kLogSocketPath, 0666);

    serverThread_ = std::thread(&LogPipe::ServerLoop, this);
    writerThread_ = std::thread(&LogPipe::WriterLoop, this);
    return true;
}

void LogPipe::Stop() {
    if (!running_.exchange(false)) return;

    if (listenFd_ >= 0) {
        ::shutdown(listenFd_, SHUT_RDWR);
        ::close(listenFd_);
        listenFd_ = -1;
    }
    {
        std::lock_guard<std::mutex> lock(clientMutex_);
        if (clientFd_ >= 0) {
            ::shutdown(clientFd_, SHUT_RDWR);
            ::close(clientFd_);
            clientFd_ = -1;
        }
    }
    queueCv_.notify_all();

    if (serverThread_.joinable()) serverThread_.join();
    if (writerThread_.joinable()) writerThread_.join();
    ::unlink(kLogSocketPath);
}

void LogPipe::ServerLoop() {
    while (running_.load()) {
        struct pollfd pfd{listenFd_, POLLIN, 0};
        int ret = ::poll(&pfd, 1, 200);
        if (ret <= 0) continue;

        int fd = ::accept(listenFd_, nullptr, nullptr);
        if (fd < 0) {
            if (errno != EINTR)
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        if (!running_.load()) {
            ::close(fd);
            break;
        }
        SuppressSigpipe(fd);

        {
            std::lock_guard<std::mutex> lock(clientMutex_);
            if (clientFd_ >= 0) ::close(clientFd_);
            clientFd_ = fd;
        }
        Logger::Info("LogPipe: log viewer connected");

        // Wait until the viewer disconnects (or we stop)
        while (running_.load()) {
            struct pollfd p{fd, POLLIN, 0};
            int r = ::poll(&p, 1, 200);
            if (r <= 0) continue;
            if (p.revents & (POLLERR | POLLNVAL)) break;
            char c;
            ssize_t n = ::recv(fd, &c, 1, MSG_PEEK);
            if (n == 0) break;                        // peer closed
            if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) break;
        }

        {
            std::lock_guard<std::mutex> lock(clientMutex_);
            if (clientFd_ == fd) clientFd_ = -1;
        }
        ::close(fd);
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

        int fd = -1;
        {
            std::lock_guard<std::mutex> lock(clientMutex_);
            fd = clientFd_;
        }
        if (fd < 0) continue;  // no viewer connected — drop

        // Non-blocking best effort: drop the line if the socket would block.
        size_t off = 0;
        while (off < line.size()) {
            struct pollfd p{fd, POLLOUT, 0};
            if (::poll(&p, 1, 0) <= 0) break;
            ssize_t n = ::send(fd, line.data() + off, line.size() - off,
#ifdef MSG_NOSIGNAL
                               MSG_NOSIGNAL
#else
                               0
#endif
            );
            if (n > 0) { off += static_cast<size_t>(n); continue; }
            break;
        }
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
    int fd = -1;

    // Connect with retry until the dashboard appears (or we quit)
    while (running_.load()) {
        fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd >= 0) {
            struct sockaddr_un addr{};
            addr.sun_family = AF_UNIX;
            std::strncpy(addr.sun_path, kLogSocketPath, sizeof(addr.sun_path) - 1);
            if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) == 0)
                break;
            ::close(fd);
            fd = -1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (!running_.load()) {
        if (fd >= 0) ::close(fd);
        return;
    }

    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    SuppressSigpipe(fd);

    char buf[8192];
    std::string pending;
    while (running_.load()) {
        struct pollfd p{fd, POLLIN, 0};
        int r = ::poll(&p, 1, 200);
        if (r <= 0) continue;
        if (p.revents & (POLLERR | POLLNVAL)) break;
        if (p.revents & POLLHUP) {
            onLine("[主进程已退出，日志通道关闭 — 按 q 退出]");
            break;
        }

        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n == 0) {
            onLine("[主进程已退出，日志通道关闭 — 按 q 退出]");
            break;
        }
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            break;
        }
        pending.append(buf, static_cast<size_t>(n));
        size_t pos;
        while ((pos = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, pos);
            pending.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            onLine(line);
        }
    }
    if (fd >= 0) ::close(fd);
    running_.store(false);
}

#endif // _WIN32
