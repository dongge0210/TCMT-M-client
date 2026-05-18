// SmartReader.cpp — DeviceIoControl SMART reader for Windows physical disks

#include "SmartReader.h"
#include "../DataStruct/DataStruct.h"
#include "../Utils/Logger.h"

#ifdef TCMT_WINDOWS

#include <winsock2.h>
#include <windows.h>
#include <winioctl.h>
#include <ntdddisk.h>
#include <ntddstor.h>
#include <stdio.h>

// SMART_RCV_DRIVE_DATA structure
#ifndef SMART_RCV_DRIVE_DATA
typedef struct _GETVERSIONINPARAMS {
    BYTE bVersion;
    BYTE bRevision;
    BYTE bReserved;
    BYTE bIDEDeviceMap;
    DWORD fCapabilities;
    DWORD dwReserved[4];
} GETVERSIONINPARAMS;

typedef struct _IDEREGS {
    BYTE bFeaturesReg;
    BYTE bSectorCountReg;
    BYTE bSectorNumberReg;
    BYTE bCylLowReg;
    BYTE bCylHighReg;
    BYTE bDriveHeadReg;
    BYTE bCommandReg;
    BYTE bReserved;
} IDEREGS;

typedef struct _SENDCMDINPARAMS {
    DWORD cBufferSize;
    IDEREGS irDriveRegs;
    BYTE bDriveNumber;
    BYTE bReserved[3];
    DWORD dwReserved[4];
    BYTE bBuffer[1];
} SENDCMDINPARAMS;

typedef struct _DRIVERSTATUS {
    BYTE bDriverError;
    BYTE bIDEError;
    BYTE bReserved[2];
    DWORD dwReserved[2];
} DRIVERSTATUS;

typedef struct _SENDCMDOUTPARAMS {
    DWORD cBufferSize;
    DRIVERSTATUS DriverStatus;
    BYTE bBuffer[1];
} SENDCMDOUTPARAMS;

#define SMART_GET_VERSION       CTL_CODE(IOCTL_DISK_BASE, 0x0020, METHOD_BUFFERED, FILE_READ_ACCESS)
#define SMART_RCV_DRIVE_DATA    CTL_CODE(IOCTL_DISK_BASE, 0x0022, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define SMART_CMD               CTL_CODE(IOCTL_DISK_BASE, 0x0021, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#endif // !SMART_RCV_DRIVE_DATA

// These constants are NOT in the SDK — always define
#define READ_ATTRIBUTES         0xD0
#define SMART_READ_DATA         0xD0
#define ID_CMD                  0xEC

// ====================================================================
// Read SMART for one physical disk
// ====================================================================
bool SmartReader::Read(int diskIndex, PhysicalDiskSmartData& smartData) {
    wchar_t path[64];
    swprintf_s(path, 64, L"\\\\.\\PhysicalDrive%d", diskIndex);

    HANDLE hDevice = CreateFileW(path,
                                  GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  nullptr, OPEN_EXISTING, 0, nullptr);
    if (hDevice == INVALID_HANDLE_VALUE) return false;

    bool success = false;
    DWORD bytesReturned = 0;

    // Step 1: Get SMART version / capabilities
    GETVERSIONINPARAMS gvp{};
    if (DeviceIoControl(hDevice, SMART_GET_VERSION,
                        nullptr, 0,
                        &gvp, sizeof(gvp),
                        &bytesReturned, nullptr) &&
        bytesReturned >= sizeof(GETVERSIONINPARAMS) &&
        (gvp.fCapabilities & 1)) {  // SMART supported

        smartData.smartSupported = true;
        smartData.smartEnabled = true;

        // Step 2: Send SMART_READ_DATA command
        BYTE sendBuf[sizeof(SENDCMDINPARAMS) + 512 - 1] = {};
        auto* scip = reinterpret_cast<SENDCMDINPARAMS*>(sendBuf);
        scip->cBufferSize = 512;
        scip->irDriveRegs.bFeaturesReg = READ_ATTRIBUTES;
        scip->irDriveRegs.bSectorCountReg = 1;
        scip->irDriveRegs.bSectorNumberReg = 1;
        scip->irDriveRegs.bCylLowReg = SMART_READ_DATA;
        scip->irDriveRegs.bCylHighReg = 0xC2; // magic
        scip->irDriveRegs.bCommandReg = ID_CMD;
        scip->bDriveNumber = static_cast<BYTE>(diskIndex);

        BYTE outBuf[sizeof(SENDCMDOUTPARAMS) + 512 - 1] = {};

        if (DeviceIoControl(hDevice, SMART_RCV_DRIVE_DATA,
                            sendBuf, sizeof(sendBuf),
                            outBuf, sizeof(outBuf),
                            &bytesReturned, nullptr) &&
            bytesReturned >= sizeof(SENDCMDOUTPARAMS) + 512) {

            auto* scop = reinterpret_cast<SENDCMDOUTPARAMS*>(outBuf);
            const BYTE* raw = scop->bBuffer;  // 512 bytes of SMART attributes

            success = true;

            // --- Temperature ---
            // Most drives: attribute 194 (temp) or 190 (airflow for SSD)
            // Raw value bytes 5-10 (6 bytes LE); temp is often in byte 5 or 9
            double tempRead = -1;
            for (int attrOff = 2; attrOff < 512 - 12; attrOff += 12) {
                if (raw[attrOff] == 0 || raw[attrOff] == 0xFF) break;
                int attrId = raw[attrOff];
                if (attrId == 0xBE || attrId == 0xC2) {
                    for (int bo = 5; bo <= 9; bo += 4) {
                        int t = raw[attrOff + bo];
                        if (t >= 15 && t <= 120) { tempRead = (double)t; break; }
                    }
                    if (tempRead < 0) {
                        // Some drives encode temp in upper byte of raw
                        int hi = (raw[attrOff + 7] << 8) | raw[attrOff + 6];
                        if (hi >= 15 && hi <= 120) tempRead = (double)hi;
                    }
                }
            }
            if (tempRead > 0) smartData.temperature = tempRead;

            // --- Health percentage ---
            // Check critical attributes: 5 (Reallocated), 196 (Reallocated Event),
            // 197 (Current Pending), 198 (Uncorrectable)
            int health = 100;
            for (int attrOff = 2; attrOff < 512 - 12; attrOff += 12) {
                if (raw[attrOff] == 0 || raw[attrOff] == 0xFF) break;
                int attrId = raw[attrOff];
                if (attrId == 5 || attrId == 196 || attrId == 197 || attrId == 198) {
                    // Raw value is 6 bytes LE at offset 5-10
                    uint64_t rawVal = 0;
                    for (int j = 0; j < 6; j++)
                        rawVal |= (static_cast<uint64_t>(raw[attrOff + 5 + j]) << (8 * j));
                    if (rawVal > 0) {
                        if (attrId == 5 && rawVal > 0) health = std::min(health, 95);
                        if (attrId == 5 && rawVal > 10) health = std::min(health, 80);
                        if (attrId == 197 && rawVal > 0) health = std::min(health, 90);
                        if (attrId == 198 && rawVal > 0) health = std::min(health, 85);
                        // Store reallocated
                        if (attrId == 5) smartData.reallocatedSectorCount = rawVal;
                        if (attrId == 197) smartData.currentPendingSector = rawVal;
                        if (attrId == 198) smartData.uncorrectableErrors = rawVal;
                    }
                }
                // Power-on hours (ID 9)
                if (attrId == 9) {
                    uint64_t hours = 0;
                    for (int j = 0; j < 6; j++)
                        hours |= (static_cast<uint64_t>(raw[attrOff + 5 + j]) << (8 * j));
                    if (hours < 100000000)  // sanity check
                        smartData.powerOnHours = hours;
                }
                // Wear leveling count (SSD) — ID 177, 230, 233
                if (attrId == 177 || attrId == 230 || attrId == 233) {
                    int wear = raw[attrOff + 3]; // current normalized value
                    if (wear > 0 && wear <= 100) {
                        smartData.wearLeveling = 1.0 - (wear / 100.0);
                    }
                }
            }
            smartData.healthPercentage = static_cast<uint8_t>(health);
        }
    }

    CloseHandle(hDevice);

    if (!success) {
        // NVMe path: IOCTL_STORAGE_QUERY_PROPERTY + StorageDeviceProtocolSpecificProperty
        // Query NVME_LOG_PAGE_HEALTH_INFO (log page 0x02) — does NOT require admin rights
        HANDLE hNvme = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
        if (hNvme != INVALID_HANDLE_VALUE) {
            BYTE nvmeBuf[sizeof(STORAGE_PROPERTY_QUERY) + sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) + 512] = {};

            auto* query = reinterpret_cast<STORAGE_PROPERTY_QUERY*>(nvmeBuf);
            query->PropertyId = StorageDeviceProtocolSpecificProperty;
            query->QueryType = PropertyStandardQuery;

            auto* protocolData = reinterpret_cast<STORAGE_PROTOCOL_SPECIFIC_DATA*>(
                nvmeBuf + sizeof(STORAGE_PROPERTY_QUERY));
            protocolData->ProtocolType = ProtocolTypeNvme;
            protocolData->DataType = NVMeDataTypeLogPage;
            protocolData->ProtocolDataRequestValue = NVME_LOG_PAGE_HEALTH_INFO;
            protocolData->ProtocolDataRequestSubValue = 0;
            protocolData->ProtocolDataOffset = sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA);
            protocolData->ProtocolDataLength = sizeof(NVME_HEALTH_INFO_LOG);

            DWORD nvmeBytesReturned = 0;
            if (DeviceIoControl(hNvme, IOCTL_STORAGE_QUERY_PROPERTY,
                                nvmeBuf, sizeof(STORAGE_PROPERTY_QUERY) + sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA),
                                nvmeBuf, sizeof(nvmeBuf),
                                &nvmeBytesReturned, nullptr)) {

                auto* desc = reinterpret_cast<STORAGE_PROTOCOL_DATA_DESCRIPTOR*>(nvmeBuf);
                auto* retProtocolData = &desc->ProtocolSpecificData;
                auto* healthLog = reinterpret_cast<NVME_HEALTH_INFO_LOG*>(
                    reinterpret_cast<BYTE*>(retProtocolData) + retProtocolData->ProtocolDataOffset);

                smartData.smartSupported = true;
                smartData.smartEnabled = true;
                success = true;

                // Temperature: composite temp (2 bytes LE, Kelvin) → Celsius
                USHORT tempKelvin = healthLog->Temperature[0] | (healthLog->Temperature[1] << 8);
                if (tempKelvin > 0) {
                    int tempC = static_cast<int>(tempKelvin) - 273;
                    if (tempC > 0 && tempC < 128) {
                        smartData.temperature = static_cast<double>(tempC);
                    }
                }

                // Health percentage (100 - PercentageUsed)
                uint8_t pctUsed = healthLog->PercentageUsed;
                if (pctUsed <= 100) {
                    smartData.healthPercentage = static_cast<uint8_t>(100 - pctUsed);
                    smartData.wearLeveling = pctUsed / 100.0;
                }

                // Power-on hours (128-bit field at byte offset 32, read first 8 bytes)
                uint64_t hours = 0;
                memcpy(&hours, reinterpret_cast<const BYTE*>(healthLog) + 32, sizeof(uint64_t));
                if (hours > 0 && hours < 100000000) {
                    smartData.powerOnHours = hours;
                }

                // AvailableSpare below threshold indicates degraded health
                if (healthLog->AvailableSpare < healthLog->AvailableSpareThreshold) {
                    smartData.healthPercentage = (std::min)(smartData.healthPercentage, uint8_t(50));
                }

                // Critical warning set indicates serious drive issue
                if (healthLog->CriticalWarning > 0) {
                    smartData.healthPercentage = (std::min)(smartData.healthPercentage, uint8_t(30));
                }
            }
            CloseHandle(hNvme);
        }
    }

    if (!success) {
        // Fallback: try StorageDeviceTemperatureProperty (Windows 10+)
        success = false;
        HANDLE hDev2 = CreateFileW(path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
        if (hDev2 != INVALID_HANDLE_VALUE) {
            // STORAGE_TEMPERATURE_DATA_DESCRIPTOR
            BYTE tempBuf[64] = {};
            STORAGE_PROPERTY_QUERY query{};
            query.PropertyId = StorageDeviceTemperatureProperty;
            query.QueryType = PropertyStandardQuery;
            if (DeviceIoControl(hDev2, IOCTL_STORAGE_QUERY_PROPERTY,
                                &query, sizeof(query),
                                tempBuf, sizeof(tempBuf),
                                &bytesReturned, nullptr) &&
                bytesReturned >= 8) {
                // First 8 bytes: Version(4) + Size(4), then STORAGE_TEMPERATURE_INFO array
                int temp = static_cast<int>(*(SHORT*)(tempBuf + 8));
                if (temp > 0 && temp < 128) {
                    smartData.temperature = static_cast<double>(temp);
                    success = true;
                }
            }
            CloseHandle(hDev2);
        }
    }

    return success;
}

#else
// macOS/Linux stub — delegates to platform implementation
extern "C" bool SmartReaderMacRead(int diskIndex, PhysicalDiskSmartData& smartData);
bool SmartReader::Read(int diskIndex, PhysicalDiskSmartData& smartData) {
    return SmartReaderMacRead(diskIndex, smartData);
}
#endif
