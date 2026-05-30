# Session State (2026-05-30)

## Branch
`dev`

## Current State
- **SpsManager — Unified Apple SPU HID Sensor Monitor**
  - 单后台线程 + CFRunLoop 读取所有 `AppleSPUHIDDevice` 传感器
  - 自动枚举 0xFF00/3(重力)、0xFF00/9(陀螺仪)、0xFF00/4(ALS)、0x0020/138(开盖角度)
  - 未知传感器 (0xFF00/5, 0xFF0C/1, 0xFF0C/5) 前 5 次回调 dump 原始 hex
  - 使用 `IOHIDDeviceRegisterInputReportWithTimeStampCallback`（中断驱动，macOS 15+ 无需 root）
  - 启动时唤醒全部 `AppleSPUHIDDriver`（`SensorPropertyReportingState=1`, `SensorPropertyPowerState=1`, `ReportInterval=1000`）
  - TUI Sensors 面板显示: ALS、Gravity (g)、Gyro (deg/s)、Lid Angle (°)
- **Accelerometer (BMI284) 技术验证完成**
  - 22 字节 HID 报告格式：x int32 LE @ offset 6, y @ 10, z @ 14, /65536 → g
  - 已验证模长约 1g（静止时），倾斜时分量变化正确
  - 废弃了 SMJobBless 特权助手路径（代码保留，不再编译）
  - 旧 helper PID 8741 仍可通过 launchd 残留运行，SpsManager 完全忽略其 SHM
- **macOS 15+ WiFi SSID 修复完成**
  - `.app` bundle 打包 + Apple Development 证书签名 + Info.plist 定位描述
  - CoreLocation `requestWhenInUseAuthorization` 触发权限弹窗
  - 用户授权后 CWInterface.ssid 正常读取、不再被 `redacted`
- **DisplayInfo 模块** (`src/core/display/`) — 读取显示器信息
  - 名称（`localizedName`，macOS 14+）+ 分辨率（物理像素）+ 刷新率 + HDR + Built-in 检测
  - 刷新率回退链：`CGDisplayModeGetRefreshRate` → `maximumFramesPerSecond`(12+) → `maximumPotentialFrameRate`(14+)
  - TUI 右侧 Displays 面板展示所有屏幕信息
- **Thermal State** — 读取 `NSProcessInfo.processInfo.thermalState`（0=Nominal / 1=Fairly Serious / 2=Critical）
  - TUI Power 面板中显示 Thermal 状态，Critical 时红色高亮
- **温度读取** — 修复 M4 Mac Mini SMC key 兼容性
  - M1/M2/M3: `Tp*`(P-Core), `Te*`(E-Core), `Tg*`(GPU)
  - M4: `TPD*`(P-Core), `TDeL`+`Te*`(E-Core), `Tg*`(GPU), `TRD*`(RAM), `TB*T`(Board)... 共 112 个温度传感器
  - AppleSMCKeysEndpoint 匹配策略：先按类名直接查找（M4），再回退到 AppleSMC 子节点遍历（M1/M2/M3）
  - TUI Temps 面板显示所有 SMC 传感器，自动分页（3秒翻页，每页6条）
  - Cluster 传感器命名修复：显示完整后缀（如 `Cluster #0A` / `#0B` / `#0C`）

## 构建方式 (macOS)
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j8
# 从 bundle 启动（非裸 binary，否则 CoreLocation 弹窗不生效）：
./build/src/TCMT-M.app/Contents/MacOS/TCMT-M
```

## 构建命令 (Windows VS2026)
```bash
msbuild TCMT.sln /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0
# 或 MSBuild Debug:
msbuild TCMT.sln /p:Configuration=Debug /p:Platform=x64
```

## 本次 Session 改动 (2026-05-30)

### 显示 & 兼容
- **ALS RGBC 归一化** — 原始 ADC 值（4位数）→ 除以 max(R,G,B)×255，显示 0-255 范围
- **Windows/PDCurses 兼容** — pid_t 定义、便携 wcwidth/wcswidth 实现、ESCDELAY guard、_CRT_SECURE_NO_WARNINGS、wchar_t >> 强制转型
- **Cluster 命名修复** — `key.substr(2)` 代替 `std::string(1, key[2])`，显示完整后缀（`Cluster #0A` / `#0B` 而非全变 `#0`）

### 清理
- `.DS_Store` × 20+ 删除
- `AvaloniaUI.backup/` 删除
- cmake 缓存清理后修复 `LANGUAGES C CXX`（tcmt_sensor_helper.c 需要 C）
- `main_mac.cpp` 删除已不存在的 `chargerWatts` 引用

### 尝试后撤回
- VRM 单独聚合 → 撤回（用户要求还原）
- 全部温度按类别聚合（CPU/GPU/VRM/RAM 等）→ 撤回（用户要求还原）
- TV* catch-all 改为 VRM 命名 → 同步撤回

## 已知约束
- **macOS 15+ 定位权限**：必须从 `.app` bundle 启动，且用 Apple Development 证书签名（ad-hoc 签名下 `requestWhenInUseAuthorization` 静默失败）
- **DisplayInfo**：`NSScreen.localizedName` 需 macOS 14+；`CGDisplayIsBuiltin` 在某些 Apple Silicon 机型上可能返回 0 导致内置屏未被标记
- **Thermal State**：`@available` 不能在 `.cpp`（纯 C++）文件中使用——热状态读取代码必须在 `.mm`（Obj-C++）文件中或使用 `#ifdef __OBJC__` 包裹；`NSProcessInfoThermalState` 从 macOS 10.10.3+ 可用
- **Apple Development signing identity**: `dongge0210@qq.com (U8U894TFMV)`，hash `6AD7D5B2E77AB209D2A6D8B038F297A22EAC8179`
- **`com.apple.developer.location-wifi` entitlement**：不需要——CoreLocation 弹窗 + Info.plist 即可让 CWInterface.ssid 正常工作；该 entitlement 在 macOS 上即使真证书也触发 AMFI kill
- **system_profiler 兜底**：已过滤 `<redacted>` 字符串，识别到后设 `locationDenied` flag
- **AppKit 依赖**：DisplayInfo.mm 需要 `AppKit.framework`（已在 `cmake/Platform.cmake` 中添加）；`CoreGraphics.framework` 系统自带链接
- **unknown sensor 格式待识别**：0xFF00/5, 0xFF0C/1, 0xFF0C/5 的原始 hex 已 dump 到日志，需进一步分析确定数据类型

## 遗留问题
- CPU 逻辑核心数在 Apple Silicon 上冗余（无超线程）
- Windows SMART 表格数据待验证
- IPCService ViewModel 集成稳定性
<<<<<<< HEAD
- SpsManager 未知传感器格式解码（0xFF00/5, 0xFF0C/1, 0xFF0C/5）
- AccelSensor.h/.mm 可与 SpsManager 合并（旧单传感器封装可删除）
=======
- `kIOMainPortDefault` 需 macOS 12.0 但 deploy target 设 11.0（编译警告）
- Apple 签名时间戳服务器不稳定（`--timestamp` 临时移除）
>>>>>>> 99dde4eb (docs: update session.md with 2026-05-30 changes)

## 用户习惯
- 零编译警告容忍
- 每个改动一个独立 commit
- 不混功能代码和诊断代码
- 通过 `acceptEdits` 模式授权 agent 改文件
- 偏好 sonnet 或 pro 模型做代码改动
