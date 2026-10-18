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

        // Create() already mapped the segment; mapping again would replace
        // address_ without unmapping the first view.
        if (!shm->GetAddress()) {
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
