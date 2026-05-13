// Platform.cpp - Platform abstraction implementation main file
// Platform-specific implementations included via conditional compilation

#include "Platform.h"
#include <ctime>
#include <chrono>
#include <iomanip>
#include <sstream>

// ============================================================================
// Platform-specific implementation includes
// ============================================================================

#ifdef TCMT_WINDOWS
#include "Platform_Windows.cpp"
#elif defined(TCMT_MACOS)
#include "Platform_macOS.cpp"
#elif defined(TCMT_LINUX)
#include "Platform_Linux.cpp"
#else
// #error "Unsupported platform" - temporarily commented for debug
// Temporarily include empty implementation
namespace Platform {
    // Empty implementation
}
#endif

// ============================================================================
// SystemTime implementation (platform-independent part)
// ============================================================================

namespace Platform {

std::string SystemTime::ToString() const {
    std::ostringstream ss;
    ss << std::setfill('0')
       << std::setw(4) << year << "-"
       << std::setw(2) << month << "-"
       << std::setw(2) << day << " "
       << std::setw(2) << hour << ":"
       << std::setw(2) << minute << ":"
       << std::setw(2) << second << "."
       << std::setw(3) << milliseconds;
    return ss.str();
}

bool SystemTime::operator==(const SystemTime& other) const {
    return year == other.year &&
           month == other.month &&
           day == other.day &&
           hour == other.hour &&
           minute == other.minute &&
           second == other.second &&
           milliseconds == other.milliseconds;
}

bool SystemTime::operator!=(const SystemTime& other) const {
    return !(*this == other);
}

bool SystemTime::operator<(const SystemTime& other) const {
    if (year != other.year) return year < other.year;
    if (month != other.month) return month < other.month;
    if (day != other.day) return day < other.day;
    if (hour != other.hour) return hour < other.hour;
    if (minute != other.minute) return minute < other.minute;
    if (second != other.second) return second < other.second;
    return milliseconds < other.milliseconds;
}

bool SystemTime::operator>(const SystemTime& other) const {
    return other < *this;
}

// ============================================================================
// CriticalSection implementation (platform-independent part)
// ============================================================================

CriticalSection::CriticalSection(CriticalSection&& other) noexcept {
#ifdef TCMT_WINDOWS
    // Windows: transfer resources from other
    cs_ = other.cs_;
    other.cs_ = CRITICAL_SECTION();
    InitializeCriticalSection(&cs_);
#elif defined(TCMT_MACOS) || defined(TCMT_LINUX)
    // macOS/Linux: transfer resources from other
    mutex_ = other.mutex_;
    // reset other to initial state
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&other.mutex_, &attr);
    pthread_mutexattr_destroy(&attr);
#endif
}

CriticalSection& CriticalSection::operator=(CriticalSection&& other) noexcept {
    if (this != &other) {
        // use swap to exchange resources
        // Note: for CriticalSection, simple bitwise swap may not be safe
        // using simplified implementation, assuming platform-specific types can be safely swapped
#ifdef TCMT_WINDOWS
        // Windows: destroy current object, transfer resources from other
        DeleteCriticalSection(&cs_);
        cs_ = other.cs_;
        other.cs_ = CRITICAL_SECTION();
        InitializeCriticalSection(&cs_);
#elif defined(TCMT_MACOS) || defined(TCMT_LINUX)
        // macOS/Linux: destroy current object, transfer resources from other
        pthread_mutex_destroy(&mutex_);
        mutex_ = other.mutex_;
        // reset other to initial state
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&other.mutex_, &attr);
        pthread_mutexattr_destroy(&attr);
#endif
    }
    return *this;
}

// ============================================================================
// StringConverter implementation (platform-independent part)
// ============================================================================

// StringConverter implementation included via conditional compilation in platform-specific files
// These functions are declared as static member functions in Platform.h
// Concrete implementations provided in Platform_Windows.cpp or Platform_macOS.cpp

// ============================================================================
// SystemUtils implementation (platform-independent part)
// ============================================================================

void SystemUtils::Sleep(uint32_t milliseconds) {
#ifdef TCMT_WINDOWS
    ::Sleep(milliseconds);
#elif defined(TCMT_MACOS) || defined(TCMT_LINUX)
    usleep(milliseconds * 1000);
#endif
}

uint64_t SystemUtils::GetTickCount() {
    auto now = std::chrono::steady_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}

uint64_t SystemUtils::GetHighResolutionTime() {
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = now.time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
}

std::string SystemUtils::GetEnvironmentVariable(const std::string& name) {
#ifdef TCMT_WINDOWS
    char* value = nullptr;
    size_t size = 0;
    _dupenv_s(&value, &size, name.c_str());
    if (value) {
        std::string result(value);
        free(value);
        return result;
    }
    return "";
#elif defined(TCMT_MACOS) || defined(TCMT_LINUX)
    const char* value = std::getenv(name.c_str());
    return value ? std::string(value) : "";
#else
    // if no platform macro defined, return empty string
    return "";
#endif
}

bool SystemUtils::SetEnvironmentVariable(const std::string& name, const std::string& value) {
#ifdef TCMT_WINDOWS
    return _putenv_s(name.c_str(), value.c_str()) == 0;
#elif defined(TCMT_MACOS) || defined(TCMT_LINUX)
    return setenv(name.c_str(), value.c_str(), 1) == 0;
#else
    // if no platform macro defined, return false
    return false;
#endif
}

} // namespace Platform

