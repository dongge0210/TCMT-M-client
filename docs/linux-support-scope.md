# Linux 支持 Phase 1 范围

本文件是两个并行工作流的共同约定。所有结论以仓库代码和内核文档为准，不要依赖任何会话摘要。

## 目标

1. Linux 成为可构建、可启动的 TUI 目标，不再只是脚手架。
2. 核心读数在普通用户权限下可用：CPU、内存、hwmon 温度/风扇、磁盘容量与吞吐、网络吞吐与链路、电池/AC、AMD 与 NVIDIA GPU、进程列表。
3. 需要特权的数据（CPU 封装功耗、SATA/NVMe 健康、DMI、无线扫描）允许不可用，必须优雅降级，不得伪造 0。
4. WiFi 与蓝牙在系统提供 D-Bus 服务时可用；服务不存在时返回未检测，不影响主流程。
5. 新增 Ubuntu CI 构建任务，作为唯一可执行的验证手段（本机是 macOS，无法编译 Linux 目标）。

## 非目标（Phase 2）

- IPC 共享内存发布与 Avalonia UI 对接。`main_linux.cpp` 当前只驱动 TUI，不写 IPC。
- 特权 helper / systemd 服务 / udev 规则安装器。
- Intel 核显功耗、逐进程跨用户数据、WiFi 扫描。
- 非 systemd 发行版与容器内的特殊权限模型。

## 工作流与文件归属

### A: linux-platform（构建与平台）

拥有：
- `src/CMakeLists.txt`
- `cmake/Platform.cmake`、`cmake/Dependencies.cmake`、`cmake/Options.cmake` 中与 Linux 有关的部分
- `src/main_linux.cpp`
- `src/core/wifi/WiFiInfo_linux.cpp`（新建）
- `src/core/bluetooth/BluetoothInfo_linux.cpp`（新建）
- `src/core/Platform/Platform_Linux.cpp`
- `src/core/DataStruct/SharedMemoryManager_Linux.cpp`
- `.github/workflows/build.yml`

职责：
- 移除 Linux 的 `[UNSUPPORTED]` 警告与状态标记，把 Linux 当作正式目标。
- 登记 Linux 需要的源文件；`WiFiInfo.cpp` 是 Windows WLAN 实现，Linux 不得直接编译它。
- 修复 `main_linux.cpp` 的编译问题（Windows 类型、未声明函数、平台分支）。
- WiFi：优先 NetworkManager D-Bus（`org.freedesktop.NetworkManager`），拿 SSID/BSSID/RSSI/频道/频段/速率；无 NM 时回退 `/proc/net/wireless` 的信号部分。
- 蓝牙：优先 BlueZ D-Bus（`org.bluez`），列适配器与已连接设备；无 BlueZ 时返回未检测。
- libdbus-1 必须是可选依赖：`pkg_check_modules(DBUS dbus-1)` 找到才定义 `TCMT_HAVE_DBUS`，找不到也要能编译链接。
- 新增 Ubuntu CI：安装 `cmake g++ pkg-config libncurses-dev libdbus-1-dev`（24.04 的宽字符开发包在 `libncurses-dev` 里），用 `-DTCMT_ALLOW_UNSUPPORTED_LINUX=ON` 配置并构建。

### B: linux-sensors（数据采集）

拥有：
- `src/core/cpu/CpuInfo.cpp`
- `src/core/memory/MemoryInfo.cpp`
- `src/core/gpu/GpuInfo.cpp`
- `src/core/disk/DiskInfo.cpp`、`src/core/disk/SmartReader.cpp`
- `src/core/network/NetworkAdapter.cpp`
- `src/core/power/PowerInfo.cpp`
- `src/core/temperature/TemperatureWrapper.cpp`
- `src/core/coordinator/ModuleCoordinator*.cpp`
- `src/core/process/` 下的 Linux 实现（如需新建文件，先通知 A）
- `src/core/DataStruct/DataStruct.h`
- `src/core/{cpu,disk,gpu,power,process}/` 中 `#elif defined(TCMT_LINUX)` 分支（Linux 采集实现与模块同文件）
- `src/core/Utils/Sysfs.h`（跨模块共享的 sysfs/procfs 只读辅助）

职责：
- CPU：`/proc/stat` 总占用与逐核占用；`/proc/cpuinfo` 型号/核心；`cpufreq/scaling_cur_freq` 频率；hwmon `coretemp`/`k10temp` 温度；`thermal_zone` 兜底。
- 内存：`/proc/meminfo`（`MemAvailable`）、`/proc/swaps`；RAM 类型/频率走 DMI 时不可用属正常。
- 温度/风扇：遍历 `/sys/class/hwmon`，按 `name` 分类 CPU/主板/硬盘/GPU；`fan*_input`。
- GPU：NVIDIA 用 `dlopen("libnvidia-ml.so.1")`，不需要编译期依赖；AMD 用 `/sys/class/drm/card*/device/gpu_busy_percent`、`mem_info_vram_*`、hwmon；Intel 独显用 i915/xe hwmon。
- 磁盘：`/proc/diskstats` 吞吐；容量用 `statvfs`；温度优先 NVMe hwmon 与 `drivetemp` hwmon；SMART 失败时标记 `smartSupported=false`。
- 网络：`/proc/net/dev` 吞吐；`/sys/class/net/*` 型号/MAC/速率；`ETHTOOL_GLINKSETTINGS` 取链路速率。
- 电源：`/sys/class/power_supply/BAT*` 与 `AC*/online`；容量、电压、电流、功率、循环次数按文件存在性读取。
- 进程：`/proc/<pid>/stat`、`status`、`statm`；同用户数据可用，跨用户失败就跳过。
- CPU 功耗：RAPL `energy_uj` 默认 root-only。普通用户读失败时 `powerAvailable=false`，不要用估算值填充。

### 共享文件规则

- `src/CMakeLists.txt` 只由 A 修改。B 新增文件后立刻把路径发给 A。
- `src/core/DataStruct/DataStruct.h` 只由 B 修改。A 需要新字段时先发消息。
- 任何一方要改对方的文件，先 SendMessage 说明原因，得到确认再动。
- 不执行 `git commit` / `git push`，改动留在工作区。

## 已验证的接口与权限

| 数据 | 接口 | 普通用户 |
|---|---|---|
| CPU 占用/频率 | `/proc/stat`、`cpufreq/scaling_cur_freq` | 可读 |
| CPU 温度 | `hwmon` coretemp/k10temp、`thermal_zone` | 可读 |
| 内存 | `/proc/meminfo`、`/proc/swaps` | 可读 |
| 温度/风扇 | `/sys/class/hwmon/*/temp*_input`、`fan*_input` | 可读 |
| NVIDIA GPU | NVML | 可读 |
| AMD GPU | `gpu_busy_percent`、`mem_info_vram_*`、hwmon | 可读 |
| Intel 独显 | i915/xe hwmon | 可读 |
| 磁盘吞吐 | `/proc/diskstats` | 可读 |
| NVMe/SATA 温度 | NVMe hwmon、drivetemp hwmon | 可读 |
| SATA/NVMe 健康 | SMART ioctl | 需 root/CAP，失败降级 |
| 网络吞吐 | `/proc/net/dev` | 可读 |
| 链路速率 | `ETHTOOL_GLINKSETTINGS` | 可读 |
| 电池 | `/sys/class/power_supply/BAT*` | 通常可读，父目录权限因发行版而异 |
| CPU 功耗 | `powercap/*/energy_uj` | root-only，失败降级 |
| 内存条频率/类型 | DMI | root-only，失败降级 |
| WiFi SSID | NetworkManager D-Bus | 受 polkit/会话影响 |
| 蓝牙 | BlueZ D-Bus | 受 D-Bus 策略/polkit 影响 |

更细的调用路径见 [API.md](./API.md) 的 Linux 部分。

## 约束

- C++20，沿用现有命名与日志风格。
- 不引入必须安装的新依赖；libdbus-1 与 NVML 都走可选/运行时加载。
- 不改变 macOS/Windows 分支的行为。
- 所有采集函数必须有失败路径，返回空或 false，不抛异常到主循环。
- 不要把每帧日志写进 INFO；高频失败只记一次。
- 文件控制在 800 行以内；Linux 专属实现留在所属模块的 `#elif defined(TCMT_LINUX)` 分支，通用 sysfs/procfs 辅助集中到 `src/core/Utils/Sysfs.h`。需要新增文件时先通知 A。

## 验收

1. Ubuntu 22.04 或 24.04 上 `cmake -S . -B build -DTCMT_ALLOW_UNSUPPORTED_LINUX=ON && cmake --build build -j` 成功。
2. `TCMT-M` 能启动 TUI，`--mcp` 能启动。
3. 无电池、无独显、无 hwmon 的机器上不崩溃，界面显示不可用而不是 0。
4. 不新增编译警告。
5. CI 的 Ubuntu job 通过。
