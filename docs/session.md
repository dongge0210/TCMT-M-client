# Session State (2026-06-11)

## Branch
`dev`

## Today
- **NamedPipeServer removed**: `NamedPipeServer.h/.cpp` deleted, merged into `IPCServer` with `#ifdef` (already done in previous session)
- **Tech debt audit**: documented in `docs/tech-debt.md` — dual IPC structs, main.cpp bloat, three main files duplication, TemperatureWrapper inline hardware layers

## Paused
- **HTTPServer module** (from 2026-06-10): shelved, `src/core/HTTPServer/` untracked, CMakeLists.txt + main_mac.cpp changes discarded

## Known Issues
- brotli `-L` path hardcoded for Homebrew (`/opt/homebrew/lib`) on macOS — needs CMake fix for Linux/Windows
- openssl submodule not yet built into CMake (uses Homebrew openssl@3 on macOS)
- h2o builds its bundled mruby as part of CMake (`[ 63%] Built target mruby`) — adds ~5s to each build
- `H2O_USE_LIBUV=0` must be defined on both the TCMTCore and TCMT-M targets to avoid uv.h include error

## User Habits
- Zero warnings policy
- One change per commit
- Prefers sonnet or pro models for code changes
