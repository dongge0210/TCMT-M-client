#ifdef TCMT_WINDOWS
#include "PawnIOWrapper.h"
#include "../Utils/Logger.h"

bool PawnIOWrapper::Open() {
    if (_hDevice != INVALID_HANDLE_VALUE) return true;

    _hDevice = CreateFileW(PAWNIO_DEVICE_PATH,
                           GENERIC_READ | GENERIC_WRITE,
                           0, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);

    if (_hDevice == INVALID_HANDLE_VALUE) {
        static bool loggedOnce = false;
        if (!loggedOnce) {
            Logger::Debug("PawnIO: device not available");
            loggedOnce = true;
        }
        return false;
    }
    static bool openedLogged = false;
    if (!openedLogged) {
        Logger::Info("PawnIO: device opened, DIMM probing started");
        openedLogged = true;
    }
    return true;
}

void PawnIOWrapper::Close() {
    if (_hDevice != INVALID_HANDLE_VALUE) {
        CloseHandle(_hDevice);
        _hDevice = INVALID_HANDLE_VALUE;
    }
}

bool PawnIOWrapper::LoadModuleFromMemory(const uint8_t* data, size_t size, const char* moduleName) {
    if (_hDevice == INVALID_HANDLE_VALUE || !data || size == 0) return false;

    DWORD bytesReturned = 0;
    BOOL ok = DeviceIoControl(_hDevice, IOCTL_PIO_LOAD_BINARY,
                              (LPVOID)data, (DWORD)size,
                              nullptr, 0, &bytesReturned, nullptr);
    if (!ok) {
        Logger::Debug(std::string("PawnIO: LoadModule failed for ") + moduleName);
        return false;
    }
    _loadedModules.push_back(moduleName);
    Logger::Debug(std::string("PawnIO: loaded module ") + moduleName);
    return true;
}

bool PawnIOWrapper::LoadModule(const wchar_t* modulePath) {
    HANDLE hFile = CreateFileW(modulePath, GENERIC_READ, FILE_SHARE_READ,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) return false;

    DWORD size = GetFileSize(hFile, nullptr);
    if (size == INVALID_FILE_SIZE || size == 0) { CloseHandle(hFile); return false; }

    std::vector<uint8_t> buf(size);
    DWORD read = 0;
    BOOL ok = ReadFile(hFile, buf.data(), size, &read, nullptr);
    CloseHandle(hFile);

    if (!ok || read != size) return false;

    // Use filename as module name
    const wchar_t* name = wcsrchr(modulePath, L'\\');
    name = name ? name + 1 : modulePath;
    char mbName[128] = {};
    WideCharToMultiByte(CP_UTF8, 0, name, -1, mbName, sizeof(mbName) - 1, nullptr, nullptr);

    return LoadModuleFromMemory(buf.data(), size, mbName);
}

bool PawnIOWrapper::Execute(const char* funcName,
                            const uint64_t* inBuf, uint32_t inCount,
                            uint64_t* outBuf, uint32_t outCount,
                            uint32_t* returnSize) {
    if (_hDevice == INVALID_HANDLE_VALUE || !funcName) return false;

    // PawnIO requires fixed 32-byte function name field
    constexpr size_t FN_LEN = 32;
    size_t totalInSize = FN_LEN + inCount * sizeof(uint64_t);
    if (totalInSize < FN_LEN) totalInSize = FN_LEN;
    std::vector<uint8_t> inData(totalInSize, 0);
    for (size_t i = 0; i < FN_LEN - 1 && funcName[i]; i++)
        inData[i] = static_cast<uint8_t>(funcName[i]);
    if (inBuf && inCount > 0)
        memcpy(inData.data() + FN_LEN, inBuf, inCount * sizeof(uint64_t));

    size_t totalOutSize = outCount * sizeof(uint64_t);
    std::vector<uint8_t> outData(totalOutSize);

    DWORD bytesReturned = 0;
    BOOL ok = DeviceIoControl(_hDevice, IOCTL_PIO_EXECUTE_FN,
                              inData.data(), (DWORD)totalInSize,
                              outData.data(), (DWORD)totalOutSize,
                              &bytesReturned, nullptr);
    if (!ok) return false;

    if (outBuf && outCount > 0) {
        memcpy(outBuf, outData.data(), (std::min)((size_t)bytesReturned, totalOutSize));
        // NTSTATUS error: upper bit of lower 32 bits set
        uint32_t ntStatus = (uint32_t)(outBuf[0] & 0xFFFFFFFFULL);
        if (ntStatus & 0x80000000) return false;
    }
    if (returnSize)
        *returnSize = bytesReturned;

    return true;
}

bool PawnIOWrapper::IsInstalled() {
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
        L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PawnIO",
        0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return true;
    }
    // Also check if device exists
    HANDLE h = CreateFileW(PAWNIO_DEVICE_PATH, 0, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return true; }
    return false;
}
#endif // TCMT_WINDOWS
