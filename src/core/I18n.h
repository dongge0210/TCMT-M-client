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
    {"hint.panel",           "Tab:Panel",                                    "Tab:面板"},
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
    {"log.level",            "level",                                        "级别"},
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
    {"help.panel_focus",     "move panel focus",                             "切换面板焦点"},
    {"help.cpu_details",     "open panel details (CPU / GPU / Disks / Power / Network)", "打开面板详情（CPU / GPU / 磁盘 / 电源 / 网络面板聚焦时）"},
    {"help.select_row",      "select a process row",                         "选择进程行"},
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
    {"settings.language",    "Language",                                     "语言"},
    {"settings.sample_interval", "Sample interval",                          "采集间隔"},
    {"hint.sample",              "Sample",                                   "采集"},
    {"settings.sample_bounds",   " ms  (bounds 200-5000)",                   " 毫秒（范围 200-5000）"},
    {"lang.name.en",         "EN",                                           "EN"},
    {"lang.name.zh",         "简体中文",                                       "简体中文"},


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
    {"wifi.loc_root",        "unknown (root can't request permission)",      "未知（root 无法申请权限）"},
    {"wifi.g_root",          "root can't request permission",                "root 无法申请权限"},
    {"wifi.ssid_root",       "SSID hidden (root can't request permission)",  "SSID 不可用（root 无法申请权限）"},
    {"wifi.g1_root",         "Running as root: can't request permission",    "以 root 运行，无法申请定位权限"},
    {"bt.on",                "On (%d devices)",                              "开（%d 台设备）"},
    {"bt.off",               "Off",                                          "关"},
    {"bt.paired",            "+%d paired",                                   "+%d 已配对"},

    // ── CPU details overlay ──────────────────────────────────────────
    {"cpu.no_percore",       "Per-core data is not collected on this platform", "此平台未采集逐核数据"},
    {"details.close",        "Esc / Space close",                             "Esc / 空格关闭"},
    {"details.gpu_switch",   "Left/Right switch GPU",                         "左右键切换 GPU"},
    {"disk.none",            "No disks detected",                             "未检测到磁盘"},
    {"battery.none",         "No battery detected",                           "未检测到电池"},

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

    // ── SMART attribute names ────────────────────────────────────────
    // ATA attribute ids; NVMe health-log fields use their own pseudo ids
    // (smart.nvme.*). Both platforms read these, so the same drive shows
    // the same words whichever SMART path reached it.
    {"smart.1",              "Raw Read Error Rate",                          "读取错误率"},
    {"smart.3",              "Spin-Up Time",                                 "硬盘启动时间"},
    {"smart.4",              "Start/Stop Count",                             "启停次数"},
    {"smart.5",              "Reallocated Sectors Count",                    "重映射扇区计数"},
    {"smart.7",              "Seek Error Rate",                              "寻道错误率"},
    {"smart.9",              "Power-On Hours",                               "通电时间"},
    {"smart.10",             "Spin Retry Count",                             "启动重试次数"},
    {"smart.12",             "Power Cycle Count",                            "通电次数"},
    {"smart.15",             "Data Integrity Errors",                        "数据完整性错误"},
    {"smart.16",             "Error Log Entries",                            "错误日志条目数"},
    {"smart.148",            "Reserved",                                     "预留"},
    {"smart.149",            "Reserved",                                     "预留"},
    {"smart.167",            "SSD Protect Mode",                             "固态硬盘保护模式"},
    {"smart.168",            "SATA Phy Error Count",                         "SATA 物理层错误计数"},
    {"smart.169",            "Remaining Life",                               "剩余寿命"},
    {"smart.170",            "Available Reserved Space",                     "可用预留空间"},
    {"smart.171",            "SSD Program Fail Count",                       "SSD 编程失败计数"},
    {"smart.172",            "SSD Erase Fail Count",                         "SSD 擦除失败计数"},
    {"smart.173",            "SSD Wear Leveling Count",                      "SSD 磨损均衡计数"},
    {"smart.174",            "Unexpected Power Loss",                        "意外断电计数"},
    {"smart.175",            "Power-On Hours",                               "通电时间"},
    {"smart.176",            "Erase Fail Count",                             "擦除失败计数"},
    {"smart.177",            "Wear Leveling Count",                          "磨损均衡计数"},
    {"smart.179",            "Used Reserved Block Count",                    "已用预留块计数"},
    {"smart.180",            "Program Fail Count",                           "编程失败计数"},
    {"smart.181",            "Program Fail Count",                           "编程失败计数"},
    {"smart.182",            "Erase Fail Count",                             "擦除失败计数"},
    {"smart.183",            "SATA Downshift Count",                         "SATA 下行计数"},
    {"smart.184",            "End-to-End Error",                             "端到端错误"},
    {"smart.187",            "Reported Uncorrectable Errors",                "报告不可校正错误"},
    {"smart.188",            "Command Timeout",                              "命令超时"},
    {"smart.189",            "Factory Bad Block",                            "工厂坏块"},
    {"smart.190",            "Airflow Temperature",                          "气流温度"},
    {"smart.191",            "G-Sense Error Rate",                           "G-Sense 错误率"},
    {"smart.192",            "Power-Off Retract Count",                      "断电撤回计数"},
    {"smart.193",            "Load/Unload Cycle Count",                      "加载卸载计数"},
    {"smart.194",            "Temperature",                                  "温度"},
    {"smart.195",            "Hardware ECC Recovered",                       "硬件 ECC 恢复"},
    {"smart.196",            "Reallocation Event Count",                     "重新分配事件"},
    {"smart.197",            "Current Pending Sector",                       "当前待处理扇区"},
    {"smart.198",            "Offline Uncorrectable",                        "脱机不可校正"},
    {"smart.199",            "CRC Error Count",                              "CRC 错误计数"},
    {"smart.200",            "Write Error Rate",                             "写入错误率"},
    {"smart.201",            "Soft Read Error Rate",                         "软读取错误率"},
    {"smart.202",            "Data Address Mark Errors",                     "数据地址标记错误"},
    {"smart.203",            "Run Out Cancel",                               "耗尽取消"},
    {"smart.204",            "Soft ECC Correction",                          "软 ECC 修正"},
    {"smart.205",            "Thermal Asperity",                             "热糙度"},
    {"smart.206",            "Flying Height",                                "磁头高度"},
    {"smart.207",            "Spin Vibration",                               "硬盘旋转震动"},
    {"smart.208",            "Write Vibration",                              "硬盘写入震动"},
    {"smart.210",            "RAID Recovery",                                "RAID 恢复"},
    {"smart.218",            "CRC Error Count",                              "CRC 错误计数"},
    {"smart.220",            "Disk Shift",                                   "磁盘偏移"},
    {"smart.222",            "Loaded Hours",                                 "已加载小时"},
    {"smart.223",            "Load/Unload Retry Count",                      "加载卸载重试"},
    {"smart.224",            "Load Friction",                                "负载摩擦"},
    {"smart.225",            "Load/Unload Cycle Count",                      "负载卸载计数"},
    {"smart.226",            "Load-In Time",                                 "负载时间"},
    {"smart.227",            "Torque Amplification Count",                   "扭矩放大计数"},
    {"smart.228",            "Power-Off Retract Count",                      "断电撤回计数"},
    {"smart.230",            "SSD Life",                                     "SSD 寿命"},
    {"smart.231",            "SSD Remaining Life",                           "SSD 剩余寿命"},
    {"smart.232",            "Available Reserved Space",                     "可用预留空间"},
    {"smart.233",            "NAND Writes Total",                            "NAND 写入总量"},
    {"smart.234",            "Average Erase Count",                          "平均擦除次数"},
    {"smart.235",            "Max Erase Count",                              "最大擦除次数"},
    {"smart.240",            "Head Flying Hours",                            "磁头飞行时间"},
    {"smart.241",            "Host Writes Total",                            "主机总计写入"},
    {"smart.242",            "Host Reads Total",                             "主机总计读取"},
    {"smart.244",            "Average Erase Count",                          "平均擦除次数"},
    {"smart.245",            "Max Erase Count",                              "最大擦除次数"},
    {"smart.246",            "Total Erase Count",                            "总计擦除次数"},
    {"smart.247",            "Host Reads",                                   "主机读取量"},
    {"smart.248",            "Host Writes",                                  "主机写入量"},
    {"smart.249",            "NAND Reads",                                   "NAND 读取量"},
    {"smart.250",            "Read Error Retry Rate",                        "读取错误重试率"},
    {"smart.251",            "Remaining Spare Capacity",                     "剩余备用容量"},

    // ── NVMe health log fields ───────────────────────────────────────
    {"smart.nvme.warning",       "Critical Warning",                "严重警告标志"},
    {"smart.nvme.temp",          "Temperature",                     "温度"},
    {"smart.nvme.avail_spare",   "Available Spare",                 "可用备用空间"},
    {"smart.nvme.spare_thresh",  "Available Spare Threshold",       "可用备用空间阈值"},
    {"smart.nvme.pct_used",      "Percentage Used",                 "已用寿命百分比"},
    {"smart.nvme.data_read",     "Data Units Read",                 "数据读取量"},
    {"smart.nvme.data_written",  "Data Units Written",              "数据写入量"},
    {"smart.nvme.host_reads",    "Host Read Commands",              "主机读命令数"},
    {"smart.nvme.host_writes",   "Host Write Commands",             "主机写命令数"},
    {"smart.nvme.busy_time",     "Controller Busy Time",            "控制器忙碌时间"},
    {"smart.nvme.power_cycles",  "Power Cycles",                    "通电次数"},
    {"smart.nvme.power_on",      "Power On Hours",                  "通电时间"},
    {"smart.nvme.unsafe_shutdowns", "Unsafe Shutdowns",             "异常断电次数"},
    {"smart.nvme.media_errors",  "Media & Integrity Errors",        "媒体与完整性错误"},
    {"smart.nvme.error_log",     "Error Log Entries",               "错误日志条目数"},
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
