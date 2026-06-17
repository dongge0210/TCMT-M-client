#include "Logger.h"

#if defined(TCMT_MACOS) || defined(TCMT_LINUX) || defined(_WIN32)
#include "../../tui/LogBuffer.h"
#endif

#include <filesystem>

std::ofstream Logger::logFile;
std::mutex Logger::logMutex;
bool Logger::consoleOutputEnabled = true;
LogLevel Logger::currentLogLevel = LOG_DEBUG;
#ifdef TCMT_WINDOWS
void* Logger::hConsole = nullptr;
#else
void* Logger::hConsole = nullptr;
#endif

// Async logging members
std::vector<std::string> Logger::logQueue;
std::condition_variable Logger::queueCV;
std::thread Logger::workerThread;
std::atomic<bool> Logger::shutdownFlag{false};
std::atomic<bool> Logger::logFileOpen{false};

#if defined(TCMT_MACOS) || defined(TCMT_LINUX) || defined(_WIN32)
// Global TUI log buffer (for TUI mode)
static tcmt::LogBuffer g_tuiLogBuffer;
#endif

// ======================== Async Worker Thread ========================
void Logger::WorkerThreadFunc() {
    std::vector<std::string> localQueue;
    while (true) {
        {
            std::unique_lock<std::mutex> lock(logMutex);
            queueCV.wait(lock, [] { return shutdownFlag.load() || !logQueue.empty(); });
            localQueue.swap(logQueue);
        }

        // Write batch to file
        for (const auto& entry : localQueue) {
            logFile.write(entry.c_str(), entry.size());
        }
        localQueue.clear();

        if (shutdownFlag.load()) {
            // Check for entries that arrived between swap and flag check
            std::lock_guard<std::mutex> lock(logMutex);
            if (logQueue.empty()) break;
            localQueue.swap(logQueue);
        }
    }

    // Final drain of localQueue (from the shutdown-path swap above)
    for (const auto& entry : localQueue) {
        logFile.write(entry.c_str(), entry.size());
    }
    localQueue.clear();

    logFile.flush();
}

// Cleanup guard for static destruction ordering: ensures the worker
// thread is joined before its static members are destroyed.
namespace {
    struct LogTerminator {
        ~LogTerminator() {
            Logger::Shutdown();
        }
    } logTerminator;
}

#ifdef TCMT_WINDOWS
// ======================== Windows Implementation ========================
// NOTE: winsock2.h must be included BEFORE windows.h
#include <winsock2.h>
#include <windows.h>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <iostream>
#include <stdexcept>
#include <algorithm>
#include <vector>

void Logger::Initialize(const std::string& logFilePath) {
    // Rotate log if it exceeds ~10 MB
    std::error_code ec;
    auto fileSize = std::filesystem::file_size(logFilePath, ec);
    if (!ec && fileSize > 10 * 1024 * 1024) {
        std::filesystem::rename(logFilePath, logFilePath + ".old", ec);
        (void)ec;
    }
    logFile.open(logFilePath, std::ios::binary | std::ios::app);
    if (!logFile.is_open()) {
        throw std::runtime_error("Cannot open log file");
    }
    if (logFile.tellp() == 0) {
        const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
        logFile.write(reinterpret_cast<const char*>(bom), sizeof(bom));
    }
    SetConsoleCP(65001);
    SetConsoleOutputCP(65001);
    hConsole = GetStdHandle(STD_OUTPUT_HANDLE);

    // Start async worker thread
    logFileOpen.store(true);
    workerThread = std::thread(WorkerThreadFunc);
}

void Logger::EnableConsoleOutput(bool enable) { consoleOutputEnabled = enable; }
void Logger::SetLogLevel(LogLevel level) { currentLogLevel = level; }
LogLevel Logger::GetLogLevel() { return currentLogLevel; }
bool Logger::IsInitialized() { return logFile.is_open(); }

void Logger::SetConsoleColor(ConsoleColor color) {
    if (hConsole != nullptr && hConsole != INVALID_HANDLE_VALUE)
        SetConsoleTextAttribute((HANDLE)hConsole, static_cast<WORD>(color));
}

void Logger::ResetConsoleColor() {
    if (hConsole != nullptr && hConsole != INVALID_HANDLE_VALUE)
        SetConsoleTextAttribute((HANDLE)hConsole, 7);
}

void Logger::WriteLog(const std::string& level, const std::string& message,
                      LogLevel msgLevel, ConsoleColor color) {
    if (msgLevel < currentLogLevel) return;
    constexpr size_t MAX_LOG_LENGTH = 4096;
    if (message.empty() || message.length() > MAX_LOG_LENGTH) return;

    auto now = std::chrono::system_clock::now();
    auto time_now = std::chrono::system_clock::to_time_t(now);
    std::tm timeinfo;
    if (localtime_s(&timeinfo, &time_now) != 0) return;

    std::stringstream ss;
    ss << "[" << std::put_time(&timeinfo, "%Y-%m-%d %H:%M:%S") << "]"
       << "[" << level << "] " << message << "\n";
    std::string logEntry = ss.str();

    // Push to TUI buffer before moving string to async queue
#if defined(TCMT_MACOS) || defined(TCMT_LINUX) || defined(_WIN32)
    g_tuiLogBuffer.Push(logEntry);
#endif

    // Push to async queue (non-blocking for caller)
    {
        std::lock_guard<std::mutex> lock(logMutex);
        if (!logFileOpen.load()) return;
        logQueue.push_back(std::move(logEntry));
    }
    queueCV.notify_one();

    if (consoleOutputEnabled && hConsole != INVALID_HANDLE_VALUE) {
        HANDLE hCon = (HANDLE)hConsole;
        DWORD written = 0;
        SetConsoleColor(color);
        WriteFile(hCon, ss.str().c_str(), (DWORD)ss.str().size(), &written, nullptr);
        ResetConsoleColor();
    }
}

void Logger::Trace(const std::string& message)    { WriteLog("TRACE", message, LOG_TRACE, ConsoleColor::PURPLE); }
void Logger::Debug(const std::string& message)    { WriteLog("DEBUG", message, LOG_DEBUG, ConsoleColor::PURPLE); }
void Logger::Info(const std::string& message)     { WriteLog("INFO",  message, LOG_INFO,  ConsoleColor::LIGHT_GREEN); }
void Logger::Warn(const std::string& message)     { WriteLog("WARN",  message, LOG_WARNING, ConsoleColor::YELLOW); }
void Logger::Error(const std::string& message)    { WriteLog("ERROR", message, LOG_ERROR, ConsoleColor::ORANGE); }
void Logger::Critical(const std::string& message){ WriteLog("CRITICAL", message, LOG_CRITICAL, ConsoleColor::RED); }
void Logger::Fatal(const std::string& message)   { WriteLog("FATAL", message, LOG_FATAL, ConsoleColor::DARK_RED); }

#elif defined(TCMT_MACOS)
// ======================== macOS Implementation ========================
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <iostream>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>

// ANSI color codes for macOS terminal
#define ANSI_RESET   "\033[0m"
#define ANSI_FG_BLACK   "\033[30m"
#define ANSI_FG_RED     "\033[31m"
#define ANSI_FG_GREEN   "\033[32m"
#define ANSI_FG_YELLOW  "\033[33m"
#define ANSI_FG_BLUE    "\033[34m"
#define ANSI_FG_MAGENTA "\033[35m"
#define ANSI_FG_CYAN    "\033[36m"
#define ANSI_FG_WHITE   "\033[37m"
#define ANSI_FG_BRIGHT_BLACK   "\033[90m"
#define ANSI_FG_BRIGHT_RED     "\033[91m"
#define ANSI_FG_BRIGHT_GREEN   "\033[92m"
#define ANSI_FG_BRIGHT_YELLOW  "\033[93m"
#define ANSI_FG_BRIGHT_BLUE    "\033[94m"
#define ANSI_FG_BRIGHT_MAGENTA "\033[95m"
#define ANSI_FG_BRIGHT_CYAN    "\033[96m"
#define ANSI_FG_BRIGHT_WHITE   "\033[97m"

// Check if stdout is a TTY (supports colors)
static bool IsTTY() {
    return isatty(fileno(stdout)) == 1;
}

void Logger::Initialize(const std::string& logFilePath) {
    // Rotate log if it exceeds ~10 MB
    std::error_code ec;
    auto fileSize = std::filesystem::file_size(logFilePath, ec);
    if (!ec && fileSize > 10 * 1024 * 1024) {
        std::filesystem::rename(logFilePath, logFilePath + ".old", ec);
        (void)ec;
    }
    logFile.open(logFilePath, std::ios::binary | std::ios::app);
    if (!logFile.is_open()) {
        throw std::runtime_error("Cannot open log file: " + logFilePath);
    }
    if (logFile.tellp() == 0) {
        const unsigned char bom[] = {0xEF, 0xBB, 0xBF};
        logFile.write(reinterpret_cast<const char*>(bom), sizeof(bom));
    }
    consoleOutputEnabled = IsTTY();

    // Start async worker thread
    logFileOpen.store(true);
    workerThread = std::thread(WorkerThreadFunc);
}

void Logger::EnableConsoleOutput(bool enable) { consoleOutputEnabled = enable; }
void Logger::SetLogLevel(LogLevel level) { currentLogLevel = level; }
LogLevel Logger::GetLogLevel() { return currentLogLevel; }
bool Logger::IsInitialized() { return logFile.is_open(); }

void Logger::SetConsoleColor(ConsoleColor /*color*/) {
    // No-op for macOS; color handled in WriteLog using ANSI codes
}
void Logger::ResetConsoleColor() {
    if (consoleOutputEnabled) printf("%s", ANSI_RESET);
}

void Logger::WriteLog(const std::string& level, const std::string& message,
                      LogLevel msgLevel, ConsoleColor /*color*/) {
    if (msgLevel < currentLogLevel) return;
    constexpr size_t MAX_LOG_LENGTH = 4096;
    if (message.empty() || message.length() > MAX_LOG_LENGTH) return;

    auto now = std::chrono::system_clock::now();
    auto time_now = std::chrono::system_clock::to_time_t(now);
    std::tm timeinfo;
    localtime_r(&time_now, &timeinfo);

    std::stringstream ss;
    ss << "[" << std::put_time(&timeinfo, "%Y-%m-%d %H:%M:%S") << "]"
       << "[" << level << "] " << message << "\n";
    std::string logEntry = ss.str();

    // Push to TUI log buffer before moving string to async queue
#if defined(TCMT_MACOS) || defined(TCMT_LINUX) || defined(_WIN32)
    g_tuiLogBuffer.Push(logEntry);
#endif

    // Push to async queue (non-blocking for caller)
    {
        std::lock_guard<std::mutex> lock(logMutex);
        if (!logFileOpen.load()) return;
        logQueue.push_back(std::move(logEntry));
    }
    queueCV.notify_one();

    if (consoleOutputEnabled) {
        const char* colorCode = ANSI_RESET;
        if (msgLevel <= LOG_DEBUG) colorCode = ANSI_FG_BRIGHT_MAGENTA;
        else if (msgLevel == LOG_INFO) colorCode = ANSI_FG_BRIGHT_GREEN;
        else if (msgLevel == LOG_WARNING) colorCode = ANSI_FG_BRIGHT_YELLOW;
        else if (msgLevel == LOG_ERROR) colorCode = ANSI_FG_BRIGHT_RED;
        else colorCode = ANSI_FG_RED;

        char timeBuf[64];
        strftime(timeBuf, sizeof(timeBuf), "%Y-%m-%d %H:%M:%S", &timeinfo);
        printf("%s[%s]%s[%s] %s%s%s\n",
               ANSI_FG_BRIGHT_BLACK, timeBuf,
               ANSI_RESET, colorCode,
               level.c_str(),
               ANSI_RESET,
               message.c_str());
        fflush(stdout);
    }
}

void Logger::Trace(const std::string& message)    { WriteLog("TRACE",   message, LOG_TRACE,   ConsoleColor::PURPLE); }
void Logger::Debug(const std::string& message)     { WriteLog("DEBUG",   message, LOG_DEBUG,   ConsoleColor::PURPLE); }
void Logger::Info(const std::string& message)     { WriteLog("INFO",    message, LOG_INFO,    ConsoleColor::GREEN); }
void Logger::Warn(const std::string& message)     { WriteLog("WARN",    message, LOG_WARNING, ConsoleColor::YELLOW); }
void Logger::Error(const std::string& message)    { WriteLog("ERROR",   message, LOG_ERROR,   ConsoleColor::ORANGE); }
void Logger::Critical(const std::string& message) { WriteLog("CRITICAL",message, LOG_CRITICAL,ConsoleColor::RED); }
void Logger::Fatal(const std::string& message)    { WriteLog("FATAL",   message, LOG_FATAL,   ConsoleColor::RED); }

#else
// ======================== Linux / Fallback ========================
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <iostream>

void Logger::Initialize(const std::string& logFilePath) {
    // Rotate log if it exceeds ~10 MB
    std::error_code ec;
    auto fileSize = std::filesystem::file_size(logFilePath, ec);
    if (!ec && fileSize > 10 * 1024 * 1024) {
        std::filesystem::rename(logFilePath, logFilePath + ".old", ec);
        (void)ec;
    }
    logFile.open(logFilePath, std::ios::binary | std::ios::app);
    if (!logFile.is_open()) {
        throw std::runtime_error("Cannot open log file");
    }
    consoleOutputEnabled = true;

    // Start async worker thread
    logFileOpen.store(true);
    workerThread = std::thread(WorkerThreadFunc);
}
void Logger::EnableConsoleOutput(bool enable) { consoleOutputEnabled = enable; }
void Logger::SetLogLevel(LogLevel level) { currentLogLevel = level; }
LogLevel Logger::GetLogLevel() { return currentLogLevel; }
bool Logger::IsInitialized() { return logFile.is_open(); }
void Logger::SetConsoleColor(ConsoleColor) {}
void Logger::ResetConsoleColor() {}

void Logger::WriteLog(const std::string& level, const std::string& message,
                      LogLevel msgLevel, ConsoleColor) {
    if (msgLevel < currentLogLevel) return;
    auto now = std::chrono::system_clock::now();
    auto time_now = std::chrono::system_clock::to_time_t(now);
    std::tm timeinfo;
    localtime_r(&time_now, &timeinfo);
    std::stringstream ss;
    ss << "[" << std::put_time(&timeinfo, "%Y-%m-%d %H:%M:%S") << "]"
       << "[" << level << "] " << message << "\n";
    std::string logEntry = ss.str();

    // Console output before moving logEntry to queue
    if (consoleOutputEnabled) {
        printf("%s", logEntry.c_str());
        fflush(stdout);
    }

    // Push to async queue (non-blocking for caller)
    {
        std::lock_guard<std::mutex> lock(logMutex);
        if (!logFileOpen.load()) return;
        logQueue.push_back(std::move(logEntry));
    }
    queueCV.notify_one();
}

void Logger::Trace(const std::string& message)    { WriteLog("TRACE",   message, LOG_TRACE,   ConsoleColor::PURPLE); }
void Logger::Debug(const std::string& message)    { WriteLog("DEBUG",   message, LOG_DEBUG,   ConsoleColor::PURPLE); }
void Logger::Info(const std::string& message)     { WriteLog("INFO",    message, LOG_INFO,    ConsoleColor::GREEN); }
void Logger::Warn(const std::string& message)     { WriteLog("WARN",    message, LOG_WARNING, ConsoleColor::YELLOW); }
void Logger::Error(const std::string& message)    { WriteLog("ERROR",   message, LOG_ERROR,   ConsoleColor::ORANGE); }
void Logger::Critical(const std::string& message) { WriteLog("CRITICAL",message, LOG_CRITICAL,ConsoleColor::RED); }
void Logger::Fatal(const std::string& message)    { WriteLog("FATAL",   message, LOG_FATAL,   ConsoleColor::RED); }

#endif

void Logger::Flush() {
    // Signal the worker to process any pending entries
    queueCV.notify_one();

    // Drain entries that the worker may not have picked up yet.
    // We take a short lock, swap, then write synchronously.
    std::vector<std::string> entries;
    {
        std::lock_guard<std::mutex> lock(logMutex);
        entries.swap(logQueue);
    }
    for (const auto& entry : entries) {
        logFile.write(entry.c_str(), entry.size());
    }
    logFile.flush();
}

void Logger::Shutdown() {
    shutdownFlag.store(true);
    queueCV.notify_one();
    if (workerThread.joinable()) {
        workerThread.join();
    }

    // Drain any entries that arrived after the worker exited
    std::vector<std::string> remaining;
    {
        std::lock_guard<std::mutex> lock(logMutex);
        remaining.swap(logQueue);
    }
    for (const auto& entry : remaining) {
        logFile.write(entry.c_str(), entry.size());
    }

    logFile.flush();
    logFileOpen.store(false);
    if (logFile.is_open()) {
        logFile.close();
    }
}

#if defined(TCMT_MACOS) || defined(TCMT_LINUX) || defined(_WIN32)
// GetTuiBuffer - returns global log buffer for TUI
tcmt::LogBuffer& Logger::GetTuiBuffer() {
    return g_tuiLogBuffer;
}
#endif