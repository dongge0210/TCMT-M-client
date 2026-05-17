
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <algorithm>
#include <atomic>
#include <cctype>

// winsock2.h must be before windows.h to avoid symbol redefinition
#include <winsock2.h>
#include <Windows.h>

#include "SharedMemoryManager.h"
#include "../Platform/Platform.h"
#include "../Utils/WinUtils.h"
#include "../Utils/Logger.h"
#include <iostream>
#include <sstream>
#include <stdexcept>

#ifndef WINUTILS_IMPLEMENTED
// Fallback implementation for FormatWindowsErrorMessage
inline std::string FallbackFormatWindowsErrorMessage(DWORD errorCode) {
    std::stringstream ss;
    ss << "Error code: " << errorCode;
    return ss.str();
}
#endif


// Initialize static members
HANDLE SharedMemoryManager::hMapFile = NULL;
SharedMemoryBlock* SharedMemoryManager::pBuffer = nullptr;
std::string SharedMemoryManager::lastError = "";
// Cross-process mutex for synchronizing shared memory
static HANDLE g_hMutex = NULL;

bool SharedMemoryManager::InitSharedMemory() {
    // Clear any previous error
    lastError.clear();

    try {
        // Try to enable privileges needed for creating global objects
        bool hasPrivileges = WinUtils::EnablePrivilege(L"SeCreateGlobalPrivilege");
        if (!hasPrivileges) {
            Logger::Warn("Failed to enable SeCreateGlobalPrivilege - attempting to continue");
        }
    } catch(...) {
        Logger::Warn("Exception occurred when enabling SeCreateGlobalPrivilege - attempting to continue");
        // Continue execution as this is not critical
    }

    // Create global mutex for multi-process synchronization
    if (!g_hMutex) {
        g_hMutex = CreateMutexW(NULL, FALSE, L"Global\\SystemMonitorSharedMemoryMutex");
        if (!g_hMutex) {
            Logger::Error("Failed to create global mutex for shared memory synchronization");
            return false;
        }
    }

    // Create security attributes to allow sharing between processes
    SECURITY_ATTRIBUTES securityAttributes;
    SECURITY_DESCRIPTOR securityDescriptor;

    // Initialize the security descriptor
    if (!InitializeSecurityDescriptor(&securityDescriptor, SECURITY_DESCRIPTOR_REVISION)) {
        DWORD errorCode = ::GetLastError();
        std::stringstream ss;
        ss << "Failed to initialize security descriptor. Error code: " << errorCode
           << " ("
           #ifdef WINUTILS_IMPLEMENTED
                << WinUtils::FormatWindowsErrorMessage(errorCode)
           #else
                << FallbackFormatWindowsErrorMessage(errorCode)
           #endif
           << ")";
        lastError = ss.str();
        Logger::Error(lastError);
        return false;
    }

    // Set the DACL to NULL for unrestricted access
    if (!SetSecurityDescriptorDacl(&securityDescriptor, TRUE, NULL, FALSE)) {
        DWORD errorCode = ::GetLastError();
        std::stringstream ss;
        ss << "Failed to set security descriptor DACL. Error code: " << errorCode
           << " ("
           #ifdef WINUTILS_IMPLEMENTED
                << WinUtils::FormatWindowsErrorMessage(errorCode)
           #else
                << FallbackFormatWindowsErrorMessage(errorCode)
           #endif
           << ")";
        lastError = ss.str();
        Logger::Error(lastError);
        return false;
    }

    // Setup security attributes
    securityAttributes.nLength = sizeof(SECURITY_ATTRIBUTES);
    securityAttributes.lpSecurityDescriptor = &securityDescriptor;
    securityAttributes.bInheritHandle = FALSE;

    // Create file mapping object in Global namespace
    hMapFile = CreateFileMapping(
        INVALID_HANDLE_VALUE,
        &securityAttributes,
        PAGE_READWRITE,
        0,
        sizeof(SharedMemoryBlock),
        L"Global\\SystemMonitorSharedMemory"
    );
    if (hMapFile == NULL) {
        DWORD errorCode = ::GetLastError();
        // Fallback if Global is not permitted, try Local or no prefix
        Logger::Warn("Failed to create global shared memory, trying local namespace");

        hMapFile = CreateFileMapping(
            INVALID_HANDLE_VALUE,
            &securityAttributes,
            PAGE_READWRITE,
            0,
            sizeof(SharedMemoryBlock),
            L"Local\\SystemMonitorSharedMemory"
        );
        if (hMapFile == NULL) {
            hMapFile = CreateFileMapping(
                INVALID_HANDLE_VALUE,
                &securityAttributes,
                PAGE_READWRITE,
                0,
                sizeof(SharedMemoryBlock),
                L"SystemMonitorSharedMemory"
            );
        }

        // If still NULL after fallbacks, report error
        if (hMapFile == NULL) {
            errorCode = ::GetLastError();
            std::stringstream ss;
            ss << "Failed to create shared memory. Error code: " << errorCode
               << " ("
               #ifdef WINUTILS_IMPLEMENTED
                    << WinUtils::FormatWindowsErrorMessage(errorCode)
               #else
                    << FallbackFormatWindowsErrorMessage(errorCode)
               #endif
               << ")";
            // Possibly shared memory already exists
            if (errorCode == ERROR_ALREADY_EXISTS) {
                ss << " (Shared memory already exists)";
            }
            lastError = ss.str();
            Logger::Error(lastError);
            return false;
        }
    }

    // Check if we created a new mapping or opened an existing one
    DWORD errorCode = ::GetLastError();
    if (errorCode == ERROR_ALREADY_EXISTS) {
        Logger::Info("Opened existing shared memory mapping.");
    } else {
        Logger::Info("Created new shared memory mapping.");
    }

    // Map to process address space
    pBuffer = static_cast<SharedMemoryBlock*>(
        MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedMemoryBlock))
    );
    if (pBuffer == nullptr) {
        DWORD errorCode = ::GetLastError();
        std::stringstream ss;
        ss << "Failed to map shared memory view. Error code: " << errorCode
           << " ("
           #ifdef WINUTILS_IMPLEMENTED
                << WinUtils::FormatWindowsErrorMessage(errorCode)
           #else
                << FallbackFormatWindowsErrorMessage(errorCode)
           #endif
           << ")";
        lastError = ss.str();
        Logger::Error(lastError);
        CloseHandle(hMapFile);
        hMapFile = NULL;
        return false;
    }


    // No longer initialize CriticalSection in shared memory structure

    // Zero out the shared memory to avoid dirty data (only on first creation)
    if (errorCode != ERROR_ALREADY_EXISTS) {
        memset(pBuffer, 0, sizeof(SharedMemoryBlock));
    }

    Logger::Info("Shared memory successfully initialized, sizeof(SharedMemoryBlock)=" + std::to_string(sizeof(SharedMemoryBlock)));
    return true;
}

void SharedMemoryManager::CleanupSharedMemory() {
    if (pBuffer) {
        UnmapViewOfFile(pBuffer);
        pBuffer = nullptr;
    }
    if (hMapFile) {
        CloseHandle(hMapFile);
        hMapFile = NULL;
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

    DWORD waitResult = WaitForSingleObject(g_hMutex, 5000);
    if (waitResult != WAIT_OBJECT_0 && waitResult != WAIT_ABANDONED) {
        Logger::Critical("Failed to acquire shared memory mutex");
        return;
    }
    if (waitResult == WAIT_ABANDONED) {
        Logger::Warn("Acquired abandoned shared memory mutex - previous owner may have crashed");
    }

    // seqlock: mark write in progress (odd)
    pBuffer->writeSequence++;
    std::atomic_thread_fence(std::memory_order_release);

    auto SafeCopyWideString = [](wchar_t* dest, size_t destSize, const std::wstring& src) {
        try {
            if (dest == nullptr || destSize == 0) return;
            memset(dest, 0, destSize * sizeof(wchar_t));
            if (src.empty()) { dest[0] = L'\0'; return; }
            size_t copyLen = std::min<size_t>(src.length(), destSize - 1);
            for (size_t i = 0; i < copyLen; ++i) dest[i] = src[i];
            dest[copyLen] = L'\0';
        } catch (...) { if (dest && destSize > 0) dest[0] = L'\0'; }
    };
    auto SafeCopyFromWideArray = [](wchar_t* dest, size_t destSize, const wchar_t* src, size_t srcCapacity) {
        if (!dest || destSize == 0) return;
        memset(dest, 0, destSize * sizeof(wchar_t));
        if (!src) return;
        size_t len = 0;
        while (len < srcCapacity && src[len] != L'\0') ++len;
        if (len >= destSize) len = destSize - 1;
        for (size_t i = 0; i < len; ++i) dest[i] = src[i];
        dest[len] = L'\0';
    };
    try {
        // Clear main string/array areas
        memset(pBuffer->cpuName, 0, sizeof(pBuffer->cpuName));
        for (int i = 0; i < 2; ++i) { memset(pBuffer->gpus[i].name, 0, sizeof(pBuffer->gpus[i].name)); memset(pBuffer->gpus[i].brand, 0, sizeof(pBuffer->gpus[i].brand)); }
        for (int i = 0; i < 8; ++i) { memset(&pBuffer->disks[i], 0, sizeof(pBuffer->disks[i])); memset(&pBuffer->physicalDisks[i], 0, sizeof(pBuffer->physicalDisks[i])); }
        for (int i = 0; i < 10; ++i) { memset(pBuffer->temperatures[i].sensorName, 0, sizeof(pBuffer->temperatures[i].sensorName)); }

        // Battery / power
        pBuffer->batteryPercent = systemInfo.batteryPercent;
        pBuffer->acOnline = systemInfo.acOnline;

        // OS version
        SafeCopyWideString(pBuffer->osVersion, 128, WinUtils::StringToWstring(systemInfo.osVersion));

        // CPU
        SafeCopyWideString(pBuffer->cpuName, 128, WinUtils::StringToWstring(systemInfo.cpuName));
        pBuffer->physicalCores = systemInfo.physicalCores;
        pBuffer->logicalCores = systemInfo.logicalCores;
        pBuffer->cpuUsage = systemInfo.cpuUsage;
        pBuffer->performanceCores = systemInfo.performanceCores;
        pBuffer->efficiencyCores = systemInfo.efficiencyCores;
        pBuffer->pCoreFreq = systemInfo.performanceCoreFreq;
        pBuffer->eCoreFreq = systemInfo.efficiencyCoreFreq;
        pBuffer->pCoreBaseFreq = systemInfo.pCoreBaseFreq;
        pBuffer->eCoreBaseFreq = systemInfo.eCoreBaseFreq;
        pBuffer->hyperThreading = systemInfo.hyperThreading;
        pBuffer->virtualization = systemInfo.virtualization;

        // Memory
        pBuffer->totalMemory = systemInfo.totalMemory;
        pBuffer->usedMemory = systemInfo.usedMemory;
        pBuffer->availableMemory = systemInfo.availableMemory;
        pBuffer->compressedMemory = systemInfo.compressedMemory;

        // GPU (compatible with old fields)
        pBuffer->gpuCount = 0;
        if (!systemInfo.gpuName.empty()) {
            SafeCopyWideString(pBuffer->gpus[0].name, 128, WinUtils::StringToWstring(systemInfo.gpuName));
            SafeCopyWideString(pBuffer->gpus[0].brand, 64, WinUtils::StringToWstring(systemInfo.gpuBrand));
            pBuffer->gpus[0].memory = systemInfo.gpuMemory;
            pBuffer->gpus[0].coreClock = systemInfo.gpuCoreFreq;
            pBuffer->gpus[0].isVirtual = systemInfo.gpuIsVirtual;
            pBuffer->gpus[0].usage = systemInfo.gpuUsage;
            pBuffer->gpuCount = 1;
        }
        // If later want to support vector<GPUData>, can extend here

        // Network adapters (NetworkAdapterData in SystemInfo.adapters has wchar_t array fields)
        pBuffer->adapterCount = 0;
        int adapterWriteCount = static_cast<int>(std::min<size_t>(systemInfo.adapters.size(), size_t(4)));
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
        if (adapterWriteCount == 0 && !systemInfo.networkAdapterName.empty()) {
            SafeCopyWideString(pBuffer->adapters[0].name, 128, WinUtils::StringToWstring(systemInfo.networkAdapterName));
            SafeCopyWideString(pBuffer->adapters[0].mac, 32, WinUtils::StringToWstring(systemInfo.networkAdapterMac));
            SafeCopyWideString(pBuffer->adapters[0].ipAddress, 64, WinUtils::StringToWstring(systemInfo.networkAdapterIp));
            SafeCopyWideString(pBuffer->adapters[0].adapterType, 32, WinUtils::StringToWstring(systemInfo.networkAdapterType));
            pBuffer->adapters[0].speed = systemInfo.networkAdapterSpeed;
            pBuffer->adapterCount = 1;
        }

        // Logical disks (label / fileSystem in SystemInfo.disks are std::string)
        pBuffer->diskCount = static_cast<int>(std::min<size_t>(systemInfo.disks.size(), static_cast<size_t>(8)));
        for (int i = 0; i < pBuffer->diskCount; ++i) {
            const auto& disk = systemInfo.disks[i];
            pBuffer->disks[i].letter = disk.letter;
            std::string safeLabel = disk.label;
            if (safeLabel.empty()) safeLabel = ""; // Allow unnamed, replace in UI
            else if (!WinUtils::IsLikelyUtf8(safeLabel)) {
                // Degraded handling: convert via current ACP to wide then back to UTF-8, try to salvage
                std::wstring w = WinUtils::Utf8ToWstring(safeLabel); // If not utf8 will get empty
                if (w.empty()) {
                    int len = MultiByteToWideChar(CP_ACP, 0, safeLabel.c_str(), (int)safeLabel.size(), nullptr, 0);
                    if (len > 0) { w.resize(len); MultiByteToWideChar(CP_ACP, 0, safeLabel.c_str(), (int)safeLabel.size(), &w[0], len); }
                }
                safeLabel = WinUtils::WstringToUtf8(w);
            }
            SafeCopyWideString(pBuffer->disks[i].label, 128, WinUtils::StringToWstring(safeLabel));
            SafeCopyWideString(pBuffer->disks[i].fileSystem, 32, WinUtils::StringToWstring(disk.fileSystem));
            pBuffer->disks[i].totalSize = disk.totalSize;
            pBuffer->disks[i].usedSpace = disk.usedSpace;
            pBuffer->disks[i].freeSpace = disk.freeSpace;
        }

        // Physical disks + SMART (fields in SystemInfo.physicalDisks are already wchar_t arrays)
        pBuffer->physicalDiskCount = static_cast<int>(std::min<size_t>(systemInfo.physicalDisks.size(), static_cast<size_t>(8)));
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
            int ldCount = 0;
            for (char l : src.logicalDriveLetters) {
                if (ldCount >= 8 || l == 0) break;
                if (std::isalpha(static_cast<unsigned char>(l))) pBuffer->physicalDisks[i].logicalDriveLetters[ldCount++] = l;
            }
            pBuffer->physicalDisks[i].logicalDriveCount = ldCount;
            int attrCount = src.attributeCount;
            if (attrCount < 0) attrCount = 0; if (attrCount > 32) attrCount = 32;
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

        // Temperature array (sensor names in vector<pair<string,double>>)
        pBuffer->tempCount = static_cast<int>(std::min<size_t>(systemInfo.temperatures.size(), static_cast<size_t>(10)));
        for (int i = 0; i < pBuffer->tempCount; ++i) {
            const auto& temp = systemInfo.temperatures[i];
            SafeCopyWideString(pBuffer->temperatures[i].sensorName, 64, WinUtils::StringToWstring(temp.first));
            pBuffer->temperatures[i].temperature = temp.second;
        }

        // Independent CPU / GPU temperatures
        pBuffer->cpuTemperature = systemInfo.cpuTemperature;
        pBuffer->gpuTemperature = systemInfo.gpuTemperature;
        pBuffer->cpuUsageSampleIntervalMs = systemInfo.cpuUsageSampleIntervalMs;

        // TPM info
        memset(&pBuffer->tpm, 0, sizeof(pBuffer->tpm));
        pBuffer->tpmCount = 0;
        if (!systemInfo.tpms.empty()) {
            const auto& src = systemInfo.tpms[0];
            SafeCopyFromWideArray(pBuffer->tpm.manufacturer, 32, src.manufacturer, 32);
            SafeCopyFromWideArray(pBuffer->tpm.firmwareVersion, 32, src.firmwareVersion, 32);
            pBuffer->tpm.firmwareVersionMajor = src.firmwareVersionMajor;
            pBuffer->tpm.firmwareVersionMinor = src.firmwareVersionMinor;
            pBuffer->tpm.firmwareVersionBuild = src.firmwareVersionBuild;
            pBuffer->tpm.isPresent = src.isPresent;
            pBuffer->tpm.isEnabled = src.isEnabled;
            pBuffer->tpm.isActive = src.isActive;
            pBuffer->tpm.status = src.isPresent ? 1 : 3; // 1=OK, 3=Disabled
            pBuffer->tpmCount = 1;
        }

        // WiFi
        pBuffer->wifi.powerOn = systemInfo.wifiPowerOn;
        pBuffer->wifi.isConnected = systemInfo.wifiIsConnected;
        pBuffer->wifi.rssi = systemInfo.wifiRSSI;
        pBuffer->wifi.channel = systemInfo.wifiChannel;
        SafeCopyWideString(pBuffer->wifi.ssid, 32, WinUtils::StringToWstring(systemInfo.wifiSSID));
        SafeCopyWideString(pBuffer->wifi.security, 16, WinUtils::StringToWstring(systemInfo.wifiSecurity));
        SafeCopyWideString(pBuffer->wifi.band, 8, WinUtils::StringToWstring(systemInfo.wifiBand));
        SafeCopyWideString(pBuffer->wifi.wifiGen, 12, WinUtils::StringToWstring(systemInfo.wifiGen));

        // Bluetooth
        pBuffer->bluetooth.powerOn = systemInfo.btPowerOn;
        pBuffer->bluetooth.deviceCount = systemInfo.btDeviceCount;

        pBuffer->lastUpdate = Platform::SystemTime::Now();
        Logger::Trace("Successfully wrote system/disk/SMART information to shared memory");
    } catch (const std::exception& e) {
        // seqlock: mark write complete (even) despite failure
        std::atomic_thread_fence(std::memory_order_release);
        pBuffer->writeSequence++;
        lastError = std::string("Exception in WriteToSharedMemory: ") + e.what();
        Logger::Error(lastError);
        ReleaseMutex(g_hMutex);
        return;
    } catch (...) {
        // seqlock: mark write complete (even) despite failure
        std::atomic_thread_fence(std::memory_order_release);
        pBuffer->writeSequence++;
        lastError = "Unknown exception in WriteToSharedMemory";
        Logger::Error(lastError);
        ReleaseMutex(g_hMutex);
        return;
    }
    // seqlock: mark write complete (even)
    std::atomic_thread_fence(std::memory_order_release);
    pBuffer->writeSequence++;
    ReleaseMutex(g_hMutex);
}