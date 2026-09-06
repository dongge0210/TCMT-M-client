// MSVC deprecation noise suppression (must come before any includes)
#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <curses.h>
#include "TuiApp.h"
#include <ctime>
#include <cstring>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <chrono>
#include <wchar.h>
#include <clocale>
#include <cstdlib>

// ── Portable wcwidth/wcswidth (missing on Windows/PDCurses) ────────
#ifndef wcswidth
static int portable_wcwidth(wchar_t wc) {
    // Zero-width characters
    if (wc == 0x200B || wc == 0x200C || wc == 0x200D || wc == 0xFEFF) return 0;
    // CJK / wide characters
    if ((wc >= 0x1100 && wc <= 0x115F) ||  // Hangul Jamo
        (wc >= 0x2E80 && wc <= 0x9FFF) ||  // CJK Radicals .. CJK Unified
        (wc >= 0xA000 && wc <= 0xA4FF) ||  // Yi
        (wc >= 0xAC00 && wc <= 0xD7AF) ||  // Hangul Syllables
        (wc >= 0xF900 && wc <= 0xFAFF) ||  // CJK Compatibility
        (wc >= 0xFE10 && wc <= 0xFE19) ||  // Vertical forms
        (wc >= 0xFE30 && wc <= 0xFE6F) ||  // CJK Compatibility Forms
        (wc >= 0xFF01 && wc <= 0xFF60) ||  // Fullwidth Forms
        (wc >= 0xFFE0 && wc <= 0xFFE6) ||  // Fullwidth Signs
        (wc >= 0x20000 && wc <= 0x2FFFF))  // CJK Extension B+
        return 2;
    // Control characters
    if (wc < 0x20 || (wc >= 0x7F && wc < 0xA0)) return -1;
    return 1;
}

static int portable_wcswidth(const wchar_t* wcs, size_t n) {
    int w = 0;
    for (size_t i = 0; i < n && wcs[i]; i++) {
        int cw = portable_wcwidth(wcs[i]);
        if (cw < 0) return -1;
        w += cw;
    }
    return w;
}

#ifndef wcwidth
#define wcwidth portable_wcwidth
#endif
#ifndef wcswidth
#define wcswidth portable_wcswidth
#endif
#endif
// ─────────────────────────────────────────────────────────────────────

namespace tcmt {

// A snapshot this old means the monitor loop has stopped feeding the TUI;
// the dashboard then shows a "stale Ns" chip on the status row.
static constexpr int64_t kStaleAfterUs = 3000000;  // 3 s

// ── Severity color policy (#5) ─────────────────────────────────────────────
// One table of thresholds, one pair of helpers — no more scattered integer
// literals (temps 60/80 in three places, battery, health…). Curses pair
// roles are fixed in InitColors: 3 = warning (yellow), 4 = critical (red).
// Normal values render in the DEFAULT foreground: painting every normal row
// green made the whole dashboard a colored field, so no value was actually
// highlighted (the log page got the same fix earlier).
static int HighIsWorsePair(double value, double warnAt, double critAt) {
    return (value >= critAt) ? 4 : (value >= warnAt) ? 3 : -1;  // -1 = leave default fg
}
static int LowIsWorsePair(double value, double warnBelow, double critBelow) {
    return (value <= critBelow) ? 4 : (value <= warnBelow) ? 3 : -1;
}
// Thresholds by metric (values unchanged from the pre-#5 rules; only the
// "paint everything" policy changed).
static constexpr double kTempWarn = 60.0, kTempCrit = 80.0;      // sensor temps, °C
static constexpr double kProcCpuWarn = 20.0, kProcCpuCrit = 50.0; // per-process cpu %
static constexpr double kUsageWarn = 70.0, kUsageCrit = 90.0;    // cpu/ram/gpu usage % (bar rows)
static constexpr double kHealthWarn = 80.0, kHealthCrit = 60.0;  // battery health % (low is worse)
static constexpr double kBattWarn = 50.0, kBattCrit = 20.0;      // battery charge % (low is worse)

// ============================================================================
// TuiApp
// ============================================================================

// Platform capability probe — the ONLY #ifdef switch point in this file for
// "which UI pieces exist here". Everything below (key bindings, page state,
// hints, guidance text) reads caps_ at runtime, so the three platforms no
// longer drift apart in behavior by accident.
static PlatformCaps DetectPlatformCaps() {
    PlatformCaps c;
#ifdef TCMT_WINDOWS
    // Windows console pairs the dashboard with the standalone Win32 log
    // window (LogWindow); there is no in-TUI log page.
    c.nativeLogWindow = true;
#else
    // macOS/Linux keep the in-TUI log page for headless/SSH sessions.
    c.inlineLogPage = true;
#endif
#ifdef __PDCURSES__
    c.resizableTerminal = true;   // is_termresized() reports live resizes
#endif
#ifdef TCMT_MACOS
    c.nativeLogWindow = true;     // AppKit MacLogWindow
    c.wifiLocationServices = true; // macOS 15+ Location Services SSID flow
#endif
    return c;
}

TuiApp::TuiApp() : caps_(DetectPlatformCaps()) {
    logBuf_ = &defaultBuffer_;
}

TuiApp::~TuiApp() {
    Stop();
}

void TuiApp::Start() {
    if (running_.load()) return;
    running_ = true;
    thread_ = std::thread(&TuiApp::Run, this);
}

void TuiApp::Stop() {
    // Always join if a thread exists. The TUI thread sets running_ = false
    // itself when quitting via 'q', so an early return here would leave
    // thread_ joinable and std::terminate() in ~thread at scope exit.
    running_ = false;
    if (thread_.joinable()) {
        thread_.join();
    }
    SafeEndwin();
    std::fflush(stdout);
}

bool TuiApp::IsRunning() const {
    return running_.load();
}

void TuiApp::UpdateData(const TuiData& data) {
    std::lock_guard<std::mutex> lock(dataMutex_);
    data_ = data;
    lastUpdateUs_.store(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

void TuiApp::SetLogBuffer(LogBuffer* buf) {
    logBuf_ = buf ? buf : &defaultBuffer_;
}

void TuiApp::InitColors() {
    // NO_COLOR: never start colors. With color support uninitialized curses
    // treats every COLOR_PAIR() as inert, so the UI renders purely in the
    // terminal's default foreground — no palette probing, no color codes.
    if (!has_colors() || noColor_) return;
    start_color();
    use_default_colors();

    // Pair roles (semantic; severity helpers above map to 3/4):
    //   1 header cyan · 2 ok green (upload sparkline, update done) ·
    //   3 warning yellow · 4 critical red · 5 labels white · 6 blue (bars)
    init_pair(1, COLOR_CYAN, -1);    // Header
    init_pair(2, COLOR_GREEN, -1);   // OK / normal-as-good (sparkline U:, update done)
    init_pair(3, COLOR_YELLOW, -1);  // Warning
    init_pair(4, COLOR_RED, -1);     // Critical
    init_pair(5, COLOR_WHITE, -1);   // Label / titles
    init_pair(6, COLOR_BLUE, -1);    // Usage bars, traffic download
}

std::string TuiApp::FormatSize(uint64_t bytes) {
    const double GB = 1024.0 * 1024.0 * 1024.0;
    const double MB = 1024.0 * 1024.0;
    const double KB = 1024.0;

    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1);
    if (bytes >= (uint64_t)(10 * GB)) ss << (bytes / GB) << " GB";
    else if (bytes >= (uint64_t)GB) ss << (bytes / GB) << " GB";
    else if (bytes >= (uint64_t)MB) ss << (bytes / MB) << " MB";
    else if (bytes >= (uint64_t)KB) ss << (bytes / KB) << " KB";
    else ss << bytes << " B";
    return ss.str();
}

std::string TuiApp::FormatSpeed(uint64_t bps) {
    const double GB = 1000.0 * 1000.0 * 1000.0;
    const double MB = 1000.0 * 1000.0;
    const double KB = 1000.0;

    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1);
    if (bps >= (uint64_t)GB) ss << (bps / GB) << " Gbps";
    else if (bps >= (uint64_t)MB) ss << (bps / MB) << " Mbps";
    else if (bps >= (uint64_t)KB) ss << (bps / KB) << " Kbps";
    else ss << bps << " bps";
    return ss.str();
}

// Throughput in bytes/second, decimal (1000) — the same base as FormatSpeed
// so "MB/s" on this screen never silently means MiB/s. Mixing the binary
// FormatSize (used for storage capacities above) into rate lines made one
// "MB" glyph carry two bases on the same screen.
std::string TuiApp::FormatRate(uint64_t bytesPerSec) {
    const double GB = 1000.0 * 1000.0 * 1000.0;
    const double MB = 1000.0 * 1000.0;
    const double KB = 1000.0;

    std::ostringstream ss;
    ss << std::fixed << std::setprecision(1);
    if (bytesPerSec >= (uint64_t)GB) ss << (bytesPerSec / GB) << " GB/s";
    else if (bytesPerSec >= (uint64_t)MB) ss << (bytesPerSec / MB) << " MB/s";
    else if (bytesPerSec >= (uint64_t)KB) ss << (bytesPerSec / KB) << " KB/s";
    else ss << bytesPerSec << " B/s";
    return ss.str();
}

// Draw one "label + usage bar + right-aligned value" row. The fill is
// colored — blue while the value is normal, warning/critical when it
// crosses the usage thresholds (#5 table) — and the empty track renders
// dim in the terminal's default foreground, so the bar reads as a
// skeleton rather than a second colored field. All value lines on the
// panel share the same right edge (value column), which is what makes
// the dashboard scannable.
void TuiApp::DrawUsageBarRow(WINDOW* win, int y, int x0, int maxW,
                             const char* label, double pct,
                             const std::string& value) {
    const int valueX = x0 + maxW - 2;                       // shared right edge
    const int vw = std::min(static_cast<int>(value.size()),
                            (std::max)(4, maxW - 8));       // truncation guard
    const int valueStart = valueX - vw + 1;
    const int barEnd = valueStart - 2;                      // 1-col gap before value
    int barStart = x0 + 2 + static_cast<int>(std::strlen(label)) + 1;
    if (barEnd - barStart + 1 < 4) barStart = barEnd - 3;   // keep a minimal bar
    if (barStart < x0 + 2) barStart = x0 + 2;

    mvwprintw(win, y, x0 + 2, "%s", label);

    double p = pct;
    if (p < 0) p = 0;
    if (p > 100) p = 100;
    const int barW = barEnd - barStart + 1;
    const int nFill = static_cast<int>(p * barW / 100.0);

    // B · neutral usage bar: solid ▰ fill in the DEFAULT foreground (theme-
    // aware, no dedicated bar color) over a dim ▱ track; crossing the usage
    // thresholds moves the fill to the warning/critical pair. The green
    // accent is reserved for focus/ok segments, per the B direction.
    const int sev = HighIsWorsePair(pct, kUsageWarn, kUsageCrit);
    int x = barStart;
    if (sev >= 0) wattron(win, COLOR_PAIR(sev));
    for (int i = 0; i < nFill && x <= barEnd; ++i) mvwaddstr(win, y, x++, barFill_.c_str());
    if (sev >= 0) wattroff(win, COLOR_PAIR(sev));
    if (x <= barEnd) {
        wattron(win, A_DIM);
        while (x <= barEnd) mvwaddstr(win, y, x++, barTrack_.c_str());
        wattroff(win, A_DIM);
    }
    if (sev >= 0) wattron(win, COLOR_PAIR(sev));
    mvwprintw(win, y, valueStart, "%.*s", vw, value.c_str());
    if (sev >= 0) wattroff(win, COLOR_PAIR(sev));
}

// Panel title strip — B (powerline) segmented form: a reverse "chip" with
// the title, followed by a dim textured remainder to the panel edge. The
// seam between chip and remainder is the segment edge — no border needed.
// Reverse adapts to any terminal theme and survives NO_COLOR; the focused
// chip switches to green bold text (pair 2), falling back to bold under
// NO_COLOR where the pair is inert. Zero row-budget cost, as before.
void TuiApp::DrawPanelTitle(WINDOW* win, int y, int x0, int maxW,
                            const char* title, bool focused) {
    if (maxW < 5) return;
    int tw = static_cast<int>(std::strlen(title));
    if (tw > maxW - 2) tw = maxW - 2;

    // Chip: ' title ' — reversed by default, green+bold when focused.
    if (focused) {
        wattron(win, COLOR_PAIR(2) | A_BOLD);
        mvwprintw(win, y, x0, " %-*s ", tw, title);
        wattroff(win, COLOR_PAIR(2) | A_BOLD);
    } else {
        wattron(win, A_REVERSE);
        mvwprintw(win, y, x0, " %-*s ", tw, title);
        wattroff(win, A_REVERSE);
    }

    // Remainder: dim shade texture up to the panel edge.
    const int rest = maxW - (tw + 2);
    if (rest > 0) {
        wattron(win, A_DIM);
        for (int x = x0 + tw + 2; x < x0 + maxW; ++x) {
            mvwaddstr(win, y, x, segShade_.c_str());
        }
        wattroff(win, A_DIM);
    }
}

// "label left, value right-aligned at the panel edge" — the plain-data
// variant of DrawUsageBarRow. Optional pair colors the value (severity).
void TuiApp::DrawLabeledValue(WINDOW* win, int y, int x0, int maxW,
                              const char* label, const std::string& value, int pair) {
    const int valueX = x0 + maxW - 2;
    const int vw = std::min(static_cast<int>(value.size()),
                            (std::max)(4, maxW - 6));
    const int valueStart = valueX - vw + 1;
    const int labelMax = valueStart - (x0 + 2) - 1;   // 1-col gap before value
    if (labelMax < 3) {   // too tight for a label: value alone, right-aligned
        if (pair >= 0) wattron(win, COLOR_PAIR(pair));
        mvwprintw(win, y, valueStart, "%.*s", vw, value.c_str());
        if (pair >= 0) wattroff(win, COLOR_PAIR(pair));
        return;
    }
    mvwprintw(win, y, x0 + 2, "%.*s", labelMax, label);
    if (pair >= 0) wattron(win, COLOR_PAIR(pair));
    mvwprintw(win, y, valueStart, "%.*s", vw, value.c_str());
    if (pair >= 0) wattroff(win, COLOR_PAIR(pair));
}

std::string TuiApp::TrimRight(const std::string& s, size_t maxLen) {
    if (s.size() <= maxLen) return s;
    return s.substr(0, maxLen);
}

// ── UTF-8 display width helpers (CJK-aware) ──────────────────────────
// Returns column display width of a UTF-8 string (2 for CJK chars).
static int utf8_display_width(const std::string& s) {
    if (s.empty()) return 0;
    size_t wlen = std::mbstowcs(nullptr, s.c_str(), 0);
    if (wlen == (size_t)-1) return (int)s.size();  // not valid UTF-8, fallback
    std::wstring wstr(wlen, L'\0');
    std::mbstowcs(&wstr[0], s.c_str(), wlen);
    int w = wcswidth(wstr.c_str(), wlen);
    return (w >= 0) ? w : (int)s.size();
}

// Truncate a UTF-8 string so its display width ≤ maxW, appending "~" if cut.
static std::string utf8_truncate(const std::string& s, int maxW) {
    if (maxW < 1) return std::string();
    if (utf8_display_width(s) <= maxW) return s;

    size_t wlen = std::mbstowcs(nullptr, s.c_str(), 0);
    if (wlen == (size_t)-1) {
        // Fallback: byte truncation
        return s.substr(0, std::max(0, maxW - 1)) + "~";
    }
    std::wstring wstr(wlen, L'\0');
    std::mbstowcs(&wstr[0], s.c_str(), wlen);

    int targetW = maxW - 1;  // leave room for "~"
    int curW = 0;
    size_t i = 0;
    for (; i < wlen; i++) {
        int cw = wcwidth(wstr[i]);
        if (cw < 0) cw = 1;
        if (curW + cw > targetW) break;
        curW += cw;
    }

    // Convert wide chars back to UTF-8
    std::string out;
    for (size_t j = 0; j < i; j++) {
        wchar_t wc = wstr[j];
        unsigned int uc = (unsigned int)wc;
        if (uc < 0x80)      { out += (char)uc; }
        else if (uc < 0x800) { out += (char)(0xC0 | (uc >> 6));
                                out += (char)(0x80 | (uc & 0x3F)); }
        else if (uc < 0x10000) { out += (char)(0xE0 | (uc >> 12));
                                out += (char)(0x80 | ((uc >> 6) & 0x3F));
                                out += (char)(0x80 | (uc & 0x3F)); }
        else                { out += (char)(0xF0 | (uc >> 18));
                                out += (char)(0x80 | ((uc >> 12) & 0x3F));
                                out += (char)(0x80 | ((uc >> 6) & 0x3F));
                                out += (char)(0x80 | (uc & 0x3F)); }
    }
    out += "~";
    return out;
}
// ─────────────────────────────────────────────────────────────────────

void TuiApp::DrawHeader(WINDOW* win, const TuiData& data) {
    // Title only. Key hints, the update banner and the stale chip live on
    // the status row and are laid out together in Run() so they can never
    // collide on narrow terminals.
    int rows, cols;
    getmaxyx(win, rows, cols);
    (void)rows;
    wattron(win, COLOR_PAIR(1) | A_BOLD);
    std::string title = "TCMT Monitor  " + data.timestamp;
    int x = (cols - static_cast<int>(title.size())) / 2;
    mvwprintw(win, 0, std::max(0, x), "%s", title.c_str());
    wattroff(win, COLOR_PAIR(1) | A_BOLD);
}

int TuiApp::DrawCpuPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW) {
    if (maxW < 10) return 0;
    int lines = 0;

    DrawPanelTitle(win, y + lines, x0, maxW, "CPU");
    lines++;

    auto name = TrimRight(data.cpuName, maxW - 4);
    mvwprintw(win, y + lines, x0 + 2, "%.*s", maxW - 2, name.c_str());
    lines++;

    {
        char val[32];
        snprintf(val, sizeof(val), "%.1f%%", data.cpuUsage);
        DrawUsageBarRow(win, y + lines, x0, maxW, "Use:", data.cpuUsage, val);
    }
    lines++;

    if (data.performanceCores > 0 || data.efficiencyCores > 0) {
        std::ostringstream ss;
        ss << "P:" << data.performanceCores;
        if (data.pCoreFreq > 0) {
            ss << "(" << static_cast<int>(data.pCoreFreq);
            if (data.pCoreMaxFreq > 0 && static_cast<int>(data.pCoreMaxFreq) != static_cast<int>(data.pCoreFreq))
                ss << "/" << static_cast<int>(data.pCoreMaxFreq);
            ss << "M)";
        }
        ss << "  E:" << data.efficiencyCores;
        if (data.eCoreFreq > 0) {
            ss << "(" << static_cast<int>(data.eCoreFreq);
            if (data.eCoreMaxFreq > 0 && static_cast<int>(data.eCoreMaxFreq) != static_cast<int>(data.eCoreFreq))
                ss << "/" << static_cast<int>(data.eCoreMaxFreq);
            ss << "M)";
        }
        // Composite P:/E: line stays left-aligned — it carries two values.
        mvwprintw(win, y + lines, x0 + 2, "%.*s", maxW - 2, ss.str().c_str());
    } else if (data.physicalCores > 0) {
        DrawLabeledValue(win, y + lines, x0, maxW, "Cores:",
                         std::to_string(data.physicalCores));
    }
    lines++;

    return lines;
}

int TuiApp::DrawMemoryPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW) {
    if (maxW < 10) return 0;
    int lines = 0;

    DrawPanelTitle(win, y + lines, x0, maxW, "RAM");
    lines++;

    double upct = (data.totalMemory > 0) ? 100.0 * data.usedMemory / data.totalMemory : 0;
    {
        auto usedStr = FormatSize(data.usedMemory);
        auto totalStr = FormatSize(data.totalMemory);
        DrawUsageBarRow(win, y + lines, x0, maxW, "Used:", upct,
                        usedStr + " / " + totalStr);
    }
    lines++;

    DrawLabeledValue(win, y + lines, x0, maxW, "Avail:", FormatSize(data.availableMemory));
    lines++;

    if (data.compressedMemory > 0) {
        DrawLabeledValue(win, y + lines, x0, maxW, "Compressed:",
                         FormatSize(data.compressedMemory));
        lines++;
    }
    if (data.swapTotal > 0) {
        DrawLabeledValue(win, y + lines, x0, maxW, "Swap:",
                         FormatSize(data.swapUsed) + " / " + FormatSize(data.swapTotal));
        lines++;
    }
    if (data.ramSpeed > 0) {
        std::string ramStr = std::string(data.ramType) + "-" + std::to_string(data.ramSpeed);
        mvwprintw(win, y + lines, x0 + 2, "%.*s", maxW - 4, ramStr.c_str());
        lines++;
    }

    return lines;
}

int TuiApp::DrawGpuPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW) {
    if (maxW < 10) return 0;
    int lines = 0;

    DrawPanelTitle(win, y + lines, x0, maxW, "GPU");
    lines++;

#ifndef TCMT_MACOS
    auto name = TrimRight(data.gpuName, maxW - 4);
    mvwprintw(win, y + lines, x0 + 2, "%.*s", maxW - 2, name.c_str());
    lines++;
#endif

    {
        char val[32];
        snprintf(val, sizeof(val), "%.1f%%", data.gpuUsage);
        DrawUsageBarRow(win, y + lines, x0, maxW, "Use:", data.gpuUsage, val);
    }
    lines++;

#ifndef TCMT_MACOS
    if (data.gpuMemoryPercent > 1 && data.gpuMemory > 0) {
        uint64_t used = (uint64_t)(data.gpuMemory * data.gpuMemoryPercent / 100.0);
        DrawLabeledValue(win, y + lines, x0, maxW, "VRAM:",
                         FormatSize(used) + " / " + FormatSize(data.gpuMemory));
        lines++;
    }
#endif

    if (data.gpuFreq > 0) {
        std::string freqStr;
        if (data.gpuMaxFreq > 0 && static_cast<int>(data.gpuMaxFreq) != static_cast<int>(data.gpuFreq)) {
            char buf[48];
            snprintf(buf, sizeof(buf), "%d/%d MHz", static_cast<int>(data.gpuFreq), static_cast<int>(data.gpuMaxFreq));
            freqStr = buf;
        } else {
            freqStr = std::to_string(static_cast<int>(data.gpuFreq)) + " MHz";
        }
        DrawLabeledValue(win, y + lines, x0, maxW, "Freq:", freqStr);
        lines++;
    }
    for (const auto& gf : data.gpuFans) {
        char val[32];
        if (gf.isRpm)
            snprintf(val, sizeof(val), "%d RPM", gf.speedRpm);
        else
            snprintf(val, sizeof(val), "%d%%", gf.speedRpm);
        char label[32];
        snprintf(label, sizeof(label), "Fan#%u:", gf.index);
        DrawLabeledValue(win, y + lines, x0, maxW, label, val);
        lines++;
    }
    return lines;
}

int TuiApp::DrawDiskPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW) {
    if (maxW < 10) return 0;

    DrawPanelTitle(win, y, x0, maxW, "Disks");
    int lines = 1;

    for (const auto& d : data.disks) {
        // Compact single-line format: [C:] Label  92% 210G/228G
        char lineBuf[128];
        int off = 0;
        if (d.letter >= 'A' && d.letter <= 'Z')
            off = snprintf(lineBuf, sizeof(lineBuf), "[%c:] ", d.letter);
        auto label = d.label.empty() ? "?" : d.label;
        if (label.size() > 10) label = label.substr(0, 8) + "..";
        double upct = (d.totalSize > 0) ? 100.0 * d.usedSpace / d.totalSize : 0;
        auto usedStr = FormatSize(d.usedSpace);
        auto totalStr = FormatSize(d.totalSize);
        off += snprintf(lineBuf + off, sizeof(lineBuf) - off,
                       "%s  %d%% %s/%s",
                       label.c_str(), static_cast<int>(upct),
                       usedStr.c_str(), totalStr.c_str());
        lineBuf[off] = '\0';
        mvwprintw(win, y + lines, x0 + 2, "%.*s", maxW - 4, lineBuf);
        lines++;
    }
    return lines;
}

int TuiApp::DrawNetworkPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW) {
    if (maxW < 10) return 0;
    DrawPanelTitle(win, y, x0, maxW, "Network");
    int lines = 1;

    for (const auto& n : data.adapters) {
        if (n.ip.empty()) continue;
        auto name = TrimRight(n.name, maxW - 4);
        mvwprintw(win, y + lines, x0 + 2, "%.*s", maxW - 4, name.c_str());
        lines++;
        mvwprintw(win, y + lines, x0 + 4, "%.*s", maxW - 6, n.ip.c_str());
        lines++;
        if (!n.type.empty()) {
            auto typeStr = TrimRight(n.type, maxW - 6);
            mvwprintw(win, y + lines, x0 + 4, "%.*s", maxW - 6, typeStr.c_str());
            lines++;
        }
        if (!n.mac.empty() && n.mac != "00:00:00:00:00:00") {
            mvwprintw(win, y + lines, x0 + 4, "%.*s", maxW - 6, n.mac.c_str());
            lines++;
        }
        if (n.speed > 0) {
            auto speedStr = FormatSpeed(n.speed);
            mvwprintw(win, y + lines, x0 + 4, "L: %.*s", maxW - 8, speedStr.c_str());
            lines++;
        }
        if (!n.ip.empty()) {
            // Rates are decimal bytes/s (FormatRate), not binary storage
            // sizes — see FormatRate for why the bases must not mix.
            mvwprintw(win, y + lines, x0 + 4, "D: %.*s", maxW - 8,
                      FormatRate(n.downloadSpeed).c_str());
            lines++;
            mvwprintw(win, y + lines, x0 + 4, "U: %.*s", maxW - 8,
                      FormatRate(n.uploadSpeed).c_str());
            lines++;
        }
    }
    return lines;
}

int TuiApp::DrawWifiBluetoothPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW) {
    if (maxW < 10) return 0;
    int lines = 0;

    // Three explicit states: n/a (no adapter present) / Disconnected
    // (adapter on, no association) / Connected (SSID + details).
    if (!data.hasWiFi) {
        wattron(win, COLOR_PAIR(5));
        mvwprintw(win, y + lines, x0 + 2, "WiFi:");
        wattroff(win, COLOR_PAIR(5));
        mvwprintw(win, y + lines, x0 + 8, "%.*s", maxW - 10, "n/a");
        lines++;
    } else if (!data.wifiConnected) {
        wattron(win, COLOR_PAIR(5));
        mvwprintw(win, y + lines, x0 + 2, "WiFi:");
        wattroff(win, COLOR_PAIR(5));
        // macOS 15+: without a granted Location Services permission the
        // system hides BOTH the SSID and the association state — so
        // "Disconnected" would be a guess, not a reading. Say what is
        // actually true and give the next step instead; "Disconnected" is
        // only reported when Location is not standing in the way.
        const bool locBlocked = caps_.wifiLocationServices &&
            (data.wifiLocationStatus == 0 || data.wifiLocationStatus == 1 || data.wifiLocationDenied);
        if (locBlocked) {
            const bool denied = (data.wifiLocationStatus == 1 || data.wifiLocationDenied);
            wattron(win, COLOR_PAIR(6));
            mvwprintw(win, y + lines, x0 + 8, "%.*s", maxW - 10,
                      denied ? "unknown (Location denied)" : "unknown (Location off)");
            wattroff(win, COLOR_PAIR(6));
            lines++;
            wattron(win, COLOR_PAIR(6));
            mvwprintw(win, y + lines, x0 + 8, "%.*s", maxW - 10,
                      denied ? "grant: System Settings > Privacy & Security"
                              : "press R to request Location Services");
            wattroff(win, COLOR_PAIR(6));
            lines++;
        } else {
            wattron(win, COLOR_PAIR(3));
            mvwprintw(win, y + lines, x0 + 8, "%.*s", maxW - 10, "Disconnected");
            wattroff(win, COLOR_PAIR(3));
            lines++;
        }
    } else {
        std::string wifiStr = "Connected";
        if (data.wifiLocationStatus == 1 || data.wifiLocationDenied) {
            wifiStr += "  SSID unavailable (Location Services denied)";
        } else {
            if (!data.wifiSSID.empty()) wifiStr += "  SSID: " + data.wifiSSID;
            if (!data.wifiBSSID.empty()) wifiStr += "  BSSID: " + data.wifiBSSID;
            if (data.wifiChannel > 0) wifiStr += "  Ch: " + std::to_string(data.wifiChannel);
            if (data.wifiRSSI < 0) wifiStr += "  RSSI: " + std::to_string(data.wifiRSSI) + " dBm";
            if (!data.wifiSecurity.empty()) wifiStr += "  " + data.wifiSecurity;
            if (!data.wifiBand.empty()) wifiStr += "  " + data.wifiBand;
            if (!data.wifiGen.empty()) wifiStr += "  " + data.wifiGen;
            if (data.wifiTxRate > 0) wifiStr += "  Tx: " + std::to_string(static_cast<int>(data.wifiTxRate)) + "Mbps";
        }
        wifiStr = TrimRight(wifiStr, maxW - 8);
        wattron(win, COLOR_PAIR(5));
        mvwprintw(win, y + lines, x0 + 2, "WiFi:");
        wattroff(win, COLOR_PAIR(5));
        wattron(win, COLOR_PAIR(2));
        mvwprintw(win, y + lines, x0 + 8, "%.*s", maxW - 10, wifiStr.c_str());
        wattroff(win, COLOR_PAIR(2));
        lines++;

        // Location Services guidance — the R affordance stays visible in
        // every non-authorized state so the user can tell where they stand
        // by pressing it: an authorization prompt appears while the decision
        // is still open; nothing happens when it was denied (and the SSID
        // stays hidden) — the settings path below is then the only way in.
        // A platform capability: only macOS fills wifiLocationStatus, so
        // Windows (WLAN API) and Linux must never see this macOS flow.
        if (caps_.wifiLocationServices &&
            (data.wifiLocationStatus == 0 || data.wifiLocationStatus == 1 || data.wifiLocationDenied)) {
            wattron(win, COLOR_PAIR(6));
            if (data.wifiLocationStatus == 1 || data.wifiLocationDenied) {
                mvwprintw(win, y + lines, x0 + 2, "Location denied, SSID unavailable");
            } else {
                mvwprintw(win, y + lines, x0 + 2, "Location not granted, SSID hidden");
            }
            lines++;
            mvwprintw(win, y + lines, x0 + 2, "Press R to request Location Services");
            lines++;
            if (data.wifiLocationStatus == 1 || data.wifiLocationDenied) {
                mvwprintw(win, y + lines, x0 + 2, "System Settings > Privacy & Security");
                lines++;
                mvwprintw(win, y + lines, x0 + 2, "> Location Services, allow TCMT-M");
                lines++;
            } else {
                mvwprintw(win, y + lines, x0 + 2, "System Settings > Privacy & Security");
                lines++;
            }
            wattroff(win, COLOR_PAIR(6));
        }
    }

    if (data.hasBluetooth) {
        std::string btStr = data.btPowerOn
            ? "On (" + std::to_string(data.btDeviceCount) + " devices)"
            : "Off";
        lines++;  // blank line between WiFi and BT
        mvwprintw(win, y + lines, x0 + 2, "BT: %.*s", maxW - 8, btStr.c_str());
        lines++;
    }

    return lines;
}

int TuiApp::DrawDisplayPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW) {
    if (maxW < 10) return 0;
    if (data.displays.empty()) return 0;
    int lines = 0;

    DrawPanelTitle(win, y + lines, x0, maxW, "Displays");
    lines++;

    for (const auto& d : data.displays) {
        std::string line = d.name;
        line += " " + std::to_string(d.width) + "x" + std::to_string(d.height);
        if (d.refreshRate > 0)
            line += " @" + std::to_string(d.refreshRate) + "Hz";
        if (d.isHDR)
            line += " HDR";
        if (d.isBuiltin)
            line += " (built-in)";
        else
            line += " scale=" + std::to_string(d.backingScale).substr(0, 3);
        mvwprintw(win, y + lines, x0 + 2, "%.*s", maxW - 4, TrimRight(line, maxW - 4).c_str());
        lines++;
    }

    return lines;
}

int TuiApp::DrawTpmPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW) {
    if (maxW < 10) return 0;
    if (data.tpmInfo.empty() || data.tpmInfo == "No TPM") return 0;
    DrawPanelTitle(win, y, x0, maxW, "TPM");
    mvwprintw(win, y + 1, x0 + 2, "%.*s", maxW - 2, data.tpmInfo.c_str());
    return 2;
}

int TuiApp::DrawPhysicalDiskPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW) {
    if (maxW < 10 || data.physicalDisks.empty()) return 0;
    int lines = 0;

    for (const auto& pd : data.physicalDisks) {
        if (y + lines >= LINES - 5) break;
        // Show model + type, then whatever health data is available.
        // Pool disks may have health% from WMI but no direct SMART access,
        // so we display partial data instead of a blank line.
        std::string line = pd.model;
        if (!pd.diskType.empty()) line += " " + pd.diskType;
        {
            char buf[128];
            int off = 0;
            if (pd.smartSupported && pd.healthPct > 0)
                off += snprintf(buf + off, sizeof(buf) - off, " %d%%", pd.healthPct);
            else
                off += snprintf(buf + off, sizeof(buf) - off, " N/A");
            if (pd.temperature > 0)
                off += snprintf(buf + off, sizeof(buf) - off, " %.0fC", pd.temperature);
            if (pd.smartSupported && pd.powerOnHours > 0)
                off += snprintf(buf + off, sizeof(buf) - off, " %lluh", pd.powerOnHours);
            if (pd.smartSupported && pd.wearLeveling > 0 && pd.wearLeveling <= 1.0)
                off += snprintf(buf + off, sizeof(buf) - off, " WL%.0f%%", pd.wearLeveling * 100.0);
            line += buf;
        }
        line = TrimRight(line, maxW - 2);
        mvwprintw(win, y + lines++, x0, "%.*s", maxW, line.c_str());
    }
    return lines;
}

int TuiApp::DrawTempPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW) {
    if (maxW < 10) return 0;
    DrawPanelTitle(win, y, x0, maxW, "Temps");
    int lines = 1;

    int halfW = maxW / 2;
    // Grow the grid with the space actually available below the title
    // (content may run down to row LINES-8, see Run), capped at 8 rows:
    // with room, every sensor is visible at once and no paging happens;
    // the 3-second auto-rotate only kicks in when the panel truly overflows.
    const int availRows = (LINES - 8) - (y + 1);
    const int contentRows = (std::max)(1, (std::min)(availRows, 8));

    // All sensors are pre-filtered upstream; display everything
    std::vector<std::pair<std::string, double>> displayTemps;
    for (const auto& [name, temp] : data.temperatures)
        displayTemps.push_back({name, temp});

    int perPage = contentRows * 2;
    int totalPages = (std::max)(1, (static_cast<int>(displayTemps.size()) + perPage - 1) / perPage);
    bool needPaging = totalPages > 1;

    static int currentPage = 0;
    static auto lastPageFlip = std::chrono::steady_clock::now();
    if (needPaging) {
        auto tNow = std::chrono::steady_clock::now();
        if (currentPage >= totalPages) currentPage = 0;  // shrink guard
        if (std::chrono::duration_cast<std::chrono::seconds>(tNow - lastPageFlip).count() >= 3) {
            currentPage = (currentPage + 1) % totalPages;
            lastPageFlip = tNow;
        }
    } else {
        currentPage = 0;
    }

    int actualRows = 0;
    int startIdx = currentPage * contentRows * 2;
    int limit = needPaging ? contentRows : static_cast<int>(displayTemps.size());
    for (int p = 0; p < limit; p++) {
        int leftIdx = startIdx + p * 2;
        if (leftIdx >= static_cast<int>(displayTemps.size())) break;

        auto& [nameL, tempL] = displayTemps[leftIdx];
        auto labelL = TrimRight(nameL, halfW - 10);
        const int tcL = HighIsWorsePair(tempL, kTempWarn, kTempCrit);
        if (tcL >= 0) wattron(win, COLOR_PAIR(tcL));
        // Labels pad into a fixed field so every temperature value in the
        // column shares one left edge (#6); sensor names are ASCII.
        mvwprintw(win, y + lines, x0 + 2, "%-*s %.1f C", halfW - 10, labelL.c_str(), tempL);
        if (tcL >= 0) wattroff(win, COLOR_PAIR(tcL));

        int rightIdx = leftIdx + 1;
        if (rightIdx < static_cast<int>(displayTemps.size())) {
            auto& [nameR, tempR] = displayTemps[rightIdx];
            auto labelR = TrimRight(nameR, halfW - 10);
            const int tcR = HighIsWorsePair(tempR, kTempWarn, kTempCrit);
            if (tcR >= 0) wattron(win, COLOR_PAIR(tcR));
            mvwprintw(win, y + lines, x0 + 2 + halfW, "%-*s %.1f C", halfW - 10, labelR.c_str(), tempR);
            if (tcR >= 0) wattroff(win, COLOR_PAIR(tcR));
        }
        lines++;
        actualRows = p + 1;
    }

    if (needPaging) {
        // Fill remaining content rows
        while (lines < 1 + contentRows)
            lines++;
        // Page indicator
        mvwprintw(win, y + lines++, x0 + 2, "%.*s", maxW - 2,
                  ("[" + std::to_string(currentPage + 1) + "/" + std::to_string(totalPages) + "]").c_str());
        return 1 + contentRows + 1; // header + content rows + 1 page row
    } else {
        return 1 + actualRows; // header + actual sensor rows only
    }
}

int TuiApp::DrawPowerPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW) {
    if (maxW < 10) return 0;
    bool hasPower = (data.cpuPower > 0 || data.gpuPower > 0 || data.anePower > 0);
    bool hasBattery = (data.batteryCycleCount > 0 || data.batteryHealthPercent > 0);
    if (!hasPower && !hasBattery && data.thermalState == 0) return 0;

    DrawPanelTitle(win, y, x0, maxW, "Power");
    int lines = 1;

    // Thermal state — value carries the severity pair.
    if (data.thermalState > 0) {
        static const char* labels[] = {"", "Fairly Serious", "Critical"};
        const char* label = (data.thermalState < 3) ? labels[data.thermalState] : "Unknown";
        const int pair = (data.thermalState >= 2) ? 4 : 3;
        DrawLabeledValue(win, y + lines, x0, maxW, "Thermal:", label, pair);
        lines++;
    }

    // Power consumption — always render all rows; 0 means no data (yet),
    // hiding rows made the panel look broken when sampling is unavailable.
    {
        char val[32];
        double totalPower = (data.cpuPower + data.gpuPower + data.anePower) / 1000.0;
        snprintf(val, sizeof(val), "%.2f W", data.cpuPower / 1000.0);
        DrawLabeledValue(win, y + lines, x0, maxW, "CPU:", val);
        lines++;
        snprintf(val, sizeof(val), "%.2f W", data.gpuPower / 1000.0);
        DrawLabeledValue(win, y + lines, x0, maxW, "GPU:", val);
        lines++;
        snprintf(val, sizeof(val), "%.2f W", data.anePower / 1000.0);
        DrawLabeledValue(win, y + lines, x0, maxW, "ANE:", val);
        lines++;
        snprintf(val, sizeof(val), "%.2f W", totalPower);
        DrawLabeledValue(win, y + lines, x0, maxW, "Total:", val);
        lines++;
    }

    // Battery health
    if (hasBattery) {
        if (lines > 1) lines++;  // blank line separator
        DrawLabeledValue(win, y + lines, x0, maxW, "Cycles:",
                         std::to_string(data.batteryCycleCount));
        lines++;
        // Health % (low is worse): normal health no longer painted green
        // — only a drop below the warning band gets a color.
        int hp = (int)(data.batteryHealthPercent + 0.5);
        const int hpColor = LowIsWorsePair(hp, kHealthWarn, kHealthCrit);
        DrawLabeledValue(win, y + lines, x0, maxW, "Health:",
                         std::to_string(hp) + "%", hpColor);
        lines++;
        // Charge/discharge power
        if (data.batteryAmperage != 0 && data.batteryVoltage > 0) {
            int64_t powerMw = (int64_t)std::abs(data.batteryAmperage) * (int64_t)data.batteryVoltage / 1000;
            if (powerMw > 0) {
                const char* dir = (data.batteryAmperage > 0) ? "Chg" : "Dchg";
                char val[48];
                snprintf(val, sizeof(val), "%s %.2f W", dir, powerMw / 1000.0);
                DrawLabeledValue(win, y + lines, x0, maxW, "Power:", val);
                lines++;
            }
        }
        // Charger rated wattage
        if (data.chargerWatts > 0) {
            char val[32];
            snprintf(val, sizeof(val), "%.0f W", data.chargerWatts);
            DrawLabeledValue(win, y + lines, x0, maxW, "Charger:", val);
            lines++;
        }
        // Battery temp — shown in Temperature panel (from TemperatureWrapper/iokit_battery_temp)
    }

    return lines;
}

int TuiApp::DrawAccelPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW) {
    if (maxW < 10) return 0;
    bool hasSensor = data.alsValid || data.accel.hasDevice || data.gyro.valid ||
                     data.lidAngle.valid ||
                     data.motionHb.valid || data.deviceMotion.valid;
    if (!hasSensor) return 0;

    DrawPanelTitle(win, y, x0, maxW, "Sensors");
    int lines = 1;

    // ALS (ambient light sensor) — lux + raw RGBC channels
    if (data.alsValid) {
        if (data.alsChannels.valid) {
            uint32_t alsMax = std::max({data.alsChannels.r, data.alsChannels.g, data.alsChannels.b});
            if (alsMax == 0) alsMax = 1;
            uint8_t alsR = (uint8_t)(data.alsChannels.r * 255 / alsMax);
            uint8_t alsG = (uint8_t)(data.alsChannels.g * 255 / alsMax);
            uint8_t alsB = (uint8_t)(data.alsChannels.b * 255 / alsMax);
            mvwprintw(win, y + lines, x0 + 2, "ALS:     %.0f lux  R:%-3u G:%-3u B:%-3u",
                      data.alsLux, alsR, alsG, alsB);
            lines++;
        } else {
            mvwprintw(win, y + lines++, x0 + 2, "ALS:     %.0f lux", data.alsLux);
        }
    }

    // Accelerometer — gravity/orientation vector (0xFF00/3)
    if (data.accel.hasDevice && data.accel.valid) {
        mvwprintw(win, y + lines++, x0 + 2, "Gravity: %.2f %.2f %.2f g",
                  data.accel.x, data.accel.y, data.accel.z);
    } else if (data.accel.hasDevice) {
        mvwprintw(win, y + lines++, x0 + 2, "Gravity: waiting...");
    }

    // Gyroscope — angular velocity (0xFF00/9)
    if (data.gyro.valid) {
        mvwprintw(win, y + lines++, x0 + 2, "Gyro:    %.2f %.2f %.2f deg/s",
                  data.gyro.x, data.gyro.y, data.gyro.z);
    }

    // Lid angle (0x0020/138)
    if (data.lidAngle.valid) {
        mvwprintw(win, y + lines++, x0 + 2, "Lid:     %.1f%s", data.lidAngle.angle, degSuffixAngle_.c_str());
    }

    // Motion heartbeat (0xFF0C/1 — SPU fusion liveliness indicator)
    if (data.motionHb.valid) {
        mvwprintw(win, y + lines++, x0 + 2, "Heart:   cnt=%u type=0x%02x",
                  (unsigned)data.motionHb.counter, (unsigned)data.motionHb.eventFlag);
    }

    // DeviceMotion fusion (0xFF0C/5 — CMDeviceMotion, only when CoreMotion active)
    if (data.deviceMotion.valid) {
        mvwprintw(win, y + lines++, x0 + 2, "Fusion:  raw=%.0f", data.deviceMotion.raw);
    }

    return lines;
}

// ─── Per-Core Sensor Panel ───
int TuiApp::DrawCorePanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW) {
    if (maxW < 20 || data.perCoreCount == 0) return 0;

    DrawPanelTitle(win, y, x0, maxW, "Per-Core Sensors");
    int lines = 1;

    int count = std::min((int)data.perCoreCount, 16);
    // Compact header: Core#0  Core#1  Core#2 ...
    for (int i = 0; i < count; i++) {
        int offset = x0 + 2 + i * 8;
        if (offset + 7 > maxW) break;
        mvwprintw(win, y + lines, offset, "C%02d", i);
    }
    lines++;
    // Temperature row
    for (int i = 0; i < count; i++) {
        int offset = x0 + 2 + i * 8;
        if (offset + 7 > maxW) break;
        int t = static_cast<int>(data.perCoreTemp[i]);
        const int color = HighIsWorsePair(static_cast<double>(t), kTempWarn, kTempCrit);
        if (color >= 0) wattron(win, COLOR_PAIR(color));
        mvwprintw(win, y + lines, offset, "%4d%s", t, degSuffixTemp_.c_str());
        if (color >= 0) wattroff(win, COLOR_PAIR(color));
    }
    lines++;
    // Frequency row
    for (int i = 0; i < count; i++) {
        int offset = x0 + 2 + i * 8;
        if (offset + 7 > maxW) break;
        int f = static_cast<int>(data.perCoreFreq[i]);
        if (f > 0)
            mvwprintw(win, y + lines, offset, "%4dM", f);
        else
            mvwprintw(win, y + lines, offset, "    -");
    }
    lines++;
    return lines + 1;
}

// ─── Network Traffic Sparkline ───
// Unicode block characters ▁▂▃▄▅▆▇█ (U+2581..U+2588); sparkChars_ holds the
// ASCII fallback set when TCMT_ASCII is set (see Run).

int TuiApp::DrawNetGraphPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW) {
    if (maxW < 20 || data.dlHistoryLen < 2) return 0;

    int graphW = std::min(maxW - 14, data.dlHistoryLen);
    if (graphW < 4) return 0;

    DrawPanelTitle(win, y, x0, maxW, "Traffic");
    int lines = 1;

    // Find max for scaling
    uint64_t maxVal = 1;
    for (int i = 0; i < data.dlHistoryLen; i++) {
        int idx = (data.dlHistoryPos - 1 - i + TuiData::NET_HISTORY_MAX) % TuiData::NET_HISTORY_MAX;
        uint64_t v = data.dlHistory[idx];
        if (v > maxVal) maxVal = v;
        v = data.ulHistory[idx];
        if (v > maxVal) maxVal = v;
    }

    // "D:" + sparkline (blue)
    std::string dlSpark;
    for (int i = graphW - 1; i >= 0; i--) {
        int idx = (data.dlHistoryPos - 1 - i + TuiData::NET_HISTORY_MAX) % TuiData::NET_HISTORY_MAX;
        int level = (maxVal > 0) ? (int)(data.dlHistory[idx] * 7 / maxVal) : 0;
        if (level < 0) level = 0; if (level > 7) level = 7;
        dlSpark += sparkChars_[level];
    }
    wattron(win, COLOR_PAIR(6));
    mvwprintw(win, y + lines, x0 + 2, "D:%s", dlSpark.c_str());
    wattroff(win, COLOR_PAIR(6));
    lines++;

    // "U:" + sparkline (green) using pair 2
    std::string ulSpark;
    for (int i = graphW - 1; i >= 0; i--) {
        int idx = (data.dlHistoryPos - 1 - i + TuiData::NET_HISTORY_MAX) % TuiData::NET_HISTORY_MAX;
        int level = (maxVal > 0) ? (int)(data.ulHistory[idx] * 7 / maxVal) : 0;
        if (level < 0) level = 0; if (level > 7) level = 7;
        ulSpark += sparkChars_[level];
    }
    wattron(win, COLOR_PAIR(2));
    mvwprintw(win, y + lines, x0 + 2, "U:%s", ulSpark.c_str());
    wattroff(win, COLOR_PAIR(2));
    lines++;

    return lines + 1;
}

int TuiApp::DrawProcessPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW) {
    if (maxW < 15 || data.topProcesses.empty()) return 0;

    // Focused strip (bold) while a process selection exists — the panel is
    // then the active surface; reverse keeps it theme- and NO_COLOR-proof.
    DrawPanelTitle(win, y, x0, maxW, "Top Processes", selPid_ >= 0);
    int lines = 1;

    int nameW = maxW - 30;  // room for pid(7) + mem(5) + cpu(7) + spaces
    if (nameW < 6) nameW = 6;

    for (const auto& p : data.topProcesses) {
        // Pad/truncate name to exact display width (CJK-safe)
        std::string name = p.name;
        int dw = utf8_display_width(name);
        if (dw > nameW) {
            name = utf8_truncate(name, nameW);
        } else if (dw < nameW) {
            name.append(nameW - dw, ' ');
        }

        // Format PID
        std::string pidStr = std::to_string(p.pid);

        // Format memory
        std::string memStr;
        if (p.memoryBytes >= (uint64_t)1024 * 1024 * 1024)
            memStr = std::to_string(p.memoryBytes / (1024 * 1024 * 1024)) + "G";
        else
            memStr = std::to_string(p.memoryBytes / (1024 * 1024)) + "M";

        // Name / pid / memory stay in the default foreground; only the CPU
        // cell carries the severity color — coloring the whole row turns
        // every row into a colored field and the signal disappears.
        const bool selected = (p.pid == selPid_);
        if (selected) wattron(win, A_REVERSE);

        char headBuf[160];
        snprintf(headBuf, sizeof(headBuf), "%s %6s %4s ",
                 name.c_str(), pidStr.c_str(), memStr.c_str());
        mvwprintw(win, y + lines, x0 + 2, "%s", headBuf);

        // Column of "%5.1f%%": name (padded to nameW) + " %6s %4s " above.
        // Only out-of-range CPU% gets a color (severity table, #5); the
        // selected row flips to plain reverse (no per-cell color) so the
        // highlight stays unambiguous.
        const int cpuColor = HighIsWorsePair(p.cpuPercent, kProcCpuWarn, kProcCpuCrit);
        const int cpuCol = x0 + 2 + nameW + 13;
        if (cpuColor >= 0 && !selected) wattron(win, COLOR_PAIR(cpuColor));
        mvwprintw(win, y + lines, cpuCol, "%5.1f%%", p.cpuPercent);
        if (cpuColor >= 0 && !selected) wattroff(win, COLOR_PAIR(cpuColor));
        if (selected) wattroff(win, A_REVERSE);
        lines++;
    }

    return lines + 1;  // +1 bottom padding
}

// ────────────────────────────────────────────────────────────────────────────
// Settings page — framed interactive form for the server push settings.
// Up/Down (or Tab) moves focus; Space toggles ON/OFF fields; typing edits
// the URL field (Left/Right/Home/End/Backspace); Enter saves + applies via
// the settings handler; Esc cancels without applying.
void TuiApp::RenderSettingsPage(int rows, int cols, int ch) {
    const int FIELD_COUNT = 4;
    auto insertUrlChar = [&](char c) {
        draftSettings_.url.insert(draftSettings_.url.begin() + urlCursor_, c);
        urlCursor_ += 1;
    };
    auto deleteUrlChar = [&]() {
        if (urlCursor_ > 0) {
            draftSettings_.url.erase(draftSettings_.url.begin() + urlCursor_ - 1);
            urlCursor_ -= 1;
        }
    };

    // ---- key handling ----
    switch (ch) {
        case 27: // Esc — cancel without saving
            settingsPage_ = false;
            curs_set(0);
            clear();
            return;
        case '\t':
        case KEY_DOWN:
            settingsFocus_ = (settingsFocus_ + 1) % FIELD_COUNT;
            urlCursor_ = (int)draftSettings_.url.size();
            break;
        case KEY_UP:
            settingsFocus_ = (settingsFocus_ + FIELD_COUNT - 1) % FIELD_COUNT;
            urlCursor_ = (int)draftSettings_.url.size();
            break;
        case ' ':
            if (settingsFocus_ == 0) draftSettings_.enabled = !draftSettings_.enabled;
            else if (settingsFocus_ == 2) draftSettings_.insecure = !draftSettings_.insecure;
            else if (settingsFocus_ == 1) insertUrlChar(' ');
            break;
        case '\n':
        case '\r':
#ifdef KEY_ENTER
        case KEY_ENTER:
#endif
            // Hard bounds for the upload interval (1..60 s).
            if (draftSettings_.intervalSec < 1) draftSettings_.intervalSec = 1;
            if (draftSettings_.intervalSec > 60) draftSettings_.intervalSec = 60;
            if (settingsHandler_) settingsHandler_(draftSettings_);
            {
                std::lock_guard<std::mutex> lock(dataMutex_);
                serverSettings_ = draftSettings_;
            }
            settingsPage_ = false;
            curs_set(0);
            clear();
            return;
        default:
            if (settingsFocus_ == 1 && ch >= 32 && ch < 127) {
                insertUrlChar((char)ch);
            } else if (settingsFocus_ == 1 && (ch == KEY_BACKSPACE || ch == 127 || ch == 8)) {
                deleteUrlChar();
            } else if (settingsFocus_ == 1 && ch == KEY_LEFT && urlCursor_ > 0) {
                urlCursor_ -= 1;
            } else if (settingsFocus_ == 1 && ch == KEY_RIGHT &&
                       urlCursor_ < (int)draftSettings_.url.size()) {
                urlCursor_ += 1;
            } else if (settingsFocus_ == 1 && ch == KEY_HOME) {
                urlCursor_ = 0;
            } else if (settingsFocus_ == 1 && ch == KEY_END) {
                urlCursor_ = (int)draftSettings_.url.size();
            } else if (settingsFocus_ == 3 && ch >= '0' && ch <= '9') {
                const int next = draftSettings_.intervalSec * 10 + (ch - '0');
                draftSettings_.intervalSec = (next > 60) ? 60 : next;
            } else if (settingsFocus_ == 3 && (ch == KEY_BACKSPACE || ch == 127 || ch == 8)) {
                draftSettings_.intervalSec /= 10;
            }
            break;
    }

    // ---- draw framed dialog ----
    const int W = std::min(72, cols - 8);
    const int H = 15;
    const int x0 = std::max(1, (cols - W) / 2);
    const int y0 = std::max(1, (rows - H) / 2);

    erase();
    // Outer frame (ASCII-safe for both curses and PDCurses)
    std::string hlineStr(W, '-');
    mvwprintw(stdscr, y0, x0, "%s", ("+" + hlineStr + "+").c_str());
    for (int r = y0 + 1; r < y0 + H - 1; r++) {
        mvwprintw(stdscr, r, x0, "|");
        mvwprintw(stdscr, r, x0 + W - 1, "|");
    }
    mvwprintw(stdscr, y0 + H - 1, x0, "%s", ("+" + hlineStr + "+").c_str());
    mvwprintw(stdscr, y0, x0 + 3, " Server Upload Settings ");

    // Field rows: [n] label : [ value ] — focused row renders reversed.
    auto drawField = [&](int idx, int row, const std::string& label, const std::string& value) {
        const int lx = x0 + 2;
        const bool focused = (settingsFocus_ == idx);
        char buf[160];
        snprintf(buf, sizeof(buf), "[%d] %-16s : [ %s ]", idx + 1, label.c_str(), value.c_str());
        if (focused) attron(A_REVERSE);
        mvprintw(row, lx, "%s", buf);
        if (focused) attroff(A_REVERSE);
    };

    const bool on = draftSettings_.enabled;
    drawField(0, y0 + 2, "Data upload", on ? " ON " : " OFF ");

    // URL field: fixed-width window that follows the cursor.
    const int URL_BOX = 40;
    std::string shown = draftSettings_.url;
    int winStart = 0;
    if ((int)shown.size() > URL_BOX) {
        winStart = std::max(0, std::min(urlCursor_ - URL_BOX / 2,
                                       (int)shown.size() - URL_BOX));
    }
    std::string window = shown.substr(winStart, URL_BOX);
    window.resize(URL_BOX, ' ');
    drawField(1, y0 + 3, "Server URL", window);

    drawField(2, y0 + 4, "Skip TLS verify", draftSettings_.insecure ? " ON " : " OFF ");
    drawField(3, y0 + 5, "Upload interval",
              std::to_string(draftSettings_.intervalSec) + " s  (bounds 1-60)");

    // Status + hints
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        // Prefer the live status from the monitor loop (data_ is refreshed
        // every frame); fall back to the last-saved settings value.
        const std::string st = !data_.serverStatus.empty() ? data_.serverStatus
            : (serverSettings_.status.empty()
                ? (serverSettings_.enabled ? "starting..." : "disabled")
                : serverSettings_.status);
        mvwprintw(stdscr, y0 + 7, x0 + 2, " Status : %s", st.c_str());
    }
    mvwprintw(stdscr, y0 + 9, x0 + 2, " Enter save+apply   Esc cancel   Up/Down or Tab focus");
    mvwprintw(stdscr, y0 + 10, x0 + 2, " Space toggles   type edits URL   digits edit interval");
    mvwprintw(stdscr, y0 + H - 2, x0 + 2, " Settings persist to system_monitor.json (server.*)");

    // Place the cursor inside the URL box when it is focused. Never move the
    // cursor to the screen corners — ncurses treats the bottom-right cell as
    // a wrap trigger and can loop forever trying to reposition (classic
    // freeze: wrefresh -> TransformLine -> _nc_mvcur_sp).
    if (settingsFocus_ == 1) {
        const int labelW = 3 + 2 + 16 + 3;   // "[1] " + label + " : "
        const int valuePad = 2;              // "[ "
        const int cursorInBox = std::max(0, std::min(urlCursor_ - winStart, URL_BOX - 1));
        move(y0 + 3, x0 + 2 + labelW + valuePad + cursorInBox);
    }
}

// Log page — full-screen scrolling log view inside the main TUI. Compiled on
// every platform but only reachable where caps_.inlineLogPage is set: Windows
// uses the standalone Win32 LogWindow instead (dashboard-only console), and
// the page stays dormant there. Data comes from the in-process Logger log
// buffer (no IPC, no files).
// ────────────────────────────────────────────────────────────────────────────
void TuiApp::RenderLogPage(int rows, int cols, int ch) {
    if (ch == KEY_UP) { logScrollOffset_++; logFollow_ = false; }
    else if (ch == KEY_DOWN) {
        if (logScrollOffset_ > 0) logScrollOffset_--;
        else logFollow_ = true;
    }
    else if (ch == KEY_PPAGE) { logScrollOffset_ += 10; logFollow_ = false; }
    else if (ch == KEY_NPAGE) {
        logScrollOffset_ = (std::max)(0, logScrollOffset_ - 10);
        if (logScrollOffset_ == 0) logFollow_ = true;
    }
    // Home = oldest entry (top of the buffer); End = newest (follow).
    // Home is deferred until the buffer size is known below.
    else if (ch == KEY_HOME) { logFollow_ = false; logHomePending_ = true; }
    else if (ch == KEY_END)  { logFollow_ = true; logScrollOffset_ = 0; }
    else if (ch == 'f' || ch == 'F') {
        logFollow_ = !logFollow_;
        if (logFollow_) logScrollOffset_ = 0;
    }

    std::vector<std::string> lines;
    if (logBuf_)
        lines = logBuf_->GetRecent(LogBuffer::MAX_LINES);

    erase();

    std::string topBot(cols, '-');
    mvwprintw(stdscr, 0, 0, "%s", topBot.c_str());
    mvwprintw(stdscr, rows - 1, 0, "%s", topBot.c_str());

    std::string header = " TCMT Log    lines: " + std::to_string(lines.size()) +
                         "    " + (logFollow_ ? "[FOLLOW]" : "[SCROLL]") +
                         "    " + upArrow_ + "/" + downArrow_ + " scroll  Home/End  f=follow  ?=help";
    // (Esc/l back to dashboard and q=quit are listed in the ? help page;
    // they are deliberately not repeated on every log line of the header.)
    wattron(stdscr, COLOR_PAIR(5) | A_BOLD);
    mvwprintw(stdscr, 1, 1, "%.*s", cols - 2, header.c_str());
    wattroff(stdscr, COLOR_PAIR(5) | A_BOLD);

    int contentRows = rows - 3;
    int total = static_cast<int>(lines.size());

    // HOME parks at the oldest line once the buffer size is known.
    if (logHomePending_) {
        logHomePending_ = false;
        logScrollOffset_ = (std::max)(0, total - contentRows);
    }

    int start = 0;
    if (logFollow_) {
        start = (std::max)(0, total - contentRows);
    } else {
        start = (std::max)(0, total - logScrollOffset_ - contentRows);
    }

    if (total == 0) {
        mvwprintw(stdscr, 2, 2, "No log entries yet (l/Esc to dashboard)");
    }

    for (int r = 0; r < contentRows; ++r) {
        int idx = start + r;
        if (idx >= total) break;
        const std::string& entry = lines[idx];

        // Severity comes from the level tag parsed at its fixed position
        // ("[ts][LEVEL] msg"), never from substring search over the whole
        // line: CRITICAL/FATAL must read as errors, INFO stays in the
        // default foreground, and message text quoting a level word cannot
        // recolor an entry.
        std::string lvl;
        size_t c1 = entry.find(']');
        size_t o2 = (c1 != std::string::npos) ? entry.find('[', c1 + 1) : std::string::npos;
        size_t c2 = (o2 != std::string::npos) ? entry.find(']', o2 + 1) : std::string::npos;
        if (c2 != std::string::npos) lvl = entry.substr(o2 + 1, c2 - o2 - 1);

        int color = -1;
        if (lvl == "ERROR" || lvl == "CRITICAL" || lvl == "FATAL") color = 4;
        else if (lvl == "WARN" || lvl == "WARNING") color = 3;
        else if (lvl == "DEBUG" || lvl == "TRACE") color = 6;

        std::string disp = utf8_truncate(entry, cols - 3);
        if (color >= 0) wattron(stdscr, COLOR_PAIR(color));
        mvwprintw(stdscr, 2 + r, 1, "%.*s", cols - 2, disp.c_str());
        if (color >= 0) wattroff(stdscr, COLOR_PAIR(color));
    }
}

// Help page — full-screen key reference (press ?). Every key listed here is
// gated exactly like the header hints (caps_ + wired handlers), so the page
// never advertises a dead key. It is a modal like Settings: Esc, q or ?
// close it — q inside help does NOT quit the app.
void TuiApp::RenderHelpPage(int rows, int cols, int ch) {
    if (ch == 27 || ch == 'q' || ch == 'Q' || ch == '?') {
        helpPage_ = false;
        clear();
        return;
    }

    const int W = std::min(74, cols - 6);
    const int H = rows - 4;
    const int x0 = std::max(1, (cols - W) / 2);
    const int y0 = 2;

    erase();
    std::string hlineStr(W, '-');  // NB: not 'hline' — that is a curses function
    mvwprintw(stdscr, y0, x0, "%s", ("+" + hlineStr + "+").c_str());
    for (int r = y0 + 1; r < y0 + H - 1; r++) {
        mvwprintw(stdscr, r, x0, "|");
        mvwprintw(stdscr, r, x0 + W - 1, "|");
    }
    mvwprintw(stdscr, y0 + H - 1, x0, "%s", ("+" + hlineStr + "+").c_str());
    mvwprintw(stdscr, y0, x0 + 3, " Key Bindings ");

    int y = y0 + 1;
    auto group = [&](const std::string& heading,
                     const std::vector<std::pair<std::string, std::string>>& items) {
        if (y >= y0 + H - 2) return;
        wattron(stdscr, COLOR_PAIR(5) | A_BOLD);
        mvwprintw(stdscr, y++, x0 + 2, "%.*s", W - 4, heading.c_str());
        wattroff(stdscr, COLOR_PAIR(5) | A_BOLD);
        for (const auto& item : items) {
            if (y >= y0 + H - 2) return;
            char line[160];
            snprintf(line, sizeof(line), "%-17s %s", item.first.c_str(), item.second.c_str());
            mvwprintw(stdscr, y++, x0 + 2, "%.*s", W - 4, line);
        }
    };

    // Same gating as the header hints: only list what this build supports.
    std::vector<std::pair<std::string, std::string>> common, dash, logpage;
    common.push_back({"S", "server push settings"});
    if (caps_.inlineLogPage)
        common.push_back({"L / Tab", "switch to log page"});
    if (updateRequestHandler_)
        common.push_back({"U", "check for update"});
    if (locationRequestHandler_)
        common.push_back({"R", "request Location Services (WiFi SSID)"});
    common.push_back({"Esc", "go back one level"});
    common.push_back({"?", "close this help"});
    common.push_back({"q", "quit TCMT Monitor"});

    dash.push_back({upArrow_ + " / " + downArrow_, "select a process row"});
    dash.push_back({"Enter", "process details (PID, CPU, memory)"});
    dash.push_back({"c", "connections list (expand the status chip)"});

    if (caps_.inlineLogPage) {
        logpage.push_back({upArrow_ + " / " + downArrow_, "scroll one line"});
        logpage.push_back({"PgUp / PgDn", "scroll one page"});
        logpage.push_back({"Home", "oldest entry"});
        logpage.push_back({"End", "newest entry"});
        logpage.push_back({"f", "follow newest lines"});
        logpage.push_back({"Esc / L / Tab", "back to dashboard"});
    }

    group("Common", common);
    group("Dashboard", dash);
    if (!logpage.empty()) group("Log page", logpage);

    // Settings keys are hinted inline on that page; one summary line here.
    if (y < y0 + H - 2) {
        mvwprintw(stdscr, y++, x0 + 2, "%.*s", W - 4,
                  "Settings page: Enter save, Esc cancel, Tab or arrows move focus, Space toggles");
    }
}

// Process-details overlay — Enter on a selected dashboard row. Shows what
// the monitor loop already collects for that pid (same source as the row);
// Esc / Enter / q close it, q does not quit the app.
void TuiApp::RenderProcessDetails(int rows, int cols, int ch) {
    const bool isEnter = (ch == '\n' || ch == '\r'
#ifdef KEY_ENTER
        || ch == KEY_ENTER
#endif
    );
    if (ch == 27 || ch == 'q' || ch == 'Q' || isEnter) {
        detailsPage_ = false;
        clear();
        return;
    }

    // Re-resolve the pid every frame against the freshest snapshot; if the
    // process is gone the overlay dismisses itself.
    TuiData data;
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        data = data_;
    }
    const TuiData::ProcessTopEntry* proc = nullptr;
    for (const auto& p : data.topProcesses) {
        if (p.pid == selPid_) { proc = &p; break; }
    }
    if (proc == nullptr) {
        detailsPage_ = false;
        clear();
        return;
    }

    const int W = std::min(56, cols - 8);
    const int H = 11;
    const int x0 = std::max(1, (cols - W) / 2);
    const int y0 = std::max(1, (rows - H) / 2);

    erase();
    std::string hlineStr(W, '-');
    mvwprintw(stdscr, y0, x0, "%s", ("+" + hlineStr + "+").c_str());
    for (int r = y0 + 1; r < y0 + H - 1; r++) {
        mvwprintw(stdscr, r, x0, "|");
        mvwprintw(stdscr, r, x0 + W - 1, "|");
    }
    mvwprintw(stdscr, y0 + H - 1, x0, "%s", ("+" + hlineStr + "+").c_str());
    mvwprintw(stdscr, y0, x0 + 3, " Process Details ");

    std::string memStr;
    if (proc->memoryBytes >= (uint64_t)1024 * 1024 * 1024)
        memStr = std::to_string(proc->memoryBytes / (1024 * 1024 * 1024)) + " GB";
    else
        memStr = std::to_string(proc->memoryBytes / (1024 * 1024)) + " MB";

    char line[192];
    int y = y0 + 2;
    snprintf(line, sizeof(line), "Name:   %s", proc->name.c_str());
    mvwprintw(stdscr, y++, x0 + 2, "%.*s", W - 4, line);
    snprintf(line, sizeof(line), "PID:    %d", (int)proc->pid);
    mvwprintw(stdscr, y++, x0 + 2, "%.*s", W - 4, line);

    const int cpuColor = HighIsWorsePair(proc->cpuPercent, kProcCpuWarn, kProcCpuCrit);
    snprintf(line, sizeof(line), "CPU:    %5.1f%%", proc->cpuPercent);
    if (cpuColor >= 0) wattron(stdscr, COLOR_PAIR(cpuColor));
    mvwprintw(stdscr, y++, x0 + 2, "%.*s", W - 4, line);
    if (cpuColor >= 0) wattroff(stdscr, COLOR_PAIR(cpuColor));

    snprintf(line, sizeof(line), "Memory: %s", memStr.c_str());
    mvwprintw(stdscr, y++, x0 + 2, "%.*s", W - 4, line);

    mvwprintw(stdscr, y0 + H - 2, x0 + 2, " Enter/Esc/q close ");
}

// Connections list overlay — 'c' on the dashboard. The compact bottom chip
// shows the aggregate; this expands into one row per live client. Client
// rows come from the per-connection type vector the monitor loop fills;
// per-client connect times are not collected yet (see docs/session.md).
void TuiApp::RenderConnectionsList(int rows, int cols, int ch) {
    const bool isEnter = (ch == '\n' || ch == '\r'
#ifdef KEY_ENTER
        || ch == KEY_ENTER
#endif
    );
    if (ch == 27 || ch == 'q' || ch == 'Q' || isEnter) {
        connListPage_ = false;
        clear();
        return;
    }

    TuiData data;
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        data = data_;
    }

    const int W = std::min(64, cols - 8);
    const int H = 16;
    const int x0 = std::max(1, (cols - W) / 2);
    const int y0 = std::max(1, (rows - H) / 2);

    erase();
    std::string hlineStr(W, '-');
    mvwprintw(stdscr, y0, x0, "%s", ("+" + hlineStr + "+").c_str());
    for (int r = y0 + 1; r < y0 + H - 1; r++) {
        mvwprintw(stdscr, r, x0, "|");
        mvwprintw(stdscr, r, x0 + W - 1, "|");
    }
    mvwprintw(stdscr, y0 + H - 1, x0, "%s", ("+" + hlineStr + "+").c_str());
    mvwprintw(stdscr, y0, x0 + 3, " Connections ");

    char line[192];
    int y = y0 + 2;
    if (data.connectionCount <= 0) {
        mvwprintw(stdscr, y++, x0 + 2, "  no clients connected");
    } else {
        snprintf(line, sizeof(line), "  %d client%s",
                 data.connectionCount, data.connectionCount == 1 ? "" : "s");
        mvwprintw(stdscr, y++, x0 + 2, "%.*s", W - 4, line);
        int n = 1;
        for (uint8_t t : data.clientTypes) {
            const char* name = (t == 1) ? "Avalonia" : (t == 2) ? "MCP" : "unknown";
            snprintf(line, sizeof(line), "  #%-3d %s", n++, name);
            mvwprintw(stdscr, y++, x0 + 2, "%.*s", W - 4, line);
            if (y >= y0 + H - 4) break;   // keep room for the push line
        }
        if (data.httpClientCount > 0) {
            snprintf(line, sizeof(line), "  #%-3d Web (local HTTP) x%d", n, data.httpClientCount);
            mvwprintw(stdscr, y++, x0 + 2, "%.*s", W - 4, line);
        }
        if (!data.connectionSince.empty()) {
            snprintf(line, sizeof(line), "  Since: %s", data.connectionSince.c_str());
            mvwprintw(stdscr, y++, x0 + 2, "%.*s", W - 4, line);
        }
        if (y < y0 + H - 3) y++;
    }

    // Server push line — same severity coloring as the bottom status row.
    std::string pushStr;
    int color = -1;
    if (!data.serverPushEnabled) {
        pushStr = "Push: disabled (press S to configure)";
    } else {
        std::string age = "never";
        if (data.lastPushMs > 0) {
            int64_t sec = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()
                - data.lastPushMs / 1000;
            age = std::to_string((std::max)(int64_t(0), sec)) + "s ago";
        }
        const bool err = data.serverStatus.rfind("error", 0) == 0;
        const bool connecting = data.serverStatus.rfind("connecting", 0) == 0;
        color = err ? 4 : (connecting ? 3 : 2);
        pushStr = "Push: " + data.serverStatus + " (last " + age + ")";
    }
    if (color >= 0) wattron(stdscr, COLOR_PAIR(color));
    mvwprintw(stdscr, y, x0 + 2, "%.*s", W - 4, pushStr.c_str());
    if (color >= 0) wattroff(stdscr, COLOR_PAIR(color));

    mvwprintw(stdscr, y0 + H - 2, x0 + 2, " Enter/Esc/q close ");
}

void TuiApp::Run() {
    setlocale(LC_ALL, "");

    // Downgrade switches (#11), read once at startup:
    //   NO_COLOR      — any non-empty value disables color output.
    //   TCMT_ASCII=1  — ASCII glyphs (sparkline levels, degree signs). Also
    //                   forced when the active locale is not UTF-8, where
    //                   multibyte glyphs cannot be trusted to render.
    const char* noColorEnv = std::getenv("NO_COLOR");
    noColor_ = (noColorEnv != nullptr) && (*noColorEnv != '\0');
    const char* asciiEnv = std::getenv("TCMT_ASCII");
    const char* curLocale = std::setlocale(LC_ALL, nullptr);
    const bool utf8Locale = curLocale &&
        (std::strstr(curLocale, "UTF-8") != nullptr || std::strstr(curLocale, "utf-8") != nullptr);
    asciiMode_ = (asciiEnv != nullptr && std::strcmp(asciiEnv, "0") != 0) || !utf8Locale;
    if (asciiMode_) {
        sparkChars_ = " .:-=+*#";    // 8 ink-ascending levels, index 0 = empty
        degSuffixTemp_ = "C";
        degSuffixAngle_ = "deg";
        upArrow_ = "^";
        downArrow_ = "v";
        segShade_ = "-";
        barFill_ = "=";
        barTrack_ = "-";
    }

    initscr();
    cursesActive_ = true;
#ifndef __PDCURSES__
    ESCDELAY = 25;   // 25ms Esc timeout (default 1000ms) — fix "half-exit" feel
#endif
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    nodelay(stdscr, TRUE);

    InitColors();

    getmaxyx(stdscr, termRows_, termCols_);

    while (running_.load()) {
        int rows = termRows_, cols = termCols_;

        // Detect terminal resize via PDCurses
#ifdef __PDCURSES__
        if (is_termresized()) {
            resize_term(0, 0);
            getmaxyx(stdscr, rows, cols);
            termRows_ = rows;
            termCols_ = cols;
            clear();
        }
#else
        getmaxyx(stdscr, rows, cols);
#endif

        int ch = getch();

        // Settings page is modal: Esc cancels, Enter saves — never quits.
        // 30 ms ≈ 33 fps is plenty for a form; the old 10 ms burned CPU
        // repainting the same dialog.
        if (settingsPage_) {
            RenderSettingsPage(rows, cols, ch);
            refresh();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }

        // Help page is a modal overlay like Settings. Checked before the
        // global quit handler so q inside help closes the page, not the app.
        if (helpPage_) {
            RenderHelpPage(rows, cols, ch);
            refresh();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }

        // Process-details overlay (Enter on a selected dashboard row). Modal
        // like the others: Esc / Enter / q close it — q here closes the
        // overlay, not the app.
        if (detailsPage_) {
            RenderProcessDetails(rows, cols, ch);
            refresh();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }

        // Connections list overlay ('c') — the compact bottom chip expands
        // into one row per live client. Same modal semantics.
        if (connListPage_) {
            RenderConnectionsList(rows, cols, ch);
            refresh();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }

        // Esc is "back": from the log page it returns to the dashboard; on
        // the dashboard it is inert. q is the only quit key.
        // Ctrl+C may arrive as a key (0x03, or KEY_BREAK on PDCurses) instead
        // of firing CTRL_C_EVENT — treat it as quit so the TUI exits cleanly.
        if (ch == 'q' || ch == 'Q' || ch == 3
#ifdef KEY_BREAK
            || ch == KEY_BREAK
#endif
        ) {
            running_ = false;
            break;
        }
        if (ch == 's' || ch == 'S') {
            // Open the settings page with a fresh draft of the current values.
            {
                std::lock_guard<std::mutex> lock(dataMutex_);
                draftSettings_ = serverSettings_;
            }
            settingsFocus_ = 0;
            urlCursor_ = (int)draftSettings_.url.size();
            settingsPage_ = true;
            logPage_ = false;   // settings overlays whatever page was shown
            curs_set(1);
            clear();
        }
        if ((ch == 'r' || ch == 'R') && locationRequestHandler_) {
            locationRequestHandler_();
        }
        if ((ch == 'u' || ch == 'U') && updateRequestHandler_) {
            updateRequestHandler_();
        }
        // The in-TUI log page is a capability: on Windows the dashboard
        // pairs with the standalone Win32 log window, so L/Tab stay inert.
        if (caps_.inlineLogPage && (ch == 'l' || ch == 'L' || ch == '\t')) {
            logPage_ = !logPage_;
            clear();
        }
        if (ch == 27 && caps_.inlineLogPage && logPage_) {  // Esc = back to dashboard (not quit)
            logPage_ = false;
            clear();
        }
        if (ch == '?') {   // key reference — reachable from dashboard and log page
            helpPage_ = true;
            clear();
        }
        if (ch == 'c' || ch == 'C') {   // expand the Connections chip into a client list
            connListPage_ = true;
            clear();
        }
        if (rows < 24 || cols < 80) {
            clear();
            mvprintw(0, 0, "Terminal too small. Current: %dx%d", cols, rows);
            refresh();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        // Log page — in-process full-screen log view (Tab / L to switch back).
        // logPage_ can only become true where caps_.inlineLogPage is set, so
        // this branch is naturally dormant on Windows (native log window).
        // Skip the repaint when nothing changed: no key, same buffer version
        // (LogBuffer::Version()) and same terminal size. New lines repaint
        // immediately because the version counter moves; an idle scrolled
        // view stops burning CPU on erase + full redraw every frame.
        if (logPage_) {
            const bool needRedraw = (ch != ERR) || rows != lastLogRows_ || cols != lastLogCols_ ||
                                    (logBuf_ && logBuf_->Version() != lastLogVer_);
            if (needRedraw) {
                RenderLogPage(rows, cols, ch);
                lastLogVer_ = logBuf_ ? logBuf_->Version() : 0;
                lastLogRows_ = rows;
                lastLogCols_ = cols;
                refresh();
            }
            for (int i = 0; i < 3 && running_.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            continue;
        }

        TuiData data;
        {
            std::lock_guard<std::mutex> lock(dataMutex_);
            data = data_;
        }

        // Dashboard-local keys need the current snapshot, so they are
        // handled here: ↑/↓ move the process selection (the first focusable
        // surface of the dashboard), Enter opens the details overlay. The
        // top list re-sorts every refresh, so the selection tracks the pid —
        // if the process leaves the list the highlight clears.
        int selIdx = -1;
        if (selPid_ >= 0) {
            for (size_t i = 0; i < data.topProcesses.size(); ++i) {
                if (data.topProcesses[i].pid == selPid_) { selIdx = static_cast<int>(i); break; }
            }
            if (selIdx < 0) selPid_ = -1;   // selected process left the top list
        }
        if (!data.topProcesses.empty()) {
            const int last = static_cast<int>(data.topProcesses.size()) - 1;
            int nextIdx = selIdx;
            if (ch == KEY_UP) {
                if (selIdx > 0) nextIdx = selIdx - 1;
                else if (selIdx < 0) nextIdx = last;   // arrows enter the list from the bottom
            } else if (ch == KEY_DOWN) {
                if (selIdx >= 0 && selIdx < last) nextIdx = selIdx + 1;
                else if (selIdx < 0) nextIdx = 0;
            }
            if (nextIdx != selIdx) {
                selPid_ = data.topProcesses[nextIdx].pid;
                selIdx = nextIdx;
            }
            const bool isEnter = (ch == '\n' || ch == '\r'
#ifdef KEY_ENTER
                || ch == KEY_ENTER
#endif
            );
            if (isEnter && selIdx >= 0) {
                detailsPage_ = true;
                clear();
            }
        }

        erase();

        int lx = 1;
        int rx = cols / 2 + 1;
        int divCol = cols / 2;
        int leftW = rx - lx - 1;
        int rightW = cols - rx - 1;

        // === Draw structural lines FIRST ===
        // Top border
        std::string topBot(cols, '-');
        mvwprintw(stdscr, 0, 0, "%s", topBot.c_str());
        mvwprintw(stdscr, rows - 1, 0, "%s", topBot.c_str());

        // Side borders
        for (int r = 1; r < rows - 1; r++) {
            mvwprintw(stdscr, r, 0, "|");
            mvwprintw(stdscr, r, cols - 1, "|");
        }
        mvwprintw(stdscr, 0, 0, "+");
        mvwprintw(stdscr, 0, cols - 1, "+");
        mvwprintw(stdscr, rows - 1, 0, "+");
        mvwprintw(stdscr, rows - 1, cols - 1, "+");

        // (No separator under the header: the title row + one blank row
        // separate header from content; a second full-width rule stacked
        // under the top border just doubled the chrome.)

        // Vertical divider — confined to the panel content area; the rows
        // below (Connections/System/bottom border) are reserved bands.
        // Divider runs alongside the content area, which now extends two
        // rows deeper (the bottom status rows are compact, tier-1 chrome).
        int maxContentRow = rows - 6;
        for (int r = 2; r <= maxContentRow; r++) {
            mvwprintw(stdscr, r, divCol, "|");
        }

        // === Header ===
        DrawHeader(stdscr, data);

        // Status row: three occupants share row 0/1 without ever colliding —
        // key hints sit right-aligned on the title row and fall back to the
        // right end of row 1 on narrow terminals; the stale chip anchors the
        // row-1 left edge; the update banner centers in whatever row-1 space
        // is left between them. Hints are built from caps_ + handlers so no
        // dead keys are advertised.
        std::string hint = "S:Settings";
        if (caps_.inlineLogPage) hint += " L:Log";
        if (updateRequestHandler_) hint += " U:Update";
        if (locationRequestHandler_) hint += " R:Location";
        hint += " ?:Help Q:Quit";
        const int hintW = static_cast<int>(hint.size());

        std::string title = "TCMT Monitor  " + data.timestamp;
        const int titleEnd = (cols - static_cast<int>(title.size())) / 2
                           + static_cast<int>(title.size());
        const bool hintRow1 = (cols - hintW - 1) < titleEnd + 2;
        wattron(stdscr, COLOR_PAIR(5));
        mvwprintw(stdscr, hintRow1 ? 1 : 0, cols - hintW - 1, "%s", hint.c_str());
        wattroff(stdscr, COLOR_PAIR(5));

        // Row-1 left reservation: the stale chip when the monitor loop is
        // silent (yellow = warning class); nothing otherwise.
        int row1Left = 1;   // first column still free after left occupants
        const int64_t lastUs = lastUpdateUs_.load();
        const int64_t staleUs = (lastUs == 0) ? 0
            : std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count() - lastUs;
        if (staleUs > kStaleAfterUs) {
            std::string chip = " stale " + std::to_string(staleUs / 1000000) + "s ";
            wattron(stdscr, COLOR_PAIR(3));
            mvwprintw(stdscr, 1, 1, "%.*s", cols - 3, chip.c_str());
            wattroff(stdscr, COLOR_PAIR(3));
            row1Left += static_cast<int>(chip.size());
        }

        // Update banner — centered in the free row-1 span (clipped on the
        // right so it never overwrites the row-1 key hints).
        if (!data.updateStatus.empty()) {
            const int color = data.updateState == 6 ? 4
                : (data.updateState == 5 ? 2 : 3);
            wattron(stdscr, COLOR_PAIR(color) | A_BOLD);
            std::string banner = " " + data.updateStatus + " ";
            const int rightReserve = hintRow1 ? hintW + 1 : 0;
            const int zoneL = row1Left + 1;
            const int zoneR = cols - 2 - rightReserve;
            if (zoneR >= zoneL) {
                const int zoneW = zoneR - zoneL + 1;
                int bx = zoneL + (zoneW - static_cast<int>(banner.size())) / 2;
                bx = (std::max)(zoneL, bx);
                mvwprintw(stdscr, 1, bx, "%.*s", zoneR - bx + 1, banner.c_str());
            }
            wattroff(stdscr, COLOR_PAIR(color) | A_BOLD);
        }

        // === Left panels (CPU + GPU + Memory) ===
        // Panels may start one row deeper than before: the bottom status
        // rows (rows-4..rows-2) are the only reserved band now, and content
        // reaching rows-5 is safe — anything below rows-4 gets wiped and
        // repainted by the status rows.
        int maxY = rows - 4;
        int ly = 2;
        if (ly < maxY) {
            int cpuLines = DrawCpuPanel(stdscr, data, ly, lx, leftW);
            ly += cpuLines + 1;
            if (ly < maxY) {
                ly += DrawGpuPanel(stdscr, data, ly, lx, leftW) + 1;
            }
            if (ly < maxY) {
                ly += DrawMemoryPanel(stdscr, data, ly, lx, leftW);
            }
            if (ly < maxY) {
                ly += 2;  // blank rows before processes
            }
            if (ly < maxY) {
                ly += DrawProcessPanel(stdscr, data, ly, lx, leftW);
            }
            if (ly < maxY) {
                ly += DrawCorePanel(stdscr, data, ly, lx, leftW);
            }
        }
        if (ly > maxY) ly = maxY;

        // === Right panels (storage / network / thermal first) ===
        int ry = 2;
        if (ry < maxY) {
            ry += DrawDiskPanel(stdscr, data, ry, rx, rightW);
            if (ry < maxY) {
                ry += DrawPhysicalDiskPanel(stdscr, data, ry, rx, rightW);
            }
            if (ry < maxY) {
                ry += DrawNetworkPanel(stdscr, data, ry, rx, rightW);
            }
            if (ry < maxY) {
                ry += DrawNetGraphPanel(stdscr, data, ry, rx, rightW);
            }
            // WiFi & Bluetooth supplementary info after Network
            if (ry < maxY) {
                ry += DrawWifiBluetoothPanel(stdscr, data, ry, rx, rightW);
            }
            // Temps and Power rank above the informational extras: on a short
            // terminal it is Displays/Accel/TPM that silently drop, not the
            // thermal state.
            if (ry < maxY) {
                ry += DrawTempPanel(stdscr, data, ry, rx, rightW);
            }
            if (ry < maxY) {
                ry += DrawPowerPanel(stdscr, data, ry, rx, rightW);
            }
            if (ry < maxY) {
                ry += DrawDisplayPanel(stdscr, data, ry, rx, rightW);
            }
            if (ry < maxY) {
                ry += DrawAccelPanel(stdscr, data, ry, rx, rightW);
            }
            if (ry < maxY) {
                ry += DrawTpmPanel(stdscr, data, ry, rx, rightW);
            }
        }
        if (ry > maxY) ry = maxY;

        // === Bottom status rows (GUI tier-1) ===
        // The old framed Connections + System bands (6 rows incl. two
        // separators) collapse into three compact rows, giving the panels
        // two extra rows while keeping every piece of information:
        //   rows-4  uptime | load | procs        (plain)
        //   rows-3  [Connections] clients · push (single severity color)
        //   rows-2  [System] OS ······ battery   (battery keeps its color)
        // Content may run down to rows-5; anything below is wiped and
        // these rows repaint on top, so panel overrun can never corrupt them.
        int contentEnd = ly > ry ? ly : ry;
        if (contentEnd >= rows - 4) contentEnd = rows - 5;
        for (int r = rows - 4; r <= rows - 2; ++r) {
            mvhline(r, 0, ' ', cols);
        }

        // Row rows-4: uptime | load | processes (was System line 2).
        if (data.uptimeSeconds > 0 || data.loadAvg1 > 0 || data.processCount > 0) {
            std::string sysStr;
            if (data.uptimeSeconds > 0) {
                uint64_t days = data.uptimeSeconds / 86400;
                uint64_t hours = (data.uptimeSeconds % 86400) / 3600;
                uint64_t mins = (data.uptimeSeconds % 3600) / 60;
                sysStr += "Uptime: ";
                if (days > 0) sysStr += std::to_string(days) + "d ";
                sysStr += std::to_string(hours) + "h " + std::to_string(mins) + "m";
            }
            if (data.loadAvg1 > 0) {
                char buf[64];
                snprintf(buf, sizeof(buf), "   Load: %.2f %.2f %.2f",
                         data.loadAvg1, data.loadAvg5, data.loadAvg15);
                sysStr += buf;
            }
            if (data.processCount > 0) {
                sysStr += "   Procs: " + std::to_string(data.processCount);
            }
            mvwprintw(stdscr, rows - 4, 2, "%.*s", cols - 4,
                      TrimRight(sysStr, cols - 4).c_str());
        }

        // Row rows-3: Connections + server push on one line. One severity
        // color for the whole row: red on push error, yellow while
        // connecting, green when clients are attached or uploading.
        if (data.connectionCount >= 0) {
            std::string line;
            if (data.connectionCount > 0) {
                int avaloniaCount = 0, mcpCount = 0, unknownCount = 0;
                for (auto t : data.clientTypes) {
                    if (t == 1) avaloniaCount++;
                    else if (t == 2) mcpCount++;
                    else unknownCount++;
                }
                if (avaloniaCount > 0) line += "Avalonia x" + std::to_string(avaloniaCount) + " ";
                if (mcpCount > 0) line += "MCP x" + std::to_string(mcpCount) + " ";
                if (unknownCount > 0) line += "? x" + std::to_string(unknownCount) + " ";
                if (data.httpClientCount > 0)
                    line += "Web x" + std::to_string(data.httpClientCount) + " ";
                if (!data.connectionSince.empty())
                    line += "since " + data.connectionSince;
            } else {
                line = "no clients";
            }
            std::string pushStr;
            int color = (data.connectionCount > 0) ? 2 : -1;
            if (!data.serverPushEnabled) {
                pushStr = "Push: disabled (press S to configure)";
            } else {
                std::string age = "never";
                if (data.lastPushMs > 0) {
                    int64_t sec = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count()
                        - data.lastPushMs / 1000;
                    age = std::to_string((std::max)(int64_t(0), sec)) + "s ago";
                }
                // "connecting..." / "uploading (dev_xxx)" / "error: ...".
                const bool err = data.serverStatus.rfind("error", 0) == 0;
                const bool connecting = data.serverStatus.rfind("connecting", 0) == 0;
                color = err ? 4 : (connecting ? 3 : 2);
                pushStr = "Push: " + data.serverStatus + " (last " + age + ")";
            }
            if (!pushStr.empty()) line += "   " + pushStr;
            line += "   [c=list]";   // 'c' expands this chip into the client list
            wattron(stdscr, A_REVERSE);
            mvwprintw(stdscr, rows - 3, 1, " Connections ");
            wattroff(stdscr, A_REVERSE);
            const int availW = cols - 17;
            if (color >= 0) wattron(stdscr, COLOR_PAIR(color));
            mvwprintw(stdscr, rows - 3, 16, "%.*s", availW,
                      TrimRight(line, availW).c_str());
            if (color >= 0) wattroff(stdscr, COLOR_PAIR(color));
        }

        // Row rows-2: [System] OS · battery (battery keeps its severity color).
        wattron(stdscr, A_REVERSE);
        mvwprintw(stdscr, rows - 2, 1, " System ");
        wattroff(stdscr, A_REVERSE);
        if (!data.osVersion.empty()) {
            const std::string os = TrimRight(data.osVersion, (cols > 46) ? cols - 42 : 12);
            mvwprintw(stdscr, rows - 2, 10, "%s", os.c_str());
        } else {
            mvwprintw(stdscr, rows - 2, 10, "Unknown OS");
        }
        if (data.batteryPercent >= 0 && data.batteryPercent <= 100) {
            auto batStr = (data.acOnline ? "AC" : "BAT") + std::string(" ") + std::to_string(data.batteryPercent) + "%";
            const int color = LowIsWorsePair(data.batteryPercent, kBattWarn, kBattCrit);
            if (color >= 0) wattron(stdscr, COLOR_PAIR(color));
            mvwprintw(stdscr, rows - 2, cols - static_cast<int>(batStr.size()) - 2, "%s", batStr.c_str());
            if (color >= 0) wattroff(stdscr, COLOR_PAIR(color));
        }

        // Bottom border — repainted last so panel overflow can never leave
        // stray cells on it after the band wipe above.
        mvwprintw(stdscr, rows - 1, 0, "%s", topBot.c_str());
        mvwprintw(stdscr, rows - 1, 0, "+");
        mvwprintw(stdscr, rows - 1, cols - 1, "+");

        // Never park the cursor on the bottom-right corner: the bottom
        // border writes there every frame, terminals treat that cell as a
        // wrap trigger, and ncurses can loop forever re-positioning the
        // cursor (_nc_mvcur_sp) — a probabilistic UI freeze. Move away
        // from the corner before refresh.
        move(rows - 2, 0);

        refresh();

        // Check resize more frequently during sleep
        for (int i = 0; i < 3 && running_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    SafeEndwin();
}

void TuiApp::SafeEndwin() {
    bool expected = true;
    if (cursesActive_.compare_exchange_strong(expected, false)) {
        endwin();
    }
}

} // namespace tcmt
