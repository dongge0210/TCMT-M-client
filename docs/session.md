# Session State (2026-06-27)

## Branch
`dev`

## Today
- **Avalonia UI redesign**: All page XAML rewritten to match `designs/avalonia-ui.pen` (Dashboard, CPU, Memory, GPU, Network, Storage, Temperatures — except Terminal)
- **Sidebar redesign**: Changed from 72px emoji-only to 200px with text labels + icons, selected-state highlighting (`#3B82F626` bg / `#3B82F6` text), version at bottom
- **Colors updated**: Dark theme colors aligned to pen design (`#0D1117`, `#161B22`, `#8B949E`, `#3B82F6`, `#238636`, etc.)
- **IPC app version**: Added `appVersion[16]` to `IPCDataBlock` + schema field on macOS, read by `IPCSystemInfoMapper`, displayed in sidebar and top bar (no longer hardcoded)
- **New converters**: `PercentToWidthConverter`, `NavSelectedToBgConverter`, `NavSelectedToFgConverter`, `NavSelectedToFontWeightConverter`
- **Build**: Both C++ (macOS) and C# (Avalonia) build successfully
- **TCMT-Dev Agent 创建**: `.github/agents/tcmt-dev.agent.md` — 自主开发个体，遵循先查文档、零假代码、不清就问、自觉记笔记等工作流
- **笔记系统初始化**: `.notes/` 目录 (journal.md, insights.md, backup/)，已加入 .gitignore
- **P0: IPC 统一迁移 (SharedMemoryBlock → IPCDataBlock)** — 今日：
  - `IPCDataBlock` 补齐 WiFi band/gen + TPM 字段
  - `BuildWindowsIpcSchema` 从 offsetof(SharedMemoryBlock) 切换到 offsetof(IPCDataBlock)，WCHAR→String, Float64→Float32, Int32→UInt8
  - `SharedMemoryManager` 所有平台切换到 IPCDataBlock*
  - `WriteToSharedMemory` (Windows) 重写：SafeCopyWideString→SafeCopyStr, 类型缩窄 (double→float, int→uint8_t)
  - macOS/Linux `WriteToSharedMemory` 改为 stub（这些平台用 IPCServer 直接路径）
  - `main.cpp` 移除冗余 WiFi/BT 直接写入（WriteToSharedMemory 已覆盖）
  - **待后续**: `SharedMemoryBlock` 定义可删（无引用），C# 端 schema 仍兼容

## Previous (2026-06-11)
- **NamedPipeServer removed**: `NamedPipeServer.h/.cpp` deleted, merged into `IPCServer` with `#ifdef` (already done in previous session)
- **Tech debt audit**: documented in `docs/tech-debt.md` — dual IPC structs, main.cpp bloat, three main files duplication, TemperatureWrapper inline hardware layers

## Paused
- ~~Windows `SharedMemoryBlock` not yet updated with `appVersion` field~~ → **DONE**: IPCDataBlock 已有 `appVersion[16]`, schema 已注册 `app/version`

## Known Issues
- brotli `-L` path hardcoded for Homebrew (`/opt/homebrew/lib`) on macOS — needs CMake fix for Linux/Windows
- openssl submodule not yet built into CMake (uses Homebrew openssl@3 on macOS)
- h2o builds its bundled mruby as part of CMake (`[ 63%] Built target mruby`) — adds ~5s to each build
- `H2O_USE_LIBUV=0` must be defined on both the TCMTCore and TCMT-M targets to avoid uv.h include error
- `WrapPanel` `Spacing` not supported in Avalonia 11 — replaced with `StackPanel` + `Margin`

## User Habits
- Zero warnings policy
- One change per commit
- Prefers sonnet or pro models for code changes
