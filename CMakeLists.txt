# TCMT Windows Client - CMake build system
# Supports Windows and macOS platforms with conditional compilation

cmake_minimum_required(VERSION 3.20)
project(TCMT-M
    VERSION 0.14.0
    DESCRIPTION "TCMT Windows Client - Cross-platform hardware monitoring tool"
    LANGUAGES CXX
)

# Set C++20 standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 输出构建类型
message(STATUS "Project: ${PROJECT_NAME} v${PROJECT_VERSION}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "C++ standard: C++${CMAKE_CXX_STANDARD}")

# Include CMake modules
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/cmake")
include(Platform)
include(Options)
include(Dependencies)

# 平台检测和配置
tcmt_detect_platform()

# Configure compile options
tcmt_set_compile_options()

# Add subdirectories
add_subdirectory(src)

# Install configuration (if needed)
if(TCMT_INSTALL)
    install(TARGETS TCMT-M
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION lib
        ARCHIVE DESTINATION lib
    )

    if(TCMT_WINDOWS)
        # Windows-specific install configuration
        install(FILES ${PROJECT_SOURCE_DIR}/LICENSE DESTINATION .)
    elseif(TCMT_MACOS)
        # macOS-specific install configuration
        install(FILES ${PROJECT_SOURCE_DIR}/LICENSE DESTINATION .)
    endif()
endif()

# Overall build configuration
message(STATUS "========================================")
message(STATUS "TCMT Client Build Configuration Summary")
message(STATUS "========================================")
message(STATUS "Platform: ${TCMT_PLATFORM_NAME}")
message(STATUS "Architecture: ${CMAKE_SYSTEM_PROCESSOR}")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "Build type: ${CMAKE_BUILD_TYPE}")
if(TCMT_WINDOWS)
    message(STATUS "Windows SDK: ${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
    if(MSVC)
        message(STATUS "MSVC工具集: v${MSVC_TOOLSET_VERSION}")
    endif()
endif()
message(STATUS "========================================")