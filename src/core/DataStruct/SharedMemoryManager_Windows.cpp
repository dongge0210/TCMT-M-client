
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
tcmt::ipc::IPCDataBlock* SharedMemoryManager::pBuffer = nullptr;
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
        sizeof(tcmt::ipc::IPCDataBlock),
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
            sizeof(tcmt::ipc::IPCDataBlock),
            L"Local\\SystemMonitorSharedMemory"
        );
        if (hMapFile == NULL) {
            hMapFile = CreateFileMapping(
                INVALID_HANDLE_VALUE,
                &securityAttributes,
                PAGE_READWRITE,
                0,
                sizeof(tcmt::ipc::IPCDataBlock),
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
    pBuffer = static_cast<tcmt::ipc::IPCDataBlock*>(
        MapViewOfFile(hMapFile, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(tcmt::ipc::IPCDataBlock))
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
        memset(pBuffer, 0, sizeof(tcmt::ipc::IPCDataBlock));
    }

    Logger::Info("Shared memory successfully initialized, sizeof(IPCDataBlock)=" + std::to_string(sizeof(tcmt::ipc::IPCDataBlock)));
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

    auto SafeCopyStr = [](char* dest, size_t destSize, const std::string& src) {
        try {
            if (dest == nullptr || destSize == 0) return;
            memset(dest, 0, destSize);
            if (src.empty()) { dest[0] = '\0'; return; }
            size_t copyLen = (std::min)(src.length(), destSize - 1);
            for (size_t i = 0; i < copyLen; ++i) dest[i] = src[i];
            dest[copyLen] = '\0';
        } catch (...) { if (dest && destSize > 0) dest[0] = '\0'; }
    };

    try {
        // CPU (narrow int→uint8_t, double→float)
        SafeCopyStr(pBuffer->cpuName, sizeof(pBuffer->cpuName), systemInfo.cpuName);
        pBuffer->physicalCores = static_cast<uint8_t>((std::min)(systemInfo.physicalCores, 255));
        pBuffer->logicalCores = static_cast<uint8_t>((std::min)(systemInfo.logicalCores, 255));
        pBuffer->cpuUsage = static_cast<float>(systemInfo.cpuUsage);
        pBuffer->performanceCores = static_cast<uint8_t>((std::min)(systemInfo.performanceCores, 255));
        pBuffer->efficiencyCores = static_cast<uint8_t>((std::min)(systemInfo.efficiencyCores, 255));
        pBuffer->pCoreFreq = static_cast<float>(systemInfo.performanceCoreFreq);
        pBuffer->eCoreFreq = static_cast<float>(systemInfo.efficiencyCoreFreq);
        pBuffer->cpuBaseFreq = static_cast<float>(systemInfo.cpuBaseFreq);
        pBuffer->hyperThreading = systemInfo.hyperThreading;
        pBuffer->virtualization = systemInfo.virtualization;
        pBuffer->cpuTemp = static_cast<float>(systemInfo.cpuTemperature);
        pBuffer->cpuPcoreTemp = static_cast<float>(systemInfo.cpuPcoreTemperature);
        pBuffer->cpuEcoreTemp = static_cast<float>(systemInfo.cpuEcoreTemperature);
        pBuffer->cpuSampleIntervalMs = static_cast<float>(systemInfo.cpuUsageSampleIntervalMs);

        // Memory
        pBuffer->totalMemory = systemInfo.totalMemory;
        pBuffer->usedMemory = systemInfo.usedMemory;
        pBuffer->availableMemory = systemInfo.availableMemory;
        pBuffer->compressedMemory = systemInfo.compressedMemory;
        pBuffer->swapUsed = systemInfo.swapUsed;
        pBuffer->swapTotal = systemInfo.swapTotal;
        pBuffer->ramSpeed = systemInfo.ramSpeed;
        SafeCopyStr(pBuffer->ramType, sizeof(pBuffer->ramType), systemInfo.ramType);

        // Battery / power
        pBuffer->batteryPercent = systemInfo.batteryPercent;
        pBuffer->acOnline = systemInfo.acOnline;
        pBuffer->cpuPower = static_cast<float>(systemInfo.cpuPower);
        pBuffer->gpuPower = static_cast<float>(systemInfo.gpuPower);
        pBuffer->anePower = static_cast<float>(systemInfo.anePower);

        // OS
        SafeCopyStr(pBuffer->osVersion, sizeof(pBuffer->osVersion), systemInfo.osVersion);
        SafeCopyStr(pBuffer->hardwareModel, sizeof(pBuffer->hardwareModel), systemInfo.hardwareModel);

        // GPU
        SafeCopyStr(pBuffer->gpuName, sizeof(pBuffer->gpuName), systemInfo.gpuName);
        SafeCopyStr(pBuffer->gpuBrand, sizeof(pBuffer->gpuBrand), systemInfo.gpuBrand);
        pBuffer->gpuMemory = systemInfo.gpuMemory;
        pBuffer->gpuMemoryPercent = static_cast<float>(systemInfo.gpuCoreFreq); // NVML VRAM%
        pBuffer->gpuUsage = static_cast<float>(systemInfo.gpuUsage);
        pBuffer->gpuTemp = static_cast<float>(systemInfo.gpuTemperature);
        pBuffer->gpuFreq = static_cast<float>(systemInfo.gpuFreq);
        pBuffer->gpuIsVirtual = systemInfo.gpuIsVirtual;

        // Network adapters
        pBuffer->adapterCount = 0;
        int adapterWriteCount = static_cast<int>((std::min)(systemInfo.adapters.size(), static_cast<size_t>(4)));
        for (int i = 0; i < adapterWriteCount; ++i) {
            const auto& src = systemInfo.adapters[i];
            SafeCopyStr(pBuffer->adapters[i].name, sizeof(pBuffer->adapters[i].name),
                        WinUtils::WstringToString(src.name));
            SafeCopyStr(pBuffer->adapters[i].ip, sizeof(pBuffer->adapters[i].ip),
                        WinUtils::WstringToString(src.ipAddress));
            SafeCopyStr(pBuffer->adapters[i].mac, sizeof(pBuffer->adapters[i].mac),
                        WinUtils::WstringToString(src.mac));
            SafeCopyStr(pBuffer->adapters[i].type, sizeof(pBuffer->adapters[i].type),
                        WinUtils::WstringToString(src.adapterType));
            pBuffer->adapters[i].speed = src.speed;
            pBuffer->adapters[i].downloadSpeed = src.downloadSpeed;
            pBuffer->adapters[i].uploadSpeed = src.uploadSpeed;
        }
        pBuffer->adapterCount = static_cast<uint8_t>(adapterWriteCount);
        if (adapterWriteCount == 0 && !systemInfo.networkAdapterName.empty()) {
            SafeCopyStr(pBuffer->adapters[0].name, sizeof(pBuffer->adapters[0].name), systemInfo.networkAdapterName);
            SafeCopyStr(pBuffer->adapters[0].mac, sizeof(pBuffer->adapters[0].mac), systemInfo.networkAdapterMac);
            SafeCopyStr(pBuffer->adapters[0].ip, sizeof(pBuffer->adapters[0].ip), systemInfo.networkAdapterIp);
            SafeCopyStr(pBuffer->adapters[0].type, sizeof(pBuffer->adapters[0].type), systemInfo.networkAdapterType);
            pBuffer->adapters[0].speed = systemInfo.networkAdapterSpeed;
            pBuffer->adapterCount = 1;
        }

        // Logical disks
        pBuffer->diskCount = static_cast<uint8_t>((std::min)(systemInfo.disks.size(), static_cast<size_t>(4)));
        for (int i = 0; i < static_cast<int>(pBuffer->diskCount); ++i) {
            const auto& disk = systemInfo.disks[i];
            SafeCopyStr(pBuffer->disks[i].label, sizeof(pBuffer->disks[i].label), disk.label);
            SafeCopyStr(pBuffer->disks[i].fs, sizeof(pBuffer->disks[i].fs), disk.fileSystem);
            pBuffer->disks[i].totalSize = disk.totalSize;
            pBuffer->disks[i].usedSpace = disk.usedSpace;
            pBuffer->disks[i].freeSpace = disk.freeSpace;
        }

        // Physical disks + SMART (simplified: model/serial/capacity/interface/temp/health/attrsJson)
        pBuffer->physDiskCount = static_cast<uint8_t>((std::min)(systemInfo.physicalDisks.size(), static_cast<size_t>(8)));
        for (int i = 0; i < static_cast<int>(pBuffer->physDiskCount); ++i) {
            const auto& src = systemInfo.physicalDisks[i];
            SafeCopyStr(pBuffer->physicalDisks[i].model, sizeof(pBuffer->physicalDisks[i].model),
                        WinUtils::WstringToString(src.model));
            SafeCopyStr(pBuffer->physicalDisks[i].serial, sizeof(pBuffer->physicalDisks[i].serial),
                        WinUtils::WstringToString(src.serialNumber));
            SafeCopyStr(pBuffer->physicalDisks[i].interfaceType, sizeof(pBuffer->physicalDisks[i].interfaceType),
                        WinUtils::WstringToString(src.interfaceType));
            pBuffer->physicalDisks[i].capacity = src.capacity;
            pBuffer->physicalDisks[i].temperature = static_cast<float>(src.temperature);
            pBuffer->physicalDisks[i].healthPercent = static_cast<float>(src.healthPercentage);
            pBuffer->physicalDisks[i].smartSupported = src.smartSupported;
            int attrCount = src.attributeCount;
            if (attrCount < 0) attrCount = 0; if (attrCount > 32) attrCount = 32;
            pBuffer->physicalDisks[i].attrCount = attrCount;
            strncpy_s(pBuffer->physicalDisks[i].attrsJson, sizeof(pBuffer->physicalDisks[i].attrsJson), src.attrsJson, _TRUNCATE);
        }

        // Temperature sensors
        pBuffer->tempCount = static_cast<uint8_t>((std::min)(systemInfo.temperatures.size(), static_cast<size_t>(10)));
        for (int i = 0; i < static_cast<int>(pBuffer->tempCount); ++i) {
            const auto& temp = systemInfo.temperatures[i];
            SafeCopyStr(pBuffer->temperatures[i].name, sizeof(pBuffer->temperatures[i].name), temp.first);
            pBuffer->temperatures[i].value = static_cast<float>(temp.second);
        }

        // TPM
        pBuffer->tpmCount = 0;
        if (!systemInfo.tpms.empty()) {
            const auto& src = systemInfo.tpms[0];
            SafeCopyStr(pBuffer->tpm.manufacturer, sizeof(pBuffer->tpm.manufacturer),
                        WinUtils::WstringToString(src.manufacturer));
            SafeCopyStr(pBuffer->tpm.firmwareVersion, sizeof(pBuffer->tpm.firmwareVersion),
                        WinUtils::WstringToString(src.firmwareVersion));
            pBuffer->tpm.firmwareVersionMajor = src.firmwareVersionMajor;
            pBuffer->tpm.firmwareVersionMinor = src.firmwareVersionMinor;
            pBuffer->tpm.firmwareVersionBuild = src.firmwareVersionBuild;
            pBuffer->tpm.isPresent = src.isPresent;
            pBuffer->tpm.isEnabled = src.isEnabled;
            pBuffer->tpm.isActive = src.isActive;
            pBuffer->tpm.selfTestStatus = src.selfTestStatus;
            pBuffer->tpm.status = src.isPresent ? 1 : 3;
            pBuffer->tpmCount = 1;
        }

        // WiFi
        pBuffer->wifi.powerOn = systemInfo.wifiPowerOn;
        pBuffer->wifi.isConnected = systemInfo.wifiIsConnected;
        pBuffer->wifi.rssi = systemInfo.wifiRSSI;
        pBuffer->wifi.channel = systemInfo.wifiChannel;
        SafeCopyStr(pBuffer->wifi.ssid, sizeof(pBuffer->wifi.ssid), systemInfo.wifiSSID);
        SafeCopyStr(pBuffer->wifi.security, sizeof(pBuffer->wifi.security), systemInfo.wifiSecurity);
        SafeCopyStr(pBuffer->wifi.band, sizeof(pBuffer->wifi.band), systemInfo.wifiBand);
        SafeCopyStr(pBuffer->wifi.wifiGen, sizeof(pBuffer->wifi.wifiGen), systemInfo.wifiGen);

        // Bluetooth
        pBuffer->bluetooth.powerOn = systemInfo.btPowerOn;
        pBuffer->bluetooth.deviceCount = systemInfo.btDeviceCount;

        // App version
        SafeCopyStr(pBuffer->appVersion, sizeof(pBuffer->appVersion), systemInfo.appVersion);

        Logger::Trace("Successfully wrote system info to IPC shared memory");
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