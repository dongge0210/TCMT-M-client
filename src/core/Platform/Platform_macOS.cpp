// Platform_macOS.cpp - macOS platform-specific implementation

// This file is only compiled when TCMT_MACOS is defined
// Included via conditional compilation in Platform.cpp

#ifndef TCMT_MACOS
#error "This file should only be compiled for macOS platform (TCMT_MACOS defined)"
#endif

#include "Platform.h"
#include <sys/time.h>
#include <mach/mach_time.h>
#include <mach/mach_init.h>
#include <mach/thread_act.h>
#include <mach/mach_port.h>
#include <mach/mach_host.h>
#include <mach/vm_map.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <cstring>

namespace Platform {

// ============================================================================
// SystemTime implementation (macOS-specific)
// ============================================================================

SystemTime SystemTime::Now() {
    timeval tv;
    gettimeofday(&tv, nullptr);

    time_t rawtime = tv.tv_sec;
    struct tm* timeinfo = gmtime(&rawtime);

    SystemTime result;
    result.year = timeinfo->tm_year + 1900;
    result.month = timeinfo->tm_mon + 1;
    result.dayOfWeek = timeinfo->tm_wday;
    result.day = timeinfo->tm_mday;
    result.hour = timeinfo->tm_hour;
    result.minute = timeinfo->tm_min;
    result.second = timeinfo->tm_sec;
    result.milliseconds = tv.tv_usec / 1000;

    return result;
}

// ============================================================================
// CriticalSection implementation (macOS-specific)
// ============================================================================

CriticalSection::CriticalSection() {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&mutex_, &attr);
    pthread_mutexattr_destroy(&attr);
}

CriticalSection::~CriticalSection() {
    pthread_mutex_destroy(&mutex_);
}

void CriticalSection::Enter() {
    pthread_mutex_lock(&mutex_);
}

void CriticalSection::Leave() {
    pthread_mutex_unlock(&mutex_);
}

bool CriticalSection::TryEnter() {
    return pthread_mutex_trylock(&mutex_) == 0;
}

// ============================================================================
// SharedMemory implementation (macOS-specific)
// ============================================================================

SharedMemory::SharedMemory()
    : address_(nullptr), size_(0), created_(false), shm_fd_(-1) {
}

SharedMemory::~SharedMemory() {
    Unmap();
}

bool SharedMemory::Create(const std::string& name, size_t size) {
    Unmap();

    // POSIX shm_open names: must start with '/', max NAME_MAX (31 on macOS) chars.
    // InterprocessMutex appends "_mutex" to name internally, so base name must
    // leave headroom. Use a 20-char limit (20 + 7 + 1 = 28 < 31).
    std::string safe_name = name;
    size_t pos = safe_name.find('/');
    while (pos != std::string::npos) { safe_name.erase(pos, 1); pos = safe_name.find('/'); }
    if (safe_name.size() > 20) safe_name = safe_name.substr(0, 20);
    shm_name_ = "/" + safe_name;

    // macOS: shm_unlink does NOT allow immediate re-creation of same name in same
    // process if the kernel hasn't fully freed the object. Always unlink first.
    shm_unlink(shm_name_.c_str());

    // Create shared memory object
    shm_fd_ = shm_open(shm_name_.c_str(), O_CREAT | O_RDWR, 0666);
    if (shm_fd_ == -1) {
        last_error_ = "shm_open(" + shm_name_ + ") failed: " + strerror(errno);
        return false;
    }

    // Set size (macOS cannot shrink existing shm, so ensure shm_unlink is called first) 
    if (ftruncate(shm_fd_, static_cast<off_t>(size)) == -1) {
        last_error_ = "ftruncate(" + shm_name_ + ") failed: " + strerror(errno);
        close(shm_fd_);
        shm_fd_ = -1;
        return false;
    }

    size_ = size;
    created_ = true;
    return Map();
}

bool SharedMemory::Open(const std::string& name, size_t size) {
    Unmap();

    // Same safe name transformation as Create (must match Create exactly)
    std::string safe_name = name;
    size_t pos = safe_name.find('/');
    while (pos != std::string::npos) { safe_name.erase(pos, 1); pos = safe_name.find('/'); }
    if (safe_name.size() > 20) safe_name = safe_name.substr(0, 20);
    shm_name_ = "/" + safe_name;

    shm_fd_ = shm_open(shm_name_.c_str(), O_RDWR, 0);
    if (shm_fd_ == -1) {
        last_error_ = "shm_open(" + shm_name_ + ") failed: " + strerror(errno);
        return false;
    }

    size_ = size;
    created_ = false;
    return Map();
}

bool SharedMemory::Map() {
    if (shm_fd_ == -1) {
        last_error_ = "No shared memory file descriptor";
        return false;
    }

    address_ = mmap(nullptr, size_, PROT_READ | PROT_WRITE,
                    MAP_SHARED, shm_fd_, 0);

    if (address_ == MAP_FAILED) {
        address_ = nullptr;
        last_error_ = "mmap failed: " + std::string(strerror(errno));
        return false;
    }

    return true;
}

bool SharedMemory::Unmap() {
    if (address_) {
        munmap(address_, size_);
        address_ = nullptr;
    }

    if (shm_fd_ != -1) {
        close(shm_fd_);
        shm_fd_ = -1;

        // If creator, delete shared memory object
        if (created_) {
            shm_unlink(shm_name_.c_str());
        }
    }

    size_ = 0;
    created_ = false;
    shm_name_.clear();
    return true;
}

// ============================================================================
// InterprocessMutex implementation (macOS-specific)
// ============================================================================

InterprocessMutex::InterprocessMutex()
    : mutex_ptr_(nullptr), is_creator_(false) {
}

InterprocessMutex::~InterprocessMutex() {
    if (mutex_ptr_) {
        pthread_mutex_destroy(mutex_ptr_);
        delete mutex_ptr_;
    }
}

bool InterprocessMutex::Create(const std::string& name) {
    name_ = name;

    // Create shared memory to store mutex
    if (!shm_.Create(name + "_mutex", sizeof(pthread_mutex_t))) {
        last_error_ = "Failed to create shared memory for mutex: " + shm_.GetLastError();
        return false;
    }

    is_creator_ = true;
    mutex_ptr_ = static_cast<pthread_mutex_t*>(shm_.GetAddress());

    // Initialize interprocess mutex attributes
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);

    if (pthread_mutex_init(mutex_ptr_, &attr) != 0) {
        last_error_ = "Failed to initialize interprocess mutex";
        pthread_mutexattr_destroy(&attr);
        return false;
    }

    pthread_mutexattr_destroy(&attr);
    return true;
}

bool InterprocessMutex::Open(const std::string& name) {
    name_ = name;

    // Open existing shared memory
    if (!shm_.Open(name + "_mutex", sizeof(pthread_mutex_t))) {
        last_error_ = "Failed to open shared memory for mutex: " + shm_.GetLastError();
        return false;
    }

    is_creator_ = false;
    mutex_ptr_ = static_cast<pthread_mutex_t*>(shm_.GetAddress());
    return true;
}

bool InterprocessMutex::Lock(uint32_t timeout_ms) {
    if (!mutex_ptr_) {
        last_error_ = "Mutex not created or opened";
        return false;
    }

    // Simple implementation, no timeout support (POSIX mutex doesn't support timeout) 
    if (pthread_mutex_lock(mutex_ptr_) != 0) {
        last_error_ = "Failed to lock mutex";
        return false;
    }

    return true;
}

bool InterprocessMutex::Unlock() {
    if (!mutex_ptr_) {
        last_error_ = "Mutex not created or opened";
        return false;
    }

    if (pthread_mutex_unlock(mutex_ptr_) != 0) {
        last_error_ = "Failed to unlock mutex";
        return false;
    }

    return true;
}

// ============================================================================
// FileHandle implementation (macOS-specific)
// ============================================================================

void FileHandle::Close() {
    if (handle_ != InvalidHandle()) {
        close(reinterpret_cast<intptr_t>(handle_));
        handle_ = InvalidHandle();
    }
}

void* FileHandle::InvalidHandle() {
    return reinterpret_cast<void*>(-1);
}

// ============================================================================
// StringConverter implementation (macOS-specific)
// ============================================================================

std::wstring StringConverter::Utf8ToWide(const std::string& utf8) {
    // macOS uses UTF-8 as native encoding, wchar_t is 32-bit
    std::wstring result;
    result.reserve(utf8.size());

    const char* ptr = utf8.c_str();
    size_t len = utf8.size();
    size_t i = 0;

    while (i < len) {
        wchar_t wc = 0;
        unsigned char c = static_cast<unsigned char>(ptr[i]);

        if ((c & 0x80) == 0) {
            // 1-byte UTF-8
            wc = c;
            i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            // 2-byte UTF-8
            if (i + 1 < len) {
                wc = ((c & 0x1F) << 6) | (ptr[i + 1] & 0x3F);
                i += 2;
            } else {
                wc = '?';
                i += 1;
            }
        } else if ((c & 0xF0) == 0xE0) {
            // 3-byte UTF-8
            if (i + 2 < len) {
                wc = ((c & 0x0F) << 12) | ((ptr[i + 1] & 0x3F) << 6) | (ptr[i + 2] & 0x3F);
                i += 3;
            } else {
                wc = '?';
                i += 1;
            }
        } else if ((c & 0xF8) == 0xF0) {
            // 4-byte UTF-8 (requires surrogate pair, simplified) 
            wc = 0xFFFD; // Replacement character
            i += 4;
        } else {
            wc = '?';
            i += 1;
        }

        result.push_back(wc);
    }

    return result;
}

std::string StringConverter::WideToUtf8(const std::wstring& wide) {
    std::string result;
    result.reserve(wide.size() * 4); // max 4 bytes per character

    for (wchar_t wc : wide) {
        if (wc <= 0x7F) {
            // 1-byte UTF-8
            result.push_back(static_cast<char>(wc));
        } else if (wc <= 0x7FF) {
            // 2-byte UTF-8
            result.push_back(static_cast<char>(0xC0 | (wc >> 6)));
            result.push_back(static_cast<char>(0x80 | (wc & 0x3F)));
        } else if (wc <= 0xFFFF) {
            // 3-byte UTF-8
            result.push_back(static_cast<char>(0xE0 | (wc >> 12)));
            result.push_back(static_cast<char>(0x80 | ((wc >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (wc & 0x3F)));
        } else {
            // 4-byte UTF-8 (simplified, assumes wc <= 0x10FFFF) 
            result.push_back(static_cast<char>(0xF0 | (wc >> 18)));
            result.push_back(static_cast<char>(0x80 | ((wc >> 12) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | ((wc >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (wc & 0x3F)));
        }
    }

    return result;
}

// UTF-8 to char16_t (UTF-16) conversion
std::u16string StringConverter::Utf8ToChar16(const std::string& utf8) {
    std::u16string result;
    result.reserve(utf8.size());

    const char* ptr = utf8.c_str();
    size_t len = utf8.size();
    size_t i = 0;

    while (i < len) {
        unsigned char c = static_cast<unsigned char>(ptr[i]);
        char32_t cp;

        if ((c & 0x80) == 0) {
            cp = c;
            i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            if (i + 1 >= len) { cp = 0xFFFD; i += 1; }
            else { cp = ((c & 0x1F) << 6) | (static_cast<unsigned char>(ptr[i + 1]) & 0x3F); i += 2; }
        } else if ((c & 0xF0) == 0xE0) {
            if (i + 2 >= len) { cp = 0xFFFD; i += 1; }
            else { cp = ((c & 0x0F) << 12) | ((static_cast<unsigned char>(ptr[i + 1]) & 0x3F) << 6) | (static_cast<unsigned char>(ptr[i + 2]) & 0x3F); i += 3; }
        } else if ((c & 0xF8) == 0xF0) {
            if (i + 3 >= len) { cp = 0xFFFD; i += 1; }
            else {
                cp = ((c & 0x07) << 18) | ((static_cast<unsigned char>(ptr[i + 1]) & 0x3F) << 12)
                    | ((static_cast<unsigned char>(ptr[i + 2]) & 0x3F) << 6)
                    | (static_cast<unsigned char>(ptr[i + 3]) & 0x3F);
                i += 4;
            }
        } else {
            cp = 0xFFFD;
            i += 1;
        }

        // Encode as UTF-16
        if (cp <= 0xFFFF) {
            result.push_back(static_cast<char16_t>(cp));
        } else if (cp <= 0x10FFFF) {
            cp -= 0x10000;
            result.push_back(static_cast<char16_t>(0xD800 | (cp >> 10)));
            result.push_back(static_cast<char16_t>(0xDC00 | (cp & 0x3FF)));
        } else {
            result.push_back(static_cast<char16_t>(0xFFFD));
        }
    }

    return result;
}

std::string StringConverter::Char16ToUtf8(const std::u16string& utf16) {
    std::string result;
    result.reserve(utf16.size() * 3);

    for (size_t i = 0; i < utf16.size(); ++i) {
        char32_t cp = utf16[i];

        // Handle surrogate pairs
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < utf16.size()) {
            char16_t low = utf16[i + 1];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                ++i;
            }
        }

        // Encode as UTF-8
        if (cp <= 0x7F) {
            result.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FF) {
            result.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            result.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0x10FFFF) {
            result.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            result.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            result.push_back(static_cast<char>(0xEF)); // U+FFFD replacement
            result.push_back(static_cast<char>(0xBF));
            result.push_back(static_cast<char>(0xBD));
        }
    }

    return result;
}

std::string StringConverter::AnsiToUtf8(const std::string& ansi) {
    // macOS has no ANSI concept, assume input is already UTF-8
    return ansi;
}

std::string StringConverter::Utf8ToAnsi(const std::string& utf8) {
    // macOS has no ANSI concept, return UTF-8 directly
    return utf8;
}

bool StringConverter::IsValidUtf8(const std::string& str) {
    const char* ptr = str.c_str();
    size_t len = str.size();
    size_t i = 0;

    while (i < len) {
        unsigned char c = static_cast<unsigned char>(ptr[i]);

        if ((c & 0x80) == 0) {
            // 1-byte UTF-8
            i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            // 2-byte UTF-8
            if (i + 1 >= len || (ptr[i + 1] & 0xC0) != 0x80) {
                return false;
            }
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            // 3-byte UTF-8
            if (i + 2 >= len || (ptr[i + 1] & 0xC0) != 0x80 || (ptr[i + 2] & 0xC0) != 0x80) {
                return false;
            }
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
            // 4-byte UTF-8
            if (i + 3 >= len || (ptr[i + 1] & 0xC0) != 0x80 ||
                (ptr[i + 2] & 0xC0) != 0x80 || (ptr[i + 3] & 0xC0) != 0x80) {
                return false;
            }
            i += 4;
        } else {
            return false;
        }
    }

    return true;
}

// ============================================================================
// SystemUtils implementation (macOS-specific)
// ============================================================================

std::string SystemUtils::GetLastErrorString() {
    return strerror(errno);
}

uint64_t SystemUtils::GetCurrentProcessId() {
    return static_cast<uint64_t>(getpid());
}

uint64_t SystemUtils::GetCurrentThreadId() {
    uint64_t tid = 0;
    pthread_threadid_np(NULL, &tid);
    return tid;
}

// ============================================================================
// PlatformInitialize/Cleanup (macOS-specific)
// ============================================================================

bool PlatformInitialize() {
    // macOS doesn't need special initialization
    return true;
}

void PlatformCleanup() {
    // macOS doesn't need special cleanup
}

// ============================================================================
// macOS-specific helper functions
// ============================================================================

namespace PlatformMacOS {

// SMC connection handle
static void* g_smc_connection = nullptr;

bool OpenSMCConnection() {
    // Dynamically load IOKit framework
    void* iokit = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY);
    if (!iokit) {
        return false;
    }

    // Get function pointers (simplified implementation) 
    // This is just a placeholder, actual SMC access requires more code
    g_smc_connection = reinterpret_cast<void*>(1); // placeholder
    return true;
}

void CloseSMCConnection() {
    if (g_smc_connection) {
        g_smc_connection = nullptr;
    }
}

bool ReadSMCKey(const char* key, uint32_t* outSize, void* outValue) {
    if (!g_smc_connection || !key || !outSize || !outValue) {
        return false;
    }

    // Simplified implementation, actual IOKit function calls needed
    // Returns dummy data for testing
    if (strcmp(key, "TC0P") == 0) {
        // CPUTemperature
        float temp = 45.0f; // Sample temperature
        *outSize = sizeof(float);
        memcpy(outValue, &temp, sizeof(float));
        return true;
    } else if (strcmp(key, "TG0P") == 0) {
        // GPUTemperature
        float temp = 55.0f; // Sample temperature
        *outSize = sizeof(float);
        memcpy(outValue, &temp, sizeof(float));
        return true;
    }

    return false;
}

void* GetIOKitService(const char* serviceName) {
    // Simplified implementation
    return reinterpret_cast<void*>(1);
}

void ReleaseIOKitObject(void* object) {
    // Simplified implementation, actual IOKit object release needed
}

} // namespace PlatformMacOS

} // namespace Platform