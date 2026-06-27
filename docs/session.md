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

## Previous (2026-06-11)
- **NamedPipeServer removed**: `NamedPipeServer.h/.cpp` deleted, merged into `IPCServer` with `#ifdef` (already done in previous session)
- **Tech debt audit**: documented in `docs/tech-debt.md` — dual IPC structs, main.cpp bloat, three main files duplication, TemperatureWrapper inline hardware layers

## Paused
- Windows `SharedMemoryBlock` not yet updated with `appVersion` field

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
