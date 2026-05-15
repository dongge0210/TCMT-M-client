# Session State (2026-05-04)

## Branch
`feat/restore-fixes` — 基于 `98357344` (dev working commit) + 子模块更新

## Current State
- macOS: C++ + C# 编译零警告零错误
- Windows: MSBuild + Avalonia 待验证
- PR 未创建

## Commits (约 40 个)
最新: `0f5a1f29` — fix: hide TPM section on macOS

## 遗留问题

### macOS Avalonia
- GPU 显存/品牌/温度隐藏（需 `dotnet build` 后验证）
- TPM 隐藏（需验证）
- 实时网速仍然是 0（需 `cmake --build build`，C++ 写入 downloadSpeed/uploadSpeed）
- CPU 逻辑核心数在 Apple Silicon 上冗余（无超线程）

### Windows
- 编译缺 `IPHLPAPI.lib` 等链接错误（GetIfTable2 需要）
- TUI 网速显示待验证
- SMART 表格数据待验证

### 功能待实现
- 实时网络吞吐量 Windows Avalonia 2 次显示问题
- IPCService ViewModel 集成稳定性

## 用户习惯
- 零编译警告容忍
- 每个改动一个独立 commit
- 不混功能代码和诊断代码
- 测试/CI 编译没问题但不能保证运行时
- 不喜欢 agent team 模式（haiku 模型 idle）
- 偏好 sonnet 或 pro 模型做代码改动
- 通过 `acceptEdits` 模式授权 agent 改文件

## 构建命令 (macOS)
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DASIO_INCLUDE_DIR=src/third_party/asio/asio/include
cmake --build build -j$(sysctl -n hw.ncpu)
dotnet build AvaloniaUI/AvaloniaUI.csproj -c Release -r osx-arm64
./build/src/TCMT-M --json
```

## 构建命令 (Windows VS2025)
```bash
msbuild TCMT.sln /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0
# 或 MSBuild Debug:
msbuild TCMT.sln /p:Configuration=Debug /p:Platform=x64
```
