# TCMT Windows Client - CMake build system
# Supports Windows and macOS platforms with conditional compilation

cmake_minimum_required(VERSION 3.20)

# macOS deployment target & architecture (must be set BEFORE project())
# Supports macOS 15+ on arm64 (Apple Silicon) and x86_64 (Intel, until Apple drops support)
if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum macOS deployment target")
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures")
endif()

project(TCMT-M
    VERSION 0.0.79
    DESCRIPTION "TCMT Windows Client - Cross-platform hardware monitoring tool"
    LANGUAGES C CXX
)

# 对外显示的版本串，带阶段标记：ALPHA_0.0.79。
#
# 为什么不直接写进 project(VERSION)：CMake 只接受 主.次.修订[.补丁] 这种纯数字，
# 传 ALPHA_0.0.79 进去 configure 就报错。所以数字给 CMake 管（它自己要用），
# 带标记的那份在这里拼一次，再由 src/CMakeLists.txt 注入成编译宏。
#
# 改版本时这几处要一起改（清单也写在 AvaloniaUI/Core/Constants.cs 上面）：
#   本文件 project(VERSION) + 这一行
#   src/core/DataStruct/DataStruct.h          TCMT_VERSION_STR 兜底
#   AvaloniaUI/Core/Constants.cs              AppVersion
#   AvaloniaUI/TCMT.Avalonia.csproj           AssemblyVersion（同样只能填数字）
#   src/core/MCP/MCPServer.cpp                SERVER_VERSION
#   CLAUDE.md                                 开头那句
# git 短 SHA：同一个版本号的不同构建靠它区分。取不到（源码包解压出来没有 .git）就退成 nogit。
# 注意如果是一条命令都跑不起来，CMAKE_CURRENT_SOURCE_DIR 这类变量此时已经可用。
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    OUTPUT_VARIABLE TCMT_GIT_SHA
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT TCMT_GIT_SHA)
    set(TCMT_GIT_SHA "nogit")
endif()

# 故意不加 CACHE：SHA 每次提交都在变，缓存住的话第一次 configure 之后它就冻在那儿了，
# 后面所有构建都会顶着第一个 SHA —— 比不显示还糟，因为它看着是对的。
set(TCMT_VERSION_DISPLAY "ALPHA_${PROJECT_VERSION}-${TCMT_GIT_SHA}")

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