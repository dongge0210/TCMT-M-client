// LinuxDisk.h — /proc/diskstats and disk hwmon helpers.
#pragma once

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
