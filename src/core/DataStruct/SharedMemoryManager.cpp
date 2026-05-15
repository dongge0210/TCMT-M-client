// SharedMemoryManager.cpp - Include platform-specific implementation via conditional compilation
// Note: This file does not contain actual implementation, only serves as a dispatcher for platform-specific implementations

#include "SharedMemoryManager.h"
#include "../Utils/Logger.h"

// Auto-detect platform and define corresponding TCMT_ macros
#if defined(_WIN32) || defined(_WIN64)
    #ifndef TCMT_WINDOWS
        #define TCMT_WINDOWS
    #endif
#elif defined(__APPLE__) && defined(__MACH__)
    #ifndef TCMT_MACOS
        #define TCMT_MACOS
    #endif
#elif defined(__linux__)
    #ifndef TCMT_LINUX
        #define TCMT_LINUX
    #endif
#endif

// If no platform macro is defined, try to guess based on other macros
#if !defined(TCMT_WINDOWS) && !defined(TCMT_MACOS) && !defined(TCMT_LINUX)
    #if defined(_WIN32) || defined(_WIN64)
        #define TCMT_WINDOWS
    #elif defined(__APPLE__) && defined(__MACH__)
        #define TCMT_MACOS
    #elif defined(__linux__)
        #define TCMT_LINUX
    #else
        // Cannot detect platform, default to macOS (as current development target)
        #define TCMT_MACOS
    #endif
#endif

// Include corresponding implementation based on platform
#ifdef TCMT_WINDOWS
#include "SharedMemoryManager_Windows.cpp"
#elif defined(TCMT_MACOS)
#include "SharedMemoryManager_macOS.cpp"
#elif defined(TCMT_LINUX)
#include "SharedMemoryManager_Linux.cpp"
#else
#error "Unsupported platform - please define TCMT_WINDOWS, TCMT_MACOS, or TCMT_LINUX"
#endif