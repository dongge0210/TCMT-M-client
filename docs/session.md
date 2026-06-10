# Session State (2026-06-02)

## Branch
`dev`

## Goal
Add Linux build support for TCMT using OrbStack's Ubuntu VM, with TUI but without MCP.

## Current State
- **OrbStack**: Ubuntu noble x86_64 VM running, filesystem shared at same macOS paths
- **Linux build**: CMake Debug mode compiles and links successfully. Release blocked by GCC 13 LTO bug.
- **Binary works**: CPU, RAM, Disk, Network, Temperature, Power all display live data in TUI
- **WiFi**: `WiFiInfo` Linux impl reads `iw dev wl* link` - SSID/RSSI/channel/band/txRate all working
- **Bluetooth**: `BluetoothInfo` Linux impl reads `/sys/class/bluetooth/hci*` - adapter detection working
- **GPU**: AMD `gpu_busy_percent` + VRAM via sysfs/drm, Intel `gt_act_freq_mhz`, temp via hwmon

## Changes (2026-06-02)
- **WiFi+BT wired into main loop**: `main_linux.cpp` now includes `WiFiInfo.h`/`BluetoothInfo.h` and runs detection every ~3 seconds (every 6th loop iteration), populating TuiData fields
- **Compilation fixes**:
  - WinUtils.cpp added `#include <unistd.h>`
  - TimeUtils.cpp added Linux implementations for all functions
  - Logger.h/cpp added `TCMT_LINUX` guards for TUI buffer
  - DeviceChangeNotifier.h added `USB_Hub` to Linux fallback enum
  - main_linux.cpp added `#include <sys/stat.h>`
- **SharedMemoryManager_Linux.cpp**: Created with POSIX shm_open implementation

## Blocked
- **GCC 13 LTO bug**: `lto1: internal compiler error` on Release builds with `-flto`. Workaround: build Debug or add `-fno-lto`

## Build (Linux/OrbStack)
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j8
```
Release workaround (if LTO bug persists): add `-DCMAKE_INTERPROCEDURAL_OPTIMIZATION=OFF` to cmake.

## Known Constraints
- WiFi scanning via `iw dev` subprocess adds ~20ms latency per call on some systems
- Bluetooth device enumeration on Linux limited to adapter detection (full device list needs D-Bus/bluez)
- GPU AMD/Intel only; NVIDIA not tested
- LTO broken on GCC 13 (ICE in lto1)

## User Habits
- Zero warnings policy
- One change per commit
- Prefers sonnet or pro models for code changes
