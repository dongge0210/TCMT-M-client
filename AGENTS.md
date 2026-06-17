# AGENTS.md

## Build

```bash
# ─── macOS (Apple Silicon) ───
# C++ core (TCMT-M, ncurses TUI)
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j8
# IMPORTANT: run from .app bundle (not bare binary) for CoreLocation/SSID:
#   ./build/src/TCMT-M.app/Contents/MacOS/TCMT-M
# AvaloniaUI (separate, reads SHM)
dotnet build AvaloniaUI/AvaloniaUI.csproj -c Release -r osx-arm64

# ─── Windows (x64, VS 2022/VS 2026 ) ───
# Build order is critical (sln dependencies)
git submodule update --init --recursive
dotnet build src/third_party/LibreHardwareMonitor/LibreHardwareMonitorLib/LibreHardwareMonitorLib.csproj -c Release -f net472
msbuild src/CPP-parsers/CPP-parsers/CPP-parsers.vcxproj /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0 /m
msbuild TCMT.sln /p:Configuration=Release /p:Platform=x64 /p:PlatformToolset=v143 /p:WindowsTargetPlatformVersion=10.0 /m
cd AvaloniaUI && dotnet build AvaloniaUI.csproj -c Release
```

## Architecture

- **IPC**: Schema-driven pipeline. `IPCServer` broadcasts field offsets over Unix socket (macOS) / NamedPipe (Windows). C# `IPCPipeClient` receives schema, `IPCMemoryReader` reads from shared memory (`IPCDataBlock` on macOS, `SharedMemoryBlock` on Windows).
- **MCP**: `TCMT-M --mcp` — JSON-RPC 2.0 over stdio, 8 hardware tools. Reads from running TCMT via IPC or falls back to direct HW.
- **UI**: AvaloniaUI (.NET 10.0, cross-platform) or ncurses TUI (cross-platform)
- **C++ entry**: `src/main.cpp` (Windows), `src/main_mac.cpp` (macOS) — not interchangeable

## IPC Gotchas (critical)

1. **Windows shared memory name**: `Global\SystemMonitorSharedMemory` (with Local/no-prefix fallback). C# must try all 3.
2. **Float64 first**: mapper must use `ReadFloat64` before `ReadFloat32` — the latter reads 4 bytes of an 8-byte double and blocks the fallback.
3. **WString on Windows**: all string fields in SharedMemoryBlock are `wchar_t` (UTF-16). Use `ReadWString`, not `ReadString`.
4. **Windows NamedPipe**: fire-and-forget (no handshake). C# skips HELLO/HELLO_ACK on Windows.
5. **MCP mode**: NEVER starts IPCServer. External IPC first, direct HW fallback. Non-blocking connect (500ms timeout).
6. **Schema MaxFields**: 200 (both `IPCData.h` and `IPCSchema.cs`). Keep in sync.
7. **IPCServer**: multi-threaded (detached per client) on macOS; simple fire-and-forget on Windows.

## Submodules

8 submodules in `src/third_party/` plus `src/CPP-parsers/`. CPP-parsers (dongge0210 fork) has **5 nested extern submodules** (inih, json, tinyxml2, tomlplusplus, yaml-cpp). Always use `--recursive`:
```bash
git submodule update --init --recursive
```
AGENTS.md previously claimed 9 submodules — FFmpeg is listed in `.gitmodules` history but does not exist in the current checkout.

## CPP-parsers

Unified config parser (JSON/YAML/XML/TOML/INI) via `IConfigParser` interface + `ConfigParserFactory`. On macOS only JSON is available (via nlohmann/json header-only lib in `extern/json/single_include/`). The factory (`ConfigParserFactory.h`) includes ALL parser backends — do NOT include it on macOS unless all 5 extern libs are built first.

## ConfigManager

Located at `src/core/Config/ConfigManager.h`. Uses nlohmann/json directly (not through IConfigParser). Loaded on macOS startup from `system_monitor.json` in project root. Currently NOT wired into Windows `main.cpp`.

## Output Paths

Do NOT hardcode build output paths. Use the MCP server's inference:
- `.csproj` → parsed from XML (`OutputType`, `TargetFramework`, `AssemblyName`, `-r` flag)
- `.vcxproj` → parsed from XML (`ConfigurationType`, `TargetName`, MSBuild defaults for OutDir)
- CMake → read from `build/CMakeCache.txt` (`CMAKE_RUNTIME_OUTPUT_DIRECTORY`)

## Known stale / wrong claims (from earlier AGENTS.md)

- `FFmpeg` submodule — does not exist in `.gitmodules` (only 8, not 9)
- macOS output `build/` — actual C++ binary is `build/src/TCMT-M` (not `build/bin/TCMT-M`)
- macOS requires `brew install ncurses` — true, but `find_package(Curses)` in CMake handles it

## User additional notices
- **structure**: If need to check the location of file or menu, please check `docs/repo-directory.md` **FRIST**.
- **sessions**: change `docs/session.md` when current status changed also you see current status from that file.