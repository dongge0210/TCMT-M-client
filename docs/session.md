# Session State (2026-05-28)

## Branch
`dev`

## Current State
- macOS 15+ WiFi SSID 修复完成
  - `.app` bundle 打包 + Apple Development 证书签名 + Info.plist 定位描述
  - CoreLocation `requestWhenInUseAuthorization` 触发权限弹窗
  - 用户授权后 CWInterface.ssid 正常读取、不再被 `redacted`
- **DisplayInfo 模块** (`src/core/display/`) — 读取显示器信息
  - 名称（`localizedName`，macOS 14+）+ 分辨率（物理像素）+ 刷新率 + HDR + Built-in 检测
  - 刷新率回退链：`CGDisplayModeGetRefreshRate` → `maximumFramesPerSecond`(12+) → `maximumPotentialFrameRate`(14+)
  - TUI 右侧 Displays 面板展示所有屏幕信息
- **Thermal State** — 读取 `NSProcessInfo.processInfo.thermalState`（0=Nominal / 1=Fairly Serious / 2=Critical）
  - TUI Power 面板中显示 Thermal 状态，Critical 时红色高亮
- **Accelerometer (BMI284)** — 已添加 `AccelSensor` 模块 + SMJobBless 特权 helper
  - macOS 15 `motionRestrictedService = Yes` 内核级阻断所有运动传感器
  - TUI 优雅显示 "N/A"
- **温度读取** — 修复 M4 Mac Mini SMC key 兼容性
  - M1/M2/M3: `Tp*`(P-Core), `Te*`(E-Core), `Tg*`(GPU)
  - M4: `TPD*`(P-Core), `TDeL`+`Te*`(E-Core), `Tg*`(GPU), `TRD*`(RAM), `TB*T`(Board)... 共 112 个温度传感器
  - AppleSMCKeysEndpoint 匹配策略：先按类名直接查找（M4），再回退到 AppleSMC 子节点遍历（M1/M2/M3）

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

## 已知约束
- **macOS 15+ 定位权限**：必须从 `.app` bundle 启动，且用 Apple Development 证书签名（ad-hoc 签名下 `requestWhenInUseAuthorization` 静默失败）
- **DisplayInfo**：`NSScreen.localizedName` 需 macOS 14+；`CGDisplayIsBuiltin` 在某些 Apple Silicon 机型上可能返回 0 导致内置屏未被标记
- **Thermal State**：`@available` 不能在 `.cpp`（纯 C++）文件中使用——热状态读取代码必须在 `.mm`（Obj-C++）文件中或使用 `#ifdef __OBJC__` 包裹；`NSProcessInfoThermalState` 从 macOS 10.10.3+ 可用
- **Apple Development signing identity**: `dongge0210@qq.com (U8U894TFMV)`，hash `6AD7D5B2E77AB209D2A6D8B038F297A22EAC8179`
- **`com.apple.developer.location-wifi` entitlement**：不需要——CoreLocation 弹窗 + Info.plist 即可让 CWInterface.ssid 正常工作；该 entitlement 在 macOS 上即使真证书也触发 AMFI kill
- **system_profiler 兜底**：已过滤 `<redacted>` 字符串，识别到后设 `locationDenied` flag
- **AppKit 依赖**：DisplayInfo.mm 需要 `AppKit.framework`（已在 `cmake/Platform.cmake` 中添加）；`CoreGraphics.framework` 系统自带链接

## 遗留问题
- CPU 逻辑核心数在 Apple Silicon 上冗余（无超线程）
- Windows SMART 表格数据待验证
- IPCService ViewModel 集成稳定性

## 用户习惯
- 零编译警告容忍
- 每个改动一个独立 commit
- 不混功能代码和诊断代码
- 通过 `acceptEdits` 模式授权 agent 改文件
- 偏好 sonnet 或 pro 模型做代码改动
