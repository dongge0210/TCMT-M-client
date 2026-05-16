// SmartReader.cpp — DeviceIoControl SMART reader for Windows physical disks

#include "SmartReader.h"
#include "../DataStruct/DataStruct.h"
#include "../Utils/Logger.h"

#ifdef TCMT_WINDOWS

#include <winsock2.h>
#include <windows.h>
#include <winioctl.h>
#include <ntdddisk.h>
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

#define READ_ATTRIBUTES         0xD0
#define SMART_READ_DATA         0xD0
#define ID_CMD                  0xEC
#endif

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
            // Most drives report temperature at SMART attribute 194 or 190.
            // In the raw data (512 bytes), attributes start at offset 2.
            // Each attribute is 12 bytes: ID(1), Flags(2), Current(1), Worst(1),
            //   RawValue(6), Reserved(1)
            // Temperature is typically in attribute 194 (0xC2) raw byte 5 or 0
            // For SSDs, attribute 190 (0xBE) may have temperature.
            // Simpler: raw byte 115 is the "airflow temperature" on many drives.
            int tempRead = -1;
            for (int attrOff = 2; attrOff < 512 - 12; attrOff += 12) {
                if (raw[attrOff] == 0 || raw[attrOff] == 0xFF) break;
                int attrId = raw[attrOff];
                // Attribute 194 (0xC2): Temperature
                // Attribute 190 (0xBE): Airflow Temperature (SSD)
                if (attrId == 0xBE || attrId == 0xC2) {
                    // Raw value is at offset 5-10 (6 bytes), but normalized temp
                    // is often in the first byte of raw or current value.
                    int val = raw[attrOff + 3]; // Current value (normalized)
                    // Some drives put raw temp in raw[attrOff+5] or raw[attrOff+9]
                    int rawTemp = raw[attrOff + 5];
                    if (rawTemp > 0 && rawTemp < 128) tempRead = rawTemp;
                    else if (val > 0 && val < 128) tempRead = val;
                }
            }
            if (tempRead > 0 && tempRead < 128)
                smartData.temperature = static_cast<double>(tempRead);

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
// Non-Windows stub
bool SmartReader::Read(int, PhysicalDiskSmartData&) { return false; }
#endif
