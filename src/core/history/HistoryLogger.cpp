#include "HistoryLogger.h"

#ifndef _WIN32
#include <sqlite3.h>
#include <chrono>
#include <algorithm>
#include <cstring>
#endif

#ifndef _WIN32

// ============================================================================
// Construction / destruction
// ============================================================================

HistoryLogger::HistoryLogger()
    : lastRotationCheck_(std::chrono::steady_clock::now())
{
}

HistoryLogger::~HistoryLogger()
{
    Shutdown();
}

// ============================================================================
// Initialize / Shutdown / IsRunning
// ============================================================================

bool HistoryLogger::Initialize(const std::string& dbPath)
{
    dbPath_ = dbPath;

    // Open (or create) the SQLite database
    int rc = sqlite3_open(dbPath_.c_str(), &db_);
    if (rc != SQLITE_OK) {
        db_ = nullptr;
        return false;
    }

    // Enable WAL mode for better concurrent read/write performance
    char* errMsg = nullptr;
    rc = sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        sqlite3_free(errMsg);
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }

    // Create the schema
    if (!CreateTables()) {
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }

    // Start the background flush thread
    running_.store(true);
    worker_ = std::thread(&HistoryLogger::RunLoop, this);

    return true;
}

void HistoryLogger::Shutdown()
{
    if (!running_.load())
        return;

    // Signal the worker to stop
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_.store(false);
        cv_.notify_one();
    }

    // Join the worker thread
    if (worker_.joinable())
        worker_.join();

    // Flush any remaining data that may still be in the pending queue
    std::vector<SensorSnapshot> remaining;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        remaining.swap(pending_);
    }
    if (!remaining.empty())
        FlushBatch(remaining);

    // Close database
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool HistoryLogger::IsRunning() const
{
    return running_.load();
}

// ============================================================================
// Public API
// ============================================================================

void HistoryLogger::WriteBatch(const std::vector<SensorSnapshot>& batch)
{
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.insert(pending_.end(), batch.begin(), batch.end());
    if (pending_.size() >= maxPending_)
        cv_.notify_one();
}

// ============================================================================
// Query interface
// ============================================================================

std::vector<SensorSnapshot>
HistoryLogger::GetSnapshots(const std::string& name, int limit)
{
    if (!db_)
        return {};

    static const char* sql =
        "SELECT name, value, units, timestamp_ms "
        "FROM sensors WHERE name = ? "
        "ORDER BY timestamp_ms DESC LIMIT ?;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK)
        return {};

    sqlite3_bind_text(stmt, 1, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, limit);

    std::vector<SensorSnapshot> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SensorSnapshot s;
        const char* rawName = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        s.name        = rawName ? rawName : "";
        s.value       = sqlite3_column_double(stmt, 1);
        const char* rawUnits = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        s.units       = rawUnits ? rawUnits : "";
        s.timestampMs = static_cast<uint64_t>(sqlite3_column_int64(stmt, 3));
        results.push_back(std::move(s));
    }

    sqlite3_finalize(stmt);
    return results;
}

std::vector<SensorSnapshot>
HistoryLogger::GetSnapshots(int64_t sinceTimestampMs, int limit)
{
    if (!db_)
        return {};

    static const char* sql =
        "SELECT name, value, units, timestamp_ms "
        "FROM sensors WHERE timestamp_ms >= ? "
        "ORDER BY timestamp_ms DESC LIMIT ?;";

    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK)
        return {};

    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(sinceTimestampMs));
    sqlite3_bind_int(stmt, 2, limit);

    std::vector<SensorSnapshot> results;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        SensorSnapshot s;
        const char* rawName = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        s.name        = rawName ? rawName : "";
        s.value       = sqlite3_column_double(stmt, 1);
        const char* rawUnits = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        s.units       = rawUnits ? rawUnits : "";
        s.timestampMs = static_cast<uint64_t>(sqlite3_column_int64(stmt, 3));
        results.push_back(std::move(s));
    }

    sqlite3_finalize(stmt);
    return results;
}

// ============================================================================
// Background worker
// ============================================================================

void HistoryLogger::RunLoop()
{
    while (running_.load()) {
        std::vector<SensorSnapshot> batch;

        // Dequeue all pending snapshots (block up to 1 second)
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (pending_.empty())
                cv_.wait_for(lock, std::chrono::seconds(1));
            if (!pending_.empty())
                batch.swap(pending_);
        }

        // Flush to disk
        if (!batch.empty())
            FlushBatch(batch);

        // Check data retention once per hour
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::hours>(
            now - lastRotationCheck_);
        if (elapsed.count() >= 1) {
            RotateIfNeeded();
            lastRotationCheck_ = now;
        }
    }
}

// ============================================================================
// SQLite helpers
// ============================================================================

bool HistoryLogger::CreateTables()
{
    const char* sql = R"(
        CREATE TABLE IF NOT EXISTS sensors (
            id          INTEGER PRIMARY KEY AUTOINCREMENT,
            name        TEXT    NOT NULL,
            value       REAL    NOT NULL,
            units       TEXT,
            timestamp_ms INTEGER NOT NULL
        );
    )";

    char* errMsg = nullptr;
    int rc = sqlite3_exec(db_, sql, nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        sqlite3_free(errMsg);
        return false;
    }

    // Index on timestamp_ms so RotateIfNeeded can efficiently DELETE old rows
    rc = sqlite3_exec(
        db_,
        "CREATE INDEX IF NOT EXISTS idx_sensors_timestamp_ms "
        "ON sensors(timestamp_ms);",
        nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        sqlite3_free(errMsg);
        return false;
    }

    return true;
}

void HistoryLogger::FlushBatch(const std::vector<SensorSnapshot>& batch)
{
    if (!db_ || batch.empty())
        return;

    // ---- transaction ----
    char* errMsg = nullptr;
    int rc = sqlite3_exec(db_, "BEGIN TRANSACTION;", nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        sqlite3_free(errMsg);
        return;
    }

    // ---- prepared statement ----
    const char* sql = "INSERT INTO sensors (name, value, units, timestamp_ms) "
                      "VALUES (?, ?, ?, ?);";
    sqlite3_stmt* stmt = nullptr;
    rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
        return;
    }

    // ---- bind & execute each row ----
    for (const auto& s : batch) {
        sqlite3_bind_text(stmt, 1, s.name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_double(stmt, 2, s.value);
        sqlite3_bind_text(stmt, 3, s.units.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 4,
                           static_cast<sqlite3_int64>(s.timestampMs));

        rc = sqlite3_step(stmt);
        if (rc != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
            return;
        }

        sqlite3_reset(stmt);
    }

    sqlite3_finalize(stmt);

    // ---- commit ----
    rc = sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, &errMsg);
    if (rc != SQLITE_OK) {
        sqlite3_free(errMsg);
        sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    }
}

void HistoryLogger::RotateIfNeeded()
{
    if (!db_)
        return;

    auto cutoff = std::chrono::system_clock::now()
                - std::chrono::hours(24 * retentionDays_);
    auto cutoffMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            cutoff.time_since_epoch()).count();

    const char* sql = "DELETE FROM sensors WHERE timestamp_ms < ?;";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK)
        return;

    sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(cutoffMs));

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return;
    }

    sqlite3_finalize(stmt);
}

#else  // _WIN32 — no sqlite3, provide stubs

HistoryLogger::HistoryLogger() {}
HistoryLogger::~HistoryLogger() {}
bool HistoryLogger::Initialize(const std::string&) { return false; }
void HistoryLogger::Shutdown() {}
bool HistoryLogger::IsRunning() const { return false; }
void HistoryLogger::WriteBatch(const std::vector<SensorSnapshot>&) {}
std::vector<SensorSnapshot> HistoryLogger::GetSnapshots(const std::string&, int) { return {}; }
std::vector<SensorSnapshot> HistoryLogger::GetSnapshots(int64_t, int) { return {}; }
void HistoryLogger::RunLoop() {}
bool HistoryLogger::CreateTables() { return false; }
void HistoryLogger::FlushBatch(const std::vector<SensorSnapshot>&) {}
void HistoryLogger::RotateIfNeeded() {}

#endif
