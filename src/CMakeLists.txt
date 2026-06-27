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
    # PawnIO DIMM temperature (Windows-only)
    $<$<BOOL:${TCMT_WINDOWS}>:${CMAKE_CURRENT_SOURCE_DIR}/core/memory/PawnIOWrapper.cpp>
    $<$<BOOL:${TCMT_WINDOWS}>:${CMAKE_CURRENT_SOURCE_DIR}/core/memory/MemoryTempReader.cpp>
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
        ${CMAKE_CURRENT_SOURCE_DIR}/core/display/DisplayInfo.mm
        ${CMAKE_CURRENT_SOURCE_DIR}/core/battery/BatteryHealth.mm
        ${CMAKE_CURRENT_SOURCE_DIR}/core/process/ProcessTop.mm
        ${CMAKE_CURRENT_SOURCE_DIR}/core/als/ALSensor.mm
        ${CMAKE_CURRENT_SOURCE_DIR}/core/accel/SpsManager.mm
        ${CMAKE_CURRENT_SOURCE_DIR}/core/HTTPServer/MotionHTTPServer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/core/ServerProbe.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/core/disk/SmartReader_mac.mm
        ${CMAKE_CURRENT_SOURCE_DIR}/core/ioreport/IOReportSampler.mm
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
        ${CMAKE_CURRENT_SOURCE_DIR}/core/Utils/WMIManager.cpp
    )
    list(APPEND TEMP_SOURCES ${TEMP_SOURCES_WINDOWS})
endif()

if(TCMT_MACOS OR TCMT_LINUX)
    list(APPEND TEMP_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/core/temperature/TemperatureWrapper.cpp
    )
endif()

# TUI module (Windows, macOS, and Linux)
set(TUI_SOURCES "")
if(TCMT_WINDOWS OR TCMT_MACOS OR TCMT_LINUX)
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

elseif(TCMT_LINUX)
    set(CURSES_NEED_WIDE TRUE)
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
    ${CMAKE_CURRENT_SOURCE_DIR}/core/battery
    ${CMAKE_CURRENT_SOURCE_DIR}/core/process
    ${CMAKE_CURRENT_SOURCE_DIR}/core/als
    ${CMAKE_CURRENT_SOURCE_DIR}/core/accel
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

elseif(TCMT_LINUX)
    target_compile_definitions(TCMTCore PRIVATE
        TCMT_LINUX _GNU_SOURCE
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

elseif(TCMT_LINUX)
    target_link_libraries(TCMTCore PRIVATE ${CURSES_LIBRARIES})
    target_include_directories(TCMTCore PRIVATE ${CURSES_INCLUDE_DIR})
endif()

# SQLite3 — pre-generated amalgamation from the sqlite submodule.
# Amalgamation files are committed directly (no CI-time generation needed).
# To regenerate: cd src/third_party/sqlite && ./configure && make sqlite3.c
#                 then copy sqlite3.c sqlite3.h sqlite3ext.h to sqlite-amalgamation/
set(SQLITE_AMALG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite-amalgamation")
set(SQLITE_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite")
set(SQLITE3_C "${SQLITE_AMALG_DIR}/sqlite3.c")
set(SQLITE3_H "${SQLITE_AMALG_DIR}/sqlite3.h")

# Stage sqlite3.h to a clean include dir (source dir has files like 'version' that
# break C++ compilation when exposed via PUBLIC include paths)
set(SQLITE_BIN_DIR "${CMAKE_BINARY_DIR}/sqlite3")
file(MAKE_DIRECTORY "${SQLITE_BIN_DIR}")
add_custom_target(sqlite3_header
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${SQLITE3_H}" "${SQLITE_BIN_DIR}/sqlite3.h"
    DEPENDS "${SQLITE3_H}"
    COMMENT "Staging sqlite3.h"
)

add_library(sqlite3 STATIC "${SQLITE3_C}")
add_dependencies(sqlite3 sqlite3_header)
target_include_directories(sqlite3
    PUBLIC  "${SQLITE_BIN_DIR}"       # sqlite3.h
    PRIVATE "${SQLITE_SRC_DIR}"        # sqlite_cfg.h, sqlite3ext.h
)
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_ENABLE_DBSTAT_VTAB
    SQLITE_ENABLE_FTS5
    SQLITE_ENABLE_JSON1
    SQLITE_ENABLE_RTREE
    SQLITE_THREADSAFE=1
)
if(MSVC)
    target_compile_options(sqlite3 PRIVATE /W0)
else()
    target_compile_options(sqlite3 PRIVATE -w)
endif()

target_link_libraries(TCMTCore PRIVATE sqlite3)
message(STATUS "  SQLite3: built from submodule amalgamation")

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
    find_library(SERVICEMANAGEMENT_LIB ServiceManagement)
    find_library(SECURITY_LIB Security)
    if(SERVICEMANAGEMENT_LIB)
        target_link_libraries(TCMTCore PRIVATE ${SERVICEMANAGEMENT_LIB})
        message(STATUS "  ServiceManagement.framework: ${SERVICEMANAGEMENT_LIB}")
    endif()
    if(SECURITY_LIB)
        target_link_libraries(TCMTCore PRIVATE ${SECURITY_LIB})
        message(STATUS "  Security.framework: ${SECURITY_LIB}")
    endif()
endif()

# ============================================================
# SMJobBless privileged helper (BMI284 accelerometer)
# ============================================================
if(TCMT_MACOS)
    # Build the helper executable
    add_executable(tcmt_sensor_helper
        ${CMAKE_CURRENT_SOURCE_DIR}/helper/tcmt_sensor_helper.c
    )
    target_link_libraries(tcmt_sensor_helper PRIVATE
        "-framework IOKit"
        "-framework CoreFoundation"
    )
    # Embed CFBundle Info.plist into __TEXT,__info_plist (CFBundleIdentifier, SMAuthorizedClients)
    target_link_options(tcmt_sensor_helper PRIVATE
        "-Wl,-sectcreate,__TEXT,__info_plist,${CMAKE_CURRENT_SOURCE_DIR}/helper/com.tcmt.sensorhelper.info.plist"
    )
    # Embed launchd plist into __TEXT,__launchd_plist (Label, KeepAlive — required by SMJobBless)
    target_link_options(tcmt_sensor_helper PRIVATE
        "-Wl,-sectcreate,__TEXT,__launchd_plist,${CMAKE_CURRENT_SOURCE_DIR}/helper/com.tcmt.sensorhelper.launchd.plist"
    )
    set_target_properties(tcmt_sensor_helper PROPERTIES
        C_STANDARD 11
        C_STANDARD_REQUIRED ON
    )
endif()

# ============================================================
# Build executable
# ============================================================
if(TCMT_WINDOWS)
    # Windows: use main.cpp (with TUI support via #ifdef)
    add_executable(TCMT-M
        ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp
    )

    # C++/CLI support for Windows
    if(MSVC)
        set_source_files_properties(
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

elseif(TCMT_LINUX)
    # Linux: use main_linux.cpp
    add_executable(TCMT-M
        ${CMAKE_CURRENT_SOURCE_DIR}/main_linux.cpp
    )
endif()

target_compile_definitions(TCMTCore PUBLIC TCMT_VERSION_STR="${PROJECT_VERSION}")
target_compile_definitions(TCMT-M PRIVATE APP_VERSION="${PROJECT_VERSION}")
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
    # macOS: build .app bundle so CoreLocation/SSID permission dialog works.
    # Use generator expressions so the bundle lands alongside the binary
    # regardless of CMAKE_RUNTIME_OUTPUT_DIRECTORY.
    add_custom_command(TARGET TCMT-M POST_BUILD
        # Always start fresh to avoid stale signature hash mismatch
        COMMAND ${CMAKE_COMMAND} -E rm -rf
            "$<TARGET_FILE_DIR:TCMT-M>/TCMT-M.app"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:TCMT-M>/TCMT-M.app/Contents/MacOS"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:TCMT-M>"
            "$<TARGET_FILE_DIR:TCMT-M>/TCMT-M.app/Contents/MacOS/TCMT-M"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_CURRENT_SOURCE_DIR}/TCMT-M-Info.plist"
            "$<TARGET_FILE_DIR:TCMT-M>/TCMT-M.app/Contents/Info.plist"
        # Copy SMJobBless helper to Contents/Library/LaunchServices/
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_FILE_DIR:TCMT-M>/TCMT-M.app/Contents/Library/LaunchServices"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:tcmt_sensor_helper>"
            "$<TARGET_FILE_DIR:TCMT-M>/TCMT-M.app/Contents/Library/LaunchServices/com.tcmt.sensorhelper"
        # Conditionally sign: skip if identity not in keychain (e.g. CI)
        COMMAND ${CMAKE_SOURCE_DIR}/scripts/codesign.sh
            "$<TARGET_FILE_DIR:TCMT-M>"
            "${CMAKE_CURRENT_SOURCE_DIR}/TCMT-M.entitlements"
        COMMENT "Creating TCMT-M.app bundle (codesign if identity available)"
    )
elseif(TCMT_LINUX)
    # Linux: no .app bundle, no codesign, no SMJobBless helper
    target_link_options(TCMT-M PRIVATE
        -pthread
    )
endif()

message(STATUS "TCMT-M build configured successfully")
