#pragma once
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include "IPCData.h"

namespace tcmt::ipc {

class IPCServer {
public:
    IPCServer();
    ~IPCServer();

    // Start the IPC server
    //   macOS: UDS listener + POSIX shm + schema broadcast
    //   Windows: NamedPipe listener + schema broadcast (reuses SharedMemoryBlock)
    bool Start();
    void Stop();
    bool IsRunning() const { return running_.load(); }

    // Shared memory access (macOS: own shm; Windows: delegates to SharedMemoryManager)
#ifdef _WIN32
    void* GetShmPtr() const { return shmPtr_; }
    size_t GetShmSize() const { return shmSize_; }
    void SetShmPtr(void* ptr, size_t sz) { shmPtr_ = ptr; shmSize_ = sz; }
#else
    void* GetShmPtr() const { return shmPtr_; }
    size_t GetShmSize() const { return shmSize_; }
#endif

    // Client tracking
    int GetClientCount();
    bool HasClients();
    std::vector<ClientType> GetClientTypes();

    // Broadcast schema to all connected clients
    void UpdateSchema(const SchemaHeader& header, const std::vector<FieldDef>& fields);
    // Broadcast config-change notification to all clients (#6 — config hot-reload)
    void NotifyConfigChange();

    std::string GetLastError() const { return lastError_; }

private:
#ifdef _WIN32
    void ServerLoop();      // Windows: NamedPipe accept loop
    void HandlePipeClient(void* hPipe);  // Windows: handle named pipe client
#else
    void AcceptLoop();      // macOS/Linux: UDS accept loop
    void HandleClient(int clientFd);     // macOS/Linux: handle UDS client
#endif
    void SendSchemaToPeer(void* peerHandle);
    std::vector<uint8_t> SerializeSchema();

    std::atomic<bool> running_{false};
    std::thread serverThread_;

    // Shared memory pointer (macOS: own shm; Windows: set by caller via SetShmPtr)
    void* shmPtr_ = nullptr;
    size_t shmSize_ = 0;

#ifdef _WIN32
    // Windows: Named Pipe
    mutable std::mutex clientsMutex_;
    struct PipeClientInfo { void* hPipe = nullptr; ClientType type = ClientType::Unknown; };
    std::vector<PipeClientInfo> clients_;
#else
    // macOS/Linux: Unix Domain Socket + POSIX shm
    int listenFd_ = -1;
    int shmFd_ = -1;
    struct ClientInfo { int fd = -1; ClientType type = ClientType::Unknown; };
    std::vector<ClientInfo> clients_;
    mutable std::mutex clientsMutex_;
#endif

    mutable std::mutex mutex_;

    // Current schema
    SchemaHeader schemaHeader_;
    std::vector<FieldDef> schemaFields_;

    std::string lastError_;
};

} // namespace tcmt::ipc
