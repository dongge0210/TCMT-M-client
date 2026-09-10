#pragma once

#include <string>
#include <atomic>
#include <thread>
#include <mutex>

// Self-update: at startup checks GitHub Releases for a newer version, then
// (on user request) downloads the package, verifies it against an embedded
// Ed25519 public key, and swaps the running binary. Requires TCMT_USE_TLS
// (OpenSSL) for HTTPS — a no-op without it (e.g. Windows builds for now).
class Updater {
public:
    enum class State { Idle, Checking, Available, Downloading, Verifying, Ready, Failed };

    Updater() = default;
    ~Updater() { Stop(); }

    // Path of the running binary (argv[0]); must be set before use.
    void SetExePath(const std::string& p) { exePath_ = p; }

    // Non-blocking; runs the check on a background thread.
    void CheckForUpdate();
    // Non-blocking; downloads + verifies the update found by CheckForUpdate.
    void StartDownload();
    void Stop();

    State GetState() const;
    std::string StatusText() const;   // human-readable, for the TUI
    std::string LatestVersion() const;

private:
    void CheckThread();
    void DownloadThread();
    bool HttpsGet(const std::string& url, std::string& out);
    bool VerifyManifest(const std::string& manifestJson, const std::string& sigB64);

    std::string exePath_;
    std::string latestVersion_;
    std::string assetUrl_;
    std::string manifestJson_;
    std::string sigB64_;
    std::atomic<int> state_{0};
    std::atomic<bool> running_{false};
    mutable std::mutex mutex_;
    std::string status_;
    std::thread thread_;
};
