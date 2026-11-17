#pragma once

#include <string>
#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <thread>
#include <mutex>
#include <vector>

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

    // Forget the device token and id for the current server URL. Called when
    // the enrollment code changed: the token was issued against the old code,
    // so keeping it would keep reporting under the old identity.
    void ClearIdentity();

    // Optional enrollment code for servers started with --enroll-token.
    // Sent as the `enroll` field on /api/register; omitted when empty.
    void SetEnrollCode(const std::string& code);

    // Sections this build can collect (cpu, memory, gpu, thermal, motion,
    // storage, network, power). Reported on /api/register so the server can
    // tell "this machine has no battery" apart from "the reading went
    // missing". Call before Start().
    void SetSections(std::vector<std::string> sections);

    // Settings the server pushes back with every accepted request. The probe
    // keeps the upload cadence and backfill flag for itself; everything else
    // is handed to the collector, which decides what goes into a snapshot.
    struct ServerSettings {
        std::string rev;              // server's config revision
        int intervalSec = 0;          // upload cadence, 0 when not stated
        bool backfill = true;         // replay the queue after (re)registering
        std::map<std::string, bool> collect;  // section -> enabled
    };
    // Fires on the upload thread, at most once per rev. Must be cheap and
    // thread-safe: it runs between batches.
    void SetOnServerSettings(std::function<void(const ServerSettings&)> callback);

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

    // Last /api/ingest acknowledgement: epoch milliseconds of the response and
    // how many samples it accepted. 0 / false until the first acknowledged
    // batch, so the TUI can show that the server confirmed receipt rather than
    // only that a request left the machine.
    bool HasServerAck() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return lastAckMs_ > 0;
    }
    int64_t LastAckMs() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return lastAckMs_;
    }
    int LastAckCount() const {
        std::lock_guard<std::mutex> lk(mutex_);
        return lastAckCount_;
    }
    // Seconds since the last acknowledged batch, or -1 if none yet.
    int64_t LastAckAgeSec() const;

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
    // Reads the `cfg` envelope a server attaches to every accepted response
    // and applies the one-time-per-rev parts. Safe with older servers that
    // send no envelope at all.
    void ApplyServerSettings(const std::string& response);
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
    std::vector<std::string> sections_;  // set before Start, then read-only
    std::function<void(const ServerSettings&)> onSettings_;  // guarded by mutex_
    std::string appliedRev_;       // guarded by mutex_
    bool backfillEnabled_ = true;  // guarded by mutex_
    // The server asked for the section list (it has none for this device).
    // Answered by registering again, once per process.
    bool sectionsReported_ = false;       // guarded by mutex_
    std::atomic<bool> reRegister_{false};
    // Bumped by ClearIdentity(). A registration response that comes back for
    // an older generation is discarded, so a detached thread cannot restore a
    // token that was just revoked.
    uint64_t identityGen_ = 0;  // guarded by mutex_
    int64_t lastAckMs_ = 0;     // guarded by mutex_
    int lastAckCount_ = 0;      // guarded by mutex_
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
