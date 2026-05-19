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
// SMART attribute ID to human-readable name (Chinese)
// ====================================================================
static const wchar_t* GetSmartAttrName(uint8_t id) {
    switch (id) {
        case 1:   return L"读取错误率";
        case 3:   return L"硬盘启动时间";
        case 4:   return L"启停次数";
        case 5:   return L"重映射扇区计数";
        case 7:   return L"寻道错误率";
        case 9:   return L"通电时间";
        case 10:  return L"启动重试次数";
        case 12:  return L"通电次数";
        case 169: return L"剩余寿命";
        case 170: return L"可用预留空间";
        case 171: return L"SSD 编程失败计数";
        case 172: return L"SSD 擦除失败计数";
        case 173: return L"SSD 磨损均衡计数";
        case 174: return L"意外断电计数";
        case 175: return L"通电时间";
        case 176: return L"擦除失败计数";
        case 177: return L"磨损均衡计数";
        case 179: return L"已用预留块计数";
        case 180: return L"编程失败计数";
        case 181: return L"编程失败计数";
        case 182: return L"擦除失败计数";
        case 183: return L"SATA 下行计数";
        case 184: return L"端到端错误";
        case 187: return L"报告不可校正错误";
        case 188: return L"命令超时";
        case 189: return L"工厂坏块";
        case 190: return L"气流温度";
        case 191: return L"G-Sense 错误率";
        case 192: return L"断电撤回计数";
        case 193: return L"加载卸载计数";
        case 194: return L"温度";
        case 195: return L"硬件 ECC 恢复";
        case 196: return L"重新分配事件";
        case 197: return L"当前待处理扇区";
        case 198: return L"脱机不可校正";
        case 199: return L"CRC 错误计数";
        case 200: return L"写入错误率";
        case 201: return L"软读取错误率";
        case 202: return L"数据地址标记错误";
        case 203: return L"耗尽取消";
        case 204: return L"软 ECC 修正";
        case 205: return L"热糙度";
        case 206: return L"磁头高度";
        case 207: return L"硬盘旋转震动";
        case 208: return L"硬盘写入震动";
        case 210: return L"RAID 恢复";
        case 220: return L"磁盘偏移";
        case 222: return L"已加载小时";
        case 223: return L"加载卸载重试";
        case 224: return L"负载摩擦";
        case 225: return L"负载卸载计数";
        case 226: return L"负载时间";
        case 227: return L"扭矩放大计数";
        case 228: return L"断电撤回计数";
        case 230: return L"SSD 寿命";
        case 231: return L"SSD 剩余寿命";
        case 232: return L"可用预留空间";
        case 233: return L"NAND 写入总量";
        case 234: return L"平均擦除次数";
        case 235: return L"最大擦除次数";
        case 240: return L"磁头飞行时间";
        case 241: return L"主机总计写入";
        case 242: return L"主机总计读取";
        case 245: return L"最大擦除次数";
        case 246: return L"总计擦除次数";
        case 247: return L"主机读取量";
        case 248: return L"主机写入量";
        case 249: return L"NAND 读取量";
        case 250: return L"读取错误重试率";
        case 251: return L"剩余备用容量";
        default:  return L"";
    }
}

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
            scip->irDriveRegs.bCylLowReg = 0x4F;  // SMART signature "SM"
            scip->irDriveRegs.bCylHighReg = 0xC2; // SMART signature "SM"
            scip->irDriveRegs.bCommandReg = 0xB0; // ATA SMART command
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

                // Parse SMART attributes — standard 12-byte grid starting at
                // offset 2 (after 2-byte version header). The ATA SMART READ
                // DATA command (0xB0) returns properly formatted data.
                double tempRead = -1;
                bool isSSD = false;
                bool hasRotationAttr = false;
                int health = 100;
                int attrIdx = 0;

                for (int attrOff = 2; attrOff < 512 - 12 && attrIdx < 32; attrOff += 12) {
                    int id = raw[attrOff];
                    if (id == 0 || id == 0xFF) continue;
                    int fl = raw[attrOff + 1];
                    int cur = raw[attrOff + 3];
                    int worst = raw[attrOff + 4];

                    uint64_t rawVal = 0;
                    for (int j = 0; j < 6; j++)
                        rawVal |= (static_cast<uint64_t>(raw[attrOff + 5 + j]) << (8 * j));

                    auto& attr = smartData.attributes[attrIdx];
                    attr.id = static_cast<uint8_t>(id);
                    attr.flags = static_cast<uint8_t>(fl);
                    attr.current = static_cast<uint8_t>(cur);
                    attr.worst = static_cast<uint8_t>(worst);
                    attr.rawValue = rawVal;
                    wcsncpy_s(attr.name, GetSmartAttrName(attr.id), _TRUNCATE);
                    attrIdx++;

                    // Temperature: 190(=BE)=Airflow, 194(=C2)=Temperature, 231(=E7)=SSD Temp
                    if (id == 0xBE || id == 0xC2 || id == 0xE7) {
                        for (int bo = 5; bo <= 10; bo++) {
                            int t = raw[attrOff + bo];
                            if (t >= 15 && t <= 120) { tempRead = (double)t; break; }
                        }
                        if (tempRead < 0) {
                            int nv = cur;
                            if (nv >= 15 && nv <= 120) tempRead = (double)nv;
                        }
                    }

                    // Health-critical: 5, 196, 197, 198
                    if (id == 5 || id == 196 || id == 197 || id == 198) {
                        if (rawVal > 0) {
                            if (id == 5 && rawVal > 0) health = (std::min)(health, 95);
                            if (id == 5 && rawVal > 10) health = (std::min)(health, 80);
                            if (id == 197 && rawVal > 0) health = (std::min)(health, 90);
                            if (id == 198 && rawVal > 0) health = (std::min)(health, 85);
                            if (id == 5) smartData.reallocatedSectorCount = rawVal;
                            if (id == 197) smartData.currentPendingSector = rawVal;
                            if (id == 198) smartData.uncorrectableErrors = rawVal;
                        }
                    }

                    // Power-on hours (ID 9)
                    if (id == 9 && rawVal < 100000000)
                        smartData.powerOnHours = rawVal;

                    // HDD-specific: spin-up time (3), start-stop count (4)
                    if (id == 3 || id == 4) hasRotationAttr = true;

                    // Wear leveling / SSD remaining life
                    if (id == 169 || id == 177 || id == 230 ||
                        id == 231 || id == 232 || id == 233 || id == 0xE7) {
                        if (cur > 0 && cur <= 100) {
                            smartData.wearLeveling = 1.0 - (cur / 100.0);
                            isSSD = true;
                            health = (std::min)(health, cur);
                        }
                    }
                }

                smartData.attributeCount = attrIdx;
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
                auto addAttr = [&](uint8_t id, uint8_t cur, uint64_t rawVal, const wchar_t* name) {
                    if (smartData.attributeCount >= 32) return;
                    auto& a = smartData.attributes[smartData.attributeCount++];
                    a.id = id; a.current = cur; a.rawValue = rawVal; a.worst = 0;
                    wcsncpy_s(a.name, name, _TRUNCATE);
                };
                addAttr(200, raw[0], raw[0], L"严重警告标志");                          // CriticalWarning
                addAttr(201, (uint8_t)(raw[1] | (raw[2] << 8)) - 273,                 // Temperature °C
                            raw[1] | (raw[2] << 8), L"温度 (NVMe)");
                addAttr(202, raw[3], raw[3], L"可用备用空间");                           // AvailableSpare %
                addAttr(203, raw[4], raw[4], L"可用备用阈值");                           // AvailableSpareThresh
                addAttr(204, raw[5], raw[5], L"已用寿命百分比");                         // PercentageUsed
                uint64_t nvmeHours = 0;
                memcpy(&nvmeHours, raw + 32, sizeof(uint64_t));
                addAttr(209, 0, nvmeHours, L"通电时间 (NVMe)");                        // PowerOnHours

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
