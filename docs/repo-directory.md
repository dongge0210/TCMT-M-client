# TCMT Repository Directory

327 tracked files. 由 `git ls-files` 生成 —— 加完目录跑一次
`node tools/gen-repo-directory.mjs`，别手改这一份。

```
|-- .codegraph/
|   |-- .gitignore
|-- .github/
|   |-- agents/
|   |   |-- tcmt-dev.agent.md
|   |-- workflows/
|   |   |-- build.yml    -- CI build (Windows + macOS)
|   |   |-- msvc.yml
|   |   |-- sync-to-gitee-gitlab.yml    -- Mirror sync
|   |   |-- update-pawnio.yml
|   |-- dependabot.yml    -- Dependabot config
|-- AvaloniaUI/
|   |-- Assets/
|   |   |-- avalonia-logo.ico    -- App icon
|   |-- Core/
|   |   |-- Converters/
|   |   |   |-- Converters.axaml
|   |   |   |-- ValueConverters.cs    -- NullToDefault, Bool/Percent/Temp color converters
|   |   |-- Extensions/
|   |   |   |-- FormatUtils.cs
|   |   |-- Constants.cs
|   |   |-- HardwareSpecBuilder.cs
|   |   |-- HistoryBuffer.cs
|   |   |-- PlatformCaps.cs
|   |   |-- Sparkline.cs
|   |   |-- Thresholds.cs
|   |-- Localization/
|   |   |-- EnglishStrings.cs
|   |   |-- Translator.cs
|   |-- Models/
|   |   |-- HardwareSpec.cs
|   |   |-- SystemInfo.cs    -- All data models: GpuData, DiskData, TpmData, etc.
|   |-- Services/
|   |   |-- Contracts/
|   |   |   |-- IClientSettingsStore.cs
|   |   |   |-- IDialogService.cs
|   |   |   |-- IFontService.cs
|   |   |   |-- IHardwareService.cs
|   |   |   |-- INavigationService.cs
|   |   |   |-- IProcessController.cs
|   |   |-- Hardware/
|   |   |   |-- IPC/
|   |   |   |   |-- IPCMemoryReader.cs    -- Schema-driven dynamic SHM reader
|   |   |   |   |-- IPCPipeClient.cs    -- Named pipe schema receiver
|   |   |   |   |-- IPCSchema.cs    -- FieldDef, SchemaHeader, protocol constants
|   |   |   |   |-- IPCService.cs    -- IPC orchestrator (pipe + memory)
|   |   |   |   |-- IPCSystemInfoMapper.cs    -- Schema fields → SystemInfo mapper
|   |   |   |-- HardwareMonitorService.cs
|   |   |-- ClientSettingsStore.cs
|   |   |-- DialogService.cs
|   |   |-- FontService.cs
|   |   |-- LogStream.cs
|   |   |-- NavigationService.cs
|   |-- Themes/
|   |   |-- DarkTheme.axaml
|   |   |-- ThemeManager.cs
|   |-- ViewModels/
|   |   |-- DashboardViewModel.cs
|   |   |-- HardwareSpecViewModel.cs
|   |   |-- LogViewModel.cs
|   |   |-- MainWindowViewModel.cs    -- Main VM: timers, commands, sensor bindings
|   |   |-- NetworkViewModel.cs
|   |   |-- PanelRows.cs
|   |   |-- ProcessRow.cs
|   |   |-- ProcessesViewModel.cs
|   |   |-- SettingsViewModel.cs
|   |   |-- ViewModelBase.cs    -- ObservableObject base
|   |-- Views/
|   |   |-- DashboardView.axaml
|   |   |-- DashboardView.axaml.cs
|   |   |-- HardwareSpecView.axaml
|   |   |-- HardwareSpecView.axaml.cs
|   |   |-- LogView.axaml
|   |   |-- LogView.axaml.cs
|   |   |-- MainWindow.axaml    -- Main window XAML layout
|   |   |-- MainWindow.axaml.cs    -- Code-behind
|   |   |-- NetworkView.axaml
|   |   |-- NetworkView.axaml.cs
|   |   |-- ProcessesView.axaml
|   |   |-- ProcessesView.axaml.cs
|   |   |-- SettingsView.axaml
|   |   |-- SettingsView.axaml.cs
|   |-- _archive/
|   |   |-- ViewModels/
|   |   |   |-- CpuViewModel.cs
|   |   |   |-- GpuViewModel.cs
|   |   |   |-- MemoryViewModel.cs
|   |   |   |-- StorageViewModel.cs
|   |   |   |-- TemperaturesViewModel.cs
|   |   |-- Views/
|   |   |   |-- CpuView.axaml
|   |   |   |-- CpuView.axaml.cs
|   |   |   |-- GpuView.axaml
|   |   |   |-- GpuView.axaml.cs
|   |   |   |-- MemoryView.axaml
|   |   |   |-- MemoryView.axaml.cs
|   |   |   |-- StorageView.axaml
|   |   |   |-- StorageView.axaml.cs
|   |   |   |-- TemperaturesView.axaml
|   |   |   |-- TemperaturesView.axaml.cs
|   |   |-- README.md    -- Project overview (zh)
|   |-- designs/
|   |   |-- README.md    -- Project overview (zh)
|   |   |-- design-prototype.html
|   |-- App.axaml    -- Styles, tray icon, data templates
|   |-- App.axaml.cs    -- Window creation, service setup
|   |-- Program.cs    -- App entry, Serilog init
|   |-- TCMT.Avalonia.csproj
|   |-- app.manifest    -- Windows DPIAware manifest
|   |-- design-notes.md
|   |-- global.json    -- .NET SDK pinning
|-- cmake/
|   |-- Dependencies.cmake    -- find_package resolution
|   |-- Options.cmake    -- Compile options and flags
|   |-- PDCurses.vcxproj    -- PDCurses VS project
|   |-- Platform.cmake    -- Win32 vs macOS detection
|-- designs/
|   |-- vendor/
|   |   |-- three/
|   |   |   |-- build/
|   |   |   |   |-- three.module.js
|   |   |   |-- examples/
|   |   |   |   |-- jsm/
|   |   |   |   |   |-- controls/
|   |   |   |   |   |   |-- OrbitControls.js
|   |-- attitude-3d.html
|   |-- attitude-3d.js
|   |-- avalonia-ui.pen
|   |-- ipc-client.js
|-- docs/
|   |-- adr/
|   |   |-- 0001-schema-driven-ipc.md    -- ADR: Schema-driven IPC protocol
|   |   |-- 0002-mcp-pure-client-no-ipcserver.md    -- ADR: MCP pure client architecture
|   |   |-- 0004-nvml-dynamic-loading.md    -- ADR: NVML dynamic loading
|   |   |-- 0005-unified-ipcserver-with-ifdef.md    -- ADR: Unified IPCServer with #ifdef
|   |-- session/
|   |   |-- 2026-07-22-jmicron-smart.md
|   |-- specs/
|   |   |-- jm20329-limitation.md
|   |   |-- nvme-health-information-log.md
|   |   |-- windows-apis-for-hardware-monitoring.md
|   |-- superpowers/
|   |   |-- specs/
|   |   |   |-- 2026-04-18-disk-smart-tpm-design.md    -- SMART + TPM design
|   |   |   |-- 2026-04-18-wpf-to-avalonia-migration.md    -- WPF→Avalonia migration
|   |   |   |-- 2026-04-19-tcmt-ui-fixes-design.md    -- UI fixes design
|   |-- API.md
|   |-- apple-silicon-private-apis.md
|   |-- docs_architecture_and_protocol_plan_v0.14_full_Version11.md    -- Architecture design doc v11
|   |-- elements_BAN_list.md
|   |-- jmicron_TEST.txt
|   |-- linux-gap-analysis.md
|   |-- linux-support-scope.md
|   |-- macos27-power-and-tcmt-powerd.md
|   |-- repo-directory.md    -- This file
|   |-- session.md
|   |-- tech-debt.md
|-- scripts/
|   |-- codesign.sh
|   |-- install_helper.sh
|-- src/
|   |-- core/
|   |   |-- Config/
|   |   |   |-- ConfigManager.cpp    -- Config via nlohmann/json
|   |   |   |-- ConfigManager.h    -- JSON config: typed getters/setters
|   |   |-- DataStruct/
|   |   |   |-- DataStruct.h    -- SharedMemoryBlock, GPU/disk/network/TPM/Bluetooth/WiFi structs
|   |   |   |-- SharedMemoryManager.cpp    -- Platform dispatch
|   |   |   |-- SharedMemoryManager.h    -- Cross-platform SHM manager header
|   |   |   |-- SharedMemoryManager_Linux.cpp
|   |   |   |-- SharedMemoryManager_Windows.cpp    -- CreateFileMapping/MapViewOfFile
|   |   |   |-- SharedMemoryManager_macOS.cpp    -- shm_open/mmap/ftruncate
|   |   |-- HTTPServer/
|   |   |   |-- MotionHTTPServer.cpp
|   |   |   |-- MotionHTTPServer.h
|   |   |-- IPC/
|   |   |   |-- IPCClient.cpp    -- Cross-platform IPC client
|   |   |   |-- IPCClient.h    -- IPC client header (connect, read, subscribe)
|   |   |   |-- IPCData.h    -- IPCDataBlock, FieldDef, schema protocol
|   |   |   |-- IPCServer.cpp    -- UDS listener + schema broadcast
|   |   |   |-- IPCServer.h    -- macOS UDS server header
|   |   |-- MCP/
|   |   |   |-- MCPServer.cpp    -- JSON-RPC 2.0 MCP over stdin/stdout
|   |   |   |-- MCPServer.h    -- MCP protocol server header
|   |   |-- Platform/
|   |   |   |-- Platform.cpp    -- Platform-independent helpers
|   |   |   |-- Platform.h    -- SystemTime, CriticalSection, SharedMemory, mutex
|   |   |   |-- Platform_Linux.cpp
|   |   |   |-- Platform_Windows.cpp    -- Win32: CRITICAL_SECTION, privilege, error format
|   |   |   |-- Platform_macOS.cpp    -- POSIX: pthread_mutex, SMC, shm_open
|   |   |-- Utils/
|   |   |   |-- ComInitializationHelper.cpp    -- CoInitializeEx wrapper
|   |   |   |-- JThreadCompat.h    -- jthread compatibility shim for older C++ std
|   |   |   |-- LogBuffer.h    -- 500-line thread-safe ring buffer
|   |   |   |-- Logger.cpp    -- File + console + TUI buffer output
|   |   |   |-- Logger.h    -- 7-level thread-safe logger
|   |   |   |-- Sysfs.h
|   |   |   |-- TimeUtils.cpp    -- std::chrono helpers
|   |   |   |-- TimeUtils.h    -- Boot time, uptime, timestamps
|   |   |   |-- TpmBridge.cpp    -- TBS API wrappers
|   |   |   |-- TpmBridge.h    -- TPM 2.0 detection header
|   |   |   |-- WMIManager.cpp    -- WMI locator + IWbemServices
|   |   |   |-- WMIManager.h    -- WMI COM service header
|   |   |   |-- WinUtils.cpp    -- UTF-8↔UTF-16, admin check, elevation
|   |   |   |-- WinUtils.h    -- String encoding + privilege utilities
|   |   |   |-- stdafx.h    -- Precompiled header
|   |   |-- accel/
|   |   |   |-- SpsManager.h
|   |   |   |-- SpsManager.mm
|   |   |-- als/
|   |   |   |-- ALSensor.h
|   |   |   |-- ALSensor.mm
|   |   |-- battery/
|   |   |   |-- BatteryHealth.h
|   |   |   |-- BatteryHealth.mm
|   |   |-- bluetooth/
|   |   |   |-- BluetoothInfo.cpp    -- Windows Bluetooth APIs (winsock/BTH)
|   |   |   |-- BluetoothInfo.h    -- Bluetooth adapter + device info header
|   |   |   |-- BluetoothInfo.mm    -- macOS CoreBluetooth via IOKit
|   |   |   |-- BluetoothInfo_linux.cpp
|   |   |-- board/
|   |   |   |-- BoardInfo.cpp    -- WMI Win32_BaseBoard / IOKit
|   |   |   |-- BoardInfo.h    -- Motherboard info struct
|   |   |-- coordinator/
|   |   |   |-- ModuleCoordinator.cpp    -- Module init, tick, shutdown dispatch
|   |   |   |-- ModuleCoordinator.h    -- Module lifecycle coordinator header
|   |   |   |-- ModuleCoordinator_cpu.cpp    -- CPU + memory module coordinator
|   |   |   |-- ModuleCoordinator_io.cpp    -- I/O module coordinator (disk/network/wifi/bluetooth)
|   |   |-- cpu/
|   |   |   |-- CpuInfo.cpp    -- PDH (Win) / sysctl (macOS)
|   |   |   |-- CpuInfo.h    -- CPU detection header
|   |   |   |-- CpuPowerReader.cpp
|   |   |   |-- CpuPowerReader.h
|   |   |   |-- CpuTempReader.cpp
|   |   |   |-- CpuTempReader.h
|   |   |   |-- RyzenSmuReader.cpp
|   |   |   |-- RyzenSmuReader.h
|   |   |-- disk/
|   |   |   |-- DiskInfo.cpp    -- Logical drive + physical disk SMART
|   |   |   |-- DiskInfo.h    -- Disk/volume data header
|   |   |   |-- NVMe_HealthLog.h
|   |   |   |-- SmartReader.cpp
|   |   |   |-- SmartReader.h
|   |   |   |-- SmartReader_mac.mm
|   |   |-- display/
|   |   |   |-- DisplayInfo.h
|   |   |   |-- DisplayInfo.mm
|   |   |-- etw/
|   |   |   |-- EtwMonitor.cpp
|   |   |   |-- EtwMonitor.h
|   |   |-- fan/
|   |   |   |-- FanSpeed.h
|   |   |   |-- FanSpeed.mm
|   |   |-- gpu/
|   |   |   |-- GpuInfo.cpp    -- WMI + DirectX + NVML / IOKit + Metal
|   |   |   |-- GpuInfo.h    -- GPU detection header
|   |   |-- history/
|   |   |   |-- HistoryLogger.cpp    -- Background flush worker, retention
|   |   |   |-- HistoryLogger.h    -- SQLite sensor log header
|   |   |-- memory/
|   |   |   |-- MemoryInfo.cpp    -- GlobalMemoryStatusEx / host_statistics64
|   |   |   |-- MemoryInfo.h    -- RAM stats header
|   |   |   |-- MemoryTempReader.cpp
|   |   |   |-- MemoryTempReader.h
|   |   |   |-- PawnIOWrapper.cpp
|   |   |   |-- PawnIOWrapper.h
|   |   |-- network/
|   |   |   |-- NetworkAdapter.cpp    -- GetAdaptersAddresses / getifaddrs + throughput
|   |   |   |-- NetworkAdapter.h    -- NIC info header
|   |   |-- notifications/
|   |   |   |-- DeviceChangeNotifier.h
|   |   |   |-- SystemEventMonitor.cpp
|   |   |   |-- SystemEventMonitor.h
|   |   |   |-- UserNotifier.cpp
|   |   |   |-- UserNotifier.h
|   |   |-- os/
|   |   |   |-- OSInfo.cpp    -- RtlGetVersion / sysctl kern.osproductversion
|   |   |   |-- OSInfo.h    -- OS version header
|   |   |-- power/
|   |   |   |-- PowerInfo.cpp    -- WMI battery + IOKit AppleSmartBattery
|   |   |   |-- PowerInfo.h    -- Battery + power plan header
|   |   |   |-- PowerMonitor.cpp
|   |   |   |-- PowerMonitor.h
|   |   |   |-- PowerMonitor_mac.mm
|   |   |-- process/
|   |   |   |-- ProcessMonitor.cpp
|   |   |   |-- ProcessMonitor.h
|   |   |   |-- ProcessTop.h
|   |   |   |-- ProcessTop.mm
|   |   |-- temperature/
|   |   |   |-- AcpiThermalZoneReader.cpp
|   |   |   |-- AcpiThermalZoneReader.h
|   |   |   |-- BoardTempReader.cpp
|   |   |   |-- BoardTempReader.h
|   |   |   |-- IntelMchbarReader.cpp
|   |   |   |-- IntelMchbarReader.h
|   |   |   |-- IntelOobmsmReader.cpp
|   |   |   |-- IntelOobmsmReader.h
|   |   |   |-- TemperatureWrapper.cpp    -- PawnIO+NVML (Win) / SMC+IOKit (macOS)
|   |   |   |-- TemperatureWrapper.h    -- Cross-platform temperature facade
|   |   |-- usb/
|   |   |   |-- UsbInfo.cpp    -- SetupAPI (Win) / IOService (macOS)
|   |   |   |-- UsbInfo.h    -- USB device header
|   |   |-- wifi/
|   |   |   |-- WiFiInfo.cpp    -- Windows WLAN API
|   |   |   |-- WiFiInfo.h    -- WiFi adapter + network info header
|   |   |   |-- WiFiInfo.mm    -- macOS CoreWLAN
|   |   |   |-- WiFiInfo_linux.cpp
|   |   |   |-- WiFiInfo_wlan.c    -- WLAN callback + scan helper
|   |   |   |-- WiFiInfo_wlan.h    -- WLAN helper header
|   |   |-- I18n.h
|   |   |-- ServerProbe.cpp
|   |   |-- ServerProbe.h
|   |   |-- Updater.cpp
|   |   |-- Updater.h
|   |-- helper/
|   |   |-- powerd/
|   |   |   |-- com.tcmt.powerd.plist
|   |   |   |-- tcmt-powerd_main.cpp
|   |   |-- com.tcmt.sensorhelper.info.plist
|   |   |-- com.tcmt.sensorhelper.launchd.plist
|   |   |-- com.tcmt.sensorhelper.plist
|   |   |-- tcmt_sensor_helper.c
|   |-- resources/
|   |   |-- pawnio/
|   |   |   |-- .sha256sum
|   |   |   |-- .version
|   |   |   |-- AMDFamily0F.bin
|   |   |   |-- AMDFamily10.bin
|   |   |   |-- AMDFamily17.bin
|   |   |   |-- AMDReset.bin
|   |   |   |-- ARMMSR.bin
|   |   |   |-- DellSMM.bin
|   |   |   |-- Echo.bin
|   |   |   |-- IntelMCHBAR.bin
|   |   |   |-- IntelMSR.bin
|   |   |   |-- IntelOOBMSM.bin
|   |   |   |-- IntelPCHThermal.bin
|   |   |   |-- IsaBridgeEC.bin
|   |   |   |-- LedsValve.bin
|   |   |   |-- LpcACPIEC.bin
|   |   |   |-- LpcCrOSEC.bin
|   |   |   |-- LpcIO.bin
|   |   |   |-- Nvidia.bin
|   |   |   |-- RyzenSMU.bin
|   |   |   |-- SmbusI801.bin
|   |   |   |-- SmbusIntelSkylakeIMC.bin
|   |   |   |-- SmbusNCT6793.bin
|   |   |   |-- SmbusPIIX4.bin
|   |   |   |-- ZhaoxinMSR.bin
|   |-- third_party/
|   |   |-- sqlite-amalgamation/
|   |   |   |-- sqlite3.c
|   |   |   |-- sqlite3.h
|   |   |   |-- sqlite3ext.h
|   |   |-- PDCurses
|   |   |-- USBMonitor-cpp
|   |   |-- curl
|   |   |-- json
|   |   |-- libusb_scsi_command
|   |   |-- openssl
|   |   |-- sqlite
|   |   |-- tpm2-tss
|   |   |-- zlib
|   |-- tools/
|   |   |-- sensor_recorder.mm
|   |-- tui/
|   |   |-- LogWindow.cpp
|   |   |-- LogWindow.h
|   |   |-- MacLogWindow.h
|   |   |-- MacLogWindow.mm
|   |   |-- TuiApp.cpp    -- CPU/Mem/GPU/Disk/Net/Bluetooth/WiFi/TPM/Temp panels
|   |   |-- TuiApp.h    -- ncurses dashboard header
|   |-- CMakeLists.txt    -- Core library + executable build
|   |-- TCMT-M-Info.plist
|   |-- TCMT-M.entitlements
|   |-- main.cpp    -- Windows entry (C++/CLI, SEH, WMI/PDH/NVML, Bluetooth/WiFi)
|   |-- main_linux.cpp
|   |-- main_mac.cpp    -- macOS entry (ncurses TUI, mach/sysctl/IOKit, Bluetooth/WiFi)
|   |-- resources.rc
|   |-- tcmt.manifest
|-- tests/
|   |-- TCMT.Tests.vcxproj    -- VS test project
|   |-- test_config.cpp    -- ConfigManager unit tests
|   |-- test_ipc.cpp    -- IPC layer unit tests
|   |-- vcpkg.json    -- vcpkg manifest
|-- tools/
|   |-- ioreport/
|   |   |-- ioreport_dump.cpp
|   |-- gen-repo-directory.mjs
|   |-- install-powerd.sh
|   |-- mcp-http-bridge.mjs
|   |-- release-mac.sh
|   |-- uninstall-powerd.sh
|-- .gitattributes
|-- .gitignore
|-- .gitmodules
|-- AGENTS.md    -- Pointer to CLAUDE.md (kept for tools that only read AGENTS.md)
|-- CLAUDE.md    -- Claude Code project guidelines
|-- CMakeLists.txt    -- Core library + executable build
|-- LICENSE
|-- README-WF.md    -- Submodule licenses
|-- README.md    -- Project overview (zh)
|-- TCMT.sln    -- Visual Studio solution
|-- TCMT.vcxproj    -- Visual Studio project (Win32 C++/CLI)
|-- TCMT.vcxproj.filters    -- VS project filters
```
