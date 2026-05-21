# Session State (2026-05-14)

## Branch
`dev`

## Current State
- unknown

## Commits (约 40 个)
最新: `0f5a1f29` — fix: hide TPM section on macOS

## 遗留问题

### macOS Avalonia
- CPU 逻辑核心数在 Apple Silicon 上冗余（无超线程）

### Windows
- SMART 表格数据待验证

### 功能待实现
- IPCService ViewModel 集成稳定性

## 用户习惯
- 零编译警告容忍
- 每个改动一个独立 commit
- 不混功能代码和诊断代码
- 测试/CI 编译没问题但不能保证运行时
- 不喜欢 agent team 模式（仅haiku 模型 idle时）
- 偏好 sonnet 或 pro 模型做代码改动
- 通过 `acceptEdits` 模式授权 agent 改文件

## 构建命令 (macOS)
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DASIO_INCLUDE_DIR=src/third_party/asio/asio/include
cmake --build build -j8
dotnet build AvaloniaUI/AvaloniaUI.csproj -c Release -r osx-arm64
./build/src/TCMT-M --json
```

## 构建命令 (Windows VS2026)
```bash
msbuild TCMT.sln /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0
# 或 MSBuild Debug:
msbuild TCMT.sln /p:Configuration=Debug /p:Platform=x64
```
