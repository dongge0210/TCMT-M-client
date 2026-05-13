# Compilation options configuration for TCMT Client
# 编译选项配置模块

function(tcmt_set_compile_options)
    message(STATUS "Setting compilation options...")

    # 全局编译选项
    if(MSVC)
        # MSVC编译器选项
        add_compile_options(
            /MP           # 多处理器编译
            /EHsc         # C++异常处理
            /Zc:__cplusplus  # 启用正确的__cplusplus宏
            /permissive-  # 标准一致性模式
        )

        # 调试选项
        if(CMAKE_BUILD_TYPE STREQUAL "Debug")
            add_compile_options(
                /Zi         # 调试信息
                /Od         # 禁用优化
                /RTC1       # 运行时检查
            )
            add_definitions(-D_DEBUG)
        else()
            add_compile_options(
                /O2         # 最大优化（速度）
                /GL         # 全程序优化
                /Gy         # 函数级链接
            )
            add_definitions(-DNDEBUG)
        endif()

    else()
        # GCC/Clang编译器选项
        add_compile_options(
            -fvisibility=hidden
            -fstack-protector-strong
        )

        # 调试选项
        if(CMAKE_BUILD_TYPE STREQUAL "Debug")
            add_compile_options(
                -g          # 调试信息
                -O0         # 禁用优化
                -fno-inline # 禁用内联
            )
            add_definitions(-D_DEBUG)
        else()
            add_compile_options(
                -O3         # 最大优化
                -flto       # 链接时优化
            )
            add_definitions(-DNDEBUG)
        endif()

        # 平台特定选项
        if(TCMT_MACOS)
            add_compile_options(
                -mmacosx-version-min=11.0  # 最低macOS版本
                -stdlib=libc++
            )
        endif()
    endif()

    # 通用定义
    add_definitions(
        -DTCMT_VERSION="${PROJECT_VERSION}"
        -DTCMT_BUILD_TIMESTAMP="${CMAKE_BUILD_TIMESTAMP}"
    )

    # 安全相关选项
    if(NOT MSVC)
        add_compile_options(
            -D_FORTIFY_SOURCE=2
            -Wformat
            -Wformat-security
        )
    endif()

    # 显示设置的选项
    message(STATUS "  C++ standard: C++${CMAKE_CXX_STANDARD}")
    if(MSVC)
        message(STATUS "  MSVC options: /MP /EHsc /permissive-")
        if(CMAKE_BUILD_TYPE STREQUAL "Debug")
            message(STATUS "  Debug options: /Zi /Od /RTC1")
        else()
            message(STATUS "  Release options: /O2 /GL /Gy")
        endif()
    else()
        message(STATUS "  GCC/Clang options: -fvisibility=hidden -fstack-protector-strong")
        if(TCMT_MACOS)
            message(STATUS "  macOS options: -mmacosx-version-min=11.0 -stdlib=libc++")
        endif()
    endif()

endfunction()

# 目标特定编译选项
function(tcmt_target_set_options target)
    message(STATUS "Setting options for target ${target}...")

    # 目标属性设置
    set_target_properties(${target} PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
    )

    # 平台特定选项
    if(TCMT_WINDOWS)
        # Windows目标选项
        if(MSVC)
            target_compile_options(${target} PRIVATE
                /W4           # 警告级别4
                /wd4251       # 禁用dll接口警告（如果适用）
                /wd4275       # 禁用非dll接口基类警告
            )
        endif()

        # Windows子系统设置
        if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
            target_link_options(${target} PRIVATE
                /SUBSYSTEM:CONSOLE
                /ENTRY:mainCRTStartup
            )
        endif()

    elseif(TCMT_MACOS)
        # macOS目标选项
        target_compile_options(${target} PRIVATE
            -ObjC++          # Objective-C++支持
        )

        # macOS框架链接
        find_library(COREFOUNDATION_LIB CoreFoundation)
        find_library(IOKIT_LIB IOKit)
        find_library(FOUNDATION_LIB Foundation)
        find_library(APPKIT_LIB AppKit)

        if(COREFOUNDATION_LIB)
            target_link_libraries(${target} PRIVATE ${COREFOUNDATION_LIB})
        endif()
        if(IOKIT_LIB)
            target_link_libraries(${target} PRIVATE ${IOKIT_LIB})
        endif()
        if(FOUNDATION_LIB)
            target_link_libraries(${target} PRIVATE ${FOUNDATION_LIB})
        endif()
        if(APPKIT_LIB)
            target_link_libraries(${target} PRIVATE ${APPKIT_LIB})
        endif()
    endif()

    # 链接时优化
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        if(MSVC)
            target_link_options(${target} PRIVATE /LTCG)
        else()
            target_compile_options(${target} PRIVATE -flto)
            target_link_options(${target} PRIVATE -flto)
        endif()
    endif()

    message(STATUS "  Target options configured")
endfunction()