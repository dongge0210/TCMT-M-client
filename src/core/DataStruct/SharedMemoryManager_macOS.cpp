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
SharedMemoryBlock* SharedMemoryManager::pBuffer = nullptr;
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
        if (!shm->Create("SystemMonitorSharedMemory", sizeof(SharedMemoryBlock))) {
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
        pBuffer = static_cast<SharedMemoryBlock*>(shm->GetAddress());

        // If newly created shared memory, zero it
        if (shm->IsCreated()) {
            memset(static_cast<void*>(pBuffer), 0, sizeof(SharedMemoryBlock));
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
    if (!pBuffer) {
        lastError = "Shared memory not initialized";
        Logger::Critical(lastError);
        return;
    }

    // Acquire mutex
    auto* mutex = static_cast<Platform::InterprocessMutex*>(interprocessMutex);
    if (!mutex) {
        lastError = "Inter-process mutex not initialized";
        Logger::Critical(lastError);
        return;
    }

    if (!mutex->Lock(5000)) { // Wait up to 5 seconds
        Logger::Critical("Failed to acquire shared memory mutex");
        return;
    }

    try {
        // Clear main string/array areas
        memset(pBuffer->cpuName, 0, sizeof(pBuffer->cpuName));
        for (int i = 0; i < 2; ++i) {
            memset(pBuffer->gpus[i].name, 0, sizeof(pBuffer->gpus[i].name));
            memset(pBuffer->gpus[i].brand, 0, sizeof(pBuffer->gpus[i].brand));
        }
        for (int i = 0; i < 8; ++i) {
            memset(&pBuffer->disks[i], 0, sizeof(pBuffer->disks[i]));
            memset(&pBuffer->physicalDisks[i], 0, sizeof(pBuffer->physicalDisks[i]));
        }
        for (int i = 0; i < 10; ++i) {
            memset(pBuffer->temperatures[i].sensorName, 0, sizeof(pBuffer->temperatures[i].sensorName));
        }

        // Battery / power
        pBuffer->batteryPercent = systemInfo.batteryPercent;
        pBuffer->acOnline = systemInfo.acOnline;

        // OS version
        SafeCopyWideString(pBuffer->osVersion, 128,
                          Platform::StringConverter::Utf8ToChar16(systemInfo.osVersion));

        // CPU information
        SafeCopyWideString(pBuffer->cpuName, 128,
                          Platform::StringConverter::Utf8ToChar16(systemInfo.cpuName));
        pBuffer->physicalCores = systemInfo.physicalCores;
        pBuffer->logicalCores = systemInfo.logicalCores;
        pBuffer->cpuUsage = systemInfo.cpuUsage;
        pBuffer->performanceCores = systemInfo.performanceCores;
        pBuffer->efficiencyCores = systemInfo.efficiencyCores;
        pBuffer->pCoreFreq = systemInfo.performanceCoreFreq;
        pBuffer->eCoreFreq = systemInfo.efficiencyCoreFreq;
        pBuffer->hyperThreading = systemInfo.hyperThreading;
        pBuffer->virtualization = systemInfo.virtualization;

        // Memory information
        pBuffer->totalMemory = systemInfo.totalMemory;
        pBuffer->usedMemory = systemInfo.usedMemory;
        pBuffer->availableMemory = systemInfo.availableMemory;
        pBuffer->compressedMemory = systemInfo.compressedMemory;

        // GPU information (compatible with old fields)
        pBuffer->gpuCount = 0;
        if (!systemInfo.gpuName.empty()) {
            SafeCopyWideString(pBuffer->gpus[0].name, 128,
                              Platform::StringConverter::Utf8ToChar16(systemInfo.gpuName));
            SafeCopyWideString(pBuffer->gpus[0].brand, 64,
                              Platform::StringConverter::Utf8ToChar16(systemInfo.gpuBrand));
            pBuffer->gpus[0].memory = systemInfo.gpuMemory;
            pBuffer->gpus[0].coreClock = systemInfo.gpuCoreFreq;
            pBuffer->gpus[0].isVirtual = systemInfo.gpuIsVirtual;
            pBuffer->gpus[0].usage = systemInfo.gpuUsage;
            pBuffer->gpuCount = 1;
        }
        // If later want to support vector<GPUData>, can extend here

        // Network adapter information
        pBuffer->adapterCount = 0;
        int adapterWriteCount = static_cast<int>(std::min(systemInfo.adapters.size(), size_t(4)));
        for (int i = 0; i < adapterWriteCount; ++i) {
            const auto& src = systemInfo.adapters[i];
            SafeCopyFromWideArray(pBuffer->adapters[i].name, 128, src.name, 128);
            SafeCopyFromWideArray(pBuffer->adapters[i].mac, 32, src.mac, 32);
            SafeCopyFromWideArray(pBuffer->adapters[i].ipAddress, 64, src.ipAddress, 64);
            SafeCopyFromWideArray(pBuffer->adapters[i].adapterType, 32, src.adapterType, 32);
            pBuffer->adapters[i].speed = src.speed;
            pBuffer->adapters[i].downloadSpeed = src.downloadSpeed;
            pBuffer->adapters[i].uploadSpeed = src.uploadSpeed;
        }
        pBuffer->adapterCount = adapterWriteCount;

        // Compatible with old network adapter fields
        if (adapterWriteCount == 0 && !systemInfo.networkAdapterName.empty()) {
            SafeCopyWideString(pBuffer->adapters[0].name, 128,
                              Platform::StringConverter::Utf8ToChar16(systemInfo.networkAdapterName));
            SafeCopyWideString(pBuffer->adapters[0].mac, 32,
                              Platform::StringConverter::Utf8ToChar16(systemInfo.networkAdapterMac));
            SafeCopyWideString(pBuffer->adapters[0].ipAddress, 64,
                              Platform::StringConverter::Utf8ToChar16(systemInfo.networkAdapterIp));
            SafeCopyWideString(pBuffer->adapters[0].adapterType, 32,
                              Platform::StringConverter::Utf8ToChar16(systemInfo.networkAdapterType));
            pBuffer->adapters[0].speed = systemInfo.networkAdapterSpeed;
            pBuffer->adapterCount = 1;
        }

        // Logical disk information
        pBuffer->diskCount = static_cast<int>(std::min(systemInfo.disks.size(), static_cast<size_t>(8)));
        for (int i = 0; i < pBuffer->diskCount; ++i) {
            const auto& disk = systemInfo.disks[i];
            pBuffer->disks[i].letter = disk.letter;

            // Handle volume label encoding
            std::string safeLabel = disk.label;
            if (safeLabel.empty()) {
                safeLabel = "";
            } else if (!Platform::StringConverter::IsValidUtf8(safeLabel)) {
                // If not valid UTF-8, try using current locale conversion
                // Simplified handling: use raw string directly
            }

            SafeCopyWideString(pBuffer->disks[i].label, 128,
                              Platform::StringConverter::Utf8ToChar16(safeLabel));
            SafeCopyWideString(pBuffer->disks[i].fileSystem, 32,
                              Platform::StringConverter::Utf8ToChar16(disk.fileSystem));
            pBuffer->disks[i].totalSize = disk.totalSize;
            pBuffer->disks[i].usedSpace = disk.usedSpace;
            pBuffer->disks[i].freeSpace = disk.freeSpace;
        }

        // Physical disk SMART information
        pBuffer->physicalDiskCount = static_cast<int>(std::min(systemInfo.physicalDisks.size(), static_cast<size_t>(8)));
        for (int i = 0; i < pBuffer->physicalDiskCount; ++i) {
            const auto& src = systemInfo.physicalDisks[i];
            SafeCopyFromWideArray(pBuffer->physicalDisks[i].model, 128, src.model, 128);
            SafeCopyFromWideArray(pBuffer->physicalDisks[i].serialNumber, 64, src.serialNumber, 64);
            SafeCopyFromWideArray(pBuffer->physicalDisks[i].firmwareVersion, 32, src.firmwareVersion, 32);
            SafeCopyFromWideArray(pBuffer->physicalDisks[i].interfaceType, 32, src.interfaceType, 32);
            SafeCopyFromWideArray(pBuffer->physicalDisks[i].diskType, 16, src.diskType, 16);
            pBuffer->physicalDisks[i].capacity = src.capacity;
            pBuffer->physicalDisks[i].temperature = src.temperature;
            pBuffer->physicalDisks[i].healthPercentage = src.healthPercentage;
            pBuffer->physicalDisks[i].isSystemDisk = src.isSystemDisk;
            pBuffer->physicalDisks[i].smartEnabled = src.smartEnabled;
            pBuffer->physicalDisks[i].smartSupported = src.smartSupported;
            pBuffer->physicalDisks[i].powerOnHours = src.powerOnHours;
            pBuffer->physicalDisks[i].powerCycleCount = src.powerCycleCount;
            pBuffer->physicalDisks[i].reallocatedSectorCount = src.reallocatedSectorCount;
            pBuffer->physicalDisks[i].currentPendingSector = src.currentPendingSector;
            pBuffer->physicalDisks[i].uncorrectableErrors = src.uncorrectableErrors;
            pBuffer->physicalDisks[i].wearLeveling = src.wearLeveling;
            pBuffer->physicalDisks[i].totalBytesWritten = src.totalBytesWritten;
            pBuffer->physicalDisks[i].totalBytesRead = src.totalBytesRead;

            // Logical drive letters
            int ldCount = 0;
            for (char l : src.logicalDriveLetters) {
                if (ldCount >= 8 || l == 0) break;
                if (std::isalpha(static_cast<unsigned char>(l))) {
                    pBuffer->physicalDisks[i].logicalDriveLetters[ldCount++] = l;
                }
            }
            pBuffer->physicalDisks[i].logicalDriveCount = ldCount;

            // SMART attributes
            int attrCount = src.attributeCount;
            if (attrCount < 0) attrCount = 0;
            if (attrCount > 32) attrCount = 32;
            pBuffer->physicalDisks[i].attributeCount = attrCount;

            for (int a = 0; a < attrCount; ++a) {
                const auto& sa = src.attributes[a];
                auto& dst = pBuffer->physicalDisks[i].attributes[a];
                dst.id = sa.id;
                dst.flags = sa.flags;
                dst.current = sa.current;
                dst.worst = sa.worst;
                dst.threshold = sa.threshold;
                dst.rawValue = sa.rawValue;
                dst.isCritical = sa.isCritical;
                dst.physicalValue = sa.physicalValue;
                SafeCopyFromWideArray(dst.name, 64, sa.name, 64);
                SafeCopyFromWideArray(dst.description, 128, sa.description, 128);
                SafeCopyFromWideArray(dst.units, 16, sa.units, 16);
            }
        }

        // Temperature sensor information
        pBuffer->tempCount = static_cast<int>(std::min(systemInfo.temperatures.size(), static_cast<size_t>(10)));
        for (int i = 0; i < pBuffer->tempCount; ++i) {
            const auto& temp = systemInfo.temperatures[i];
            SafeCopyWideString(pBuffer->temperatures[i].sensorName, 64,
                              Platform::StringConverter::Utf8ToChar16(temp.first));
            pBuffer->temperatures[i].temperature = temp.second;
        }

        // Independent CPU/GPU temperatures
        pBuffer->cpuTemperature = systemInfo.cpuTemperature;
        pBuffer->gpuTemperature = systemInfo.gpuTemperature;
        pBuffer->cpuUsageSampleIntervalMs = systemInfo.cpuUsageSampleIntervalMs;

        // TPM data (macOS has no TPM, zero it)
        memset(&pBuffer->tpm, 0, sizeof(TpmInfo));
        pBuffer->tpmCount = 0;

        // Update timestamp
        pBuffer->lastUpdate = Platform::SystemTime::Now();

        Logger::Trace("Successfully wrote system/disk/SMART information to shared memory");
    } catch (const std::exception& e) {
        lastError = std::string("Exception in WriteToSharedMemory: ") + e.what();
        Logger::Error(lastError);
    } catch (...) {
        lastError = "Unknown exception in WriteToSharedMemory";
        Logger::Error(lastError);
    }

    // Release mutex
    mutex->Unlock();
}