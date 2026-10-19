#include "DiskInfo.h"
#include "../Utils/Logger.h"
#include "SmartReader.h"

#ifdef TCMT_WINDOWS
// ======================== Windows Implementation ========================
#include "../Utils/WinUtils.h"
#include "../Utils/WmiManager.h"
#include <wbemidl.h>
#pragma comment(lib, "wbemuuid.lib")

DiskInfo::DiskInfo() { QueryDrives(); }

void DiskInfo::QueryDrives() {
    drives.clear();
    DWORD driveMask = GetLogicalDrives();
    if (driveMask == 0) { Logger::Error("GetLogicalDrives Failed"); return; }
    for (int i = 0; i < 26; ++i) {
        if ((driveMask & (1 << i)) == 0) continue;
        char driveLetter = static_cast<char>('A' + i);
        if (driveLetter == 'A' || driveLetter == 'B') continue;
        std::wstring rootPath;
        rootPath.reserve(4);
        rootPath.push_back(static_cast<wchar_t>(L'A' + i));
        rootPath.append(L":\\");
        UINT driveType = GetDriveTypeW(rootPath.c_str());
        if (!(driveType == DRIVE_FIXED || driveType == DRIVE_REMOVABLE)) continue;
        ULARGE_INTEGER freeBytesAvailable{}, totalBytes{}, totalFreeBytes{};
        if (!GetDiskFreeSpaceExW(rootPath.c_str(), &freeBytesAvailable, &totalBytes, &totalFreeBytes)) continue;
        if (totalBytes.QuadPart == 0) continue;
        DriveInfo info{};
        info.letter = driveLetter;
        // 挂载点就是卷根。"C:\" 是 Win32 定义的三种 volume mount point 之一
        // （另两种是卷 GUID 路径和挂载文件夹），所以同一个字段在 macOS 上
        // 装 "/System/Volumes/Data" 也成立。卷名不必再取一次 ——
        // 下面 GetVolumeInformationW 给的已经是真卷标。
        info.mountPoint = WinUtils::WstringToString(rootPath);
        info.totalSize = totalBytes.QuadPart;
        info.freeSpace = totalFreeBytes.QuadPart;
        info.usedSpace = (totalBytes.QuadPart >= totalFreeBytes.QuadPart)
                         ? (totalBytes.QuadPart - totalFreeBytes.QuadPart) : 0ULL;
        wchar_t volumeName[MAX_PATH + 1] = {0};
        wchar_t fileSystemName[MAX_PATH + 1] = {0};
        DWORD fsFlags = 0;
        if (!GetVolumeInformationW(rootPath.c_str(), volumeName, MAX_PATH, nullptr, nullptr, &fsFlags, fileSystemName, MAX_PATH)) {
            info.label = "";
            info.fileSystem = "Unknown";
        } else {
            info.label = WinUtils::WstringToString(volumeName);
            if (info.label.empty()) info.label = "Unnamed";
            info.fileSystem = WinUtils::WstringToString(fileSystemName);
        }
        drives.push_back(std::move(info));
    }
    std::sort(drives.begin(), drives.end(), [](const DriveInfo& a, const DriveInfo& b) {
        return a.letter < b.letter;
    });
}

void DiskInfo::Refresh() { QueryDrives(); }
const std::vector<DriveInfo>& DiskInfo::GetDrives() const { return drives; }

std::vector<DiskData> DiskInfo::GetDisks() {
    std::vector<DiskData> disks;
    disks.reserve(drives.size());
    for (const auto& drive : drives) {
        DiskData d;
        d.letter = drive.letter;
        d.mountPoint = drive.mountPoint;
        d.label = drive.label;
        d.fileSystem = drive.fileSystem;
        d.totalSize = drive.totalSize;
        d.usedSpace = drive.usedSpace;
        d.freeSpace = drive.freeSpace;
        disks.push_back(std::move(d));
    }
    return disks;
}

static bool ParseDiskPartition(const std::wstring& text, int& diskIndexOut) {
    size_t posDisk = text.find(L"Disk #");
    if (posDisk == std::wstring::npos) return false;
    posDisk += 6;
    if (posDisk >= text.size()) return false;
    int num = 0;
    bool any = false;
    while (posDisk < text.size() && iswdigit(text[posDisk])) {
        any = true;
        num = num * 10 + (text[posDisk] - L'0');
        ++posDisk;
    }
    if (!any) return false;
    diskIndexOut = num;
    return true;
}

// 使用 WMI 关联查询获取物理磁盘到逻辑驱动器的映射
// Fast, non-blocking physical disk list (single Win32_DiskDrive query).
// No ASSOCIATORS letter mapping and no SMART reads — those can be slow or
// block on unresponsive drives, so callers publish this list first and read
// SMART per disk separately.
void DiskInfo::CollectPhysicalDiskInfo(WmiManager& wmi, SystemInfo& sysInfo) {
    IWbemServices* svc = wmi.GetWmiService();
    if (!svc) { Logger::Debug("WMI service invalid, skipping physical disk enumeration"); return; }

    std::map<int, PhysicalDiskSmartData> tempDisks;
    IEnumWbemClassObject* pEnum = nullptr;
    HRESULT hr = svc->ExecQuery(bstr_t(L"WQL"),
        bstr_t(L"SELECT Index,Model,SerialNumber,InterfaceType,Size,MediaType FROM Win32_DiskDrive"),
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &pEnum);
    if (SUCCEEDED(hr) && pEnum) {
        IWbemClassObject* obj = nullptr;
        ULONG ret = 0;
        while (pEnum->Next(WBEM_INFINITE, 1, &obj, &ret) == S_OK) {
            VARIANT vIndex, vModel, vSerial, vIface, vSize, vMedia;
            VariantInit(&vIndex); VariantInit(&vModel); VariantInit(&vSerial);
            VariantInit(&vIface); VariantInit(&vSize); VariantInit(&vMedia);
            if (SUCCEEDED(obj->Get(L"Index", 0, &vIndex, 0, 0))
                && (vIndex.vt == VT_I4 || vIndex.vt == VT_UI4)) {
                int idx = (vIndex.vt == VT_I4) ? vIndex.intVal : static_cast<int>(vIndex.uintVal);
                PhysicalDiskSmartData data{};
                data.physicalIndex = idx;
                if (SUCCEEDED(obj->Get(L"Model", 0, &vModel, 0, 0)) && vModel.vt == VT_BSTR)
                    wcsncpy_s(data.model, vModel.bstrVal, _TRUNCATE);
                if (SUCCEEDED(obj->Get(L"SerialNumber", 0, &vSerial, 0, 0)) && vSerial.vt == VT_BSTR)
                    wcsncpy_s(data.serialNumber, vSerial.bstrVal, _TRUNCATE);
                if (SUCCEEDED(obj->Get(L"InterfaceType", 0, &vIface, 0, 0)) && vIface.vt == VT_BSTR)
                    wcsncpy_s(data.interfaceType, vIface.bstrVal, _TRUNCATE);
                if (SUCCEEDED(obj->Get(L"Size", 0, &vSize, 0, 0))) {
                    if (vSize.vt == VT_UI8) data.capacity = vSize.ullVal;
                    else if (vSize.vt == VT_BSTR) data.capacity = _wcstoui64(vSize.bstrVal, nullptr, 10);
                }
                if (SUCCEEDED(obj->Get(L"MediaType", 0, &vMedia, 0, 0)) && vMedia.vt == VT_BSTR) {
                    std::wstring media = vMedia.bstrVal;
                    if (media.find(L"SSD") != std::wstring::npos || media.find(L"Solid State") != std::wstring::npos)
                        wcsncpy_s(data.diskType, L"SSD", _TRUNCATE);
                    else
                        wcsncpy_s(data.diskType, L"HDD", _TRUNCATE);
                } else {
                    // MediaType missing — infer from model name (case-insensitive)
                    bool isSSD = false;
                    if (vModel.vt == VT_BSTR) {
                        std::wstring mdl = vModel.bstrVal;
                        isSSD = (mdl.find(L"SSD") != std::wstring::npos ||
                                 mdl.find(L"ssd") != std::wstring::npos ||
                                 mdl.find(L"Solid State") != std::wstring::npos ||
                                 mdl.find(L"NVMe") != std::wstring::npos ||
                                 mdl.find(L"MZ") != std::wstring::npos ||
                                 mdl.find(L"mz") != std::wstring::npos);
                    }
                    wcsncpy_s(data.diskType, isSSD ? L"SSD" : L"HDD", _TRUNCATE);
                }
                data.smartSupported = false;
                data.smartEnabled = false;
                data.healthPercentage = 100;
                data.temperature = -1;
                data.logicalDriveCount = 0;
                tempDisks[idx] = data;
            }
            VariantClear(&vIndex); VariantClear(&vModel); VariantClear(&vSerial);
            VariantClear(&vIface); VariantClear(&vSize); VariantClear(&vMedia);
            obj->Release();
        }
        pEnum->Release();
    }

    sysInfo.physicalDisks.clear();
    for (auto& kv : tempDisks) {
        sysInfo.physicalDisks.push_back(kv.second);
        if (sysInfo.physicalDisks.size() >= 8) break;
    }
    Logger::Debug("Physical disk info enumeration complete: " + std::to_string(sysInfo.physicalDisks.size()));
}

void DiskInfo::CollectPhysicalDisks(WmiManager& wmi, const std::vector<DiskData>& logicalDisks,
                                    SystemInfo& sysInfo, bool readSmart) {
    IWbemServices* svc = wmi.GetWmiService();
    if (!svc) { Logger::Debug("WMI service invalid, skipping physical disk enumeration"); return; }
    std::map<int, std::vector<char>> physicalIndexToLetters;
    IEnumWbemClassObject* pEnum = nullptr;

    // 方法：对每个物理磁盘，使用 ASSOCIATORS OF 查询关联的分区，再查询关联的逻辑驱动器
    HRESULT hr = svc->ExecQuery(bstr_t(L"WQL"),
        bstr_t(L"SELECT DeviceID, Index FROM Win32_DiskDrive"),
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &pEnum);
    if (SUCCEEDED(hr) && pEnum) {
        IWbemClassObject* obj = nullptr;
        ULONG ret = 0;
        while (pEnum->Next(WBEM_INFINITE, 1, &obj, &ret) == S_OK) {
            VARIANT vtDeviceID, vtIndex;
            VariantInit(&vtDeviceID); VariantInit(&vtIndex);
            int diskIndex = -1;
            if (SUCCEEDED(obj->Get(L"Index", 0, &vtIndex, 0, 0)) 
                && (vtIndex.vt == VT_I4 || vtIndex.vt == VT_UI4)) {
                diskIndex = (vtIndex.vt == VT_I4) ? vtIndex.intVal : static_cast<int>(vtIndex.uintVal);
            }
            if (SUCCEEDED(obj->Get(L"DeviceID", 0, &vtDeviceID, 0, 0)) 
                && vtDeviceID.vt == VT_BSTR && diskIndex >= 0) {
                std::wstring diskDeviceID = vtDeviceID.bstrVal;
                // ASSOCIATORS OF {Win32_DiskDrive.DeviceID='...'} WHERE AssocClass = Win32_DiskDriveToDiskPartition
                std::wstring query = L"ASSOCIATORS OF {Win32_DiskDrive.DeviceID='";
                query += diskDeviceID;
                query += L"'} WHERE AssocClass = Win32_DiskDriveToDiskPartition";
                IEnumWbemClassObject* pEnumPart = nullptr;
                HRESULT hrPart = svc->ExecQuery(bstr_t(L"WQL"), bstr_t(query.c_str()), 
                    WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &pEnumPart);
                if (SUCCEEDED(hrPart) && pEnumPart) {
                    IWbemClassObject* objPart = nullptr;
                    ULONG retPart = 0;
                    while (pEnumPart->Next(WBEM_INFINITE, 1, &objPart, &retPart) == S_OK) {
                        VARIANT vtPartDeviceID;
                        VariantInit(&vtPartDeviceID);
                        if (SUCCEEDED(objPart->Get(L"DeviceID", 0, &vtPartDeviceID, 0, 0)) 
                            && vtPartDeviceID.vt == VT_BSTR) {
                            std::wstring partDeviceID = vtPartDeviceID.bstrVal;
                            // 再查询关联的逻辑驱动器
                            std::wstring queryLogical = L"ASSOCIATORS OF {Win32_DiskPartition.DeviceID='";
                            queryLogical += partDeviceID;
                            queryLogical += L"'} WHERE AssocClass = Win32_LogicalDiskToPartition";
                            IEnumWbemClassObject* pEnumLogical = nullptr;
                            HRESULT hrLogical = svc->ExecQuery(bstr_t(L"WQL"), bstr_t(queryLogical.c_str()),
                                WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &pEnumLogical);
                            if (SUCCEEDED(hrLogical) && pEnumLogical) {
                                IWbemClassObject* objLogical = nullptr;
                                ULONG retLogical = 0;
                                while (pEnumLogical->Next(WBEM_INFINITE, 1, &objLogical, &retLogical) == S_OK) {
                                    VARIANT vtLogicalDeviceID;
                                    VariantInit(&vtLogicalDeviceID);
                                    if (SUCCEEDED(objLogical->Get(L"DeviceID", 0, &vtLogicalDeviceID, 0, 0)) 
                                        && vtLogicalDeviceID.vt == VT_BSTR) {
                                        std::wstring logicalDeviceID = vtLogicalDeviceID.bstrVal;
                                        if (logicalDeviceID.length() >= 2 && logicalDeviceID[1] == L':') {
                                            char letter = static_cast<char>(::toupper(logicalDeviceID[0]));
                                            physicalIndexToLetters[diskIndex].push_back(letter);
                                        }
                                    }
                                    VariantClear(&vtLogicalDeviceID);
                                    objLogical->Release();
                                }
                                pEnumLogical->Release();
                            }
                        }
                        VariantClear(&vtPartDeviceID);
                        objPart->Release();
                    }
                    pEnumPart->Release();
                }
            }
            VariantClear(&vtDeviceID); VariantClear(&vtIndex);
            obj->Release();
        }
        pEnum->Release();
    }

    std::map<int, PhysicalDiskSmartData> tempDisks;
    pEnum = nullptr;
    hr = svc->ExecQuery(bstr_t(L"WQL"),
        bstr_t(L"SELECT Index,Model,SerialNumber,InterfaceType,Size,MediaType FROM Win32_DiskDrive"),
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &pEnum);
    if (SUCCEEDED(hr) && pEnum) {
        IWbemClassObject* obj = nullptr;
        ULONG ret = 0;
        while (pEnum->Next(WBEM_INFINITE, 1, &obj, &ret) == S_OK) {
            VARIANT vIndex, vModel, vSerial, vIface, vSize, vMedia;
            VariantInit(&vIndex); VariantInit(&vModel); VariantInit(&vSerial);
            VariantInit(&vIface); VariantInit(&vSize); VariantInit(&vMedia);
            if (SUCCEEDED(obj->Get(L"Index", 0, &vIndex, 0, 0))
                && (vIndex.vt == VT_I4 || vIndex.vt == VT_UI4)) {
                int idx = (vIndex.vt == VT_I4) ? vIndex.intVal : static_cast<int>(vIndex.uintVal);
                PhysicalDiskSmartData data{};
                data.physicalIndex = idx;
                if (SUCCEEDED(obj->Get(L"Model", 0, &vModel, 0, 0)) && vModel.vt == VT_BSTR)
                    wcsncpy_s(data.model, vModel.bstrVal, _TRUNCATE);
                if (SUCCEEDED(obj->Get(L"SerialNumber", 0, &vSerial, 0, 0)) && vSerial.vt == VT_BSTR)
                    wcsncpy_s(data.serialNumber, vSerial.bstrVal, _TRUNCATE);
                if (SUCCEEDED(obj->Get(L"InterfaceType", 0, &vIface, 0, 0)) && vIface.vt == VT_BSTR)
                    wcsncpy_s(data.interfaceType, vIface.bstrVal, _TRUNCATE);
                if (SUCCEEDED(obj->Get(L"Size", 0, &vSize, 0, 0))) {
                    if (vSize.vt == VT_UI8) data.capacity = vSize.ullVal;
                    else if (vSize.vt == VT_BSTR) data.capacity = _wcstoui64(vSize.bstrVal, nullptr, 10);
                }
                if (SUCCEEDED(obj->Get(L"MediaType", 0, &vMedia, 0, 0)) && vMedia.vt == VT_BSTR) {
                    std::wstring media = vMedia.bstrVal;
                    if (media.find(L"SSD") != std::wstring::npos || media.find(L"Solid State") != std::wstring::npos)
                        wcsncpy_s(data.diskType, L"SSD", _TRUNCATE);
                    else
                        wcsncpy_s(data.diskType, L"HDD", _TRUNCATE);
                } else {
                    // MediaType missing — infer from model name (case-insensitive)
                    bool isSSD = false;
                    if (vModel.vt == VT_BSTR) {
                        std::wstring mdl = vModel.bstrVal;
                        isSSD = (mdl.find(L"SSD") != std::wstring::npos ||
                                 mdl.find(L"ssd") != std::wstring::npos ||
                                 mdl.find(L"Solid State") != std::wstring::npos ||
                                 mdl.find(L"NVMe") != std::wstring::npos ||
                                 mdl.find(L"MZ") != std::wstring::npos ||
                                 mdl.find(L"mz") != std::wstring::npos);
                    }
                    wcsncpy_s(data.diskType, isSSD ? L"SSD" : L"HDD", _TRUNCATE);
                }
                data.smartSupported = false;
                data.smartEnabled = false;
                data.healthPercentage = 100;
                data.temperature = -1;
                data.logicalDriveCount = 0;
                tempDisks[idx] = data;
                // Try DeviceIoControl SMART — skip when caller wants a non-blocking
                // WMI-only enumeration (SMART can hang on unresponsive drives).
                if (readSmart)
                    SmartReader::Read(idx, tempDisks[idx]);
            }
            VariantClear(&vIndex); VariantClear(&vModel); VariantClear(&vSerial);
            VariantClear(&vIface); VariantClear(&vSize); VariantClear(&vMedia);
            obj->Release();
        }
        pEnum->Release();
    }
    for (auto& kv : physicalIndexToLetters) {
        int diskIdx = kv.first;
        auto it = tempDisks.find(diskIdx);
        if (it == tempDisks.end()) continue;
        auto& pd = it->second;
        int count = 0;
        for (char L : kv.second) {
            if (count >= 8) break;
            pd.logicalDriveLetters[count++] = L;
        }
        pd.logicalDriveCount = count;
    }
    sysInfo.physicalDisks.clear();
    for (auto& kv : tempDisks) {
        sysInfo.physicalDisks.push_back(kv.second);
        if (sysInfo.physicalDisks.size() >= 8) break;
    }
    Logger::Debug("Physical disk enumeration complete: " + std::to_string(sysInfo.physicalDisks.size()));
}

void DiskInfo::CollectSmartData(SystemInfo& sysInfo) {
    // SMART data is collected via SmartReader::Read() in the main enumeration path
}

#elif defined(TCMT_MACOS)
// ======================== macOS Implementation ========================
#include <sys/mount.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <nlohmann/json.hpp>

#include <dirent.h>
#include <algorithm>
#include <set>
#include <cstring>
#include <sys/attr.h>    // getattrlist / ATTR_VOL_NAME
#include <sys/param.h>   // MAXPATHLEN

// 取挂载点的真实卷名（ATTR_VOL_NAME）。读不到返回空串。
// 纯 C 路径，不需要 ObjC++，也就不必为此新增 .mm 文件。
//
// 返回布局（实测，见下面注释的字节）不是"长度 + 字符串"，而是：
//   u_int32_t        属性缓冲总长
//   attrreference_t  ATTR_VOL_NAME 的引用：attr_dataoffset 相对 attrreference 自身，
//                    attr_length 才是名字长度
//   <名字本体>        不带结尾 NUL，长度精确
// 把整块当"长度 + 字符串"读会切进两个长度字段和补零区，拿到夹着控制字符的串。
// 实测 /System/Volumes/Data：14 00 00 00 | 08 00 00 00 | 05 00 00 00 | "Data"
static std::string VolumeNameAt(const std::string& mountPoint) {
    struct attrlist attrs = {};
    attrs.bitmapcount = ATTR_BIT_MAP_COUNT;
    attrs.volattr = ATTR_VOL_INFO | ATTR_VOL_NAME;

    char buf[sizeof(uint32_t) + sizeof(attrreference_t) + MAXPATHLEN] = {};
    if (getattrlist(mountPoint.c_str(), &attrs, buf, sizeof(buf), 0) != 0) return {};

    attrreference_t ref = {};
    std::memcpy(&ref, buf + sizeof(uint32_t), sizeof(ref));
    if (ref.attr_dataoffset < 0 || ref.attr_length == 0) return {};

    const size_t base = sizeof(uint32_t);
    const size_t nameOffset = base + static_cast<size_t>(ref.attr_dataoffset);
    if (nameOffset + ref.attr_length > sizeof(buf)) return {};   // 越界守卫

    std::string name(buf + nameOffset, static_cast<size_t>(ref.attr_length));
    // attr_length 把结尾 NUL 也算在内（实测 "Data" 报 5、"Macintosh HD" 报 13）。
    // 不剥掉的话日志里会混进 NUL 字节，把整份日志变成 grep 认定的大二进制文件。
    if (!name.empty() && name.back() == '\0') name.pop_back();
    return name;
}

DiskInfo::DiskInfo() { QueryDrives(); }

void DiskInfo::QueryDrives() {
    drives.clear();

    // Enumerate mounted volumes via statfs
    int mountCount = getfsstat(nullptr, 0, MNT_NOWAIT);
    if (mountCount <= 0) {
        Logger::Error("DiskInfo: getfsstat failed");
        return;
    }

    std::vector<struct statfs> mounts(mountCount);
    getfsstat(mounts.data(), (int)(mounts.size() * sizeof(struct statfs)), MNT_NOWAIT);

    for (const auto& fs : mounts) {
        std::string mountpoint(fs.f_mntonname);
        std::string fstype(fs.f_fstypename);

        // 文件系统层面：这些根本不是磁盘卷
        if (fstype == "autofs" || fstype == "devfs" || fstype == "fdesc"
            || fstype == "procfs" || fstype == "devpts" || fstype == "overlay"
            || fstype == "nullfs" || fstype == "simfs") {
            continue;
        }

        // 卷层面改用挂载标志，不再靠一张越加越长的路径名单：
        //   MNT_LOCAL       排掉网络挂载
        //   MNT_DONTBROWSE  "Finder 不列它"。Preboot / VM / Update / xarts /
        //                   iSCPreboot / Hardware、/Volumes/Recovery、CoreSimulator
        //                   的 cryptex 挂载和模拟器卷全带这个标志 ——
        //                   原来那张名单漏掉了后面三类。
        // 两个例外，都是启动卷组：
        //   "/"                      密封只读系统快照，可用空间恒为 0，报了是废数
        //   "/System/Volumes/Data"   带 DONTBROWSE，但数据都在这儿，
        //                            是这台机器上唯一有真实可用空间的卷
        // 所以启动盘只报 Data 卷，卷组关系交给设计稿那行说明去讲。
        const bool isBootData = (mountpoint == "/System/Volumes/Data");
        if (!(fs.f_flags & MNT_LOCAL) || mountpoint == "/" ||
            ((fs.f_flags & MNT_DONTBROWSE) && !isBootData)) {
            continue;
        }

        // Get capacity and free space
        uint64_t total = (uint64_t)fs.f_blocks * (uint64_t)fs.f_bsize;
        uint64_t free = (uint64_t)fs.f_bfree * (uint64_t)fs.f_bsize;
        uint64_t used = total - free;

        // 随本次改动清掉两处死代码：
        //   1. 一个 <100MB 的过滤，本意跳过系统小卷 —— 新规则已覆盖，
        //      留着只会误伤用户挂的小镜像；
        //   2. 一个空壳的 "Skip boot partition" 判断，花括号里只有一句注释。
        // 配套去掉了 devname / bsdName 两个变量：它们只服务于上面那两处。

        DriveInfo info{};
        info.totalSize = total;
        info.freeSpace = free;
        info.usedSpace = used;
        info.fileSystem = fstype;
        info.mountPoint = mountpoint;

        // 真实卷名走 getattrlist(ATTR_VOL_NAME)。原来取挂载路径末段，
        // 那只对 /Volumes/MyDisk 这类碰巧成立；系统卷和模拟器卷会得到
        // "Data" 或一长串 UUID，都不是卷名。
        info.label = VolumeNameAt(mountpoint);
        if (info.label.empty()) {
            // 属性读不到才回落路径末段
            size_t lastSlash = mountpoint.find_last_of('/');
            info.label = (lastSlash != std::string::npos && lastSlash + 1 < mountpoint.size())
                         ? mountpoint.substr(lastSlash + 1) : mountpoint;
            if (info.label.empty()) info.label = "Untitled";
        }

        info.letter = 0;   // macOS 没有盘符；落点看 mountPoint

        // 不再按物理盘去重。原做法是"同一物理盘只留 getfsstat 里的第一个卷"：
        // 对系统盘毫无作用（新过滤规则已经滤掉了其余卷），却会误伤同盘的第二卷
        // —— 外置 SSD 上放两个 APFS 卷时只报一个。
        // 先记日志再 move —— move 之后 info.label 的值就不再保证了
        Logger::Debug("DiskInfo: found volume " + mountpoint
                    + " name=\"" + info.label + "\""
                    + " (" + fstype + ") size=" + std::to_string(total));
        drives.push_back(std::move(info));
    }
}

void DiskInfo::Refresh() { QueryDrives(); }
const std::vector<DriveInfo>& DiskInfo::GetDrives() const { return drives; }

std::vector<DiskData> DiskInfo::GetDisks() {
    std::vector<DiskData> disks;
    disks.reserve(drives.size());
    for (const auto& drive : drives) {
        DiskData d;
        d.letter = drive.letter;
        d.mountPoint = drive.mountPoint;
        d.label = drive.label;
        d.fileSystem = drive.fileSystem;
        d.totalSize = drive.totalSize;
        d.usedSpace = drive.usedSpace;
        d.freeSpace = drive.freeSpace;
        disks.push_back(std::move(d));
    }
    return disks;
}

// Collect SMART data - available on all platforms
void DiskInfo::CollectSmartData(SystemInfo& sysInfo) {
    FILE* fp = popen("system_profiler SPNVMeDataType -json 2>/dev/null", "r");
    if (!fp) { fp = popen("system_profiler SPSerialATADataType -json 2>/dev/null", "r"); if (!fp) return; }

    std::string json;
    char buf[4096];
    while (fgets(buf, sizeof(buf), fp)) json += buf;
    int rc = pclose(fp);
    if (rc != 0 || json.empty()) return;

    auto j = nlohmann::json::parse(json, nullptr, false);
    if (j.is_discarded()) return;

    const nlohmann::json* arr = nullptr;
    if (j.contains("SPNVMeDataType")) arr = &j["SPNVMeDataType"];
    else if (j.contains("SPSerialATADataType")) arr = &j["SPSerialATADataType"];
    if (!arr || !arr->is_array()) return;

    auto copyToWchar = [](WCHAR* dst, size_t dstLen, const std::string& src) {
        size_t n = std::min(dstLen - 1, src.size());
        for (size_t i = 0; i < n; ++i) dst[i] = static_cast<WCHAR>(src[i]);
        dst[n] = 0;
    };

    for (const auto& group : *arr) {
        const nlohmann::json& items = group.contains("_items") ? group["_items"] : group;
        if (!items.is_array()) continue;
        for (const auto& item : items) {
            PhysicalDiskSmartData pd = {};
            copyToWchar(pd.model, sizeof(pd.model)/sizeof(WCHAR), item.value("device_model", item.value("_name", "")));
            copyToWchar(pd.serialNumber, sizeof(pd.serialNumber)/sizeof(WCHAR), item.value("device_serial", ""));

            std::string status = item.value("smart_status", "");
            pd.smartSupported = !status.empty();
            pd.smartEnabled = (status == "Verified");
            pd.healthPercentage = (status == "Verified") ? 100u : 0u;

            if (item.contains("size_in_bytes")) {
                auto& sv = item["size_in_bytes"];
                if (sv.is_number()) pd.capacity = sv.get<uint64_t>();
                else if (sv.is_string()) try { pd.capacity = std::stoull(sv.get<std::string>()); } catch(...) {}
            }
            sysInfo.physicalDisks.push_back(pd);

            // Extract disk index from bsd_name (e.g., "disk0" → 0)
            std::string bsd = item.value("bsd_name", "");
            int diskIdx = 0;
            if (bsd.rfind("disk", 0) == 0) {
                try { diskIdx = std::stoi(bsd.substr(4)); } catch(...) {}
            }
            SmartReader::Read(diskIdx, sysInfo.physicalDisks.back());
        }
    }

    Logger::Debug("DiskInfo: SMART collected " + std::to_string(sysInfo.physicalDisks.size()) + " disks");
}

#elif defined(TCMT_LINUX)
// ======================== Linux Implementation ========================
#include "../Utils/Sysfs.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>

namespace tcmt::linux {
namespace {

struct DiskCounters {
    uint64_t readsCompleted = 0;
    uint64_t sectorsRead = 0;
    uint64_t writesCompleted = 0;
    uint64_t sectorsWritten = 0;
};

struct StatsState {
    std::mutex mutex;
    int64_t prevUs = 0;
    int64_t lastComputeUs = 0;
    std::map<std::string, DiskCounters> prev;
    std::map<std::string, DiskIoDelta> last;
};

StatsState& State() {
    static StatsState state;
    return state;
}

int64_t NowUs() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool StartsWith(const std::string& s, const char* prefix) {
    const size_t n = std::strlen(prefix);
    return s.size() >= n && s.compare(0, n, prefix) == 0;
}

bool AllDigits(const std::string& s, size_t from) {
    if (from >= s.size()) return false;
    for (size_t i = from; i < s.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

bool AllLetters(const std::string& s, size_t from) {
    if (from >= s.size()) return false;
    for (size_t i = from; i < s.size(); ++i) {
        if (!std::islower(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

std::map<std::string, DiskCounters> ReadDiskstats() {
    std::map<std::string, DiskCounters> result;
    std::ifstream ifs("/proc/diskstats");
    if (!ifs.is_open()) return result;

    std::string line;
    while (std::getline(ifs, line)) {
        std::istringstream ss(line);
        uint64_t major = 0, minor = 0;
        std::string name;
        if (!(ss >> major >> minor >> name)) continue;
        uint64_t values[11] = {};
        int count = 0;
        while (count < 11 && (ss >> values[count])) ++count;
        if (count < 7) continue;

        DiskCounters counters;
        counters.readsCompleted = values[0];
        counters.sectorsRead = values[2];
        counters.writesCompleted = values[4];
        counters.sectorsWritten = values[6];
        result[name] = counters;
    }
    return result;
}

double ReadMillidegreeCelsius(const std::string& path) {
    int64_t milli = 0;
    if (!ReadInt64(path, milli)) return 0.0;
    const double celsius = static_cast<double>(milli) / 1000.0;
    return celsius > 0.0 && celsius <= 150.0 ? celsius : 0.0;
}

std::string RealPath(const std::string& path) {
    char buffer[PATH_MAX] = {};
    if (!::realpath(path.c_str(), buffer)) return {};
    return std::string(buffer);
}

}  // namespace

bool IsWholeDisk(const std::string& name) {
    if (StartsWith(name, "nvme")) {
        // nvme0n1; partitions are nvme0n1p1 and never appear in /sys/block.
        const size_t n = name.find('n', 4);
        if (n == std::string::npos || n == 4) return false;
        return AllDigits(name.substr(4, n - 4), 0) && AllDigits(name, n + 1);
    }
    if (StartsWith(name, "mmcblk")) {
        return AllDigits(name, 6);
    }
    if (StartsWith(name, "sd") || StartsWith(name, "vd") || StartsWith(name, "hd") ||
        StartsWith(name, "xvd")) {
        const size_t prefix = StartsWith(name, "xvd") ? 3 : 2;
        return AllLetters(name, prefix);
    }
    return false;
}

std::vector<std::string> ListPhysicalDisks() {
    std::vector<std::string> disks;
    for (const std::string& name : ListDir("/sys/block")) {
        if (IsWholeDisk(name)) disks.push_back(name);
    }
    return disks;
}

std::map<std::string, DiskIoDelta> SampleDiskStats() {
    const auto counters = ReadDiskstats();
    const int64_t nowUs = NowUs();

    StatsState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);

    // Multiple disks are collected in one burst; only the first call of a
    // burst may compute a delta, otherwise the later calls see a ~0 interval.
    if (state.prevUs != 0 && state.lastComputeUs != 0 &&
        nowUs - state.lastComputeUs < 1000000) {
        return state.last;
    }

    std::map<std::string, DiskIoDelta> deltas;
    if (state.prevUs != 0 && nowUs > state.prevUs) {
        const double seconds = static_cast<double>(nowUs - state.prevUs) / 1.0e6;
        for (const auto& [name, current] : counters) {
            if (!IsWholeDisk(name)) continue;
            const auto it = state.prev.find(name);
            if (it == state.prev.end()) continue;
            const DiskCounters& prev = it->second;
            if (current.sectorsRead < prev.sectorsRead ||
                current.sectorsWritten < prev.sectorsWritten ||
                current.readsCompleted < prev.readsCompleted ||
                current.writesCompleted < prev.writesCompleted) {
                continue;  // counter reset (hotplug / overflow)
            }

            DiskIoDelta delta;
            delta.seconds = seconds;
            delta.readBytesPerSec = static_cast<uint64_t>(
                static_cast<double>((current.sectorsRead - prev.sectorsRead) * 512ULL) /
                seconds);
            delta.writeBytesPerSec = static_cast<uint64_t>(
                static_cast<double>((current.sectorsWritten - prev.sectorsWritten) * 512ULL) /
                seconds);
            delta.readIops = static_cast<uint64_t>(
                static_cast<double>(current.readsCompleted - prev.readsCompleted) / seconds);
            delta.writeIops = static_cast<uint64_t>(
                static_cast<double>(current.writesCompleted - prev.writesCompleted) / seconds);
            deltas[name] = delta;
        }
    }

    state.prev = counters;
    state.prevUs = nowUs;
    state.lastComputeUs = nowUs;
    state.last = deltas;
    return deltas;
}

bool ReadDiskTemperatureC(const std::string& device, double& celsius) {
    // NVMe hwmon is registered on the controller (nvme0), not on the
    // namespace (nvme0n1). Depending on kernel and driver, the namespace's
    // "device" symlink points at either one, so try every known location.
    std::vector<std::string> hwmonRoots;
    hwmonRoots.push_back("/sys/block/" + device + "/device/hwmon/");
    // Some kernels link the namespace's "device" at the namespace itself, so
    // the controller is reached through a second "device" hop.
    hwmonRoots.push_back("/sys/block/" + device + "/device/device/hwmon/");

    std::string controllerName;
    if (StartsWith(device, "nvme")) {
        const size_t n = device.find('n', 4);
        if (n != std::string::npos && n > 4) controllerName = device.substr(0, n);
    }
    if (!controllerName.empty()) {
        hwmonRoots.push_back("/sys/class/nvme/" + controllerName + "/hwmon/");
        hwmonRoots.push_back("/sys/class/nvme/" + controllerName + "/device/hwmon/");
    }

    for (const std::string& root : hwmonRoots) {
        for (const std::string& hw : ListDir(root, "hwmon")) {
            const double value = ReadMillidegreeCelsius(root + hw + "/temp1_input");
            if (value > 0.0) {
                celsius = value;
                return true;
            }
        }
    }

    // SATA/SAS: drivetemp registers a hwmon under the SCSI device. NVMe is
    // also matched here for kernels where the namespace/controller symlinks
    // differ from the paths above.
    const std::string deviceReal = RealPath("/sys/block/" + device + "/device");
    const std::string namespaceReal = RealPath("/sys/block/" + device);
    std::string controllerReal;
    if (!namespaceReal.empty()) {
        const size_t slash = namespaceReal.find_last_of('/');
        if (slash != std::string::npos) controllerReal = namespaceReal.substr(0, slash);
    }

    for (const std::string& hw : ListDir("/sys/class/hwmon", "hwmon")) {
        const std::string base = "/sys/class/hwmon/" + hw;
        const std::string name = ReadLine(base + "/name");
        if (name != "drivetemp" && name != "nvme") continue;

        bool matches = false;
        if (PathExists(base + "/device/block/" + device)) {
            matches = true;
        } else {
            const std::string hwmonDevice = RealPath(base + "/device");
            if (!hwmonDevice.empty()) {
                matches = hwmonDevice == deviceReal ||
                          (!controllerReal.empty() && hwmonDevice == controllerReal);
            }
        }
        if (!matches) continue;

        const double value = ReadMillidegreeCelsius(base + "/temp1_input");
        if (value > 0.0) {
            celsius = value;
            return true;
        }
    }
    return false;
}

std::string ReadDiskInterfaceType(const std::string& device) {
    if (StartsWith(device, "nvme")) return "NVMe";
    if (StartsWith(device, "mmcblk")) return "SD/MMC";
    if (StartsWith(device, "vd")) return "VirtIO";
    if (StartsWith(device, "xvd")) return "Xen";
    if (StartsWith(device, "hd")) return "IDE";

    if (StartsWith(device, "sd")) {
        std::string target = RealPath("/sys/block/" + device + "/device");
        std::transform(target.begin(), target.end(), target.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (target.find("/usb") != std::string::npos) return "USB";
        if (target.find("virtio") != std::string::npos) return "VirtIO";
        if (target.find("sas") != std::string::npos) return "SAS";
        if (target.find("/ata") != std::string::npos ||
            target.find("sata") != std::string::npos) {
            return "SATA";
        }
        return "SCSI";
    }
    return "Unknown";
}

}  // namespace tcmt::linux
#include <fstream>
#include <sstream>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <atomic>
#include <cstring>
#include <algorithm>
#include <map>
#include <set>
#include <utility>
#include <cctype>

DiskInfo::DiskInfo() { QueryDrives(); }

void DiskInfo::QueryDrives() {
    drives.clear();

    std::ifstream mountsFile("/proc/mounts");
    if (!mountsFile.is_open()) {
        static std::atomic<bool> logged{false};
        tcmt::linux::LogOnce(logged, "DiskInfo: /proc/mounts unreadable", true);
        return;
    }

    // Only local filesystems with a statvfs volume. Network and pseudo
    // filesystems are intentionally excluded.
    std::set<std::string> realFs = {
        "ext2", "ext3", "ext4", "xfs", "btrfs",
        "ntfs", "vfat", "fat32", "exfat", "hfsplus", "fuseblk",
        "zfs", "f2fs", "jfs", "reiserfs"
    };

    std::set<std::string> seenMounts;
    // Bind mounts repeat one filesystem at several paths (OrbStack mounts the
    // same device under / and /home/<user>). Track the device id as well so
    // the volume is listed once.
    std::set<dev_t> seenDevices;
    std::string line;
    while (std::getline(mountsFile, line)) {
        std::stringstream ss(line);
        std::string device, mountpoint, fstype, options;
        int dump = 0, pass = 0;
        ss >> device >> mountpoint >> fstype >> options >> dump >> pass;

        if (realFs.find(fstype) == realFs.end()) continue;
        if (mountpoint.find("/sys") == 0 || mountpoint.find("/proc") == 0 ||
            mountpoint.find("/dev") == 0 || mountpoint.find("/run") == 0) {
            continue;
        }
        // Bind mounts and btrfs subvolumes can repeat the same path.
        if (!seenMounts.insert(mountpoint).second) continue;

        struct stat st {};
        if (::stat(mountpoint.c_str(), &st) != 0) continue;
        if (!seenDevices.insert(st.st_dev).second) continue;

        struct statvfs vfs {};
        if (statvfs(mountpoint.c_str(), &vfs) != 0) continue;

        const uint64_t total =
            static_cast<uint64_t>(vfs.f_blocks) * static_cast<uint64_t>(vfs.f_frsize);
        // f_bavail is what an unprivileged user can actually use; f_bfree
        // includes the root-reserved blocks.
        const uint64_t freeSpace =
            static_cast<uint64_t>(vfs.f_bavail) * static_cast<uint64_t>(vfs.f_frsize);
        const uint64_t freeAll =
            static_cast<uint64_t>(vfs.f_bfree) * static_cast<uint64_t>(vfs.f_frsize);
        if (total == 0) continue;

        DriveInfo info{};
        info.totalSize = total;
        info.freeSpace = freeSpace;
        info.usedSpace = total >= freeAll ? total - freeAll : 0ULL;

        // No volume-label API without root; the mount point is the honest
        // identifier. "/" stays "/" rather than becoming "Unnamed".
        if (mountpoint == "/") {
            info.label = "/";
        } else {
            const size_t lastSlash = mountpoint.find_last_of('/');
            info.label = (lastSlash != std::string::npos && lastSlash + 1 < mountpoint.size())
                             ? mountpoint.substr(lastSlash + 1)
                             : mountpoint;
            if (info.label.empty()) info.label = mountpoint;
        }
        info.fileSystem = fstype;
        info.mountPoint = mountpoint;
        info.letter = 0;

        drives.push_back(std::move(info));
    }
}

void DiskInfo::Refresh() { QueryDrives(); }
const std::vector<DriveInfo>& DiskInfo::GetDrives() const { return drives; }

std::vector<DiskData> DiskInfo::GetDisks() {
    std::vector<DiskData> disks;
    disks.reserve(drives.size());
    for (const auto& drive : drives) {
        DiskData d;
        d.letter = drive.letter;
        d.mountPoint = drive.mountPoint;
        d.label = drive.label;
        d.fileSystem = drive.fileSystem;
        d.totalSize = drive.totalSize;
        d.usedSpace = drive.usedSpace;
        d.freeSpace = drive.freeSpace;
        disks.push_back(std::move(d));
    }
    return disks;
}

static void CopyStringToWchar(WCHAR* dst, size_t dstLen, const std::string& src) {
    size_t n = std::min(dstLen - 1, src.size());
    for (size_t i = 0; i < n; ++i)
        dst[i] = static_cast<WCHAR>(static_cast<unsigned char>(src[i]));
    dst[n] = 0;
}

void DiskInfo::CollectSmartData(SystemInfo& sysInfo) {
    Logger::Debug("DiskInfo::CollectSmartData - Linux: enumerating /sys/block");

    // Deltas from /proc/diskstats. The caller refreshes this list every 60 s,
    // so the throughput is a 60-second average; the first call reports 0.
    const std::map<std::string, tcmt::linux::DiskIoDelta> ioDeltas =
        tcmt::linux::SampleDiskStats();

    // Which whole disk backs "/"? /proc/mounts points at a partition, so a
    // prefix match against "/dev/<disk>" is enough.
    std::string rootDevice;
    {
        std::ifstream mounts("/proc/mounts");
        std::string line;
        while (std::getline(mounts, line)) {
            std::istringstream ss(line);
            std::string device, mountpoint;
            if (ss >> device >> mountpoint && mountpoint == "/") {
                rootDevice = device;
                break;
            }
        }
    }

    const std::vector<std::string> physical = tcmt::linux::ListPhysicalDisks();
    for (size_t index = 0; index < physical.size(); ++index) {
        const std::string& name = physical[index];
        const std::string base = "/sys/block/" + name;

        PhysicalDiskSmartData pd{};
        // SmartReader re-enumerates the same sorted list; the index is the
        // stable key between DiskInfo and SmartReader on Linux.
        pd.physicalIndex = static_cast<int>(index);
        pd.smartSupported = false;
        pd.smartEnabled = false;
        pd.healthPercentage = 0;  // 0 = unknown; never pretend a failed read is 100 %
        pd.temperature = 0.0;
        pd.logicalDriveCount = 0;
        pd.attributeCount = 0;

        std::string model = tcmt::linux::ReadLine(base + "/device/model");
        if (model.empty()) model = tcmt::linux::ReadLine(base + "/device/name");
        if (!model.empty()) {
            CopyStringToWchar(pd.model, sizeof(pd.model) / sizeof(WCHAR), model);
        }

        const std::string serial = tcmt::linux::ReadLine(base + "/device/serial");
        if (!serial.empty()) {
            CopyStringToWchar(pd.serialNumber, sizeof(pd.serialNumber) / sizeof(WCHAR), serial);
        }

        const std::string firmware = tcmt::linux::ReadLine(base + "/device/firmware_rev");
        if (!firmware.empty()) {
            CopyStringToWchar(pd.firmwareVersion,
                              sizeof(pd.firmwareVersion) / sizeof(WCHAR), firmware);
        }

        uint64_t sectors = 0;
        if (tcmt::linux::ReadUInt64(base + "/size", sectors)) {
            // sysfs "size" is in 512-byte sectors for every block driver.
            pd.capacity = sectors * 512ULL;
        }

        std::string diskType;
        if (name.compare(0, 4, "nvme") == 0) {
            diskType = "SSD";  // NVMe is flash by definition
        } else {
            uint64_t rotational = 0;
            if (tcmt::linux::ReadUInt64(base + "/queue/rotational", rotational)) {
                diskType = rotational == 1 ? "HDD" : "SSD";
            } else {
                diskType = "Unknown";
            }
        }
        CopyStringToWchar(pd.diskType, sizeof(pd.diskType) / sizeof(WCHAR), diskType);
        CopyStringToWchar(pd.interfaceType, sizeof(pd.interfaceType) / sizeof(WCHAR),
                          tcmt::linux::ReadDiskInterfaceType(name));

        // Temperature from NVMe hwmon or drivetemp works without privileges.
        double hwmonTemp = 0.0;
        if (tcmt::linux::ReadDiskTemperatureC(name, hwmonTemp)) {
            pd.temperature = hwmonTemp;
        }

        // SMART ioctls need root/CAP_SYS_RAWIO. Failure leaves the disk in the
        // list with smartSupported=false and health 0; it must not break the
        // list or log on every refresh.
        SmartReader::Read(static_cast<int>(index), pd);
        if (!pd.smartSupported) {
            pd.smartEnabled = false;
            pd.healthPercentage = 0;
        }
        if (pd.temperature <= 0.0 && hwmonTemp > 0.0) pd.temperature = hwmonTemp;

        const auto io = ioDeltas.find(name);
        if (io != ioDeltas.end()) {
            pd.readBytesPerSec = io->second.readBytesPerSec;
            pd.writeBytesPerSec = io->second.writeBytesPerSec;
            pd.readIops = io->second.readIops;
            pd.writeIops = io->second.writeIops;
        }

        if (!rootDevice.empty() &&
            rootDevice.compare(0, 5 + name.size(), "/dev/" + name) == 0) {
            pd.isSystemDisk = true;
        }

        sysInfo.physicalDisks.push_back(pd);
    }

    Logger::Debug("DiskInfo: found " + std::to_string(sysInfo.physicalDisks.size())
                  + " physical disks on Linux");
}

#else
#error "Unsupported platform"
#endif
