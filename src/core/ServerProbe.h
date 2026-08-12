#pragma once

#include <string>
#include <atomic>
#include <thread>
#include <mutex>

// Sends TCMT sensor data to a remote tcmt-server via HTTP(S) POST.
// Registers on a background thread (retries until reachable), then POSTs
// data snapshots. Identity is a persisted random clientKey so devices work
// across NAT rebinds, IP changes, and shared public servers.
class ServerProbe {
public:
    ServerProbe() = default;
    ~ServerProbe() { Stop(); }

    // Start sending to given server URL. Cross-device friendly:
    //   "http://127.0.0.1:8080"          (local)
    //   "http://192.168.1.20:8080"       (LAN IP)
    //   "http://tcmt-server.local:8080"  (hostname)
    //   "https://tcmt.example.com"       (public internet, needs TLS build)
    //   "http://host:8080/tcmt"          (reverse-proxy base path)
    // Returns true if the target is usable; registration itself happens on a
    // background thread and retries until the server is reachable.
    bool Start(const std::string& serverUrl);

    void Stop();

    // Skip TLS certificate verification (self-signed / internal CAs).
    // Only meaningful with https URLs. Use at your own risk.
    void SetInsecure(bool insecure) { insecure_ = insecure; }

    // Call from main loop to push current data.
    // Thread-safe — posts asynchronously via a background thread.
    // Returns the probe's device token (empty if not started).
    std::string Token() const;

    // Queue a JSON snapshot for upload. Non-blocking.
    void PostSnapshot(const std::string& jsonData);

private:
    void ProbeThread();
    bool HttpPost(const std::string& path, const std::string& body, std::string& response);
    bool HttpPostSync(const std::string& path, const std::string& body, std::string& response);

    std::string serverUrl_;
    std::string serverHost_ = "127.0.0.1";
    int serverPort_ = 8080;
    std::string basePath_; // optional URL prefix, e.g. "/tcmt"
    bool useTls_ = false;
    bool insecure_ = false;
    std::string clientKey_;  // stable per-machine identity (persisted)
    std::string deviceName_;
    std::string os_;
    std::string model_;
    std::string token_;
    std::string deviceId_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    mutable std::mutex mutex_;
    std::string pendingSnapshot_;
};
