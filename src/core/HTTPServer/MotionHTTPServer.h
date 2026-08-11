#pragma once

#include <string>
#include <functional>
#include <atomic>
#include <thread>

// Minimal HTTP server — serves motion sensor data as JSON.
// Single-connection, single-threaded accept loop.
// Intended for localhost dev tools only (not production).
//
// Callback signature: std::string handler(const std::string& method, const std::string& path)
//   method: "GET" or "POST"
//   path:   e.g. "/sensors/motion"
//   return: HTTP response body (JSON)

class MotionHTTPServer {
public:
    using Handler = std::function<std::string(const std::string& method, const std::string& path)>;

    MotionHTTPServer() = default;
    ~MotionHTTPServer() { Stop(); }

    bool Start(int port, Handler handler);
    void Stop();
    // Number of currently open HTTP connections (accepted, not yet closed).
    int OpenClients() const;
    // Clients considered connected: open connections, or any request within
    // the last 2s (webpage polls open/close per request, so instantaneous
    // open count flickers).
    int ActiveClients() const;

private:
    void AcceptLoop();
    static std::string BuildResponse(int status, const std::string& body, const std::string& contentType);

    int _fd = -1;
    Handler _handler;
    std::atomic<bool> _running{false};
    std::thread _thread;
};
