#pragma once

#include "IPCData.h"
#include <optional>
#include <string>
#include <vector>
#include <map>
#include <cstdint>

#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#endif

namespace tcmt::ipc {

// C++ IPC client — mirrors C# IPCPipeClient + IPCMemoryReader.
// Connects via Unix domain socket (macOS) / NamedPipe (Windows),
// receives schema, reads from shared memory by field name.
class IPCClient {
public:
    IPCClient();
    ~IPCClient();

    bool Connect();   // Full handshake: connect → HELLO → schema → ACK → shm_open
    // In-process direct connection: use existing shm pointer + parsed schema
    // (no socket/pipe handshake, no shm_open — caller owns the memory)
    bool ConnectDirect(void* shmPtr, size_t shmSize,
                       const SchemaHeader& header,
                       const std::vector<FieldDef>& fields);
    // Close socket/pipe but keep shared memory mapped (frees server slot for other clients)
    void ClosePipe();
    void Disconnect();
    bool IsConnected() const { return connected_; }

    // Field readers — return nullopt if field not found
    std::optional<std::string> ReadString(const std::string& name);
    std::optional<double>      ReadFloat64(const std::string& name);
    std::optional<float>       ReadFloat32(const std::string& name);
    std::optional<uint64_t>    ReadUInt64(const std::string& name);
    std::optional<int32_t>     ReadInt32(const std::string& name);
    std::optional<bool>        ReadBool(const std::string& name);
    bool                       HasField(const std::string& name) const;

private:
    bool ConnectSocket();
    bool Handshake();
    bool OpenSharedMemory();

    int ReadPipe(void* buf, size_t len);
    int WritePipe(const void* buf, size_t len);

#ifdef _WIN32
    HANDLE pipeHandle_ = INVALID_HANDLE_VALUE;
    HANDLE shmHandle_  = nullptr;
#else
    int sockFd_ = -1;
#endif
    void* shmPtr_ = nullptr;
    size_t shmSize_ = 0;
    bool ownsShm_ = false;

    std::map<std::string, FieldDef> fields_;
    SchemaHeader schemaHeader_{};

    bool connected_ = false;
    std::string lastError_;
};

} // namespace tcmt::ipc
