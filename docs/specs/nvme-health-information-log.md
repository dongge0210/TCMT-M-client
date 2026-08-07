# NVMe Health Information Log Page Layout

Reference: NVM Express Base Specification, section 5.14.1.3

The Health Information Log Page is retrieved via Get Log Page command (Log ID 0x02) using `IOCTL_STORAGE_QUERY_PROPERTY` with `STORAGE_PROTOCOL_SPECIFIC_DATA`.

## Structure Layout

| Offset | Size | Field |
|--------|------|-------|
| 0 | 1 byte | Critical Warning |
| 1-2 | 2 bytes | Temperature (Kelvin, int16) |
| 3 | 1 byte | Available Spare (%) |
| 4 | 1 byte | Available Spare Threshold (%) |
| 5 | 1 byte | Percentage Used (%) |
| 6-7 | 2 bytes | Data Units Read [0:15] |
| 8-15 | 8 bytes | Data Units Read (total, in 512-byte units × 1000) |
| 16-23 | 8 bytes | Data Units Written |
| 24-31 | 8 bytes | Host Read Commands |
| 32-39 | 8 bytes | Host Write Commands |
| 40-47 | 8 bytes | Controller Busy Time (minutes) |
| 48-55 | 8 bytes | Power Cycles |
| 56-63 | 8 bytes | Power On Hours |
| 64-71 | 8 bytes | Unsafe Shutdowns |
| 72-79 | 8 bytes | Media and Data Integrity Errors |
| 80-87 | 8 bytes | Number of Error Information Log Entries |
| 88-95 | 8 bytes | Warning Composite Temperature Time |
| 96-103 | 8 bytes | Critical Composite Temperature Time |
| 104-107 | 4 bytes | Temperature Sensor 1 (Kelvin) |
| 108-111 | 4 bytes | Temperature Sensor 2 |
| 112-115 | 4 bytes | Temperature Sensor 3 |
| 116-119 | 4 bytes | Temperature Sensor 4 |
| 120-123 | 4 bytes | Temperature Sensor 5 |
| 124-127 | 4 bytes | Temperature Sensor 6 |
| 128-255 | 128 bytes | Reserved |

**NOTE**: This is the NVM Express base spec layout. Some vendors may differ.
The layout checked against DiskGenius output shows Power On Hours at **byte 128** (0x37E3 = 14307).

## Recommended Reading Function

Always read at least 128 bytes beyond offset 5 to get all standard fields up to Temperature Sensor. Power On Hours at offset 128 requires reading 136 bytes total.
