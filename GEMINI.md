# TCMT (Total Computer Monitor Tool)

TCMT is a high-performance, cross-platform hardware monitoring tool for Windows and macOS. It uses a C++ backend for data collection and an AvaloniaUI frontend for display.

## Project Overview

- **Core**: C++20 backend collecting hardware metrics (CPU, GPU, RAM, Disk SMART, Network, etc.).
- **Frontend**: AvaloniaUI (.NET 10.0) modern interface.
- **IPC**: Schema-driven protocol using Named Pipes (Windows) or Unix Domain Sockets (macOS) for control, and Shared Memory (SHM) for high-bandwidth data.
- **TUI**: ncurses/PDCurses dashboard available in the backend.
- **MCP**: Integrated Model Context Protocol server for AI agent hardware access.

## Project Structure & Navigation

- **Primary Index**: Always check `docs/repo-directory.md` first for file locations.
- **Session Status**: Check and update `docs/session.md` for current task status and user preferences.
- **Core Logic**: `src/core/` contains monitoring modules and coordination.
- **IPC Implementation**: `src/core/IPC/` (backend) and `AvaloniaUI/Services/IPCServices/` (frontend).
- **Submodules**: 8 submodules in `src/third_party/` plus `src/CPP-parsers/`.

## Building and Running

### Prerequisites
- **Git**: Always use `--recursive` for submodule updates:
  ```bash
  git submodule update --init --recursive
  ```

### macOS (Apple Silicon)
- **Backend (TCMT-M)**:
  ```bash
  cmake -B build -DCMAKE_BUILD_TYPE=Release
  cmake --build build -j8
  ./build/src/TCMT-M --json
  ```
- **Frontend (AvaloniaUI)**:
  ```bash
  dotnet build AvaloniaUI/AvaloniaUI.csproj -c Release -r osx-arm64
  ```

### Windows (x64, VS 2022/2026)
- **Backend**:
  ```bash
  msbuild TCMT.sln /p:Configuration=Release /p:Platform=x64
  ```
- **Frontend**:
  ```bash
  cd AvaloniaUI && dotnet build AvaloniaUI.csproj -c Release
  ```

## Development Conventions

- **Standards**: C++20 (`std::jthread`, `std::atomic`). No compilation warnings allowed.
- **Platform Separation**:
    - Windows: `*_Windows.cpp` or `#ifdef TCMT_WINDOWS`. Uses Win32 C++/CLI, WMI, PDH, NVML.
    - macOS: `*_macOS.cpp`, `.mm` files, or `#ifdef TCMT_MACOS`. Uses IOKit, SMC, Mach APIs.
- **Atomic Commits**: Each logical change should be a separate, independent commit.
- **IPC Schema**:
    - Handshake broadcasts `FieldDef` array.
    - Windows: Named Pipe is fire-and-forget; SHM name `Global\SystemMonitorSharedMemory`.
    - Strings: Windows uses `wchar_t` (UTF-16); macOS/Frontend use UTF-8.
    - Logic: Always read Float64 before Float32 in the mapper.
- **Logging**: Use thread-safe `Logger` (`src/core/Utils/Logger.h`).

## AI Agent Guidelines

- **MCP Mode**: Run with `--mcp`. It does NOT start an IPCServer; it tries to connect to an existing one first, then falls back to direct HW access.
- **Output Paths**: Do not hardcode paths. Infer from `build/CMakeCache.txt` or `.csproj` XML properties.
- **Code Changes**: Prefer Sonnet or Pro models for implementation.
- **Memory**: Do not cross-reference memory tiers. Keep project-specific notes in the private memory folder.
