#pragma once

#include <string>
#include <atomic>
#include <thread>
#include <mutex>

// Sends TCMT sensor data to a remote tcmt-server via HTTP POST.
// Registers as a device on first call, then POSTs data snapshots.
class ServerProbe {
public:
    ServerProbe() = default;
    ~ServerProbe() { Stop(); }

    // Start sending to given server URL (e.g. "http://127.0.0.1:8080").
    // Returns true if registration succeeded.
    bool Start(const std::string& serverUrl);

    void Stop();

    // Call from main loop to push current data.
    // Thread-safe — posts asynchronously via a background thread.
    // Returns the probe's device token (empty if not started).
    const std::string& Token() const { return token_; }

    // Queue a JSON snapshot for upload. Non-blocking.
    void PostSnapshot(const std::string& jsonData);

private:
    void ProbeThread();
    bool HttpPost(const std::string& path, const std::string& body, std::string& response);
    bool HttpPostSync(const std::string& path, const std::string& body, std::string& response);

    std::string serverUrl_;
    std::string token_;
    std::string deviceId_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    std::mutex mutex_;
    std::string pendingSnapshot_;
};
