#pragma once
#include "DataStruct.h"
#include <string>

// Shared memory management class to avoid multiple definitions
class SharedMemoryManager {
private:
    // Platform-specific implementation details
    #ifdef TCMT_WINDOWS
    static void* hMapFile;  // Windows: HANDLE (void* for abstraction)
    #elif defined(TCMT_MACOS) || defined(TCMT_LINUX)
    static void* shmPtr;    // POSIX: pointer to shared memory object info
    #endif

    static SharedMemoryBlock* pBuffer;
    static std::string lastError; // Store last error message
    static void* interprocessMutex; // Platform-specific interprocess mutex

public:
    // Initialize shared memory
    static bool InitSharedMemory();

    // Write system info to shared memory
    static void WriteToSharedMemory(const SystemInfo& sysInfo);

    // Clean up shared memory resources
    static void CleanupSharedMemory();

    // Get buffer pointer (if needed)
    static SharedMemoryBlock* GetBuffer() { return pBuffer; }

    // Get last error message
    static std::string GetLastError();
};
