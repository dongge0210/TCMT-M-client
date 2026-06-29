# Session State (2026-06-26)

## Branch
`dev`

## Today
- **OS support scope defined**: Win10 22H2+ / LTSC 2021, Win11 24H2+, macOS 15+ (arm64 + Intel), Ubuntu 22.04 LTS+
  - `TCMT.vcxproj`: `WindowsTargetPlatformMinVersion` → `10.0.19041.0` (was 26100), SDK stays `10.0`
  - `cmake/Platform.cmake`: `CMAKE_OSX_DEPLOYMENT_TARGET` → `15.0`, `CMAKE_OSX_ARCHITECTURES` → `arm64;x86_64`
  - `AvaloniaUI/TCMT.Avalonia.csproj`: `RuntimeIdentifiers` → `osx-arm64;osx-x64;win-x64;linux-x64`, `SupportedOSPlatformVersion` added

## MCP Servers Installed
- **mcp-ssh-manager** (`mcp-ssh-manager` v3.6.4): 37 SSH tools for remote server management. Server configs in `~/.ssh-manager/.env` (4 servers configured: winhost, 180, 187, 188).
- **vscode-mcp-server** (`@mseep/vscode-mcp-server` v0.4.0): VS Code extension exposing editor features via MCP. Connects to `http://localhost:3000/mcp`. VS Code settings enabled with shell/diagnostics/symbol tools (file/edit disabled to avoid duplicating Claude Code native tools).

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
