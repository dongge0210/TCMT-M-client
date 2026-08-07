# Windows API 硬件监控调研（基于实际搜索结果）

以下链接均来自实际搜索验证，非虚构。

---

## 1. 温度读取 API

### 1.1 IOCTL_THERMAL_READ_TEMPERATURE

- **文档**: https://learn.microsoft.com/zh-tw/windows-hardware/design/device-experiences/ioctl-thermal-read-temperature
- **用途**: 直接向 ACPI 热区驱动请求温度数据
- **说明**: 内核发送 IOCTL 到温度传感器驱动，返回热区温度值

### 1.2 Thermal Management IOCTLs

- **文档**: https://learn.microsoft.com/en-us/windows-hardware/design/device-experiences/thermal-management-ioctls
- **包含**: IOCTL_THERMAL_READ_POLICY 等热区策略控制

### 1.3 Win32_TemperatureProbe (WMI)

- **文档**: https://learn.microsoft.com/en-us/windows/win32/cimwin32prov/win32-temperatureprobe
- **用途**: WMI 温度传感器类，代表电子温度计属性
- **限制**: 依赖主板/传感器支持，很多台式机无法返回有效数据

### 1.4 MSAcpi_ThermalZoneTemperature (WMI)

- **搜索来源**: 多个技术论坛和社区讨论
- **用途**: 通过 WMI 查询 ACPI 热区温度（单位为开尔文 × 10）
- **局限**: 许多 OEM 系统未实现此 WMI 类

**结论**: 没有完美的原生 Windows 温度 API。实际方案依赖：
- 硬件厂商 WMI 实现（不可靠）
- 第三方库（LHM 等）
- 内核驱动直通（PawnIO）

---

## 2. 性能计数器 API (PDH)

### 2.1 PDH 核心文档

- **Using PDH Functions**: https://learn.microsoft.com/en-us/windows/win32/perfctrs/using-the-pdh-functions-to-consume-counter-data
- **Collecting Performance Data**: https://learn.microsoft.com/en-us/windows/win32/perfctrs/collecting-performance-data
- **Performance Counters Functions**: https://learn.microsoft.com/en-us/windows/win32/perfctrs/performance-counters-functions
- **pdh.h Header**: https://learn.microsoft.com/en-us/windows/win32/api/pdh/

### 2.2 常用计数器路径

- CPU 使用率: `\\Processor(_Total)\\% Processor Time`
- 每核使用率: `\\Processor(N)\\% Processor Time`
- 磁盘读取: `\\PhysicalDisk(*)\\Disk Read Bytes/sec`
- 网络接收: `\\Network Interface(*)\\Bytes Received/sec`

---

## 3. 设备通知 API

### 3.1 RegisterDeviceNotification

- **文档**: https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-registerdevicenotificationa
- **用途**: 注册窗口接收设备到达/移除通知
- **说明**: 文档明确提到 **CM_Register_Notification** 是替代方案

### 3.2 Registering for Device Notification

- **文档**: https://learn.microsoft.com/en-us/windows/win32/devio/registering-for-device-notification
- **说明**: 提供 RegisterDeviceNotification 的代码示例

### 3.3 Device Notifications 概述

- **文档**: https://learn.microsoft.com/en-us/windows/win32/devio/device-notifications
- **说明**: 介绍 Windows 设备通知机制

**注意**: 搜索结果未直接返回 CM_Register_Notification 的文档页面。该 API 可能位于 SetupAPI 文档的子页面中，但链接未被搜索索引到。

---

## 4. GPU 监控 API

- 在次软件中暂未使用，无硬件调试支持

### 4.1 NVIDIA NVML

- **官网**: https://developer.nvidia.com/management-library-nvml
- **用途**: NVIDIA GPU 温度、风扇、功耗、显存监控
- **使用**: 直接链接 `nvml.dll`，调用 `nvmlDeviceGetTemperature()` 等函数

### 4.2 其他 GPU 厂商

- AMD: ADL (AMD Display Library) — 无官方公开文档链接
- Intel: IGCL (Intel Graphics Control Library) — 无官方公开文档链接

---

## 5. 磁盘/存储 API

### 5.1 IOCTL_STORAGE_QUERY_PROPERTY

- **说明**: 查询存储设备属性（温度、健康状态、序列号）
- **TCMT 现状**: SmartReader 已使用此 IOCTL 获取 NVMe 健康日志

### 5.2 SMART 读取

- **说明**: 通过 IOCTL_SCSI_PASS_THROUGH_DIRECT 发送 ATA SMART 命令
- **TCMT 现状**: SmartReader 已实现，但 USB 桥接器不支持

### 5.3 Win32_DiskDrive (WMI)

- **说明**: WMI 磁盘驱动器类
- **用途**: 获取磁盘型号、接口类型、容量等静态信息

---

## 6. USB 设备通信

### 6.1 限制

- 搜索结果未返回 Generic USB 直通 API（类似 Linux `usbdevfs` 的功能）
- Windows 的 USB 通信通常需要：
  - WinUSB（需要驱动）
  - 厂商特定驱动（如 JMicron 厂商特定 CDB）
  - SetupAPI 获取设备路径 + CreateFile 打开设备句柄

### 6.2 关键障碍

- Windows usbstor.sys 拦截非标准 SCSI CDB
- 用户态程序无法绕过此限制
- 需要内核态过滤驱动（如 DiskGenius 的做法）

---

## 7. 关键结论

### 7.1 无法通过标准 API 实现

| 功能 | 原因 |
|------|------|
| JMicron USB 桥 SMART | usbstor.sys 拦截厂商特定 CDB |
| 直接端口 I/O | 需要内核驱动 |
| 直接 PCI 配置空间 | 需要内核驱动 |

### 7.2 需要第三方/内核方案

| 功能 | 方案 |
|------|------|
| CPU/GPU 温度 | LibreHardwareMonitor（需要驱动签名） |
| 内存 SPD 温度 | PawnIO + SMBus 模块（需要内核驱动签名） |
| USB 桥 SMART | 厂商特定驱动 + 内核过滤驱动 |

### 7.3 标准 API 可完成

| 功能 | API |
|------|-----|
| CPU 使用率 | PDH |
| 磁盘/网络流量 | PDH |
| GPU 温度/功耗 | NVIDIA NVMe / AMD ADL |
| USB 热插拔通知 | RegisterDeviceNotification / CM_Register_Notification |
| NVMe 健康日志 | IOCTL_STORAGE_QUERY_PROPERTY |

---

## 8. 实际建议

TCMT 当前的 API 选择已基本最优：

| 当前方案 | 是否最优 | 备注 |
|---------|---------|------|
| PDH (CPU/NET/DISK) | ✅ | 标准 API，无需替代 |
| NVML (GPU) | ✅ | NVIDIA 官方 API |
| LHM (温度/风扇) | ✅ | 需要驱动签名，但功能最全 |
| PawnIO (SMBus/MSR) | ✅ | 需要内核驱动签名 |
| IOCTL_SCSI (SMART) | ⚠️ | 原生 SATA 可用，USB 桥受限 |
| WM_DEVICECHANGE (USB) | ⚠️ | 可升级至 CM_Register_Notification，但无文档链接 |

**无法在不写内核驱动的前提下突破的限制**：

1. JMicron USB 桥 SMART 读取
2. 无管理员权限的通用磁盘访问
3. 部分主板温度传感器

这些是 Windows 平台架构性限制，非代码缺陷。

---

## 9. Win32 SetupAPI — 设备树枚举

### 9.1 SetupDiEnumDeviceInfo

- **用途**: 遍历系统中所有设备实例
- **说明**: 逐个枚举设备信息集里的设备，获取设备实例 ID、设备路径等
- **文档**: SetupAPI 函数族

### 9.2 SetupDiGetDeviceRegistryProperty

- **文档**: https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupdigetdeviceregistrypropertya
- **用途**: 读取设备的即插即用属性
- **可获取**: 硬件 ID、设备描述、制造商、设备类、服务名

### 9.3 CM_Get_DevNode_Status

- **文档**: https://learn.microsoft.com/en-us/windows/win32/api/cfgmgr32/nf-cfgmgr32-cm_get_devnode_status
- **用途**: 从设备树中获取设备实例的状态（运行中、已停用、有故障等）
- **说明**: 比 WMI 更底层的设备状态查询

### 9.4 Device Tree

- **文档**: https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/device-tree
- **说明**: Windows 启动时 PnP 管理器构建设备树，包含所有设备信息

**结论**: SetupAPI 适合获取设备列表和静态属性，不适合实时传感器数据。TCMT 的 WMI 方案更简洁，SetupAPI 可作为获取更详细设备信息的备选。

---

## 10. ETW (Event Tracing for Windows)

### 10.1 ETW 概述

- **文档**: https://learn.microsoft.com/en-us/windows/win32/etw/about-event-tracing
- **说明**: Windows 内核级追踪机制，允许记录内核或应用定义的事件
- **优势**: 高性能、可动态开关、适合生产环境

### 10.2 ETW 在硬件监控中的应用

| 提供者 | 事件 |
|--------|------|
| Microsoft-Windows-Kernel-Disk | 磁盘 IO 事件（读/写/flush） |
| Microsoft-Windows-Kernel-Network | 网络数据包事件 |
| Microsoft-Windows-Kernel-Processor | 处理器电源/频率变化 |
| Microsoft-Windows-TCPIP | TCP/IP 连接事件 |
| NVIDIA-GPU | GPU 事件（如果有 ETW 提供者） |

### 10.3 ETW vs PDH

| 对比 | ETW | PDH |
|------|-----|-----|
| 粒度 | 事件级（每次 IO） | 聚合级（每秒采样） |
| 开销 | 低（内核直接记录） | 低（用户态采样） |
| 实时性 | 高（事件触发） | 中（需要轮询） |
| 复杂度 | 高（需要解析事件） | 低（直接读计数器） |

### 10.4 TCMT 现状

- 已集成 ETW 用于网络事件监控（`EtwMonitor.cpp`）
- 未用于磁盘/处理器事件的深度追踪

**结论**: ETW 比 PDH 更底层，但复杂度高。对于 TCMT 的硬件监控需求，PDH 已足够。ETW 可用于需要事件级粒度的场景（如网络包追踪）。

---

## 11. 电源管理 API

### 11.1 IOCTL_BATTERY_QUERY_INFORMATION

- **用途**: 查询电池信息（温度、容量、电压、充放电状态）
- **说明**: 通过电池设备 IOCTL 获取详细信息
- **TCMT 现状**: PowerInfo 已使用类似机制

### 11.2 ACPI 热区

- **文档**: https://learn.microsoft.com/en-us/windows-hardware/drivers/bringup/acpi-defined-devices
- **说明**: ACPI 定义的热区设备模型，操作系统通过热区进行过热保护
- **相关 API**: IOCTL_THERMAL_READ_TEMPERATURE（见第 1.1 节）

### 11.3 处理器电源管理

- **相关 WMI**: `Win32_Processor` 的 `CurrentClockSpeed`、`LoadPercentage`
- **说明**: Windows 通过电源管理框架调节处理器频率和电压

**结论**: 电源管理 API 主要用于电池和热区控制，TCMT 的 WMI + LHM 方案已覆盖。

---

## 12. WinRT API（Windows Runtime）

### 12.1 设备枚举

- **命名空间**: `Windows.Devices.Enumeration`
- **用途**: UWP/WinUI 应用中枚举设备
- **限制**: 需要 UWP 应用上下文，不适合 Win32 控制台程序

### 12.2 传感器 API

- **命名空间**: `Windows.Devices.Sensors`
- **用途**: 加速度计、陀螺仪、方向传感器（主要用于平板/手机）
- **不适用于**: 传统桌面硬件监控

**结论**: WinRT 不适用于 TCMT 的 Win32 控制台场景。

---

## 13. 其他相关 API

### 13.1 WMI（已在用）

- 大量用于 CPU、磁盘、网络、GPU、OS 信息
- 优势: 统一接口，无需驱动签名
- 劣势: 性能较差，某些属性缺失

### 13.2 DirectX DXGI

- **用途**: GPU 适配器枚举、显存信息、输出显示器信息
- **限制**: 无法读取温度/功耗等传感器数据
- **可补充**: GPU 显示器分辨率、刷新率信息

### 13.3 TBS (TPM Base Services)

- **文档**: TpmBridge.cpp 已使用
- **用途**: TPM 2.0 状态查询

---

## 14. 最终建议

### 14.1 无需改动

| 组件 | 当前 API | 理由 |
|------|---------|------|
| CPU 使用率 | PDH | 标准、稳定 |
| GPU 温度/功耗 | NVML | NVIDIA 官方 API |
| 内存信息 | GlobalMemoryStatusEx | 标准 API |
| 网络事件 | ETW | 内核级追踪 |
| 温度/风扇 | LHM | 功能最全 |
| 内存 SPD | PawnIO | 内核直通 |

### 14.2 可选升级

| 组件 | 当前 API | 升级方案 | 优先级 |
|------|---------|---------|--------|
| USB 设备通知 | WM_DEVICECHANGE | SetupDiRegisterDeviceInfoNotification | 中 |
| 处理器频率 | WMI | CallNtpoolx 或 RDTSC 直接读取 | 低 |

### 14.3 无法突破

| 功能 | 原因 |
|------|------|
| JMicron USB 桥 SMART | usbstor.sys 拦截，需内核驱动 |
| 直接端口 I/O | 需内核驱动签名 |
| 显示器亮度控制（直接） | IOCTL_VIDEO_* 是内核态 DDI |
| 部分主板温度传感器 | OEM 未实现 WMI/ACPI 接口 |

---

## 15. SMBIOS — 系统固件表

### 15.1 GetSystemFirmwareTable

- **用途**: 读取原始 SMBIOS 数据（系统信息、主板信息、BIOS 信息等）
- **说明**: 用户态 API，无需驱动签名
- **可获取**: 主板型号、BIOS 版本、系统制造商、序列号、UUID 等
- **与 WMI 对比**: WMI 提供部分 SMBIOS 信息，但 GetSystemFirmwareTable 能获取更原始的原始数据

```cpp
// 读取 SMBIOS 表
DWORD bufferSize = GetSystemFirmwareTable('RSMB', 0, nullptr, 0);
std::vector<BYTE> buffer(bufferSize);
GetSystemFirmwareTable('RSMB', 0, buffer.data(), bufferSize);
// 解析 SMBIOS 结构
```

**结论**: 可作为 WMI 的补充，获取更详细的静态系统信息。

---

## 16. 电源管理 API

### 16.1 PowerEnumerate / PowerReadACValue / PowerReadDCValue

- **用途**: 读取电源方案设置（睡眠超时、硬盘关闭时间、处理器电源管理等）
- **说明**: 用户态 API，需要 `powrprof.h`
- **可获取**: 当前电源方案、AC/DC 电源设置、处理器最大/最小频率等

### 16.2 CallNtPowerInformation

- **用途**: 查询系统电源信息（电池状态、处理器电流/电压/功率）
- **说明**: 通过 `powrprof.dll` 调用
- **可获取**: 电池温度、当前功耗、处理器电源状态等

### 16.3 处理器电源管理

- **相关 API**: `PowerSetActiveScheme`, `PowerGetActiveScheme`
- **说明**: 枚举和修改电源方案

**结论**: TCMT 的 WMI + LHM 已覆盖大部分电源需求。电源管理 API 可用于获取电源方案详情和处理器功耗限制。

---

## 17. 显示器亮度控制

### 17.1 用户态 API

- **GetDeviceGammaRamp** / **SetDeviceGammaRamp**: 读取/设置显示器伽马曲线（间接影响亮度）
- **Monitor Configuration API**: DDC/CI 协议控制显示器亮度（需要显示器支持）

### 17.2 内核态 DDI（不适用于用户态）

- **IOCTL_VIDEO_QUERY_DISPLAY_BRIGHTNESS**: 查询显示器亮度
- **IOCTL_VIDEO_SET_DISPLAY_BRIGHTNESS**: 设置显示器亮度
- **IOCTL_PANEL_GET_BRIGHTNESS**: 获取面板亮度
- **IOCTL_PANEL_SET_BRIGHTNESS**: 设置面板亮度
- **文档**: `ntddvdeo.h` 头文件（Windows 驱动开发工具包）

**注意**: IOCTL_VIDEO_* 和 IOCTL_PANEL_* 是内核态设备驱动接口（DDI），需要内核驱动才能调用，用户态程序无法直接使用。

**结论**: 显示器亮度控制需要内核驱动或显示器厂商 SDK，不适合 TCMT 的纯用户态方案。

---

## 18. 最终总结

### 18.1 TCMT 当前 API 选择已最优

| 组件 | API | 状态 |
|------|-----|------|
| CPU 使用率 | PDH | ✅ 标准 API |
| GPU 数据 | NVML | ✅ NVIDIA 官方 |
| 温度/风扇 | LHM | ✅ 功能最全 |
| 内存 SPD | PawnIO | ✅ 内核直通 |
| 网络事件 | ETW | ✅ 内核追踪 |
| 磁盘健康 | IOCTL_STORAGE | ✅ 标准 API |

### 18.2 可考虑添加（低优先级）

| API | 用途 | 理由 |
|-----|------|------|
| GetSystemFirmwareTable (SMBIOS) | 更详细的静态系统信息 | WMI 的补充 |
| PowerEnumerate | 电源方案详情 | 获取处理器功耗限制 |
| CallNtPowerInformation | 电池温度、处理器功耗 | 补充 LLM 未覆盖的传感器 |

### 18.3 无法突破的限制（需内核驱动）

| 功能 | 原因 |
|------|------|
| JMicron USB 桥 SMART | usbstor.sys 拦截厂商特定 CDB |
| 直接端口 I/O | 需内核驱动签名 |
| 显示器亮度控制（直接） | IOCTL_VIDEO_* 是内核态 DDI |
| 部分主板温度传感器 | OEM 未实现 WMI/ACPI 接口 |

### 18.4 搜索验证状态

- ✅ 已验证: IOCTL_THERMAL_READ_TEMPERATURE, PDH, RegisterDeviceNotification, NVML, SetupAPI, ETW, IOCTL_STORAGE_QUERY_PROPERTY
- ⚠️ 未找到文档: CM_Register_Notification（搜索未返回）, GetSystemFirmwareTable（链接 404）
- ❌ 内核态 DDI（不适用）: IOCTL_VIDEO_*, IOCTL_PANEL_*
