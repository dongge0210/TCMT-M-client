#pragma once
#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <functional>
#include "IPCData.h"

namespace tcmt::ipc {

class IPCServer {
public:
    IPCServer();
    ~IPCServer();

    // Start the IPC server
    //   macOS: UDS listener + POSIX shm + schema broadcast
    //   Windows: NamedPipe listener + schema broadcast (reuses IPCDataBlock SHM)
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
    int GetClientCount() const;
    bool HasClients() const;
    std::vector<ClientType> GetClientTypes() const;

    // Broadcast schema to all connected clients
    void UpdateSchema(const SchemaHeader& header, const std::vector<FieldDef>& fields);
    // Broadcast config-change notification to all clients (#6 — config hot-reload)
    void NotifyConfigChange();

    // ── 配置通道 ────────────────────────────────────────────────────────
    //
    // 服务端不认识配置本身：它只负责把两个 JSON 字符串搬来搬去。谁读、谁写、
    // 哪些键允许改，全在 main 那边定 —— 只有那儿有 ConfigManager，也只有那儿
    // 知道改完一个键之后要重启哪个探针。服务端越权去理解键名，以后加一个配置项
    // 就得改三处。
    //
    // provider: 客户端握手完成时叫一次，返回当前配置的 JSON 对象
    // handler:  收到客户端的 ConfigSet 时叫，参数是它发来的 JSON 对象
    using ConfigSnapshotProvider = std::function<std::string()>;
    using ConfigSetHandler = std::function<void(const std::string& payloadJson)>;

    void SetConfigBridge(ConfigSnapshotProvider provider, ConfigSetHandler handler);
    // 配置变了：把新快照推给所有已连接的客户端
    void BroadcastConfigSnapshot();

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

    // 把当前配置快照发给某一个对端（客户端握手完成时用）。provider 为空就什么都不发。
    void SendConfigSnapshotToPeer(void* peerHandle);
    // 读掉一条配置消息的 payload。返回空串表示读失败或超过上限。
    std::string ReadConfigPayload(void* peerHandle, uint16_t payloadSize);
    // 收到客户端 ConfigSet 时调用：交给 handler，然后由 main 决定怎么应用。
    void HandleConfigSetPayload(void* peerHandle, uint16_t payloadSize);

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

    // 配置通道的两端。见 SetConfigBridge 上面那段说明。
    ConfigSnapshotProvider configProvider_;
    ConfigSetHandler configSetHandler_;

    std::string lastError_;
};

} // namespace tcmt::ipc
