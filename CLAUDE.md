# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Cross-platform hardware monitoring tool (ALPHA_0.0.79, GPL-3.0). Monitors CPU, GPU, memory, disk, network, WiFi, Bluetooth, OS, and temperature data, exposing results via shared memory IPC for a desktop UI or TUI.

**Version.** One string, `ALPHA_<主>.<次>.<修订>` — the stage marker stays in front because
everything shipped so far is alpha and the wire format is not frozen. Six places carry a copy:

| 位置 | 值 | 备注 |
|---|---|---|
| `CMakeLists.txt` | `project(VERSION 0.0.79)` + `TCMT_VERSION_DISPLAY` | `project()` 只吃数字，`ALPHA_` 在 `TCMT_VERSION_DISPLAY` 里拼 |
| `src/core/DataStruct/DataStruct.h` | `TCMT_VERSION_STR` | 兜底值；CMake 正常构建时会覆盖它，但 vcxproj 那条路会用到 |
| `AvaloniaUI/Core/Constants.cs` | `AppConstants.AppVersion` | 界面离线时显示的那个 |
| `AvaloniaUI/TCMT.Avalonia.csproj` | `<AssemblyVersion>` | 同样只能填数字，不带 `ALPHA_` |
| `src/core/MCP/MCPServer.cpp` | `SERVER_VERSION` | MCP 对客户端报的版本 |
| 这一行 | | |

They have drifted twice already — that is how `alpha-0.2` (2026-05-11, only two files touched)
and `2.0.0` (2026-06-17 merge) got in. There is no single source; change all six together.

`ALPHA_0.0.79` is 11 characters and fits `appVersion[16]`. **Adding a git suffix would not** —
`ALPHA_0.0.79-129ead3a` is 20 and would be truncated at 15; that change needs the field widened
first.

**Two separate build systems co-exist:**
- **CMake** — C++20 core library (`TCMTCore` static lib) + CLI entry point (`TCMT-M`). Runs on both macOS ARM64 and Windows x64. **Linux is deliberately not a supported target** (no machine to validate readings against; the `TCMT_LINUX` paths are a half-wired scaffold that fails at link). `cmake/Platform.cmake` warns at configure time and prints `[UNSUPPORTED]` (silence it with `-DTCMT_ALLOW_UNSUPPORTED_LINUX=ON`); a Linux build may still fail at link.
- **MSBuild** — Windows-only C++/CLI app (`TCMT.exe`, .NET Framework 4.7.2) with an AvaloniaUI .NET 10.0 frontend.

**CRITICAL: When adding new source files (.cpp/.h/.c/.mm), you MUST register them in ALL build systems:**
1. **`TCMT.vcxproj`** — `<ClCompile Include="...">` for .cpp/.c/.mm, `<ClInclude Include="...">` for .h
2. **`TCMT.vcxproj.filters`** — matching entries in both ItemGroups
3. **`src/CMakeLists.txt`** — `list(APPEND CORE_MODULES_SOURCES ...)` or appropriate variable
Missing a build system causes linker errors on one platform while the other builds fine.

Three exceptions / caveats (2026-10-13):

- **`src/tools/` is deliberately unregistered.** Files there are on-demand, hand-compiled
  offline utilities (e.g. `sensor_recorder.mm` carries its own `clang++` command line in its
  header). Not registering them is intentional — do not "fix" it.
- **Some files are compiled through a parent's `#include`, so registering them directly would
  double-compile.** Every platform implementation is pulled in this way: `Platform.cpp`
  includes `Platform_{Windows,macOS,Linux}.cpp`, and `SharedMemoryManager.cpp` includes
  `SharedMemoryManager_{Windows,macOS,Linux}.cpp`, each behind an `#ifdef TCMT_*` block. Those
  six files must not appear in any build list — the parent is what gets registered. (An audit
  once listed four of them as "never compiled"; they are compiled, just not by a direct entry.)
- **The stated symptom (a linker error) only appears when the symbol is referenced.** A file
  nothing calls is dropped silently, so a missing registration looks exactly like a clean
  build. After registering a new file, also confirm something actually *references* it.
  `AccelSensor.mm` is the one genuine case found this way: nothing includes `AccelSensor.h`
  and `main_mac.cpp` uses `SpsManager` instead, so it is superseded code, not a missing
  registration.

## Build Commands

### macOS (Apple Silicon)
```bash
# C++ core + ncurses TUI
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j8
# AvaloniaUI desktop frontend
dotnet build AvaloniaUI/AvaloniaUI.csproj -c Release -r osx-arm64
# Run — from inside the .app bundle, NOT the bare binary
# (CoreLocation / SSID need the bundle; the bare binary silently returns nothing)
./build/src/TCMT-M.app/Contents/MacOS/TCMT-M
```
macOS requires `brew install ncurses` (CMake's `find_package(Curses)` handles it).

### Windows (x64, VS 2022 / 2026)
Build order is critical (see `.github/workflows/build.yml` for CI reference):
```bash
git submodule update --init --recursive

# 1. Main C++/CLI app
msbuild TCMT.sln /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0 /m

# 2. AvaloniaUI
dotnet build AvaloniaUI/AvaloniaUI.csproj -c Release
```
Prerequisites: VS 2022 / 2026 with C++/CLI support, CUDA Toolkit 13.2+, .NET 8.0+ SDK.

## Output Paths
Never hardcode a build output path. Ask the MCP server to infer it:
- `.csproj` → parse the XML (`OutputType`, `TargetFramework`, `AssemblyName`, `-r` flag)
- `.vcxproj` → parse the XML (`ConfigurationType`, `TargetName`, MSBuild defaults for `OutDir`)
- CMake → read `build/CMakeCache.txt` (`CMAKE_RUNTIME_OUTPUT_DIRECTORY`)

## Architecture

### Data Flow
```
C++ Core (TCMT-M / TCMT.exe)
  │  Collects: CPU, GPU, Memory, Disk, Network, WiFi, Bluetooth, OS, Temperature
  │  Sources: WMI (Windows), IOKit/sysctl (macOS), PawnIO (Windows kernel),
  │           NVML/CUDA (Windows), CoreWLAN/IOBluetooth (macOS), WLAN API (Windows)
  ▼
IPCDataBlock (single schema-driven block, both platforms)
  │  Same field layout everywhere; only the transport differs — Unix socket (macOS) / NamedPipe (Windows)
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
  - IOReport: `power/PowerMonitor_mac.mm` (macOS-only, private framework, no sudo; the macOS impl of `PowerMonitor`)
  - PowerMonitor: `power/PowerMonitor.h/.cpp` (cross-platform power/freq API)
  - Notifications: `notifications/` — DeviceChangeNotifier (USB/BT hotplug), UserNotifier (desktop toasts), SystemEventMonitor (sleep/wake/disk/network/thermal callbacks)
  - Config: `Config/ConfigManager.h/.cpp` — loads `system_monitor.json` from the project root at startup (nlohmann/json, header-only via `src/third_party/json/single_include/`). **Not wired into Windows `main.cpp` yet.**
  - Utils: `Logger.cpp` (async producer-consumer, 10MB rotation), `WMIManager`, `JThreadCompat.h`, `LogBuffer.h` (TUI log ring; lives here, not in `src/tui/`, so `core` does not depend on the TUI)
  - Experimental, macOS-only motion/ALS line, **not** part of the shipped UI: `accel/` +
    `als/` plus a localhost HTTP side channel (`HTTPServer/MotionHTTPServer.cpp`). Live path is
    `SpsManager` (SPU HID sensors) with `ALSensor` as the ALS fallback; `AccelSensor.mm` is a
    superseded earlier design and is called by nothing. The endpoint is off unless
    `motion_http.enabled=true` in `system_monitor.json` (there is no CLI flag) and serves
    `127.0.0.1:9876/sensors/motion` with `{ax,ay,az,gx,gy,gz,lidAngle,hb,imut}`. No IPC field
    carries motion data, and the only consumer in this repo is the design prototype
    `designs/attitude-3d.html` + `designs/ipc-client.js` (its `MotionClient` polls that URL every
    200 ms). Note `tools/mcp-http-bridge.mjs` also defaults to port 9876 — they cannot run at the
    same time. Reverse-engineering reference: `docs/apple-silicon-private-apis.md`.
- `src/main_mac.cpp` — macOS entry (pure C++, ncurses TUI, ObjC++ via .mm modules)
- `src/main.cpp` — Windows entry (C++/CLI, Console app with SEH)
- `src/tui/` — ncurses TUI (cross-platform; PDCurses on Windows, ncurses on macOS)
- `src/core/DataStruct/DataStruct.h` — collector-side structs (`SystemInfo`, `DiskData`, `GPUData`, `TemperatureData`, `NetworkAdapterData`, `SmartAttributeData`, …). **`WCHAR`** = `char16_t` on macOS, `wchar_t` on Windows (`using WCHAR`, lines 23/25). Do NOT change.
- `AvaloniaUI/Services/Hardware/IPC/` — C# IPC pipeline (IPCPipeClient, IPCMemoryReader, IPCSchema, IPCSystemInfoMapper)

### PawnIO Kernel Driver (Windows-only)

PawnIO is a Windows kernel driver (`namazso/PawnIO.Modules`) that allows user-mode programs
to execute signed kernel modules for raw hardware access — SMBus, LPC/Super I/O, MSR, and PCI
config space.

- **Driver**: the PawnIO installer (`PawnIO_setup.exe`) creates `\\.\GLOBALROOT\Device\PawnIO`.
- **Modules (.bin)**: signed, pre-compiled binaries from the official 0.2.8 release:
  https://github.com/namazso/PawnIO.Modules/releases/tag/0.2.8
  (single `release_0_2_8.zip`). Stored in `src/resources/pawnio/`, version tracking
  in `src/resources/pawnio/.version`. Auto-updated weekly via
  `.github/workflows/update-pawnio.yml`.
  Our `src/resources.rc` embeds the subset we use as RCDATA.
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
- `PowerMonitor_mac.mm` — macOS only (private IOReport.framework + IOKit pmgr, no sudo). The macOS impl of `PowerMonitor`; `PowerMonitor.cpp` is the non-macOS stub.
- `DisplayInfo.mm` — macOS only (NSScreen / CGDisplay). No non-macOS implementation and no non-macOS caller.

**Convention — do not put a platform guard inside these files.** Which platform compiles
them is decided by the build system (`.mm` files sit under `if(TCMT_MACOS)` in
`src/CMakeLists.txt`; their Windows counterparts are registered in `TCMT.vcxproj`), so an
`#ifdef TCMT_MACOS` inside a `.mm` is tautological and its `#else` can never be reached.
If you want a guard anyway, make it `#error`, never a stub. A stub turns "no
implementation on this platform" into "the reading is zero", which is indistinguishable
from a genuine zero and fails silently. This has already bitten us once (`WiFiInfo.mm`,
`DisplayInfo.mm`, `BluetoothInfo.mm` each carried an unreachable stub branch until
2026-10-12).

## IPC / Shared Memory Gotchas

### Cross-platform
1. **macOS `wchar_t` is 4 bytes** — `DataStruct.h` uses `WCHAR` typedef (`char16_t` on non-Windows). Do NOT revert.
2. **One shared memory block, two transports**: `IPCDataBlock` (`src/core/IPC/IPCData.h`) is the only block on both platforms — there is no `SharedMemoryBlock`. The platform difference is *how the bytes are reached* (POSIX shm + Unix socket on macOS, NamedPipe + `MemoryMappedViewAccessor` on Windows), **not the field layout**. Names and offsets are identical everywhere because the schema is broadcast; never hand-align the two sides.
3. **Schema MaxFields=300** (`IPC_MAX_FIELDS`) — must match in both `IPCData.h` and `IPCSchema.cs`. Bump both when adding fields. Going over the limit is **silent**: `IPCClient.cpp:184` drops the extras, so overflow shows up as *missing fields*, not as an error. (Was 200 until 2026-05-16, when WiFi's schema crossed index 257 and got cut.)

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
- `$(ProjectDir)src\third_party\json\single_include` — nlohmann/json (header-only)

## Submodules
11 submodules in `src/third_party/` (incl. nlohmann/json at `src/third_party/json`). Always use `--recursive`:
```bash
git submodule update --init --recursive
```

## Key Dependencies
- **Windows**: CUDA 13.2, WMI, PDH, WLAN API (wlanapi.lib), Bluetooth API (Bthprops.lib)
- **macOS**: IOKit, CoreFoundation, CoreWLAN, IOBluetooth, ncurses
- **AvaloniaUI**: Avalonia 12.0.1, CommunityToolkit.Mvvm 8.2.2, Serilog 3.1.1
- **Config**: nlohmann/json (header-only submodule at `src/third_party/json`)

## Logging Conventions

- **Default runtime level is `LOG_INFO`** in all three entry points (`main.cpp` / `main_mac.cpp` / `main_linux.cpp`). Lower it per run with `--debug` (LOG_DEBUG) or `--verbose` (LOG_INFO, a no-op at the default); explicit `logging.level` in config overrides the default — valid values are `debug` / `info` / `warning` / `error` — and CLI flags win over config.
- Level semantics: FATAL = unrecoverable / process dying; ERROR = feature actually broken, not recovered; WARN = transient, degraded but working; INFO = lifecycle / user-visible state changes only (never in sampling or per-tick loops); DEBUG = probing, capability absence, per-tick detail. See `src/core/Utils/Logger.h`.
- Detection of missing optional capabilities ("not found / not installed / not available / not supported / needs root / expected / skipping") is DEBUG — one of these in an INFO/WARN/ERROR message is a bug to fix.
- Defensive try/catch inside small formatting/util helpers logs DEBUG; the owning collector reports the real error at its own level.
- Never call `Logger::` from hot loops at INFO+; loop-body failures repeat every tick and flood the 2000-line ring buffer and log file.

## User addition notices
- **structure**: If need to check the location of file or menu, please check `docs/repo-directory.md` **FRIST**. Its file tree is generated by `tools/gen-repo-directory.mjs` (run it after adding or moving files), **but the `-- 说明` column is carried over from the previous copy of the document** — re-running never corrects a stale description and never fills in a new one. To fix a description, edit that line in `docs/repo-directory.md` itself; the next run carries the corrected text forward.
- **sessions**: change `docs/session.md` when current status changed also you see current status from that file.
- **read-scope**: **Always use `Read` with `offset` and `limit` parameters.** Never read entire files — read only the relevant range.
- **no-section-sign**: **Never use the section sign (U+00A7, 节号) anywhere in this repository** — not in source, comments, docs, commit messages, or generated output. That character is not a project convention; it leaks in from generated documentation. Cite a section as `7.4 节` or the bare number `7.4`. The 4 occurrences under `src/third_party/` are upstream RFC citations inside submodules — leave those alone.
- **banned-words**: Do not use the word `契约` anywhere in this repository (source, comments, docs, commit messages, generated output, agent notes). The owner never writes it; it is agent-introduced jargon that keeps reappearing. Write `约定` / `接口约定` / `字段约定` instead. This list starts with the one word below and grows on request — check it before coining terminology:
  | 禁用 | 改用 |
  |---|---|
  | `契约` | `约定` / `接口约定` / `字段约定` |

## 已更正的历史错误（别再写回去）

下面这些说法都在本仓库的文档里出现过，而且全是错的。任意一条重新出现，说明有人在凭记忆写文档而不是去查：

| 曾经写过的错 | 真相 | 纠正时间 |
|---|---|---|
| Schema `MaxFields=200` | 300；超限是静默截断，不是报错 | 2026-05-16 |
| `src/third_party/` 有 9 个（后来改成 10 个）子模块 | 11 个。FFmpeg 只出现在 `.gitmodules` 的历史里，当前 checkout 从来没有过 | 2026-09-03 |
| macOS 产物在 `build/bin/TCMT-M` | 在 `build/src/TCMT-M`，且运行要进 `.app` 包内那个（裸二进制拿不到 CoreLocation/SSID） | 2026-10-12 |
| AGENTS.md 是一份独立的规则文件 | 它只是 CLAUDE.md 的并行副本，已经漂移出错误内容，现压成一行指针 | 2026-10-12 |
| `IPCDataBlock` (macOS) + `SharedMemoryBlock` (Windows) 两块，字段名与偏移随平台变 | 只有一块 `IPCDataBlock`；平台差异在传输方式（POSIX shm+UDS ↔ NamedPipe），布局全平台一致 | 2026-10-13 |
| `src/DataStruct.h` — Packed `SharedMemoryBlock` | 真身 `src/core/DataStruct/DataStruct.h`，定义的是采集侧结构体（`SystemInfo` / `DiskData` / `GPUData` …），没有 `SharedMemoryBlock` | 2026-10-13 |
| 「CsWin32 / P/Invoke Rules」整节（原 15-18 条 + `SetupApi.cs` 事件） | 整节作废。LibreHardwareMonitor 子模块已由 `d98e601f` 移除；全仓已无 CsWin32 包引用、无 `NativeMethods.txt`、无 `PInvoke.` 调用（手动 `DllImport` 另存 5 处，与本规则无关） | 2026-10-13 |
| `AvaloniaUI/Services/IPCServices/` | 目录不存在，实际是 `AvaloniaUI/Services/Hardware/IPC/` | 2026-10-13 |

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