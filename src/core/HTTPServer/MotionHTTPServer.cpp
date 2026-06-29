#if !defined(_WIN32) && !defined(_WIN64)
#include "MotionHTTPServer.h"
#include "../Utils/Logger.h"
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <sstream>

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

int s_connCount = 0;

void MotionHTTPServer::AcceptLoop() {
    while (_running) {
        sockaddr_in client{};
        socklen_t len = sizeof(client);
        int clientFd = accept(_fd, (sockaddr*)&client, &len);
        if (clientFd < 0) continue;
        s_connCount++;

        // Read request
        char buf[4096] = {};
        ssize_t n = recv(clientFd, buf, sizeof(buf) - 1, 0);
        if (n <= 0) { close(clientFd); continue; }
        buf[n] = '\0';

        // Parse first line: METHOD /path HTTP/1.x
        std::string method, path;
        const char* p = buf;
        while (*p && *p != ' ') method += *p++;
        if (*p == ' ') p++;
        while (*p && *p != ' ') path += *p++;

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
        close(clientFd);
    }
}

std::string MotionHTTPServer::BuildResponse(int, const std::string& body, const std::string&) {
    return body;
}
#endif
