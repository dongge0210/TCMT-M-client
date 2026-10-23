// SmartReader_mac.mm — macOS IOKit SMART reader (NVMe + ATA dual-path)
// Objective-C++ for CoreFoundation/IOKit types

#import <CoreFoundation/CoreFoundation.h>
#import <IOKit/IOKitLib.h>
#import <IOKit/storage/IOMedia.h>
#import <IOKit/storage/IOBlockStorageDevice.h>
#import <IOKit/storage/IOStorageDeviceCharacteristics.h>
#import <IOKit/storage/ata/ATASMARTLib.h>
#import <IOKit/storage/nvme/NVMeSMARTLibExternal.h>
#import <IOKit/IOCFPlugIn.h>

#include "SmartReader.h"
#include "../DataStruct/DataStruct.h"
#include "../I18n.h"   // attribute names are prose, so they live in the table
#include "../Utils/Logger.h"

#include <algorithm>
#include <cwchar>

// WCHAR = char16_t on macOS, wchar_t on Windows — polyfill for both
template<typename T>
static inline T* wcs_ncopy(T* dst, const T* src, size_t maxLen) {
    size_t i = 0;
    while (i < maxLen - 1 && src[i] != 0) { dst[i] = src[i]; i++; }
    if (i < maxLen) dst[i] = 0;
    return dst;
}

// ====================================================================
// Helpers
// ====================================================================

static io_service_t GetIOMediaForBSD(const char *bsdName) {
    CFMutableDictionaryRef match = IOBSDNameMatching(kIOMasterPortDefault, 0, bsdName);
    if (!match) return IO_OBJECT_NULL;
    io_service_t svc = IOServiceGetMatchingService(kIOMasterPortDefault, match);
    return svc;
}

static io_service_t FindNVMeController(io_service_t media) {
    // Walk up the IOKit parent chain. On Apple Silicon the NVMe controller
    // sits behind IOBlockStorageDriver with a chip-specific class name.
    // Instead of guessing class names, just probe every node up to 3 levels
    // deep and return the first one that responds to NVMe SMART reads.
    io_registry_entry_t node = media;
    io_service_t found = IO_OBJECT_NULL;
    IOObjectRetain(node); // we'll release at end

    for (int level = 0; level < 4 && node != IO_OBJECT_NULL; level++) {
        // Try NVMe SMART plugin on this node
        IOCFPlugInInterface **plugin = nullptr;
        SInt32 score = 0;
        if (IOCreatePlugInInterfaceForService(node,
                kIONVMeSMARTUserClientTypeID, kIOCFPlugInInterfaceID,
                &plugin, &score) == kIOReturnSuccess && plugin) {
            IONVMeSMARTInterface **iface = nullptr;
            HRESULT hr = (*plugin)->QueryInterface(plugin,
                CFUUIDGetUUIDBytes(kIONVMeSMARTInterfaceID), (void**)&iface);
            if (SUCCEEDED(hr) && iface) {
                NVMeSMARTData raw{};
                if ((*iface)->SMARTReadData(iface, &raw) == kIOReturnSuccess) {
                    found = node;
                    IOObjectRetain(found); // extra retain for caller
                    (*iface)->Release(iface);
                    IODestroyPlugInInterface(plugin);
                    break;
                }
                (*iface)->Release(iface);
            }
            IODestroyPlugInInterface(plugin);
        }
        // Move up one level
        io_iterator_t upIter;
        if (IORegistryEntryGetParentIterator(node, kIOServicePlane, &upIter) == KERN_SUCCESS) {
            io_registry_entry_t up = IOIteratorNext(upIter);
            // Drain remaining
            io_registry_entry_t rest;
            while ((rest = IOIteratorNext(upIter)) != IO_OBJECT_NULL)
                IOObjectRelease(rest);
            IOObjectRelease(upIter);
            IOObjectRelease(node);
            node = up;
        } else {
            IOObjectRelease(node);
            node = IO_OBJECT_NULL;
        }
    }
    if (node != IO_OBJECT_NULL) IOObjectRelease(node);
    return found;
}

static io_service_t FindATABlockDevice(io_service_t media) {
    io_iterator_t iter;
    if (IORegistryEntryGetParentIterator(media, kIOServicePlane, &iter) != KERN_SUCCESS)
        return IO_OBJECT_NULL;
    io_service_t found = IO_OBJECT_NULL;
    io_registry_entry_t parent;
    while ((parent = IOIteratorNext(iter)) != IO_OBJECT_NULL) {
        if (IOObjectConformsTo(parent, kIOBlockStorageDeviceClass)) {
            found = parent; break;
        }
        IOObjectRelease(parent);
    }
    IOObjectRelease(iter);
    return found;
}

// ====================================================================
// SMART attribute ID to human-readable name & description (Chinese)
// On macOS WCHAR = char16_t, so use u"" literals not L"".
// ====================================================================
// UTF-8 → UTF-16 (WCHAR is char16_t here). Static buffer: callers copy
// immediately, which is what every call site does.
static const WCHAR* WideFromUtf8(const char* s) {
    static WCHAR buf[192];
    size_t o = 0;
    const auto* p = reinterpret_cast<const unsigned char*>(s);
    while (*p && o + 2 < sizeof(buf) / sizeof(buf[0])) {
        uint32_t cp;
        int extra;
        if (*p < 0x80)            { cp = *p;        extra = 0; }
        else if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; extra = 1; }
        else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; extra = 2; }
        else                      { cp = *p & 0x07; extra = 3; }
        ++p;
        for (int i = 0; i < extra && (*p & 0xC0) == 0x80; ++i, ++p)
            cp = (cp << 6) | (*p & 0x3F);
        if (cp > 0xFFFF) {          // surrogate pair
            cp -= 0x10000;
            buf[o++] = static_cast<WCHAR>(0xD800 + (cp >> 10));
            buf[o++] = static_cast<WCHAR>(0xDC00 + (cp & 0x3FF));
        } else {
            buf[o++] = static_cast<WCHAR>(cp);
        }
    }
    buf[o] = 0;
    return buf;
}

// Attribute names come from the shared i18n table, the same entries the
// Windows reader uses, so both platforms and both SMART paths agree.
static const WCHAR* GetSmartAttrName(uint8_t id) {
    char key[24];
    snprintf(key, sizeof(key), "smart.%u", static_cast<unsigned>(id));
    return WideFromUtf8(tcmt::Tr(key));
}

static const WCHAR* GetSmartAttrDesc(uint8_t id) {
    switch (id) {
        case 1:   return u"底层数据读取错误率，反映磁盘表面或磁头读取可靠性";
        case 3:   return u"磁盘电机从静止到达工作转速所需时间";
        case 15:  return u"SSD/闪存介质数据完整性错误计数，>0 需关注";
        case 16:  return u"存储的错误信息日志总条目数";
        case 4:   return u"硬盘磁头启停/加载卸载的累计次数";
        case 5:   return u"已被映射到备用扇区的坏扇区数量，>0 说明磁盘开始老化";
        case 7:   return u"磁头定位到目标磁道的错误率";
        case 9:   return u"磁盘累计通电运行小时数";
        case 12:  return u"磁盘累计通电/断电循环次数";
        case 169: return u"SSD 剩余可擦写寿命百分比，100=全新，0=耗尽";
        case 170: return u"SSD 可用于替换坏块的预留空间百分比";
        case 171: return u"SSD 编程(写入)操作失败的总次数";
        case 172: return u"SSD 擦除操作失败的总次数";
        case 173: return u"SSD 各块的擦写次数均衡程度，数值越低越不均匀";
        case 174: return u"非正常断电导致磁盘停止工作的次数";
        case 177: return u"SSD 擦写磨损程度，100=全新，数值越低磨损越严重";
        case 187: return u"已检测到但无法通过 ECC 纠正的数据错误数";
        case 188: return u"磁盘命令执行超时的累计次数";
        case 190: return u"硬盘内部气流温度（摄氏度）";
        case 192: return u"异常断电后磁头紧急缩回/撤离的次数";
        case 193: return u"磁头加载/卸载到停泊区的累计次数";
        case 194: return u"磁盘当前工作温度（摄氏度），过高会缩短寿命";
        case 196: return u"已被重新分配到备用扇区的扇区事件计数，>0 需关注";
        case 197: return u"当前已检测到不稳定但尚未重新分配的待处理扇区数";
        case 198: return u"无法通过 ECC 和重试修复的永久性坏扇区数";
        case 199: return u"SATA 数据线传输 CRC 校验错误，常由数据线/接口接触不良引起";
        case 230: return u"SSD 剩余寿命百分比，类似 169/177";
        case 231: return u"SSD 剩余寿命百分比，100=全新，0=耗尽";
        case 232: return u"SSD 可用于替换坏块的空间余量百分比";
        case 233: return u"SSD NAND 闪存累计写入数据量";
        case 234: return u"SSD 所有块的平均擦写次数";
        case 235: return u"SSD 单个块的最高擦写次数";
        case 202: return u"SSD 已消耗的写入寿命百分比，数值越高磨损越严重";
        case 241: return u"主机通过接口向磁盘累计写入的数据总量";
        case 242: return u"主机通过接口从磁盘累计读取的数据总量";
        case 246: return u"SSD 自出厂以来所有块的累计擦写总次数";
        default:  return u"";
    }
}

// ====================================================================
// NVMe SMART
// ====================================================================
static bool ReadNVMeSmart(int diskIndex, PhysicalDiskSmartData &out) {
    char bsd[32];
    snprintf(bsd, sizeof(bsd), "disk%d", diskIndex);

    io_service_t media = GetIOMediaForBSD(bsd);
    if (media == IO_OBJECT_NULL) return false;

    io_service_t nvmeCtrl = FindNVMeController(media);
    IOObjectRelease(media);
    if (nvmeCtrl == IO_OBJECT_NULL) return false;

    IOCFPlugInInterface **plugin = nullptr;
    SInt32 score = 0;
    bool ok = false;

    kern_return_t kr = IOCreatePlugInInterfaceForService(nvmeCtrl, kIONVMeSMARTUserClientTypeID,
                                           kIOCFPlugInInterfaceID, &plugin, &score);
    if (kr == kIOReturnSuccess && plugin) {
        IONVMeSMARTInterface **iface = nullptr;
        HRESULT hr = (*plugin)->QueryInterface(plugin,
            CFUUIDGetUUIDBytes(kIONVMeSMARTInterfaceID), (void**)&iface);
        if (SUCCEEDED(hr) && iface) {
            NVMeSMARTData raw{};
            if ((*iface)->SMARTReadData(iface, &raw) == kIOReturnSuccess) {
                out.smartSupported = true;
                out.smartEnabled = true;

                int tempC = (int)raw.TEMPERATURE - 273;
                if (tempC > 0 && tempC < 128)
                    out.temperature = (double)tempC;

                if (raw.PERCENTAGE_USED <= 100) {
                    out.healthPercentage = (uint8_t)(100 - raw.PERCENTAGE_USED);
                    out.wearLeveling = raw.PERCENTAGE_USED / 100.0;
                }

                uint64_t hours = raw.POWER_ON_HOURS[0];
                if (hours > 0 && hours < 100000000)
                    out.powerOnHours = hours;

                // Populate NVMe pseudo SMART attributes for display
                out.attributeCount = 0;
                auto addAttr = [&](uint8_t id, uint8_t cur, uint64_t rawVal, const WCHAR* name, const WCHAR* desc) {
                    if (out.attributeCount >= 32) return;
                    auto& a = out.attributes[out.attributeCount++];
                    a.id = id; a.current = cur; a.rawValue = rawVal; a.worst = 0;
                    wcs_ncopy(a.name, name, 63);
                    wcs_ncopy(a.description, desc, 127);
                };
                addAttr(0x01, raw.CRITICAL_WARNING, raw.CRITICAL_WARNING,
                    WideFromUtf8(tcmt::Tr("smart.nvme.warning")),
                    u"NVMe 关键警告：0=正常，非0表示存在严重问题");
                addAttr(0x02, (uint8_t)tempC, raw.TEMPERATURE,
                    WideFromUtf8(tcmt::Tr("smart.nvme.temp")),
                    u"NVMe 复合温度传感器（摄氏度）");
                addAttr(0x03, raw.AVAILABLE_SPARE, raw.AVAILABLE_SPARE,
                    WideFromUtf8(tcmt::Tr("smart.nvme.avail_spare")),
                    u"剩余可替换坏块的备用空间百分比");
                addAttr(0x04, raw.AVAILABLE_SPARE_THRESHOLD, raw.AVAILABLE_SPARE_THRESHOLD,
                    WideFromUtf8(tcmt::Tr("smart.nvme.spare_thresh")),
                    u"备用空间低于此阈值时触发警告");
                addAttr(0x05, raw.PERCENTAGE_USED, raw.PERCENTAGE_USED,
                    WideFromUtf8(tcmt::Tr("smart.nvme.pct_used")),
                    u"已消耗的额定写入寿命百分比，100=寿命耗尽");
                addAttr(0x0B, 0, raw.POWER_ON_HOURS[0],
                    WideFromUtf8(tcmt::Tr("smart.nvme.power_on")),
                    u"NVMe 盘累计通电运行小时数");

                ok = true;
            }
            (*iface)->Release(iface);
        }
        IODestroyPlugInInterface(plugin);
    }

    IOObjectRelease(nvmeCtrl);
    return ok;
}

// ====================================================================
// ATA SMART (SATA drives, older Intel Macs)
// ====================================================================

static bool ReadATASmart(int diskIndex, PhysicalDiskSmartData &out) {
    char bsd[32];
    snprintf(bsd, sizeof(bsd), "disk%d", diskIndex);

    io_service_t media = GetIOMediaForBSD(bsd);
    if (media == IO_OBJECT_NULL) return false;

    io_service_t ataSvc = FindATABlockDevice(media);
    if (ataSvc == IO_OBJECT_NULL) { IOObjectRelease(media); return false; }

    IOCFPlugInInterface **plugin = nullptr;
    SInt32 score = 0;
    bool ok = false;

    if (IOCreatePlugInInterfaceForService(ataSvc, kIOATASMARTUserClientTypeID,
                                           kIOCFPlugInInterfaceID, &plugin, &score) == kIOReturnSuccess && plugin) {
        IOATASMARTInterface **iface = nullptr;
        if ((*plugin)->QueryInterface(plugin, CFUUIDGetUUIDBytes(kIOATASMARTInterfaceID),
                                       (void**)&iface) == kIOReturnSuccess && iface) {
            ATASMARTData raw{};
            if ((*iface)->SMARTReadData(iface, &raw) == kIOReturnSuccess) {
                const UInt8 *bytes = (const UInt8 *)&raw;
                out.smartSupported = true;
                out.smartEnabled = true;

                double tempRead = -1;
                bool isSSD = false;
                bool hasRotationAttr = false;
                int health = 100;
                int attrIdx = 0;

                for (int attrOff = 2; attrOff < 512 - 12 && attrIdx < 32; attrOff += 12) {
                    int aid = bytes[attrOff];
                    if (aid == 0 || aid == 0xFF) continue;
                    int cur = bytes[attrOff + 3];
                    int worst = bytes[attrOff + 4];

                    uint64_t rawVal = 0;
                    for (int j = 0; j < 6; j++)
                        rawVal |= ((uint64_t)bytes[attrOff + 5 + j] << (8 * j));

                    auto& attr = out.attributes[attrIdx];
                    attr.id = (uint8_t)aid;
                    attr.flags = bytes[attrOff + 1];
                    attr.current = (uint8_t)cur;
                    attr.worst = (uint8_t)worst;
                    attr.rawValue = rawVal;
                    wcs_ncopy(attr.name, GetSmartAttrName(attr.id), 63);
                    wcs_ncopy(attr.description, GetSmartAttrDesc(attr.id), 127);
                    attrIdx++;

                    // Temperature: 190(=BE)=Airflow, 194(=C2)=Temperature
                    if (aid == 0xBE || aid == 0xC2) {
                        for (int bo = 5; bo <= 10; bo++) {
                            int t = bytes[attrOff + bo];
                            if (t >= 15 && t <= 120) { tempRead = (double)t; break; }
                        }
                        if (tempRead < 0) {
                            int nv = cur;
                            if (nv >= 15 && nv <= 120) tempRead = (double)nv;
                        }
                    }

                    // Power-on hours (ID 9)
                    if (aid == 9 && rawVal < 100000000)
                        out.powerOnHours = rawVal;

                    // HDD-specific: spin-up time (3), start-stop count (4)
                    if (aid == 3 || aid == 4) hasRotationAttr = true;

                    // Health-critical: 5, 196, 197, 198
                    if (aid == 5 || aid == 196 || aid == 197 || aid == 198) {
                        if (rawVal > 0) {
                            if (aid == 5 && rawVal > 0) health = (std::min)(health, 95);
                            if (aid == 5 && rawVal > 10) health = (std::min)(health, 80);
                            if (aid == 197 && rawVal > 0) health = (std::min)(health, 90);
                            if (aid == 198 && rawVal > 0) health = (std::min)(health, 85);
                            if (aid == 5) out.reallocatedSectorCount = rawVal;
                            if (aid == 197) out.currentPendingSector = rawVal;
                            if (aid == 198) out.uncorrectableErrors = rawVal;
                        }
                    }

                    // Wear leveling / SSD remaining life
                    if (aid == 169 || aid == 177 || aid == 230 ||
                        aid == 231 || aid == 232 || aid == 233 || aid == 0xE7) {
                        if (cur > 0 && cur <= 100) {
                            out.wearLeveling = 1.0 - (cur / 100.0);
                            isSSD = true;
                            health = (std::min)(health, cur);
                        }
                    }
                }

                out.attributeCount = attrIdx;
                if (tempRead > 0) out.temperature = tempRead;
                out.healthPercentage = (uint8_t)health;
                if (!isSSD && !hasRotationAttr) isSSD = true;
                ok = true;
            }
            (*iface)->Release(iface);
        }
        IODestroyPlugInInterface(plugin);
    }

    IOObjectRelease(ataSvc);
    IOObjectRelease(media);
    return ok;
}

// ====================================================================
// Public entry point (called from SmartReader.cpp stub)
// ====================================================================

extern "C" bool SmartReaderMacRead(int diskIndex, PhysicalDiskSmartData &smartData) {
    if (ReadNVMeSmart(diskIndex, smartData)) {
        wcs_ncopy(smartData.diskType, u"SSD", 15);
        return true;
    }

    if (ReadATASmart(diskIndex, smartData)) {
        // isSSD is set inside ReadATASmart when wear-leveling attrs are found.
        // Fallback: all Macs with NVMe/SSD default to SSD; HDD only if rotation attrs present.
        // diskType already set by ReadATASmart's isSSD logic through the wear/heuristic path.
        // But since isSSD is local, check wearLeveling and health to decide.
        if (smartData.wearLeveling > 0.0) {
            wcs_ncopy(smartData.diskType, u"SSD", 15);
        } else {
            wcs_ncopy(smartData.diskType, u"HDD", 15);
        }
        return true;
    }

    return false;
}
