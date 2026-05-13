# TCMT Repository Directory (144 tracked files)

```
TCMT-Windows-client/
|
|-- .claude/
|   |-- settings.local.json                -- Claude Code workspace settings
|
|-- .gitattributes                          -- Git attributes (line endings)
|-- .gitignore                              -- Git ignore rules
|-- .gitmodules                             -- Git submodule references
|-- AGENTS.md                               -- Agent behavioral rules
|-- CLAUDE.md                               -- Claude Code project guidelines
|-- CMakeLists.txt                          -- Root CMake build (Windows + macOS)
|-- LICENSE                                 -- GPL-3.0
|-- README.md                               -- Project overview (zh)
|-- README-WF.md                            -- Submodule licenses
|-- TCMT.sln                                -- Visual Studio solution
|-- TCMT.vcxproj                            -- Visual Studio project (Win32 C++/CLI)
|-- TCMT.vcxproj.filters                    -- VS project filters
|
|-- .github/
|   |-- dependabot.yml                      -- Dependabot config
|   |-- workflows/
|       |-- build.yml                       -- CI build (Windows + macOS)
|       |-- sync-to-gitee-gitlab.yml        -- Mirror sync
|
|-- cmake/
|   |-- Dependencies.cmake                  -- find_package resolution
|   |-- Options.cmake                       -- Compile options and flags
|   |-- PDCurses.vcxproj                    -- PDCurses VS project
|   |-- Platform.cmake                      -- Win32 vs macOS detection
|
|-- docs/
|   |-- adr/
|   |   |-- 0001-schema-driven-ipc.md       -- ADR: Schema-driven IPC protocol
|   |   |-- 0002-mcp-pure-client-no-ipcserver.md -- ADR: MCP pure client architecture
|   |   |-- 0003-cpp-cli-bridge-librehardwaremonitor.md -- ADR: C++/CLI LHM bridge
|   |   |-- 0004-nvml-dynamic-loading.md    -- ADR: NVML dynamic loading
|   |   |-- 0005-unified-ipcserver-with-ifdef.md -- ADR: Unified IPCServer with #ifdef
|   |
|   |-- superpowers/specs/
|   |   |-- 2026-04-18-disk-smart-tpm-design.md     -- SMART + TPM design
|   |   |-- 2026-04-18-wpf-to-avalonia-migration.md -- WPF→Avalonia migration
|   |   |-- 2026-04-19-tcmt-ui-fixes-design.md      -- UI fixes design
|   |
|   |-- docs_architecture_and_protocol_plan_v0.14_full_Version11.md -- Architecture design doc v11
|   |-- repo-directory.md                   -- This file
|   |-- session-restore.md                  -- Session restore design notes
|
|-- src/
|   |-- CMakeLists.txt                      -- Core library + executable build
|   |-- main.cpp                            -- Windows entry (C++/CLI, SEH, WMI/PDH/NVML, Bluetooth/WiFi)
|   |-- main_mac.cpp                        -- macOS entry (ncurses TUI, mach/sysctl/IOKit, Bluetooth/WiFi)
|   |-- CPP-parsers/                        -- [submodule] Multi-format config parsers
|   |
|   |-- core/
|   |   |-- ModuleCoordinator.h             -- Module lifecycle coordinator header
|   |   |-- ModuleCoordinator.cpp           -- Module init, tick, shutdown dispatch
|   |   |-- ModuleCoordinator_cpu.cpp       -- CPU + memory module coordinator
|   |   |-- ModuleCoordinator_io.cpp        -- I/O module coordinator (disk/network/wifi/bluetooth)
|   |   |-- placeholder.cpp                 -- Per-module polling guard implementation
|   |   |
|   |   |-- bluetooth/
|   |   |   |-- BluetoothInfo.h             -- Bluetooth adapter + device info header
|   |   |   |-- BluetoothInfo.cpp           -- Windows Bluetooth APIs (winsock/BTH)
|   |   |   |-- BluetoothInfo.mm            -- macOS CoreBluetooth via IOKit
|   |   |
|   |   |-- Config/
|   |   |   |-- ConfigManager.h             -- JSON config: typed getters/setters
|   |   |   |-- ConfigManager.cpp           -- Config via nlohmann/json
|   |   |
|   |   |-- DataStruct/
|   |   |   |-- DataStruct.h                -- SharedMemoryBlock, GPU/disk/network/TPM/Bluetooth/WiFi structs
|   |   |   |-- SharedMemoryManager.h       -- Cross-platform SHM manager header
|   |   |   |-- SharedMemoryManager.cpp     -- Platform dispatch
|   |   |   |-- SharedMemoryManager_Windows.cpp -- CreateFileMapping/MapViewOfFile
|   |   |   |-- SharedMemoryManager_macOS.cpp    -- shm_open/mmap/ftruncate
|   |   |
|   |   |-- IPC/
|   |   |   |-- IPCClient.h                 -- IPC client header (connect, read, subscribe)
|   |   |   |-- IPCClient.cpp               -- Cross-platform IPC client
|   |   |   |-- IPCData.h                   -- IPCDataBlock, FieldDef, schema protocol
|   |   |   |-- IPCServer.h                 -- macOS UDS server header
|   |   |   |-- IPCServer.cpp               -- UDS listener + schema broadcast
|   |   |   |-- NamedPipeServer.h           -- Windows named pipe server header
|   |   |   |-- NamedPipeServer.cpp         -- Named pipe creation + schema send
|   |   |
|   |   |-- MCP/
|   |   |   |-- MCPServer.h                 -- MCP protocol server header
|   |   |   |-- MCPServer.cpp               -- JSON-RPC 2.0 MCP over stdin/stdout
|   |   |
|   |   |-- Platform/
|   |   |   |-- Platform.h                  -- SystemTime, CriticalSection, SharedMemory, mutex
|   |   |   |-- Platform.cpp                -- Platform-independent helpers
|   |   |   |-- Platform_Windows.cpp        -- Win32: CRITICAL_SECTION, privilege, error format
|   |   |   |-- Platform_macOS.cpp          -- POSIX: pthread_mutex, SMC, shm_open
|   |   |
|   |   |-- Utils/
|   |   |   |-- ComInitializationHelper.cpp -- CoInitializeEx wrapper
|   |   |   |-- JThreadCompat.h             -- jthread compatibility shim for older C++ std
|   |   |   |-- LibreHardwareMonitorBridge.h -- C++/CLI bridge header
|   |   |   |-- LibreHardwareMonitorBridge.cpp -- HW sensor + SMART bridge
|   |   |   |-- Logger.h                    -- 7-level thread-safe logger
|   |   |   |-- Logger.cpp                  -- File + console + TUI buffer output
|   |   |   |-- TimeUtils.h                 -- Boot time, uptime, timestamps
|   |   |   |-- TimeUtils.cpp               -- std::chrono helpers
|   |   |   |-- TpmBridge.h                 -- TPM 2.0 detection header
|   |   |   |-- TpmBridge.cpp               -- TBS API wrappers
|   |   |   |-- WMIManager.h                -- WMI COM service header
|   |   |   |-- WMIManager.cpp              -- WMI locator + IWbemServices
|   |   |   |-- WinUtils.h                  -- String encoding + privilege utilities
|   |   |   |-- WinUtils.cpp                -- UTF-8↔UTF-16, admin check, elevation
|   |   |   |-- stdafx.h                    -- Precompiled header
|   |   |
|   |   |-- board/
|   |   |   |-- BoardInfo.h                 -- Motherboard info struct
|   |   |   |-- BoardInfo.cpp               -- WMI Win32_BaseBoard / IOKit
|   |   |
|   |   |-- cpu/
|   |   |   |-- CpuInfo.h                   -- CPU detection header
|   |   |   |-- CpuInfo.cpp                 -- PDH (Win) / sysctl (macOS)
|   |   |
|   |   |-- disk/
|   |   |   |-- DiskInfo.h                  -- Disk/volume data header
|   |   |   |-- DiskInfo.cpp                -- Logical drive + physical disk SMART
|   |   |
|   |   |-- gpu/
|   |   |   |-- GpuInfo.h                   -- GPU detection header
|   |   |   |-- GpuInfo.cpp                 -- WMI + DirectX + NVML / IOKit + Metal
|   |   |
|   |   |-- history/
|   |   |   |-- HistoryLogger.h             -- SQLite sensor log header
|   |   |   |-- HistoryLogger.cpp           -- Background flush worker, retention
|   |   |
|   |   |-- memory/
|   |   |   |-- MemoryInfo.h                -- RAM stats header
|   |   |   |-- MemoryInfo.cpp              -- GlobalMemoryStatusEx / host_statistics64
|   |   |
|   |   |-- network/
|   |   |   |-- NetworkAdapter.h            -- NIC info header
|   |   |   |-- NetworkAdapter.cpp          -- GetAdaptersAddresses / getifaddrs + throughput
|   |   |
|   |   |-- os/
|   |   |   |-- OSInfo.h                    -- OS version header
|   |   |   |-- OSInfo.cpp                  -- RtlGetVersion / sysctl kern.osproductversion
|   |   |
|   |   |-- power/
|   |   |   |-- PowerInfo.h                 -- Battery + power plan header
|   |   |   |-- PowerInfo.cpp               -- WMI battery + IOKit AppleSmartBattery
|   |   |
|   |   |-- temperature/
|   |   |   |-- LibreHardwareMonitorBridge.h -- Temperature bridge header
|   |   |   |-- LibreHardwareMonitorBridge.cpp -- LHM thermal sensor enumeration
|   |   |   |-- TemperatureWrapper.h        -- Cross-platform temperature facade
|   |   |   |-- TemperatureWrapper.cpp      -- LHM (Win) / SMC+IOKit (macOS)
|   |   |
|   |   |-- usb/
|   |   |   |-- UsbInfo.h                   -- USB device header
|   |   |   |-- UsbInfo.cpp                 -- SetupAPI (Win) / IOService (macOS)
|   |   |
|   |   |-- wifi/
|   |       |-- WiFiInfo.h                  -- WiFi adapter + network info header
|   |       |-- WiFiInfo.cpp                -- Windows WLAN API
|   |       |-- WiFiInfo.mm                 -- macOS CoreWLAN
|   |       |-- WiFiInfo_wlan.h             -- WLAN helper header
|   |       |-- WiFiInfo_wlan.c             -- WLAN callback + scan helper
|   |
|   |-- third_party/
|   |   |-- LibreHardwareMonitor/           -- [submodule] .NET HW monitor lib
|   |   |-- PDCurses/                       -- [submodule] Windows console curses
|   |   |-- TC/                             -- [submodule] Terminal control
|   |   |-- USBMonitor-cpp/                 -- [submodule] USB hotplug monitor
|   |   |-- curl/                           -- [submodule] libcurl HTTP (unused)
|   |   |-- tpm2-tss/                       -- [submodule] TCG TPM 2.0 stack
|   |   |-- websocketpp/                    -- [submodule] WebSocket++ (unused)
|   |   |-- zlib/                           -- [submodule] Compression (unused)
|   |
|   |-- tui/
|       |-- LogBuffer.h                     -- 500-line thread-safe ring buffer
|       |-- TuiApp.h                        -- ncurses dashboard header
|       |-- TuiApp.cpp                      -- CPU/Mem/GPU/Disk/Net/Bluetooth/WiFi/TPM/Temp panels
|
|-- tests/
|   |-- TCMT.Tests.vcxproj                  -- VS test project
|   |-- test_config.cpp                     -- ConfigManager unit tests
|   |-- test_ipc.cpp                        -- IPC layer unit tests
|   |-- vcpkg.json                          -- vcpkg manifest
|
|-- AvaloniaUI/
    |-- Program.cs                          -- App entry, Serilog init
    |-- App.axaml                           -- Styles, tray icon, data templates
    |-- App.axaml.cs                        -- Window creation, service setup
    |-- AvaloniaUI.csproj                   -- .NET 10.0 project (Avalonia 12.0.1)
    |-- ViewLocator.cs                      -- VM→View resolution
    |-- app.manifest                        -- Windows DPIAware manifest
    |-- global.json                         -- .NET SDK pinning
    |
    |-- Assets/
    |   |-- avalonia-logo.ico               -- App icon
    |
    |-- Controls/
    |   |-- LineChart.cs                    -- Custom perf curve control
    |
    |-- Converters/
    |   |-- BoolInvertConverter.cs          -- Boolean inversion converter
    |   |-- ValueConverters.cs              -- NullToDefault, Bool/Percent/Temp color converters
    |
    |-- Models/
    |   |-- SystemInfo.cs                   -- All data models: GpuData, DiskData, TpmData, etc.
    |
    |-- Services/
    |   |-- IPCServices/
    |       |-- IPCMemoryReader.cs          -- Schema-driven dynamic SHM reader
    |       |-- IPCPipeClient.cs            -- Named pipe schema receiver
    |       |-- IPCSchema.cs                -- FieldDef, SchemaHeader, protocol constants
    |       |-- IPCService.cs               -- IPC orchestrator (pipe + memory)
    |       |-- IPCSystemInfoMapper.cs      -- Schema fields → SystemInfo mapper
    |
    |-- ViewModels/
    |   |-- MainWindowViewModel.cs          -- Main VM: timers, commands, sensor bindings
    |   |-- ViewModelBase.cs                -- ObservableObject base
    |
    |-- Views/
        |-- MainWindow.axaml                -- Main window XAML layout
        |-- MainWindow.axaml.cs             -- Code-behind
```