#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <cstdint>
#include <chrono>

struct sqlite3;

// ---------------------------------------------------------------------------
// SensorSnapshot: A single sensor reading at a point in time.
// ---------------------------------------------------------------------------
struct SensorSnapshot {
    std::string name;
    double      value      = 0.0;
    std::string units;
    uint64_t    timestampMs = 0;
};

// ---------------------------------------------------------------------------
// HistoryLogger: Background thread that buffers sensor snapshots and flushes
// them to a SQLite database on a 1-second interval.
// ---------------------------------------------------------------------------
class HistoryLogger {
public:
    HistoryLogger();
    ~HistoryLogger();

    HistoryLogger(const HistoryLogger&)            = delete;
    HistoryLogger& operator=(const HistoryLogger&) = delete;

    /// Open a SQLite database at |dbPath|, create tables, and start the
    /// background flush thread.  Returns false on failure.
    bool Initialize(const std::string& dbPath);

    /// Signal the background thread to stop, join it, flush remaining data,
    /// and close the database.
    void Shutdown();

    /// True while the background thread is running.
    bool IsRunning() const;

    /// Set retention in days (call before Initialize).
    void SetRetentionDays(int days) { retentionDays_ = days; }

    /// Enqueue one or more snapshots.  If the internal pending queue exceeds
    /// maxPending_, the background thread is woken immediately.
    void WriteBatch(const std::vector<SensorSnapshot>& batch);

    /// Retrieve recent snapshots for a specific sensor name.
    /// The result is ordered by timestamp descending (most recent first).
    /// @param name  Sensor name to look up.
    /// @param limit Maximum number of rows to return (default 100).
    /// @return A vector of SensorSnapshot values, empty if the sensor is not found.
    /// @note On Windows this always returns an empty vector.
    std::vector<SensorSnapshot> GetSnapshots(const std::string& name, int limit = 100);

    /// Retrieve snapshots recorded at or after |sinceTimestampMs|.
    /// The result is ordered by timestamp descending (most recent first).
    /// @param sinceTimestampMs  Epoch-millisecond threshold (inclusive).
    /// @param limit             Maximum number of rows to return (default 100).
    /// @note On Windows this always returns an empty vector.
    std::vector<SensorSnapshot> GetSnapshots(int64_t sinceTimestampMs, int limit = 100);

private:
    void RunLoop();
    bool CreateTables();
    void FlushBatch(const std::vector<SensorSnapshot>& batch);
    void RotateIfNeeded();

    std::string dbPath_;
    sqlite3*    db_ = nullptr;

    std::atomic<bool>                    running_{false};
    std::thread                          worker_;
    mutable std::mutex                   mutex_;
    std::condition_variable              cv_;
    std::vector<SensorSnapshot>          pending_;
    size_t                               maxPending_        = 1000;
    int                                  retentionDays_      = 7;

    std::chrono::steady_clock::time_point lastRotationCheck_;
};
