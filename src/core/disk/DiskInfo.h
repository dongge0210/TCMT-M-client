#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <map>
#include "../DataStruct/DataStruct.h"

#ifdef TCMT_WINDOWS
// winsock2.h must be before windows.h
#include <winsock2.h>
#include <windows.h>
#endif

class WmiManager;

struct DriveInfo {
    char letter;              // Windows 盘符；macOS/Linux 恒为 0（那边没有这个概念）
    std::string mountPoint;   // 平台中性的落点：Windows "C:\"，macOS "/" 或 "/Volumes/X"
    uint64_t totalSize;
    uint64_t freeSpace;
    uint64_t usedSpace;
    std::string label;        // 卷名（macOS 取 ATTR_VOL_NAME，不是挂载路径末段）
    std::string fileSystem;
};

class DiskInfo {
public:
    DiskInfo();
    const std::vector<DriveInfo>& GetDrives() const;
    void Refresh();
    std::vector<DiskData> GetDisks();

#ifdef TCMT_WINDOWS
    // readSmart=false: WMI-only enumeration (fast, never blocks). SMART reads can
    // hang on unresponsive drives, so callers can publish the list first and read
    // SMART per disk separately.
    static void CollectPhysicalDisks(WmiManager& wmi, const std::vector<DiskData>& logicalDisks,
                                     SystemInfo& sysInfo, bool readSmart = true);
    // Fast, non-blocking physical disk list: a single Win32_DiskDrive query.
    // No ASSOCIATORS mapping, no SMART reads. SMART is filled in separately
    // (per disk) by the caller via SmartReader.
    static void CollectPhysicalDiskInfo(WmiManager& wmi, SystemInfo& sysInfo);
#endif

    // Collect SMART data (DeviceIoControl on Windows, IOKit on macOS)
    static void CollectSmartData(SystemInfo& sysInfo);

private:
    void QueryDrives();
    std::vector<DriveInfo> drives;
};

#ifdef TCMT_LINUX

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace tcmt::linux {

struct DiskIoDelta {
    double seconds = 0.0;
    uint64_t readBytesPerSec = 0;
    uint64_t writeBytesPerSec = 0;
    uint64_t readIops = 0;
    uint64_t writeIops = 0;
};

// Whole physical block devices (/sys/block), sorted by name. Partitions,
// loop/ram/zram/dm/md devices are excluded.
std::vector<std::string> ListPhysicalDisks();

// Deltas against the previous call for every physical disk. Sector counts in
// /proc/diskstats are 512-byte units. Calls less than one second apart reuse
// the previous result so a burst of callers does not produce noisy deltas.
std::map<std::string, DiskIoDelta> SampleDiskStats();

// NVMe hwmon (name "nvme") or the SCSI "drivetemp" hwmon. Returns false when
// no temperature source exists for this device.
bool ReadDiskTemperatureC(const std::string& device, double& celsius);

// Human-readable transport: NVMe / SATA / SAS / USB / SD/MMC / VirtIO / Xen.
std::string ReadDiskInterfaceType(const std::string& device);

bool IsWholeDisk(const std::string& name);

}  // namespace tcmt::linux

#endif  // TCMT_LINUX
