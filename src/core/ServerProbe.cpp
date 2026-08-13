#include "ServerProbe.h"
#include "Utils/Logger.h"
#include "nlohmann/json.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <direct.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <sys/stat.h>
#endif
#include <fcntl.h>
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <thread>
#include <chrono>
#include <fstream>
#include <random>

#ifdef TCMT_USE_TLS
#include <openssl/ssl.h>
#include <openssl/err.h>
#endif

using json = nlohmann::json;

// Small platform shims: winsock on Windows, POSIX sockets elsewhere.
#ifdef _WIN32
static int CloseFd(int fd) { return closesocket(fd); }
static int MakeDir(const std::string& p) { return _mkdir(p.c_str()); }
#else
static int CloseFd(int fd) { return close(fd); }
static int MakeDir(const std::string& p) { return mkdir(p.c_str(), 0755); }
#endif

#ifdef TCMT_USE_TLS
// One shared client SSL_CTX (verify peer against system roots by default).
static SSL_CTX* GlobalSslCtx() {
    static SSL_CTX* ctx = [] {
        OPENSSL_init_ssl(0, nullptr);
        SSL_CTX* c = SSL_CTX_new(TLS_client_method());
        if (c) {
            SSL_CTX_set_verify(c, SSL_VERIFY_PEER, nullptr);
            SSL_CTX_set_default_verify_paths(c);
        }
        return c;
    }();
    return ctx;
}
#endif

// ── HTTP(S) POST helper ─────────────────────────────────────────────
// Returns HTTP status code (200..599), or 0 on connect/write failure.
static int RawPost(const std::string& host, int port, const std::string& path,
                   const std::string& body, std::string& response,
                   bool useTls, bool insecure) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string portStr = std::to_string(port);
    if (getaddrinfo(host.c_str(), portStr.c_str(), &hints, &res) != 0 || !res) return 0;

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); return 0; }
    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        CloseFd(fd); freeaddrinfo(res); return 0;
    }
    freeaddrinfo(res);

    // NAT / public-internet hygiene: never hang forever on a dead peer.
#ifdef _WIN32
    DWORD tv = 8000; // SO_RCVTIMEO/SO_SNDTIMEO take milliseconds on winsock
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char*)&tv, sizeof(tv));
#else
    timeval tv{8, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

#ifdef TCMT_USE_TLS
    SSL* ssl = nullptr;
    if (useTls) {
        SSL_CTX* ctx = GlobalSslCtx();
        if (!ctx) { close(fd); return 0; }
        ssl = SSL_new(ctx);
        if (!ssl) { close(fd); return 0; }
        if (insecure) SSL_set_verify(ssl, SSL_VERIFY_NONE, nullptr);
        SSL_set_fd(ssl, fd);
        if (SSL_connect(ssl) != 1) {
            ERR_clear_error();
            SSL_free(ssl);
            CloseFd(fd);
            return 0;
        }
    }
#else
    (void)useTls;
    (void)insecure;
#endif

    std::ostringstream req;
    req << "POST " << path << " HTTP/1.1\r\n"
        << "Host: " << host << ":" << port << "\r\n"
        << "Content-Type: application/json\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n"
        << "\r\n"
        << body;
    const std::string request = req.str();

    auto writeAll = [&](const char* data, size_t n) -> bool {
        size_t off = 0;
        while (off < n) {
#ifdef TCMT_USE_TLS
            int w = ssl ? SSL_write(ssl, data + off, static_cast<int>(n - off))
                        : static_cast<int>(send(fd, data + off, n - off, 0));
#else
            int w = static_cast<int>(send(fd, data + off, n - off, 0));
#endif
            if (w <= 0) return false;
            off += static_cast<size_t>(w);
        }
        return true;
    };

    if (!writeAll(request.data(), request.size())) {
#ifdef TCMT_USE_TLS
        if (ssl) { SSL_free(ssl); }
#endif
        CloseFd(fd);
        return 0;
    }

    std::string raw;
    char buf[8192];
    for (;;) {
#ifdef TCMT_USE_TLS
        int n = ssl ? SSL_read(ssl, buf, static_cast<int>(sizeof(buf)))
                    : static_cast<int>(recv(fd, buf, sizeof(buf), 0));
#else
        int n = static_cast<int>(recv(fd, buf, sizeof(buf), 0));
#endif
        if (n <= 0) break;
        raw.append(buf, static_cast<size_t>(n));
    }

    int status = 0;
    if (raw.rfind("HTTP/1.1 ", 0) == 0 || raw.rfind("HTTP/1.0 ", 0) == 0) {
        status = std::atoi(raw.c_str() + 9);
    }
    const char* bodyStart = std::strstr(raw.c_str(), "\r\n\r\n");
    response = bodyStart ? std::string(bodyStart + 4) : raw;

#ifdef TCMT_USE_TLS
    if (ssl) {
        SSL_shutdown(ssl);
        SSL_free(ssl);
    }
#endif
    CloseFd(fd);
    return status;
}

// Parse "http(s)://host[:port][/base/path]". Bare "host:port" is accepted.
static bool ParseServerUrl(const std::string& url, std::string& host, int& port,
                           std::string& base, bool& https) {
    std::string s = url;
    https = (s.rfind("https://", 0) == 0);
    if (s.rfind("http://", 0) != 0 && !https) s = "http://" + s;

    const size_t hostStart = https ? 8 : 7; // after "https://" / "http://"
    size_t hostEnd = s.find_first_of("/?", hostStart);
    if (hostEnd == std::string::npos) hostEnd = s.size();

    const std::string hostPort = s.substr(hostStart, hostEnd - hostStart);
    const size_t colon = hostPort.rfind(':');
    if (colon != std::string::npos) {
        host = hostPort.substr(0, colon);
        port = std::atoi(hostPort.c_str() + colon + 1);
        if (port <= 0 || port > 65535) port = https ? 443 : 8080;
    } else {
        host = hostPort;
        port = https ? 443 : 8080;
    }

    base = (hostEnd < s.size()) ? s.substr(hostEnd) : "";
    while (base.size() > 1 && base.back() == '/') base.pop_back();
    return !host.empty();
}

// ── Client identity (persisted, per-server) ─────────────────────────
// Kept in ~/.tcmt/client.json so device identity survives restarts, IP
// changes, and NAT rebinds. Identity is a random clientKey, not a hostname,
// so different machines never collide on a shared/public server.
static std::string ClientConfigPath() {
#ifdef _WIN32
    const char* home = std::getenv("USERPROFILE");
    return home ? std::string(home) + "\\.tcmt\\client.json" : "C:\\tcmt_client.json";
#else
    const char* home = std::getenv("HOME");
    return home ? std::string(home) + "/.tcmt/client.json" : "/tmp/tcmt_client.json";
#endif
}

static std::string GenClientKey() {
    static const char hex[] = "0123456789abcdef";
    static thread_local std::mt19937 rng(std::random_device{}());
    std::string key = "tcmtc_";
    for (int i = 0; i < 12; ++i) key += hex[rng() % 16];
    return key;
}

static void LoadClientIdentity(const std::string& serverUrl,
                               std::string& clientKey,
                               std::string& id, std::string& token) {
    std::ifstream f(ClientConfigPath());
    if (!f) return;
    try {
        json cfg = json::parse(f);
        clientKey = cfg.value("clientKey", "");
        if (cfg.contains("servers") && cfg["servers"].contains(serverUrl)) {
            id = cfg["servers"][serverUrl].value("id", "");
            token = cfg["servers"][serverUrl].value("token", "");
        }
    } catch (...) { /* corrupt file — will rewrite */ }
}

static void SaveClientIdentity(const std::string& serverUrl,
                               const std::string& clientKey,
                               const std::string& id, const std::string& token) {
    try {
        json cfg = json::object();
        {
            std::ifstream f(ClientConfigPath());
            if (f) { try { cfg = json::parse(f); } catch (...) { cfg = json::object(); } }
        }
        cfg["clientKey"] = clientKey;
        cfg["servers"][serverUrl] = {{"id", id}, {"token", token}};
        const std::string dir = ClientConfigPath().substr(0, ClientConfigPath().find_last_of("/\\"));
        MakeDir(dir);
        std::ofstream f(ClientConfigPath());
        if (f) f << cfg.dump(2);
    } catch (...) { /* non-fatal */ }
}

// ── ServerProbe ─────────────────────────────────────────────────────

bool ServerProbe::Start(const std::string& serverUrl) {
#ifdef _WIN32
    // Winsock init (once per process).
    static const bool s_wsaReady = [] {
        WSADATA wsa;
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    if (!s_wsaReady) {
        Logger::Error("ServerProbe: WSAStartup failed");
        return false;
    }
#endif
    serverUrl_ = serverUrl;
    if (!ParseServerUrl(serverUrl, serverHost_, serverPort_, basePath_, useTls_)) {
        Logger::Warn("ServerProbe: invalid server URL: " + serverUrl);
        return false;
    }
#ifndef TCMT_USE_TLS
    if (useTls_) {
        Logger::Warn("ServerProbe: https requested but binary built without TLS");
        return false;
    }
#endif

    // Restore or create the stable client identity.
    LoadClientIdentity(serverUrl_, clientKey_, deviceId_, token_);
    if (clientKey_.empty()) clientKey_ = GenClientKey();

    // Static register payload fields.
    deviceName_ = "TCMT-M on " + [] {
        char h[256];
        if (gethostname(h, sizeof(h)) == 0) return std::string(h);
        return std::string("unknown");
    }();
#ifdef _WIN32
    os_ = "Windows";
#else
    os_ = "macOS";
#endif
    model_ = "TCMT-M";

    running_ = true;
    thread_ = std::thread(&ServerProbe::ProbeThread, this);
    Logger::Info("ServerProbe: started, target " + serverUrl
                 + (useTls_ ? " (TLS)" : "")
                 + (token_.empty() ? ", registering..." : ", cached identity"));
    return true;
}

void ServerProbe::Stop() {
    running_ = false;
    // Do NOT join: ProbeThread may be blocked inside a network call (TCP
    // connect to an unreachable host can take minutes on macOS). Joining
    // here would freeze the monitor loop that calls Stop(). The thread
    // checks running_ every 2s and exits on its own.
    if (thread_.joinable()) thread_.detach();
}

std::string ServerProbe::Token() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return token_;
}

void ServerProbe::PostSnapshot(const std::string& jsonData) {
    std::lock_guard<std::mutex> lk(mutex_);
    pendingSnapshot_ = jsonData;
}

void ServerProbe::ProbeThread() {
    bool registered = false;
    bool wasOnline = false;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        registered = !token_.empty();
    }

    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2000));

        // ── Register / re-register until the server accepts us ──
        if (!registered) {
            const json body = {
                {"name", deviceName_},
                {"os", os_},
                {"model", model_},
                {"clientKey", clientKey_},
            };
            std::string response;
            const int status = RawPost(serverHost_, serverPort_,
                                       basePath_ + "/api/register",
                                       body.dump(), response, useTls_, insecure_);
            if (status == 200) {
                std::string id, token;
                auto tokPos = response.find("\"token\":\"");
                auto idPos  = response.find("\"id\":\"");
                if (tokPos != std::string::npos) {
                    tokPos += 9;
                    const auto end = response.find('"', tokPos);
                    token = response.substr(tokPos, end - tokPos);
                }
                if (idPos != std::string::npos) {
                    idPos += 6;
                    const auto end = response.find('"', idPos);
                    id = response.substr(idPos, end - idPos);
                }
                if (!token.empty()) {
                    {
                        std::lock_guard<std::mutex> lk(mutex_);
                        token_ = token;
                        deviceId_ = id;
                    }
                    SaveClientIdentity(serverUrl_, clientKey_, id, token);
                    registered = true;
                    wasOnline = true;
                    Logger::Info("ServerProbe: registered as " + id);
                }
            } else if (wasOnline) {
                Logger::Warn("ServerProbe: lost server (HTTP " + std::to_string(status) + "), retrying");
                wasOnline = false;
            }
            continue;
        }

        // ── Upload latest snapshot ──
        std::string snapshot;
        std::string token;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (pendingSnapshot_.empty()) continue;
            snapshot = std::move(pendingSnapshot_);
            token = token_;
        }

        std::string body = "{\"token\":\"" + token + "\"";
        if (!snapshot.empty() && snapshot.front() == '{') {
            body += "," + snapshot.substr(1);
        } else {
            body += "}";
        }

        std::string response;
        const int status = RawPost(serverHost_, serverPort_,
                                   basePath_ + "/api/ingest",
                                   body, response, useTls_, insecure_);
        if (status == 200) {
            if (!wasOnline) Logger::Info("ServerProbe: upload resumed");
            wasOnline = true;
        } else if (status == 401) {
            // Token revoked (server DB reset / moved). Re-register next cycle.
            Logger::Warn("ServerProbe: unauthorized — re-registering");
            {
                std::lock_guard<std::mutex> lk(mutex_);
                token_.clear();
            }
            registered = false;
            wasOnline = false;
        } else if (status == 0) {
            if (wasOnline) {
                Logger::Debug("ServerProbe: server unreachable, will retry");
                wasOnline = false;
            }
        }
    }
}
