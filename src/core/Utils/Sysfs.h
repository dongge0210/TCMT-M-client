// Sysfs.h — small, header-only helpers for the Linux collectors.
//
// Everything here is best-effort: a missing file, a permission error or a
// malformed value returns false/empty instead of throwing. Callers are
// expected to treat false as "unknown" and keep the previous value or zero.
#pragma once

#ifdef TCMT_LINUX

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "Logger.h"

namespace tcmt::linux {

inline std::string Trim(const std::string& in) {
    const size_t first = in.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const size_t last = in.find_last_not_of(" \t\r\n");
    return in.substr(first, last - first + 1);
}

// Whole file as text (sysfs files are tiny). Returns false when unreadable.
inline bool ReadFile(const std::string& path, std::string& out) {
    std::ifstream ifs(path, std::ios::in | std::ios::binary);
    if (!ifs.is_open()) return false;
    std::ostringstream ss;
    ss << ifs.rdbuf();
    out = ss.str();
    return ifs.good() || ifs.eof();
}

// First line, trimmed. Empty string when unreadable/empty.
inline std::string ReadLine(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs.is_open()) return {};
    std::string line;
    std::getline(ifs, line);
    return Trim(line);
}

inline bool ReadUInt64(const std::string& path, uint64_t& out) {
    const std::string s = ReadLine(path);
    if (s.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long long v = std::strtoull(s.c_str(), &end, 10);
    if (errno != 0 || end == s.c_str()) return false;
    out = static_cast<uint64_t>(v);
    return true;
}

inline bool ReadInt64(const std::string& path, int64_t& out) {
    const std::string s = ReadLine(path);
    if (s.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    if (errno != 0 || end == s.c_str()) return false;
    out = static_cast<int64_t>(v);
    return true;
}

// sysfs numeric attributes are usually decimal, PCI ids are "0x1234".
inline bool ReadHexUInt(const std::string& path, unsigned int& out) {
    const std::string s = ReadLine(path);
    if (s.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const unsigned long v = std::strtoul(s.c_str(), &end, 0);
    if (errno != 0 || end == s.c_str()) return false;
    out = static_cast<unsigned int>(v);
    return true;
}

inline bool PathExists(const std::string& path) {
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0;
}

inline bool IsDirectory(const std::string& path) {
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) return false;
    return S_ISDIR(st.st_mode);
}

// Directory entries, sorted, optionally filtered by prefix. Never throws.
inline std::vector<std::string> ListDir(const std::string& path,
                                        const std::string& prefix = {}) {
    std::vector<std::string> names;
    DIR* dir = ::opendir(path.c_str());
    if (!dir) return names;
    struct dirent* entry = nullptr;
    while ((entry = ::readdir(dir)) != nullptr) {
        std::string name(entry->d_name);
        if (name == "." || name == "..") continue;
        if (!prefix.empty() && name.compare(0, prefix.size(), prefix) != 0) continue;
        names.push_back(std::move(name));
    }
    ::closedir(dir);
    std::sort(names.begin(), names.end());
    return names;
}

// Log a failure exactly once per flag. Used for root-only/absent interfaces so
// a 500 ms sampling loop cannot flood the log.
inline void LogOnce(std::atomic<bool>& flag, const std::string& message,
                    bool warning = false) {
    if (flag.exchange(true)) return;
    if (warning) Logger::Warn(message);
    else Logger::Debug(message);
}

// Every fan*_input channel under /sys/class/hwmon, named
// "<hwmon-name> Fan <label|#index>". fan*_input is RPM, not a temperature, so
// these must not be merged into the temperature list.
inline void ReadHwmonFans(std::vector<std::pair<std::string, double>>& fans) {
    for (const std::string& hw : ListDir("/sys/class/hwmon", "hwmon")) {
        const std::string base = "/sys/class/hwmon/" + hw;
        std::string hwName = ReadLine(base + "/name");
        if (hwName.empty()) hwName = hw;

        std::map<int, std::string> labels;
        std::map<int, double> inputs;
        for (const std::string& file : ListDir(base)) {
            if (file.compare(0, 3, "fan") != 0) continue;
            const size_t underscore = file.find('_', 3);
            if (underscore == std::string::npos) continue;
            int index = 0;
            try {
                index = std::stoi(file.substr(3, underscore - 3));
            } catch (...) {
                continue;
            }
            const std::string suffix = file.substr(underscore);
            if (suffix == "_label") {
                labels[index] = ReadLine(base + "/" + file);
            } else if (suffix == "_input") {
                int64_t rpm = 0;
                if (ReadInt64(base + "/" + file, rpm) && rpm > 0 && rpm <= 100000) {
                    inputs[index] = static_cast<double>(rpm);
                }
            }
        }
        for (const auto& [index, rpm] : inputs) {
            std::string label = hwName + " Fan #" + std::to_string(index);
            const auto lit = labels.find(index);
            if (lit != labels.end() && !lit->second.empty()) {
                label = hwName + " Fan " + lit->second;
            }
            fans.emplace_back(std::move(label), rpm);
        }
    }
}

// /proc/swaps: "Filename Type Size Used Priority" with KiB units.
inline bool ReadSwapUsage(uint64_t& usedBytes, uint64_t& totalBytes) {
    usedBytes = 0;
    totalBytes = 0;
    std::ifstream ifs("/proc/swaps");
    if (!ifs.is_open()) return false;
    std::string line;
    if (!std::getline(ifs, line)) return false;  // header
    while (std::getline(ifs, line)) {
        std::istringstream ss(line);
        std::string filename, type;
        uint64_t sizeKb = 0, usedKb = 0;
        int priority = 0;
        if (!(ss >> filename >> type >> sizeKb >> usedKb >> priority)) continue;
        totalBytes += sizeKb * 1024ULL;
        usedBytes += usedKb * 1024ULL;
    }
    return true;
}

}  // namespace tcmt::linux

#endif  // TCMT_LINUX
