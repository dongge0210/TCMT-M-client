# Session State (2026-06-23)

## Branch
`dev`

## Today
- **Avalonia UI redesign**: All page XAML rewritten to match `designs/avalonia-ui.pen` (Dashboard, CPU, Memory, GPU, Network, Storage, Temperatures — except Terminal)
- **Sidebar redesign**: Changed from 72px emoji-only to 200px with text labels + icons, selected-state highlighting (`#3B82F626` bg / `#3B82F6` text), version at bottom
- **Colors updated**: Dark theme colors aligned to pen design (`#0D1117`, `#161B22`, `#8B949E`, `#3B82F6`, `#238636`, etc.)
- **IPC app version**: Added `appVersion[16]` to `IPCDataBlock` + schema field on macOS, read by `IPCSystemInfoMapper`, displayed in sidebar and top bar (no longer hardcoded)
- **New converters**: `PercentToWidthConverter`, `NavSelectedToBgConverter`, `NavSelectedToFgConverter`, `NavSelectedToFontWeightConverter`
- **Build**: Both C++ (macOS) and C# (Avalonia) build successfully

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
