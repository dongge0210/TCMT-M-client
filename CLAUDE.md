# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Cross-platform hardware monitoring tool (alpha-0.2, GPL-3.0). Monitors CPU, GPU, memory, disk, network, WiFi, Bluetooth, OS, and temperature data, exposing results via shared memory IPC for a desktop UI or TUI.

**Two separate build systems co-exist:**
- **CMake** — C++20 core library (`TCMTCore` static lib) + CLI entry point (`TCMT-M`). Runs on both macOS ARM64 and Windows x64.
- **MSBuild** — Windows-only C++/CLI app (`TCMT.exe`, .NET Framework 4.7.2) with an AvaloniaUI .NET 10.0 frontend.

**CRITICAL: When adding new source files (.cpp/.h/.c/.mm), you MUST register them in ALL build systems:**
1. **`TCMT.vcxproj`** — `<ClCompile Include="...">` for .cpp/.c/.mm, `<ClInclude Include="...">` for .h
2. **`TCMT.vcxproj.filters`** — matching entries in both ItemGroups
3. **`src/CMakeLists.txt`** — `list(APPEND CORE_MODULES_SOURCES ...)` or appropriate variable
Missing a build system causes linker errors on one platform while the other builds fine.

## Build Commands

### macOS (Apple Silicon)
```bash
# C++ core + ncurses TUI
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j8
# AvaloniaUI desktop frontend
dotnet build AvaloniaUI/AvaloniaUI.csproj -c Release -r osx-arm64
# Run
./build/src/TCMT-M
```
macOS requires `brew install ncurses` (CMake's `find_package(Curses)` handles it).

### Windows (x64, VS 2022 / 2026)
Build order is critical (see `.github/workflows/build.yml` for CI reference):
```bash
git submodule update --init --recursive

# 1. CPP-parsers
msbuild src/CPP-parsers/CPP-parsers/CPP-parsers.vcxproj /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0 /m

# 2. Main C++/CLI app
msbuild TCMT.sln /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0 /m

# 3. AvaloniaUI
dotnet build AvaloniaUI/AvaloniaUI.csproj -c Release
```
Prerequisites: VS 2022 / 2026 with C++/CLI support, CUDA Toolkit 13.2+, .NET 8.0+ SDK.

## Architecture

### Data Flow
```
C++ Core (TCMT-M / TCMT.exe)
  │  Collects: CPU, GPU, Memory, Disk, Network, WiFi, Bluetooth, OS, Temperature
  │  Sources: WMI (Windows), IOKit/sysctl (macOS), PawnIO (Windows kernel),
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
  - Coordinator: `coordinator/ModuleCoordinator` — 6 background jthread loops (CPU/Memory/Disk/Network/Temp/Power), `Snapshot()` feeds main loop
  - IOReport: `ioreport/IOReportSampler.mm` (macOS-only, private framework, no sudo)
  - PowerMonitor: `power/PowerMonitor.h/.cpp` (cross-platform power/freq API)
  - Notifications: `notifications/` — DeviceChangeNotifier (USB/BT hotplug), UserNotifier (desktop toasts), SystemEventMonitor (sleep/wake/disk/network/thermal callbacks)
  - Config: `Config/ConfigManager.cpp` (wraps CPP-parsers IConfigParser + nlohmann/json)
  - Utils: `Logger.cpp` (async producer-consumer, 10MB rotation), `WMIManager`, `JThreadCompat.h`
- `src/main_mac.cpp` — macOS entry (pure C++, ncurses TUI, ObjC++ via .mm modules)
- `src/main.cpp` — Windows entry (C++/CLI, Console app with SEH)
- `src/tui/` — ncurses TUI (cross-platform; PDCurses on Windows, ncurses on macOS)
- `src/DataStruct.h` — Packed `SharedMemoryBlock`. **`WCHAR`** = `char16_t` on macOS, `wchar_t` on Windows. Do NOT change.
- `AvaloniaUI/Services/IPCServices/` — C# IPC pipeline (IPCPipeClient, IPCMemoryReader, IPCSchema, IPCSystemInfoMapper)

### PawnIO Kernel Driver (Windows-only)

PawnIO is a Windows kernel driver (`namazso/PawnIO.Modules`) that allows user-mode programs
to execute signed kernel modules for raw hardware access — SMBus, LPC/Super I/O, MSR, and PCI
config space.

- **Driver**: the PawnIO installer (`PawnIO_setup.exe`) creates `\\.\GLOBALROOT\Device\PawnIO`.
- **Modules (.bin)**: signed, pre-compiled binaries from the official 0.1.6 release:
  https://github.com/namazso/PawnIO.Modules/releases/tag/0.1.6
  (single `release_0_1_6.zip`, extracted into `Resources/PawnIo/*.bin` in the LHM submodule).
  Our `src/resources.rc` embeds a subset as RCDATA for the PawnIOWrapper.
- **Wrapper**: `src/core/memory/PawnIOWrapper.h/.cpp` — `CreateFileW` + `DeviceIoControl`.
- **Consumers**:
  - `CpuTempReader` — Intel CPU temp via MSR (IA32_THERM_STATUS)
  - `MemoryTempReader` — DDR5 SPD Hub + LM75 temp via SMBus (I801, PIIX4, NCT6793, Skylake IMC)
  - `BoardTempReader` — NCT6798D motherboard sensors via LpcIO (Super I/O port I/O)
- **Detection**: `PawnIOWrapper::IsInstalled()` checks
  `HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\PawnIO` and the device path.
  All consumers are **opt-in** — if the driver is not installed, they silently return empty results.

## Platform-Specific Files

Some `.cpp` / `.mm` files are compiled on ONE platform only:
- `WiFiInfo.mm` — macOS only (CoreWLAN ObjC++). Windows: `WiFiInfo.cpp` + `WiFiInfo_wlan.c`.
- `BluetoothInfo.mm` — macOS only (IOBluetooth ObjC++). Windows: `BluetoothInfo.cpp`.
- `TemperatureWrapper.cpp` — Cross-platform (PawnIO + NVML on Windows, SMC on macOS, sysfs on Linux).
- `Platform_Windows.cpp` — Windows only.
- `IOReportSampler.mm` — macOS only (private IOReport.framework + IOKit pmgr, no sudo).

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

### CsWin32 / P/Invoke Rules (CRITICAL)

The LibreHardwareMonitor submodule uses **CsWin32 v0.3.275** to generate Windows API wrappers from `NativeMethods.txt`. This means:

15. **NEVER manually write `[DllImport]` stubs** for functions already listed in `NativeMethods.txt`. CsWin32 will generate the correct signatures with safe `SafeHandle` return types, `in` ref params, and proper enum types. Manual stubs cause signature mismatches with callers written for CsWin32-generated overloads.
16. **Callers MUST use `PInvoke.SetupDiXxx(...)` prefix**, not bare `using static` imports from a manually-written stub class.
17. **Concrete SafeHandle types matter**: CsWin32 generates typed safe handles (e.g. `SetupDiDestroyDeviceInfoListSafeHandle`). Don't pass the base `SafeHandle` class — cast to the concrete type when needed.
18. **Enums, not ints**: CsWin32 overloads accept `SETUP_DI_REGISTRY_PROPERTY` enum directly. Don't cast to `uint`.

**What happened (2026-05-12)**: A manual `SetupApi.cs` was added with raw pointer DllImport signatures (`Guid*`, `SP_DEVINFO_DATA*`, returns `HDEVINFO`). This conflicted with `BatteryGroup.cs` and `Stm32PortFinder.cs`, which were written for CsWin32-generated safe wrappers (returns `SetupDiDestroyDeviceInfoListSafeHandle`, takes `Guid` by value, uses `ref`/`in`). Fix: deleted `SetupApi.cs` (functions already in `NativeMethods.txt`), switched callers to `PInvoke.` prefix, and fixed helper methods to match the generated overloads.

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

## User addition notices
- **structure**: If need to check the location of file or menu, please check `docs/repo-directory.md` **FRIST**.
- **sessions**: change `docs/session.md` when current status changed also you see current status from that file.
- **read-scope**: **Always use `Read` with `offset` and `limit` parameters.** Never read entire files — read only the relevant range.

## Scope & Boundaries
- **Stay focused**: When debugging, focus narrowly on the specific error or crash. Do NOT deep-dive into unrelated internals, propose tangential refactors, or expand the scope without asking.
- **No external actions**: Do NOT create GitHub issues, PRs, or external tickets unless explicitly asked. Never push to `main` without confirmation.
- **Plan before large changes**: For features touching >3 files or adding new modules, present a brief plan before coding.

## Repository Safety Rules
- Before any file edit, verify working directory with `pwd` and `git status`.
- When creating a PR, always ask the user whether it should be **draft** or **ready** first. Never assume one or the other.
- All PRs target `dev` unless told otherwise.

## Edit Verification
- After every code change on macOS, run `cmake --build build -j8` and confirm 0 errors. Use fixed `-j8`, NOT `$(nproc)` or `$(sysctl -n hw.ncpu)` — command substitution breaks auto-approval.
- After every code change on Windows, the build command is `msbuild TCMT.sln /p:Configuration=Release /p:Platform=x64 /m`.
- **Zero warnings tolerance** — fix any warning immediately, do not suppress or ignore.