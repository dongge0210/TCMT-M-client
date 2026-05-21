# TCMT Client core library build
# Cross-platform build with conditional compilation

message(STATUS "Configuring TCMT-M build system...")

# ============================================================
# Source files (cross-platform)
# ============================================================
set(PLATFORM_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Platform/Platform.cpp
)

set(UTILS_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Utils/Logger.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Utils/WinUtils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Utils/ComInitializationHelper.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Utils/TimeUtils.cpp
)

# Core modules
set(CORE_MODULES_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/core/board/BoardInfo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/cpu/CpuInfo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/gpu/GpuInfo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/memory/MemoryInfo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/disk/DiskInfo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/disk/SmartReader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/network/NetworkAdapter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/os/OSInfo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/power/PowerInfo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/power/PowerMonitor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/usb/UsbInfo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/notifications/UserNotifier.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/notifications/SystemEventMonitor.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/coordinator/ModuleCoordinator.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/coordinator/ModuleCoordinator_cpu.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/core/coordinator/ModuleCoordinator_io.cpp
)
# macOS-only modules (.mm Objective-C++, not valid for MSVC)
if(TCMT_MACOS)
    list(APPEND CORE_MODULES_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/core/wifi/WiFiInfo.mm
        ${CMAKE_CURRENT_SOURCE_DIR}/core/bluetooth/BluetoothInfo.mm
        ${CMAKE_CURRENT_SOURCE_DIR}/core/disk/SmartReader_mac.mm
        ${CMAKE_CURRENT_SOURCE_DIR}/core/power/PowerMonitor_mac.mm
    )
endif()

# Windows Bluetooth (bthprops.cpl API)
if(TCMT_WINDOWS)
    list(APPEND CORE_MODULES_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/core/bluetooth/BluetoothInfo.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/core/etw/EtwMonitor.cpp
    )
endif()

# DataStruct / SharedMemory
set(DATASTRUCT_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/core/DataStruct/DataStruct.h
    ${CMAKE_CURRENT_SOURCE_DIR}/core/DataStruct/SharedMemoryManager.cpp
)

# Application config (CPP-parsers + nlohmann/json wrapper)
set(CONFIG_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Config/ConfigManager.h
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Config/ConfigManager.cpp
)
# MCP server (JSON-RPC 2.0 over stdio)
set(MCP_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/core/MCP/MCPServer.cpp
)


# IPC module
set(IPC_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/core/IPC/IPCClient.cpp
)
if(TCMT_MACOS)
    list(APPEND IPC_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/core/IPC/IPCServer.cpp
    )
endif()

# HistoryLogger (SQLite sensor data logging)
set(HISTORY_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/core/history/HistoryLogger.cpp
)

# Temperature modules (Windows only for now)
set(TEMP_SOURCES "")
set(TEMP_SOURCES_WINDOWS "")
if(TCMT_WINDOWS)
    list(APPEND TEMP_SOURCES_WINDOWS
        ${CMAKE_CURRENT_SOURCE_DIR}/core/temperature/LibreHardwareMonitorBridge.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/core/temperature/TemperatureWrapper.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/core/Utils/LibreHardwareMonitorBridge.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/core/Utils/WMIManager.cpp
    )
    list(APPEND TEMP_SOURCES ${TEMP_SOURCES_WINDOWS})
endif()

if(TCMT_MACOS)
    list(APPEND TEMP_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/core/temperature/TemperatureWrapper.cpp
    )
endif()

# TUI module (Windows and macOS)
set(TUI_SOURCES "")
if(TCMT_WINDOWS OR TCMT_MACOS)
    list(APPEND TUI_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/tui/TuiApp.cpp
    )
endif()

# Curses library - platform specific
if(TCMT_WINDOWS)
    # Windows: use PDCurses from third_party
    set(CURSES_INCLUDE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/PDCurses" CACHE PATH "PDCurses include")
    set(CURSES_LIBRARY "pdcurses" CACHE PATH "PDCurses library")
    # Note: PDCurses needs to be built first or use prebuilt library
    
elseif(TCMT_MACOS)
    set(CURSES_NEED_WIDE TRUE)
    # Prefer Homebrew ncurses (has wide chars) over system SDK (no ncursesw)
    if(APPLE)
        find_program(BREW brew)
        if(BREW)
            execute_process(COMMAND ${BREW} --prefix ncurses
                OUTPUT_VARIABLE NCURSES_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET RESULT_VARIABLE _brew_rc)
            if(_brew_rc EQUAL 0)
                set(CURSES_INCLUDE_PATH "${NCURSES_PREFIX}/include" CACHE PATH "Homebrew ncurses include")
                list(APPEND CMAKE_PREFIX_PATH "${NCURSES_PREFIX}")
            endif()
        endif()
    endif()
    find_package(Curses REQUIRED)
endif()

# ============================================================
# Build TCMTCore static library
# ============================================================
add_library(TCMTCore STATIC
    ${PLATFORM_SOURCES}
    ${UTILS_SOURCES}
    ${CORE_MODULES_SOURCES}
    ${DATASTRUCT_SOURCES}
    ${CONFIG_SOURCES}
    ${MCP_SOURCES}
	    ${IPC_SOURCES}
    ${HISTORY_SOURCES}
    ${TEMP_SOURCES}
    ${TUI_SOURCES}
)

set_target_properties(TCMTCore PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

target_include_directories(TCMTCore PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/core/board
    ${CMAKE_CURRENT_SOURCE_DIR}/core/power
    ${CMAKE_CURRENT_SOURCE_DIR}/core/usb
    ${CMAKE_CURRENT_SOURCE_DIR}/core/wifi
    ${CMAKE_CURRENT_SOURCE_DIR}/core/bluetooth
    ${CMAKE_CURRENT_SOURCE_DIR}/core/history
    ${CMAKE_CURRENT_SOURCE_DIR}/core/MCP

    # CPP-parsers unified config interface
    ${CMAKE_CURRENT_SOURCE_DIR}/CPP-parsers/include
    ${CMAKE_CURRENT_SOURCE_DIR}/CPP-parsers/src

    # nlohmann/json (header-only) — single include
    ${CMAKE_CURRENT_SOURCE_DIR}/CPP-parsers/extern/json/single_include
)

# Platform-specific defines
if(TCMT_WINDOWS)
    target_compile_definitions(TCMTCore PRIVATE
        TCMT_WINDOWS UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
    )

elseif(TCMT_MACOS)
    target_compile_definitions(TCMTCore PRIVATE
        TCMT_MACOS _DARWIN_C_SOURCE
    )
endif()

# Apply compile options from cmake/Options.cmake
tcmt_target_set_options(TCMTCore)
tcmt_set_platform_libraries(TCMTCore)
if(TCMT_WINDOWS)
    target_link_libraries(TCMTCore PRIVATE tdh wtsapi32)
endif()
tcmt_set_output_directories(TCMTCore)

# Link curses on Windows and macOS
if(TCMT_WINDOWS)
    # Windows: PDCurses - link with prebuilt or skip for now
    # target_link_libraries(TCMTCore PRIVATE ${CURSES_LIBRARY})
    # target_include_directories(TCMTCore PRIVATE ${CURSES_INCLUDE_PATH})
    # For now, skip - will be added after PDCurses is built
    
elseif(TCMT_MACOS)
    target_link_libraries(TCMTCore PRIVATE ${CURSES_LIBRARIES})
    target_include_directories(TCMTCore PRIVATE ${CURSES_INCLUDE_DIR})
endif()

# SQLite3 — macOS provides it via the system SDK (just -lsqlite3).
# Windows users should install it via vcpkg (vcpkg install sqlite3) and
# add find_package(SQLite3) to cmake/Dependencies.cmake.
if(TCMT_MACOS)
    target_link_libraries(TCMTCore PRIVATE sqlite3)
    message(STATUS "  SQLite3: system library (-lsqlite3)")
endif()
if(TCMT_WINDOWS)
    message(STATUS "  SQLite3: link manually via vcpkg (disabled for now)")
endif()

# IOBluetooth.framework — macOS Bluetooth monitoring (C API)
if(TCMT_MACOS)
    find_library(IOBLUETOOTH_LIB IOBluetooth)
    if(IOBLUETOOTH_LIB)
        target_link_libraries(TCMTCore PRIVATE ${IOBLUETOOTH_LIB})
        message(STATUS "  IOBluetooth.framework: ${IOBLUETOOTH_LIB}")
    else()
        message(WARNING "  IOBluetooth.framework not found — Bluetooth monitoring disabled")
    endif()
endif()

# DiskArbitration, SystemConfiguration, IOKit — macOS system event monitoring
if(TCMT_MACOS)
    find_library(DISKARBITRATION_LIB DiskArbitration)
    find_library(SYSTEMCONFIGURATION_LIB SystemConfiguration)
    find_library(IOKIT_LIB IOKit)
    if(DISKARBITRATION_LIB)
        target_link_libraries(TCMTCore PRIVATE ${DISKARBITRATION_LIB})
        message(STATUS "  DiskArbitration.framework: ${DISKARBITRATION_LIB}")
    endif()
    if(SYSTEMCONFIGURATION_LIB)
        target_link_libraries(TCMTCore PRIVATE ${SYSTEMCONFIGURATION_LIB})
        message(STATUS "  SystemConfiguration.framework: ${SYSTEMCONFIGURATION_LIB}")
    endif()
    if(IOKIT_LIB)
        target_link_libraries(TCMTCore PRIVATE ${IOKIT_LIB})
        message(STATUS "  IOKit.framework: ${IOKIT_LIB}")
    endif()
endif()

# ============================================================
# Build executable
# ============================================================
if(TCMT_WINDOWS)
    # Windows: use main.cpp (with TUI support via #ifdef)
    add_executable(TCMT-M
        ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
        # Windows-specific GPU bridge (C++/CLI)
        ${CMAKE_CURRENT_SOURCE_DIR}/core/Utils/LibreHardwareMonitorBridge.cpp
    )

    # C++/CLI support for Windows
    if(MSVC)
        set_source_files_properties(
            ${CMAKE_CURRENT_SOURCE_DIR}/core/Utils/LibreHardwareMonitorBridge.cpp
            ${CMAKE_CURRENT_SOURCE_DIR}/core/temperature/LibreHardwareMonitorBridge.cpp
            PROPERTIES
            COMPILE_FLAGS "/clr"
        )
    endif()

elseif(TCMT_MACOS)
    # macOS: use main_mac.cpp
    add_executable(TCMT-M
        ${CMAKE_CURRENT_SOURCE_DIR}/main_mac.cpp
    )
endif()

target_link_libraries(TCMT-M PRIVATE TCMTCore)

set_target_properties(TCMT-M PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

# Platform-specific exe settings
if(TCMT_WINDOWS)
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_link_options(TCMT-M PRIVATE
            /SUBSYSTEM:CONSOLE /ENTRY:mainCRTStartup
        )
    endif()
elseif(TCMT_MACOS)
    # macOS executable settings
endif()

message(STATUS "TCMT-M build configured successfully")
