#pragma once
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include "IPCData.h"

// Cross-platform: use void* for Windows HANDLE in public interface
typedef void* PipeHandle;

namespace tcmt::ipc {

class NamedPipeServer {
public:
    NamedPipeServer();
    ~NamedPipeServer();

    bool Start();
    void Stop();
    bool IsRunning() const { return running_.load(); }

    void UpdateSchema(const SchemaHeader& header, const std::vector<FieldDef>& fields);

    std::string LastError() const { return lastError_; }

private:
    void ServerLoop();
    void SendSchema(PipeHandle pipe);
    std::vector<uint8_t> SerializeSchema();

    std::atomic<bool> running_{false};
    std::thread serverThread_;
    mutable std::mutex mutex_;

    std::vector<PipeHandle> clientPipes_;
    std::mutex clientsMutex_;

    SchemaHeader schemaHeader_;
    std::vector<FieldDef> schemaFields_;

    std::string lastError_;
};

} // namespace tcmt::ipc
