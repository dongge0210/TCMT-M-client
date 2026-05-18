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

// NVME_LOG_PAGE_HEALTH_INFO — may be missing in older SDKs
#ifndef NVME_LOG_PAGE_HEALTH_INFO
#define NVME_LOG_PAGE_HEALTH_INFO    0x02
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
    bool success = false;
    DWORD bytesReturned = 0;

    // ATA path — only try if we got a handle (NVMe drives reject GENERIC_WRITE)
    if (hDevice != INVALID_HANDLE_VALUE) {
        // Step 1: Get SMART version / capabilities
        GETVERSIONINPARAMS gvp{};
        DWORD gvpErr = 0;
        bool gvpOk = DeviceIoControl(hDevice, SMART_GET_VERSION,
                                     nullptr, 0,
                                     &gvp, sizeof(gvp),
                                     &bytesReturned, nullptr);
        if (!gvpOk) gvpErr = GetLastError();
        static int diagCount2 = 0;
        if (diagCount2 < 5) {
            Logger::Info("SMART disk" + std::to_string(diskIndex) +
                " ATA: handleOk gvp=" + std::to_string(gvpOk) +
                " sz=" + std::to_string(bytesReturned) +
                " caps=" + std::to_string(gvp.fCapabilities) +
                " err=" + std::to_string(gvpErr));
            diagCount2++;
        }
        if (gvpOk &&
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

            DWORD rcvErr = 0;
            bool rcvOk = DeviceIoControl(hDevice, SMART_RCV_DRIVE_DATA,
                                         sendBuf, sizeof(sendBuf),
                                         outBuf, sizeof(outBuf),
                                         &bytesReturned, nullptr);
            if (!rcvOk) rcvErr = GetLastError();
            Logger::Info("SMART disk" + std::to_string(diskIndex) +
                " RCV: ok=" + std::to_string(rcvOk) +
                " sz=" + std::to_string(bytesReturned) +
                " err=" + std::to_string(rcvErr));

            // Driver may return 528 bytes (16-byte header + 512 data) without padding
            if (rcvOk &&
                bytesReturned >= offsetof(SENDCMDOUTPARAMS, bBuffer) + 512) {

                auto* scop = reinterpret_cast<SENDCMDOUTPARAMS*>(outBuf);
                const BYTE* raw = scop->bBuffer;  // 512 bytes of SMART attributes

                success = true;

                // Parse all SMART attributes in a single pass
                bool isSSD = false;
                double tempRead = -1;
                int health = 100;
                int attrIdx = 0;

                for (int attrOff = 2; attrOff < 512 - 12 && attrIdx < 32; attrOff += 12) {
                    if (raw[attrOff] == 0 || raw[attrOff] == 0xFF) break;
                    int attrId = raw[attrOff];
                    uint64_t rawVal = 0;
                    for (int j = 0; j < 6; j++)
                        rawVal |= (static_cast<uint64_t>(raw[attrOff + 5 + j]) << (8 * j));

                    // Populate attribute entry
                    auto& attr = smartData.attributes[attrIdx];
                    attr.id = static_cast<uint8_t>(attrId);
                    attr.flags = static_cast<uint8_t>(raw[attrOff + 1]);
                    attr.current = raw[attrOff + 3];
                    attr.worst = raw[attrOff + 4];
                    attr.threshold = raw[attrOff + 0];  // threshold not in raw data, use 0
                    attr.rawValue = rawVal;
                    attr.threshold = 0;  // threshold not directly available in standard SMART read
                    attrIdx++;

                    // Temperature (190=Airflow, 194=Temperature)
                    if (attrId == 0xBE || attrId == 0xC2) {
                        for (int bo = 5; bo <= 10; bo++) {
                            int t = raw[attrOff + bo];
                            if (t >= 15 && t <= 120) { tempRead = (double)t; break; }
                        }
                        if (tempRead < 0) {
                            int nv = raw[attrOff + 3];
                            if (nv >= 15 && nv <= 120) tempRead = (double)nv;
                        }
                        if (tempRead < 0) {
                            int hi = raw[attrOff + 5] | (raw[attrOff + 6] << 8);
                            if (hi >= 15 && hi <= 120) tempRead = (double)hi;
                        }
                    }

                    // Health-critical attributes: 5, 196, 197, 198
                    if (attrId == 5 || attrId == 196 || attrId == 197 || attrId == 198) {
                        if (rawVal > 0) {
                            if (attrId == 5 && rawVal > 0) health = (std::min)(health, 95);
                            if (attrId == 5 && rawVal > 10) health = (std::min)(health, 80);
                            if (attrId == 197 && rawVal > 0) health = (std::min)(health, 90);
                            if (attrId == 198 && rawVal > 0) health = (std::min)(health, 85);
                            if (attrId == 5) smartData.reallocatedSectorCount = rawVal;
                            if (attrId == 197) smartData.currentPendingSector = rawVal;
                            if (attrId == 198) smartData.uncorrectableErrors = rawVal;
                        }
                    }

                    // Power-on hours (ID 9)
                    if (attrId == 9 && rawVal < 100000000)
                        smartData.powerOnHours = rawVal;

                    // Wear leveling (SSD) — ID 177, 230, 233
                    if (attrId == 177 || attrId == 230 || attrId == 233) {
                        int cur = raw[attrOff + 3];
                        if (cur > 0 && cur <= 100) {
                            smartData.wearLeveling = 1.0 - (cur / 100.0);
                            isSSD = true;
                        }
                    }
                }

                smartData.attributeCount = attrIdx;
                if (tempRead > 0) smartData.temperature = tempRead;
                smartData.healthPercentage = static_cast<uint8_t>(health);
                wcsncpy_s(smartData.diskType, isSSD ? L"SSD" : L"HDD", _TRUNCATE);
            }
        }

        CloseHandle(hDevice);
    }

    if (!success) {
        // NVMe path: IOCTL_STORAGE_QUERY_PROPERTY + StorageDeviceProtocolSpecificProperty
        // Query NVME_LOG_PAGE_HEALTH_INFO (log page 0x02) — does NOT require admin rights
        Logger::Info("SMART disk" + std::to_string(diskIndex) + " trying NVMe path...");
        HANDLE hNvme = CreateFileW(path, FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
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
            protocolData->ProtocolDataLength = 512;  // NVMe health log is 512 bytes

            DWORD nvmeBytesReturned = 0;
            if (DeviceIoControl(hNvme, IOCTL_STORAGE_QUERY_PROPERTY,
                                nvmeBuf, sizeof(STORAGE_PROPERTY_QUERY) + sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA),
                                nvmeBuf, sizeof(nvmeBuf),
                                &nvmeBytesReturned, nullptr)) {

                auto* desc = reinterpret_cast<STORAGE_PROTOCOL_DATA_DESCRIPTOR*>(nvmeBuf);
                auto* retProtocolData = &desc->ProtocolSpecificData;
                const BYTE* raw = reinterpret_cast<const BYTE*>(retProtocolData)
                                + retProtocolData->ProtocolDataOffset;

                smartData.smartSupported = true;
                smartData.smartEnabled = true;
                success = true;

                // NVMe drives are always SSDs
                wcsncpy_s(smartData.diskType, L"SSD", _TRUNCATE);

                // NVMe SMART / Health Information Log layout (NVM Express 1.4 §5.14.1.2):
                // Offset 0:   CriticalWarning (1 byte)
                // Offset 1:   Temperature[2] (composite temp, uint16 LE, Kelvin)
                // Offset 3:   AvailableSpare
                // Offset 4:   AvailableSpareThreshold
                // Offset 5:   PercentageUsed
                // Offset 32:  PowerOnHours[16] (first 8 bytes LE)

                // Temperature: composite temp (2 bytes LE, Kelvin) → Celsius
                USHORT tempKelvin = raw[1] | (raw[2] << 8);
                if (tempKelvin > 273) {
                    int tempC = static_cast<int>(tempKelvin) - 273;
                    if (tempC < 128) smartData.temperature = static_cast<double>(tempC);
                }

                // Health percentage (100 - PercentageUsed)
                uint8_t pctUsed = raw[5];
                if (pctUsed <= 100) {
                    smartData.healthPercentage = static_cast<uint8_t>(100 - pctUsed);
                    smartData.wearLeveling = pctUsed / 100.0;
                }

                // Power-on hours (offset 32, first 8 bytes LE)
                uint64_t hours = 0;
                memcpy(&hours, raw + 32, sizeof(uint64_t));
                if (hours > 0 && hours < 100000000)
                    smartData.powerOnHours = hours;

                // AvailableSpare below threshold indicates degraded health
                if (raw[3] < raw[4])
                    smartData.healthPercentage = (std::min)(smartData.healthPercentage, uint8_t(50));

                // Critical warning set indicates serious drive issue
                if (raw[0] > 0)
                    smartData.healthPercentage = (std::min)(smartData.healthPercentage, uint8_t(30));
            }
            CloseHandle(hNvme);
        }
    }

    if (!success) {
        // Fallback: try StorageDeviceTemperatureProperty (Windows 10+)
        success = false;
        HANDLE hDev2 = CreateFileW(path, FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
        if (hDev2 != INVALID_HANDLE_VALUE) {
            // STORAGE_TEMPERATURE_DATA_DESCRIPTOR layout:
            //   Version(4) + Size(4) + TemperatureInfo[0..N]
            // Each TemperatureInfo: Identifier(4) + Temperature(4, signed, tenths °C)
            BYTE tempBuf[64] = {};
            STORAGE_PROPERTY_QUERY query{};
            query.PropertyId = StorageDeviceTemperatureProperty;
            query.QueryType = PropertyStandardQuery;
            if (DeviceIoControl(hDev2, IOCTL_STORAGE_QUERY_PROPERTY,
                                &query, sizeof(query),
                                tempBuf, sizeof(tempBuf),
                                &bytesReturned, nullptr) &&
                bytesReturned >= 16) {
                // Temperature at offset 12 (after Version+Size+Identifier = 4+4+4)
                LONG tempTenthsC = *(LONG*)(tempBuf + 12);
                double tempC = static_cast<double>(tempTenthsC) / 10.0;
                if (tempC > 0.0 && tempC < 128.0) {
                    smartData.temperature = tempC;
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
