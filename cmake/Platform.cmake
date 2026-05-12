# Platform detection and configuration for TCMT Client
# Platform detection and configuration module

function(tcmt_detect_platform)
    message(STATUS "Detecting platform...")

    # Platform detection
    if(WIN32)
        set(TCMT_WINDOWS ON CACHE BOOL "Building for Windows platform")
        set(TCMT_PLATFORM_NAME "Windows" CACHE STRING "Platform name")
        set(TCMT_PLATFORM "windows" CACHE STRING "Platform identifier")
        add_definitions(-DTCMT_WINDOWS)

        # Windows-specific definitions
        add_definitions(-DUNICODE -D_UNICODE -DNOMINMAX -DWIN32_LEAN_AND_MEAN)

        # Windows SDK版本检测
        if(NOT CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
            set(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION "10.0.26100.0" CACHE STRING "Windows Target Platform Version")
        endif()

        message(STATUS "  Target: Windows ${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")

    elseif(APPLE)
        set(TCMT_MACOS ON CACHE BOOL "Building for macOS platform")
        set(TCMT_PLATFORM_NAME "macOS" CACHE STRING "Platform name")
        set(TCMT_PLATFORM "macos" CACHE STRING "Platform identifier")
        add_definitions(-DTCMT_MACOS)

        # macOS version detection
        execute_process(
            COMMAND sw_vers -productVersion
            OUTPUT_VARIABLE MACOS_VERSION
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        message(STATUS "  Target: macOS ${MACOS_VERSION}")

    else()
        message(FATAL_ERROR "Unsupported platform: ${CMAKE_SYSTEM_NAME}")
    endif()

    # 架构检测
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        set(TCMT_ARCH_X64 ON CACHE BOOL "x64 architecture")
        message(STATUS "  Architecture: x64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(TCMT_ARCH_ARM64 ON CACHE BOOL "ARM64 architecture")
        message(STATUS "  Architecture: ARM64")
    else()
        message(WARNING "Unknown architecture: ${CMAKE_SYSTEM_PROCESSOR}")
    endif()

    # Set platform variables for parent scope
    set(TCMT_WINDOWS ${TCMT_WINDOWS} PARENT_SCOPE)
    set(TCMT_MACOS ${TCMT_MACOS} PARENT_SCOPE)
    set(TCMT_PLATFORM_NAME ${TCMT_PLATFORM_NAME} PARENT_SCOPE)
    set(TCMT_PLATFORM ${TCMT_PLATFORM} PARENT_SCOPE)
    set(TCMT_ARCH_X64 ${TCMT_ARCH_X64} PARENT_SCOPE)
    set(TCMT_ARCH_ARM64 ${TCMT_ARCH_ARM64} PARENT_SCOPE)

endfunction()

# 平台特定库配置
function(tcmt_set_platform_libraries target)
    message(STATUS "Configuring platform libraries for ${target}...")

    if(TCMT_WINDOWS)
        # Windows平台库
        target_link_libraries(${target} PRIVATE
            kernel32
            user32
            advapi32
            shell32
            ole32
            oleaut32
            uuid
            psapi
            pdh
            wbemuuid
            winmm
        )

        # Windows-specific compile options
        if(MSVC)
            target_compile_options(${target} PRIVATE
                /W4           # Warning level 4
                /wd4100       # Disable unused parameter warning
                /wd4201       # Disable unnamed struct/union warning
                /wd4456       # Disable hidden local variable declaration warning
                /wd4458       # Disable hidden class member declaration warning
                /wd4459       # Disable hidden global declaration warning
                /wd4996       # Disable unsafe function warning
            )

            # 设置Windows SDK版本
            target_compile_definitions(${target} PRIVATE
                _WIN32_WINNT=0x0A00  # Windows 10
                NTDDI_VERSION=0x0A000000  # Windows 10
            )
        endif()

    elseif(TCMT_MACOS)
        # macOS platform libraries
        find_library(IOKIT_LIB IOKit)
        find_library(COREFOUNDATION_LIB CoreFoundation)
        find_library(APPLICATIONSERVICES_LIB ApplicationServices)
        find_library(FOUNDATION_LIB Foundation)
        find_library(CORESERVICES_LIB CoreServices)
        find_library(COREWLAN_LIB CoreWLAN)

        # 创建要链接的库列表
        set(MACOS_LIBS)

        if(IOKIT_LIB)
            list(APPEND MACOS_LIBS ${IOKIT_LIB})
            message(STATUS "    Found IOKit framework")
        endif()
        if(COREFOUNDATION_LIB)
            list(APPEND MACOS_LIBS ${COREFOUNDATION_LIB})
            message(STATUS "    Found CoreFoundation framework")
        endif()
        if(APPLICATIONSERVICES_LIB)
            list(APPEND MACOS_LIBS ${APPLICATIONSERVICES_LIB})
            message(STATUS "    Found ApplicationServices framework")
        endif()
        if(FOUNDATION_LIB)
            list(APPEND MACOS_LIBS ${FOUNDATION_LIB})
            message(STATUS "    Found Foundation framework")
        endif()
        if(CORESERVICES_LIB)
            list(APPEND MACOS_LIBS ${CORESERVICES_LIB})
            message(STATUS "    Found CoreServices framework")
        endif()
        if(COREWLAN_LIB)
            list(APPEND MACOS_LIBS ${COREWLAN_LIB})
            message(STATUS "    Found CoreWLAN framework")
        endif()
        target_link_libraries(${target} PRIVATE ${MACOS_LIBS})

        # macOS-specific compile options
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wno-unused-parameter
            -Wno-deprecated-declarations
        )

        # macOS版本定义
        target_compile_definitions(${target} PRIVATE
            _DARWIN_C_SOURCE
            _DARWIN_USE_64_BIT_INODE=1
        )

    endif()

    message(STATUS "  Platform libraries configured")
endfunction()

# Platform-specific output directory setup
function(tcmt_set_output_directories target)
    if(TCMT_WINDOWS)
        # Windows输出目录（与Visual Studio项目保持一致）
        set_target_properties(${target} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/${CMAKE_BUILD_TYPE}"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/${CMAKE_BUILD_TYPE}"
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib/${CMAKE_BUILD_TYPE}"
        )
    else()
        # Unix-style output directory
        set_target_properties(${target} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
            LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
            ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
        )
    endif()
endfunction()