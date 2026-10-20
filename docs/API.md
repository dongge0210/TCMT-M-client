# TCMT-M 系统与硬件 API 清单

> 源码核对日期：2026-10-20

## 范围与口径

- 本文记录 `src/core` 中读取操作系统或硬件信息的接口及调用位置，不是所有可用 API 的列表。
- 覆盖 CPU、内存、温度、风扇、功耗、GPU、磁盘、网络、Wi-Fi、蓝牙、电池、显示、USB 和进程。
- TCMT 自身的 IPC、MCP、运动传感器 HTTP 接口不在本文范围内。
- API 是否可用取决于平台、硬件、驱动、权限及系统版本；缺少可选能力时，部分读数可能不可用。
- Linux 是 CMake 支持目标。Linux 相关实现按 `TCMT_LINUX` 编译；设备驱动和权限仍会影响实际读数。
- 此清单按当前源码整理；改动采集实现后应同步核对本文。

## macOS

### 1. 系统标识与机型

调用位置：[OSInfo.cpp](../src/core/os/OSInfo.cpp)、[BoardInfo.cpp](../src/core/board/BoardInfo.cpp)、[CpuInfo.cpp](../src/core/cpu/CpuInfo.cpp)、[main_mac.cpp](../src/main_mac.cpp)。

- `sysctlbyname`：`kern.osproductversion`、`kern.osrelease`、`hw.model`、`machdep.cpu.vendor`、`machdep.cpu.brand_string`、`hw.machine`、`hw.hypervisor`、`hw.memsize`。
- IOKit：`IOServiceGetMatchingService`、`IORegistryEntryCreateCFProperty("model")`，读取主板或整机型号。
- `kern.boottime`：读取启动时间并计算运行时长。

### 2. CPU 占用、核心数与频率

调用位置：[CpuInfo.cpp](../src/core/cpu/CpuInfo.cpp)、[ModuleCoordinator_cpu.cpp](../src/core/coordinator/ModuleCoordinator_cpu.cpp)。

- Mach：`mach_host_self`、`host_statistics`（`HOST_CPU_LOAD_INFO`）、`host_processor_info`（`PROCESSOR_CPU_LOAD_INFO`），读取 CPU 及逐核负载。
- `sysctlbyname`：`hw.physicalcpu`、`hw.logicalcpu`、`hw.performancecores`、`hw.perflevel0.logicalcpu`、`hw.perflevel1.logicalcpu`、`hw.cpufrequency`、`hw.cpufrequency_min`、`hw.cpufrequency_max`。

### 3. 内存与 Swap

调用位置：[MemoryInfo.cpp](../src/core/memory/MemoryInfo.cpp)、[ModuleCoordinator_cpu.cpp](../src/core/coordinator/ModuleCoordinator_cpu.cpp)。

- `host_statistics64`（`HOST_VM_INFO64` / `vm_statistics64`）：物理内存、可用内存、压缩内存。
- `sysctlbyname("hw.memsize")`、`sysctlbyname("vm.swapusage")`：物理内存总量与 Swap。

### 4. 温度

调用位置：[TemperatureWrapper.cpp](../src/core/temperature/TemperatureWrapper.cpp)、[tcmt_sensor_helper.c](../src/helper/tcmt_sensor_helper.c)。

- AppleSMC：`IOServiceMatching("AppleSMC")`、`IOServiceOpen`、`IOConnectCallStructMethod` 选择子 5，按 SMC 键读取温度。键覆盖 CPU、GPU、内存、电池、主板、SSD 和环境传感器；实际可读键因机型而异。
- IOKit HID：`IOHIDEventService`、`IORegistryEntryCreateCFProperty("Temperature")`，作为部分温度读数的补充来源。
- `powermetrics` 子进程：使用 `--samplers thermal -i N -n 1` 获取补充温度；由后台线程缓存，读取通常需要提升权限。
- 特权 helper 提供 AppleSMC 读取路径。

### 5. 风扇

调用位置：[FanSpeed.mm](../src/core/fan/FanSpeed.mm)。

- Intel Mac：AppleSMC `IOServiceMatching`、`IOServiceOpen`、`IOConnectCallStructMethod`，读取 `FNum` 和 `F{n}Ac`。
- 无风扇的 MacBook Air 机型本身没有可观测风扇转速；这是具体机型的硬件限制，不能据此推断所有 MacBook Air 或 Apple silicon Mac 都无风扇。当前代码只在 Intel Mac 上尝试 AppleSMC 转速读取；Apple silicon 的其他有风扇机型尚无 RPM 读取实现，返回空列表。不能将 IOReport 功耗/频率读取当作风扇数据来源。

### 6. 功耗与频率

调用位置：[PowerMonitor_mac.mm](../src/core/power/PowerMonitor_mac.mm)、[TemperatureWrapper.cpp](../src/core/temperature/TemperatureWrapper.cpp)。

- Apple Silicon 功耗和频率优先使用私有 IOReport 框架：运行时 `dlopen("/usr/lib/libIOReport.dylib")` / `dlsym` 加载 `IOReportCopyChannelsInGroup`、`IOReportCreateSubscription`、`IOReportCreateSamples`、`IOReportCreateSamplesDelta` 等函数，读取 `"Energy Model"`、`"CPU Stats"`、`"GPU Stats"` 通道及状态驻留。
- 通过 IOKit `pmgr` 属性读取 DVFS 频率表。较新 macOS 版本可能限制普通进程读取 IOReport 能耗数据；实现会回退到 `powermetrics`，macOS 27+ 的受限情形还可由 `tcmt-powerd` 提供数据。
- CPU/GPU/ANE 功耗补充路径也会使用 `powermetrics`。
- CPU 频率另读取 `hw.cpufrequency` 等 sysctl 项。

### 7. GPU

调用位置：[GpuInfo.cpp](../src/core/gpu/GpuInfo.cpp)。

- IOKit 枚举 `IOAccelerator` 服务；没有结果时回退到 `IOGraphics`。
- `IORegistryEntryCreateCFProperty` 读取 `"model"`、`"IOName"`、`"VRAM,mb"`、`"VRAM"` 和 `"PerformanceStatistics"` 中的 `"Device Utilization %"`。
- `hw.machine`、`hw.memsize` 用于识别 Apple Silicon 和估算统一内存。
- `DetectGpusViaMetal` 是历史函数名；当前枚举路径使用 IOKit，并未调用 Metal API。

### 8. 磁盘与 SMART

调用位置：[DiskInfo.cpp](../src/core/disk/DiskInfo.cpp)、[SmartReader_mac.mm](../src/core/disk/SmartReader_mac.mm)。

- `statfs`、`getfsstat`、`readdir`：卷容量、挂载点及目录枚举。
- IOKit：`IOServiceGetMatchingService`、`IORegistryEntryGetParentIterator`，定位物理磁盘并读取可用的 SMART 信息。

### 9. 网络与 Wi-Fi

调用位置：[NetworkAdapter.cpp](../src/core/network/NetworkAdapter.cpp)、[WiFiInfo.mm](../src/core/wifi/WiFiInfo.mm)。

- `getifaddrs`、`sysctl`：网络接口、地址、路由及统计信息。
- CoreWLAN：`CWWiFiClient`、`CWInterface`、`CWChannel`、`CWSecurity`，读取 SSID、BSSID、RSSI、噪声、信道和速率。
- CoreLocation：`CLLocationManager`，处理读取 Wi-Fi 名称所需的位置授权。
- 获取 SSID 需要从 `.app` 包内启动；直接运行裸二进制可能拿不到 SSID。

### 10. 蓝牙

调用位置：[BluetoothInfo.mm](../src/core/bluetooth/BluetoothInfo.mm)。

- `IOBluetoothHostController`：蓝牙控制器状态。
- `IOBluetoothDevice` / `pairedDevices`：已配对设备及其连接状态。
- 周期采集不调用 `IOBluetoothDeviceInquiry`，不会主动扫描附近设备。

### 11. 电池与电源

调用位置：[PowerInfo.cpp](../src/core/power/PowerInfo.cpp)、[BatteryHealth.mm](../src/core/battery/BatteryHealth.mm)。

- IOPowerSources：`IOPSCopyPowerSourcesInfo`、`IOPSCopyPowerSourcesList`、`IOPSGetPowerSourceDescription`，读取电源来源、电量和充电状态。
- IOKit `AppleSmartBattery`：`IOServiceMatching`、`IOServiceGetMatchingService`、`IORegistryEntryCreateCFProperties`，读取循环次数、容量、健康度、电压、电流、温度和适配器信息。

### 12. 显示

调用位置：[DisplayInfo.mm](../src/core/display/DisplayInfo.mm)。

- AppKit `NSScreen`：显示器和分辨率信息。
- CoreGraphics：`CGDisplayCopyDisplayMode`、`CGDisplayModeGetRefreshRate`、`CGDisplayModeRelease`、`CGDisplayIsBuiltin`，读取刷新率和内建显示器状态。

### 13. USB

调用位置：[UsbInfo.cpp](../src/core/usb/UsbInfo.cpp)。

- IOKit 枚举 `IOUSBHostDevice`，必要时回退到 `IOUSBDevice`。
- `IOServiceGetMatchingServices`、`IORegistryEntryCreateCFProperties`：读取产品名、厂商、序列号、`idVendor`、`idProduct`、`bcdUSB`、最大功率和供电属性。

### 14. 进程

调用位置：[ProcessTop.mm](../src/core/process/ProcessTop.mm)。

- libproc：`proc_listallpids` 枚举进程；`proc_pidinfo` 使用 `PROC_PIDTASKINFO` 读取逐进程 CPU 时间和内存，使用 `PROC_PIDTBSDINFO` 读取进程信息。

### 15. 系统事件与热状态

调用位置：[SystemEventMonitor.cpp](../src/core/notifications/SystemEventMonitor.cpp)、[DeviceChangeNotifier.h](../src/core/notifications/DeviceChangeNotifier.h)。

- IOPowerSources：`IOPSNotificationCreateRunLoopSource` 和 `IOPSKeys`，监听电源/电池变化。
- DiskArbitration：`DASessionCreate`、`DARegisterDiskAppearedCallback`、`DARegisterDiskDisappearedCallback`、`DASessionScheduleWithRunLoop`，监听磁盘挂载变化。
- IOKit：`IONotificationPortCreate`、`IOServiceAddMatchingNotification`，监听 USB/蓝牙设备变化；`IORegistryEntryFromPath("IODeviceTree:/")` 用于系统事件相关读取。
- `NSProcessInfo.thermalState`：读取系统热压力等级。

## Windows

### 1. WMI 与 COM

调用位置：[WMIManager.cpp](../src/core/Utils/WMIManager.cpp) 及 CPU、内存、GPU、磁盘、网络、电源、主板采集器。

- COM：`CoInitializeEx`、`CoCreateInstance`、`CoSetProxyBlanket`、`CoUninitialize`、`VariantInit`。
- WMI 对象：`IWbemLocator` → `IWbemServices` → `IWbemClassObject`。
- 使用的类包括 `Win32_Processor`、`Win32_PhysicalMemory`、`Win32_VideoController`、`Win32_NetworkAdapter`、`Win32_DiskDrive`、`Win32_DiskPartition`、`Win32_DiskDriveToDiskPartition`、`Win32_LogicalDiskToPartition`、`Win32_Battery`、`Win32_BaseBoard`、`Win32_BIOS`、`Win32_ComputerSystemProduct` 和 `Win32_SystemEnclosure`。
- `MemoryInfo.cpp` 等路径会独立创建 COM/WMI 会话，不是所有查询都经 `WMIManager`。

### 2. PDH

调用位置：[CpuInfo.cpp](../src/core/cpu/CpuInfo.cpp)。

- `PdhOpenQuery`、`PdhAddEnglishCounter`、`PdhCollectQueryData`、`PdhGetFormattedCounterValue`、`PdhCloseQuery`。
- 计数器：`\Processor(_Total)\% Processor Time`、`\Processor Information(0,0)\% Processor Performance`，以及首个 E-core 对应的 `Processor Information` 计数器。
- 用途：CPU 占用及基于性能计数器推算的实时频率。

### 3. Win32、注册表与进程

调用位置：[CpuInfo.cpp](../src/core/cpu/CpuInfo.cpp)、[MemoryInfo.cpp](../src/core/memory/MemoryInfo.cpp)、[ProcessMonitor.cpp](../src/core/process/ProcessMonitor.cpp)、[DiskInfo.cpp](../src/core/disk/DiskInfo.cpp)、[OSInfo.cpp](../src/core/os/OSInfo.cpp)、[TimeUtils.cpp](../src/core/Utils/TimeUtils.cpp)。

- 内存：`GlobalMemoryStatusEx`。
- CPU：`GetSystemInfo`、`GetLogicalProcessorInformationEx`。
- 进程：运行时从 `ntdll.dll` 加载 `NtQuerySystemInformation(SystemProcessInformation)`，枚举进程并读取 CPU 时间、工作集、线程等；`OpenProcess`、`OpenProcessToken`、`GetTokenInformation`、`LookupAccountSidW` 用于补充进程用户信息。当前实现不再用 Toolhelp32 枚举进程。
- 磁盘：`GetDriveTypeW`、`GetVolumeInformation`、`GetDiskFreeSpaceEx`；SMART/NVMe 使用 `CreateFileW("\\\\.\\PhysicalDriveN")` 和 `DeviceIoControl`。
- 系统版本/注册表：从 `ntdll.dll` 动态加载 `RtlGetVersion`；`RegOpenKeyEx`、`RegQueryValueEx` 读取处理器信息。
- 运行时长：`GetTickCount64`。

### 4. 网络与 Wi-Fi

调用位置：[NetworkAdapter.cpp](../src/core/network/NetworkAdapter.cpp)、[WiFiInfo_wlan.c](../src/core/wifi/WiFiInfo_wlan.c)。

- IP Helper：`GetAdaptersAddresses`、`GetIfTable2`。
- WLAN API：`WlanOpenHandle`、`WlanEnumInterfaces`、`WlanQueryInterface`、`WlanCloseHandle`、`WlanFreeMemory`。

### 5. 蓝牙与设备枚举

调用位置：[BluetoothInfo.cpp](../src/core/bluetooth/BluetoothInfo.cpp)、[UsbInfo.cpp](../src/core/usb/UsbInfo.cpp)、[DeviceChangeNotifier.h](../src/core/notifications/DeviceChangeNotifier.h)。

- Bluetooth API：`BluetoothFindFirstRadio` / `BluetoothFindNextRadio`、`BluetoothGetRadioInfo`、`BluetoothFindFirstDevice` / `BluetoothFindNextDevice`，以及相应的关闭函数。周期采集枚举已连接设备，不主动执行附近设备扫描。
- SetupAPI：`SetupDiGetClassDevs`、`SetupDiEnumDeviceInfo`、`SetupDiGetDeviceInstanceId`、`SetupDiGetDeviceRegistryProperty`、`SetupDiOpenDevRegKey`、`SetupDiDestroyDeviceInfoList`。
- CfgMgr32：`CM_Get*` 设备信息接口；`CM_Register_Notification` 动态加载以监听 USB/蓝牙设备变化，旧系统回退到轮询。

### 6. 电源、电池与 CPU 功耗

调用位置：[PowerInfo.cpp](../src/core/power/PowerInfo.cpp)、[CpuPowerReader.cpp](../src/core/cpu/CpuPowerReader.cpp)。

- `GetSystemPowerStatus`：AC/电池状态。
- Power API：`PowerGetActiveScheme`、`PowerReadFriendlyName`：当前电源计划。
- WMI `Win32_Battery`：电池基础属性。
- 电池设备接口：SetupAPI 枚举电池设备，`CreateFile` 后调用 `DeviceIoControl` 的 `IOCTL_BATTERY_QUERY_TAG`、`IOCTL_BATTERY_QUERY_INFORMATION`、`IOCTL_BATTERY_QUERY_STATUS`，补充设备提供的容量、电压和循环次数。
- Intel CPU package 功耗：PawnIO 加载 `INTEL_MSR` 模块，读取 RAPL 能量单位 MSR `0x606` 和 package energy MSR `0x611`，根据计数差计算功率。

### 7. 温度

调用位置：[TemperatureWrapper.cpp](../src/core/temperature/TemperatureWrapper.cpp)、[AcpiThermalZoneReader.cpp](../src/core/temperature/AcpiThermalZoneReader.cpp)。

- PawnIO 传感器路径读取 Intel MSR、AMD SMU、DDR5 SPD Hub/LM75、NCT6798D 等支持的传感器。
- WMI `ROOT\\WMI` 类 `MSAcpi_ThermalZoneTemperature`：读取 ACPI 热区温度；结果缓存 30 秒，部分机器/权限下查询可能不可用。

### 8. GPU

调用位置：[GpuInfo.cpp](../src/core/gpu/GpuInfo.cpp)。

- NVML：运行时加载 `nvml.dll`，使用 `nvmlInit`、`nvmlShutdown`、`nvmlDeviceGetCount`、`nvmlDeviceGetHandleByIndex_v2`、`nvmlDeviceGetMemoryInfo`、`nvmlDeviceGetUtilizationRates`、`nvmlDeviceGetTemperature`、`nvmlDeviceGetFanSpeed` / `nvmlDeviceGetFanSpeed_v2`、`nvmlDeviceGetClockInfo`、`nvmlDeviceGetCudaComputeCapability`、`nvmlDeviceGetPowerUsage` 和 `nvmlDeviceGetEnforcedPowerLimit`。
- NVAPI：运行时加载 `nvapi64.dll`，经 `nvapi_QueryInterface` 查询 `NVAPI_Initialize`、`NVAPI_EnumPhysicalGPUs`、`NVAPI_GPU_GetCoolerSettings`、`NVAPI_GPU_ClientFanCoolersGetStatus`。
- DXGI：`CreateDXGIFactory`、`IDXGIFactory`、`IDXGIAdapter`，枚举图形适配器。

### 9. 系统事件通知

调用位置：[SystemEventMonitor.cpp](../src/core/notifications/SystemEventMonitor.cpp)、[DeviceChangeNotifier.h](../src/core/notifications/DeviceChangeNotifier.h)。

- `RegisterPowerSettingNotification` + AC/DC 电源来源 GUID：监听电源切换。
- `RegisterDeviceNotificationW` + 卷设备接口 GUID、`WM_DEVICECHANGE`：监听卷变化。
- `CM_Register_Notification`：监听 USB、USB Hub、蓝牙设备接口变化。

### 10. ETW

调用位置：[EtwMonitor.cpp](../src/core/etw/EtwMonitor.cpp)。

- 会话与事件读取：`StartTraceW`、`EnableTraceEx2`、`OpenTraceW`、`ProcessTrace`、`ControlTraceW`、`CloseTrace`。
- 订阅提供程序：Kernel-Power、TCPIP、WLAN-AutoConfig、BTHPORT、USB-UCX、Kernel-Processor-Power。
- 事件用于电源、网络、Wi-Fi、蓝牙、USB 和处理器/电源状态变化通知。

### 11. PawnIO 内核驱动

调用位置：[PawnIOWrapper.cpp](../src/core/memory/PawnIOWrapper.cpp) 及下列读取器。

- `CreateFileW("\\\\.\\GLOBALROOT\\Device\\PawnIO")`、`DeviceIoControl`：向 PawnIO 驱动发送请求并执行已签名模块。
- 使用方包括 `CpuTempReader`（Intel MSR）、`RyzenSmuReader`、`CpuPowerReader`（Intel RAPL）、`MemoryTempReader`（SMBus/DDR5 SPD Hub）、`BoardTempReader`（NCT6798D/LpcIO）、`IntelMchbarReader` 和 `IntelOobmsmReader`。
- PawnIO 是可选能力；驱动或模块不可用时，对应读数可能为空。

## Linux

Linux 采集代码位于各模块的 `TCMT_LINUX` 分支，公共 sysfs/procfs 辅助函数位于 [Sysfs.h](../src/core/Utils/Sysfs.h)。是否有数据取决于内核驱动、设备暴露的节点及访问权限。

### 1. 系统、CPU 与内存

调用位置：[OSInfo.cpp](../src/core/os/OSInfo.cpp)、[CpuInfo.cpp](../src/core/cpu/CpuInfo.cpp)、[MemoryInfo.cpp](../src/core/memory/MemoryInfo.cpp)。

- `/etc/os-release`、`/proc` 和 `/sys`：系统版本、处理器信息、运行时长和设备属性；CPU 负载读取 `/proc/stat`，CPU 型号/拓扑读取 `/proc/cpuinfo`。
- `/sys/devices/system/cpu/cpu*/cpufreq`：可用的 CPU 频率与调速器信息。
- `/proc/meminfo`、`/proc/swaps`：物理内存与 Swap。
- DMI 属性通过 `/sys/devices/virtual/dmi/id` 等节点读取；部分属性需要额外权限。

### 2. 温度、风扇与功耗

调用位置：[TemperatureWrapper.cpp](../src/core/temperature/TemperatureWrapper.cpp)、[CpuInfo.cpp](../src/core/cpu/CpuInfo.cpp)、[PowerInfo.cpp](../src/core/power/PowerInfo.cpp)、[CpuPowerReader.cpp](../src/core/cpu/CpuPowerReader.cpp)。

- 温度：`/sys/class/thermal/thermal_zone*`、`/sys/class/hwmon/hwmon*/temp*_input`。
- 风扇：`/sys/class/hwmon/hwmon*/fan*_input`，并读取可用的 `fan*_label`。
- CPU package 功耗：`/sys/class/powercap/intel-rapl*/energy_uj` 或 `amd-rapl*/energy_uj`；节点可能需要 root 权限，读取失败时功耗不可用。

### 3. GPU

调用位置：[GpuInfo.cpp](../src/core/gpu/GpuInfo.cpp)。

- DRM/sysfs：`/sys/class/drm/card*`、`/sys` 下的设备属性，读取可用的显卡信息、利用率和显存数据。
- hwmon：GPU 设备的 `temp*_input`、`power*_input`、`fan*_input`、`energy*_input` 等节点。
- NVIDIA GPU 可运行时加载 `libnvidia-ml.so.1` 并使用 NVML；库或驱动不可用时跳过该路径。

### 4. 磁盘与 SMART

调用位置：[DiskInfo.cpp](../src/core/disk/DiskInfo.cpp)。

- `/proc/diskstats`、`/sys/block`、挂载信息和 `statvfs`：磁盘吞吐、设备与挂载卷容量。
- NVMe/SATA SMART 读取使用 Linux 设备接口/ioctl；权限不足时健康信息可能不可用。
- 可用的磁盘温度也会从设备对应的 hwmon 节点读取。

### 5. 网络与 Wi-Fi

调用位置：[NetworkAdapter.cpp](../src/core/network/NetworkAdapter.cpp)、[WiFiInfo.cpp](../src/core/wifi/WiFiInfo.cpp)。

- `getifaddrs`：网卡地址和接口状态；`/proc/net/dev`：收发字节计数。
- `SIOCETHTOOL` + `ETHTOOL_GLINKSETTINGS`：链路速率；不支持时回退读取 `/sys/class/net/<接口>/speed`。
- Wi-Fi：在 `/sys/class/net` 查找无线接口，使用 ioctl `SIOCGIFFLAGS` 读取接口状态，并调用 `iw dev <接口> link` 解析连接信息。需要系统安装并允许执行 `iw`。

### 6. 蓝牙与 USB

调用位置：[BluetoothInfo.cpp](../src/core/bluetooth/BluetoothInfo.cpp)、[UsbInfo.cpp](../src/core/usb/UsbInfo.cpp)。

- 蓝牙：读取 `/sys/class/bluetooth/hci*` 下的适配器地址和名称；缺少 sysfs 名称时尝试 `hcitool dev`。当前 Linux 实现不枚举已连接蓝牙设备。
- USB：枚举 `/sys/bus/usb/devices/` 并读取设备属性。

### 7. 电池与电源

调用位置：[PowerInfo.cpp](../src/core/power/PowerInfo.cpp)。

- `/sys/class/power_supply/*`：电池状态、电量、容量、电压、电流、温度及 AC 在线状态。
- `/sys/class/powercap` 下 Intel/AMD RAPL `energy_uj`：CPU package 功耗；节点权限不足或硬件不支持时无读数。

### 8. 进程

调用位置：[ProcessMonitor.cpp](../src/core/process/ProcessMonitor.cpp)。

- `/proc/<pid>/stat`、`statm`、`status`：进程名称、CPU 时间、内存和用户信息。

### 9. 显示与其他未实现项

- 当前没有 Linux 显示器信息采集实现；[DisplayInfo.mm](../src/core/display/DisplayInfo.mm) 是 macOS 实现。
- 各 Linux 模块可能因驱动和硬件差异只提供部分数据；具体支持范围另见 [linux-support-scope.md](./linux-support-scope.md)。
