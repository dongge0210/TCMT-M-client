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
| `AppleSPUHIDDevice` | HID page `0xFF00` | MEMS accelerometer/gyro (M3+ MBP) |
