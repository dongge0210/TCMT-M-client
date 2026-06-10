// Platform_Linux.cpp - Linux platform-specific implementation

// This file is only compiled when TCMT_LINUX is defined
// Included via conditional compilation in Platform.cpp

#ifndef TCMT_LINUX
#error "This file should only be compiled for Linux platform (TCMT_LINUX defined)"
#endif

#include "Platform.h"
#include <sys/time.h>
#include <pthread.h>
#include <unistd.h>
#include <cstring>
#include <sys/syscall.h>
#include <sys/types.h>

namespace Platform {

// ============================================================================
// SystemTime implementation (Linux-specific)
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
// CriticalSection implementation (Linux-specific)
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
// SharedMemory implementation (Linux-specific)
// ============================================================================

SharedMemory::SharedMemory()
    : address_(nullptr), size_(0), created_(false), shm_fd_(-1) {
}

SharedMemory::~SharedMemory() {
    Unmap();
}

bool SharedMemory::Create(const std::string& name, size_t size) {
    Unmap();

    std::string safe_name = name;
    size_t pos = safe_name.find('/');
    while (pos != std::string::npos) { safe_name.erase(pos, 1); pos = safe_name.find('/'); }
    if (safe_name.size() > 20) safe_name = safe_name.substr(0, 20);
    shm_name_ = "/" + safe_name;

    shm_unlink(shm_name_.c_str());

    shm_fd_ = shm_open(shm_name_.c_str(), O_CREAT | O_RDWR, 0666);
    if (shm_fd_ == -1) {
        last_error_ = "shm_open(" + shm_name_ + ") failed: " + strerror(errno);
        return false;
    }

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
// InterprocessMutex implementation (Linux-specific)
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

    if (!shm_.Create(name + "_mutex", sizeof(pthread_mutex_t))) {
        last_error_ = "Failed to create shared memory for mutex: " + shm_.GetLastError();
        return false;
    }

    is_creator_ = true;
    mutex_ptr_ = static_cast<pthread_mutex_t*>(shm_.GetAddress());

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

    if (!shm_.Open(name + "_mutex", sizeof(pthread_mutex_t))) {
        last_error_ = "Failed to open shared memory for mutex: " + shm_.GetLastError();
        return false;
    }

    is_creator_ = false;
    mutex_ptr_ = static_cast<pthread_mutex_t*>(shm_.GetAddress());
    return true;
}

bool InterprocessMutex::Lock(uint32_t timeout_ms) {
    (void)timeout_ms;
    if (!mutex_ptr_) {
        last_error_ = "Mutex not created or opened";
        return false;
    }

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
// FileHandle implementation (Linux-specific)
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
// StringConverter implementation (Linux-specific)
// ============================================================================

std::wstring StringConverter::Utf8ToWide(const std::string& utf8) {
    std::wstring result;
    result.reserve(utf8.size());

    const char* ptr = utf8.c_str();
    size_t len = utf8.size();
    size_t i = 0;

    while (i < len) {
        wchar_t wc = 0;
        unsigned char c = static_cast<unsigned char>(ptr[i]);

        if ((c & 0x80) == 0) {
            wc = c;
            i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            if (i + 1 < len) {
                wc = ((c & 0x1F) << 6) | (ptr[i + 1] & 0x3F);
                i += 2;
            } else {
                wc = '?';
                i += 1;
            }
        } else if ((c & 0xF0) == 0xE0) {
            if (i + 2 < len) {
                wc = ((c & 0x0F) << 12) | ((ptr[i + 1] & 0x3F) << 6) | (ptr[i + 2] & 0x3F);
                i += 3;
            } else {
                wc = '?';
                i += 1;
            }
        } else if ((c & 0xF8) == 0xF0) {
            wc = 0xFFFD;
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
    result.reserve(wide.size() * 4);

    for (wchar_t wc : wide) {
        if (wc <= 0x7F) {
            result.push_back(static_cast<char>(wc));
        } else if (wc <= 0x7FF) {
            result.push_back(static_cast<char>(0xC0 | (wc >> 6)));
            result.push_back(static_cast<char>(0x80 | (wc & 0x3F)));
        } else if (wc <= 0xFFFF) {
            result.push_back(static_cast<char>(0xE0 | (wc >> 12)));
            result.push_back(static_cast<char>(0x80 | ((wc >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (wc & 0x3F)));
        } else {
            result.push_back(static_cast<char>(0xF0 | (wc >> 18)));
            result.push_back(static_cast<char>(0x80 | ((wc >> 12) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | ((wc >> 6) & 0x3F)));
            result.push_back(static_cast<char>(0x80 | (wc & 0x3F)));
        }
    }

    return result;
}

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

        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < utf16.size()) {
            char16_t low = utf16[i + 1];
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                ++i;
            }
        }

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
            result.push_back(static_cast<char>(0xEF));
            result.push_back(static_cast<char>(0xBF));
            result.push_back(static_cast<char>(0xBD));
        }
    }

    return result;
}

std::string StringConverter::AnsiToUtf8(const std::string& ansi) {
    return ansi;
}

std::string StringConverter::Utf8ToAnsi(const std::string& utf8) {
    return utf8;
}

bool StringConverter::IsValidUtf8(const std::string& str) {
    const char* ptr = str.c_str();
    size_t len = str.size();
    size_t i = 0;

    while (i < len) {
        unsigned char c = static_cast<unsigned char>(ptr[i]);

        if ((c & 0x80) == 0) {
            i += 1;
        } else if ((c & 0xE0) == 0xC0) {
            if (i + 1 >= len || (ptr[i + 1] & 0xC0) != 0x80) {
                return false;
            }
            i += 2;
        } else if ((c & 0xF0) == 0xE0) {
            if (i + 2 >= len || (ptr[i + 1] & 0xC0) != 0x80 || (ptr[i + 2] & 0xC0) != 0x80) {
                return false;
            }
            i += 3;
        } else if ((c & 0xF8) == 0xF0) {
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
// SystemUtils implementation (Linux-specific)
// ============================================================================

std::string SystemUtils::GetLastErrorString() {
    return strerror(errno);
}

uint64_t SystemUtils::GetCurrentProcessId() {
    return static_cast<uint64_t>(getpid());
}

uint64_t SystemUtils::GetCurrentThreadId() {
    return static_cast<uint64_t>(syscall(SYS_gettid));
}

// ============================================================================
// PlatformInitialize/Cleanup (Linux-specific)
// ============================================================================

bool PlatformInitialize() {
    return true;
}

void PlatformCleanup() {
}

} // namespace Platform
