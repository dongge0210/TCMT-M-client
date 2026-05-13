#ifdef _WIN32
#include <windows.h>
#endif

#include "NamedPipeServer.h"
#include <cstring>
#include <algorithm>

namespace tcmt::ipc {

NamedPipeServer::NamedPipeServer() = default;

NamedPipeServer::~NamedPipeServer() {
    Stop();
}

bool NamedPipeServer::Start() {
#ifdef _WIN32
    running_ = true;
    serverThread_ = std::thread(&NamedPipeServer::ServerLoop, this);
    return true;
#else
    lastError_ = "NamedPipeServer is Windows-only";
    return false;
#endif
}

void NamedPipeServer::Stop() {
    running_ = false;

    // Close all client pipes to unblock ConnectNamedPipe
    {
        std::lock_guard<std::mutex> lock(clientsMutex_);
#ifdef _WIN32
        for (PipeHandle h : clientPipes_) {
            CancelIoEx(static_cast<HANDLE>(h), nullptr);
            CloseHandle(static_cast<HANDLE>(h));
        }
#endif
        clientPipes_.clear();
    }

    if (serverThread_.joinable())
        serverThread_.join();
}

void NamedPipeServer::ServerLoop() {
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
            DWORD gle = GetLastError();
            connected = (gle == ERROR_PIPE_CONNECTED) ? TRUE : FALSE;
        }

        if (!connected) {
            CloseHandle(hPipe);
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            clientPipes_.push_back(hPipe);
        }

        // Send schema
        SendSchema(hPipe);
    }
#endif
}

void NamedPipeServer::SendSchema(PipeHandle pipe) {
    auto data = SerializeSchema();
    if (data.empty()) return;
#ifdef _WIN32
    DWORD written = 0;
    WriteFile(static_cast<HANDLE>(pipe), data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
#endif
}

std::vector<uint8_t> NamedPipeServer::SerializeSchema() {
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

void NamedPipeServer::UpdateSchema(const SchemaHeader& header, const std::vector<FieldDef>& fields) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        schemaHeader_ = header;
        schemaFields_ = fields;
    }

    auto data = SerializeSchema();

    std::lock_guard<std::mutex> lock(clientsMutex_);
    for (auto it = clientPipes_.begin(); it != clientPipes_.end(); ) {
#ifdef _WIN32
        DWORD written = 0;
        if (!WriteFile(static_cast<HANDLE>(*it), data.data(), static_cast<DWORD>(data.size()), &written, nullptr)) {
            CloseHandle(static_cast<HANDLE>(*it));
            it = clientPipes_.erase(it);
        } else {
            ++it;
        }
#else
        ++it;
#endif
    }
}

} // namespace tcmt::ipc
