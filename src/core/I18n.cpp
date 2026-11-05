// I18n.cpp — loads src/resources/i18n.json and answers Tr() lookups.
//
// The JSON is embedded at build time: CMake generates a source that carries it
// as a raw string literal, MSBuild embeds the same file as RCDATA, so the
// table survives copying the binary anywhere. TCMT_I18N, or an i18n.json next
// to the executable, overrides the embedded copy for trying a translation
// without a rebuild.

#include "I18n.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "Utils/Logger.h"

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace {

// {en, zh-Hans}
using Entry = std::pair<std::string, std::string>;

// Ordered so a lookup can take the caller's const char* straight through the
// transparent comparator; at a few hundred entries the log-depth search costs
// nothing next to the string it returns.
using Table = std::map<std::string, Entry, std::less<>>;

Table& TableRef() {
    static Table table;
    return table;
}

bool ParseInto(const char* data, size_t len) {
    if (!data || len == 0) return false;
    const auto doc = nlohmann::json::parse(data, data + len, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) return false;

    Table table;
    for (auto it = doc.begin(); it != doc.end(); ++it) {
        if (!it.value().is_object()) continue;
        const auto& node = it.value();
        Entry entry;
        const auto en = node.find("en");
        if (en != node.end() && en->is_string()) entry.first = en->get<std::string>();
        const auto zh = node.find("zh-Hans");
        if (zh != node.end() && zh->is_string()) entry.second = zh->get<std::string>();
        if (entry.first.empty() && entry.second.empty()) continue;
        table.emplace(it.key(), std::move(entry));
    }
    if (table.empty()) return false;
    TableRef() = std::move(table);
    return true;
}

bool ParseFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream buf;
    buf << in.rdbuf();
    const std::string text = buf.str();
    return ParseInto(text.data(), text.size());
}

// Environment variables and argv reach us as UTF-8; Windows paths are UTF-16.
std::filesystem::path PathFromUtf8(const char* text) {
#ifdef _WIN32
    const int wide = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (wide <= 1) return {};
    std::wstring out(static_cast<size_t>(wide - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text, -1, out.data(), wide);
    return std::filesystem::path(out);
#else
    return std::filesystem::path(text);
#endif
}

std::filesystem::path ExecutableDir() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4];
    constexpr DWORD kBufLen = static_cast<DWORD>(sizeof(buf) / sizeof(buf[0]));
    const DWORD n = GetModuleFileNameW(nullptr, buf, kBufLen);
    if (n == 0 || n >= kBufLen) return {};
    return std::filesystem::path(buf, buf + n).parent_path();
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = static_cast<uint32_t>(sizeof(buf));
    if (_NSGetExecutablePath(buf, &size) != 0) return {};
    return std::filesystem::path(buf).parent_path();
#else
    char buf[4096];
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    buf[n] = '\0';
    return std::filesystem::path(buf).parent_path();
#endif
}

void LoadOnce() {
    if (const char* env = std::getenv("TCMT_I18N"); env && *env) {
        if (ParseFile(PathFromUtf8(env))) {
            Logger::Debug(std::string("i18n: table replaced from TCMT_I18N=") + env);
            return;
        }
        Logger::Error(std::string("i18n: TCMT_I18N did not load (") + env + "), using the embedded table");
    }

    const std::filesystem::path dir = ExecutableDir();
    if (!dir.empty()) {
        const std::filesystem::path local = dir / "i18n.json";
        std::error_code ec;
        if (std::filesystem::exists(local, ec) && ParseFile(local)) {
            Logger::Debug("i18n: table replaced from " + local.string());
            return;
        }
    }

    size_t len = 0;
    const char* embedded = tcmt::I18nEmbeddedJson(len);
    if (!ParseInto(embedded, len))
        Logger::Error("i18n: embedded table failed to parse, ids will be shown verbatim");
}

} // namespace

#ifndef TCMT_I18N_GENERATED
#ifdef _WIN32
// MSBuild build: the JSON ships inside the executable as an RCDATA resource.
namespace tcmt {
const char* I18nEmbeddedJson(size_t& len) {
    len = 0;
    HMODULE mod = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(mod, L"I18N_JSON", RT_RCDATA);
    if (!res) return nullptr;
    HGLOBAL handle = LoadResource(mod, res);
    if (!handle) return nullptr;
    const DWORD size = SizeofResource(mod, res);
    const void* data = LockResource(handle);
    if (!data || size == 0) return nullptr;
    len = size;
    return static_cast<const char*>(data);
}
} // namespace tcmt
#else
#error "I18n.cpp needs TCMT_I18N_GENERATED (CMake) or the Windows RCDATA path (MSBuild)"
#endif
#endif

namespace tcmt {

const char* Tr(const char* id) {
    if (!id) return "";
    static std::once_flag once;
    std::call_once(once, LoadOnce);

    const Table& table = TableRef();
    const auto it = table.find(id);
    if (it == table.end()) return id;

    const Entry& entry = it->second;
    if (g_lang == Lang::ZhHans && !entry.second.empty()) return entry.second.c_str();
    return entry.first.empty() ? id : entry.first.c_str();
}

} // namespace tcmt
