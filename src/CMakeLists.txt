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
        ${CMAKE_CURRENT_SOURCE_DIR}/core/fan/FanSpeed.mm
        ${CMAKE_CURRENT_SOURCE_DIR}/core/process/ProcessTop.mm
        ${CMAKE_CURRENT_SOURCE_DIR}/core/als/ALSensor.mm
        ${CMAKE_CURRENT_SOURCE_DIR}/core/accel/SpsManager.mm
        ${CMAKE_CURRENT_SOURCE_DIR}/core/HTTPServer/MotionHTTPServer.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/core/ServerProbe.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/core/Updater.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/core/disk/SmartReader_mac.mm
        ${CMAKE_CURRENT_SOURCE_DIR}/core/power/PowerMonitor_mac.mm
    )
endif()

# Linux WiFi/Bluetooth (D-Bus: NetworkManager / BlueZ, with /proc and sysfs
# fallbacks). WiFiInfo.cpp is the Windows WLAN implementation and must not be
# built here.
if(TCMT_LINUX)
    list(APPEND CORE_MODULES_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/core/wifi/WiFiInfo_linux.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/core/bluetooth/BluetoothInfo_linux.cpp
        # Shared Linux collection helpers (owned by the sensors side). Every new
        # .cpp under core/linux/ must be listed here or it silently misses the
        # build and only shows up as an undefined symbol at link time.
        ${CMAKE_CURRENT_SOURCE_DIR}/core/linux/LinuxCpu.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/core/linux/LinuxDisk.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/core/linux/LinuxGpu.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/core/linux/LinuxPower.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/core/linux/LinuxProcess.cpp
    )
endif()

# Windows Bluetooth (bthprops.cpl API)
if(TCMT_WINDOWS)
    list(APPEND CORE_MODULES_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/core/bluetooth/BluetoothInfo.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/core/etw/EtwMonitor.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/core/process/ProcessMonitor.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/core/temperature/AcpiThermalZoneReader.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/core/cpu/CpuPowerReader.cpp
    )
endif()

# DataStruct / SharedMemory
set(DATASTRUCT_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/core/DataStruct/DataStruct.h
    ${CMAKE_CURRENT_SOURCE_DIR}/core/DataStruct/SharedMemoryManager.cpp
)

# Application config (nlohmann/json wrapper)
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
if(TCMT_WINDOWS)
    # Standalone log window is Win32-only
    list(APPEND TUI_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/tui/LogWindow.cpp
    )
elseif(TCMT_MACOS)
    # In-process native log window (AppKit) — same-process mirror of LogWindow
    list(APPEND TUI_SOURCES
        ${CMAKE_CURRENT_SOURCE_DIR}/tui/MacLogWindow.mm
    )
    set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/tui/MacLogWindow.mm
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
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
    ${CMAKE_CURRENT_SOURCE_DIR}/core/fan
    ${CMAKE_CURRENT_SOURCE_DIR}/core/process
    ${CMAKE_CURRENT_SOURCE_DIR}/core/als
    ${CMAKE_CURRENT_SOURCE_DIR}/core/accel
    ${CMAKE_CURRENT_SOURCE_DIR}/core/history
    ${CMAKE_CURRENT_SOURCE_DIR}/core/MCP
    ${CMAKE_CURRENT_SOURCE_DIR}/core/Utils

    # nlohmann/json (header-only) — single include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/single_include

    # Third-party headers used on Windows (mirror TCMT.vcxproj include dirs)
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/USBMonitor-cpp/include
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite-amalgamation
)

# Platform-specific defines
if(TCMT_WINDOWS)
    target_compile_definitions(TCMTCore PRIVATE
        TCMT_WINDOWS UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN
        _CRT_SECURE_NO_WARNINGS
        # PDCurses 的两套鼠标 API：不定义这个就只有那个 0 参的 mmask_t getmouse(void)，
        # 而 TuiApp.cpp 用的是 ncurses 那种 getmouse(&mev)。定义之后 curses.h 才会把
        # 名字映射到 nc_getmouse（curses.h:258）。
        # TCMT.vcxproj 里也有一份 —— 两条构建路径都得有，msvc.yml 的代码分析走的是 CMake
        # 这一条，只在 vcxproj 里定义的话它照样红。
        PDC_NCMOUSE
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

# libdbus-1 — optional. WiFi/Bluetooth use NetworkManager and BlueZ when the
# system bus provides them. TCMT_HAVE_DBUS is only defined when pkg-config can
# find dbus-1; without it both collectors still compile and fall back to
# /proc/net/wireless and /sys/class/bluetooth.
if(TCMT_LINUX)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(TCMT_DBUS QUIET IMPORTED_TARGET dbus-1)
    endif()
    if(TCMT_DBUS_FOUND)
        target_compile_definitions(TCMTCore PRIVATE TCMT_HAVE_DBUS)
        target_link_libraries(TCMTCore PRIVATE PkgConfig::TCMT_DBUS)
        message(STATUS "  libdbus-1: ${TCMT_DBUS_VERSION} (NetworkManager / BlueZ enabled)")
    else()
        message(STATUS "  libdbus-1: not found (WiFi/Bluetooth fall back to /proc and sysfs)")
    endif()
endif()

# Apply compile options from cmake/Options.cmake
tcmt_target_set_options(TCMTCore)
tcmt_set_platform_libraries(TCMTCore)
if(TCMT_WINDOWS)
    target_link_libraries(TCMTCore PRIVATE tdh wtsapi32)
endif()
tcmt_set_output_directories(TCMTCore)

# Link curses on Windows and macOS
#
# PUBLIC, not PRIVATE: TuiApp.cpp is the only file in src/ that includes curses.h and it
# now compiles into the TCMT-M executable, not into TCMTCore. With PRIVATE the executable
# fell back to the SDK's older ncurses headers and failed on BUTTON5_PRESSED. Nothing in
# src/core/ includes curses at all, so the whole dependency belongs to the executable.
if(TCMT_WINDOWS)
    # Windows: PDCurses - link with prebuilt or skip for now
    # target_link_libraries(TCMTCore PRIVATE ${CURSES_LIBRARY})
    target_include_directories(TCMTCore PUBLIC
        ${CURSES_INCLUDE_PATH}
        ${CURSES_INCLUDE_PATH}/wincon
    )

elseif(TCMT_MACOS)
    target_link_libraries(TCMTCore PRIVATE ${CURSES_LIBRARIES})
    target_include_directories(TCMTCore PUBLIC ${CURSES_INCLUDE_DIR})

elseif(TCMT_LINUX)
    target_link_libraries(TCMTCore PRIVATE ${CURSES_LIBRARIES})
    target_include_directories(TCMTCore PUBLIC ${CURSES_INCLUDE_DIR})
endif()

# ── Optional TLS for ServerProbe (https upload to public internet) ──
#
# 用仓库里的子模块（src/third_party/openssl），**不走系统包**。
#
# 以前这段是 find_package(OpenSSL)，它只会在系统里找，找不到就试 Homebrew 那三个路径。
# 问题不在于找得到找不到，而在于**解析出来的完整路径会被 CMake 缓存钉死**：缓存里写着
# /opt/homebrew/Cellar/openssl@3/3.6.3/include，Homebrew 一升到 3.6.4，那个目录就没了，
# 构建直接断在「openssl/ssl.h not found」—— 报错和源码一点关系都没有，看着像代码坏了。
# 子模块的版本是 commit 钉死的，Homebrew 怎么升都不影响。
#
# 子模块里只有源码，而 OpenSSL 到 4.x 还在用 Perl 的 Configure（没有 CMakeLists），
# 所以这里得自己驱动一次：configure 期跑 Configure 生成 Makefile，构建期跑 make。
# 两个都带缓存守卫，第二次构建既不重新配置也不重编。
if(TCMT_MACOS OR TCMT_LINUX)
    set(_ossl_src "${CMAKE_SOURCE_DIR}/src/third_party/openssl")
    set(_ossl_build "${CMAKE_BINARY_DIR}/third_party/openssl")

    if(EXISTS "${_ossl_src}/Configure")
        if(NOT EXISTS "${_ossl_build}/Makefile")
            if(TCMT_MACOS)
                if(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
                    set(_ossl_platform "darwin64-arm64-cc")
                else()
                    set(_ossl_platform "darwin64-x86_64-cc")
                endif()
            elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
                set(_ossl_platform "linux-aarch64")
            else()
                set(_ossl_platform "linux-x86_64")
            endif()

            # no-shared：出静态库，不去和系统里的 libssl.dylib 抢符号
            # no-tests：省掉几万个测试文件，构建快一大截
            file(MAKE_DIRECTORY "${_ossl_build}")
            message(STATUS "OpenSSL: configuring bundled submodule (${_ossl_platform})")
            # macOS 的最低版本要跟着主程序走，否则 OpenSSL 按本机系统版本编（这台机器上
            # 是 27.0），而主程序按 CMAKE_OSX_DEPLOYMENT_TARGET（15.0）链 —— 链接器会对
            # 每一个 .o 报一遍「built for newer macOS version」，这个仓库里有 888 个 .o。
            #
            # 旗标挂在 CC 上而不是 CFLAGS 上：给 CFLAGS 会把 OpenSSL 自己的 -O3 -Wall 整份
            # 顶掉（实测过，Makefile 里只剩你传的那一个旗标），挂 CC 则两者都在。
            set(_ossl_env "${CMAKE_COMMAND}" -E env)
            if(TCMT_MACOS AND CMAKE_OSX_DEPLOYMENT_TARGET)
                list(APPEND _ossl_env
                     "CC=${CMAKE_C_COMPILER} -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET}")
            endif()

            execute_process(
                COMMAND ${_ossl_env} "${_ossl_src}/Configure" "${_ossl_platform}"
                        --prefix=${_ossl_build} --openssldir=${_ossl_build}/ssl
                        no-shared no-tests
                WORKING_DIRECTORY "${_ossl_build}"
                RESULT_VARIABLE _ossl_configure_result)
            if(NOT _ossl_configure_result EQUAL 0)
                message(FATAL_ERROR
                    "OpenSSL Configure 失败（退出码 ${_ossl_configure_result}）。"
                    " 删掉 ${_ossl_build} 再试一次。")
            endif()
        endif()

        add_custom_command(
            OUTPUT "${_ossl_build}/libssl.a" "${_ossl_build}/libcrypto.a"
            COMMAND ${CMAKE_MAKE_PROGRAM} -j8 build_libs
            WORKING_DIRECTORY "${_ossl_build}"
            COMMENT "构建 third_party/openssl"
            VERBATIM)
        add_custom_target(tcmt_openssl
            DEPENDS "${_ossl_build}/libssl.a" "${_ossl_build}/libcrypto.a")
        add_dependencies(TCMTCore tcmt_openssl)

        # 头文件在两边：公开的在源码树，opensslconf.h / ssl.h 这些是 Configure 生成的
        set(_ossl_includes "${_ossl_src}/include" "${_ossl_build}/include")

        add_library(TCMT::OpenSSL_SSL STATIC IMPORTED GLOBAL)
        set_target_properties(TCMT::OpenSSL_SSL PROPERTIES
            IMPORTED_LOCATION "${_ossl_build}/libssl.a"
            INTERFACE_INCLUDE_DIRECTORIES "${_ossl_includes}")
        add_library(TCMT::OpenSSL_Crypto STATIC IMPORTED GLOBAL)
        set_target_properties(TCMT::OpenSSL_Crypto PROPERTIES
            IMPORTED_LOCATION "${_ossl_build}/libcrypto.a"
            INTERFACE_INCLUDE_DIRECTORIES "${_ossl_includes}")

        target_compile_definitions(TCMTCore PRIVATE TCMT_USE_TLS)
        target_link_libraries(TCMTCore PRIVATE TCMT::OpenSSL_SSL TCMT::OpenSSL_Crypto)
        message(STATUS "OpenSSL: using bundled src/third_party/openssl")
    else()
        # 子模块没拉下来（clone 时没带 --recursive）—— 退回系统包，
        # 总比整个构建失败强，但要让人知道现在用的是系统那一份、会随它升级而变
        find_package(OpenSSL QUIET)
        if(OPENSSL_FOUND)
            message(WARNING
                "src/third_party/openssl 不存在（跑 git submodule update --init），"
                "本次退回系统 OpenSSL ${OPENSSL_VERSION} —— 它的版本不受本仓库控制")
            target_compile_definitions(TCMTCore PRIVATE TCMT_USE_TLS)
            target_link_libraries(TCMTCore PRIVATE OpenSSL::SSL OpenSSL::Crypto)
        else()
            message(WARNING
                "OpenSSL 不可用（子模块没拉、系统里也没有）—— ServerProbe https 关闭")
        endif()
    endif()
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
# tcmt-powerd privileged daemon (macOS 27+ power via /usr/bin/powermetrics
# as root — IOReport energy is entitlement-gated on 27, powermetrics is
# Apple-signed and works). Publishes CPU/GPU/ANE power (mW) to shared
# memory "tcmt-power".
# ============================================================
if(TCMT_MACOS)
    add_executable(tcmt-powerd
        ${CMAKE_CURRENT_SOURCE_DIR}/helper/powerd/tcmt-powerd_main.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/core/Utils/Logger.cpp
    )
    target_include_directories(tcmt-powerd PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/core
    )
    set_target_properties(tcmt-powerd PROPERTIES
        CXX_STANDARD 20
        CXX_STANDARD_REQUIRED ON
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

# 注入的是带阶段标记的那份（ALPHA_0.0.79），不是 PROJECT_VERSION —— 见根 CMakeLists 里
# TCMT_VERSION_DISPLAY 的说明。界面和 MCP 显示的就是这个串。
target_compile_definitions(TCMTCore PUBLIC TCMT_VERSION_STR="${TCMT_VERSION_DISPLAY}")
target_compile_definitions(TCMT-M PRIVATE APP_VERSION="${TCMT_VERSION_DISPLAY}")
target_link_libraries(TCMT-M PRIVATE TCMTCore)

# TUI sources belong to the executable, not to TCMTCore. Nothing in core calls
# TuiApp — ModuleCoordinator only needs the TuiData struct, which is a header — so
# the core library has no business carrying 2.3k lines of ncurses rendering. This
# is the precondition for ever shipping an MCP-only binary with no terminal
# dependency. Note it does not by itself change today's binary: TCMT-M runs both the
# TUI and --mcp, so it links ncurses either way.
target_sources(TCMT-M PRIVATE ${TUI_SOURCES})

set_target_properties(TCMT-M PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
)

# Platform-specific exe settings
if(TCMT_WINDOWS)
    # Embed UAC requireAdministrator manifest (fixes double console window)
    target_link_options(TCMT-M PRIVATE
        /MANIFEST:EMBED /MANIFESTUAC:NO /MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/tcmt.manifest
    )
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
    # Linux: no .app bundle, no codesign, no SMJobBless helper.
    # _GNU_SOURCE is not inherited from TCMTCore (it is PRIVATE there), and
    # -std=c++20 defines __STRICT_ANSI__, which hides POSIX declarations such as
    # localtime_r and getloadavg that main_linux.cpp uses.
    target_compile_definitions(TCMT-M PRIVATE _GNU_SOURCE)
    target_link_options(TCMT-M PRIVATE
        -pthread
    )
endif()

message(STATUS "TCMT-M build configured successfully")
