#pragma once
#include <string>
#include <fstream>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <vector>
#include <algorithm>
#ifdef TCMT_WINDOWS
// winsock2.h must be before windows.h
#include <winsock2.h>
#include <windows.h>
#endif

// Include LogBuffer for TUI support (macOS, Linux, and Windows)
#if defined(TCMT_MACOS) || defined(TCMT_LINUX) || defined(TCMT_WINDOWS)
#include "../tui/LogBuffer.h"
#endif

// Log level enumeration
enum LogLevel {
    LOG_TRACE = 0,
    LOG_DEBUG = 1,
    LOG_INFO = 2,
    LOG_WARNING = 3,
    LOG_ERROR = 4,
    LOG_CRITICAL = 5,
    LOG_FATAL = 6
};

// Console color enumeration
enum class ConsoleColor {
    BLACK = 0, DARK_BLUE = 1, DARK_GREEN = 2, DARK_CYAN = 3,
    DARK_RED = 4, DARK_MAGENTA = 5, DARK_YELLOW = 6, LIGHT_GRAY = 7,
    DARK_GRAY = 8, LIGHT_BLUE = 9, LIGHT_GREEN = 10, LIGHT_CYAN = 11,
    LIGHT_RED = 12, LIGHT_MAGENTA = 13, YELLOW = 14, WHITE = 15,
    PURPLE = 13, GREEN = 10, ORANGE = 12, RED = 12
};

class Logger {
private:
    static std::ofstream logFile;
    static std::mutex logMutex;
    static bool consoleOutputEnabled;
    static LogLevel currentLogLevel;
#ifdef TCMT_WINDOWS
    static void* hConsole;
#else
    static void* hConsole;
#endif

    // Async logging members
    static std::vector<std::string> logQueue;
    static std::condition_variable queueCV;
    static std::thread workerThread;
    static std::atomic<bool> shutdownFlag;
    static std::atomic<bool> logFileOpen;

    static void WorkerThreadFunc();
    static void WriteLog(const std::string& level, const std::string& message, LogLevel msgLevel, ConsoleColor color);
    static void SetConsoleColor(ConsoleColor color);
    static void ResetConsoleColor();

public:
    static void Initialize(const std::string& logFilePath);
    static void EnableConsoleOutput(bool enable);
    static void SetLogLevel(LogLevel level);
    static LogLevel GetLogLevel();
    static bool IsInitialized();
    static void Trace(const std::string& message);
    static void Debug(const std::string& message);
    static void Info(const std::string& message);
    static void Warn(const std::string& message);
    static void Error(const std::string& message);
    static void Critical(const std::string& message);
    static void Fatal(const std::string& message);
    static void Flush();
    static void Shutdown();

#if defined(TCMT_MACOS) || defined(TCMT_LINUX) || defined(_WIN32)
    // Get the TUI log buffer (for TUI mode)
    static tcmt::LogBuffer& GetTuiBuffer();
#endif
};