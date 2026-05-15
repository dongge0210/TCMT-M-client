#include "MemoryInfo.h"
#include "../Utils/Logger.h"

#ifdef TCMT_WINDOWS
// ======================== Windows Implementation ========================
// NOTE: winsock2.h must be included BEFORE windows.h
#include <winsock2.h>
#include <windows.h>

MemoryInfo::MemoryInfo() {
    memStatus.dwLength = sizeof(memStatus);
    GlobalMemoryStatusEx(&memStatus);
}

uint64_t MemoryInfo::GetTotalPhysical() const {
    return memStatus.ullTotalPhys;
}

uint64_t MemoryInfo::GetAvailablePhysical() const {
    return memStatus.ullAvailPhys;
}

uint64_t MemoryInfo::GetTotalVirtual() const {
    return memStatus.ullTotalVirtual;
}

#elif defined(TCMT_MACOS)
// ======================== macOS Implementation ========================
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <mach/vm_statistics.h>

MemoryInfo::MemoryInfo() : totalPhysical(0), availablePhysical(0), totalVirtual(0) {
    // Total physical memory via sysctl
    int mib[2] = {CTL_HW, HW_MEMSIZE};
    uint64_t memSize = 0;
    size_t len = sizeof(memSize);
    if (sysctl(mib, 2, &memSize, &len, nullptr, 0) == 0) {
        totalPhysical = memSize;
    } else {
        Logger::Error("MemoryInfo: sysctl HW_MEMSIZE failed");
    }

    // Available memory via host_statistics64 (free + inactive pages)
    mach_port_t host = mach_host_self();
    vm_size_t pageSize = vm_kernel_page_size;
    vm_statistics64_data_t vmStats;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;

    if (host_statistics64(host, HOST_VM_INFO64, (host_info64_t)&vmStats, &count) == KERN_SUCCESS) {
        // free + inactive = available (active + wired = used)
        uint64_t freePages = vmStats.free_count;
        uint64_t inactivePages = vmStats.inactive_count;
        availablePhysical = (freePages + inactivePages) * pageSize;
    } else {
        Logger::Error("MemoryInfo: host_statistics64 failed");
        // Fallback: approximate available as total (won't be accurate)
        availablePhysical = totalPhysical;
    }

    // Total virtual memory
    totalVirtual = totalPhysical; // macOS doesn't expose swap easily; this is safe
}

uint64_t MemoryInfo::GetTotalPhysical() const {
    return totalPhysical;
}

uint64_t MemoryInfo::GetAvailablePhysical() const {
    return availablePhysical;
}

uint64_t MemoryInfo::GetTotalVirtual() const {
    return totalVirtual;
}

#else
#error "Unsupported platform"
#endif
