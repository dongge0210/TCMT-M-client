# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Cross-platform hardware monitoring tool (v0.14.0, GPL-3.0). Monitors CPU, GPU, memory, disk, network, OS, and temperature data, exposing results via shared memory IPC for a desktop UI or TUI.

**Two separate build systems co-exist:**
- **CMake** — C++20 core library (`TCMTCore` static lib) + CLI entry point (`TCMT-M`). Runs on both macOS ARM64 and Windows x64.
- **MSBuild** — Windows-only C++/CLI app (`TCMT.exe`, .NET Framework 4.7.2) with LibreHardwareMonitor bridge and an AvaloniaUI .NET 10.0 frontend.

## Build Commands

### macOS (Apple Silicon)
```bash
# C++ core + ncurses TUI
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(sysctl -n hw.ncpu)
# AvaloniaUI desktop frontend
dotnet build AvaloniaUI/AvaloniaUI.csproj -c Release -r osx-arm64
# Run
./build/src/TCMT-M
```
macOS requires `brew install ncurses` (CMake's `find_package(Curses)` handles it).

### Windows (x64, VS 2022/ VS 2026)
Build order is critical (see `.github/workflows/build.yml` for CI reference):
```bash
git submodule update --init --recursive

# 1. LibreHardwareMonitor (.NET 4.7.2)
dotnet build src/third_party/LibreHardwareMonitor/LibreHardwareMonitorLib/LibreHardwareMonitorLib.csproj -c Release -f net472

# 2. CPP-parsers
msbuild src/CPP-parsers/CPP-parsers/CPP-parsers.vcxproj /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0 /m

# 3. Main C++/CLI app
msbuild TCMT.sln /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0 /m

# 4. AvaloniaUI
dotnet build AvaloniaUI/AvaloniaUI.csproj -c Release
```
Prerequisites: VS 2022 with C++/CLI support, CUDA Toolkit 13.2+, .NET 8.0+ SDK.

### MCP Build Server
```bash
uv run --directory tools/mcp-build tcmt-build-mcp
```
Reads `tcmt-build.json` for build configuration. Use this for AI-assisted builds to avoid hardcoding output paths.

## Architecture

### Full Build Sequence
The complete build sequence as used in CI (see `.github/workflows/build.yml`):
1. Restore NuGet packages: `nuget restore Project1/Project1.sln`
2. Build LibreHardwareMonitorLib
3. Build CPP-parsers
4. Build main project

### Data Flow
```
C++ Core (TCMT-M / TCMT.exe)
  │  Collects: CPU, GPU, Memory, Disk, Network, OS, Temperature
  │  Sources: WMI (Windows), IOKit/sysctl (macOS), LibreHardwareMonitor (Windows .NET),
  │           NVML/CUDA (Windows), SMC/powermetrics (macOS)
  ▼
IPCDataBlock (macOS) / SharedMemoryBlock (Windows)
  │  Schema-driven field offsets broadcasted over Unix socket (macOS) / NamedPipe (Windows)
  │
  ├── AvaloniaUI (.NET 10.0, cross-platform) — connects via IPCPipeClient, reads via IPCMemoryReader
  ├── ncurses TUI (macOS-only) — reads directly in main_mac.cpp
  └── MCP Server — `TCMT-M --mcp` (JSON-RPC 2.0 stdio, uses IPC for data)
```

### Source Layout
- `src/core/` — C++20 static library `TCMTCore`: hardware modules, IPC pipeline, MCP server, USB, HistoryLogger
- `src/core/IPC/` — `IPCServer` (unified UDS+Windows), `IPCClient` (C++ client), `IPCData.h` (schema/protocol)
- `src/core/MCP/` — `MCPServer` (8 hardware tools, JSON-RPC 2.0 over stdio)
- `src/DataStruct.h` — Packed `SharedMemoryBlock` with all hardware structs. **Critical**: `WCHAR` typedef (`char16_t` on macOS, `wchar_t` on Windows)
- `src/main.cpp` — Windows entry (C++/CLI, Console app with SEH)
- `src/main_mac.cpp` — macOS entry (pure C++, ncurses TUI)
- `src/tui/` — ncurses TUI implementation (cross-platform)
- `AvaloniaUI/Services/IPCServices/` — C# IPC pipeline (IPCPipeClient, IPCMemoryReader, IPCSchema, IPCSystemInfoMapper)
- `AvaloniaUI/` — .NET 10.0 Avalonia 12.0.1 desktop app (MVVM with CommunityToolkit.Mvvm, Serilog)
- `cmake/` — CMake modules (Platform detection, Compiler Options, Dependencies)
- `tools/mcp-build/` — Python MCP build server
- `docs/superpowers/specs/` — design specs for TPM, disk SMART, Avalonia migration

### IPC / Shared Memory Gotchas

**Cross-platform:**
1. **macOS `wchar_t` is 4 bytes** — `DataStruct.h` uses `WCHAR` typedef (`char16_t` on non-Windows). Do NOT revert.
2. **Two shared memory blocks**: `IPCDataBlock` (macOS, schema-driven) and `SharedMemoryBlock` (Windows, legacy). The C# mapper reads BOTH but the field names and offsets differ per platform.
3. **Schema MaxFields=200** — must match in both `IPCData.h` and `IPCSchema.cs`. Bump both when adding fields.

**Windows-specific (critical):**
4. **Shared memory name**: C++ creates `Global\SystemMonitorSharedMemory` (with fallback to `Local\` and no prefix). C# `IPCMemoryReader.OpenWindows()` must try all 3 variants.
5. **Float32/Float64**: `IPCMemoryReader.ReadFloat32` on an 8-byte field reads 4 bytes (garbage) and returns non-null, blocking `ReadFloat64` fallback via `??`. Always try `ReadFloat64` FIRST.
6. **WString vs String**: Windows stores strings as `wchar_t` (UTF-16) in shared memory. Use `ReadWString` not `ReadString` for network, disk, temperature, SMART string fields.
7. **Windows NamedPipe**: uses fire-and-forget schema delivery (no handshake). C# client on Windows skips HELLO/HELLO_ACK and reads schema bytes directly. macOS uses full handshake protocol.
8. **MCP mode**: NEVER starts IPCServer. Always external IPC first, direct HW fallback. Uses non-blocking connect with 500ms timeout.

## Key Dependencies
- **Windows**: CUDA 13.2, LibreHardwareMonitor (MPL-2.0, used as compiled DLL), WMI, PDH
- **macOS**: IOKit, CoreFoundation, Metal, SMC, ncurses
- **AvaloniaUI**: Avalonia 12.0.1, CommunityToolkit.Mvvm 8.2.2, Serilog 3.1.1
- **Config**: nlohmann/json (header-only, bundled as submodule)

## Submodules
8 submodules in `src/third_party/` plus `src/CPP-parsers`. CPP-parsers has 5 nested extern submodules (inih, json, tinyxml2, tomlplusplus, yaml-cpp). Always use `--recursive`:
```bash
git submodule update --init --recursive
```

## Resources
- `AGENTS.md` — Detailed build notes and shared memory gotchas (overlaps with this file but has MCP server specifics)
- `README-WF.md` — Submodule licenses
- `.github/workflows/build.yml` — CI build pipeline (Windows + macOS)
- `TODO.md` — Remaining macOS polish items
- No project-level test infrastructure exists yet

### Core Component (`src/core/`)
- **Hardware Modules**: CPU (`CpuInfo`), GPU (`GpuInfo`), Memory (`MemoryInfo`), Disk (`DiskInfo`), Network (`NetworkAdapter`), OS (`OSInfo`) info collectors
- **Utilities**: `WMIManager` for WMI queries, `WinUtils` for Windows APIs, `Logger` for logging
- **LibreHardwareMonitorBridge**: C++/CLI bridge to the .NET LibreHardwareMonitor library for hardware sensors
- **SharedMemoryManager**: Manages shared memory block (`SharedMemoryBlock`) for inter-process communication

### Data Structures (`src/core/DataStruct/`)
- `DataStruct.h`: Primary data structures including `SystemInfo`, `SharedMemoryBlock`, hardware-specific structs
- `SharedMemoryManager.h/cpp`: Windows shared memory implementation for IPC between C++ core and TUI
- Structures are packed (`#pragma pack(push, 1)`) for consistent memory layout

### Communication
1. C++ core collects hardware data via LibreHardwareMonitor, WMI, and direct APIs
2. TUI reads from shared memory or receives data directly for real-time display
3. Shared memory includes synchronization via `CRITICAL_SECTION`

### Dependencies
- **CUDA 12.6**: NVIDIA Management Library (NVML) for GPU monitoring
- **LibreHardwareMonitor**: .NET hardware monitoring library

## Development Notes

### Code Style
- C++ code uses modern C++20 with C++/CLI extensions where needed
- Header files use `#pragma once`
- TUI uses PDCurses for terminal rendering
- Shared memory structures use fixed-size arrays for IPC compatibility

### Important Paths
- C++ Core: `src/core/` and `Project1/`
- TUI: `src/tui/`
- Third-party submodules: `src/third_party/` and `src/CPP-parsers/`
- Build output: `Project1/x64/{Configuration}/`

### Testing
Unit tests are planned but not yet implemented (see `docs/architecture_and_protocol_plan_v0.14_full.md`).

### Platform Support
Windows only (x64). Requires Windows 10/11 with appropriate drivers for hardware monitoring.

## Common Issues

### Submodule Dependencies
Ensure all submodules are initialized before building. The build will fail if LibreHardwareMonitor or CPP-parsers are missing.

### CUDA Installation
CUDA Toolkit must be installed and accessible in `%CUDA_PATH%`. The build expects CUDA 12.6.

### Mixed-Mode Assembly
The C++/CLI project references .NET Framework 4.7.2 assemblies. Ensure appropriate targeting packs are installed.

## References
- `.github/workflows/build.yml`: CI build configuration
- `README-WF.md`: License and submodule information
- `docs/architecture_and_protocol_plan_v0.14_full.md`: Architecture notes
