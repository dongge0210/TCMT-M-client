// Platform.h - Platform abstraction interface
#pragma once

#include <string>
#include <cstdint>
#include <memory>
#include <vector>

// Platform macro definitions - set in CMake
// Note: These macros are defined by CMake via add_definitions() or target_compile_definitions()
// No checks here, as validation should be done in the build system

// Platform-specific header includes (via conditional compilation) 
#ifdef TCMT_WINDOWS
#define WIN32_LEAN_AND_MEAN
// winsock2.h must be before windows.h
#include <winsock2.h>
#include <windows.h>
#include <synchapi.h>
#elif defined(TCMT_MACOS)
#include <sys/time.h>
#include <pthread.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
// Define HANDLE for non-Windows platforms
typedef void* HANDLE;
#elif defined(TCMT_LINUX)
#include <sys/time.h>
#include <pthread.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#endif

namespace Platform {

// ============================================================================
// System time type (cross-platform)
// ============================================================================

struct SystemTime {
    uint16_t year;          // Year
    uint16_t month;         // Month (1-12)
    uint16_t dayOfWeek;     // Day of week (0=Sunday, 6=Saturday)
    uint16_t day;           // Day (1-31)
    uint16_t hour;          // Hour (0-23)
    uint16_t minute;        // Minute (0-59)
    uint16_t second;        // Second (0-59)
    uint16_t milliseconds;  // Millisecond (0-999)

    SystemTime() : year(0), month(0), dayOfWeek(0), day(0),
                   hour(0), minute(0), second(0), milliseconds(0) {}

    // Convert to string
    std::string ToString() const;

    // Get current system time
    static SystemTime Now();

    // Comparison operators
    bool operator==(const SystemTime& other) const;
    bool operator!=(const SystemTime& other) const;
    bool operator<(const SystemTime& other) const;
    bool operator>(const SystemTime& other) const;
};

// ============================================================================
// Critical section/Mutex (cross-platform)
// ============================================================================

class CriticalSection {
public:
    CriticalSection();
    ~CriticalSection();

    // Disable copy
    CriticalSection(const CriticalSection&) = delete;
    CriticalSection& operator=(const CriticalSection&) = delete;

    // Allow move (if needed)
    CriticalSection(CriticalSection&& other) noexcept;
    CriticalSection& operator=(CriticalSection&& other) noexcept;

    // Lock
    void Enter();

    // Unlock
    void Leave();

    // Try lock
    bool TryEnter();

private:
#ifdef TCMT_WINDOWS
    CRITICAL_SECTION cs_;
#elif defined(TCMT_MACOS) || defined(TCMT_LINUX)
    pthread_mutex_t mutex_;
#endif
};

// RAII wrapper
class ScopedLock {
public:
    explicit ScopedLock(CriticalSection& cs) : cs_(cs) { cs_.Enter(); }
    ~ScopedLock() { cs_.Leave(); }

    // Disable copy
    ScopedLock(const ScopedLock&) = delete;
    ScopedLock& operator=(const ScopedLock&) = delete;

private:
    CriticalSection& cs_;
};

// ============================================================================
// Shared memory (cross-platform)
// ============================================================================

class SharedMemory {
public:
    SharedMemory();
    ~SharedMemory();

    // Create or open shared memory
    bool Create(const std::string& name, size_t size);

    // Open existing shared memory
    bool Open(const std::string& name, size_t size);

    // Map to process address space
    bool Map();

    // Unmap
    bool Unmap();

    // Get mapped address
    void* GetAddress() const { return address_; }

    // Get size
    size_t GetSize() const { return size_; }

    // Is created
    bool IsCreated() const { return created_; }

    // Get error info
    std::string GetLastError() const { return last_error_; }

private:
    void* address_;
    size_t size_;
    bool created_;
    std::string last_error_;

#ifdef TCMT_WINDOWS
    HANDLE hMapFile_;
#elif defined(TCMT_MACOS) || defined(TCMT_LINUX)
    int shm_fd_;
    std::string shm_name_;
#endif
};

// ============================================================================
// Interprocess mutex (cross-platform)
// ============================================================================

class InterprocessMutex {
public:
    InterprocessMutex();
    ~InterprocessMutex();

    // Create or open mutex
    bool Create(const std::string& name);

    // Open existing mutex
    bool Open(const std::string& name);

    // Lock
    bool Lock(uint32_t timeout_ms = 0xFFFFFFFF);

    // Unlock
    bool Unlock();

    // Get error info
    std::string GetLastError() const { return last_error_; }

private:
    std::string name_;
    std::string last_error_;

#ifdef TCMT_WINDOWS
    HANDLE mutex_;
#elif defined(TCMT_MACOS) || defined(TCMT_LINUX)
    pthread_mutex_t* mutex_ptr_;
    SharedMemory shm_;
    bool is_creator_;
#endif
};

// ============================================================================
// File handle wrapper (cross-platform)
// ============================================================================

class FileHandle {
public:
    FileHandle() : handle_(InvalidHandle()) {}
    ~FileHandle() { Close(); }

    // Create from existing handle (transfer ownership)
    explicit FileHandle(void* handle) : handle_(handle) {}

    // Disable copy
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;

    // Allow move
    FileHandle(FileHandle&& other) noexcept : handle_(other.handle_) {
        other.handle_ = InvalidHandle();
    }

    FileHandle& operator=(FileHandle&& other) noexcept {
        if (this != &other) {
            Close();
            handle_ = other.handle_;
            other.handle_ = InvalidHandle();
        }
        return *this;
    }

    // Check if valid
    bool IsValid() const { return handle_ != InvalidHandle(); }

    // Get raw handle (no ownership transfer)
    void* Get() const { return handle_; }

    // Close handle
    void Close();

    // Invalid handle value
    static void* InvalidHandle();

private:
    void* handle_;
};

// ============================================================================
// String conversion (cross-platform)
// ============================================================================

class StringConverter {
public:
    // UTF-8 to wide string (Windows) or UTF-16 (other platforms)
    static std::wstring Utf8ToWide(const std::string& utf8);

    // Wide string to UTF-8
    static std::string WideToUtf8(const std::wstring& wide);

    // UTF-8 to UTF-16 (char16_t) - cross-platform, always 2-byte chars
    static std::u16string Utf8ToChar16(const std::string& utf8);

    // UTF-16 (char16_t) to UTF-8
    static std::string Char16ToUtf8(const std::u16string& utf16);

    // Current code page to UTF-8 (mainly for Windows)
    static std::string AnsiToUtf8(const std::string& ansi);

    // UTF-8 to current code page (mainly for Windows)
    static std::string Utf8ToAnsi(const std::string& utf8);

    // Check if string is valid UTF-8
    static bool IsValidUtf8(const std::string& str);
};

// ============================================================================
// System utility functions (cross-platform)
// ============================================================================

class SystemUtils {
public:
    // Get system error info
    static std::string GetLastErrorString();

    // Sleep (milliseconds)
    static void Sleep(uint32_t milliseconds);

    // Get current process ID
    static uint64_t GetCurrentProcessId();

    // Get current thread ID
    static uint64_t GetCurrentThreadId();

    // Get system tick count (milliseconds)
    static uint64_t GetTickCount();

    // Get high resolution timestamp (microseconds)
    static uint64_t GetHighResolutionTime();

    // Get environment variable
    static std::string GetEnvironmentVariable(const std::string& name);

    // Set environment variable
    static bool SetEnvironmentVariable(const std::string& name, const std::string& value);
};

// ============================================================================
// Initialize/Cleanup functions
// ============================================================================

// Platform initialize (call at program start)
bool PlatformInitialize();

// Platform cleanup (call at program end)
void PlatformCleanup();

} // namespace Platform

// ============================================================================
// Convenient type aliases
// ============================================================================

using PlatformSystemTime = Platform::SystemTime;
using PlatformCriticalSection = Platform::CriticalSection;
using PlatformScopedLock = Platform::ScopedLock;
using PlatformSharedMemory = Platform::SharedMemory;
using PlatformInterprocessMutex = Platform::InterprocessMutex;
using PlatformFileHandle = Platform::FileHandle;

// ============================================================================
// Platform-specific function declarations (implemented via conditional compilation)
// ============================================================================

#ifdef TCMT_WINDOWS

// Windows-specific helper functions
namespace PlatformWindows {
    // Convert Windows SYSTEMTIME to Platform::SystemTime
    Platform::SystemTime ConvertSystemTime(const SYSTEMTIME& st);

    // Convert Platform::SystemTime to Windows SYSTEMTIME
    SYSTEMTIME ConvertToSystemTime(const Platform::SystemTime& pt);

    // Enable Windows privilege
    bool EnablePrivilege(const wchar_t* privilegeName);

    // Format Windows error message
    std::string FormatWindowsErrorMessage(uint32_t errorCode);
}

#elif defined(TCMT_MACOS)

// macOS-specific helper functions
namespace PlatformMacOS {
    // Get SMC (System Management Controller) access
    bool OpenSMCConnection();
    void CloseSMCConnection();

    // Read SMC key value
    bool ReadSMCKey(const char* key, uint32_t* outSize, void* outValue);

    // Get IOKit service
    void* GetIOKitService(const char* serviceName);

    // Release IOKit object
    void ReleaseIOKitObject(void* object);
}

#endif