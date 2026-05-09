#include "IPCServer.h"
#include "../Utils/Logger.h"

#ifdef _WIN32
// ======================== Windows Implementation (Named Pipe) ========================
#include <windows.h>
#include <cstring>
#include <algorithm>

namespace tcmt::ipc {

IPCServer::IPCServer() = default;

IPCServer::~IPCServer() {
    Stop();
}

bool IPCServer::Start() {
#ifdef _WIN32
    running_ = true;
    serverThread_ = std::thread(&IPCServer::ServerLoop, this);
    return true;
#else
    lastError_ = "IPCServer: platform mismatch";
    return false;
#endif
}

void IPCServer::Stop() {
    running_ = false;

#ifdef _WIN32
    {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        for (auto& c : clients_) {
            if (c.hPipe) {
                CancelIoEx(static_cast<HANDLE>(c.hPipe), nullptr);
                CloseHandle(static_cast<HANDLE>(c.hPipe));
            }
        }
        clients_.clear();
    }
#endif

    if (serverThread_.joinable())
        serverThread_.join();

#ifndef _WIN32
    // macOS/Linux cleanup
    {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        PipeMessage shutdown{};
        shutdown.type = static_cast<uint8_t>(PipeMsgType::Shutdown);
        for (auto& c : clients_) {
            write(c.fd, &shutdown, PIPE_MSG_HEADER_SIZE);
        }
        for (auto& c : clients_) close(c.fd);
        clients_.clear();
    }

    if (listenFd_ != -1) { close(listenFd_); listenFd_ = -1; }
    if (shmPtr_ && shmPtr_ != MAP_FAILED) { munmap(shmPtr_, shmSize_); shmPtr_ = nullptr; }
    if (shmFd_ != -1) { close(shmFd_); shmFd_ = -1; }
    unlink(IPC_SOCK_PATH);
#endif
}

// ── Windows: Named Pipe server loop ──
// Accept loop → spawns detached thread per client for handshake + keep-alive.
// PeekNamedPipe + Sleep prevents CPU spin; PIPE_UNLIMITED_INSTANCES allows concurrency.
void IPCServer::ServerLoop() {
#ifdef _WIN32
    const char* pipeName = "\\\\.\\pipe\\TCMT_IPC_Pipe";

    while (running_) {
        HANDLE hPipe = CreateNamedPipeA(
            pipeName,
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES,
            4096, 4096, 0, nullptr);

        if (hPipe == INVALID_HANDLE_VALUE) {
            lastError_ = "CreateNamedPipe failed";
            continue;
        }

        BOOL connected = ConnectNamedPipe(hPipe, nullptr);
        if (!connected) {
            DWORD gle = ::GetLastError();
            connected = (gle == ERROR_PIPE_CONNECTED) ? TRUE : FALSE;
        }

        if (!connected) {
            CloseHandle(hPipe);
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            clients_.push_back({hPipe, ClientType::Unknown});
        }

        std::thread(&IPCServer::HandlePipeClient, this, hPipe).detach();
    }
#endif
}

// ── Windows: Handle a single named pipe client (handshake + keep-alive) ──
// Uses PeekNamedPipe + Sleep to avoid spinning on byte-mode pipes.
void IPCServer::HandlePipeClient(void* hPipe) {
#ifdef _WIN32
    HANDLE h = static_cast<HANDLE>(hPipe);
    PipeMessage msg;
    DWORD n = 0;

    // --- Phase 1: Wait for HELLO (blocking ReadFile, PIPE_WAIT mode) ---
    if (!ReadFile(h, &msg, PIPE_MSG_HEADER_SIZE, &n, nullptr) || n < PIPE_MSG_HEADER_SIZE ||
        msg.type != static_cast<uint8_t>(PipeMsgType::Hello)) {
        Logger::Debug("IPC: pipe HELLO failed, err=" + std::to_string(::GetLastError()));
        goto cleanup;
    }

    // Read client type from payload
    {
        ClientType ct = ClientType::Unknown;
        if (msg.payloadSize >= 1) {
            uint8_t tb = 0; DWORD tr = 0;
            ReadFile(h, &tb, 1, &tr, nullptr);
            if (tb <= 2) ct = static_cast<ClientType>(tb);
        }
        std::lock_guard<std::mutex> lock(clientsMutex_);
        for (auto& c : clients_) {
            if (c.hPipe == hPipe) { c.type = ct; break; }
        }
        const char* ts = ct == ClientType::Avalonia ? "Avalonia" : ct == ClientType::MCP ? "MCP" : "Unknown";
        Logger::Info(std::string("IPC: ") + ts + " client connected (pipe), " +
                     std::to_string(GetClientCount()) + " client(s) total");
    }

    // --- Phase 2: Send HELLO_ACK + schema ---
    {
        PipeMessage ack{};
        ack.type = static_cast<uint8_t>(PipeMsgType::HelloAck);
        ack.version = IPC_VERSION;
        WriteFile(h, &ack, PIPE_MSG_HEADER_SIZE, &n, nullptr);
    }
    SendSchemaToPeer(h);

    // --- Phase 3: Wait for ACK (blocking ReadFile) ---
    if (!ReadFile(h, &msg, PIPE_MSG_HEADER_SIZE, &n, nullptr) || n < PIPE_MSG_HEADER_SIZE ||
        msg.type != static_cast<uint8_t>(PipeMsgType::Ack)) {
        Logger::Debug("IPC: pipe ACK failed, err=" + std::to_string(::GetLastError()));
        goto cleanup;
    }

    // --- Phase 4: Keep-alive loop ---
    while (running_) {
        DWORD avail = 0;
        if (!PeekNamedPipe(h, nullptr, 0, nullptr, &avail, nullptr)) break; // pipe broken
        if (avail >= PIPE_MSG_HEADER_SIZE) {
            if (!ReadFile(h, &msg, PIPE_MSG_HEADER_SIZE, &n, nullptr) || n < PIPE_MSG_HEADER_SIZE)
                break;
            if (msg.type == static_cast<uint8_t>(PipeMsgType::Ping)) {
                PipeMessage pong{};
                pong.type = static_cast<uint8_t>(PipeMsgType::Pong);
                DWORD w = 0;
                WriteFile(h, &pong, PIPE_MSG_HEADER_SIZE, &w, nullptr);
            } else if (msg.type == static_cast<uint8_t>(PipeMsgType::Bye)) {
                break;
            }
        }
        Sleep(200);
    }

cleanup:
    CloseHandle(h);
    {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        clients_.erase(
            std::remove_if(clients_.begin(), clients_.end(),
                [hPipe](const PipeClientInfo& c) { return c.hPipe == hPipe; }),
            clients_.end());
    }
    Logger::Info("IPC: client disconnected (pipe), " + std::to_string(GetClientCount()) + " client(s) total");
#endif
}


void IPCServer::SendSchemaToPeer(void* peerHandle) {
    auto data = SerializeSchema();
    if (data.empty()) return;
#ifdef _WIN32
    DWORD written = 0;
    WriteFile(static_cast<HANDLE>(peerHandle), data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
#else
    write(static_cast<int>(reinterpret_cast<intptr_t>(peerHandle)), data.data(), data.size());
#endif
}

std::vector<uint8_t> IPCServer::SerializeSchema() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<uint8_t> buf;
    buf.resize(IPC_SCHEMA_HEADER_SIZE + schemaFields_.size() * IPC_FIELD_DEF_SIZE);

    schemaHeader_.fieldCount = static_cast<uint16_t>(schemaFields_.size());
    std::memcpy(buf.data(), &schemaHeader_, IPC_SCHEMA_HEADER_SIZE);

    for (size_t i = 0; i < schemaFields_.size(); ++i) {
        std::memcpy(buf.data() + IPC_SCHEMA_HEADER_SIZE + i * IPC_FIELD_DEF_SIZE,
                    &schemaFields_[i], IPC_FIELD_DEF_SIZE);
    }
    return buf;
}

void IPCServer::UpdateSchema(const SchemaHeader& header, const std::vector<FieldDef>& fields) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        schemaHeader_ = header;
        schemaFields_ = fields;
    }

    auto data = SerializeSchema();

    // Broadcast to all connected clients
    std::lock_guard<std::mutex> lock(clientsMutex_);
#ifdef _WIN32
    for (auto it = clients_.begin(); it != clients_.end(); ) {
        DWORD written = 0;
        if (!WriteFile(static_cast<HANDLE>(it->hPipe), data.data(), static_cast<DWORD>(data.size()), &written, nullptr)) {
            CloseHandle(static_cast<HANDLE>(it->hPipe));
            it = clients_.erase(it);
        } else {
            ++it;
        }
    }
#else
    for (auto it = clients_.begin(); it != clients_.end(); ) {
        int n = write(it->fd, data.data(), data.size());
        if (n <= 0) { close(it->fd); it = clients_.erase(it); } else { ++it; }
    }
#endif
}

int IPCServer::GetClientCount() const {
    std::lock_guard<std::mutex> lock(clientsMutex_);
    return static_cast<int>(clients_.size());
}

bool IPCServer::HasClients() const { return GetClientCount() > 0; }

std::vector<ClientType> IPCServer::GetClientTypes() const {
    std::lock_guard<std::mutex> lock(clientsMutex_);
    std::vector<ClientType> types;
    for (const auto& c : clients_)
        types.push_back(c.type);
    return types;
}

} // namespace tcmt::ipc

#else // !defined(_WIN32)
// ======================== macOS/Linux Implementation (UDS + POSIX shm) ========================
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cerrno>
#include <algorithm>
#include <poll.h>

namespace tcmt::ipc {

IPCServer::IPCServer() = default;

IPCServer::~IPCServer() {
    Stop();
}

bool IPCServer::Start() {
    // 1. Create shared memory file + mmap
    shm_unlink(IPC_SHM_PATH);
    shmFd_ = shm_open(IPC_SHM_PATH, O_CREAT | O_RDWR, 0666);
    if (shmFd_ == -1) {
        lastError_ = "shm_open failed: " + std::string(std::strerror(errno));
        return false;
    }
    if (ftruncate(shmFd_, sizeof(IPCDataBlock)) == -1) {
        lastError_ = "ftruncate failed: " + std::string(std::strerror(errno));
        close(shmFd_); shmFd_ = -1;
        return false;
    }
    shmPtr_ = mmap(nullptr, sizeof(IPCDataBlock), PROT_READ | PROT_WRITE, MAP_SHARED, shmFd_, 0);
    if (shmPtr_ == MAP_FAILED) {
        lastError_ = "mmap failed: " + std::string(std::strerror(errno));
        close(shmFd_); shmFd_ = -1;
        return false;
    }
    shmSize_ = sizeof(IPCDataBlock);
    std::memset(shmPtr_, 0, sizeof(IPCDataBlock));

    // 2. Create UDS
    listenFd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listenFd_ == -1) {
        lastError_ = "socket failed: " + std::string(std::strerror(errno));
        return false;
    }

    struct sockaddr_un addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, IPC_SOCK_PATH, sizeof(addr.sun_path) - 1);
    unlink(IPC_SOCK_PATH);

    if (bind(listenFd_, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        lastError_ = "bind failed: " + std::string(std::strerror(errno));
        close(listenFd_); listenFd_ = -1;
        return false;
    }

    if (listen(listenFd_, IPC_MAX_CLIENTS) == -1) {
        lastError_ = "listen failed: " + std::string(std::strerror(errno));
        close(listenFd_); listenFd_ = -1;
        return false;
    }

    // 3. Start accept thread
    running_ = true;
    serverThread_ = std::thread(&IPCServer::AcceptLoop, this);

    return true;
}

void IPCServer::Stop() {
    running_ = false;

    // Notify all clients of shutdown
    {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        PipeMessage shutdown{};
        shutdown.type = static_cast<uint8_t>(PipeMsgType::Shutdown);
        for (auto& c : clients_) {
            write(c.fd, &shutdown, PIPE_MSG_HEADER_SIZE);
        }
    }

    if (serverThread_.joinable())
        serverThread_.join();

    std::lock_guard<std::mutex> lock(clientsMutex_);
    for (auto& c : clients_) close(c.fd);
    clients_.clear();

    if (listenFd_ != -1) { close(listenFd_); listenFd_ = -1; }
    if (shmPtr_ && shmPtr_ != MAP_FAILED) { munmap(shmPtr_, shmSize_); shmPtr_ = nullptr; }
    if (shmFd_ != -1) { close(shmFd_); shmFd_ = -1; }
    unlink(IPC_SOCK_PATH);
}

void IPCServer::AcceptLoop() {
    while (running_) {
        struct pollfd pfd = {listenFd_, POLLIN, 0};
        int ret = poll(&pfd, 1, 500);
        if (ret <= 0) continue;

        struct sockaddr_un clientAddr;
        socklen_t clientLen = sizeof(clientAddr);
        int clientFd = accept(listenFd_, (struct sockaddr*)&clientAddr, &clientLen);
        if (clientFd == -1) continue;

        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            clients_.push_back({clientFd, ClientType::Unknown});
        }
        // Spawn thread per client — supports MCP + Avalonia concurrently
        std::thread(&IPCServer::HandleClient, this, clientFd).detach();
    }
}

void IPCServer::HandleClient(int clientFd) {
    // Protocol: wait for HELLO → send HELLO_ACK + SCHEMA → wait for ACK → keep-alive loop
    PipeMessage msg;
    ssize_t n = read(clientFd, &msg, PIPE_MSG_HEADER_SIZE);
    if (n <= 0 || msg.type != static_cast<uint8_t>(PipeMsgType::Hello)) {
        Logger::Debug("IPC: client sent invalid HELLO, closing");
        close(clientFd);
        return;
    }

    // Read client type from HELLO payload (1 byte)
    ClientType clientType = ClientType::Unknown;
    if (msg.payloadSize >= 1) {
        uint8_t typeByte = 0;
        read(clientFd, &typeByte, 1);
        if (typeByte <= 2) clientType = static_cast<ClientType>(typeByte);
    }

    // Store client type
    {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        for (auto& c : clients_) {
            if (c.fd == clientFd) { c.type = clientType; break; }
        }
    }

    const char* typeStr = clientType == ClientType::Avalonia ? "Avalonia" :
                           clientType == ClientType::MCP ? "MCP" : "Unknown";
    Logger::Info(std::string("IPC: ") + typeStr + " client connected, " +
                 std::to_string(GetClientCount()) + " client(s) total");

    // Send HELLO ACK
    PipeMessage ack{};
    ack.type = static_cast<uint8_t>(PipeMsgType::HelloAck);
    ack.version = IPC_VERSION;
    write(clientFd, &ack, PIPE_MSG_HEADER_SIZE);

    // Send schema
    SendSchemaToPeer(reinterpret_cast<void*>(static_cast<intptr_t>(clientFd)));

    // Wait for client ACK (or BYE)
    n = read(clientFd, &msg, PIPE_MSG_HEADER_SIZE);
    if (n <= 0 || msg.type != static_cast<uint8_t>(PipeMsgType::Ack)) {
        Logger::Debug("IPC: client didn't ACK schema, closing");
        close(clientFd);
        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            auto it = std::find_if(clients_.begin(), clients_.end(),
                [clientFd](const ClientInfo& c) { return c.fd == clientFd; });
            if (it != clients_.end()) clients_.erase(it);
        }
        return;
    }

    // Keep-alive loop: handle PING/PONG/BYE with non-blocking poll
    while (running_) {
        struct pollfd pfd = {clientFd, POLLIN, 0};
        int ret = poll(&pfd, 1, 1000);
        if (ret < 0) break;
        if (ret == 0) continue;

        n = read(clientFd, &msg, PIPE_MSG_HEADER_SIZE);
        if (n <= 0) { Logger::Debug("IPC: client pipe closed"); break; }
        if (msg.type == static_cast<uint8_t>(PipeMsgType::Ping)) {
            PipeMessage pong{};
            pong.type = static_cast<uint8_t>(PipeMsgType::Pong);
            write(clientFd, &pong, PIPE_MSG_HEADER_SIZE);
        } else if (msg.type == static_cast<uint8_t>(PipeMsgType::Bye)) {
            Logger::Info("IPC: client sent BYE, disconnecting");
            break;
        }
    }

    // Cleanup
    close(clientFd);
    {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        auto it = std::find_if(clients_.begin(), clients_.end(),
            [clientFd](const ClientInfo& c) { return c.fd == clientFd; });
        if (it != clients_.end()) clients_.erase(it);
    }
    Logger::Info("IPC: client disconnected, " + std::to_string(GetClientCount()) + " client(s) total");
}

int IPCServer::GetClientCount() const {
    std::lock_guard<std::mutex> lock(clientsMutex_);
    return static_cast<int>(clients_.size());
}

bool IPCServer::HasClients() const {
    return GetClientCount() > 0;
}

std::vector<ClientType> IPCServer::GetClientTypes() const {
    std::lock_guard<std::mutex> lock(clientsMutex_);
    std::vector<ClientType> types;
    for (const auto& c : clients_)
        types.push_back(c.type);
    return types;
}

void IPCServer::SendSchemaToPeer(void* peerHandle) {
    auto data = SerializeSchema();
    if (data.empty()) return;
    int fd = static_cast<int>(reinterpret_cast<intptr_t>(peerHandle));
    write(fd, data.data(), data.size());
}

std::vector<uint8_t> IPCServer::SerializeSchema() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<uint8_t> buf;
    buf.resize(IPC_SCHEMA_HEADER_SIZE + schemaFields_.size() * IPC_FIELD_DEF_SIZE);

    schemaHeader_.fieldCount = static_cast<uint16_t>(schemaFields_.size());
    std::memcpy(buf.data(), &schemaHeader_, IPC_SCHEMA_HEADER_SIZE);

    for (size_t i = 0; i < schemaFields_.size(); ++i) {
        std::memcpy(buf.data() + IPC_SCHEMA_HEADER_SIZE + i * IPC_FIELD_DEF_SIZE,
                    &schemaFields_[i], IPC_FIELD_DEF_SIZE);
    }
    return buf;
}

void IPCServer::UpdateSchema(const SchemaHeader& header, const std::vector<FieldDef>& fields) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        schemaHeader_ = header;
        schemaFields_ = fields;
    }

    auto data = SerializeSchema();

    // Broadcast to all connected clients
    std::lock_guard<std::mutex> lock(clientsMutex_);
    for (auto it = clients_.begin(); it != clients_.end(); ) {
        int n = write(it->fd, data.data(), data.size());
        if (n <= 0) { close(it->fd); it = clients_.erase(it); } else { ++it; }
    }
}

} // namespace tcmt::ipc

#endif // _WIN32
