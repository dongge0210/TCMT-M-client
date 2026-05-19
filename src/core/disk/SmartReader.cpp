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
            size_t minSz = offsetof(SENDCMDOUTPARAMS, bBuffer) + 512;
            Logger::Info("SMART disk" + std::to_string(diskIndex) +
                " RCV: ok=" + std::to_string(rcvOk) +
                " sz=" + std::to_string(bytesReturned) +
                " minSz=" + std::to_string(minSz) +
                " bBufOff=" + std::to_string(offsetof(SENDCMDOUTPARAMS, bBuffer)) +
                " sizeofSCOP=" + std::to_string(sizeof(SENDCMDOUTPARAMS)) +
                " err=" + std::to_string(rcvErr));

            // Driver may return 528 bytes (16-byte header + 512 data) without padding
            if (rcvOk &&
                bytesReturned >= offsetof(SENDCMDOUTPARAMS, bBuffer) + 512) {

                auto* scop = reinterpret_cast<SENDCMDOUTPARAMS*>(outBuf);
                const BYTE* raw = scop->bBuffer;  // 512 bytes of SMART attributes

                success = true;

                // Debug: log first 8 bytes of raw SMART data
                static int rawDumpCount = 0;
                if (rawDumpCount < 3) {
                    char hex[128];
                    snprintf(hex, sizeof(hex),
                        "SMART raw[0..7]: %02X %02X %02X %02X %02X %02X %02X %02X",
                        raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6], raw[7]);
                    Logger::Info(hex);
                    rawDumpCount++;
                }

                // Parse SMART attributes. Standard layout has a 2-byte version
                // header (offset 0-1) with attributes starting at offset 2.
                // Some SSDs have non-standard offset, so try both 0 and 2 and
                // pick the alignment that yields more recognized IDs.
                //
                // Recognized SMART attr IDs (upper nibble = common group):
                // 0x0x: 1-9 read-error, throughput, spin-up, start-stop,
                //        reallocated, seek-error, power-on, spin-retry
                // 0xAx: 10-12 recalib, CRC, power-cycle
                // 0xBx: 170-199 endurance, wear-level, temp, realloc, pending
                // 0xEx: 220-255 write/read totals, media-wear, NAND writes
                auto isKnownAttrId = [](int id) -> bool {
                    if (id < 1 || id > 254) return false;
                    int hi = id >> 4;
                    return hi == 0x0 || hi == 0xA || hi == 0xB || hi == 0xC || hi == 0xE || hi == 0xF;
                };

                int bestScore = 0, bestOff = 2;
                for (int tryOff : {2, 0}) {
                    int score = 0, n = 0;
                    for (int off = tryOff; off < 512 - 12; off += 12) {
                        int id = raw[off];
                        if (id == 0 || id == 0xFF) continue;
                        if (isKnownAttrId(id)) score++;
                        if (++n >= 32) break;
                    }
                    if (score > bestScore) { bestScore = score; bestOff = tryOff; }
                }

                bool isSSD = false;
                bool hasRotationAttr = false;
                double tempRead = -1;
                int health = 100;
                int attrIdx = 0;

                for (int attrOff = bestOff; attrOff < 512 - 12 && attrIdx < 32; attrOff += 12) {
                    int attrId = raw[attrOff];
                    if (attrId == 0 || attrId == 0xFF) continue;
                    uint64_t rawVal = 0;
                    for (int j = 0; j < 6; j++)
                        rawVal |= (static_cast<uint64_t>(raw[attrOff + 5 + j]) << (8 * j));

                    auto& attr = smartData.attributes[attrIdx];
                    attr.id = static_cast<uint8_t>(attrId);
                    attr.flags = static_cast<uint8_t>(raw[attrOff + 1]);
                    attr.current = raw[attrOff + 3];
                    attr.worst = raw[attrOff + 4];
                    attr.rawValue = rawVal;
                    attrIdx++;

                    // Temperature: 194=C2, 190=BE=Airflow, 231=E7=SSD Temp
                    if (attrId == 0xBE || attrId == 0xC2 || attrId == 0xE7) {
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
                        if (tempRead < 0) {
                            int inv = 100 - raw[attrOff + 5];
                            if (inv >= 15 && inv <= 120) tempRead = (double)inv;
                        }
                    }

                    // Health-critical: 5, 196, 197, 198
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

                    // HDD-specific: spin-up time (3), start-stop count (4)
                    if (attrId == 3 || attrId == 4) hasRotationAttr = true;

                    // Wear leveling (SSD): 169, 177, 230, 231, 232, 233
                    if (attrId == 169 || attrId == 177 || attrId == 230 ||
                        attrId == 231 || attrId == 232 || attrId == 233) {
                        int cur = raw[attrOff + 3];
                        if (cur > 0 && cur <= 100) {
                            smartData.wearLeveling = 1.0 - (cur / 100.0);
                            isSSD = true;
                        }
                    }
                }

                smartData.attributeCount = attrIdx;

                {
                    static int attrLogCount = 0;
                    if (attrLogCount < 5) {
                        std::string ids;
                        for (int i = 0; i < attrIdx && i < 32; i++) {
                            if (i > 0) ids += ",";
                            ids += std::to_string(smartData.attributes[i].id);
                            ids += ":";
                            ids += std::to_string(smartData.attributes[i].current);
                        }
                        Logger::Info("SMART disk" + std::to_string(diskIndex) +
                            " ATA off=" + std::to_string(bestOff) +
                            " attrs[" + std::to_string(attrIdx) + "]: " + ids);
                        attrLogCount++;
                    }
                }

                // Secondary temperature pass on stored attributes
                if (tempRead <= 0) {
                    for (int i = 0; i < attrIdx; i++) {
                        const auto& a = smartData.attributes[i];
                        if (a.id == 0xBE || a.id == 0xC2 || a.id == 0xE7) {
                            if (a.current >= 15 && a.current <= 120)
                                { tempRead = (double)a.current; break; }
                        }
                    }
                }
                if (tempRead > 0) smartData.temperature = tempRead;
                smartData.healthPercentage = static_cast<uint8_t>(health);
                if (!isSSD && !hasRotationAttr) isSSD = true;
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
            // Input: STORAGE_PROPERTY_QUERY header + STORAGE_PROTOCOL_SPECIFIC_DATA
            // Use offsetof to avoid padding / flexible-array issues
            const DWORD inSize = offsetof(STORAGE_PROPERTY_QUERY, AdditionalParameters)
                               + sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA);
            // Output: STORAGE_PROTOCOL_DATA_DESCRIPTOR + health log data
            const DWORD outSize = sizeof(STORAGE_PROTOCOL_DATA_DESCRIPTOR) + 512;
            BYTE inBuf[256] = {};
            BYTE outBuf[1024] = {};

            auto* query = reinterpret_cast<STORAGE_PROPERTY_QUERY*>(inBuf);
            query->PropertyId = StorageDeviceProtocolSpecificProperty;
            query->QueryType = PropertyStandardQuery;

            auto* protocolData = reinterpret_cast<STORAGE_PROTOCOL_SPECIFIC_DATA*>(
                inBuf + offsetof(STORAGE_PROPERTY_QUERY, AdditionalParameters));
            protocolData->ProtocolType = ProtocolTypeNvme;
            protocolData->DataType = NVMeDataTypeLogPage;
            protocolData->ProtocolDataRequestValue = NVME_LOG_PAGE_HEALTH_INFO;
            protocolData->ProtocolDataRequestSubValue = 0;
            protocolData->ProtocolDataOffset = sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA);
            protocolData->ProtocolDataLength = 512;  // NVMe health log is 512 bytes

            DWORD nvmeBytesReturned = 0;
            Logger::Info("SMART disk" + std::to_string(diskIndex) +
                " NVMe: inSize=" + std::to_string(inSize) + " outSize=" + std::to_string(outSize));

            if (DeviceIoControl(hNvme, IOCTL_STORAGE_QUERY_PROPERTY,
                                inBuf, inSize,
                                outBuf, outSize,
                                &nvmeBytesReturned, nullptr)) {

                auto* desc = reinterpret_cast<STORAGE_PROTOCOL_DATA_DESCRIPTOR*>(outBuf);
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

                // Populate NVMe health data as pseudo SMART attributes for display
                smartData.attributeCount = 0;
                auto addAttr = [&](uint8_t id, uint8_t cur, uint64_t rawVal) {
                    if (smartData.attributeCount >= 32) return;
                    auto& a = smartData.attributes[smartData.attributeCount++];
                    a.id = id; a.current = cur; a.rawValue = rawVal; a.worst = 0;
                };
                addAttr(200, raw[0], raw[0]);                                    // CriticalWarning
                addAttr(201, (uint8_t)(raw[1] | (raw[2] << 8)) - 273,           // Temperature °C
                            raw[1] | (raw[2] << 8));
                addAttr(202, raw[3], raw[3]);                                    // AvailableSpare %
                addAttr(203, raw[4], raw[4]);                                    // AvailableSpareThresh
                addAttr(204, raw[5], raw[5]);                                    // PercentageUsed
                uint64_t nvmeHours = 0;
                memcpy(&nvmeHours, raw + 32, sizeof(uint64_t));
                addAttr(209, 0, nvmeHours);                                      // PowerOnHours

                Logger::Info("SMART disk" + std::to_string(diskIndex) + " NVMe ok: tempC=" +
                    std::to_string((int)(raw[1] | (raw[2] << 8)) - 273) +
                    " used=" + std::to_string((int)raw[5]) + "%");
            } else {
                DWORD nvmeErr = GetLastError();
                Logger::Info("SMART disk" + std::to_string(diskIndex) + " NVMe IOCTL fail err=" +
                    std::to_string(nvmeErr));
            }
            CloseHandle(hNvme);
        } else {
            DWORD nvmeOpenErr = GetLastError();
            Logger::Info("SMART disk" + std::to_string(diskIndex) + " NVMe open fail err=" +
                std::to_string(nvmeOpenErr));
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
