#pragma once

#include <string>
#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <deque>
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

    // Optional enrollment code for servers started with --enroll-token.
    // Sent as the `enroll` field on /api/register; omitted when empty.
    void SetEnrollCode(const std::string& code);

    // Upload cadence the collector actually uses, in seconds. Reported as
    // `intervalSec` on /api/register so the server can derive its offline
    // threshold from this device's real cadence. Clamped to 1..300; default 2.
    void SetUploadIntervalSec(int seconds);

    // True after /api/register was refused with 403 (device disabled, or a
    // wrong enrollment code) and no later registration succeeded. The probe
    // keeps retrying in this state, at a much slower cadence.
    bool IsBlocked() const { return blocked_.load(); }

    // Call from main loop to push current data.
    // Thread-safe — posts asynchronously via a background thread.
    // Returns the probe's device token (empty if not started).
    std::string Token() const;

    // Device id assigned by the server after registration (empty until then).
    std::string DeviceId() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return deviceId_;
    }

    // Queue a JSON snapshot for upload. Non-blocking.
    void PostSnapshot(const std::string& jsonData);

    // Ask the upload thread to POST every queued sample as one batch right
    // away. Non-blocking; a failed upload keeps the samples for the next try.
    void RequestFlush();

private:
    // One collected sample waiting to be uploaded. Timestamps are absolute
    // epoch milliseconds so the server can place them on a timeline.
    struct Sample {
        int64_t ts = 0;
        std::string json;
    };

    void ProbeThread();
    // POSTs every queued sample to `path` as one batch. Samples leave the
    // queue only after the server answers 200, so a failed upload (or a
    // failed backfill) keeps them for the next attempt.
    int SendBatch(const std::string& path, const std::string& token);
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
    std::string enrollCode_;  // guarded by mutex_
    int uploadIntervalSec_ = 2;  // guarded by mutex_
    std::atomic<bool> running_{false};
    std::atomic<bool> blocked_{false};
    std::thread thread_;
    mutable std::mutex mutex_;
    // Every collected sample not yet accepted by the server. Shared by the
    // regular batch upload and the post-registration backfill.
    std::deque<Sample> sampleQueue_;  // guarded by mutex_
    // Wakes ProbeThread early when an upload is due.
    std::mutex flushMutex_;
    std::condition_variable flushCv_;
    bool flushRequested_ = false;  // guarded by flushMutex_
};
