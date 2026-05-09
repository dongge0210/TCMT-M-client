# AGENTS.md

## Build

```bash
# ─── macOS (Apple Silicon) ───
# C++ core (TCMT-M, ncurses TUI)
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(sysctl -n hw.ncpu)
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

- **IPC**: `SharedMemoryBlock` (packed C struct, 129KB) via POSIX shm_open (macOS) / MemoryMappedFile (Windows)
- **UI**: AvaloniaUI (.NET 10.0, cross-platform) or ncurses TUI (macOS-only)
- **C++ entry**: `src/main.cpp` (Windows), `src/main_mac.cpp` (macOS) — not interchangeable
- **Build tooling**: `tcmt-build.json` + MCP server at `tools/mcp-build/` (run via `uv run --directory tools/mcp-build tcmt-build-mcp`)

## Shared Memory (critical gotchas)

1. **macOS `wchar_t` is 4 bytes** — C# `ushort` expects 2 bytes. Fixed by `WCHAR` typedef in `DataStruct.h` (`char16_t` on non-Windows, `wchar_t` on Windows). Do NOT revert to plain `wchar_t` in shared memory structs.

2. **C# `Marshal.SizeOf` ≠ C++ `sizeof`** — Differs by 24 bytes (C# `byte[40]` vs C++ `pthread_mutex_t` 64 bytes for the `lock` field). The `lock` is the last field, so earlier fields are at correct offsets, but `logicalCores` etc. must be validated after any struct change.

3. **AvaloniaUI on macOS uses P/Invoke** — `SharedMemoryService.cs` calls `shm_open/mmap/munmap/close` directly via `[DllImport("libc")]`. The POSIX shared memory name transformation must match C++ `Platform_macOS.cpp`: strip `/`, truncate to 20 chars, prepend `/`.

4. **Serilog is NOT configured** — `Log.Debug()/Information()` calls compile but produce no output. The `DIAG` output in `InitializeMacOS()` uses `Console.Error.WriteLine` to bypass this. Do not assume `Log.*` works without configuring a sink.

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
