# Linux 端功能差距分析

## P0 — 编译 Bug

| 问题 | 详情 |
|---|---|
| **WiFiInfo.cpp 未编译** | `CMakeLists.txt` 只编 `WiFiInfo.mm` (macOS)。`WiFiInfo.cpp` 有完整 Linux 实现（iw 解析），但从未被加入构建。`main_linux.cpp` 第 587-589 行直接调用 `s_wifi.Detect()` → **链接会炸** |

> 修复：`CMakeLists.txt` 加 `$<$<BOOL:${TCMT_LINUX}>:core/wifi/WiFiInfo.cpp>`

---

## P1 — 功能缺失（有跨平台数据源、易补齐）

| 模块 | macOS 现状 | Linux 可对标数据源 |
|---|---|---|
| **IPCServer** | Unix domain socket，`.cpp` 已跨平台 | 同一份代码，CMake 只编 macOS，解开即可 |
| **BatteryHealth** | `.mm` AppleSmartBattery | `/sys/class/power_supply/BAT0/` — cycle_count, energy_full_design |
| **DisplayInfo** | `.mm` CGDisplay | `/sys/class/drm/card*/modes`、edid-decode 或 xrandr |
| **ProcessTop** | `.mm` libproc | `/proc/*/stat` 简单 top-N CPU |
| **config refreshRate** | `cfg.GetInt("display.refreshRate")` | Linux 主循环完全不读这个配置 |

---

## P2 — 低优先级

| 模块 | 说明 |
|---|---|
| **ALSensor** | `/sys/class/backlight/*/brightness` 亮度可读；`/sys/bus/iio/` 少数设备有 ALS，覆盖面小 |
| **Thermal zones** | `/sys/class/thermal/thermal_zone*/temp` 可聚合，无统一"thermal state" |
| **DeviceChangeNotifier** | 当前是 "always true" 轮询回退。可用 udev (`libudev` 或 `netlink`) 实现事件驱动 |
| **CPU package power** | Intel RAPL `/sys/class/powercap/intel-rapl:0/energy_uj`，对应 `cpuPower` 字段 |

---

## Linux vs macOS 功能对照表

| 功能 | macOS | Linux 现状 | 差距 |
|---|---|---|---|
| CPU 采样 | ✓ | ✓ | — |
| GPU 采样 | ✓ | ✓ | — |
| Memory | ✓ | ✓ | — |
| Disk SMART | ✓ SmartReader_mac.mm | ✓ SmartReader.cpp | — |
| Disk Volumes | ✓ | ✓ | — |
| Network RX/TX | ✓ | ✓ | — |
| OS Info | ✓ | ✓ | — |
| Power/AC | ✓ IOKit | ✓ /sys | — |
| Temperature | ✓ SMC+HID | ✓ hwmon | — |
| **WiFi** | ✓ CoreWLAN | ❌ 代码在，没编译 | P0 |
| Bluetooth | ✓ IOBluetooth | ✓ sysfs (已编译) | — |
| USB | ✓ | ✓ | — |
| **IPCServer** | ✓ | ❌ CMake 未启用 | P1 |
| **BatteryHealth** | ✓ | ❌ 缺实现 | P1 |
| **DisplayInfo** | ✓ CGDisplay | ❌ 缺实现 | P1 |
| **ProcessTop** | ✓ libproc | ❌ 缺实现 | P1 |
| ALS/亮度 | ✓ | ❌ 缺实现 | P2 |
| Thermal State | ✓ | ❌ 缺实现 | P2 |
| SPU 传感器 | ✓ Apple HID | N/A (Apple 专用) | — |
| IOReport 功耗 | ✓ | N/A (Apple 专用) | — |
| DeviceChangeNotifier | ✓ IOKit event | ⚠️ 始终 true (轮询) | P2 |
