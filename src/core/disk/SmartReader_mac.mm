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
    io_iterator_t iter;
    io_service_t found = IO_OBJECT_NULL;
    if (IORegistryEntryGetParentIterator(media, kIOServicePlane, &iter) != KERN_SUCCESS)
        return IO_OBJECT_NULL;

    io_registry_entry_t parent;
    while ((parent = IOIteratorNext(iter)) != IO_OBJECT_NULL) {
        if (IOObjectConformsTo(parent, "IONVMeController")) {
            CFTypeRef prop = IORegistryEntryCreateCFProperty(parent,
                CFSTR(kIOPropertyNVMeSMARTCapableKey), kCFAllocatorDefault, 0);
            if (prop) {
                bool ok = (CFGetTypeID(prop) == CFBooleanGetTypeID())
                    ? CFBooleanGetValue((CFBooleanRef)prop) : true;
                CFRelease(prop);
                if (ok) { found = parent; break; }
            }
        }
        IOObjectRelease(parent);
    }
    IOObjectRelease(iter);
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

    if (IOCreatePlugInInterfaceForService(nvmeCtrl, kIONVMeSMARTUserClientTypeID,
                                           kIOCFPlugInInterfaceID, &plugin, &score) == kIOReturnSuccess && plugin) {
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
                int health = 100;

                // Parse 30 attributes starting at byte 2 (each 12 bytes)
                for (int attrOff = 2; attrOff < 512 - 12; attrOff += 12) {
                    int aid = bytes[attrOff];
                    if (aid == 0 || aid == 0xFF) break;

                    // Temperature (190=Airflow, 194=Temperature)
                    if (aid == 0xBE || aid == 0xC2) {
                        for (int bo = 5; bo <= 9; bo += 4) {
                            int t = bytes[attrOff + bo];
                            if (t >= 15 && t <= 120) { out.temperature = (double)t; break; }
                        }
                        if (out.temperature == 0.0) {
                            int hi = (bytes[attrOff + 7] << 8) | bytes[attrOff + 6];
                            if (hi >= 15 && hi <= 120) out.temperature = (double)hi;
                        }
                    }

                    // Power-on hours (ID 9)
                    if (aid == 9) {
                        uint64_t hrs = 0;
                        for (int j = 0; j < 6; j++)
                            hrs |= ((uint64_t)bytes[attrOff + 5 + j] << (8 * j));
                        if (hrs < 100000000) out.powerOnHours = hrs;
                    }

                    // Wear leveling (177, 230, 233 — SSD normalized)
                    if (aid == 177 || aid == 230 || aid == 233) {
                        int cur = bytes[attrOff + 3];  // normalized current value
                        if (cur > 0 && cur <= 100)
                            out.wearLeveling = 1.0 - (cur / 100.0);
                    }

                    // Health degradation from critical attributes
                    if (aid == 5 || aid == 196 || aid == 197 || aid == 198) {
                        uint64_t rv = 0;
                        for (int j = 0; j < 6; j++)
                            rv |= ((uint64_t)bytes[attrOff + 5 + j] << (8 * j));
                        if (rv > 0) {
                            if (aid == 5) { out.reallocatedSectorCount = rv; health = (std::min)(health, 90); }
                            if (aid == 197) { out.currentPendingSector = rv; health = (std::min)(health, 85); }
                            if (aid == 198) { out.uncorrectableErrors = rv; health = (std::min)(health, 80); }
                        }
                    }
                }
                out.healthPercentage = (uint8_t)health;
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
    memset(&smartData, 0, sizeof(smartData));

    if (ReadNVMeSmart(diskIndex, smartData)) return true;
    if (ReadATASmart(diskIndex, smartData)) return true;

    return false;
}
