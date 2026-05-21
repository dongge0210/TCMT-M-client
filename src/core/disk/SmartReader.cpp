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
        DWORD gvpErr = 0;
        bool gvpOk = DeviceIoControl(hDevice, SMART_GET_VERSION,
                                     nullptr, 0,
                                     &gvp, sizeof(gvp),
                                     &bytesReturned, nullptr);
        if (!gvpOk) gvpErr = GetLastError();
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
                auto addAttr = [&](uint8_t id, uint8_t cur, uint64_t rawVal, const wchar_t* name, const wchar_t* desc) {
                    if (smartData.attributeCount >= 32) return;
                    auto& a = smartData.attributes[smartData.attributeCount++];
                    a.id = id; a.current = cur; a.rawValue = rawVal; a.worst = 0;
                    wcsncpy_s(a.name, name, _TRUNCATE);
                    wcsncpy_s(a.description, desc, _TRUNCATE);
                };
                addAttr(0x01, raw[0], raw[0], L"严重警告标志",
                    L"NVMe 关键警告：0=正常，非0表示存在严重问题");
                addAttr(0x02, (uint8_t)(raw[1] | (raw[2] << 8)) - 273,
                            raw[1] | (raw[2] << 8), L"温度",
                    L"NVMe 复合温度传感器（摄氏度）");
                addAttr(0x03, raw[3], raw[3], L"可用备用空间",
                    L"剩余可替换坏块的备用空间百分比");
                addAttr(0x04, raw[4], raw[4], L"可用备用空间阈值",
                    L"备用空间低于此阈值时触发警告");
                addAttr(0x05, raw[5], raw[5], L"已用寿命百分比",
                    L"已消耗的额定写入寿命百分比，100=寿命耗尽");
                uint64_t v6=0,v7=0,v8=0,v9=0,v10=0,v11=0,v12=0,v13=0,v14=0,v15=0;
                memcpy(&v6,  raw + 32, 8);
                memcpy(&v7,  raw + 48, 8);
                memcpy(&v8,  raw + 64, 8);
                memcpy(&v9,  raw + 80, 8);
                memcpy(&v10, raw + 96, 8);
                memcpy(&v11, raw + 112, 8);
                memcpy(&v12, raw + 128, 8);
                memcpy(&v13, raw + 144, 8);
                memcpy(&v14, raw + 160, 8);
                memcpy(&v15, raw + 176, 8);
                addAttr(0x06, 0, v6,  L"主机总计读取", L"主机累计从 NVMe 盘读取的数据量（512字节单位）");
                addAttr(0x07, 0, v7,  L"主机总计写入", L"主机累计向 NVMe 盘写入的数据量（512字节单位）");
                addAttr(0x08, 0, v8,  L"主机读命令计数", L"主机发出的读取命令累计次数");
                addAttr(0x09, 0, v9,  L"主机写命令计数", L"主机发出的写入命令累计次数");
                addAttr(0x0A, 0, v10, L"控制器忙状态时间", L"控制器处理命令的累计忙碌时间（分钟）");
                addAttr(0x0B, 0, v11, L"通电次数", L"NVMe 盘累计通电/断电循环次数");
                addAttr(0x0C, 0, v12, L"通电时间", L"NVMe 盘累计通电运行小时数");
                addAttr(0x0D, 0, v13, L"不安全关机计数", L"非正常断电/不安全关机的累计次数");
                addAttr(0x0E, 0, v14, L"介质与数据完整性错误", L"检测到的介质错误和数据完整性错误总数");
                addAttr(0x0F, 0, v15, L"错误日志项数", L"NVMe 错误信息日志中的条目数量");
            } else {
                DWORD nvmeErr = GetLastError();
                Logger::Info("SMART disk" + std::to_string(diskIndex) + " NVMe IOCTL fail err=" +
                    std::to_string(nvmeErr));
            }
            CloseHandle(hNvme);
        } else {
            Logger::Info("SMART disk" + std::to_string(diskIndex) + " NVMe open fail err=" +
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

#else  // !TCMT_WINDOWS — macOS stub

extern "C" bool SmartReaderMacRead(int diskIndex, PhysicalDiskSmartData& smartData);
bool SmartReader::Read(int diskIndex, PhysicalDiskSmartData& smartData) {
    return SmartReaderMacRead(diskIndex, smartData);
}

#endif // TCMT_WINDOWS