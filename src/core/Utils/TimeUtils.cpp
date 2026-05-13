#include "TimeUtils.h"
#include <iomanip>
#include <sstream>

#ifdef TCMT_WINDOWS
// NOTE: winsock2.h must be included BEFORE windows.h
#include <winsock2.h>
#include <windows.h>
#elif defined(TCMT_MACOS)
#include <sys/sysctl.h>
#include <time.h>
#include <mach/mach_time.h>
#endif

std::string TimeUtils::FormatTimePoint(const SystemTimePoint& tp) {
    auto sys_tp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(tp);
    std::time_t time = std::chrono::system_clock::to_time_t(sys_tp);
    std::tm tm;
#ifdef TCMT_WINDOWS
    gmtime_s(&tm, &time);
#else
    gmtime_r(&time, &tm);
#endif
    std::stringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

std::string TimeUtils::GetCurrentLocalTime() {
#ifdef TCMT_WINDOWS
    SYSTEMTIME localTime;
    GetLocalTime(&localTime);
    std::stringstream ss;
    ss << std::setfill('0')
       << localTime.wYear << "-" << std::setw(2) << localTime.wMonth << "-"
       << std::setw(2) << localTime.wDay << " "
       << std::setw(2) << localTime.wHour << ":"
       << std::setw(2) << localTime.wMinute << ":"
       << std::setw(2) << localTime.wSecond;
    return ss.str();
#elif defined(TCMT_MACOS)
    time_t now = time(nullptr);
    std::tm tm;
    localtime_r(&now, &tm);
    std::stringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return ss.str();
#endif
}

std::string TimeUtils::GetCurrentUtcTime() {
#ifdef TCMT_WINDOWS
    SYSTEMTIME utcTime;
    GetSystemTime(&utcTime);
    std::stringstream ss;
    ss << std::setfill('0')
       << utcTime.wYear << "-" << std::setw(2) << utcTime.wMonth << "-"
       << std::setw(2) << utcTime.wDay << " "
       << std::setw(2) << utcTime.wHour << ":"
       << std::setw(2) << utcTime.wMinute << ":"
       << std::setw(2) << utcTime.wSecond;
    return ss.str();
#elif defined(TCMT_MACOS)
    time_t now = time(nullptr);
    std::tm tm;
    gmtime_r(&now, &tm);
    std::stringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return ss.str();
#endif
}

std::string TimeUtils::GetBootTimeUtc() {
#ifdef TCMT_WINDOWS
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULONGLONG uptime = GetTickCount64();
    ULONGLONG currentTime = ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
    ULONGLONG bootTime = currentTime - (uptime * 10000);
    FILETIME ftBootTime;
    ftBootTime.dwHighDateTime = bootTime >> 32;
    ftBootTime.dwLowDateTime = bootTime & 0xFFFFFFFF;
    FILETIME localBootTime;
    FileTimeToLocalFileTime(&ftBootTime, &localBootTime);
    SYSTEMTIME stBootTime;
    FileTimeToSystemTime(&localBootTime, &stBootTime);
    std::stringstream ss;
    ss << std::setfill('0')
       << stBootTime.wYear << "-" << std::setw(2) << stBootTime.wMonth << "-"
       << std::setw(2) << stBootTime.wDay << " "
       << std::setw(2) << stBootTime.wHour << ":"
       << std::setw(2) << stBootTime.wMinute << ":"
       << std::setw(2) << stBootTime.wSecond;
    return ss.str();
#elif defined(TCMT_MACOS)
    struct timeval boottime;
    size_t len = sizeof(boottime);
    int mib[2] = {CTL_KERN, KERN_BOOTTIME};
    if (sysctl(mib, 2, &boottime, &len, nullptr, 0) == 0) {
        std::tm tm;
        localtime_r(&boottime.tv_sec, &tm);
        std::stringstream ss;
        ss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
        return ss.str();
    }
    return "N/A";
#endif
}

uint64_t TimeUtils::GetUptimeMilliseconds() {
#ifdef TCMT_WINDOWS
    return GetTickCount64();
#elif defined(TCMT_MACOS)
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
#endif
}

std::string TimeUtils::GetUptime() {
    uint64_t ms = GetUptimeMilliseconds();
    uint64_t days = ms / (1000 * 60 * 60 * 24);
    uint64_t hours = (ms % (1000 * 60 * 60 * 24)) / (1000 * 60 * 60);
    uint64_t minutes = (ms % (1000 * 60 * 60)) / (1000 * 60);
    uint64_t seconds = (ms % (1000 * 60)) / 1000;
    std::stringstream ss;
    if (days > 0) ss << days << "d ";
    if (hours > 0 || days > 0) ss << hours << "h ";
    if (minutes > 0 || hours > 0 || days > 0) ss << minutes << "m ";
    ss << seconds << "s";
    return ss.str();
}
