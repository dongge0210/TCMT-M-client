#include "ServerProbe.h"
#include "Utils/Logger.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <sstream>
#include <thread>
#include <chrono>

// ── Raw HTTP POST helper ──────────────────────────────────────
static bool RawPost(const std::string& host, int port,
                    const std::string& path, const std::string& body,
                    std::string& response) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd); return false;
    }

    std::ostringstream req;
    req << "POST " << path << " HTTP/1.1\r\n"
        << "Host: " << host << ":" << port << "\r\n"
        << "Content-Type: application/json\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n"
        << "\r\n"
        << body;

    std::string r = req.str();
    if (send(fd, r.c_str(), r.size(), 0) <= 0) {
        close(fd); return false;
    }

    // Read response
    char buf[4096] = {};
    ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
    close(fd);
    if (n <= 0) return false;
    buf[n] = '\0';

    // Extract body after \r\n\r\n
    const char* bodyStart = strstr(buf, "\r\n\r\n");
    if (bodyStart) response = bodyStart + 4;
    else response = buf;
    return true;
}

// ── ServerProbe ───────────────────────────────────────────────

bool ServerProbe::Start(const std::string& serverUrl) {
    serverUrl_ = serverUrl;
    running_ = true;

    // Register synchronously
    std::string response;
    std::string body = "{\"name\":\"" + std::string("TCMT-M on ") +
        []{ char h[256]; gethostname(h, sizeof(h)); return std::string(h); }() +
        "\",\"os\":\"macOS\",\"model\":\"TCMT-M\"}";

    if (!RawPost("127.0.0.1", 8080, "/api/register", body, response)) {
        Logger::Warn("ServerProbe: failed to register with tcmt-server");
        running_ = false;
        return false;
    }

    // Extract token from JSON response
    auto tokPos = response.find("\"token\":\"");
    auto idPos  = response.find("\"id\":\"");
    if (tokPos != std::string::npos) {
        tokPos += 9;
        auto end = response.find('"', tokPos);
        token_ = response.substr(tokPos, end - tokPos);
    }
    if (idPos != std::string::npos) {
        idPos += 6;
        auto end = response.find('"', idPos);
        deviceId_ = response.substr(idPos, end - idPos);
    }

    Logger::Info("ServerProbe: registered as " + deviceId_ + " token=" + token_);

    thread_ = std::thread(&ServerProbe::ProbeThread, this);
    return true;
}

void ServerProbe::Stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void ServerProbe::PostSnapshot(const std::string& jsonData) {
    std::lock_guard<std::mutex> lk(mutex_);
    pendingSnapshot_ = jsonData;
}

void ServerProbe::ProbeThread() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));

        if (token_.empty()) continue;

        std::string snapshot;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (pendingSnapshot_.empty()) continue;
            snapshot = std::move(pendingSnapshot_);
        }

        std::string body = "{\"token\":\"" + token_ + "\"";
        // Append snapshot fields (skip leading '{' of snapshot, add comma)
        if (!snapshot.empty() && snapshot.front() == '{') {
            body += "," + snapshot.substr(1);
        } else {
            body += "}";
        }

        std::string response;
        if (!RawPost("127.0.0.1", 8080, "/api/ingest", body, response)) {
            Logger::Debug("ServerProbe: POST failed");
        }
    }
}
