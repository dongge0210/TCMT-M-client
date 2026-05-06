#include "IPCClient.h"
#include "../Utils/Logger.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <cerrno>
#include <algorithm>
#include <optional>

namespace tcmt::ipc {

IPCClient::IPCClient()  = default;
IPCClient::~IPCClient() { Disconnect(); }

void IPCClient::Disconnect() {
    if (shmPtr_ && shmPtr_ != MAP_FAILED) { munmap(shmPtr_, shmSize_); shmPtr_ = nullptr; }
    if (sockFd_ != -1) { close(sockFd_); sockFd_ = -1; }
    connected_ = false;
}

// === Connect ===
bool IPCClient::Connect() {
    Disconnect();
    if (!ConnectSocket()) return false;
    if (!Handshake()) { Disconnect(); return false; }
    if (!OpenSharedMemory()) { Disconnect(); return false; }
    connected_ = true;
    return true;
}

bool IPCClient::ConnectSocket() {
    sockFd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockFd_ == -1) { lastError_ = "socket() failed"; return false; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, IPC_SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (connect(sockFd_, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        lastError_ = std::string("connect() failed: ") + strerror(errno);
        close(sockFd_); sockFd_ = -1;
        return false;
    }
    return true;
}

bool IPCClient::Handshake() {
    // 1. Send HELLO
    PipeMessage hello;
    hello.type    = static_cast<uint8_t>(PipeMsgType::Hello);
    hello.version = IPC_VERSION;
    if (write(sockFd_, &hello, PIPE_MSG_HEADER_SIZE) != (ssize_t)PIPE_MSG_HEADER_SIZE) {
        lastError_ = "HELLO write failed";
        return false;
    }

    // 2. Read HELLO_ACK
    PipeMessage msg;
    ssize_t n = read(sockFd_, &msg, PIPE_MSG_HEADER_SIZE);
    if (n != (ssize_t)PIPE_MSG_HEADER_SIZE || msg.type != static_cast<uint8_t>(PipeMsgType::HelloAck)) {
        lastError_ = "HELLO_ACK not received";
        return false;
    }

    // 3. Read SCHEMA
    SchemaHeader schemaHeader;
    n = read(sockFd_, &schemaHeader, IPC_SCHEMA_HEADER_SIZE);
    if (n != (ssize_t)IPC_SCHEMA_HEADER_SIZE || schemaHeader.magic != IPC_MAGIC) {
        lastError_ = "Schema header invalid";
        return false;
    }

    int fieldCount = schemaHeader.fieldCount;
    if (fieldCount > (int)IPC_MAX_FIELDS) fieldCount = IPC_MAX_FIELDS;
    std::vector<FieldDef> fieldBuf(fieldCount);
    size_t fieldBytes = fieldCount * IPC_FIELD_DEF_SIZE;
    size_t total = 0;
    while (total < fieldBytes) {
        n = read(sockFd_, reinterpret_cast<char*>(fieldBuf.data()) + total, fieldBytes - total);
        if (n <= 0) { lastError_ = "Schema fields read failed"; return false; }
        total += n;
    }

    schemaHeader_ = schemaHeader;
    fields_.clear();
    for (int i = 0; i < fieldCount; ++i) {
        fields_[fieldBuf[i].name] = fieldBuf[i];
    }
    Logger::Debug("IPCClient: schema received, " + std::to_string(fields_.size()) + " fields, totalSize=" + std::to_string(schemaHeader.totalSize));

    // 4. Send ACK
    PipeMessage ack;
    ack.type    = static_cast<uint8_t>(PipeMsgType::Ack);
    ack.version = IPC_VERSION;
    if (write(sockFd_, &ack, PIPE_MSG_HEADER_SIZE) != (ssize_t)PIPE_MSG_HEADER_SIZE) {
        lastError_ = "ACK write failed";
        return false;
    }

    return true;
}

bool IPCClient::OpenSharedMemory() {
    int fd = shm_open("/tcmt_ipc_shm", O_RDONLY, 0);
    if (fd == -1) {
        lastError_ = "shm_open failed";
        return false;
    }
    shmSize_ = schemaHeader_.totalSize;
    shmPtr_ = mmap(nullptr, shmSize_, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (shmPtr_ == MAP_FAILED) { shmPtr_ = nullptr; lastError_ = "mmap failed"; return false; }
    return true;
}

// === Field readers ===
bool IPCClient::HasField(const std::string& name) const {
    return fields_.find(name) != fields_.end();
}

static const FieldDef* Find(const std::map<std::string, FieldDef>& fields, const std::string& name) {
    auto it = fields.find(name);
    return it != fields.end() ? &it->second : nullptr;
}

std::optional<std::string> IPCClient::ReadString(const std::string& name) {
    if (!shmPtr_) return std::nullopt;
    auto* f = Find(fields_, name); if (!f || f->size == 0) return std::nullopt;
    size_t maxLen = std::min((size_t)f->size, shmSize_ - f->offset);
    if (maxLen == 0) return std::nullopt;
    const char* src = static_cast<const char*>(shmPtr_) + f->offset;
    size_t len = 0;
    while (len < maxLen && src[len] != '\0') ++len;
    if (len == 0) return std::string();
    return std::string(src, len);
}

// Generic size-aware numeric reader — zero-extends smaller fields to the target type
template<typename T> static std::optional<T> ReadNumeric(const FieldDef* f, const void* shmPtr, size_t shmSize) {
    if (!f || f->offset + f->size > shmSize) return std::nullopt;
    T val = 0;
    memcpy(&val, static_cast<const char*>(shmPtr) + f->offset, std::min(sizeof(T), (size_t)f->size));
    return val;
}

std::optional<double> IPCClient::ReadFloat64(const std::string& name) {
    auto* f = Find(fields_, name);
    return ReadNumeric<double>(f, shmPtr_, shmSize_);
}

std::optional<float> IPCClient::ReadFloat32(const std::string& name) {
    auto* f = Find(fields_, name);
    return ReadNumeric<float>(f, shmPtr_, shmSize_);
}

std::optional<uint64_t> IPCClient::ReadUInt64(const std::string& name) {
    auto* f = Find(fields_, name);
    return ReadNumeric<uint64_t>(f, shmPtr_, shmSize_);
}

std::optional<int32_t> IPCClient::ReadInt32(const std::string& name) {
    auto* f = Find(fields_, name);
    return ReadNumeric<int32_t>(f, shmPtr_, shmSize_);
}

std::optional<bool> IPCClient::ReadBool(const std::string& name) {
    auto* f = Find(fields_, name);
    if (!shmPtr_ || !f) return std::nullopt;
    if (f->offset >= shmSize_) return std::nullopt;
    return static_cast<const char*>(shmPtr_)[f->offset] != 0;
}

} // namespace tcmt::ipc
