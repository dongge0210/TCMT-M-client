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
    char letter;
    uint64_t totalSize;
    uint64_t freeSpace;
    uint64_t usedSpace;
    std::string label;
    std::string fileSystem;
};

class DiskInfo {
public:
    DiskInfo();
    const std::vector<DriveInfo>& GetDrives() const;
    void Refresh();
    std::vector<DiskData> GetDisks();

#ifdef TCMT_WINDOWS
    static void CollectPhysicalDisks(WmiManager& wmi, const std::vector<DiskData>& logicalDisks, SystemInfo& sysInfo);
#endif

    // Collect SMART data (placeholder on macOS, uses LibreHardwareMonitor on Windows)
    static void CollectSmartData(SystemInfo& sysInfo);

private:
    void QueryDrives();
    std::vector<DriveInfo> drives;
};
