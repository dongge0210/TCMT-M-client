// I18n.h — minimal prose-layer translation for the TUI and the updater.
//
// Design (see the TUI i18n design doc):
//   - One static id -> {en, zh-Hans} table + Tr(). No gettext, no external deps.
//   - ONLY the prose layer is translated (sentences / actions / states).
//     Panel titles, metric names, key names, units and short status tokens
//     (CPU / Use / S / GB/s / stale / FOLLOW / ON / N/A ...) stay English —
//     they are domain vocabulary and CJK would blow the 80x24 panel budget.
//   - Selection chain: TCMT_LANG > active locale (LC_ALL/LC_MESSAGES/LANG) > en.
//     A non-UTF-8 locale forces en (CJK cannot render).
//
// Header-only on purpose: it is shared by src/tui/TuiApp.cpp and
// src/core/Updater.cpp without touching the CMake source lists.
#pragma once

#include <cstring>

namespace tcmt {

enum class Lang { En, ZhHans };

// Single shared language state across translation units (C++17 inline var).
inline Lang g_lang = Lang::En;

struct I18nEntry {
    const char* id;
    const char* en;
    const char* zh;
};

// id -> (en, zh-Hans). EN strings are byte-for-byte the previous literals so
// the English UI is unchanged; zh-Hans is the reviewed translation.
inline const I18nEntry kI18nTable[] = {
    // ── top bar hints ────────────────────────────────────────────────
    {"hint.settings",        "S:Settings",                                   "S:设置"},
    {"hint.log",             "L:Log",                                        "L:日志"},
    {"hint.update",          "U:Update",                                     "U:更新"},
    {"hint.location",        "R:Location",                                   "R:定位"},
    {"hint.help",            "?:Help",                                       "?:帮助"},
    {"hint.quit",            "Q:Quit",                                       "Q:退出"},

    // ── bottom status rows ───────────────────────────────────────────
    {"status.noclients",     "no clients",                                   "无客户端"},
    {"status.since",         "since ",                                       "自 "},
    {"push.disabled",        "Push: disabled (press S to configure)",        "推送：未启用（按 S 配置）"},
    {"push.prefix",          "Push: ",                                       "推送："},
    {"push.last",            " (last ",                                      "（最近 "},
    {"push.last_close",      ")",                                            "）"},
    {"push.never",           "never",                                        "从未"},
    {"push.ago",             "s ago",                                        "s 前"},

    // ── log page ─────────────────────────────────────────────────────
    {"log.title",            "TCMT Log",                                     "TCMT 日志"},
    {"log.lines",            "lines: ",                                      "行数："},
    {"log.hint",             "scroll  Home/End  f=follow  ?=help",           "滚动  Home/End  f=跟随  ?=帮助"},
    {"log.empty",            "No log entries yet (l/Esc to dashboard)",      "暂无日志（l/Esc 返回仪表盘）"},

    // ── help page ────────────────────────────────────────────────────
    {"help.group.common",    "Common",                                       "通用"},
    {"help.group.dashboard", "Dashboard",                                    "仪表盘"},
    {"help.group.log",       "Log page",                                     "日志页"},
    {"help.server_push",     "server push settings",                         "服务器推送设置"},
    {"help.switch_log",      "switch to log page",                           "切换到日志页"},
    {"help.check_update",    "check for update",                             "检查更新"},
    {"help.request_loc",     "request Location Services (WiFi SSID)",        "请求定位权限（WiFi SSID）"},
    {"help.back",            "go back one level",                            "返回上一层"},
    {"help.close",           "close this help",                              "关闭帮助"},
    {"help.quit",            "quit TCMT Monitor",                            "退出 TCMT Monitor"},
    {"help.select_row",      "select a process row",                         "选择进程行"},
    {"help.details",         "process details (PID, CPU, memory)",           "进程详情（PID、CPU、内存）"},
    {"help.conn_list",       "connections list (expand the status chip)",    "连接列表（展开状态芯片）"},
    {"help.scroll_line",     "scroll one line",                              "滚动一行"},
    {"help.scroll_page",     "scroll one page",                              "滚动一页"},
    {"help.oldest",          "oldest entry",                                 "最早条目"},
    {"help.newest",          "newest entry",                                 "最新条目"},
    {"help.follow",          "follow newest lines",                          "跟随最新"},
    {"help.back_dash",       "back to dashboard",                            "返回仪表盘"},
    {"help.settings_hint",   "Settings page: Enter save, Esc cancel, Tab or arrows move focus, Space toggles",
                             "设置页：Enter 保存，Esc 取消，Tab/方向键移动焦点，Space 切换"},

    // ── settings page (server upload) ────────────────────────────────
    {"settings.title",       "Server Upload Settings",                       "服务器上传设置"},
    {"settings.data_upload", "Data upload",                                  "数据上传"},
    {"settings.server_url",  "Server URL",                                   "服务器 URL"},
    {"settings.skip_tls",    "Skip TLS verify",                              "跳过 TLS 校验"},
    {"settings.interval",    "Upload interval",                              "上传间隔"},
    {"settings.bounds",      " s  (bounds 1-60)",                            " 秒（范围 1-60）"},
    {"settings.status",      "Status : ",                                    "状态："},
    {"settings.hint1",       "Enter save+apply   Esc cancel   Up/Down or Tab focus",
                             "Enter 保存并应用   Esc 取消   ↑↓/Tab 移动焦点"},
    {"settings.hint2",       "Space toggles   type edits URL   digits edit interval",
                             "Space 切换   输入编辑 URL   数字键改间隔"},
    {"settings.persist",     "Settings persist to system_monitor.json (server.*)",
                             "设置保存到 system_monitor.json (server.*)"},

    // ── process details overlay ──────────────────────────────────────
    {"details.title",        "Process Details",                              "进程详情"},
    {"details.close",        "Enter/Esc/q close",                            "Enter/Esc/q 关闭"},

    // ── connections overlay ──────────────────────────────────────────
    {"conn.title",           "Connections",                                  "连接"},
    {"conn.empty",           "no clients connected",                         "无客户端连接"},
    {"conn.count",           "  %d client%s",                                "  %d 个客户端"},
    {"conn.since",           "  Since: ",                                    "  自："},
    {"conn.close",           "Enter/Esc/q close",                            "Enter/Esc/q 关闭"},

    // ── WiFi / Bluetooth ─────────────────────────────────────────────
    {"wifi.connected",       "Connected",                                    "已连接"},
    {"wifi.disconnected",    "Disconnected",                                 "未连接"},
    {"wifi.loc_off",         "unknown (Location off)",                       "未知（定位关闭）"},
    {"wifi.loc_denied",      "unknown (Location denied)",                    "未知（定位被拒）"},
    {"wifi.press_r",         "press R to request Location Services",         "按 R 请求定位权限"},
    {"wifi.grant",           "grant: System Settings > Privacy & Security",  "授权：系统设置 > 隐私与安全"},
    {"wifi.g1_denied",       "Location denied, SSID unavailable",            "定位被拒，SSID 不可用"},
    {"wifi.g1_open",         "Location not granted, SSID hidden",            "定位未授权，SSID 隐藏"},
    {"wifi.g2",              "Press R to request Location Services",         "按 R 请求定位权限"},
    {"wifi.path1",           "System Settings > Privacy & Security",         "系统设置 > 隐私与安全"},
    {"wifi.path2",           "> Location Services, allow TCMT-M",            "> 定位服务，允许 TCMT-M"},
    {"wifi.ssid_denied",     "SSID unavailable (Location Services denied)",  "SSID 不可用（定位被拒）"},
    {"bt.on",                "On (%d devices)",                              "开（%d 台设备）"},
    {"bt.off",               "Off",                                          "关"},

    // ── global / system ──────────────────────────────────────────────
    {"sys.toosmall",         "Terminal too small. Current: %dx%d",           "终端过小。当前：%dx%d"},
    {"sys.unknown_os",       "Unknown OS",                                   "未知系统"},

    // ── updater (previously hard-coded zh-Hans) ──────────────────────
    {"update.checking",      "Checking for updates…",                        "检查更新…"},
    {"update.downloading",   "Downloading update…",                          "下载更新包…"},
    {"update.newver",        "New version %s available (current %s, press U)",
                             "新版本 %s 可用（当前 %s，按 U 更新）"},
    {"update.ready",         "Update ready — restart after quitting (press Q)",
                             "更新完成，退出后重启生效（Q 退出）"},
    {"update.fail.manifest", "Release manifest incomplete",                  "发布格式不完整"},
    {"update.fail.sig",      "Signature verification failed",                "签名校验失败"},
    {"update.fail.nobin",    "Release manifest incomplete (binary missing)", "发布格式不完整（缺少二进制）"},
    {"update.fail.parse",    "Update check failed (parse)",                  "检查更新失败（解析）"},
    {"update.fail.url",      "Invalid update URL",                           "更新地址无效"},
    {"update.fail.dl",       "Download failed",                              "下载失败"},
    {"update.fail.hash",     "Hash verification failed",                     "哈希校验失败"},
    {"update.fail.tmp",      "Failed to write temp file",                    "写入临时文件失败"},
    {"update.fail.bin",      "Failed to replace binary",                     "替换二进制失败"},
};

inline constexpr int kI18nCount = static_cast<int>(sizeof(kI18nTable) / sizeof(kI18nTable[0]));

// Look up a prose string by id. Unknown ids return the id itself (visible in
// development) rather than crashing or silently rendering empty.
inline const char* Tr(const char* id) {
    for (int i = 0; i < kI18nCount; ++i) {
        if (std::strcmp(kI18nTable[i].id, id) == 0)
            return (g_lang == Lang::ZhHans) ? kI18nTable[i].zh : kI18nTable[i].en;
    }
    return id;
}

// Selection chain: TCMT_LANG > active locale > en. Non-UTF-8 forces en.
// `localeName` is the value returned by setlocale(LC_ALL, nullptr) after
// setlocale(LC_ALL, "") — it already reflects LC_ALL/LC_MESSAGES/LANG.
inline void I18nSelect(const char* tcmtLang, const char* localeName, bool utf8Locale) {
    if (!utf8Locale) {
        g_lang = Lang::En;
        return;
    }
    const char* pick = (tcmtLang && *tcmtLang) ? tcmtLang : localeName;
    if (pick && std::strncmp(pick, "zh", 2) == 0)
        g_lang = Lang::ZhHans;
    else
        g_lang = Lang::En;
}

} // namespace tcmt
