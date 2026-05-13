#pragma once

// Common C++ Standard Library includes
#include <string>
#include <fstream>
#include <mutex>
#include <iostream>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#ifdef TCMT_WINDOWS
// Windows specific includes
// NOTE: winsock2.h must be included BEFORE windows.h to avoid symbol redefinition
#include <winsock2.h>
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#endif

// C++20 specific headers if needed
#ifdef __cplusplus
#if __cplusplus >= 202002L
#include <concepts>
#include <span>
#endif
#endif
