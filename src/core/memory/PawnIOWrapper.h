#pragma once

#ifdef TCMT_WINDOWS
#include <windows.h>
#include <winioctl.h>
#include <string>
#include <vector>
#include <cstdint>

// PawnIO device name
#define PAWNIO_DEVICE_PATH L"\\\\.\\GLOBALROOT\\Device\\PawnIO"

// IOCTL codes (from PawnIO source)
#define PAWNIO_DEVICE_TYPE 41394
#define IOCTL_PIO_LOAD_BINARY  CTL_CODE(PAWNIO_DEVICE_TYPE, 0x821, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_PIO_EXECUTE_FN   CTL_CODE(PAWNIO_DEVICE_TYPE, 0x841, METHOD_BUFFERED, FILE_ANY_ACCESS)

class PawnIOWrapper {
public:
    PawnIOWrapper() : _hDevice(INVALID_HANDLE_VALUE) {}
    ~PawnIOWrapper() { Close(); }

    bool Open();
    void Close();
    bool IsOpen() const { return _hDevice != INVALID_HANDLE_VALUE; }

    // Load a .bin module from file or memory
    bool LoadModule(const wchar_t* modulePath);
    bool LoadModuleFromMemory(const uint8_t* data, size_t size, const char* moduleName);

    // Execute a named function in a loaded module
    // inBuf/inSize: input buffer (e.g. SMBus address, register)
    // outBuf/outSize: output buffer (e.g. temperature value)
    bool Execute(const char* funcName,
                 const uint64_t* inBuf, uint32_t inCount,
                 uint64_t* outBuf, uint32_t outCount,
                 uint32_t* returnSize = nullptr);

    // Check if PawnIO is installed
    static bool IsInstalled();

private:
    HANDLE _hDevice;
    std::vector<std::string> _loadedModules;
};
#endif // TCMT_WINDOWS
