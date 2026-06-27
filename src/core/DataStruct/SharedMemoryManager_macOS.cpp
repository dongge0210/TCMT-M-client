// SharedMemoryManager_macOS.cpp - macOS platform shared memory manager implementation
// Uses POSIX shared memory and Platform abstraction layer

#ifndef TCMT_MACOS
#error "This file should only be compiled for macOS platform (TCMT_MACOS defined)"
#endif

#include "SharedMemoryManager.h"
#include "../Platform/Platform.h"
#include "../Utils/Logger.h"
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <cstring>
#include <algorithm>

// Define static members (macOS platform)
void* SharedMemoryManager::shmPtr = nullptr;
tcmt::ipc::IPCDataBlock* SharedMemoryManager::pBuffer = nullptr;
std::string SharedMemoryManager::lastError = "";
void* SharedMemoryManager::interprocessMutex = nullptr;

// Helper function: safely copy wide string (WCHAR = char16_t on macOS)
static void SafeCopyWideString(WCHAR* dest, size_t destSize, const std::u16string& src) {
    try {
        if (dest == nullptr || destSize == 0) return;
        memset(dest, 0, destSize * sizeof(WCHAR));
        if (src.empty()) { dest[0] = u'\0'; return; }
        size_t copyLen = std::min(src.length(), destSize - 1);
        for (size_t i = 0; i < copyLen; ++i) dest[i] = src[i];
        dest[copyLen] = u'\0';
    } catch (...) { if (dest && destSize > 0) dest[0] = u'\0'; }
}

// Helper function: safely copy from wide character array
static void SafeCopyFromWideArray(WCHAR* dest, size_t destSize, const WCHAR* src, size_t srcCapacity) {
    if (!dest || destSize == 0) return;
    memset(dest, 0, destSize * sizeof(WCHAR));
    if (!src) return;
    size_t len = 0;
    while (len < srcCapacity && src[len] != u'\0') ++len;
    if (len >= destSize) len = destSize - 1;
    for (size_t i = 0; i < len; ++i) dest[i] = src[i];
    dest[len] = u'\0';
}

bool SharedMemoryManager::InitSharedMemory() {
    // Clear previous errors
    lastError.clear();

    // Create or open inter-process mutex
    if (!interprocessMutex) {
        auto* mutex = new Platform::InterprocessMutex();
        if (!mutex->Create("SystemMonitorSharedMemoryMutex")) {
            lastError = "Failed to create inter-process mutex: " + mutex->GetLastError();
            Logger::Error(lastError);
            delete mutex;
            return false;
        }
        interprocessMutex = mutex;
    }

    // Create shared memory object
    if (!shmPtr) {
        auto* shm = new Platform::SharedMemory();
        if (!shm->Create("SystemMonitorSharedMemory", sizeof(tcmt::ipc::IPCDataBlock))) {
            lastError = "Failed to create shared memory: " + shm->GetLastError();
            Logger::Error(lastError);
            delete shm;
            return false;
        }

        // Map to process address space
        if (!shm->Map()) {
            lastError = "Failed to map shared memory: " + shm->GetLastError();
            Logger::Error(lastError);
            delete shm;
            return false;
        }

        shmPtr = shm;
        pBuffer = static_cast<tcmt::ipc::IPCDataBlock*>(shm->GetAddress());

        // If newly created shared memory, zero it
        if (shm->IsCreated()) {
            memset(static_cast<void*>(pBuffer), 0, sizeof(tcmt::ipc::IPCDataBlock));
            Logger::Info("Created new shared memory mapping.");
        } else {
            Logger::Info("Opened existing shared memory mapping.");
        }

        Logger::Info("Shared memory successfully initialized.");
        return true;
    }

    // If already initialized, check if still valid
    auto* shm = static_cast<Platform::SharedMemory*>(shmPtr);
    if (!shm->GetAddress()) {
        lastError = "Shared memory initialized but mapping is invalid";
        Logger::Error(lastError);
        return false;
    }

    return true;
}

void SharedMemoryManager::CleanupSharedMemory() {
    // Cleanup shared memory
    if (shmPtr) {
        auto* shm = static_cast<Platform::SharedMemory*>(shmPtr);
        shm->Unmap();
        delete shm;
        shmPtr = nullptr;
        pBuffer = nullptr;
    }

    // Cleanup mutex
    if (interprocessMutex) {
        auto* mutex = static_cast<Platform::InterprocessMutex*>(interprocessMutex);
        delete mutex;
        interprocessMutex = nullptr;
    }
}

std::string SharedMemoryManager::GetLastError() {
    return lastError;
}

void SharedMemoryManager::WriteToSharedMemory(const SystemInfo& systemInfo) {
    (void)systemInfo;
    // On macOS, IPCServer manages its own IPCDataBlock directly via IPCServer::GetShmPtr().
    // SharedMemoryManager is kept for API compatibility but is not used at runtime.
    Logger::Trace("WriteToSharedMemory: macOS uses IPCServer path, skipping legacy SHM write");
}