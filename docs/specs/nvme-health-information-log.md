# NVMe Health Information Log Page Layout

Reference: NVM Express Base Specification, "SMART / Health Information" log page (Log Page Identifier 02h).

The Health Information Log Page is retrieved via Get Log Page command (Log ID 0x02) using `IOCTL_STORAGE_QUERY_PROPERTY` with `STORAGE_PROTOCOL_SPECIFIC_DATA`.

## Structure Layout

All counters are 128-bit (16-byte) little-endian values; the low 64 bits hold the count, the high 64 bits are reserved.

| Offset | Size | Field |
|--------|------|-------|
| 0 | 1 byte | Critical Warning |
| 1-2 | 2 bytes | Composite Temperature (Kelvin, uint16 LE) |
| 3 | 1 byte | Available Spare (%) |
| 4 | 1 byte | Available Spare Threshold (%) |
| 5 | 1 byte | Percentage Used (%) |
| 6-31 | 26 bytes | Reserved |
| 32-47 | 16 bytes | Data Units Read (512-byte units, in thousands) |
| 48-63 | 16 bytes | Data Units Written (512-byte units, in thousands) |
| 64-79 | 16 bytes | Host Read Commands |
| 80-95 | 16 bytes | Host Write Commands |
| 96-111 | 16 bytes | Controller Busy Time (minutes) |
| 112-127 | 16 bytes | Power Cycles |
| 128-143 | 16 bytes | Power On Hours |
| 144-159 | 16 bytes | Unsafe Shutdowns |
| 160-175 | 16 bytes | Media and Data Integrity Errors |
| 176-191 | 16 bytes | Number of Error Information Log Entries |
| 192-207 | 16 bytes | Warning Composite Temperature Time (minutes) |
| 208-223 | 16 bytes | Critical Composite Temperature Time (minutes) |
| 224-255 | 32 bytes | Temperature Sensor 1-8 (4 bytes each, Kelvin) |
| 256-511 | 256 bytes | Vendor Specific |

**Confirmed on hardware**: DiskGenius output shows Power On Hours at **byte 128** (0x37E3 = 14307 hours), which matches the standard 128-bit layout above. Byte 56 is *Host Write Commands*, not Power On Hours.

## Recommended Reading Function

Read at least 192 bytes (through Error Information Log Entries at offset 176) so every standard counter is covered. Field offsets are exposed as `constexpr` in `src/core/disk/NVMe_HealthLog.h`.