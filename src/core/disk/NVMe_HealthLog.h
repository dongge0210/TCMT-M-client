// NVMe_HealthLog.h - field offsets for the NVMe SMART / Health Information
// log page (Log Page Identifier 02h).
//
// NVM Express Base Specification, section "SMART / Health Information".
// All counters are 128-bit (16-byte) little-endian values; the low 64 bits
// hold the count, the high 64 bits are reserved. This means the standard
// Power On Hours field starts at byte 128, which matches the value confirmed
// with DiskGenius (0x37E3 = 14307 hours) on the user's NVMe drive.

#pragma once

namespace NVMeHealthLog {

// Fixed-size (1/2-byte) fields
constexpr int kCriticalWarningOffset          = 0;   // 1 byte, bit flags
constexpr int kCompositeTemperatureOffset     = 1;   // 2 bytes, Kelvin LE
constexpr int kAvailableSpareOffset           = 3;   // 1 byte, percent
constexpr int kAvailableSpareThresholdOffset  = 4;   // 1 byte, percent
constexpr int kPercentageUsedOffset           = 5;   // 1 byte, percent

// 128-bit (16-byte) counters, low 64 bits valid
constexpr int kDataUnitsReadOffset            = 32;
constexpr int kDataUnitsWrittenOffset         = 48;
constexpr int kHostReadCommandsOffset         = 64;
constexpr int kHostWriteCommandsOffset        = 80;
constexpr int kControllerBusyTimeOffset       = 96;  // minutes
constexpr int kPowerCyclesOffset              = 112;
constexpr int kPowerOnHoursOffset             = 128;
constexpr int kUnsafeShutdownsOffset          = 144;
constexpr int kMediaDataIntegrityErrorsOffset = 160;
constexpr int kErrorLogEntriesOffset          = 176;

// Total log page size
constexpr int kLogPageSize = 512;

}  // namespace NVMeHealthLog
