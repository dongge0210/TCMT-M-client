// SmartReader.cpp — DeviceIoControl SMART reader for Windows physical disks

#include "SmartReader.h"
#include "NVMe_HealthLog.h"
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
// SMART attribute ID to human-readable name & description (Chinese)
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
        case 148: return L"预留";
        case 149: return L"预留";
        case 167: return L"固态硬盘保护模式";
        case 168: return L"SATA 物理层错误计数";
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
        case 218: return L"CRC 错误计数";
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
        case 244: return L"平均擦除次数";
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
static const wchar_t* GetSmartAttrDesc(uint8_t id) {
    switch (id) {
        case 1:   return L"底层数据读取错误率，反映磁盘表面或磁头读取可靠性";
        case 3:   return L"磁盘电机从静止到达工作转速所需时间";
        case 4:   return L"硬盘磁头启停/加载卸载的累计次数";
        case 5:   return L"已被映射到备用扇区的坏扇区数量，>0 说明磁盘开始老化";
        case 7:   return L"磁头定位到目标磁道的错误率";
        case 9:   return L"磁盘累计通电运行小时数";
        case 12:  return L"磁盘累计通电/断电循环次数";
        case 169: return L"SSD 剩余可擦写寿命百分比，100=全新，0=耗尽";
        case 170: return L"SSD 可用于替换坏块的预留空间百分比";
        case 171: return L"SSD 编程(写入)操作失败的总次数";
        case 172: return L"SSD 擦除操作失败的总次数";
        case 173: return L"SSD 各块的擦写次数均衡程度，数值越低越不均匀";
        case 174: return L"非正常断电导致磁盘停止工作的次数";
        case 177: return L"SSD 擦写磨损程度，100=全新，数值越低磨损越严重";
        case 187: return L"已检测到但无法通过 ECC 纠正的数据错误数";
        case 188: return L"磁盘命令执行超时的累计次数";
        case 190: return L"硬盘内部气流温度（摄氏度）";
        case 192: return L"异常断电后磁头紧急缩回/撤离的次数";
        case 193: return L"磁头加载/卸载到停泊区的累计次数";
        case 194: return L"磁盘当前工作温度（摄氏度），过高会缩短寿命";
        case 196: return L"已被重新分配到备用扇区的扇区事件计数，>0 需关注";
        case 197: return L"当前已检测到不稳定但尚未重新分配的待处理扇区数";
        case 198: return L"无法通过 ECC 和重试修复的永久性坏扇区数";
        case 199: return L"SATA 数据线传输 CRC 校验错误，常由数据线/接口接触不良引起";
        case 230: return L"SSD 剩余寿命百分比，类似 169/177";
        case 231: return L"SSD 剩余寿命百分比，100=全新，0=耗尽";
        case 232: return L"SSD 可用于替换坏块的空间余量百分比";
        case 233: return L"SSD NAND 闪存累计写入数据量";
        case 234: return L"SSD 所有块的平均擦写次数";
        case 235: return L"SSD 单个块的最高擦写次数";
        case 241: return L"主机通过接口向磁盘累计写入的数据总量";
        case 242: return L"主机通过接口从磁盘累计读取的数据总量";
        case 246: return L"SSD 自出厂以来所有块的累计擦写总次数";
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
        bool gvpOk = DeviceIoControl(hDevice, SMART_GET_VERSION,
                                     nullptr, 0,
                                     &gvp, sizeof(gvp),
                                     &bytesReturned, nullptr);
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

            bool rcvOk = DeviceIoControl(hDevice, SMART_RCV_DRIVE_DATA,
                                         sendBuf, sizeof(sendBuf),
                                         outBuf, sizeof(outBuf),
                                         &bytesReturned, nullptr);

            // Driver may return 528 bytes (16-byte header + 512 data) without padding
            if (rcvOk &&
                bytesReturned >= offsetof(SENDCMDOUTPARAMS, bBuffer) + 512) {

                auto* scop = reinterpret_cast<SENDCMDOUTPARAMS*>(outBuf);
                const BYTE* raw = scop->bBuffer;  // 512 bytes of SMART attributes

                success = true;

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
                    wcsncpy_s(attr.description, GetSmartAttrDesc(attr.id), _TRUNCATE);
                    attrIdx++;

                    // Temperature: 190(=BE)=Airflow, 194(=C2)=Temperature
                    if (id == 0xBE || id == 0xC2) {
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

                    // Power-on hours (ID 9) — tight limit catches ATA-translation garbage
                    if (id == 9 && rawVal < 300000)
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

                // NVMe SMART / Health Information Log layout (Log Page 02h):
                // Offset 0:   CriticalWarning (1 byte)
                // Offset 1:   Temperature[2] (composite temp, uint16 LE, Kelvin)
                // Offset 3:   AvailableSpare
                // Offset 4:   AvailableSpareThreshold
                // Offset 5:   PercentageUsed
                // Offset 128: PowerOnHours[16] (low 8 bytes LE)

                // All counters are 128-bit (16-byte) LE values; the low 64
                // bits hold the count. See NVMe_HealthLog.h for all offsets.

                // Temperature: composite temp (2 bytes LE, Kelvin) → Celsius
                USHORT tempKelvin = raw[1] | (raw[2] << 8);
                if (tempKelvin > 273) {
                    int tempC = static_cast<int>(tempKelvin) - 273;
                    if (tempC <= 120) smartData.temperature = static_cast<double>(tempC);
                }

                // Health percentage (100 - PercentageUsed)
                uint8_t pctUsed = raw[5];
                if (pctUsed <= 100) {
                    smartData.healthPercentage = static_cast<uint8_t>(100 - pctUsed);
                    smartData.wearLeveling = pctUsed / 100.0;
                }

                // Power-on hours (standard offset 128, low 64 bits LE;
                // confirmed with DiskGenius: 0x37E3 = 14307 hours)
                uint64_t hours = 0;
                memcpy(&hours, raw + NVMeHealthLog::kPowerOnHoursOffset, sizeof(uint64_t));
                if (hours > 0 && hours < 300000)
                    smartData.powerOnHours = hours;

                // AvailableSpare below threshold indicates degraded health
                if (raw[3] < raw[4])
                    smartData.healthPercentage = (std::min)(smartData.healthPercentage, uint8_t(50));

                // Critical warning set indicates serious drive issue
                if (raw[0] > 0)
                    smartData.healthPercentage = (std::min)(smartData.healthPercentage, uint8_t(30));

                // Populate NVMe health data as pseudo SMART attributes for display
                smartData.attributeCount = 0;
                auto addAttr = [&](uint8_t id, uint8_t cur, uint64_t rawVal, const wchar_t* name, const wchar_t* desc) {
                    if (smartData.attributeCount >= 32) return;
                    auto& a = smartData.attributes[smartData.attributeCount++];
                    a.id = id; a.current = cur; a.rawValue = rawVal; a.worst = 0;
                    wcsncpy_s(a.name, name, _TRUNCATE);
                    wcsncpy_s(a.description, desc, _TRUNCATE);
                };
                addAttr(0x01, raw[0], raw[0], L"Critical Warning",
                    L"NVMe critical warning flags: 0=OK, non-zero indicates serious issue");
                addAttr(0x02, (uint8_t)(raw[1] | (raw[2] << 8)) - 273,
                            raw[1] | (raw[2] << 8), L"Temperature",
                    L"NVMe composite temperature sensor (Celsius)");
                addAttr(0x03, raw[3], raw[3], L"Available Spare",
                    L"Remaining spare space for bad block replacement");
                addAttr(0x04, raw[4], raw[4], L"Available Spare Threshold",
                    L"Spare threshold that triggers warning when breached");
                addAttr(0x05, raw[5], raw[5], L"Percentage Used",
                    L"Percentage of rated write endurance consumed (100=exhausted)");
                uint64_t v6=0,v7=0,v8=0,v9=0,v10=0,v11=0,v12=0,v13=0,v14=0,v15=0;
                memcpy(&v6,  raw + NVMeHealthLog::kDataUnitsReadOffset, 8);
                memcpy(&v7,  raw + NVMeHealthLog::kDataUnitsWrittenOffset, 8);
                memcpy(&v8,  raw + NVMeHealthLog::kHostReadCommandsOffset, 8);
                memcpy(&v9,  raw + NVMeHealthLog::kHostWriteCommandsOffset, 8);
                memcpy(&v10, raw + NVMeHealthLog::kControllerBusyTimeOffset, 8);
                memcpy(&v11, raw + NVMeHealthLog::kPowerCyclesOffset, 8);
                memcpy(&v12, raw + NVMeHealthLog::kPowerOnHoursOffset, 8);
                memcpy(&v13, raw + NVMeHealthLog::kUnsafeShutdownsOffset, 8);
                memcpy(&v14, raw + NVMeHealthLog::kMediaDataIntegrityErrorsOffset, 8);
                memcpy(&v15, raw + NVMeHealthLog::kErrorLogEntriesOffset, 8);
                addAttr(0x06, 0, v6,  L"Data Units Read", L"Total data read from NVMe drive (units of 512 bytes)");
                addAttr(0x07, 0, v7,  L"Data Units Written", L"Total data written to NVMe drive (units of 512 bytes)");
                addAttr(0x08, 0, v8,  L"Host Read Commands", L"Total read commands issued by host");
                addAttr(0x09, 0, v9,  L"Host Write Commands", L"Total write commands issued by host");
                addAttr(0x0A, 0, v10, L"Controller Busy Time", L"Total time controller was busy processing commands (minutes)");
                addAttr(0x0B, 0, v11, L"Power Cycles", L"Total power on/off cycles");
                addAttr(0x0C, 0, v12, L"Power On Hours", L"Total power-on hours");
                addAttr(0x0D, 0, v13, L"Unsafe Shutdowns", L"Total unsafe/abnormal power-down events");
                addAttr(0x0E, 0, v14, L"Media & Integrity Errors", L"Total media errors and data integrity errors");
                addAttr(0x0F, 0, v15, L"Error Log Entries", L"Number of entries in the NVMe error information log");
            } else {
                Logger::Debug("SMART disk" + std::to_string(diskIndex) + " NVMe IOCTL fail err=" +
                    std::to_string(GetLastError()));
            }
            CloseHandle(hNvme);
        } else {
            Logger::Debug("SMART disk" + std::to_string(diskIndex) + " NVMe open fail err=" +
                std::to_string(GetLastError()));
        }
    }

    if (!success) {
        // Fallback: try StorageDeviceTemperatureProperty (Windows 10+)
        HANDLE hDev2 = CreateFileW(path, FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    nullptr, OPEN_EXISTING, 0, nullptr);
        if (hDev2 != INVALID_HANDLE_VALUE) {
            STORAGE_PROPERTY_QUERY query{};
            query.PropertyId = StorageDeviceTemperatureProperty;
            query.QueryType = PropertyStandardQuery;
            BYTE tempBuf[256] = {};
            DWORD tempBytes = 0;
            if (DeviceIoControl(hDev2, IOCTL_STORAGE_QUERY_PROPERTY,
                                &query, sizeof(query),
                                tempBuf, sizeof(tempBuf),
                                &tempBytes, nullptr) && tempBytes >= 12) {
                // Temperature at offset 12 (after Version+Size+Identifier = 4+4+4)
                int t = *reinterpret_cast<int*>(tempBuf + 12);
                if (t != 0) smartData.temperature = static_cast<double>(t) / 10.0;
            }
            CloseHandle(hDev2);
        }
    }

    return success;
}

#elif defined(TCMT_MACOS)

extern "C" bool SmartReaderMacRead(int diskIndex, PhysicalDiskSmartData& smartData);
bool SmartReader::Read(int diskIndex, PhysicalDiskSmartData& smartData) {
    return SmartReaderMacRead(diskIndex, smartData);
}

#elif defined(TCMT_LINUX)
// ======================== Linux Implementation ========================
//
// NVMe:  NVME_IOCTL_ADMIN_CMD with Get Log Page (LID 0x02, SMART/Health).
// SATA:  SG_IO with ATA PASS-THROUGH (12), SMART READ DATA (0xD0).
//
// Both ioctls require root or CAP_SYS_RAWIO/CAP_SYS_ADMIN. When they fail the
// function returns false and leaves smartSupported=false; DiskInfo keeps the
// disk in the list and reports health 0. Failures are logged once per family.

#include "DiskInfo.h"
#include "../Utils/Sysfs.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/nvme_ioctl.h>
#include <scsi/sg.h>
#include <string>
#include <sys/ioctl.h>
#include <unistd.h>
#include <vector>

namespace {

void SetAscii(WCHAR* destination, size_t count, const char* text) {
    if (!destination || count == 0) return;
    size_t i = 0;
    for (; text && text[i] && i + 1 < count; ++i) {
        destination[i] = static_cast<WCHAR>(static_cast<unsigned char>(text[i]));
    }
    destination[i] = 0;
}

uint64_t Le128Low64(const unsigned char* bytes) {
    uint64_t value = 0;
    for (int i = 7; i >= 0; --i) value = (value << 8) | bytes[i];
    return value;
}

void AddAttribute(PhysicalDiskSmartData& disk, uint8_t id, uint8_t current,
                  uint8_t worst, uint64_t raw, const char* name, bool critical) {
    if (disk.attributeCount >= 32) return;
    SmartAttributeData& attribute = disk.attributes[disk.attributeCount++];
    attribute.id = id;
    attribute.flags = 0;
    attribute.current = current;
    attribute.worst = worst;
    attribute.threshold = 0;
    attribute.rawValue = raw;
    attribute.isCritical = critical;
    attribute.physicalValue = 0.0;
    SetAscii(attribute.name, sizeof(attribute.name) / sizeof(WCHAR), name);
    attribute.description[0] = 0;
    attribute.units[0] = 0;
}

bool ReadNvmeHealth(const std::string& device, PhysicalDiskSmartData& disk) {
    const std::string path = "/dev/" + device;
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        static std::atomic<bool> logged{false};
        tcmt::linux::LogOnce(logged,
                             "SmartReader: cannot open NVMe device node (root/CAP_SYS_ADMIN required)");
        return false;
    }

    unsigned char buffer[512] = {};
    struct nvme_admin_cmd command {};
    command.opcode = 0x02;      // Get Log Page
    command.nsid = 0xffffffff;  // controller-wide
    command.addr = reinterpret_cast<uint64_t>(buffer);
    command.data_len = sizeof(buffer);
    // CDW10: LID (bits 7:0) | NUMDL (bits 31:16, number of dwords - 1).
    command.cdw10 = 0x02U | (127U << 16);
    command.timeout_ms = 5000;

    const int result = ::ioctl(fd, NVME_IOCTL_ADMIN_CMD, &command);
    ::close(fd);
    if (result != 0) {
        static std::atomic<bool> logged{false};
        tcmt::linux::LogOnce(logged,
                             "SmartReader: NVMe admin passthrough denied (root/CAP_SYS_ADMIN required)");
        return false;
    }

    const uint8_t criticalWarning = buffer[0];
    const int kelvin = static_cast<int>(buffer[1]) | (static_cast<int>(buffer[2]) << 8);
    const uint8_t percentageUsed = buffer[5];

    // A zeroed/invalid log page would otherwise look like a perfect drive.
    // Available Spare has no meaningful zero value, so all four zero is a
    // reliable "no data" signature.
    if (kelvin <= 273 && criticalWarning == 0 && percentageUsed == 0 && buffer[3] == 0) {
        return false;
    }

    disk.smartSupported = true;
    disk.smartEnabled = true;
    disk.temperature = kelvin > 273 ? static_cast<double>(kelvin - 273) : -1.0;
    disk.wearLeveling = percentageUsed;
    disk.totalBytesRead = Le128Low64(buffer + 32) * 512000ULL;
    disk.totalBytesWritten = Le128Low64(buffer + 48) * 512000ULL;
    disk.powerCycleCount = Le128Low64(buffer + 112);
    disk.powerOnHours = Le128Low64(buffer + 128);
    disk.uncorrectableErrors = Le128Low64(buffer + 160);
    disk.healthPercentage = static_cast<uint8_t>(
        100 - (std::min)(static_cast<int>(percentageUsed), 100));

    AddAttribute(disk, 0x01, criticalWarning, 0, criticalWarning,
                 "Critical Warning", criticalWarning != 0);
    AddAttribute(disk, 0x02, kelvin > 273 ? static_cast<uint8_t>(kelvin - 273) : 0, 0,
                 static_cast<uint64_t>(std::max(kelvin, 0)),
                 "Temperature", false);
    AddAttribute(disk, 0x03, buffer[3], 0, buffer[3], "Available Spare", false);
    AddAttribute(disk, 0x05, percentageUsed, 0, percentageUsed,
                 "Percentage Used", false);
    AddAttribute(disk, 0x0B, 0, 0, disk.powerCycleCount, "Power Cycles", false);
    AddAttribute(disk, 0x0C, 0, 0, disk.powerOnHours, "Power On Hours", false);
    return true;
}

bool ReadAtaSmart(const std::string& device, PhysicalDiskSmartData& disk) {
    const std::string path = "/dev/" + device;
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        static std::atomic<bool> logged{false};
        tcmt::linux::LogOnce(logged,
                             "SmartReader: cannot open SATA device node (root/disk group required)");
        return false;
    }

    unsigned char cdb[12] = {};
    cdb[0] = 0xA1;                    // ATA PASS-THROUGH (12)
    cdb[1] = 3 << 1;                  // PROTOCOL = PIO Data-In (SAT: 3)
    cdb[2] = (1 << 5) | (1 << 3) | 2; // CK_COND, T_DIR=from device, T_LENGTH=sector count
    cdb[3] = 0xD0;                    // SMART READ DATA
    cdb[4] = 1;                       // one sector
    cdb[9] = 0xB0;                    // SMART command

    unsigned char data[512] = {};
    unsigned char sense[64] = {};
    struct sg_io_hdr header {};
    header.interface_id = 'S';
    header.dxfer_direction = SG_DXFER_FROM_DEV;
    header.cmd_len = static_cast<unsigned char>(sizeof(cdb));
    header.mx_sb_len = sizeof(sense);
    header.dxfer_len = sizeof(data);
    header.dxferp = data;
    header.cmdp = cdb;
    header.sbp = sense;
    header.timeout = 5000;

    const int result = ::ioctl(fd, SG_IO, &header);
    ::close(fd);
    if (result < 0 || header.status != 0 || header.host_status != 0 ||
        header.driver_status != 0) {
        static std::atomic<bool> logged{false};
        tcmt::linux::LogOnce(logged,
                             "SmartReader: SATA SMART passthrough denied (root/CAP_SYS_RAWIO required)");
        return false;
    }

    // 30 SMART attributes, 12 bytes each, starting at offset 2.
    bool criticalFailure = false;
    bool anyAttribute = false;
    int lifeLeft = -1;
    double temperature = -1.0;
    for (int i = 0; i < 30; ++i) {
        const unsigned char* entry = data + 2 + i * 12;
        const uint8_t id = entry[0];
        if (id == 0) continue;
        anyAttribute = true;
        const uint8_t current = entry[3];
        const uint8_t worst = entry[4];
        uint64_t raw = 0;
        for (int j = 0; j < 6; ++j) {
            raw |= static_cast<uint64_t>(entry[5 + j]) << (8 * j);
        }

        bool critical = false;
        switch (id) {
            case 5:   // Reallocated Sector Count
                disk.reallocatedSectorCount = raw & 0xFFFFFFFFFFFFULL;
                critical = disk.reallocatedSectorCount != 0;
                AddAttribute(disk, id, current, worst, raw, "Reallocated Sectors", true);
                break;
            case 9:   // Power-On Hours (low 16 bits per ATA spec)
                disk.powerOnHours = raw & 0xFFFFULL;
                AddAttribute(disk, id, current, worst, raw, "Power On Hours", false);
                break;
            case 12:  // Power Cycle Count
                disk.powerCycleCount = raw & 0xFFFFULL;
                AddAttribute(disk, id, current, worst, raw, "Power Cycles", false);
                break;
            case 187: // Reported Uncorrectable
                disk.uncorrectableErrors = raw;
                critical = raw != 0;
                AddAttribute(disk, id, current, worst, raw, "Reported Uncorrectable", true);
                break;
            case 194: {  // Temperature: raw[0] is Celsius on most drives
                const int celsius = entry[5] != 0 ? entry[5] : entry[6];
                if (celsius > 0 && celsius <= 150) temperature = celsius;
                AddAttribute(disk, id, current, worst, raw, "Temperature", false);
                break;
            }
            case 197: // Current Pending Sector
                disk.currentPendingSector = raw;
                critical = raw != 0;
                AddAttribute(disk, id, current, worst, raw, "Current Pending Sectors", true);
                break;
            case 198: // Offline Uncorrectable
                critical = raw != 0;
                AddAttribute(disk, id, current, worst, raw, "Offline Uncorrectable", true);
                break;
            case 177:
            case 231:  // SSD wear-leveling / life left (normalized 0-100)
                lifeLeft = current;
                disk.wearLeveling = 100 - static_cast<int>(std::min<uint8_t>(current, 100));
                AddAttribute(disk, id, current, worst, raw,
                             id == 177 ? "Wear Leveling Count" : "SSD Life Left", false);
                break;
            default:
                AddAttribute(disk, id, current, worst, raw, "SMART Attribute", false);
                break;
        }
        if (critical) criticalFailure = true;
    }

    // An all-zero page means the bridge/kernel returned success without data.
    if (!anyAttribute) return false;

    disk.smartSupported = true;
    disk.smartEnabled = true;
    disk.temperature = temperature;
    // SMART data has no single health percentage. Use the remaining-life
    // attribute when the drive exposes one; otherwise report 0 % for a drive
    // with a failing critical counter and 100 % when none failed.
    if (lifeLeft >= 0) {
        disk.healthPercentage = static_cast<uint8_t>((std::min)(lifeLeft, 100));
    } else {
        disk.healthPercentage = criticalFailure ? 0 : 100;
    }
    return true;
}

}  // namespace

bool SmartReader::Read(int diskIndex, PhysicalDiskSmartData& smartData) {
    const std::vector<std::string> disks = tcmt::linux::ListPhysicalDisks();
    if (diskIndex < 0 || static_cast<size_t>(diskIndex) >= disks.size()) return false;
    const std::string& device = disks[static_cast<size_t>(diskIndex)];

    if (device.compare(0, 4, "nvme") == 0) {
        return ReadNvmeHealth(device, smartData);
    }
    if (device.compare(0, 2, "sd") == 0 || device.compare(0, 2, "vd") == 0 ||
        device.compare(0, 2, "hd") == 0 || device.compare(0, 3, "xvd") == 0) {
        return ReadAtaSmart(device, smartData);
    }
    // mmcblk and other transports have no SMART log; leave the disk as
    // unsupported instead of reporting a fake health value.
    return false;
}

#else
#error "Unsupported platform"
#endif // TCMT_WINDOWS
