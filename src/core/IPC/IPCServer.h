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
    void* GetShmPtr() const { return shmPtr_; }
    size_t GetShmSize() const { return shmSize_; }

    // Client tracking
    int GetClientCount() const;
    bool HasClients() const;
    std::vector<ClientType> GetClientTypes() const;

    // Broadcast schema to all connected clients
    void UpdateSchema(const SchemaHeader& header, const std::vector<FieldDef>& fields);

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

#ifdef _WIN32
    // Windows: Named Pipe
    mutable std::mutex clientsMutex_;
    struct PipeClientInfo { void* hPipe = nullptr; ClientType type = ClientType::Unknown; };
    std::vector<PipeClientInfo> clients_;
#else
    // macOS/Linux: Unix Domain Socket + POSIX shm
    int listenFd_ = -1;
    int shmFd_ = -1;
    void* shmPtr_ = nullptr;
    size_t shmSize_ = 0;
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
