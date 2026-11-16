// MSVC deprecation noise suppression (must come before any includes)
#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif

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
#include <algorithm>
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

// While a 403 has us blocked, retry at this cadence instead of every loop.
static constexpr auto kBlockedRetryInterval = std::chrono::seconds(60);

// Samples kept in memory until the server accepts them. Covers the waiting
// period before registration and any failed uploads; 300 samples is about
// five minutes at the default 2 s upload interval (and 300 s at 1 Hz).
static constexpr size_t kSampleCapacity = 300;

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
    if (connect(fd, res->ai_addr, static_cast<int>(res->ai_addrlen)) < 0) {
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
                        : static_cast<int>(send(fd, data + off, static_cast<int>(n - off), 0));
#else
            int w = static_cast<int>(send(fd, data + off, static_cast<int>(n - off), 0));
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
                               std::string& id, std::string& token,
                               int& intervalSec) {
    intervalSec = 0;
    std::ifstream f(ClientConfigPath());
    if (!f) return;
    try {
        json cfg = json::parse(f);
        clientKey = cfg.value("clientKey", "");
        if (cfg.contains("servers") && cfg["servers"].contains(serverUrl)) {
            const json& entry = cfg["servers"][serverUrl];
            id = entry.value("id", "");
            token = entry.value("token", "");
            intervalSec = entry.value("intervalSec", 0);
        }
    } catch (...) { /* corrupt file — will rewrite */ }
}

static void SaveClientIdentity(const std::string& serverUrl,
                               const std::string& clientKey,
                               const std::string& id, const std::string& token,
                               int intervalSec) {
    try {
        json cfg = json::object();
        {
            std::ifstream f(ClientConfigPath());
            if (f) { try { cfg = json::parse(f); } catch (...) { cfg = json::object(); } }
        }
        cfg["clientKey"] = clientKey;
        cfg["servers"][serverUrl] = {
            {"id", id}, {"token", token}, {"intervalSec", intervalSec}};
        const std::string dir = ClientConfigPath().substr(0, ClientConfigPath().find_last_of("/\\"));
        MakeDir(dir);
        std::ofstream f(ClientConfigPath());
        if (f) f << cfg.dump(2);
#ifndef _WIN32
        // The file holds device tokens; keep it private to this user.
        ::chmod(ClientConfigPath().c_str(), 0600);
#endif
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
    int registeredIntervalSec = 0;
    LoadClientIdentity(serverUrl_, clientKey_, deviceId_, token_, registeredIntervalSec);
    if (clientKey_.empty()) clientKey_ = GenClientKey();

    // The server sizes its offline threshold from the `intervalSec` reported
    // at registration. A cached token was issued for the old cadence, so drop
    // it when the interval changed: ProbeThread then registers again and the
    // server learns the new value. (Same path as a 401 re-registration.)
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!token_.empty() && registeredIntervalSec > 0
            && registeredIntervalSec != uploadIntervalSec_) {
            Logger::Info("ServerProbe: upload interval changed ("
                         + std::to_string(registeredIntervalSec) + "s -> "
                         + std::to_string(uploadIntervalSec_)
                         + "s), re-registering");
            token_.clear();
        }
    }

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

    blocked_ = false;  // a new target starts unblocked
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

void ServerProbe::SetEnrollCode(const std::string& code) {
    std::lock_guard<std::mutex> lk(mutex_);
    enrollCode_ = code;
}

int64_t ServerProbe::LastAckAgeSec() const {
    std::lock_guard<std::mutex> lk(mutex_);
    if (lastAckMs_ <= 0) return -1;
    const int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const int64_t age = (nowMs - lastAckMs_) / 1000;
    return age < 0 ? 0 : age;
}

void ServerProbe::ClearIdentity() {
    std::lock_guard<std::mutex> lk(mutex_);
    ++identityGen_;  // invalidate any registration response still in flight
    token_.clear();
    deviceId_.clear();
    if (!serverUrl_.empty()) {
        SaveClientIdentity(serverUrl_, clientKey_, "", "", 0);
    }
    Logger::Info("ServerProbe: identity cleared (enrollment changed)");
}

void ServerProbe::SetUploadIntervalSec(int seconds) {
    if (seconds < 1) seconds = 1;
    if (seconds > 300) seconds = 300;
    std::lock_guard<std::mutex> lk(mutex_);
    uploadIntervalSec_ = seconds;
}

// Backfill timestamps are epoch milliseconds. Prefer a timestamp already in
// the snapshot; today the collectors emit none, so this normally falls back to
// the enqueue time — captured per sample, not once for the whole batch.
static int64_t SnapshotTimestampMs(const std::string& snapshot) {
    try {
        const json j = json::parse(snapshot);
        for (const char* key : {"ts", "timestamp"}) {
            if (j.contains(key) && j[key].is_number()) return j[key].get<int64_t>();
        }
    } catch (...) { /* not valid JSON; use the clock */ }
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

void ServerProbe::PostSnapshot(const std::string& jsonData) {
    const int64_t ts = SnapshotTimestampMs(jsonData);
    std::lock_guard<std::mutex> lk(mutex_);
    sampleQueue_.push_back({ts, jsonData});
    if (sampleQueue_.size() > kSampleCapacity) {
        sampleQueue_.pop_front();
        Logger::Debug("ServerProbe: queue full, dropped oldest sample");
    }
}

void ServerProbe::RequestFlush() {
    {
        std::lock_guard<std::mutex> lk(flushMutex_);
        flushRequested_ = true;
    }
    flushCv_.notify_one();
}

int ServerProbe::SendBatch(const std::string& path, const std::string& token) {
    std::string body;
    size_t count = 0;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        count = sampleQueue_.size();
        if (count == 0) return 200;
        body = "{\"token\":\"" + token + "\",\"samples\":[";
        for (size_t i = 0; i < count; ++i) {
            if (i) body += ",";
            body += "{\"ts\":" + std::to_string(sampleQueue_[i].ts)
                  + ",\"data\":" + sampleQueue_[i].json + "}";
        }
        body += "]}";
    }

    std::string response;
    const int status = RawPost(serverHost_, serverPort_, path,
                               body, response, useTls_, insecure_);
    if (status != 200) {
        // Keep the samples: they are retried with the next batch so a short
        // outage (or a backfill the server declined) does not lose data.
        Logger::Debug("ServerProbe: batch to " + path + " failed (HTTP "
                      + std::to_string(status) + "), "
                      + std::to_string(count) + " samples kept");
        return status;
    }

    // Remove only the samples this request carried; ones collected while the
    // POST was in flight stay queued for the next upload.
    {
        std::lock_guard<std::mutex> lk(mutex_);
        const size_t sent = (std::min)(count, sampleQueue_.size());
        sampleQueue_.erase(sampleQueue_.begin(),
                           sampleQueue_.begin() + static_cast<std::ptrdiff_t>(sent));
    }

    int accepted = -1, skipped = -1;
    try {
        const json j = json::parse(response);
        accepted = j.value("accepted", -1);
        skipped = j.value("skipped", -1);
    } catch (...) { /* response body is informational only */ }
    // Only a real ingest POST is a server acknowledgement; the backfill
    // replay does not mean the live stream is confirmed.
    if (path.find("/api/ingest") != std::string::npos) {
        std::lock_guard<std::mutex> lk(mutex_);
        lastAckMs_ = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        lastAckCount_ = accepted >= 0 ? accepted : static_cast<int>(count);
    }
    Logger::Debug("ServerProbe: " + std::to_string(count) + " samples to " + path
                  + " (accepted " + std::to_string(accepted)
                  + ", skipped " + std::to_string(skipped) + ")");
    return status;
}

void ServerProbe::ProbeThread() {
    bool registered = false;
    bool wasOnline = false;
    auto nextRegisterAttempt = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lk(mutex_);
        registered = !token_.empty();
    }

    while (running_) {
        // Sleep between register retries, but wake at once when an upload is
        // due so the batch is not held back by the retry cadence. Clearing the
        // request here also keeps a pending flush from spinning the retry loop:
        // while unregistered the queue is replayed by the backfill instead.
        bool flush = false;
        {
            std::unique_lock<std::mutex> lk(flushMutex_);
            flushCv_.wait_for(lk, std::chrono::milliseconds(2000),
                              [this] { return !running_.load() || flushRequested_; });
            flush = flushRequested_;
            flushRequested_ = false;
        }
        if (!running_) break;

        // ── Register / re-register until the server accepts us ──
        if (!registered) {
            const auto now = std::chrono::steady_clock::now();
            if (blocked_.load() && now < nextRegisterAttempt) continue;

            json body = {
                {"name", deviceName_},
                {"os", os_},
                {"model", model_},
                {"clientKey", clientKey_},
            };
            int intervalSec = 2;
            {
                std::lock_guard<std::mutex> lk(mutex_);
                intervalSec = uploadIntervalSec_;
                if (!enrollCode_.empty()) body["enroll"] = enrollCode_;
            }
            // Lets the server size its offline threshold from this device's
            // actual cadence instead of a fixed 15 s.
            body["intervalSec"] = intervalSec;
            // Snapshot the identity generation: if ClearIdentity() runs while
            // this request is in flight, its answer must not be persisted.
            uint64_t gen = 0;
            {
                std::lock_guard<std::mutex> lk(mutex_);
                gen = identityGen_;
            }
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
                int reportedIntervalSec = 2;
                bool stale = false;
                if (!token.empty()) {
                    {
                        std::lock_guard<std::mutex> lk(mutex_);
                        stale = identityGen_ != gen;
                        if (!stale) {
                            token_ = token;
                            deviceId_ = id;
                        }
                        reportedIntervalSec = uploadIntervalSec_;
                    }
                }
                if (stale) {
                    // The enrollment code changed while we were registering;
                    // the token belongs to the old identity.
                    Logger::Info("ServerProbe: discarded a registration that "
                                 "was superseded by an identity reset");
                } else if (!token.empty()) {
                    SaveClientIdentity(serverUrl_, clientKey_, id, token,
                                       reportedIntervalSec);
                    blocked_ = false;
                    registered = true;
                    wasOnline = true;
                    Logger::Info("ServerProbe: registered as " + id);
                    // Replay whatever the queue holds. A failure is history
                    // only: the samples stay queued for the next upload.
                    SendBatch(basePath_ + "/api/backfill", token);
                }
            } else if (status == 403) {
                // Refused by policy: the device was revoked, or the enrollment
                // code is wrong. Neither is fixed by retrying every 2 s, so
                // back off and log once per transition instead of per cycle.
                const bool wasBlocked = blocked_.exchange(true);
                wasOnline = false;
                if (!wasBlocked) {
                    std::string reason;
                    try {
                        const json err = json::parse(response);
                        if (err.contains("error") && err["error"].is_string())
                            reason = err["error"].get<std::string>();
                    } catch (...) { /* non-JSON body */ }
                    Logger::Error("ServerProbe: registration refused (HTTP 403)"
                                  + (reason.empty() ? std::string(", no reason given")
                                                    : ": " + reason));
                }
            } else if (wasOnline) {
                Logger::Warn("ServerProbe: lost server (HTTP " + std::to_string(status) + "), retrying");
                wasOnline = false;
            }
            if (blocked_.load()) nextRegisterAttempt = now + kBlockedRetryInterval;
            continue;
        }

        // ── Upload every queued sample as one batch ──
        if (!flush) continue;

        std::string token;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            token = token_;
        }

        const int status = SendBatch(basePath_ + "/api/ingest", token);
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
