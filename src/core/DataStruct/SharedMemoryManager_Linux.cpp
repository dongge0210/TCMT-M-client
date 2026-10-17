#ifndef TCMT_LINUX
#error "This file should only be compiled for Linux platform (TCMT_LINUX defined)"
#endif

#include "SharedMemoryManager.h"
#include "../Platform/Platform.h"
#include "../Utils/Logger.h"
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <cstring>
#include <algorithm>

void* SharedMemoryManager::shmPtr = nullptr;
tcmt::ipc::IPCDataBlock* SharedMemoryManager::pBuffer = nullptr;
std::string SharedMemoryManager::lastError = "";
void* SharedMemoryManager::interprocessMutex = nullptr;

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
    lastError.clear();

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

    if (!shmPtr) {
        auto* shm = new Platform::SharedMemory();
        if (!shm->Create("SystemMonitorSharedMemory", sizeof(tcmt::ipc::IPCDataBlock))) {
            lastError = "Failed to create shared memory: " + shm->GetLastError();
            Logger::Error(lastError);
            delete shm;
            return false;
        }

        if (!shm->Map()) {
            lastError = "Failed to map shared memory: " + shm->GetLastError();
            Logger::Error(lastError);
            delete shm;
            return false;
        }

        shmPtr = shm;
        pBuffer = static_cast<tcmt::ipc::IPCDataBlock*>(shm->GetAddress());

        if (shm->IsCreated()) {
            memset(static_cast<void*>(pBuffer), 0, sizeof(tcmt::ipc::IPCDataBlock));
            Logger::Info("Created new shared memory mapping.");
        } else {
            Logger::Info("Opened existing shared memory mapping.");
        }

        Logger::Info("Shared memory successfully initialized.");
        return true;
    }

    auto* shm = static_cast<Platform::SharedMemory*>(shmPtr);
    if (!shm->GetAddress()) {
        lastError = "Shared memory initialized but mapping is invalid";
        Logger::Error(lastError);
        return false;
    }

    return true;
}

void SharedMemoryManager::CleanupSharedMemory() {
    if (shmPtr) {
        auto* shm = static_cast<Platform::SharedMemory*>(shmPtr);
        shm->Unmap();
        delete shm;
        shmPtr = nullptr;
        pBuffer = nullptr;
    }

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
    // On Linux, IPCServer manages its own IPCDataBlock directly via IPCServer::GetShmPtr().
    // SharedMemoryManager is kept for API compatibility but is not used at runtime.
    Logger::Trace("WriteToSharedMemory: Linux uses IPCServer path, skipping legacy SHM write");
}

// Note: Unused WCHAR helper functions removed — Linux uses I