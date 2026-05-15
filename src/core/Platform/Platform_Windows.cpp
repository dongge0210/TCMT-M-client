// Platform_Windows.cpp - Windows platform-specific implementation

// This file is only compiled when TCMT_WINDOWS is defined
// Included via conditional compilation in Platform.cpp

#ifndef TCMT_WINDOWS
#error "This file should only be compiled for Windows platform (TCMT_WINDOWS defined)"
#endif

#include "Platform.h"

namespace Platform {

// ============================================================================
// SystemTime implementation (Windows-specific)
// ============================================================================

SystemTime SystemTime::Now() {
    SYSTEMTIME st;
    GetSystemTime(&st);

    SystemTime result;
    result.year = st.wYear;
    result.month = st.wMonth;
    result.dayOfWeek = st.wDayOfWeek;
    result.day = st.wDay;
    result.hour = st.wHour;
    result.minute = st.wMinute;
    result.second = st.wSecond;
    result.milliseconds = st.wMilliseconds;

    return result;
}

// ============================================================================
// CriticalSection implementation (Windows-specific)
// ============================================================================

CriticalSection::CriticalSection() {
    InitializeCriticalSection(&cs_);
}

CriticalSection::~CriticalSection() {
    DeleteCriticalSection(&cs_);
}

void CriticalSection::Enter() {
    EnterCriticalSection(&cs_);
}

void CriticalSection::Leave() {
    LeaveCriticalSection(&cs_);
}

bool CriticalSection::TryEnter() {
    return TryEnterCriticalSection(&cs_) != 0;
}

// ============================================================================
// SharedMemory implementation (Windows-specific)
// ============================================================================

SharedMemory::SharedMemory()
    : address_(nullptr), size_(0), created_(false), hMapFile_(NULL) {
}

SharedMemory::~SharedMemory() {
    Unmap();
}

bool SharedMemory::Create(const std::string& name, size_t size) {
    // Cleanup existing resources
    Unmap();

    // Create security descriptor allowing full access
    SECURITY_ATTRIBUTES sa;
    SECURITY_DESCRIPTOR sd;

    if (!InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION)) {
        last_error_ = "Failed to initialize security descriptor";
        return false;
    }

    if (!SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE)) {
        last_error_ = "Failed to set security descriptor DACL";
        return false;
    }

    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = &sd;
    sa.bInheritHandle = FALSE;

    // Convert to wide string
    std::wstring wname = StringConverter::Utf8ToWide(name);

    // Try global namespace
    hMapFile_ = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        &sa,
        PAGE_READWRITE,
        0,
        static_cast<DWORD>(size),
        (L"Global\\" + wname).c_str()
    );

    if (!hMapFile_) {
        // Try local namespace
        hMapFile_ = CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            &sa,
            PAGE_READWRITE,
            0,
            static_cast<DWORD>(size),
            (L"Local\\" + wname).c_str()
        );
    }

    if (!hMapFile_) {
        // Try no namespace prefix
        hMapFile_ = CreateFileMappingW(
            INVALID_HANDLE_VALUE,
            &sa,
            PAGE_READWRITE,
            0,
            static_cast<DWORD>(size),
            wname.c_str()
        );
    }

    if (!hMapFile_) {
        last_error_ = "Failed to create file mapping";
        return false;
    }

    size_ = size;
    created_ = true;
    return Map();
}

bool SharedMemory::Open(const std::string& name, size_t size) {
    Unmap();

    std::wstring wname = StringConverter::Utf8ToWide(name);

    // Try different namespaces
    hMapFile_ = OpenFileMappingW(
        FILE_MAP_ALL_ACCESS,
        FALSE,
        (L"Global\\" + wname).c_str()
    );

    if (!hMapFile_) {
        hMapFile_ = OpenFileMappingW(
            FILE_MAP_ALL_ACCESS,
            FALSE,
            (L"Local\\" + wname).c_str()
        );
    }

    if (!hMapFile_) {
        hMapFile_ = OpenFileMappingW(
            FILE_MAP_ALL_ACCESS,
            FALSE,
            wname.c_str()
        );
    }

    if (!hMapFile_) {
        last_error_ = "Failed to open file mapping";
        return false;
    }

    size_ = size;
    created_ = false;
    return Map();
}

bool SharedMemory::Map() {
    if (!hMapFile_) {
        last_error_ = "No file mapping handle";
        return false;
    }

    address_ = MapViewOfFile(
        hMapFile_,
        FILE_MAP_ALL_ACCESS,
        0, 0, size_
    );

    if (!address_) {
        last_error_ = "Failed to map view of file";
        return false;
    }

    return true;
}

bool SharedMemory::Unmap() {
    if (address_) {
        UnmapViewOfFile(address_);
        address_ = nullptr;
    }

    if (hMapFile_) {
        CloseHandle(hMapFile_);
        hMapFile_ = NULL;
    }

    size_ = 0;
    created_ = false;
    return true;
}

// ============================================================================
// InterprocessMutex implementation (Windows-specific)
// ============================================================================

InterprocessMutex::InterprocessMutex() : mutex_(NULL) {
}

InterprocessMutex::~InterprocessMutex() {
    if (mutex_) {
        CloseHandle(mutex_);
    }
}

bool InterprocessMutex::Create(const std::string& name) {
    if (mutex_) {
        CloseHandle(mutex_);
    }

    std::wstring wname = StringConverter::Utf8ToWide(name);
    name_ = name;

    // Create security descriptor
    SECURITY_ATTRIBUTES sa;
    SECURITY_DESCRIPTOR sd;

    if (!InitializeSecurityDescriptor(&sd, SECURITY_DESCRIPTOR_REVISION)) {
        last_error_ = "Failed to initialize security descriptor";
        return false;
    }

    if (!SetSecurityDescriptorDacl(&sd, TRUE, NULL, FALSE)) {
        last_error_ = "Failed to set security descriptor DACL";
        return false;
    }

    sa.nLength = sizeof(SECURITY_ATTRIBUTES);
    sa.lpSecurityDescriptor = &sd;
    sa.bInheritHandle = FALSE;

    mutex_ = CreateMutexW(&sa, FALSE, (L"Global\\" + wname).c_str());
    if (!mutex_) {
        mutex_ = CreateMutexW(&sa, FALSE, (L"Local\\" + wname).c_str());
    }

    if (!mutex_) {
        mutex_ = CreateMutexW(&sa, FALSE, wname.c_str());
    }

    if (!mutex_) {
        last_error_ = "Failed to create mutex";
        return false;
    }

    return true;
}

bool InterprocessMutex::Open(const std::string& name) {
    if (mutex_) {
        CloseHandle(mutex_);
    }

    std::wstring wname = StringConverter::Utf8ToWide(name);
    name_ = name;

    mutex_ = OpenMutexW(MUTEX_ALL_ACCESS, FALSE, (L"Global\\" + wname).c_str());
    if (!mutex_) {
        mutex_ = OpenMutexW(MUTEX_ALL_ACCESS, FALSE, (L"Local\\" + wname).c_str());
    }

    if (!mutex_) {
        mutex_ = OpenMutexW(MUTEX_ALL_ACCESS, FALSE, wname.c_str());
    }

    if (!mutex_) {
        last_error_ = "Failed to open mutex";
        return false;
    }

    return true;
}

bool InterprocessMutex::Lock(uint32_t timeout_ms) {
    if (!mutex_) {
        last_error_ = "Mutex not created or opened";
        return false;
    }

    DWORD result = WaitForSingleObject(mutex_, timeout_ms);
    if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) {
        return true;
    }

    if (result == WAIT_TIMEOUT) {
        last_error_ = "Timeout waiting for mutex";
    } else {
        last_error_ = "Failed to wait for mutex";
    }

    return false;
}

bool InterprocessMutex::Unlock() {
    if (!mutex_) {
        last_error_ = "Mutex not created or opened";
        return false;
    }

    if (!ReleaseMutex(mutex_)) {
        last_error_ = "Failed to release mutex";
        return false;
    }

    return true;
}

// ============================================================================
// FileHandle implementation (Windows-specific)
// ============================================================================

void FileHandle::Close() {
    if (handle_ != InvalidHandle()) {
        CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = InvalidHandle();
    }
}

void* FileHandle::InvalidHandle() {
    return INVALID_HANDLE_VALUE;
}

// ============================================================================
// StringConverter implementation (Windows-specific)
// ============================================================================

std::wstring StringConverter::Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) {
        return L"";
    }

    int size_needed = MultiByteToWideChar(CP_UTF8, 0,
                                          utf8.c_str(), (int)utf8.size(),
                                          nullptr, 0);
    if (size_needed <= 0) {
        return L"";
    }

    std::wstring result(size_needed, 0);
    MultiByteToWideChar(CP_UTF8, 0,
                        utf8.c_str(), (int)utf8.size(),
                        &result[0], size_needed);

    return result;
}

std::string StringConverter::WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) {
        return "";
    }

    int size_needed = WideCharToMultiByte(CP_UTF8, 0,
                                          wide.c_str(), (int)wide.size(),
                                          nullptr, 0, nullptr, nullptr);
    if (size_needed <= 0) {
        return "";
    }

    std::string result(size_needed, 0);
    WideCharToMultiByte(CP_UTF8, 0,
                        wide.c_str(), (int)wide.size(),
                        &result[0], size_needed, nullptr, nullptr);

    return result;
}

std::string StringConverter::AnsiToUtf8(const std::string& ansi) {
    if (ansi.empty()) {
        return "";
    }

    // First convert to wide string (using current ANSI codepage)
    int wide_size = MultiByteToWideChar(CP_ACP, 0,
                                        ansi.c_str(), (int)ansi.size(),
                                        nullptr, 0);
    if (wide_size <= 0) {
        return "";
    }

    std::wstring wide(wide_size, 0);
    MultiByteToWideChar(CP_ACP, 0,
                        ansi.c_str(), (int)ansi.size(),
                        &wide[0], wide_size);

    // Then convert from wide string to UTF-8
    return WideToUtf8(wide);
}

std::string StringConverter::Utf8ToAnsi(const std::string& utf8) {
    if (utf8.empty()) {
        return "";
    }

    // First convert to wide string (UTF-8)
    std::wstring wide = Utf8ToWide(utf8);

    // Then convert from wide string to ANSI
    int ansi_size = WideCharToMultiByte(CP_ACP, 0,
                                        wide.c_str(), (int)wide.size(),
                                        nullptr, 0, nullptr, nullptr);
    if (ansi_size <= 0) {
        return "";
    }

    std::string result(ansi_size, 0);
    WideCharToMultiByte(CP_ACP, 0,
                        wide.c_str(), (int)wide.size(),
                        &result[0], ansi_size, nullptr, nullptr);

    return result;
}

bool StringConverter::IsValidUtf8(const std::string& str) {
    // Windows implementation: try conversion, if success then valid UTF-8
    int size_needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                          str.c_str(), (int)str.size(),
                                          nullptr, 0);
    return size_needed > 0;
}

// ============================================================================
// SystemUtils implementation (Windows-specific)
// ============================================================================

std::string SystemUtils::GetLastErrorString() {
    DWORD error = GetLastError();
    if (error == 0) {
        return "";
    }

    LPSTR buffer = nullptr;
    size_t size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, error, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPSTR)&buffer, 0, NULL);

    std::string result;
    if (buffer) {
        result = std::string(buffer, size);
        LocalFree(buffer);
    } else {
        result = "Unknown error";
    }

    return result;
}

uint64_t SystemUtils::GetCurrentProcessId() {
    return static_cast<uint64_t>(::GetCurrentProcessId());
}

uint64_t SystemUtils::GetCurrentThreadId() {
    return static_cast<uint64_t>(::GetCurrentThreadId());
}

// ============================================================================
// Platform Initialize/Cleanup (Windows-specific)
// ============================================================================

bool PlatformInitialize() {
    // Initialize COM (if needed)
    // HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    // return SUCCEEDED(hr);
    return true;
}

void PlatformCleanup() {
    // Cleanup COM
    // CoUninitialize();
}

// ============================================================================
// Windows-specific helper functions
// ============================================================================

namespace PlatformWindows {

Platform::SystemTime ConvertSystemTime(const SYSTEMTIME& st) {
    Platform::SystemTime result;
    result.year = st.wYear;
    result.month = st.wMonth;
    result.dayOfWeek = st.wDayOfWeek;
    result.day = st.wDay;
    result.hour = st.wHour;
    result.minute = st.wMinute;
    result.second = st.wSecond;
    result.milliseconds = st.wMilliseconds;
    return result;
}

SYSTEMTIME ConvertToSystemTime(const Platform::SystemTime& pt) {
    SYSTEMTIME st;
    st.wYear = pt.year;
    st.wMonth = pt.month;
    st.wDayOfWeek = pt.dayOfWeek;
    st.wDay = pt.day;
    st.wHour = pt.hour;
    st.wMinute = pt.minute;
    st.wSecond = pt.second;
    st.wMilliseconds = pt.milliseconds;
    return st;
}

bool EnablePrivilege(const wchar_t* privilegeName) {
    HANDLE hToken;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken)) {
        return false;
    }

    TOKEN_PRIVILEGES tp;
    LUID luid;

    if (!LookupPrivilegeValueW(NULL, privilegeName, &luid)) {
        CloseHandle(hToken);
        return false;
    }

    tp.PrivilegeCount = 1;
    tp.Privileges[0].Luid = luid;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

    BOOL result = AdjustTokenPrivileges(
        hToken, FALSE, &tp, sizeof(TOKEN_PRIVILEGES),
        (PTOKEN_PRIVILEGES)NULL, (PDWORD)NULL);

    CloseHandle(hToken);
    return result && GetLastError() != ERROR_NOT_ALL_ASSIGNED;
}

std::string FormatWindowsErrorMessage(uint32_t errorCode) {
    LPSTR buffer = nullptr;
    size_t size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, errorCode, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPSTR)&buffer, 0, NULL);

    std::string result;
    if (buffer) {
        result = std::string(buffer, size);
        LocalFree(buffer);
    } else {
        result = "Unknown Windows error: " + std::to_string(errorCode);
    }

    return result;
}

} // namespace PlatformWindows

} // namespace Platform