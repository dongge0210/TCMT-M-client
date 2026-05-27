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
void DiskInfo::CollectPhysicalDisks(WmiManager& wmi, const std::vector<DiskData>& logicalDisks, SystemInfo& sysInfo) {
    IWbemServices* svc = wmi.GetWmiService();
    if (!svc) { Logger::Warn("WMI service invalid, skipping physical disk enumeration"); return; }
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
                    wcsncpy_s(data.diskType, L"Unknown", _TRUNCATE);
                }
                data.smartSupported = false;
                data.smartEnabled = false;
                data.healthPercentage = 100;
                data.temperature = -1;
                data.logicalDriveCount = 0;
                tempDisks[idx] = data;
                // Try DeviceIoControl SMART
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
    Logger::Debug("DiskInfo::CollectSmartData - using LibreHardwareMonitor");
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

    // Track physical disks to deduplicate APFS volumes on same device
    std::set<std::string> seenPhysicalDisks;

    for (const auto& fs : mounts) {
        std::string mountpoint(fs.f_mntonname);
        std::string fstype(fs.f_fstypename);
        std::string devname(fs.f_mntfromname);

        // Skip non-physical volumes, root, and macOS system snapshots
        if (fstype == "autofs" || fstype == "devfs" || fstype == "fdesc"
            || fstype == "procfs" || fstype == "devpts" || fstype == "overlay"
            || fstype == "nullfs" || fstype == "simfs"
            || mountpoint == "/"  // root is not a user-facing volume
            || mountpoint == "/System/Volumes/VM"
            || mountpoint == "/System/Volumes/Preboot"
            || mountpoint == "/System/Volumes/Update"
            || mountpoint == "/System/Volumes/xarts"
            || mountpoint == "/System/Volumes/iSCPreboot"
            || mountpoint == "/System/Volumes/Hardware"
            || mountpoint.find("/System/Volumes/Update/") == 0
            || mountpoint.find("/private/var/folders/") == 0
            || mountpoint.find("/Volumes/ProNTFSDrive") == 0) {
            continue;
        }

        // Get BSD name (e.g. disk0s1)
        std::string bsdName = devname;
        if (bsdName.find("/dev/") == 0)
            bsdName = bsdName.substr(5);

        // Get capacity and free space
        uint64_t total = (uint64_t)fs.f_blocks * (uint64_t)fs.f_bsize;
        uint64_t free = (uint64_t)fs.f_bfree * (uint64_t)fs.f_bsize;
        uint64_t used = total - free;

        // Only include significant volumes (skip small system volumes)
        if (total < 100 * 1024 * 1024) continue; // skip < 100MB

        // Skip boot partition
        if (bsdName.find("s1") != std::string::npos && mountpoint == "/") {
            // Root volume
        }

        DriveInfo info{};
        info.totalSize = total;
        info.freeSpace = free;
        info.usedSpace = used;
        info.fileSystem = fstype;

        // Volume name from last path component
        size_t lastSlash = mountpoint.find_last_of('/');
        info.label = (lastSlash != std::string::npos && lastSlash + 1 < mountpoint.size())
                     ? mountpoint.substr(lastSlash + 1) : mountpoint;
        if (info.label.empty()) info.label = "Untitled";

        // Derive letter: use '/' for root, 'A'+index for others
        if (mountpoint == "/") {
            info.letter = '/'; // root indicator
        } else {
            info.letter = 0; // no drive letter on macOS
        }

        // Deduplicate: extract physical disk name (e.g. "disk3s1s1" -> "disk3")
        // Skip if we've already seen this physical disk
        std::string physicalDisk = bsdName;
        // Remove partition suffixes: keep only up to first 's' after "diskN"
        auto diskPrefixEnd = physicalDisk.find('s', 4); // skip "disk"
        if (diskPrefixEnd != std::string::npos) {
            physicalDisk = physicalDisk.substr(0, diskPrefixEnd);
        }
        if (!seenPhysicalDisks.insert(physicalDisk).second) {
            // Already have a volume from this physical disk
            Logger::Debug("DiskInfo: skipping duplicate APFS volume " + mountpoint
                        + " on " + physicalDisk);
            continue;
        }

        drives.push_back(std::move(info));
        Logger::Debug("DiskInfo: found volume " + mountpoint
                    + " (" + fstype + ") size=" + std::to_string(total));
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
    if (!fp) { fp = popen("system_profiler SPSerialATADataType -json 2>/dev/null", "r"); if (!fp) { Logger::Info("SMART: popen failed"); return; } }

    std::string json;
    char buf[4096];
    while (fgets(buf, sizeof(buf), fp)) json += buf;
    int rc = pclose(fp);
    if (rc != 0 || json.empty()) {
        Logger::Info(std::string("SMART: pclose rc=") + std::to_string(rc) + " len=" + std::to_string(json.size()));
        return;
    }

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

#else
#error "Unsupported platform"
#endif
