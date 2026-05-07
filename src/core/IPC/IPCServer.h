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

    // Start the IPC server (UDS listener + schema broadcast)
    bool Start();
    void Stop();
    bool IsRunning() const { return running_.load(); }

    // Shared memory access — written by main loop, read by C# via mmap
    void* GetShmPtr() const { return shmPtr_; }
    size_t GetShmSize() const { return sizeof(IPCDataBlock); }

    // Client tracking
    int GetClientCount() const;
    bool HasClients() const;
    std::vector<ClientType> GetClientTypes() const;

    // Broadcast schema to all connected clients
    void UpdateSchema(const SchemaHeader& header, const std::vector<FieldDef>& fields);

    std::string GetLastError() const { return lastError_; }

private:
    void AcceptLoop();
    void HandleClient(int clientFd);
    void SendSchema(int fd);
    std::vector<uint8_t> SerializeSchema();

    std::atomic<bool> running_{false};
    std::thread serverThread_;
    int listenFd_ = -1;
    mutable std::mutex mutex_;

    // Shared memory
    int shmFd_ = -1;
    void* shmPtr_ = nullptr;

    // Connected clients
    struct ClientInfo { int fd = -1; ClientType type = ClientType::Unknown; };
    std::vector<ClientInfo> clients_;
    mutable std::mutex clientsMutex_;

    // Current schema
    SchemaHeader schemaHeader_;
    std::vector<FieldDef> schemaFields_;

    std::string lastError_;
};

} // namespace tcmt::ipc
