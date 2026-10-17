#if !defined(_WIN32) && !defined(_WIN64)
#include "MotionHTTPServer.h"
#include "../Utils/Logger.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <sstream>
#include <chrono>

bool MotionHTTPServer::Start(int port, Handler handler) {
    _handler = std::move(handler);
    _fd = socket(AF_INET, SOCK_STREAM, 0);
    if (_fd < 0) { Logger::Error("MotionHTTPServer: socket failed"); return false; }

    int opt = 1;
    setsockopt(_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    if (bind(_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        Logger::Error("MotionHTTPServer: bind failed"); close(_fd); _fd = -1; return false;
    }
    if (listen(_fd, 2) < 0) {
        Logger::Error("MotionHTTPServer: listen failed"); close(_fd); _fd = -1; return false;
    }
    _running = true;
    _thread = std::thread(&MotionHTTPServer::AcceptLoop, this);
    Logger::Info(std::string("MotionHTTPServer: listening on 127.0.0.1:") + std::to_string(port));
    return true;
}

void MotionHTTPServer::Stop() {
    _running = false;
    if (_fd >= 0) { shutdown(_fd, SHUT_RDWR); close(_fd); _fd = -1; }
    if (_thread.joinable()) _thread.join();
}

// Current connected HTTP clients (incremented on accept, decremented on close).
std::atomic<int> s_connCount{0};
// Monotonic microseconds of the most recent request (for "recently active").
std::atomic<int64_t> s_lastRequestUs{0};

static int64_t NowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

int MotionHTTPServer::OpenClients() const {
    return s_connCount.load();
}

int MotionHTTPServer::ActiveClients() const {
    int open = s_connCount.load();
    if (open > 0) return open;
    int64_t last = s_lastRequestUs.load();
    if (last > 0 && (NowUs() - last) < 2'000'000) return 1;
    return 0;
}

void MotionHTTPServer::AcceptLoop() {
    while (_running) {
        sockaddr_in client{};
        socklen_t len = sizeof(client);
        int clientFd = accept(_fd, (sockaddr*)&client, &len);
        if (clientFd < 0) continue;
        s_connCount++;

        // Single-threaded accept loop: a client that connects but never sends
        // (browser speculative/idle connections) would block recv() forever
        // and stall the whole server. Drop silent connections after 3s.
        timeval tv{};
        tv.tv_sec = 3;
        setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        // Read request
        char buf[4096] = {};
        ssize_t n = recv(clientFd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) { s_connCount--; close(clientFd); continue; }
        buf[n] = '\0';
        s_lastRequestUs.store(NowUs());

        // Parse first line: METHOD /path HTTP/1.x
        std::string method, path;
        const char* p = buf;
        while (*p && *p != ' ') method += *p++;
        if (*p == ' ') p++;
        while (*p && *p != ' ') path += *p++;

        // Normalize: strip query string and trailing slash so
        // "/sensors/motion?x=1" and "/sensors/motion/" both match.
        size_t q = path.find('?');
        if (q != std::string::npos) path.resize(q);
        while (path.size() > 1 && path.back() == '/') path.pop_back();

        // Handle CORS preflight (OPTIONS)
        if (method == "OPTIONS") {
            std::string r = "HTTP/1.1 204 No Content\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
                "Access-Control-Allow-Headers: Content-Type\r\n"
                "Access-Control-Max-Age: 86400\r\n"
                "Content-Length: 0\r\n"
                "Connection: close\r\n\r\n";
            send(clientFd, r.c_str(), r.size(), 0);
            s_connCount--;
            close(clientFd);
            continue;
        }

        // Call handler
        std::string body = _handler(method, path);

        // Build response with CORS
        std::ostringstream resp;
        resp << "HTTP/1.1 200 OK\r\n"
             << "Content-Type: application/json\r\n"
             << "Access-Control-Allow-Origin: *\r\n"
             << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
             << "Access-Control-Allow-Headers: Content-Type\r\n"
             << "Content-Length: " << body.size() << "\r\n"
             << "Connection: close\r\n"
             << "\r\n"
             << body;

        std::string r = resp.str();
        send(clientFd, r.c_str(), r.size(), 0);
        s_connCount--;
        close(clientFd);
    }
}

std::string MotionHTTPServer::BuildResponse(int, const std::string& body, const std::string&) {
    return body;
}
#endif
