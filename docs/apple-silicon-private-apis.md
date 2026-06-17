# Apple Silicon Private APIs for Hardware Monitoring

> 2026-05-21 research. Sources: macmon, asitop, Stats, NeoAsitop, socpowerbud, mactop, kperfdata.rs (darwin-kperf-sys).

## Already Implemented

| API | How | Sudo? |
|-----|-----|-------|
| IOReport (Energy Model + CPU Stats + GPU Stats) | `dlopen(/usr/lib/libIOReport.dylib)`, `IOReportCreateSubscription`, `IOReportCreateSamplesDelta` | No |
| IOKit pmgr DVFS tables | `IOServiceNameMatching("pmgr")` → `voltage-states*-sram` | No |
| IOHIDEventSystemClient (temperature) | `kIOHIDEventTypeTemperature` (15), usage page `0xff00`, usage `0x0005` | No |
| SMC via `IOServiceOpen` | `IOServiceOpen(device, mach_task_self(), 0, &conn)` → existing key read | No |
| `host_statistics64` | `mach_host_self()`, `HOST_VM_INFO64` | No |
| `sysctl hw.memsize` / `hw.model` / `kern.osproductversion` | Standard sysctl | No |

## Available But Not Yet Used (No Sudo)

### SMC Keys

| Key | Data | Source |
|-----|------|--------|
| `PSTR` | Total system power (Watts) | macmon, Stats |
| `PDBR` | Display backlight power (Watts) | macpow |
| `PDTR` | Power adapter delivery (Watts) | macpow |
| `wiPm` | WiFi module power (Watts) | macpow |
| `VD0R` / `ID0R` | AC voltage / current | iSMC |
| `F0Ac` / `FNum` / `F0Mn` / `F0Mx` | Fan RPM / count / min / max | Stats |
| `F{id}ID` | Fan name | Stats |

**Per-generation curated SMC temperature keys** (Stats readers.swift):
- M1: `Tp09`, `Tp0T`, `Tp01`, `Tp05`, `Tp0D`, `Tp0H`, `Tp0L`, `Tp0P`, `Tp0X`, `Tp0b`
- M2: `Tp1h`, `Tp1t`, `Tp1p`, `Tp1l`, `Tp01`, `Tp05`, `Tp09`, `Tp0D`, `Tp0X`, `Tp0b`, `Tp0f`, `Tp0j`
- M3: `Te05`, `Te0L`, `Te0P`, `Te0S`, `Tf04`, `Tf09`, `Tf0A`, `Tf0B`, `Tf0D`, `Tf0E`
- M4: `Te05`, `Te09`, `Te0H`, `Te0S`, `Tp01`, `Tp05`, `Tp09`, `Tp0D`, `Tp0V`, `Tp0Y`
- M5: `Tp00`, `Tp04`, `Tp08`, `Tp0C`, `Tp0G`, `Tp0K`, `Tp0O`, `Tp0R`, `Tp0U`, `Tp0X`

### IOReport Channels (Subscribe via `IOReportCopyChannelsInGroup`)

| Group | What | Source |
|-------|------|--------|
| `AMC Stats` | Memory controller bandwidth: PCPU/ECPU/GFX/MEDIA DCS RD/WR | NeoAsitop |
| `CLPC Stats` | CPU limit/power controller data | NeoAsitop |
| `PMP` | Power Management Processor telemetry | NeoAsitop |
| `DISP` / `DISPEXT` | Display controller power | socpowerbud |

### IOHID Power Sensors

Same path as temperature but with `kIOHIDEventTypePower = 25`:
- Usage page `0xff08`, usage `0x0002` = Current sensors
- Usage page `0xff08`, usage `0x0003` = Voltage sensors

### Mach / sysctl

| API | Data | Source |
|-----|------|--------|
| `host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO, ...)` | Per-core CPU utilization | Stats |
| `sysctl([CTL_VM, VM_SWAPUSAGE], &xsw)` | Swap total/used | macmon |
| `NSProcessInfo.thermalState` | 4-level thermal state + notification | macOS SDK |

## Require Root / Entitlement

| API | Requirement | What |
|-----|-------------|------|
| `kperf.framework` (`kpc_get_cpu_counters`, `kpep_db_create`) | `com.apple.private.kernel.kpc` | 10 PMC hardware counters (cycles, instructions, cache, branch) |
| KPEP event DB | `/usr/share/kpep/{a14,a15,as1,as3,as4}.plist` | CPU µarch event name mapping |
| `proc_pid_rusage(RUSAGE_INFO_V4)` for other processes | root | Per-process `ri_billed_energy` (nJ) |
| `powermetrics` | root (but we replaced with IOReport) | Legacy fallback |

## IOKit Services (Exploratory)

| Service | `ioreg` Search | Data |
|---------|---------------|------|
| `AppleARMPE` | `IOService:/AppleARMPE` | Product description |
| `IOPMrootDomain` | `IOServiceMatching("IOPMrootDomain")` | Clamshell, power management |
| `AppleARMThermalSensor` | `ioreg -l -r -c AppleARMThermalSensor` | Multi-point temps |
| `AppleT8103ThermalZone` (M1) / `AppleT8112ThermalZone` (M2) | IORegistry | SoC thermal zones |
| `AppleSPUHIDDevice` | HID page `0xFF00` | MEMS accelerometer/gyro/ALS/lid (M2 MacBook Air+ via SpsManager) |

## Apple SPU HID Sensors — Full Decode Reference

> Discovered 2026-05-29 via `sensor_recorder` tool on MacBook Air M2 (BMI284 IMU).
> Manager: `src/core/accel/SpsManager.mm` — async `IOHIDDeviceRegisterInputReportWithTimeStampCallback`.

### Enumeration

8 HID devices under `IOHIDDevice` with conforms-to `AppleSPUHIDDriver`:

| Usage Page | Usage | Sensor Type | Report Len | SpsType Enum |
|-----------|-------|-------------|-----------|-------------|
| `0xFF00` | 3 | Gravity / Attitude | 22 B | `Gravity` |
| `0xFF00` | 4 | Ambient Light (ALS) | 122 B | `ALS` |
| `0xFF00` | 5 | BMI284 Die Temperature | 14 B | `Temp` |
| `0xFF00` | 9 | Gyroscope | 22 B | `Gyro` |
| `0xFF00` | 255 (0xFF) | Top-level device | 1 B | (device, not data) |
| `0x0020` | 138 | Lid Angle | 8 B (3 used) | `LidAngle` |
| `0xFF0C` | 1 | Motion Heartbeat | 5 B | `ApplePriv1` |
| `0xFF0C` | 5 | DeviceMotion6 Fusion | 100 B | `ApplePriv5` |

### Sensor Format Details

#### Gravity / Attitude (0xFF00/3) — 22 bytes

```
Offset  Type    Description
────────────────────────────────────
0-1     uint8   seq, frameCnt
2-21    3×int32 X, Y, Z in Q16 fixed-point
```

- **Scale**: 1 Q16 = 1/65536 g
- **Modulus**: √(x²+y²+z²) ≈ 1.0 g (verified at rest: ~0.9995—1.002)
- **Dead zone**: X/Y < 0.015 → 0.0 (flat-on-table idle noise)
- **Z is ~1g** at rest (handbook-up orientation)

#### Gyroscope (0xFF00/9) — 22 bytes

```
Offset  Type    Description
────────────────────────────────────
0-1     uint8   seq, frameCnt
2-21    3×int32 X, Y, Z in Q16 fixed-point
```

- **Scale**: 1 Q16 = 1/65536 °/s
- **LSB**: ±2000°/s range / 32768 = 0.061 °/s per count (BMI284)
- **Dead zone**: |value| < 0.1 → 0.0 (covers ±1 LSB quantization noise)
- **At rest**: all three axes ≈ 0.0

#### ALS (Ambient Light Sensor) (0xFF00/4) — 122 bytes

Decoded format (verified via sensor_recorder 15s capture with varying light levels):

```
Offset  Type    Description
───────────────────────────────────────
0-3     —       Report header (byte0=0xEC, byte1=seq, byte2=0x04, byte3=0x00)
4-7     uint32  Microsecond timestamp (rapidly varying)
8-11    uint32  HID event counter (shared with Temp sensor — same value at same t)
12-19   —       Padding / reserved (zeros)
20-23   uint32  Raw channel 0 (uint16 stored as LE uint32) — likely Red
24-27   uint32  Raw channel 1 (uint16 stored as LE uint32) — likely Green
28-31   uint32  Raw channel 2 (uint16 stored as LE uint32) — likely Blue
32-35   uint32  Raw channel 3 (uint16 stored as LE uint32) — likely Clear
36-39   uint32  Raw channel 4 (sum / extra channel)
40-43   float32 IEEE 754, LE — **Calibrated lux value**
44-67   —       Padding / reserved (zeros)
68-...  —       Extended metadata / flicker detection (partially decoded)
```

- **Lux extraction**: bytes 40-43 as `float32` LE → lux
- **Verified range**: 142–545 lux (dim lighting room) — tracks light changes correctly
- **Raw channels (offset 20-35)**: correlate with RGBC. All 4 channels increase/decrease proportionally with light, ratios shift slightly with color temperature changes. Upper 16 bits are always zero — effectively uint16 per channel.
- **HID event counter at offset 8**: same as Temp sensor at offset 8 — shared global counter
- **Timing**: reports arrive at ~100Hz, independent of main loop cadence
- **Previously**: stored as raw first 4 bytes (meaningless). Now decoded in SpsManager with lux + RGBC display in TUI Sensors panel.

#### BMI284 Die Temperature (0xFF00/5) — 14 bytes

```
Offset  Type    Description
───────────────────────────────────────
0       uint8   ReportID (0x05)
1       uint8   Sequence number
2       uint8   Frame counter
3-4     int16   Temperature, LE, Q8.8 fixed-point
5-7     uint8   Padding / reserved
8-11    uint32  HID event counter (shared — same value seen in ALS reports at this offset)
12-13   uint8   Padding
```

- **Scale**: Q8.8 → value/256 °C
- **Typical idle**: 0x1E80 = 30.5 °C (IMU die, not CPU/GPU)
- **HID event counter at offset 8**: confirmed by cross-referencing identical values in ALS reports at same offset — NOT a temperature field
- **Decoded**: bytes 3-4 as LE int16

#### Lid Angle (0x0020/138) — 8 bytes (3 used)

```
Offset  Type    Description
────────────────────────────────────
0       uint8   ReportID (0x01)
1-2     uint16  Lid angle, LE, bottom 9 bits valid
3-7     (unused)
```

- **Range**: 0—511 counts → scaled to 0—360°
- **0°** = closed lid, **~130°** = typical laptop open position
- **Bottom 9 bits mask**: `raw & 0x1FF`

#### Motion Heartbeat — AppleVendorMotion (0xFF0C/1) — 5 bytes

```
Offset  Type    Description
────────────────────────────────────
0       uint8   Event flag (0x03=pair start, 0x02=pair end, 0x50=init burst)
1       uint8   Event sub-type
2       uint8   Data / reserved
3       uint8   Padding (usually 0x00)
4       uint8   Monotonic heartbeat counter
```

- **Apple HID usage**: `kHIDUsage_AppleVendorMotion_Motion` (page `kHIDPage_AppleVendorMotion = 0xFF0C`, usage 1)
- **Period**: ~2.54 seconds (0.39 Hz), **independent of physical motion**
- **Pair pattern**: each heartbeat fires 2 reports in quick succession:
  1. `03 02 00 00 04` (event type 0x03, subtype 0x02)
  2. `02 01 02 00 NN` (event type 0x02) where NN increments: 0x57 → 0x58 → ...
- **Status check**: counter increments monotonically → SPU fusion pipeline is alive; if counter stalls → SPU may have crashed

#### DeviceMotion6 Fusion — AppleVendorMotion (0xFF0C/5) — 100 bytes

```
Offset  Type    Description
─────────────────────────────────────
0-3     int32   Raw fusion data (first 4 bytes)
4-99    —       96 bytes, format unknown
```

- **Apple HID usage**: `kHIDUsage_AppleVendorMotion_DeviceMotion6` (page 0xFF0C, usage 5)
- **Lazy pipeline**: report **never fires** without an active CoreMotion consumer (e.g. `CMMotionManager startDeviceMotionUpdates`). The SPU fusion engine powers down when no client is connected.
- **Not captured** by `sensor_recorder` because no CoreMotion consumer was active during recording
- **100-byte report** likely contains quaternion + gravity + acceleration + rotation rate from CMDeviceMotion

### SpsManager Architecture

```
┌─────────────────────────────────────────────────┐
│  main_mac.cpp (main loop, ~500ms)              │
│  s_sps.Refresh() → atomic loads → TuiApp data  │
└─────────────────────┬───────────────────────────┘
                      │
┌─────────────────────▼───────────────────────────┐
│  SpsManager (background CFRunLoop thread)       │
│  ┌───────────────────────────────────────────┐  │
│  │  Enumerate: IOServiceMatching("AppleSPU-  │  │
│  │  HIDDriver") → open all                   │  │
│  ├───────────────────────────────────────────┤  │
│  │  Callback: HidCallback() per sensor       │  │
│  │  → atomic store (v1/v2/v3/sv/updateCount) │  │
│  ├───────────────────────────────────────────┤  │
│  │  Refresh(): atomic load → decode → sample │  │
│  │  (3-axis → sample3, scalar → sample1)    │  │
│  └───────────────────────────────────────────┘  │
└─────────────────────────────────────────────────┘
```

- **No root required**: async HID input report callback works from user-space on macOS 15+ (Apple Silicon)
- **No SMJobBless helper**: replaced the old SMJobBless+SHM approach
- **Thread-safe**: atomic stores from callback thread, atomic loads from main loop thread
- **Dead zones**: applied at Refresh() level so all consumers get clean data

### Key Discoveries

1. **Temperature offset**: bytes 3-4 (not 8), LE int16 Q8.8. Bytes 8-11 are a shared HID event counter (same value appears in ALS reports at same offset).
2. **Motion Heartbeat (0xFF0C/1)**: not a motion data sensor — it's a ~0.4Hz pipeline liveliness indicator. Counter at byte 4 increments every 2.54s regardless of device motion.
3. **DeviceMotion6 (0xFF0C/5)**: requires CoreMotion activity. SPU fusion engine is lazy — if no CM consumer is registered, the sensor never fires.
4. **IMU die vs ambient**: the 0xFF00/5 temperature is the BMI284 chip itself (~30°C idle), not CPU/GPU or case temperature. Useful as a proxy for overall chassis heat soak.
5. **ALS lux at offset 40-43**: the calibrated lux value is at bytes 40-43 as `float32 LE`, not in the first 4 bytes. Previous code storing `report[0..3]` as raw int32 captured only the report header — the lux value was 40 bytes deep in the 122-byte report.
6. **ALS raw channels at offset 20-35**: four uint32 LE values, effectively uint16 (upper 16 bits always zero). These are raw RGBC ADC counts from the VD6286 sensor. They correlate with light level and can feel color temperature shifts via R/G and B/G ratios.
