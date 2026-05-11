# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Cross-platform hardware monitoring tool (alpha-0.2, GPL-3.0). Monitors CPU, GPU, memory, disk, network, WiFi, Bluetooth, OS, and temperature data, exposing results via shared memory IPC for a desktop UI or TUI.

**Two separate build systems co-exist:**
- **CMake** — C++20 core library (`TCMTCore` static lib) + CLI entry point (`TCMT-M`). Runs on both macOS ARM64 and Windows x64.
- **MSBuild** — Windows-only C++/CLI app (`TCMT.exe`, .NET Framework 4.7.2) with LibreHardwareMonitor bridge and an AvaloniaUI .NET 10.0 frontend.

**CRITICAL: When adding new source files (.cpp/.h/.c/.mm), you MUST register them in ALL build systems:**
1. **`TCMT.vcxproj`** — `<ClCompile Include="...">` for .cpp/.c/.mm, `<ClInclude Include="...">` for .h
2. **`TCMT.vcxproj.filters`** — matching entries in both ItemGroups
3. **`src/CMakeLists.txt`** — `list(APPEND CORE_MODULES_SOURCES ...)` or appropriate variable
Missing a build system causes linker errors on one platform while the other builds fine.

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

### Windows (x64, VS 2022+)
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

## Architecture

### Data Flow
```
C++ Core (TCMT-M / TCMT.exe)
  │  Collects: CPU, GPU, Memory, Disk, Network, WiFi, Bluetooth, OS, Temperature
  │  Sources: WMI (Windows), IOKit/sysctl (macOS), LibreHardwareMonitor (Windows .NET),
  │           NVML/CUDA (Windows), CoreWLAN/IOBluetooth (macOS), WLAN API (Windows)
  ▼
IPCDataBlock (macOS) / SharedMemoryBlock (Windows)
  │  Schema-driven field offsets broadcasted over Unix socket (macOS) / NamedPipe (Windows)
  │
  ├── AvaloniaUI (.NET 10.0, cross-platform) — connects via IPCPipeClient, reads via IPCMemoryReader
  ├── ncurses TUI (macOS-only) — reads directly in main_mac.cpp
  └── MCP Server — `TCMT-M --mcp` (JSON-RPC 2.0 stdio, uses IPC for data)
```

### Key Modules
- `src/core/` — C++20 static library `TCMTCore`
  - Hardware: `cpu/`, `gpu/`, `memory/`, `disk/`, `network/`, `os/`, `power/`, `wifi/`, `bluetooth/`, `temperature/`, `board/`
  - IPC: `IPC/IPCServer.cpp` (unified UDS+Windows), `IPC/IPCClient.cpp`, `IPC/IPCData.h` (schema/protocol)
  - MCP: `MCP/MCPServer.cpp` (JSON-RPC 2.0 over stdio, 8 tools)
  - Coordinator: `ModuleCoordinator` — 6 background jthread loops (CPU/Memory/Disk/Network/Temp/Power), `Snapshot()` feeds main loop
  - Config: `Config/ConfigManager.cpp` (wraps CPP-parsers IConfigParser + nlohmann/json)
  - Utils: `Logger.cpp` (async producer-consumer, 10MB rotation), `WMIManager`, `JThreadCompat.h`
- `src/main_mac.cpp` — macOS entry (pure C++, ncurses TUI, ObjC++ via .mm modules)
- `src/main.cpp` — Windows entry (C++/CLI, Console app with SEH)
- `src/tui/` — ncurses TUI (cross-platform; PDCurses on Windows, ncurses on macOS)
- `src/DataStruct.h` — Packed `SharedMemoryBlock`. **`WCHAR`** = `char16_t` on macOS, `wchar_t` on Windows. Do NOT change.
- `AvaloniaUI/Services/IPCServices/` — C# IPC pipeline (IPCPipeClient, IPCMemoryReader, IPCSchema, IPCSystemInfoMapper)

## Platform-Specific Files

Some `.cpp` / `.mm` files are compiled on ONE platform only:
- `WiFiInfo.mm` — macOS only (CoreWLAN ObjC++). Windows: `WiFiInfo.cpp` + `WiFiInfo_wlan.c`.
- `BluetoothInfo.mm` — macOS only (IOBluetooth ObjC++). Windows: `BluetoothInfo.cpp`.
- `TemperatureWrapper.cpp` — Windows only (LibreHardwareMonitor bridge, C++/CLI).
- `Platform_Windows.cpp` — Windows only.

## IPC / Shared Memory Gotchas

### Cross-platform
1. **macOS `wchar_t` is 4 bytes** — `DataStruct.h` uses `WCHAR` typedef (`char16_t` on non-Windows). Do NOT revert.
2. **Two shared memory blocks**: `IPCDataBlock` (macOS, schema-driven) and `SharedMemoryBlock` (Windows, legacy). C# mapper reads BOTH; field names and offsets differ per platform.
3. **Schema MaxFields=200** — must match in both `IPCData.h` and `IPCSchema.cs`. Bump both when adding fields.

### Windows-specific (critical)
4. **Shared memory name**: C++ creates `Global\SystemMonitorSharedMemory` (fallback `Local\` then no prefix). C# must try all 3.
5. **Float64 first**: `ReadFloat32` on an 8-byte field reads 4 garbage bytes and returns non-null, blocking `ReadFloat64` fallback via `??`. Always try `ReadFloat64` FIRST.
6. **WString vs String**: Windows stores strings as `wchar_t` (UTF-16) in shared memory. Use `ReadWString` not `ReadString` for network, disk, temperature, SMART string fields.
7. **Windows NamedPipe**: fire-and-forget schema delivery (no handshake). C# skips HELLO/HELLO_ACK on Windows. macOS uses full handshake protocol.
8. **MCP mode**: NEVER starts IPCServer. External IPC first, direct HW fallback. Non-blocking connect with 500ms timeout.

### Windows Compiler / SDK Gotchas
9. **`std::min` / `std::max` macro collision** — `<windows.h>` defines `min`/`max` as macros. Use `(std::min)(a, b)` with parens to prevent macro expansion. Affects any file that includes windows.h (directly or transitively).
10. **`_CRT_SECURE_NO_WARNINGS`** — needed in `.c` files to suppress C4996 (`strncpy`/`snprintf`). Already defined in `WiFiInfo_wlan.c`; add to other new `.c` files.
11. **`dot11TxRate` removed in SDK 10.0.26100+** — `WLAN_ASSOCIATION_ATTRIBUTES.dot11TxRate` doesn't exist in newer Windows SDKs. WiFiInfo_wlan.c returns 0 for txRate.
12. **`BLUETOOTH_ADDRESS` union type** — field type varies across SDK versions. Use the union directly (`.ullLong` for BTH_ADDR), not the bare `BTH_ADDR` type.

### Apple Clang Gotchas
13. **No `std::jthread` / `std::stop_token`** on Xcode < 16 (Apple Clang). Use `tcmt::compat::StopToken` / `tcmt::compat::JThread` from `src/core/Utils/JThreadCompat.h`. Native `std::` types are aliased on MSVC.
14. **`.mm` files** are Objective-C++ (used for CoreWLAN, IOBluetooth). Compiled only on macOS via CMake. MSVC/vcxproj must NOT include them.

## vcxproj Include Paths

Windows build requires these include directories (set in `TCMT.vcxproj`):
- `$(ProjectDir)src\CPP-parsers\include` — IConfigParser.h
- `$(ProjectDir)src\CPP-parsers\src` — JsonConfigParser.h
- `$(ProjectDir)src\CPP-parsers\extern\json\single_include` — nlohmann/json
When adding new CPP-parsers dependencies, add their include paths here.

## Submodules
8 submodules in `src/third_party/` plus `src/CPP-parsers`. CPP-parsers has 5 nested extern submodules (inih, json, tinyxml2, tomlplusplus, yaml-cpp). Always use `--recursive`:
```bash
git submodule update --init --recursive
```

## Key Dependencies
- **Windows**: CUDA 13.2, LibreHardwareMonitor (MPL-2.0, compiled DLL), WMI, PDH, WLAN API (wlanapi.lib), Bluetooth API (Bthprops.lib)
- **macOS**: IOKit, CoreFoundation, CoreWLAN, IOBluetooth, ncurses
- **AvaloniaUI**: Avalonia 12.0.1, CommunityToolkit.Mvvm 8.2.2, Serilog 3.1.1
- **Config**: nlohmann/json (header-only, bundled in CPP-parsers submodule)
