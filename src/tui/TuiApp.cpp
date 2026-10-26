// MSVC deprecation noise suppression (must come before any includes)
#ifdef _MSC_VER
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <curses.h>
#ifdef __PDCURSES__
// PDCurses internals: the ANSI-path switch, see the note in TuiApp::Run().
extern "C" bool pdc_ansi;
#endif
#include "TuiApp.h"
#include "core/I18n.h"
#include "core/Utils/Logger.h"   // log page header shows the active level
#include <ctime>
#include <cstring>
#include <cstdio>
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

#ifdef _WIN32
extern "C" {
extern bool pdc_ansi, pdc_wt, pdc_conemu;
}
// Declared rather than including <windows.h>: its min/max macros would
// collide with the std:: calls in this file.
extern "C" __declspec(dllimport) unsigned int __stdcall GetConsoleOutputCP(void);
#endif

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

// Declared ahead of their first use: panel rows truncate by display columns,
// not bytes, and the definitions sit with the other string helpers below.
static int utf8_display_width(const std::string& s);
static std::string utf8_truncate(const std::string& s, int maxW);

// Highest sample in a history window, never zero: it is the chart's scale.
static uint64_t PeakOf(const uint64_t* hist, int pos, int len) {
    uint64_t peak = 1;
    for (int i = 0; i < len; ++i) {
        const int idx = (pos - 1 - i + TuiData::NET_HISTORY_MAX) % TuiData::NET_HISTORY_MAX;
        if (hist[idx] > peak) peak = hist[idx];
    }
    return peak;
}

// Every detail overlay draws the same box: W columns wide counting both
// corner cells, H rows tall, side borders at x0 and x0 + W - 1. The inline
// version this replaces printed W + 2 dashes between the corners, so the top
// and bottom rows ran two columns past the side borders and wrapped.
static void DrawOverlayFrame(int y0, int x0, int W, int H) {
    const std::string line =
        "+" + std::string(static_cast<size_t>(W > 2 ? W - 2 : 0), '-') + "+";
    mvwprintw(stdscr, y0, x0, "%s", line.c_str());
    for (int r = y0 + 1; r < y0 + H - 1; r++) {
        mvwprintw(stdscr, r, x0, "|");
        mvwprintw(stdscr, r, x0 + W - 1, "|");
    }
    mvwprintw(stdscr, y0 + H - 1, x0, "%s", line.c_str());
}

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
    mvwprintw(win, y, valueStart, "%s", utf8_truncate(value, vw).c_str());
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

    // Chip: ' title ' — reversed by default. Focused turns the WHOLE strip
    // into a solid green reverse bar: coloring only the chip was too quiet
    // to spot with a dozen panels on screen (the green text sat on the
    // default background, while the unfocused chip was already reversed).
    if (focused) {
        wattron(win, COLOR_PAIR(2) | A_REVERSE | A_BOLD);
        mvwprintw(win, y, x0, " %-*s ", tw, title);
        for (int x = x0 + tw + 2; x < x0 + maxW; ++x) {
            mvwaddstr(win, y, x, segShade_.c_str());
        }
        wattroff(win, COLOR_PAIR(2) | A_REVERSE | A_BOLD);
        return;
    }
    {
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
    // Budget and measure in columns: vw sets where the label stops, and a CJK
    // value occupies two columns per three bytes.
    const std::string valueStr = utf8_truncate(value, (std::max)(4, maxW - 6));
    const int vw = utf8_display_width(valueStr);
    const int valueStart = valueX - vw + 1;
    const int labelMax = valueStart - (x0 + 2) - 1;   // 1-col gap before value
    if (labelMax < 3) {   // too tight for a label: value alone, right-aligned
        if (pair >= 0) wattron(win, COLOR_PAIR(pair));
        mvwprintw(win, y, valueStart, "%s", valueStr.c_str());
        if (pair >= 0) wattroff(win, COLOR_PAIR(pair));
        return;
    }
    mvwprintw(win, y, x0 + 2, "%s", utf8_truncate(label, labelMax).c_str());
    if (pair >= 0) wattron(win, COLOR_PAIR(pair));
    mvwprintw(win, y, valueStart, "%s", valueStr.c_str());
    if (pair >= 0) wattroff(win, COLOR_PAIR(pair));
}

// ── UTF-8 display width helpers (CJK-aware) ──────────────────────────
// Decode one code point from UTF-8 without going through the C locale:
// mbstowcs decodes with whatever locale the CRT happens to have, and on a
// GBK-locale Windows box it rejects UTF-8 outright and the width silently
// falls back to the byte count — which is what pushed CJK rows two columns
// out of line.
static uint32_t Utf8Next(const std::string& s, size_t& i) {
    const unsigned char c = static_cast<unsigned char>(s[i++]);
    if (c < 0x80) return c;
    int extra = (c >= 0xF0) ? 3 : (c >= 0xE0) ? 2 : (c >= 0xC0) ? 1 : 0;
    uint32_t cp = c & (extra == 3 ? 0x07u : extra == 2 ? 0x0Fu : extra == 1 ? 0x1Fu : 0x3Fu);
    while (extra-- > 0 && i < s.size())
        cp = (cp << 6) | (static_cast<unsigned char>(s[i++]) & 0x3F);
    return cp;
}

static int Utf8CodepointWidth(uint32_t cp) {
    const int w = portable_wcwidth(static_cast<wchar_t>(cp));
    return w < 0 ? 1 : w;
}

// Returns column display width of a UTF-8 string (2 for CJK chars).
static int utf8_display_width(const std::string& s) {
    int w = 0;
    size_t i = 0;
    while (i < s.size()) w += Utf8CodepointWidth(Utf8Next(s, i));
    return w;
}

// Truncate a UTF-8 string so its display width ≤ maxW, appending "~" if cut.
// Slices on code point boundaries only.
static std::string utf8_truncate(const std::string& s, int maxW) {
    if (maxW < 1) return std::string();
    if (utf8_display_width(s) <= maxW) return s;

    const int targetW = maxW - 1;  // leave room for "~"
    int curW = 0;
    size_t i = 0, cut = 0;
    while (i < s.size()) {
        const size_t start = i;
        const int cw = Utf8CodepointWidth(Utf8Next(s, i));
        if (curW + cw > targetW) break;
        curW += cw;
        cut = i;
    }
    return s.substr(0, cut) + "~";
}

// ─────────────────────────────────────────────────────────────────────

// Same result as a byte prefix for ASCII, which is every numeric row — but a
// name, a disk label or an adapter can be CJK, and cutting one mid-sequence is
// what renders wide text as garbage. The budget is display columns, and a cut
// is marked with "~" like every other truncation in this file.
std::string TuiApp::TrimRight(const std::string& s, int maxLen) {
    // int, not size_t: callers derive the budget from panel geometry, and a
    // negative width used to wrap to a huge size_t — which made the comparison
    // below fail and truncated the string away entirely.
    if (maxLen <= 0) return std::string();
    if (utf8_display_width(s) <= maxLen) return s;
    return utf8_truncate(s, maxLen);
}

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

    // Live sampling cadence, left-aligned on the title row — always visible
    // on the dashboard, adjustable on the settings page (S). Rendered only
    // when the platform reports one, and skipped when the centred title leaves
    // no room; formatted into a stack buffer so the skipped path allocates
    // nothing.
    if (data.sampleIntervalMs <= 0) return;
    char sample[64];
    const int n = std::snprintf(sample, sizeof(sample), "%s: %dms",
                                Tr("hint.sample"), data.sampleIntervalMs);
    if (n <= 0 || n >= static_cast<int>(sizeof(sample))) return;
    // The text occupies display columns [1, w] and the title starts at x;
    // require at least one blank column between them. Measured in display
    // columns, not bytes — the zh label (采集) is wider than it is long.
    const int w = utf8_display_width(sample);
    if (x <= w + 1) return;
    wattron(win, COLOR_PAIR(5));
    mvwprintw(win, 0, 1, "%s", sample);
    wattroff(win, COLOR_PAIR(5));
}

int TuiApp::DrawCpuPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW) {
    if (maxW < 10) return 0;
    int lines = 0;

    DrawPanelTitle(win, y + lines, x0, maxW, "CPU", IsPanelFocused(PanelId::Cpu));
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

    DrawPanelTitle(win, y + lines, x0, maxW, "RAM", IsPanelFocused(PanelId::Memory));
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

const TuiData::GpuEntry* TuiApp::CurrentGpu(const TuiData& data) const {
    if (data.gpus.empty()) return nullptr;
    return &data.gpus[GpuIndexFor(data)];
}

int TuiApp::GpuIndexFor(const TuiData& data) const {
    if (data.gpus.empty()) return 0;
    int idx = gpuIndex_;
    if (idx < 0) idx = 0;
    if (idx >= static_cast<int>(data.gpus.size())) idx = static_cast<int>(data.gpus.size()) - 1;
    return idx;
}

int TuiApp::DrawGpuPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW) {
    if (maxW < 10) return 0;
    int lines = 0;

    const TuiData::GpuEntry* g = CurrentGpu(data);
    const bool multi = data.gpus.size() > 1;
    char title[32];
    if (multi) {
        snprintf(title, sizeof(title), "GPU %d/%d",
                 GpuIndexFor(data) + 1, static_cast<int>(data.gpus.size()));
    } else {
        snprintf(title, sizeof(title), "GPU");
    }
    DrawPanelTitle(win, y + lines, x0, maxW, title, IsPanelFocused(PanelId::Gpu));
    lines++;

#ifdef TCMT_MACOS
    // A single Apple GPU needs no name line: the title already says GPU.
    const bool showName = multi;
#else
    const bool showName = true;
#endif
    const std::string name = g ? g->name : data.gpuName;
    if (showName && !name.empty()) {
        auto n = TrimRight(name, maxW - 4);
        mvwprintw(win, y + lines, x0 + 2, "%.*s", maxW - 2, n.c_str());
        lines++;
    }

    const double usage = g ? g->usage : data.gpuUsage;
    {
        char val[32];
        snprintf(val, sizeof(val), "%.1f%%", usage);
        DrawUsageBarRow(win, y + lines, x0, maxW, "Use:", usage, val);
    }
    lines++;

    // VRAM only for dedicated memory: under unified memory the figure is a
    // slice of system RAM, so the row is omitted (see the detail overlay).
    const bool unified = g ? g->unified : data.gpuUnified;
    const uint64_t mem = g ? g->memory : data.gpuMemory;
    const double memPct = g ? g->memoryPercent : data.gpuMemoryPercent;
    if (!unified && mem > 0 && memPct > 1) {
        uint64_t used = (uint64_t)(mem * memPct / 100.0);
        DrawLabeledValue(win, y + lines, x0, maxW, "VRAM:",
                         FormatSize(used) + " / " + FormatSize(mem));
        lines++;
    }

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

    DrawPanelTitle(win, y, x0, maxW, "Disks", IsPanelFocused(PanelId::Disk));
    int lines = 1;

    for (const auto& d : data.disks) {
        // Compact single-line format: [C:] Label  92% 210G/228G
        char lineBuf[128];
        int off = 0;
        if (d.letter >= 'A' && d.letter <= 'Z')
            off = snprintf(lineBuf, sizeof(lineBuf), "[%c:] ", d.letter);
        auto label = d.label.empty() ? "?" : d.label;
        if (utf8_display_width(label) > 10) label = utf8_truncate(label, 10);
        double upct = (d.totalSize > 0) ? 100.0 * d.usedSpace / d.totalSize : 0;
        auto usedStr = FormatSize(d.usedSpace);
        auto totalStr = FormatSize(d.totalSize);
        off += snprintf(lineBuf + off, sizeof(lineBuf) - off,
                       "%s  %d%% %s/%s",
                       label.c_str(), static_cast<int>(upct),
                       usedStr.c_str(), totalStr.c_str());
        lineBuf[off] = '\0';
        mvwprintw(win, y + lines, x0 + 2, "%s", utf8_truncate(lineBuf, maxW - 4).c_str());
        lines++;
    }
    return lines;
}

int TuiApp::DrawNetworkPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW) {
    if (maxW < 10) return 0;
    DrawPanelTitle(win, y, x0, maxW, "Network", IsPanelFocused(PanelId::Network));
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
            // 加了 hw 前缀：下面可能还有一行当前地址，两条裸 MAC 分不清谁是谁
            mvwprintw(win, y + lines, x0 + 4, "hw  %.*s", maxW - 10, n.mac.c_str());
            lines++;
        }
        // 只在与硬件地址不同时才单列 —— macOS 的私有 Wi-Fi 地址按 SSID 轮换；
        // Windows / Linux 上两者相同，再列一行是噪音。
        if (!n.currentMac.empty() && n.currentMac != n.mac) {
            mvwprintw(win, y + lines, x0 + 4, "cur %.*s", maxW - 10, n.currentMac.c_str());
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

// Pack space-separated fields into lines no wider than `width`, wrapping to
// the next line when a field does not fit. A single field wider than the line
// is truncated (with ~) so the packing loop always terminates.
static std::vector<std::string> WrapFields(const std::vector<std::string>& fields, int width) {
    std::vector<std::string> out;
    if (width < 4) width = 4;
    std::string cur;
    for (const std::string& f : fields) {
        if (f.empty()) continue;
        std::string field = (utf8_display_width(f) > width) ? utf8_truncate(f, width) : f;
        if (cur.empty()) {
            cur = field;
        } else if (utf8_display_width(cur) + 2 + utf8_display_width(field) <= width) {
            cur += "  " + field;
        } else {
            out.push_back(cur);
            cur = field;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    if (out.empty()) out.push_back(std::string());
    return out;
}

std::string TuiApp::BlockGlyph(int level) const {
    if (level <= 0) return " ";
    if (level >= 8) return asciiMode_ ? "#" : "\xe2\x96\x88";   // █
    return SparkGlyph(level);
}

std::string TuiApp::SparkGlyph(int level) const {
    if (level < 0) level = 0;
    if (level > 7) level = 7;
    if (asciiMode_) return sparkChars_.substr(static_cast<size_t>(level), 1);
    if (level == 0) return sparkChars_.substr(0, 1);
    return sparkChars_.substr(1 + static_cast<size_t>(level - 1) * 3, 3);
}

// RSSI → percentage, using the mapping most Bluetooth stacks ship: quality =
// 2 * (dBm + 100), clamped to 0..100. So -50 dBm is 100%, -70 is 60%, -100
// is 0%. There is no standardized conversion; this one is at least readable
// without a legend. Returns -1 when the device reports no RSSI.
static int BtSignalPercent(int rssi) {
    if (rssi >= 0) return -1;
    int pct = 2 * (rssi + 100);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
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
            (data.wifiLocationRoot || data.wifiLocationStatus == 0 ||
             data.wifiLocationStatus == 1 || data.wifiLocationDenied);
        if (locBlocked) {
            const bool denied = (data.wifiLocationStatus == 1 || data.wifiLocationDenied);
            // Root never gets a Location grant, so "press R" would be a dead
            // end: name the cause and the way out instead.
            const char* headKey = data.wifiLocationRoot ? "wifi.loc_root"
                                : denied              ? "wifi.loc_denied"
                                                      : "wifi.loc_off";
            const char* hintKey = data.wifiLocationRoot ? "wifi.g_root"
                                : denied              ? "wifi.grant"
                                                      : "wifi.press_r";
            wattron(win, COLOR_PAIR(6));
            mvwprintw(win, y + lines, x0 + 8, "%s",
                      utf8_truncate(Tr(headKey), maxW - 10).c_str());
            wattroff(win, COLOR_PAIR(6));
            lines++;
            wattron(win, COLOR_PAIR(6));
            mvwprintw(win, y + lines, x0 + 8, "%s",
                      utf8_truncate(Tr(hintKey), maxW - 10).c_str());
            wattroff(win, COLOR_PAIR(6));
            lines++;
        } else {
            wattron(win, COLOR_PAIR(3));
            mvwprintw(win, y + lines, x0 + 8, "%s",
                      utf8_truncate(Tr("wifi.disconnected"), maxW - 10).c_str());
            wattroff(win, COLOR_PAIR(3));
            lines++;
        }
    } else {
        std::vector<std::string> fields;
        fields.push_back(Tr("wifi.connected"));
        if (data.wifiLocationRoot) {
            fields.push_back(Tr("wifi.ssid_root"));
        } else if (data.wifiLocationStatus == 1 || data.wifiLocationDenied) {
            fields.push_back(Tr("wifi.ssid_denied"));
        } else {
            if (!data.wifiSSID.empty()) fields.push_back("SSID: " + data.wifiSSID);
            if (!data.wifiBSSID.empty()) fields.push_back("BSSID: " + data.wifiBSSID);
        }
        // Radio-level facts are not TCC-gated: keep showing them when the
        // SSID is hidden. They are all a root/sudo process can read, and
        // dropping them made the panel look empty rather than partial.
        if (data.wifiChannel > 0) fields.push_back("Ch: " + std::to_string(data.wifiChannel));
        if (data.wifiRSSI < 0) fields.push_back("RSSI: " + std::to_string(data.wifiRSSI) + " dBm");
        if (!data.wifiSecurity.empty()) fields.push_back(data.wifiSecurity);
        if (!data.wifiBand.empty()) fields.push_back(data.wifiBand);
        if (!data.wifiGen.empty()) fields.push_back(data.wifiGen);
        if (data.wifiTxRate > 0) fields.push_back("Tx: " + std::to_string(static_cast<int>(data.wifiTxRate)) + "Mbps");
        // Wrap instead of truncating: a long SSID/BSSID must not push the
        // rest of the detail (Ch/RSSI/Tx) off the panel.
        const std::vector<std::string> wl = WrapFields(fields, maxW - 10);
        wattron(win, COLOR_PAIR(5));
        mvwprintw(win, y + lines, x0 + 2, "WiFi:");
        wattroff(win, COLOR_PAIR(5));
        wattron(win, COLOR_PAIR(2));
        for (size_t i = 0; i < wl.size(); ++i) {
            mvwprintw(win, y + lines + static_cast<int>(i), x0 + 8,
                      "%s", utf8_truncate(wl[i], maxW - 10).c_str());
        }
        wattroff(win, COLOR_PAIR(2));
        lines += static_cast<int>(wl.size());

        // Location Services guidance — the R affordance stays visible in
        // every non-authorized state so the user can tell where they stand
        // by pressing it: an authorization prompt appears while the decision
        // is still open; nothing happens when it was denied (and the SSID
        // stays hidden) — the settings path below is then the only way in.
        // A platform capability: only macOS fills wifiLocationStatus, so
        // Windows (WLAN API) and Linux must never see this macOS flow.
        if (caps_.wifiLocationServices &&
            (data.wifiLocationRoot || data.wifiLocationStatus == 0 ||
             data.wifiLocationStatus == 1 || data.wifiLocationDenied)) {
            wattron(win, COLOR_PAIR(6));
            if (data.wifiLocationRoot) {
                // Root can never be granted Location Services, so neither
                // the R key nor the System Settings path applies.
                mvwprintw(win, y + lines, x0 + 2, "%s",
                          utf8_truncate(Tr("wifi.g1_root"), maxW - 4).c_str());
                lines++;
            } else {
                const bool denied = (data.wifiLocationStatus == 1 || data.wifiLocationDenied);
                mvwprintw(win, y + lines, x0 + 2, "%s",
                          utf8_truncate(Tr(denied ? "wifi.g1_denied" : "wifi.g1_open"),
                                        maxW - 4).c_str());
                lines++;
                mvwprintw(win, y + lines, x0 + 2, "%s",
                          utf8_truncate(Tr("wifi.g2"), maxW - 4).c_str());
                lines++;
                mvwprintw(win, y + lines, x0 + 2, "%s",
                          utf8_truncate(Tr("wifi.path1"), maxW - 4).c_str());
                lines++;
                if (denied) {
                    mvwprintw(win, y + lines, x0 + 2, "%s",
                              utf8_truncate(Tr("wifi.path2"), maxW - 4).c_str());
                    lines++;
                }
            }
            wattroff(win, COLOR_PAIR(6));
        }
    }

    if (data.hasBluetooth) {
        char btBuf[64];
        if (data.btPowerOn) snprintf(btBuf, sizeof(btBuf), Tr("bt.on"), data.btDeviceCount);
        else snprintf(btBuf, sizeof(btBuf), "%s", Tr("bt.off"));
        std::string btStr = btBuf;
        lines++;  // blank line between WiFi and BT
        mvwprintw(win, y + lines, x0 + 2, "BT: %.*s", maxW - 8, btStr.c_str());
        lines++;

        // Connected devices, one per line: name, type, battery, signal.
        // Paired-but-idle devices fold into a single count line.
        int pairedIdle = 0;
        for (const auto& d : data.btDevices) {
            if (!d.connected) { ++pairedIdle; continue; }
            if (y + lines >= LINES - 5) break;
            std::string line = d.name;
            if (!d.type.empty()) line += "  " + d.type;
            if (!d.battery.empty()) line += "  " + d.battery;
            if (d.rssi < 0) {
                const int pct = BtSignalPercent(d.rssi);
                // "Signal" keeps it distinct from the battery percentages
                // that sit right before it on the same line; the raw dBm
                // stays next to it for anyone who wants the measurement.
                if (pct >= 0) {
                    line += "  Signal " + std::to_string(pct) + "% (" +
                            std::to_string(d.rssi) + "dBm)";
                }
            }
            mvwprintw(win, y + lines, x0 + 4, "%s",
                      utf8_truncate(line, maxW - 6).c_str());
            lines++;
        }
        if (pairedIdle > 0 && y + lines < LINES - 5) {
            char buf[64];
            snprintf(buf, sizeof(buf), Tr("bt.paired"), pairedIdle);
            wattron(win, A_DIM);
            mvwprintw(win, y + lines, x0 + 4, "%s",
                      utf8_truncate(buf, maxW - 6).c_str());
            wattroff(win, A_DIM);
            lines++;
        }
    }

    return lines;
}

int TuiApp::DrawDisplayPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW) {
    if (maxW < 10) return 0;
    if (data.displays.empty()) return 0;
    int lines = 0;

    DrawPanelTitle(win, y + lines, x0, maxW, "Displays", IsPanelFocused(PanelId::Display));
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
        // No %.*s precision: that counts BYTES while TrimRight budgets display
        // columns, so a CJK name would be re-cut mid-sequence here.
        mvwprintw(win, y + lines, x0 + 2, "%s", TrimRight(line, maxW - 4).c_str());
        lines++;
    }

    return lines;
}

int TuiApp::DrawTpmPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW) {
    if (maxW < 10) return 0;
    if (data.tpmInfo.empty() || data.tpmInfo == "No TPM") return 0;
    DrawPanelTitle(win, y, x0, maxW, "TPM", IsPanelFocused(PanelId::Tpm));
    mvwprintw(win, y + 1, x0 + 2, "%s", utf8_truncate(data.tpmInfo, maxW - 2).c_str());
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
                off += snprintf(buf + off, sizeof(buf) - off, " %lluh",
                                static_cast<unsigned long long>(pd.powerOnHours));
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
    // The count is part of the title so "the extra sensors disappeared" can be
    // told apart from "the panel shows fewer rows": 15 that do not fit page,
    // 8 means the list itself shrank.
    const std::string title = data.temperatures.empty()
        ? std::string("Temps")
        : "Temps (" + std::to_string(data.temperatures.size()) + ")";
    DrawPanelTitle(win, y, x0, maxW, title.c_str(), IsPanelFocused(PanelId::Temp));
    int lines = 1;

    // Grow the grid with the space actually available below the title
    // (content may run down to row LINES-5, see Run), capped at 8 rows:
    // with room, every sensor is visible at once and no paging happens;
    // the 3-second auto-rotate only kicks in when the panel truly overflows.
    // The bound is the real last row of the dashboard, not a fixed offset:
    // the right column above this panel varies in height, so a fixed base let
    // the grid run past the panel's slot and paint over the rows below.
    const int maxContentRows = (std::max)(1, (LINES - 5) - y - 1);
    int contentRows = (std::max)(1, (std::min)(maxContentRows, 8));

    // All sensors are pre-filtered upstream; display everything
    std::vector<std::pair<std::string, double>> displayTemps;
    for (const auto& [name, temp] : data.temperatures)
        displayTemps.push_back({name, temp});

    // Two columns normally. When the list overflows the rows the panel has,
    // a third column keeps every sensor on one page; rotating pages made the
    // extra sensors look like they flashed and vanished every three seconds.
    int cols = 2;
    const int colW = maxW / cols;

    int perPage = contentRows * cols;
    int totalPages = (std::max)(1, (static_cast<int>(displayTemps.size()) + perPage - 1) / perPage);
    bool needPaging = totalPages > 1;
    if (needPaging) {
        // The page indicator owns a row of its own: take it from the grid so
        // the panel keeps the height it reported to the layout.
        contentRows = (std::max)(1, contentRows - 1);
        perPage = contentRows * cols;
        totalPages = (std::max)(1, (static_cast<int>(displayTemps.size()) + perPage - 1) / perPage);
    }

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
    const int startIdx = currentPage * perPage;
    const int total = static_cast<int>(displayTemps.size());
    // Clamp the label field: colW shrinks with the panel and a negative
    // width would wrap to size_t and blank the name out entirely.
    const int labelW = (std::max)(1, colW - 8);
    for (int r = 0; r < contentRows; ++r) {
        bool drew = false;
        for (int c = 0; c < cols; ++c) {
            const int idx = startIdx + r * cols + c;
            if (idx >= total) break;

            auto& [nameS, tempS] = displayTemps[idx];
            auto label = TrimRight(nameS, labelW);
            const int tc = HighIsWorsePair(tempS, kTempWarn, kTempCrit);
            if (tc >= 0) wattron(win, COLOR_PAIR(tc));
            // Labels pad into a fixed field so every temperature value in the
            // column shares one left edge (#6); sensor names are ASCII.
            mvwprintw(win, y + lines, x0 + 2 + c * colW, "%-*s %.1f C",
                      labelW, label.c_str(), tempS);
            if (tc >= 0) wattroff(win, COLOR_PAIR(tc));
            drew = true;
        }
        if (!drew) break;
        lines++;
        actualRows = r + 1;
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
#if defined(TCMT_LINUX)
    // Linux has no IOReport, so there is no CPU/GPU/ANE split. What the
    // platform actually reports is the battery/AC state from power_supply and,
    // when readable, RAPL package power for the CPU.
    const bool hasBattery = data.batteryPercent >= 0;
    const bool hasRapl = data.cpuPower > 0.0;

    DrawPanelTitle(win, y, x0, maxW, "Power", IsPanelFocused(PanelId::Power));
    int lines = 1;

    if (data.thermalState > 0) {
        static const char* labels[] = {"", "Fairly Serious", "Critical"};
        const char* label = (data.thermalState < 3) ? labels[data.thermalState] : "Unknown";
        const int pair = (data.thermalState >= 2) ? 4 : 3;
        DrawLabeledValue(win, y + lines, x0, maxW, "Thermal:", label, pair);
        lines++;
    }

    char val[32];
    if (hasBattery) {
        snprintf(val, sizeof(val), "%d%%", data.batteryPercent);
        DrawLabeledValue(win, y + lines, x0, maxW, "Battery:", val);
        lines++;

        const char* state = data.batteryIsCharging
                                ? "Charging"
                                : (data.batteryAmperage < 0 ? "Discharging" : "Idle");
        DrawLabeledValue(win, y + lines, x0, maxW, "State:", state);
        lines++;

        if (data.batteryAmperage != 0 && data.batteryVoltage > 0) {
            const int64_t powerMw =
                static_cast<int64_t>(std::abs(data.batteryAmperage)) *
                static_cast<int64_t>(data.batteryVoltage) / 1000;
            if (powerMw > 0) {
                const char* dir = (data.batteryAmperage > 0) ? "Chg" : "Dchg";
                char watts[32];
                snprintf(watts, sizeof(watts), "%.2f W", powerMw / 1000.0);
                const std::string text = std::string(dir) + " " + watts;
                DrawLabeledValue(win, y + lines, x0, maxW, "Power:", text);
                lines++;
            }
        }
    }

    // Only laptops report an AC supply; a desktop without power_supply would
    // otherwise be labelled "offline".
    if (hasBattery) {
        DrawLabeledValue(win, y + lines, x0, maxW, "AC:",
                         data.acOnline ? "online" : "offline");
        lines++;
    }

    if (hasRapl) {
        snprintf(val, sizeof(val), "%.2f W", data.cpuPower / 1000.0);
        DrawLabeledValue(win, y + lines, x0, maxW, "CPU:", val);
        lines++;
    }
    if (data.gpuPower > 0.0) {
        snprintf(val, sizeof(val), "%.2f W", data.gpuPower / 1000.0);
        DrawLabeledValue(win, y + lines, x0, maxW, "GPU:", val);
        lines++;
    }

    if (!hasBattery && !hasRapl) {
        // Covers both cases: no powercap device at all (VMs, many desktops)
        // and a device that exists but is root-only. Do not promise sudo here.
        const std::string hint = "No battery; RAPL not readable";
        wattron(win, A_DIM);
        mvwprintw(win, y + lines, x0 + 2, "%.*s", maxW - 4,
                  TrimRight(hint, (maxW - 4) > 0 ? maxW - 4 : 0).c_str());
        wattroff(win, A_DIM);
        lines++;
    }

    return lines;
#else
    bool hasPower = (data.cpuPower > 0 || data.gpuPower > 0 || data.anePower > 0);
    bool hasBattery = (data.batteryCycleCount > 0 || data.batteryHealthPercent > 0);
    // When power is unavailable there is still a status worth showing: the
    // panel renders N/A rows plus an install hint instead of vanishing.
    const bool powerUnavailable = !data.powerAvailable;
    if (!powerUnavailable && !hasPower && !hasBattery && data.thermalState == 0) return 0;

    DrawPanelTitle(win, y, x0, maxW, "Power", IsPanelFocused(PanelId::Power));
    int lines = 1;

    // Thermal state — value carries the severity pair.
    if (data.thermalState > 0) {
        static const char* labels[] = {"", "Fairly Serious", "Critical"};
        const char* label = (data.thermalState < 3) ? labels[data.thermalState] : "Unknown";
        const int pair = (data.thermalState >= 2) ? 4 : 3;
        DrawLabeledValue(win, y + lines, x0, maxW, "Thermal:", label, pair);
        lines++;
    }

    // Power consumption. Available: always render all rows — 0 means no
    // data yet, hiding rows made the panel look broken. Unavailable:
    // IOReport energy cannot be read (macOS 27 unprivileged, or the
    // tcmt-powerd helper is not installed) — report N/A instead of a
    // misleading "0.00 W" idle reading.
    if (powerUnavailable) {
        DrawLabeledValue(win, y + lines, x0, maxW, "CPU:", "N/A");
        lines++;
        DrawLabeledValue(win, y + lines, x0, maxW, "GPU:", "N/A");
        lines++;
        DrawLabeledValue(win, y + lines, x0, maxW, "ANE:", "N/A");
        lines++;
        DrawLabeledValue(win, y + lines, x0, maxW, "Total:", "N/A");
        lines++;
#ifdef TCMT_WINDOWS
        const std::string hint = "Power unavailable — CPU needs PawnIO, GPU needs NVML";
#elif defined(TCMT_LINUX)
        const std::string hint =
            "Power unavailable — needs power_supply or RAPL (root-only)";
#else
        const std::string hint = "Power unavailable — run tools/install-powerd.sh (sudo)";
#endif
        wattron(win, A_DIM);
        mvwprintw(win, y + lines, x0 + 2, "%.*s", maxW - 4,
                  TrimRight(hint, (maxW - 4) > 0 ? maxW - 4 : 0).c_str());
        wattroff(win, A_DIM);
        lines++;
    } else {
        char val[32];
#ifdef TCMT_WINDOWS
        // Windows has no ANE, and 0 mW means "source unavailable" rather than
        // an idle 0 W reading. Keep the row but say N/A instead of 0.00 W.
        auto formatWatts = [&val](double milliwatts) -> std::string {
            if (milliwatts <= 0.0) return "N/A";
            snprintf(val, sizeof(val), "%.2f W", milliwatts / 1000.0);
            return val;
        };
        DrawLabeledValue(win, y + lines, x0, maxW, "CPU:",
                         formatWatts(data.cpuPower));
        lines++;
        DrawLabeledValue(win, y + lines, x0, maxW, "GPU:",
                         formatWatts(data.gpuPower));
        lines++;
        DrawLabeledValue(win, y + lines, x0, maxW, "Total:",
                         formatWatts(data.cpuPower + data.gpuPower));
        lines++;
#else
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
#endif
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
        // Charger: the adapter reports its marketing name, rated wattage and
        // the voltage/current it negotiated with the Mac.
        if (data.chargerWatts > 0 || !data.chargerName.empty()) {
            std::string name = data.chargerName;
            if (name.empty()) {
                char w[32];
                snprintf(w, sizeof(w), "%.0f W", data.chargerWatts);
                name = w;
            }
            DrawLabeledValue(win, y + lines, x0, maxW, "Charger:", name);
            lines++;
            if (data.chargerVoltageMv > 0 || data.chargerCurrentMa > 0) {
                char val[64];
                snprintf(val, sizeof(val), "%.0f W   %.1f V / %.2f A",
                         data.chargerWatts,
                         data.chargerVoltageMv / 1000.0,
                         data.chargerCurrentMa / 1000.0);
                DrawLabeledValue(win, y + lines, x0, maxW, "Output:", val);
                lines++;
            }
        }
        // Battery temp — shown in Temperature panel (from TemperatureWrapper/iokit_battery_temp)
    }

    return lines;
#endif
}

int TuiApp::DrawAccelPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW) {
    if (maxW < 10) return 0;
    bool hasSensor = data.alsValid || data.accel.hasDevice || data.gyro.valid ||
                     data.lidAngle.valid ||
                     data.motionHb.valid || data.deviceMotion.valid;
    if (!hasSensor) return 0;

    DrawPanelTitle(win, y, x0, maxW, "Sensors", IsPanelFocused(PanelId::Accel));
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
        mvwprintw(win, y + lines++, x0 + 2, "Lid:     %.0f%s", data.lidAngle.angle, degSuffixAngle_.c_str());
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

// "12:04" under an hour, "1:12:04" above it — the CPU-time column's shape.
static std::string CpuTimeShort(uint64_t ms) {
    const uint64_t s = ms / 1000;
    char buf[24];
    if (s >= 3600)
        snprintf(buf, sizeof(buf), "%llu:%02llu:%02llu",
                 (unsigned long long)(s / 3600), (unsigned long long)((s / 60) % 60),
                 (unsigned long long)(s % 60));
    else
        snprintf(buf, sizeof(buf), "%llu:%02llu",
                 (unsigned long long)(s / 60), (unsigned long long)(s % 60));
    return std::string(buf);
}

// Fixed-width cell: pad by display columns (CJK-safe), cut with ~ when it does
// not fit — the row's columns stay aligned without counting bytes.
static std::string PadCell(const std::string& s, int w) {
    const int dw = utf8_display_width(s);
    if (dw > w) return utf8_truncate(s, w);
    return (dw < w) ? s + std::string(w - dw, ' ') : s;
}

int TuiApp::DrawProcessPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW) {
    // 27 = name(6 minimum) + pid(6) + mem(4) + cpu(6) + the five separators
    // and offsets. Below that the fixed cells alone overflow the panel, so
    // hide the list instead of writing past its right edge.
    if (maxW < 27 || data.topProcesses.empty()) {
        procListY_ = -1;   // no list this frame — mouse hits nothing
        procListCount_ = 0;
        return 0;
    }

    // Focused strip (bold) while a process selection exists — the panel is
    // then the active surface; reverse keeps it theme- and NO_COLOR-proof.
    DrawPanelTitle(win, y, x0, maxW, "Top Processes", IsPanelFocused(PanelId::Process));
    int lines = 1;

    // Cache the list geometry for the mouse shim (MapMouseEvent). The header
    // row is not clickable, so rows start one line below it.
    procListY_ = y + 1;
    procListX0_ = x0;
    procListW_ = maxW;

    // Optional context columns after the CPU% cell: state, threads, CPU time.
    // They are added while the name still keeps kNameMin columns, and dropped
    // right-to-left (CPU time → threads → state), so a narrow terminal loses
    // the least useful column and the name stays readable. 22 = the fixed
    // pid/mem cells + padding around the row + the one-column gap between
    // the CPU% cell and the context columns.
    const int kNameMin = 10;
#ifdef TCMT_WINDOWS
    // Windows has no reliable user-mode process state: deriving it from
    // thread states classified most rows as Idle. Leave the column out and
    // give its width to the process name; macOS keeps the real state column.
    const bool showState = false;
#else
    const bool showState = (maxW - 22 - 7 >= kNameMin);
#endif
    // The thread column carries the full word "Threads": the abbreviation was
    // ambiguous, and 3 more columns are cheaper than a legend.
    const bool showThr   = (maxW - 22 - (showState ? 7 : 0) - 8 >= kNameMin);
    const bool showCpuT  = (maxW - 22 - (showState ? 7 : 0) - (showThr ? 8 : 0) - 9 >= kNameMin);
    const int optW = (showState ? 7 : 0) + (showThr ? 8 : 0) + (showCpuT ? 9 : 0);

    int nameW = maxW - 22 - optW;
    if (nameW < 6) nameW = 6;
    const int cpuCol = x0 + 2 + nameW + 13;

    // Column header. Without it the PID, memory, CPU% and context numbers are
    // unlabeled. Dim label styling, same widths as the data rows.
    {
        const std::string nameHdr = PadCell("Name", nameW);
        char headBuf[512];
        snprintf(headBuf, sizeof(headBuf), "%s %6s %4s ",
                 nameHdr.c_str(), "PID", "Mem");
        std::string tail;
        if (showState) tail += PadCell("State", 7);
        if (showThr)   { tail += " "; tail += PadCell("Threads", 7); }
        if (showCpuT)  { tail += " "; tail += PadCell("CPU Time", 8); }

        wattron(win, A_DIM);
        mvwprintw(win, y + lines, x0 + 2, "%s", headBuf);
        mvwprintw(win, y + lines, cpuCol, "%5s%%", "CPU");
        if (!tail.empty())
            mvwprintw(win, y + lines, cpuCol + 7, "%s", tail.c_str());
        wattroff(win, A_DIM);
        lines++;
    }

    for (const auto& p : data.topProcesses) {
        // Pad/truncate name to exact display width (CJK-safe)
        std::string name = p.name;
        int dw = utf8_display_width(name);
        if (dw > nameW) {
            name = utf8_truncate(name, nameW);
            dw = utf8_display_width(name);
        }
        if (dw < nameW) name.append(nameW - dw, ' ');

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

        // The wincon backend re-positions the cursor absolutely at every
        // attribute run, but a single run that carries UTF-8 bytes drifts from
        // the console's own column count (one extra column per CJK character).
        // Drawing the name and the numbers as separate runs, split by an
        // invisible dim space, makes every field land where it belongs and
        // leaves nothing behind when the row is erased.
        mvwprintw(win, y + lines, x0 + 2, "%s", name.c_str());
        wattron(win, A_DIM);
        mvwaddch(win, y + lines, x0 + 2 + nameW, ' ');
        wattroff(win, A_DIM);
        mvwprintw(win, y + lines, x0 + 2 + nameW + 1, "%6s", pidStr.c_str());
        mvwprintw(win, y + lines, x0 + 2 + nameW + 8, "%4s", memStr.c_str());

        // Column of "%5.1f%%": name (padded to nameW) + " %6s %4s " above.
        // Only out-of-range CPU% gets a color (severity table, #5); the
        // selected row flips to plain reverse (no per-cell color) so the
        // highlight stays unambiguous.
        const int cpuColor = HighIsWorsePair(p.cpuPercent, kProcCpuWarn, kProcCpuCrit);
        if (cpuColor >= 0 && !selected) wattron(win, COLOR_PAIR(cpuColor));
        mvwprintw(win, y + lines, cpuCol, "%5.1f%%", p.cpuPercent);
        if (cpuColor >= 0 && !selected) wattroff(win, COLOR_PAIR(cpuColor));

        // Context columns — dim, because they describe the process rather than
        // signal anything. They are drawn inside the row's reverse highlight
        // (A_DIM is skipped there) so a selected row stays one block.
        if (optW > 0) {
            std::string tail;
            if (showState) tail += PadCell(p.state.empty() ? "N/A" : p.state, 7);
            if (showThr) {
                // Right-aligned under the word: "Threads" is a header, the
                // column below it is numbers.
                char thr[16];
                snprintf(thr, sizeof(thr), "%7s",
                         p.threads >= 0 ? std::to_string(p.threads).c_str() : "N/A");
                tail += " ";
                tail += thr;
            }
            if (showCpuT)  { tail += " "; tail += PadCell(p.cpuTimeMs > 0 ? CpuTimeShort(p.cpuTimeMs) : "N/A", 8); }
            if (!selected) wattron(win, A_DIM);
            mvwprintw(win, y + lines, cpuCol + 7, "%s", tail.c_str());
            if (!selected) wattroff(win, A_DIM);
        }

        if (selected) wattroff(win, A_REVERSE);
        lines++;
    }

    procListCount_ = lines - 2;   // rows below the title + header
    return lines + 1;  // +1 bottom padding
}

// ────────────────────────────────────────────────────────────────────────────
// Settings page — framed interactive form for the server push settings.
// Up/Down (or Tab) moves focus; Space toggles ON/OFF fields; typing edits
// the URL field (Left/Right/Home/End/Backspace); Enter saves + applies via
// the settings handler; Esc cancels without applying.
void TuiApp::RenderSettingsPage(int rows, int cols, int ch) {
    const int FIELD_COUNT = 6;
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
            g_lang = langOnOpen_;   // revert a live language preview
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
            else if (settingsFocus_ == 5) {
                draftLang_ = (draftLang_ == Lang::En) ? Lang::ZhHans : Lang::En;
                g_lang = draftLang_;   // live preview; Esc restores, Enter keeps
            }
            break;
        case '\n':
        case '\r':
#ifdef KEY_ENTER
        case KEY_ENTER:
#endif
            // Hard bounds for the upload interval (1..60 s).
            if (draftSettings_.intervalSec < 1) draftSettings_.intervalSec = 1;
            if (draftSettings_.intervalSec > 60) draftSettings_.intervalSec = 60;
            // Hard bounds for the sampling interval (same rule ConfigManager
            // enforces on sampling.intervalMs).
            if (draftSettings_.sampleIntervalMs < kSampleIntervalMinMs)
                draftSettings_.sampleIntervalMs = kSampleIntervalMinMs;
            if (draftSettings_.sampleIntervalMs > kSampleIntervalMaxMs)
                draftSettings_.sampleIntervalMs = kSampleIntervalMaxMs;
            if (sampleIntervalHandler_) sampleIntervalHandler_(draftSettings_.sampleIntervalMs);
            if (settingsHandler_) settingsHandler_(draftSettings_);
            {
                std::lock_guard<std::mutex> lock(dataMutex_);
                serverSettings_ = draftSettings_;
            }
            g_lang = draftLang_;   // keep the language chosen in this form
            if (languageHandler_) languageHandler_(draftLang_);   // let main persist it
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
            } else if (settingsFocus_ == 4 && ch >= '0' && ch <= '9') {
                const int next = draftSettings_.sampleIntervalMs * 10 + (ch - '0');
                draftSettings_.sampleIntervalMs =
                    (next > kSampleIntervalMaxMs) ? kSampleIntervalMaxMs : next;
            } else if (settingsFocus_ == 4 && (ch == KEY_BACKSPACE || ch == 127 || ch == 8)) {
                draftSettings_.sampleIntervalMs /= 10;
            }
            break;
    }

    // ---- draw framed dialog ----
    const int W = std::min(72, cols - 8);
    const int H = 16;
    const int x0 = std::max(1, (cols - W) / 2);
    const int y0 = std::max(1, (rows - H) / 2);

    erase();
    // Outer frame (ASCII-safe for both curses and PDCurses)
    DrawOverlayFrame(y0, x0, W, H);
    mvwprintw(stdscr, y0, x0 + 3, " %s ",
              utf8_truncate(Tr("settings.title"), W - 6).c_str());

    // Field rows: [n] label : [ value ] — focused row renders reversed.
    // The label column is padded by display width so CJK labels stay aligned
    // (printf's %-16s counts bytes, which breaks on multi-byte text).
    auto drawField = [&](int idx, int row, const std::string& label, const std::string& value) {
        const int lx = x0 + 2;
        const bool focused = (settingsFocus_ == idx);
        std::string lab = label;
        const int lw = utf8_display_width(lab);
        if (lw < 16) lab.append(16 - lw, ' ');
        else lab = utf8_truncate(lab, 16);
        char buf[160];
        snprintf(buf, sizeof(buf), "[%d] %s : [ %s ]", idx + 1, lab.c_str(), value.c_str());
        if (focused) attron(A_REVERSE);
        mvprintw(row, lx, "%s", buf);
        if (focused) attroff(A_REVERSE);
    };

    const bool on = draftSettings_.enabled;
    drawField(0, y0 + 2, Tr("settings.data_upload"), on ? " ON " : " OFF ");

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
    drawField(1, y0 + 3, Tr("settings.server_url"), window);

    drawField(2, y0 + 4, Tr("settings.skip_tls"), draftSettings_.insecure ? " ON " : " OFF ");
    drawField(3, y0 + 5, Tr("settings.interval"),
              std::to_string(draftSettings_.intervalSec) + Tr("settings.bounds"));
    drawField(4, y0 + 6, Tr("settings.sample_interval"),
              std::to_string(draftSettings_.sampleIntervalMs) + Tr("settings.sample_bounds"));
    drawField(5, y0 + 7, Tr("settings.language"),
              draftLang_ == Lang::ZhHans ? Tr("lang.name.zh") : Tr("lang.name.en"));
    // Saved as ui.language via SetLanguageHandler (main persists it) and read back
    // at startup; TCMT_LANG still takes priority.

    // Status + hints
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        // Prefer the live status from the monitor loop (data_ is refreshed
        // every frame); fall back to the last-saved settings value.
        const std::string st = !data_.serverStatus.empty() ? data_.serverStatus
            : (serverSettings_.status.empty()
                ? (serverSettings_.enabled ? "starting..." : "disabled")
                : serverSettings_.status);
        const std::string stLine = std::string(" ") + Tr("settings.status") + st;
        mvwprintw(stdscr, y0 + 8, x0 + 2, "%s", utf8_truncate(stLine, W - 4).c_str());
    }
    mvwprintw(stdscr, y0 + 10, x0 + 2, " %s",
              utf8_truncate(Tr("settings.hint1"), W - 4).c_str());
    mvwprintw(stdscr, y0 + 11, x0 + 2, " %s",
              utf8_truncate(Tr("settings.hint2"), W - 4).c_str());
    mvwprintw(stdscr, y0 + H - 2, x0 + 2, " %s",
              utf8_truncate(Tr("settings.persist"), W - 4).c_str());

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
// The level the page is showing, spelled out in the header. Without it an
// empty page cannot be told apart from a page whose level gates everything out
// before it ever reaches the buffer (WriteLog returns above the TUI push) —
// which is exactly how this page stayed "mysteriously empty".
static const char* LogLevelName(LogLevel level) {
    switch (level) {
        case LOG_TRACE:    return "trace";
        case LOG_DEBUG:    return "debug";
        case LOG_INFO:     return "info";
        case LOG_WARNING:  return "warning";
        case LOG_ERROR:    return "error";
        case LOG_CRITICAL: return "critical";
        case LOG_FATAL:    return "fatal";
    }
    return "info";
}

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

    std::string header = std::string(" ") + Tr("log.title") + "    " + Tr("log.lines")
                       + std::to_string(lines.size())
                       + "    " + (logFollow_ ? "[FOLLOW]" : "[SCROLL]")
                       + "    " + Tr("log.level") + " " + LogLevelName(Logger::GetLogLevel())
                       + "    " + upArrow_ + "/" + downArrow_ + " " + Tr("log.hint");
    // (Esc/l back to dashboard and q=quit are listed in the ? help page;
    // they are deliberately not repeated on every log line of the header.)
    wattron(stdscr, COLOR_PAIR(5) | A_BOLD);
    mvwprintw(stdscr, 1, 1, "%s", utf8_truncate(header, cols - 2).c_str());
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
        mvwprintw(stdscr, 2, 2, "%s", utf8_truncate(Tr("log.empty"), cols - 4).c_str());
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
        mvwprintw(stdscr, 2 + r, 1, "%s", disp.c_str());
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
        touchwin(stdscr);   // wide-glyph halves survive clear() alone on PDCurses
        return;
    }

    const int W = std::min(74, cols - 6);
    const int H = rows - 4;
    const int x0 = std::max(1, (cols - W) / 2);
    const int y0 = 2;

    erase();
    DrawOverlayFrame(y0, x0, W, H);
    mvwprintw(stdscr, y0, x0 + 3, " Key Bindings ");

    int y = y0 + 1;
    auto group = [&](const std::string& heading,
                     const std::vector<std::pair<std::string, std::string>>& items) {
        if (y >= y0 + H - 2) return;
        wattron(stdscr, COLOR_PAIR(5) | A_BOLD);
        mvwprintw(stdscr, y++, x0 + 2, "%s", utf8_truncate(heading, W - 4).c_str());
        wattroff(stdscr, COLOR_PAIR(5) | A_BOLD);
        for (const auto& item : items) {
            if (y >= y0 + H - 2) return;
            char line[160];
            snprintf(line, sizeof(line), "%-17s %s", item.first.c_str(), item.second.c_str());
            mvwprintw(stdscr, y++, x0 + 2, "%s", utf8_truncate(line, W - 4).c_str());
        }
    };

    // Same gating as the header hints: only list what this build supports.
    std::vector<std::pair<std::string, std::string>> common, dash, logpage;
    common.push_back({"S", Tr("help.server_push")});
    if (caps_.inlineLogPage)
        common.push_back({"L", Tr("help.switch_log")});
    if (updateRequestHandler_)
        common.push_back({"U", Tr("help.check_update")});
    if (locationRequestHandler_)
        common.push_back({"R", Tr("help.request_loc")});
    common.push_back({"Esc", Tr("help.back")});
    common.push_back({"?", Tr("help.close")});
    common.push_back({"q", Tr("help.quit")});

    dash.push_back({"Tab / Shift+Tab", Tr("help.panel_focus")});
    dash.push_back({"Space", Tr("help.cpu_details")});
    dash.push_back({upArrow_ + " / " + downArrow_, Tr("help.select_row")});
    dash.push_back({"c", Tr("help.conn_list")});

    if (caps_.inlineLogPage) {
        logpage.push_back({upArrow_ + " / " + downArrow_, Tr("help.scroll_line")});
        logpage.push_back({"PgUp / PgDn", Tr("help.scroll_page")});
        logpage.push_back({"Home", Tr("help.oldest")});
        logpage.push_back({"End", Tr("help.newest")});
        logpage.push_back({"f", Tr("help.follow")});
        logpage.push_back({"Esc / L", Tr("help.back_dash")});
    }

    group(Tr("help.group.common"), common);
    group(Tr("help.group.dashboard"), dash);
    if (!logpage.empty()) group(Tr("help.group.log"), logpage);

    // Settings keys are hinted inline on that page; one summary line here.
    // Over budget it folds onto another line instead of being chopped: the
    // English hint is longer than this box on narrow terminals, and a cut
    // mid-word — or mid-CJK — is exactly the mess this page should not have.
    {
        std::vector<std::string> words;
        std::istringstream iss(Tr("help.settings_hint"));
        std::string word;
        while (iss >> word) words.push_back(word);
        for (const std::string& ln : WrapFields(words, W - 4)) {
            if (y >= y0 + H - 2) break;
            mvwprintw(stdscr, y++, x0 + 2, "%s", ln.c_str());
        }
    }
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
        touchwin(stdscr);   // wide-glyph halves survive clear() alone on PDCurses
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
    DrawOverlayFrame(y0, x0, W, H);
    mvwprintw(stdscr, y0, x0 + 3, " %s ",
              utf8_truncate(Tr("conn.title"), W - 6).c_str());

    char line[192];
    int y = y0 + 2;
    if (data.connectionCount <= 0) {
        mvwprintw(stdscr, y++, x0 + 2, "  %s",
                  utf8_truncate(Tr("conn.empty"), W - 6).c_str());
    } else {
        snprintf(line, sizeof(line), Tr("conn.count"),
                 data.connectionCount, data.connectionCount == 1 ? "" : "s");
        mvwprintw(stdscr, y++, x0 + 2, "%s", utf8_truncate(line, W - 4).c_str());
        int n = 1;
        for (uint8_t t : data.clientTypes) {
            const char* name = (t == 1) ? "Avalonia" : (t == 2) ? "MCP" : "unknown";
            snprintf(line, sizeof(line), "  #%-3d %s", n++, name);
            mvwprintw(stdscr, y++, x0 + 2, "%s", utf8_truncate(line, W - 4).c_str());
            if (y >= y0 + H - 4) break;   // keep room for the push line
        }
        if (data.httpClientCount > 0) {
            snprintf(line, sizeof(line), "  #%-3d Web (local HTTP) x%d", n, data.httpClientCount);
            mvwprintw(stdscr, y++, x0 + 2, "%s", utf8_truncate(line, W - 4).c_str());
        }
        if (!data.connectionSince.empty()) {
            snprintf(line, sizeof(line), "%s%s", Tr("conn.since"), data.connectionSince.c_str());
            mvwprintw(stdscr, y++, x0 + 2, "%s", utf8_truncate(line, W - 4).c_str());
        }
        if (y < y0 + H - 3) y++;
    }

    // Server push line — same severity coloring as the bottom status row.
    std::string pushStr;
    int color = -1;
    if (!data.serverPushEnabled) {
        pushStr = Tr("push.disabled");
    } else {
        std::string age = Tr("push.never");
        if (data.lastPushMs > 0) {
            int64_t sec = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()
                - data.lastPushMs / 1000;
            age = std::to_string((std::max)(int64_t(0), sec)) + Tr("push.ago");
        }
        const bool err = data.serverStatus.rfind("error", 0) == 0;
        const bool connecting = data.serverStatus.rfind("connecting", 0) == 0;
        color = err ? 4 : (connecting ? 3 : 2);
        pushStr = Tr("push.prefix") + data.serverStatus + Tr("push.last") + age + Tr("push.last_close");
    }
    if (color >= 0) wattron(stdscr, COLOR_PAIR(color));
    mvwprintw(stdscr, y, x0 + 2, "%s", utf8_truncate(pushStr, W - 4).c_str());
    if (color >= 0) wattroff(stdscr, COLOR_PAIR(color));

    mvwprintw(stdscr, y0 + H - 2, x0 + 2, " %s ",
              utf8_truncate(Tr("conn.close"), W - 6).c_str());
}

// CPU detail overlay — Space while the CPU panel holds focus, or a
// double-click on that panel. Carries the numbers the dashboard panel has no
// room for (cluster frequencies, base clock, per-cluster temperatures) plus a
// per-core table where the platform collects one. macOS reports cluster
// frequencies only, so the table there says so instead of showing zeros.
void TuiApp::RenderCpuDetails(int rows, int cols, int ch) {
    const bool isEnter = (ch == '\n' || ch == '\r'
#ifdef KEY_ENTER
        || ch == KEY_ENTER
#endif
    );
    if (ch == 27 || ch == 'q' || ch == 'Q' || ch == ' ' || isEnter) {
        cpuPage_ = false;
        clear();
        touchwin(stdscr);   // wide-glyph halves survive clear() alone on PDCurses
        return;
    }

    TuiData data;
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        data = data_;
    }

    // Cores covered by any of the three samples: usage (macOS), frequency
    // (macOS IOReport), and temp/freq (Linux/Windows).
    const int count = (std::min)(
        (std::max)({(std::max)(static_cast<int>(data.perCoreCount),
                               static_cast<int>(data.perCoreUsageCount)),
                    static_cast<int>(data.perCoreFreqCount)}),
        16);
    // Edge to edge like the other overlays. The per-core bars below take the
    // width left after the label and the value column, so extra columns turn
    // into a longer bar rather than blank space.
    const int W = cols;
    const int H = rows;
    const int x0 = 0;
    const int y0 = 0;

    erase();
    DrawOverlayFrame(y0, x0, W, H);
    mvwprintw(stdscr, y0, x0 + 3, " CPU Details ");

    int y = y0 + 2;
    auto emit = [&](const std::string& s) {
        if (y >= y0 + H - 2) return;
        mvwprintw(stdscr, y++, x0 + 3, "%s", utf8_truncate(s, W - 6).c_str());
    };
    char buf[192];

    if (!data.cpuName.empty()) emit("Name: " + data.cpuName);

    if (data.performanceCores > 0 || data.efficiencyCores > 0) {
        snprintf(buf, sizeof(buf), "Cores: %d (P %d / E %d)",
                 data.physicalCores, data.performanceCores, data.efficiencyCores);
    } else {
        snprintf(buf, sizeof(buf), "Cores: %d", data.physicalCores);
    }
    emit(buf);

    snprintf(buf, sizeof(buf), "Usage: %.1f%%", data.cpuUsage);
    emit(buf);

    if (data.pCoreFreq > 0 || data.eCoreFreq > 0) {
        std::string f = "Freq:";
        if (data.pCoreFreq > 0) {
            snprintf(buf, sizeof(buf), " P %.0f", data.pCoreFreq);
            f += buf;
            if (data.pCoreMaxFreq > 0) {
                snprintf(buf, sizeof(buf), "/%.0f", data.pCoreMaxFreq);
                f += buf;
            }
            f += " MHz";
        }
        if (data.eCoreFreq > 0) {
            snprintf(buf, sizeof(buf), "   E %.0f", data.eCoreFreq);
            f += buf;
            if (data.eCoreMaxFreq > 0) {
                snprintf(buf, sizeof(buf), "/%.0f", data.eCoreMaxFreq);
                f += buf;
            }
            f += " MHz";
        }
        emit(f);
    }
    if (data.cpuBaseFreq > 0) {
        snprintf(buf, sizeof(buf), "Base: %.0f MHz", data.cpuBaseFreq);
        emit(buf);
    }
#if !defined(TCMT_MACOS)
    // macOS cluster "temperatures" come from SMC sensor names that do not map
    // reliably to CPU clusters, so this overlay keeps them out on that
    // platform instead of printing a number that invites the wrong reading.
    {
        std::string t = "Temp:";
        if (data.cpuTemp > 0) {
            snprintf(buf, sizeof(buf), " CPU %.1f C", data.cpuTemp);
            t += buf;
        }
        if (data.cpuPcoreTemp > 0) {
            snprintf(buf, sizeof(buf), "   P %.1f C", data.cpuPcoreTemp);
            t += buf;
        }
        if (data.cpuEcoreTemp > 0) {
            snprintf(buf, sizeof(buf), "   E %.1f C", data.cpuEcoreTemp);
            t += buf;
        }
        if (t != "Temp:") emit(t);
    }
#endif

    if (count <= 0) {
        emit("");
        emit(Tr("cpu.no_percore"));
    } else {
        y++;   // blank line between the summary and the chart
        const int tableY = y;
        // Core label (P0.. / E0.. when the platform's numbering is known),
        // a usage bar, then the percentage and the core's frequency.
        // Per-core die temperature, when the platform exposes it (Windows MSR,
        // Linux hwmon). Reserved only if there is something to show.
        bool anyPerCoreTemp = false;
        for (int i = 0; i < count; ++i) {
            if (data.perCoreTemp[i] > 0) { anyPerCoreTemp = true; break; }
        }
        const int barX = x0 + 8;
        const int barW = (std::max)(10, W - 24 - (anyPerCoreTemp ? 6 : 0));
        // Windows reports the type of each physical core, and hybrid parts
        // interleave them; macOS only guarantees P cores first, so that case
        // still numbers by index.
        bool anyECore = false;
        for (int i = 0; i < count; ++i) {
            if (data.perCoreIsE[i]) { anyECore = true; break; }
        }
        std::vector<std::string> coreLabels(static_cast<size_t>(count));
        {
            int p = 0, e = 0;
            for (int i = 0; i < count; ++i) {
                char lb[8];
                if (data.perCorePEKnown && anyECore) {
                    if (data.perCoreIsE[i]) snprintf(lb, sizeof(lb), "E%d", e++);
                    else snprintf(lb, sizeof(lb), "P%d", p++);
                } else if (data.perCorePEKnown && data.performanceCores > 0) {
                    if (i < data.performanceCores) snprintf(lb, sizeof(lb), "P%d", i);
                    else snprintf(lb, sizeof(lb), "E%d", i - data.performanceCores);
                } else {
                    snprintf(lb, sizeof(lb), "C%02d", i);
                }
                coreLabels[i] = lb;
            }
        }

        mvwprintw(stdscr, tableY, x0 + 3, "%s", "Core   Usage");
        for (int i = 0; i < count; ++i) {
            const int ty = tableY + 1 + i;
            if (ty >= y0 + H - 2) break;

            mvwprintw(stdscr, ty, x0 + 3, "%-4s", coreLabels[i].c_str());

            const double rawPct = static_cast<double>(data.perCoreUsage[i]);
            const bool usageKnown = rawPct >= 0.0;
            double pct = usageKnown ? rawPct : 0.0;
            if (pct > 100) pct = 100;
            const int nFill = static_cast<int>(pct * barW / 100.0);
            const int sev = HighIsWorsePair(pct, kUsageWarn, kUsageCrit);
            int bx = barX;
            if (sev >= 0) wattron(stdscr, COLOR_PAIR(sev));
            for (int k = 0; k < nFill; ++k) mvwaddstr(stdscr, ty, bx++, barFill_.c_str());
            if (sev >= 0) wattroff(stdscr, COLOR_PAIR(sev));
            wattron(stdscr, A_DIM);
            for (; bx < barX + barW; ++bx) mvwaddstr(stdscr, ty, bx, barTrack_.c_str());
            wattroff(stdscr, A_DIM);

            char val[48];
            if (!usageKnown) {
                if (data.perCoreFreq[i] > 0)
                    snprintf(val, sizeof(val), "    --  %5.0fM",
                             static_cast<double>(data.perCoreFreq[i]));
                else
                    snprintf(val, sizeof(val), "    --      -");
            } else if (data.perCoreFreq[i] > 0) {
                snprintf(val, sizeof(val), " %5.1f%% %5.0fM", pct,
                         static_cast<double>(data.perCoreFreq[i]));
            } else {
                snprintf(val, sizeof(val), " %5.1f%%     -", pct);
            }
            if (anyPerCoreTemp) {
                char tb[12];
                if (data.perCoreTemp[i] > 0)
                    snprintf(tb, sizeof(tb), " %3.0fC", static_cast<double>(data.perCoreTemp[i]));
                else
                    snprintf(tb, sizeof(tb), "    -");
                strncat(val, tb, sizeof(val) - strlen(val) - 1);
            }
            mvwprintw(stdscr, ty, barX + barW, "%s", val);
        }
    }

    mvwprintw(stdscr, y0 + H - 2, x0 + 3, "%s",
              utf8_truncate(Tr("details.close"), W - 6).c_str());
}

// GPU detail overlay — the same gestures as the CPU one. macOS adds the GPU
// core count and has no VRAM reading at all: on Apple Silicon the memory
// figure would be a slice of system RAM, so it stays off the screen instead
// of being labelled "VRAM".
void TuiApp::RenderGpuDetails(int rows, int cols, int ch) {
    TuiData data;
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        data = data_;
    }

    // Left / Right walk the GPU list. The index lives on the TUI side because
    // TuiData is rebuilt by the producer every frame.
    if (ch == KEY_LEFT || ch == KEY_RIGHT) {
        const int n = static_cast<int>(data.gpus.size());
        if (n > 1) {
            gpuIndex_ = (GpuIndexFor(data) + ((ch == KEY_RIGHT) ? 1 : -1) + n) % n;
        }
    }

    const bool isEnter = (ch == '\n' || ch == '\r'
#ifdef KEY_ENTER
        || ch == KEY_ENTER
#endif
    );
    if (ch == 27 || ch == 'q' || ch == 'Q' || ch == ' ' || isEnter) {
        gpuPage_ = false;
        clear();
        touchwin(stdscr);   // wide-glyph halves survive clear() alone on PDCurses
        return;
    }

    const int W = cols;
    const int H = rows;
    const int x0 = 0;
    const int y0 = 0;

    erase();
    DrawOverlayFrame(y0, x0, W, H);
    mvwprintw(stdscr, y0, x0 + 3, " GPU Details ");

    int y = y0 + 2;
    auto emit = [&](const std::string& s) {
        if (y >= y0 + H - 2) return;
        mvwprintw(stdscr, y++, x0 + 3, "%s", utf8_truncate(s, W - 6).c_str());
    };
    char buf[128];

    const TuiData::GpuEntry* g = CurrentGpu(data);
    const bool multi = data.gpus.size() > 1;
    if (multi) {
        snprintf(buf, sizeof(buf), "GPU %d/%d", GpuIndexFor(data) + 1,
                 static_cast<int>(data.gpus.size()));
        emit(buf);
    }

    const std::string name = g ? g->name : data.gpuName;
    const bool isNvidia = name.find("NVIDIA") != std::string::npos;
    if (!name.empty()) emit("Name: " + name);
    if (g && !g->driver.empty()) {
        // nvidia-smi calls this number KMD Version; same string, and only
        // spelled out for NVIDIA cards.
        emit((isNvidia && !g->cudaVersion.empty() ? "Driver (KMD): " : "Driver: ")
             + g->driver);
    }
    if (g && !g->cudaVersion.empty()) emit("CUDA UMD: " + g->cudaVersion);
    const int coreCount = g ? g->coreCount : data.gpuCoreCount;
    if (coreCount > 0) {
        // NVIDIA calls them CUDA cores; "GPU cores" is only right for the
        // Apple/AMD/Intel side.
        snprintf(buf, sizeof(buf), "%s: %d",
                 isNvidia ? "CUDA cores" : "GPU cores", coreCount);
        emit(buf);
    }

    const double usage = g ? g->usage : data.gpuUsage;
    {
        char val[32];
        snprintf(val, sizeof(val), "%.1f%%", usage);
        DrawUsageBarRow(stdscr, y, x0 + 1, W - 2, "Use:", usage, val);
        y++;
    }

    // Per-card readings first; the aggregate scalars are the fallback so a
    // single-GPU machine still shows temperature/power even when the backend
    // only has the system-level value.
    const double tempC = (g && g->temperature > 0) ? g->temperature
                        : (multi ? 0.0 : data.gpuTemp);
    if (tempC > 0.0) {
        snprintf(buf, sizeof(buf), "Temp: %.0f C", tempC);
        emit(buf);
    }

    const double curClock = (g && g->clockMhz > 0) ? g->clockMhz : data.gpuFreq;
    const int maxClock = (g && g->maxClock > 0)
        ? g->maxClock : static_cast<int>(data.gpuMaxFreq);
    if (curClock > 0) {
        if (maxClock > 0 && maxClock != static_cast<int>(curClock)) {
            snprintf(buf, sizeof(buf), "Freq: %d/%d MHz",
                     static_cast<int>(curClock), maxClock);
        } else {
            snprintf(buf, sizeof(buf), "Freq: %d MHz", static_cast<int>(curClock));
        }
        emit(buf);
    }
    if (g && g->memClockMhz > 0) {
        snprintf(buf, sizeof(buf), "Mem clk: %d MHz", g->memClockMhz);
        emit(buf);
    }

    // A card that reports 0 W while power-gated is a valid reading; only a
    // failed query keeps -1 and falls back to the system-level value.
    double powerMw = (g && g->powerMw >= 0) ? g->powerMw : -1.0;
    if (powerMw < 0 && !multi && data.gpuPower > 0) powerMw = data.gpuPower;
    const bool haveLimit = g && g->powerLimitMw > 0.0;
    if (powerMw >= 0) {
        if (haveLimit) {
            snprintf(buf, sizeof(buf), "Power: %.1f / %.0f W",
                     powerMw / 1000.0, g->powerLimitMw / 1000.0);
        } else {
            snprintf(buf, sizeof(buf), "Power: %.1f W", powerMw / 1000.0);
        }
        emit(buf);
    } else if (haveLimit) {
        // The card exposes a limit but no draw sensor (common on laptop
        // dGPUs); nvidia-smi prints N/A for the same reason.
        snprintf(buf, sizeof(buf), "Power: N/A / %.0f W", g->powerLimitMw / 1000.0);
        emit(buf);
    }

    if (g && g->fanPercent >= 0) {
        if (g->fanCount > 1) {
            snprintf(buf, sizeof(buf), "Fan: %d%%  (%d fans)", g->fanPercent, g->fanCount);
        } else {
            snprintf(buf, sizeof(buf), "Fan: %d%%", g->fanPercent);
        }
        emit(buf);
    }

    // Discrete GPUs only. Under unified memory this figure is system RAM and
    // reads as VRAM, so it is not shown at all.
    const bool unified = g ? g->unified : data.gpuUnified;
    const uint64_t mem = g ? g->memory : data.gpuMemory;
    const double memPct = g ? g->memoryPercent : data.gpuMemoryPercent;
    if (!unified && mem > 0 && memPct > 1) {
        const uint64_t used = static_cast<uint64_t>(mem * memPct / 100.0);
        emit("VRAM: " + FormatSize(used) + " / " + FormatSize(mem));
    }

    if (g && g->pcieGen > 0 && g->pcieWidth > 0) {
        // The negotiated link drops to x1/x2 while the card idles, so the
        // capability is shown next to it; "Gen4 x2" alone reads like a fault.
        if (g->maxPcieWidth > g->pcieWidth || g->maxPcieGen > g->pcieGen) {
            snprintf(buf, sizeof(buf), "PCIe: Gen%u x%u  (max Gen%u x%u)",
                     g->pcieGen, g->pcieWidth,
                     g->maxPcieGen > 0 ? g->maxPcieGen : g->pcieGen,
                     g->maxPcieWidth);
        } else {
            snprintf(buf, sizeof(buf), "PCIe: Gen%u x%u", g->pcieGen, g->pcieWidth);
        }
        emit(buf);
    }
    if (g && !g->serial.empty()) emit("Serial: " + g->serial);
    if (g && !g->vbios.empty()) emit("VBIOS: " + g->vbios);

    {
        std::string hint = Tr("details.close");
        if (multi) { hint += "   "; hint += Tr("details.gpu_switch"); }
        mvwprintw(stdscr, y0 + H - 2, x0 + 3, "%s",
                  utf8_truncate(hint, W - 6).c_str());
    }
}

// Disk detail overlay — Space on the Disks panel, or a double-click on it.
// Volumes get a usage bar, free space and mount point; physical devices add
// health/thermal numbers and their SMART attributes where the platform
// exposes them. Content is clipped to the frame, so a machine with many
// volumes shows the first ones rather than overflowing.
void TuiApp::RenderDiskDetails(int rows, int cols, int ch) {
    const bool isEnter = (ch == '\n' || ch == '\r'
#ifdef KEY_ENTER
        || ch == KEY_ENTER
#endif
    );
    if (ch == 27 || ch == 'q' || ch == 'Q' || ch == ' ' || isEnter) {
        diskPage_ = false;
        clear();
        touchwin(stdscr);   // wide-glyph halves survive clear() alone on PDCurses
        return;
    }

    TuiData data;
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        data = data_;
    }

    // Edge to edge, like the main frame: model names and the SMART grid are
    // the content, and any margin here just hides columns that already fit.
    const int W = cols;
    const int H = rows;
    const int x0 = 0;
    const int y0 = 0;

    erase();
    DrawOverlayFrame(y0, x0, W, H);
    mvwprintw(stdscr, y0, x0 + 3, " Disk Details ");

    int y = y0 + 2;
    const int bottom = y0 + H - 3;   // last usable content row (hint below it)

    auto section = [&](const char* title) {
        if (y >= bottom) return;
        wattron(stdscr, COLOR_PAIR(5) | A_BOLD);
        mvwprintw(stdscr, y++, x0 + 3, "%s", title);
        wattroff(stdscr, COLOR_PAIR(5) | A_BOLD);
    };

    if (!data.disks.empty()) {
        section("Volumes");
        for (const auto& d : data.disks) {
            if (y + 1 >= bottom) break;
            const double upct =
                d.totalSize > 0 ? 100.0 * d.usedSpace / d.totalSize : 0.0;
            const std::string used = FormatSize(d.usedSpace);
            const std::string total = FormatSize(d.totalSize);
            const std::string free =
                FormatSize(d.totalSize > d.usedSpace ? d.totalSize - d.usedSpace : 0);
            const std::string label = d.label.empty() ? d.mountPoint : d.label;
            char head[256];
            snprintf(head, sizeof(head), "%s   %s   %.0f%%   %s / %s   free %s",
                     label.c_str(), d.fileSystem.c_str(), upct,
                     used.c_str(), total.c_str(), free.c_str());
            mvwprintw(stdscr, y++, x0 + 3, "%s", utf8_truncate(head, W - 6).c_str());

            const int barW = 24;
            double p = upct < 0 ? 0 : (upct > 100 ? 100 : upct);
            const int nFill = static_cast<int>(p * barW / 100.0);
            const int sev = HighIsWorsePair(p, kUsageWarn, kUsageCrit);
            int bx = x0 + 3;
            if (sev >= 0) wattron(stdscr, COLOR_PAIR(sev));
            for (int k = 0; k < nFill; ++k) mvwaddstr(stdscr, y, bx++, barFill_.c_str());
            if (sev >= 0) wattroff(stdscr, COLOR_PAIR(sev));
            wattron(stdscr, A_DIM);
            for (; bx < x0 + 3 + barW; ++bx) mvwaddstr(stdscr, y, bx, barTrack_.c_str());
            mvwprintw(stdscr, y, x0 + 3 + barW + 2, "%s",
                      utf8_truncate(d.mountPoint, W - 6 - barW - 2).c_str());
            wattroff(stdscr, A_DIM);
            y++;
        }
        if (y < bottom) y++;
    }

    if (!data.physicalDisks.empty()) {
        section("Physical disks");
        for (size_t di = 0; di < data.physicalDisks.size(); ++di) {
            const auto& pd = data.physicalDisks[di];
            if (y + 1 >= bottom) break;
            std::string head = pd.model;
            if (!pd.diskType.empty()) head += "  " + pd.diskType;
            if (pd.capacity > 0) head += "  " + FormatSize(pd.capacity);
            mvwprintw(stdscr, y++, x0 + 3, "%s", utf8_truncate(head, W - 6).c_str());

            std::string stats;
            char buf[128];
            if (pd.smartSupported && pd.healthPct > 0) {
                snprintf(buf, sizeof(buf), "Health %d%%", static_cast<int>(pd.healthPct));
                stats += buf;
            } else {
                stats += "Health N/A";
            }
            if (pd.temperature > 0) {
                snprintf(buf, sizeof(buf), "   Temp %.0f C", pd.temperature);
                stats += buf;
            }
            if (pd.powerOnHours > 0) {
                snprintf(buf, sizeof(buf), "   Power-on %lluh",
                         static_cast<unsigned long long>(pd.powerOnHours));
                stats += buf;
            }
            if (pd.wearLeveling > 0 && pd.wearLeveling <= 1.0) {
                snprintf(buf, sizeof(buf), "   Wear %.0f%%", pd.wearLeveling * 100.0);
                stats += buf;
            }
            const int hc = pd.smartSupported
                ? LowIsWorsePair(static_cast<int>(pd.healthPct), kHealthWarn, kHealthCrit)
                : -1;
            if (hc >= 0) wattron(stdscr, COLOR_PAIR(hc));
            mvwprintw(stdscr, y++, x0 + 3, "%s", utf8_truncate(stats, W - 6).c_str());
            if (hc >= 0) wattroff(stdscr, COLOR_PAIR(hc));

            std::string meta;
            if (!pd.serial.empty()) meta += "Serial " + pd.serial;
            if (!pd.interfaceType.empty()) {
                if (!meta.empty()) meta += "   ";
                meta += pd.interfaceType;
            }
            if (!meta.empty() && y < bottom) {
                wattron(stdscr, A_DIM);
                mvwprintw(stdscr, y++, x0 + 3, "%s", utf8_truncate(meta, W - 6).c_str());
                wattroff(stdscr, A_DIM);
            }

            // Live throughput, one line. The graphs themselves are drawn once
            // for all disks at the bottom of the page.
            if (y < bottom) {
                char rateLine[96];
                snprintf(rateLine, sizeof(rateLine), "Read %s   Write %s",
                         FormatRate(pd.readBps).c_str(), FormatRate(pd.writeBps).c_str());
                mvwprintw(stdscr, y++, x0 + 3, "%s", rateLine);
            }

            wattron(stdscr, A_DIM);
            // Attribute grid: as many columns as the width allows. Real drives
            // report 20-30 attributes, so a full-width terminal fits three or
            // four and the grid stops being the thing that clips.
            const int nameW = 24;
            const int attrW = nameW + 2 + 7 + 2 + 8;   // name, cur/wor, raw
            const int gridCols = (std::max)(1, (std::min)(4, (W - 6 + 2) / (attrW + 2)));
            const int cellW = gridCols > 1
                ? (W - 6 - 2 * (gridCols - 1)) / gridCols
                : (W - 6);

            // Every field goes to an absolute column. Attribute names are
            // Chinese on most drives: one glyph is two cells but three bytes,
            // so a single composed string drifts and every value after the
            // name lands off its column.
            const int rightEdge = x0 + W - 3;   // last usable content column
            auto cellX = [&](int c) { return x0 + 3 + c * (cellW + 2); };

            if (y < bottom) {
                for (int c = 0; c < gridCols; ++c) {
                    const int cx = cellX(c);
                    if (cx + cellW > rightEdge) break;
                    mvwprintw(stdscr, y, cx, "%s", "Attribute");
                    snprintf(buf, sizeof(buf), "  %3s/%3s  %8s", "Cur", "Wor", "Raw");
                    mvwprintw(stdscr, y, cx + nameW, "%s", buf);
                }
                y++;
            }
            const int n = static_cast<int>(pd.attributes.size());
            const int perCol = (n + gridCols - 1) / gridCols;
            for (int i = 0; i < perCol; ++i) {
                if (y >= bottom) break;
                for (int c = 0; c < gridCols; ++c) {
                    const int idx = i + c * perCol;
                    if (idx >= n) break;
                    const int cx = cellX(c);
                    if (cx + cellW > rightEdge) break;
                    const auto& a = pd.attributes[static_cast<size_t>(idx)];
                    mvwprintw(stdscr, y, cx, "%s",
                              utf8_truncate(a.name, nameW).c_str());
                    snprintf(buf, sizeof(buf), "  %3u/%3u  %8llu",
                             a.current, a.worst,
                             static_cast<unsigned long long>(a.rawValue));
                    mvwprintw(stdscr, y, cx + nameW, "%s", buf);
                }
                y++;
            }
            wattroff(stdscr, A_DIM);
            if (y < bottom) y++;
        }
    }

    // Throughput: the network page's chart, drawn once for every disk, read
    // above write. Only when the SMART grids left enough height — the charts
    // are the part that gives way first.
    if (!data.physicalDisks.empty() && data.diskHistory.size() >= data.physicalDisks.size()) {
        const int nDisk = (std::min)(static_cast<int>(data.physicalDisks.size()), 3);
        bool anyHistory = false;
        for (int k = 0; k < nDisk; ++k) {
            if (data.diskHistory[static_cast<size_t>(k)].len >= 2) { anyHistory = true; break; }
        }
        if (anyHistory) {
            section("Throughput");
            // Per column: the disk name, then two charts of header + rows +
            // time axis.
            const int chartRows = (bottom - y - 1 - 4) / 2;
            if (chartRows >= 2) {
                // No wider than the history it holds, or the columns wrap
                // around and the chart repeats old samples to fill the space.
                int maxLen = 1;
                for (int k = 0; k < nDisk; ++k)
                    maxLen = (std::max)(maxLen, data.diskHistory[static_cast<size_t>(k)].len);
                const int perCol = (std::max)(20, (std::min)((W - 6) / nDisk, maxLen + 10));
                const double colSec =
                    (data.sampleIntervalMs > 0 ? data.sampleIntervalMs : 500) / 1000.0;
                for (int k = 0; k < nDisk; ++k) {
                    const auto& h = data.diskHistory[static_cast<size_t>(k)];
                    if (h.len < 2) continue;
                    const int cx = x0 + 3 + k * perCol;
                    mvwprintw(stdscr, y, cx, "%s",
                              utf8_truncate(data.physicalDisks[static_cast<size_t>(k)].model,
                                            perCol - 2).c_str());
                    const int last = (h.pos - 1 + TuiData::NET_HISTORY_MAX)
                                   % TuiData::NET_HISTORY_MAX;
                    int cy = DrawRateChart(cx, y + 1, perCol - 1, chartRows, bottom, "Read",
                                           h.rd, h.pos, h.rd[last],
                                           PeakOf(h.rd, h.pos, h.len), 6, colSec);
                    DrawRateChart(cx, cy, perCol - 1, chartRows, bottom, "Write",
                                  h.wr, h.pos, h.wr[last],
                                  PeakOf(h.wr, h.pos, h.len), 2, colSec);
                }
            }
        }
    }

    if (data.disks.empty() && data.physicalDisks.empty()) {
        mvwprintw(stdscr, y0 + 2, x0 + 3, "%s",
                  utf8_truncate(Tr("disk.none"), W - 6).c_str());
    }

    mvwprintw(stdscr, y0 + H - 2, x0 + 3, "%s",
              utf8_truncate(Tr("details.close"), W - 6).c_str());
}

// Battery detail overlay — Space on the Power panel (the panel that owns the
// battery rows), or a double-click on it. Everything shown comes from
// AppleSmartBattery / IOPS and needs no privileges. The charger block only
// appears while an adapter is connected.
void TuiApp::RenderBatteryDetails(int rows, int cols, int ch) {
    const bool isEnter = (ch == '\n' || ch == '\r'
#ifdef KEY_ENTER
        || ch == KEY_ENTER
#endif
    );
    if (ch == 27 || ch == 'q' || ch == 'Q' || ch == ' ' || isEnter) {
        batteryPage_ = false;
        clear();
        touchwin(stdscr);   // wide-glyph halves survive clear() alone on PDCurses
        return;
    }

    TuiData data;
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        data = data_;
    }

    const int W = cols;
    const int H = rows;
    const int x0 = 0;
    const int y0 = 0;

    erase();
    DrawOverlayFrame(y0, x0, W, H);
    mvwprintw(stdscr, y0, x0 + 3, " Battery Details ");

    int y = y0 + 2;
    const int bottom = y0 + H - 3;   // last usable content row (hint below it)
    auto emit = [&](const std::string& s) {
        if (y >= bottom) return;
        mvwprintw(stdscr, y++, x0 + 3, "%s", utf8_truncate(s, W - 6).c_str());
    };
    char buf[160];

    if (data.batteryPercent < 0) {
        emit(Tr("battery.none"));
    } else {
        std::string state = data.batteryIsCharging ? "Charging"
                          : (data.batteryAmperage < 0 ? "Discharging" : "Idle");
        if (data.batteryFullyCharged) state = "Fully charged";
        emit("State: " + state + (data.acOnline ? "   (AC connected)" : "   (on battery)"));

        // Charge bar: unlike the usage bars, a low charge is the bad end, so
        // the severity colors come from kBattWarn / kBattCrit.
        {
            if (y < bottom) {
                mvwprintw(stdscr, y, x0 + 3, "%s", "Charge:");
                const int barX = x0 + 12;
                const int barW = (std::max)(8, (std::min)(30, W - 26));
                int pct = data.batteryPercent;
                if (pct < 0) pct = 0;
                if (pct > 100) pct = 100;
                const int nFill = pct * barW / 100;
                const int col = LowIsWorsePair(pct, kBattWarn, kBattCrit);
                int bx = barX;
                if (col >= 0) wattron(stdscr, COLOR_PAIR(col));
                for (int k = 0; k < nFill; ++k) mvwaddstr(stdscr, y, bx++, barFill_.c_str());
                if (col >= 0) wattroff(stdscr, COLOR_PAIR(col));
                wattron(stdscr, A_DIM);
                for (; bx < barX + barW; ++bx) mvwaddstr(stdscr, y, bx, barTrack_.c_str());
                wattroff(stdscr, A_DIM);
                snprintf(buf, sizeof(buf), "  %d%%", pct);
                mvwprintw(stdscr, y, barX + barW, "%s", buf);
                y++;
            }
        }

        if (data.batteryAmperage != 0 && data.batteryVoltage > 0) {
            const double watts = std::abs(data.batteryAmperage) *
                                 static_cast<double>(data.batteryVoltage) / 1e6;
            snprintf(buf, sizeof(buf), "Power: %s %.2f W   (%.2f V / %.2f A)",
                     data.batteryAmperage > 0 ? "Chg" : "Dchg", watts,
                     data.batteryVoltage / 1000.0,
                     std::abs(data.batteryAmperage) / 1000.0);
            emit(buf);
        }

        if (data.batteryTimeRemainingMin > 0) {
            snprintf(buf, sizeof(buf), "Time: %d:%02d %s",
                     data.batteryTimeRemainingMin / 60,
                     data.batteryTimeRemainingMin % 60,
                     data.batteryIsCharging ? "to full" : "remaining");
            emit(buf);
        }

        {
            const int hp = static_cast<int>(data.batteryHealthPercent + 0.5);
            snprintf(buf, sizeof(buf), "Health: %d%%   Cycles: %d",
                     hp, data.batteryCycleCount);
            const int hc = LowIsWorsePair(hp, kHealthWarn, kHealthCrit);
            if (hc >= 0) wattron(stdscr, COLOR_PAIR(hc));
            emit(buf);
            if (hc >= 0) wattroff(stdscr, COLOR_PAIR(hc));
        }

        if (data.batteryDesignCapacity > 0 || data.batteryMaxCapacity > 0) {
            snprintf(buf, sizeof(buf), "Capacity: %d / %d mAh",
                     data.batteryMaxCapacity, data.batteryDesignCapacity);
            emit(buf);
        }
        if (data.batteryTemp > 0) {
            snprintf(buf, sizeof(buf), "Temp: %.1f C", data.batteryTemp);
            emit(buf);
        }

        if (!data.chargerName.empty() || data.chargerWatts > 0) {
            if (y < bottom) y++;
            if (y < bottom) {
                wattron(stdscr, COLOR_PAIR(5) | A_BOLD);
                mvwprintw(stdscr, y++, x0 + 3, "%s", "Charger");
                wattroff(stdscr, COLOR_PAIR(5) | A_BOLD);
            }
            if (!data.chargerName.empty()) {
                emit("Name: " + data.chargerName +
                     (data.chargerWireless ? "   (wireless)" : ""));
            }
            if (data.chargerWatts > 0 || data.chargerVoltageMv > 0 ||
                data.chargerCurrentMa > 0) {
                snprintf(buf, sizeof(buf), "Output: %.0f W   %.1f V / %.2f A",
                         data.chargerWatts, data.chargerVoltageMv / 1000.0,
                         data.chargerCurrentMa / 1000.0);
                emit(buf);
            }
            std::string make;
            if (!data.chargerManufacturer.empty()) make = data.chargerManufacturer;
            if (!data.chargerModel.empty()) {
                if (!make.empty()) make += "   ";
                make += "Model " + data.chargerModel;
            }
            if (!make.empty()) emit("Adapter: " + make);
            if (!data.chargerSerial.empty()) emit("Serial: " + data.chargerSerial);
        }
    }

    mvwprintw(stdscr, y0 + H - 2, x0 + 3, "%s",
              utf8_truncate(Tr("details.close"), W - 6).c_str());
}

// One direction's rate chart: a coloured header carrying the current value, a
// value axis, block columns (8 levels per cell) and a time axis. The network
// and disk pages both call this, so the two read the same; the disk charts are
// simply given fewer rows. Returns the row after the time axis.
int TuiApp::DrawRateChart(int cx, int cy, int colW, int rows, int bottom,
                          const char* label, const uint64_t* hist, int pos,
                          uint64_t now, uint64_t scale, int pair, double colSec) {
    if (cy >= bottom) return cy;
    char head[96];
    snprintf(head, sizeof(head), "%s  %s", label, FormatRate(now).c_str());
    wattron(stdscr, COLOR_PAIR(pair));
    mvwprintw(stdscr, cy, cx, "%s", head);
    cy++;

    // Value axis: the scale's top value on the first row and the unit on the
    // last, so the bars read without the header line. The column has to fit
    // the widest label: a fixed nine columns clipped "560.3 KB/s".
    const char* unit = (scale >= 1000ull * 1000) ? "MB/s"
                     : (scale >= 1000ull ? "KB/s" : "B/s");
    const std::string topLabel = FormatRate(scale);
    const std::string zeroLabel = std::string("0 ") + unit;
    const int axisW = (std::max)(9, (std::max)(static_cast<int>(topLabel.size()),
                                               static_cast<int>(zeroLabel.size())));
    const int barX = cx + axisW + 1;
    const int graphW = (std::max)(4, colW - axisW - 1);

    const int levelsPerRow = 8;
    for (int r = 0; r < rows && cy < bottom; ++r, ++cy) {
        if (r == 0) {
            wattron(stdscr, A_DIM);
            mvwprintw(stdscr, cy, cx, "%*s", axisW, topLabel.c_str());
            wattroff(stdscr, A_DIM);
        } else if (r == rows - 1) {
            wattron(stdscr, A_DIM);
            mvwprintw(stdscr, cy, cx, "%*s", axisW, zeroLabel.c_str());
            wattroff(stdscr, A_DIM);
        }

        // Left boundary: one column of '|' from the peak down to zero, so the
        // bar area reads as a chart and not as floating blocks.
        wattron(stdscr, A_DIM);
        mvwaddstr(stdscr, cy, barX - 1, "|");
        wattroff(stdscr, A_DIM);

        // Each cell holds 8 sublevels; the row's band from the bottom is
        // (rows-1-r).
        const uint64_t band = static_cast<uint64_t>(rows - 1 - r) * levelsPerRow;
        std::string row;
        row.reserve(static_cast<size_t>(graphW) * 3);
        for (int c = 0; c < graphW; ++c) {
            const int idx = (pos - graphW + c
                             + TuiData::NET_HISTORY_MAX * 2) % TuiData::NET_HISTORY_MAX;
            // Total fill in sublevels (0 .. rows*8).
            const int total = static_cast<int>(hist[idx] * rows * levelsPerRow / scale);
            int cell = total - static_cast<int>(band);
            if (cell > levelsPerRow) cell = levelsPerRow;
            if (cell < 0) cell = 0;
            row += BlockGlyph(cell);
        }
        mvwprintw(stdscr, cy, barX, "%s", row.c_str());
    }
    wattroff(stdscr, COLOR_PAIR(pair));

    // Time axis: one column is one history sample, appended once per heavy
    // frame (~the sample interval). Rightmost column is "now".
    if (cy < bottom) {
        const int ticks = graphW >= 30 ? 4 : 2;
        wattron(stdscr, A_DIM);
        for (int t = 0; t < ticks; ++t) {
            const int c = (graphW - 1) - t * (graphW - 1) / (ticks - 1);
            const int secs = static_cast<int>((graphW - 1 - c) * colSec + 0.5);
            char lb[16];
            if (secs == 0) snprintf(lb, sizeof(lb), "now");
            else snprintf(lb, sizeof(lb), "-%ds", secs);
            int lx = barX + c;
            const int maxX = barX + graphW - static_cast<int>(std::strlen(lb));
            if (lx > maxX) lx = maxX;
            if (lx < barX) lx = barX;
            mvwprintw(stdscr, cy, lx, "%s", lb);
        }
        wattroff(stdscr, A_DIM);
        cy++;
    }
    return cy;
}

// Network detail overlay — Space on the Network panel, or a double-click on
// it. Holds what the dashboard panels have no room for: every adapter with
// its addresses and rates, the traffic history as a wide sparkline, the WiFi
// state including the wireless chip, and the Bluetooth controller with its
// device list. The same facts used to be spread across the Network, Traffic
// and WiFi/BT panels.
void TuiApp::RenderNetworkDetails(int rows, int cols, int ch) {
    const bool isEnter = (ch == '\n' || ch == '\r'
#ifdef KEY_ENTER
        || ch == KEY_ENTER
#endif
    );
    if (ch == 27 || ch == 'q' || ch == 'Q' || ch == ' ' || isEnter) {
        networkPage_ = false;
        clear();
        touchwin(stdscr);   // wide-glyph halves survive clear() alone on PDCurses
        return;
    }

    TuiData data;
    {
        std::lock_guard<std::mutex> lock(dataMutex_);
        data = data_;
    }

    // More than one connected interface gets one chart column each, so the
    // box widens with the column count instead of squeezing every plot into
    // 82 columns. It used to plot adapter 0 only, which on a wired + wireless
    // machine meant the other NIC never had a chart at all.
    int trafficCols = 0;
    if (data.adapterHistory.size() == data.adapters.size()) {
        for (const auto& a : data.adapters) {
            if ((!a.ip.empty() || a.speed > 0) && ++trafficCols >= 3) break;
        }
    }
    if (trafficCols < 2) trafficCols = 0;
    // Edge to edge: the per-interface charts divide W - 6 between them, so a
    // wider terminal widens every chart instead of leaving a margin.
    const int W = cols;
    const int H = rows;
    const int x0 = 0;
    const int y0 = 0;

    erase();
    DrawOverlayFrame(y0, x0, W, H);
    mvwprintw(stdscr, y0, x0 + 3, " Network Details ");

    int y = y0 + 2;
    const int bottom = y0 + H - 3;
    auto emit = [&](const std::string& s) {
        if (y >= bottom) return;
        mvwprintw(stdscr, y++, x0 + 3, "%s", utf8_truncate(s, W - 6).c_str());
    };
    auto emitDim = [&](const std::string& s) {
        if (y >= bottom) return;
        wattron(stdscr, A_DIM);
        mvwprintw(stdscr, y++, x0 + 3, "%s", utf8_truncate(s, W - 6).c_str());
        wattroff(stdscr, A_DIM);
    };
    auto section = [&](const char* title) {
        if (y >= bottom) return;
        wattron(stdscr, COLOR_PAIR(5) | A_BOLD);
        mvwprintw(stdscr, y++, x0 + 3, "%s", title);
        wattroff(stdscr, COLOR_PAIR(5) | A_BOLD);
    };
    char buf[192];

    if (!data.adapters.empty()) {
        section("Adapters");
        for (const auto& a : data.adapters) {
            if (y + 1 >= bottom) break;
            std::string head = a.name;
            if (!a.type.empty()) head += "  " + a.type;
            if (a.speed > 0) head += "  " + FormatSpeed(a.speed);
            if (!a.ip.empty()) head += "  " + a.ip;
            emit(head);
            std::string meta;
            if (!a.mac.empty()) meta += "hw " + a.mac;
            if (!meta.empty()) meta += "   ";
            meta += "D " + FormatRate(a.downloadSpeed) + "   U " + FormatRate(a.uploadSpeed);
            emitDim(meta);
        }
        if (y < bottom) y++;
    }

    if (data.dlHistoryLen >= 2) {
        section("Traffic");
        // Two stacked charts share the height left after reserving room for
        // the WiFi/Bluetooth sections below; the cap keeps a tall terminal
        // from turning this into a full-screen graph.
        const int remaining = bottom - y;
        // Reserve the fixed lines below: two chart labels, the peak line, a
        // trailing blank, then the WiFi and Bluetooth sections. What is left
        // splits evenly between the two charts.
        const int wifiReserve = data.hasWiFi ? 7 : 0;
        const int btReserve = data.hasBluetooth ? 5 : 0;
        // A per-interface layout spends one more line on the adapter name.
        int chartRows = (remaining - 6 - wifiReserve - btReserve - (trafficCols > 0 ? 2 : 0)) / 2;
        if (chartRows < 2) chartRows = 2;

        // One D/U pair. cx/colW place it, so the same code draws the single
        // tall chart and the per-interface columns. Returns the row after the
        // pair's time axis.
        auto drawChart = [&](int cx, int cy, int colW, const char* label,
                             const uint64_t* hist, int pos,
                             uint64_t now, uint64_t scale, int pair) -> int {
            return DrawRateChart(cx, cy, colW, chartRows, bottom, label, hist, pos,
                                 now, scale, pair,
                                 (data.sampleIntervalMs > 0 ? data.sampleIntervalMs : 500) / 1000.0);
        };

        // Each direction gets its own scale. A shared one made the upload
        // chart invisible whenever the download side had a single spike.
        auto peakOf = [](const uint64_t* hist, int pos, int len) {
            return PeakOf(hist, pos, len);
        };

        if (trafficCols >= 2) {
            // One column per connected interface. Each keeps its own history
            // and scale, so a busy link cannot flatten the plot beside it.
            std::vector<int> cols;
            for (size_t i = 0; i < data.adapters.size() && cols.size() < 3; ++i) {
                if (!data.adapters[i].ip.empty() || data.adapters[i].speed > 0)
                    cols.push_back(static_cast<int>(i));
            }
            const int perCol = (W - 6) / static_cast<int>(cols.size());
            int endY = y;
            for (size_t j = 0; j < cols.size(); ++j) {
                const auto& h = data.adapterHistory[static_cast<size_t>(cols[j])];
                if (h.len < 2) continue;
                const int cx = x0 + 3 + static_cast<int>(j) * perCol;
                mvwprintw(stdscr, y, cx, "%s",
                          utf8_truncate(data.adapters[static_cast<size_t>(cols[j])].name,
                                        perCol - 2).c_str());
                const int last = (h.pos - 1 + TuiData::NET_HISTORY_MAX) % TuiData::NET_HISTORY_MAX;
                int cy = drawChart(cx, y + 1, perCol - 1, "D", h.dl, h.pos,
                                   h.dl[last], peakOf(h.dl, h.pos, h.len), 6);
                cy = drawChart(cx, cy, perCol - 1, "U", h.ul, h.pos,
                               h.ul[last], peakOf(h.ul, h.pos, h.len), 2);
                if (cy > endY) endY = cy;
            }
            y = endY;
        } else {
            // 3-col left margin + 9-col value axis + 1-col gap + right margin.
            const int colW = (std::min)(W - 6, data.dlHistoryLen + 10);
            if (colW - 10 >= 4) {
                const uint64_t dlMax = peakOf(data.dlHistory, data.dlHistoryPos, data.dlHistoryLen);
                const uint64_t ulMax = peakOf(data.ulHistory, data.dlHistoryPos, data.dlHistoryLen);
                const int last = (data.dlHistoryPos - 1 + TuiData::NET_HISTORY_MAX)
                               % TuiData::NET_HISTORY_MAX;
                int cy = drawChart(x0 + 3, y, colW, "D", data.dlHistory, data.dlHistoryPos,
                                   data.dlHistory[last], dlMax, 6);
                cy = drawChart(x0 + 3, cy, colW, "U", data.ulHistory, data.dlHistoryPos,
                               data.ulHistory[last], ulMax, 2);
                y = cy;

                snprintf(buf, sizeof(buf), "now %s down / %s up   peak D %s   U %s",
                         FormatRate(data.dlHistory[last]).c_str(),
                         FormatRate(data.ulHistory[last]).c_str(),
                         FormatRate(dlMax).c_str(), FormatRate(ulMax).c_str());
                emitDim(buf);
            }
        }
        if (y < bottom) y++;
    }

    if (data.hasWiFi) {
        section("WiFi");
        emit(std::string("State: ") +
             (data.wifiConnected ? Tr("wifi.connected") : Tr("wifi.disconnected")));

        std::string ids;
        if (!data.wifiSSID.empty()) ids = "SSID: " + data.wifiSSID;
        if (!data.wifiBSSID.empty()) {
            if (!ids.empty()) ids += "   ";
            ids += "BSSID: " + data.wifiBSSID;
        }
        if (!ids.empty()) {
            emit(ids);
        } else if (data.wifiLocationRoot) {
            emit(Tr("wifi.ssid_root"));
        } else if (data.wifiLocationStatus == 1 || data.wifiLocationDenied) {
            emit(Tr("wifi.ssid_denied"));
        } else {
            emit(Tr("wifi.loc_off"));
        }

        std::string radio;
        if (data.wifiChannel > 0) radio += "Ch " + std::to_string(data.wifiChannel);
        if (data.wifiRSSI < 0) {
            if (!radio.empty()) radio += "   ";
            radio += "RSSI " + std::to_string(data.wifiRSSI) + " dBm";
        }
        if (!data.wifiSecurity.empty()) {
            if (!radio.empty()) radio += "   ";
            radio += data.wifiSecurity;
        }
        if (!data.wifiBand.empty()) {
            if (!radio.empty()) radio += "   ";
            radio += data.wifiBand;
        }
        if (!data.wifiGen.empty()) {
            if (!radio.empty()) radio += "   ";
            radio += data.wifiGen;
        }
        if (data.wifiTxRate > 0) {
            if (!radio.empty()) radio += "   ";
            radio += "Tx " + std::to_string(static_cast<int>(data.wifiTxRate)) + " Mbps";
        }
        if (!radio.empty()) emit(radio);
        if (!data.wifiChipset.empty()) emit("Chip: " + data.wifiChipset);
        if (y < bottom) y++;
    }

    if (data.hasBluetooth) {
        section("Bluetooth");
        char btBuf[64];
        if (data.btPowerOn) snprintf(btBuf, sizeof(btBuf), Tr("bt.on"), data.btDeviceCount);
        else snprintf(btBuf, sizeof(btBuf), "%s", Tr("bt.off"));
        std::string head = btBuf;
        if (!data.btChipset.empty()) head += "   Chip " + data.btChipset;
        if (!data.btFirmware.empty()) head += "   FW " + data.btFirmware;
        emit(head);

        int pairedIdle = 0;
        for (const auto& d : data.btDevices) {
            if (!d.connected) { ++pairedIdle; continue; }
            std::string line = d.name;
            if (!d.type.empty()) line += "  " + d.type;
            if (!d.battery.empty()) line += "  " + d.battery;
            if (d.rssi < 0) {
                const int pct = BtSignalPercent(d.rssi);
                if (pct >= 0) {
                    line += "  Signal " + std::to_string(pct) + "% (" +
                            std::to_string(d.rssi) + "dBm)";
                }
            }
            emit(line);
        }
        if (pairedIdle > 0) {
            snprintf(buf, sizeof(buf), Tr("bt.paired"), pairedIdle);
            emitDim(buf);
        }
    }

    mvwprintw(stdscr, y0 + H - 2, x0 + 3, "%s",
              utf8_truncate(Tr("details.close"), W - 6).c_str());
}

// Full-screen splash for the first collection round. Deliberately minimal:
// title, percentage, progress bar, quit hint. The dashboard only appears once
// every collector has reported, so nobody sees a frame full of empty panels.
void TuiApp::RenderLoadingScreen(int rows, int cols, int done, int total) {
    erase();
    const int pct = total > 0 ? done * 100 / total : 100;

    const char* title = "TCMT Monitor";
    char label[48];
    snprintf(label, sizeof(label), "LOADING %d%%", pct);

    int y = rows / 2 - 3;
    if (y < 1) y = 1;

    wattron(stdscr, COLOR_PAIR(1) | A_BOLD);
    mvwprintw(stdscr, y, (std::max)(1, (cols - static_cast<int>(std::strlen(title))) / 2),
              "%s", title);
    wattroff(stdscr, COLOR_PAIR(1) | A_BOLD);

    mvwprintw(stdscr, y + 2, (std::max)(1, (cols - static_cast<int>(std::strlen(label))) / 2),
              "%s", label);

    const int barW = (std::max)(10, (std::min)(40, cols - 10));
    const int nFill = pct * barW / 100;
    const int barX = (std::max)(1, (cols - barW) / 2);
    int bx = barX;
    for (int i = 0; i < nFill; ++i) mvwaddstr(stdscr, y + 3, bx++, barFill_.c_str());
    wattron(stdscr, A_DIM);
    for (; bx < barX + barW; ++bx) mvwaddstr(stdscr, y + 3, bx, barTrack_.c_str());
    wattroff(stdscr, A_DIM);

    const char* hint = "Q:Quit";
    wattron(stdscr, A_DIM);
    mvwprintw(stdscr, y + 5, (std::max)(1, (cols - static_cast<int>(std::strlen(hint))) / 2),
              "%s", hint);
    wattroff(stdscr, A_DIM);

    // Never park the cursor on the bottom-right corner (see the dashboard
    // note about the ncurses wrap loop).
    move(0, 0);
}

// Focus order is draw order: the left column top-down, then the right
// column. Only panels that actually rendered are registered, so Tab skips
// the ones a short terminal or missing hardware dropped this frame.
void TuiApp::RegisterPanel(PanelId id, int y, int x, int w, int h) {
    if (h <= 0 || w <= 0) return;
    panelHits_.push_back({id, y, x, w, h});
}

void TuiApp::CyclePanelFocus(int dir) {
    if (panelHits_.empty()) return;
    const int n = static_cast<int>(panelHits_.size());
    int cur = -1;
    for (int i = 0; i < n; ++i) {
        if (panelHits_[i].id == focusedPanel_) { cur = i; break; }
    }
    if (cur < 0) {
        // Focus sits on a panel that is not on screen: enter at the near end.
        focusedPanel_ = (dir >= 0) ? panelHits_.front().id : panelHits_.back().id;
        return;
    }
    focusedPanel_ = panelHits_[(cur + dir + n) % n].id;
}

// Minimal mouse (B direction): translate one mouse event into the keyboard
// action a user would have typed. Every existing handler stays the source
// of truth — mouse is only an input shim:
//   click a panel             -> focus it (dashboard only)
//   click a process row       -> focus the list and select that row
//   click the Connections row -> 'c' (client list)
//   wheel                     -> ignored (see below)
// Clicking is gated on the dashboard being the visible surface: on any page the
// list geometry below is left over from the last dashboard frame, and acting on
// it moved the dashboard selection from behind the page the user was reading.
int TuiApp::MapMouseEvent(int rows, int cols) {
#ifdef KEY_MOUSE
    MEVENT mev;
    if (getmouse(&mev) != OK) return ERR;
    (void)cols;

    const bool dashboardActive = !logPage_ && !settingsPage_ && !helpPage_ && !connListPage_;
    if (dashboardActive &&
        (mev.bstate & (BUTTON1_PRESSED | BUTTON1_CLICKED | BUTTON1_DOUBLE_CLICKED))) {
        // Any click inside a panel drawn last frame focuses it. The list
        // check below runs afterwards: it also moves the row selection, and
        // its rectangle lies inside the registered process panel.
        for (const auto& p : panelHits_) {
            if (mev.y >= p.y && mev.y < p.y + p.h &&
                mev.x >= p.x && mev.x < p.x + p.w) {
                focusedPanel_ = p.id;
                break;
            }
        }
        // Double-click on a panel with a detail view opens it: hand the
        // keyboard handler the same synthetic key the keyboard would send.
        if ((mev.bstate & BUTTON1_DOUBLE_CLICKED) &&
            (focusedPanel_ == PanelId::Cpu || focusedPanel_ == PanelId::Gpu ||
             focusedPanel_ == PanelId::Disk || focusedPanel_ == PanelId::Power ||
             focusedPanel_ == PanelId::Network)) {
            return ' ';
        }
        // Click inside the Top Processes list region (geometry cached by
        // DrawProcessPanel every frame).
        if (procListY_ >= 0 && mev.y > procListY_ &&
            mev.y <= procListY_ + procListCount_ &&
            mev.x >= procListX0_ && mev.x < procListX0_ + procListW_) {
            const int idx = mev.y - procListY_ - 1;
            {
                std::lock_guard<std::mutex> lock(dataMutex_);
                if (idx >= 0 && idx < static_cast<int>(data_.topProcesses.size())) {
                    selPid_ = data_.topProcesses[idx].pid;
                }
            }
            return ERR;   // handled — the next frame paints the selection
        }
        // Click on the bottom Connections status row -> client list ('c').
        if (mev.y == rows - 3 && mev.x >= 1 && mev.x < cols - 1) {
            connListPage_ = true;
            clear();
            return ERR;
        }
        return ERR;
    }
    // Wheel is deliberately unmapped: forwarding it as KEY_UP/KEY_DOWN made
    // the process selection chase a backlog of queued events, so the list
    // visibly lagged behind the gesture. Keyboard arrows are the only way to
    // move the selection.
#else
    (void)rows;
    (void)cols;
#endif
    return ERR;
}

void TuiApp::Run() {
#ifdef _WIN32
    // PDC_WIDE decodes narrow addstr() input through the C locale. On Windows
    // setlocale("") resolves to the "C" locale, which would decode our UTF-8
    // strings as the ANSI code page; ".UTF-8" is the UCRT name that makes the
    // conversion correct. Keep the environment-based fallback for systems
    // without UCRT UTF-8 support (older Windows 10).
    if (!std::setlocale(LC_ALL, ".UTF-8")) std::setlocale(LC_ALL, "en_US.UTF-8");
#else
    setlocale(LC_ALL, "");
#endif

    // Downgrade switches (#11), read once at startup:
    //   NO_COLOR      — any non-empty value disables color output.
    //   TCMT_ASCII=1  — ASCII glyphs (sparkline levels, degree signs). Also
    //                   forced when the active locale is not UTF-8, where
    //                   multibyte glyphs cannot be trusted to render.
    const char* noColorEnv = std::getenv("NO_COLOR");
    noColor_ = (noColorEnv != nullptr) && (*noColorEnv != '\0');
    const char* asciiEnv = std::getenv("TCMT_ASCII");
    const char* curLocale = std::setlocale(LC_ALL, nullptr);
    // UCRT may report the locale as "...65001" instead of "...UTF-8", so the
    // code page number counts as well.
#ifdef _WIN32
    // What decodes our output is the console code page (main.cpp sets it to
    // UTF-8). The CRT locale can still be the ANSI one, which used to force
    // the whole UI into ASCII fallback even though every glyph rendered.
    const bool utf8Console = GetConsoleOutputCP() == 65001;
#else
    const bool utf8Console = false;
#endif
    const bool utf8Locale = utf8Console || (curLocale &&
        (std::strstr(curLocale, "UTF-8") != nullptr ||
         std::strstr(curLocale, "utf-8") != nullptr ||
         std::strstr(curLocale, "65001") != nullptr));
    // i18n: TCMT_LANG > ui.language from config > active locale > en; non-UTF-8 forces en.
    // I18nSelect only looks at the "zh" prefix, so the saved config value
    // ("zh-Hans" / "en") rides the same slot as the locale.
    I18nSelect(std::getenv("TCMT_LANG"),
               configuredLang_.empty() ? curLocale : configuredLang_.c_str(),
               utf8Locale);
    asciiMode_ = (asciiEnv != nullptr && std::strcmp(asciiEnv, "0") != 0) || !utf8Locale;
    Logger::Debug(std::string("TUI: locale=") + (curLocale ? curLocale : "(null)") +
                  " ascii=" + (asciiMode_ ? "1" : "0") +
                  " TCMT_ASCII=" + (asciiEnv ? asciiEnv : "(unset)"));
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
#ifdef __PDCURSES__
    // conhost picks the CHAR_INFO path (pdc_ansi == false) and converts every
    // byte of a multibyte UTF-8 sequence as its own cell, so Chinese process
    // names and SSIDs turn into mojibake. The ANSI path writes the whole line
    // with WriteConsoleA, where the console's UTF-8 code page decodes it.
    // PDCurses enables VT processing before emitting its escapes.
    Logger::Debug(std::string("TUI: terminal=") + (pdc_wt ? "windows-terminal"
                                                   : pdc_conemu ? "conemu" : "conhost"));
    pdc_ansi = true;
#endif
#ifndef __PDCURSES__
    ESCDELAY = 25;   // 25ms Esc timeout (default 1000ms) — fix "half-exit" feel
#endif

    consoleMode_ = asciiMode_ ? "ASCII" : "UTF-8";
#ifdef _WIN32
    // Show the locale that triggered the downgrade; without it the status row
    // only says "ASCII" and the cause has to be guessed.
    if (asciiMode_ && curLocale)
        consoleMode_ += std::string(" (locale=") + curLocale + ")";
#endif
#ifdef __PDCURSES__
    consoleMode_ += std::string(" / ") + (pdc_wt ? "Windows Terminal"
                                          : pdc_conemu ? "ConEmu" : "conhost");
#endif

    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    nodelay(stdscr, TRUE);

    // Minimal mouse: terminals that report (iTerm2, Kitty, Alacritty, tmux,
    // PDCurses console) get clicks and wheel; Apple Terminal sends nothing
    // and simply stays keyboard-only. Enable always — no downside where the
    // terminal does not report.
    mousemask(ALL_MOUSE_EVENTS, nullptr);

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

#ifdef KEY_MOUSE
        // Minimal mouse shim: translate the event into the keyboard action
        // before any handler sees it (see MapMouseEvent for the mapping).
        if (ch == KEY_MOUSE) ch = MapMouseEvent(rows, cols);
#endif

        // While a page owns the screen the dashboard is not drawn, so its
        // cached list geometry is stale: drop it, and no mouse event can
        // address a rectangle that is not on screen.
        if (settingsPage_ || helpPage_ || connListPage_ || logPage_ || cpuPage_ ||
            gpuPage_ || diskPage_ || batteryPage_ || networkPage_) procListY_ = -1;

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

        // Connections list overlay ('c') — the compact bottom chip expands
        // into one row per live client. Same modal semantics.
        if (connListPage_) {
            RenderConnectionsList(rows, cols, ch);
            refresh();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }

        // CPU details overlay — Space on the focused CPU panel, or a
        // double-click on it. Same modal semantics as the other pages.
        if (cpuPage_) {
            RenderCpuDetails(rows, cols, ch);
            refresh();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }

        if (gpuPage_) {
            RenderGpuDetails(rows, cols, ch);
            refresh();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }

        if (diskPage_) {
            RenderDiskDetails(rows, cols, ch);
            refresh();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }

        if (batteryPage_) {
            RenderBatteryDetails(rows, cols, ch);
            refresh();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }

        if (networkPage_) {
            RenderNetworkDetails(rows, cols, ch);
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
            langOnOpen_ = g_lang;
            draftLang_ = g_lang;
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
        // pairs with the standalone Win32 log window, so L stays inert.
        // Tab used to toggle the log page too; on the dashboard it now
        // cycles panel focus, so it only acts as "back" while the page is
        // already open.
        if (caps_.inlineLogPage && (ch == 'l' || ch == 'L' || (ch == '\t' && logPage_))) {
            logPage_ = !logPage_;
            clear();
            ch = ERR;   // consume: Tab must not also advance panel focus
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
            mvprintw(0, 0, Tr("sys.toosmall"), cols, rows);
            refresh();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        // Log page — in-process full-screen log view (L to switch back).
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

        // Splash from the very first frame: before any snapshot arrives the
        // dashboard would paint an empty frame, and that flash is what the
        // splash is meant to hide. `lastUpdateUs_ == 0` means "no data yet".
        if (lastUpdateUs_.load() == 0) {
            RenderLoadingScreen(rows, cols, 0, 1);
            refresh();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            continue;
        }
        // First collection round: show a full-screen splash instead of a
        // dashboard full of half-empty panels. q still quits (handled above);
        // every other dashboard key waits until the splash is gone.
        if (data.loadTotal > 0 && data.loadDone < data.loadTotal) {
            RenderLoadingScreen(rows, cols, data.loadDone, data.loadTotal);
            refresh();
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        // Tab / Shift+Tab walk the panel focus ring. The ring is the set of
        // panels drawn last frame, in draw order, so hidden panels are never
        // a stop. Mouse clicks focus a panel the same way (MapMouseEvent).
        if (ch == '\t') {
            CyclePanelFocus(1);
        }
#ifdef KEY_BTAB
        else if (ch == KEY_BTAB) {
            CyclePanelFocus(-1);
        }
#endif

        // Space expands the focused panel. CPU and GPU have detail views; a
        // double-click on either arrives here as a synthetic space.
        if (ch == ' ') {
            bool pageOpened = false;
            if (focusedPanel_ == PanelId::Cpu) {
                cpuPage_ = true;
                clear();
                pageOpened = true;
            } else if (focusedPanel_ == PanelId::Gpu) {
                gpuPage_ = true;
                clear();
                pageOpened = true;
            } else if (focusedPanel_ == PanelId::Disk) {
                diskPage_ = true;
                clear();
                pageOpened = true;
            } else if (focusedPanel_ == PanelId::Power) {
                batteryPage_ = true;
                clear();
                pageOpened = true;
            } else if (focusedPanel_ == PanelId::Network) {
                networkPage_ = true;
                clear();
                pageOpened = true;
            }
            // PDCurses counts one cell per UTF-8 byte, so the second column
            // of a wide glyph lands in a cell it believes is blank and never
            // repaints — the leftover half-glyph stays on screen. Touching
            // the window makes the first frame of the page rewrite every
            // cell, which is exactly what clear() alone fails to do.
            if (pageOpened) touchwin(stdscr);
        }

        // Dashboard-local keys need the current snapshot, so they are
        // handled here: ↑/↓ move the process selection while Top Processes
        // is the focused panel. The top list re-sorts every refresh, so the
        // selection tracks the pid — if the process leaves the list the
        // highlight clears.
        int selIdx = -1;
        if (selPid_ >= 0) {
            for (size_t i = 0; i < data.topProcesses.size(); ++i) {
                if (data.topProcesses[i].pid == selPid_) { selIdx = static_cast<int>(i); break; }
            }
            if (selIdx < 0) selPid_ = -1;   // selected process left the top list
        }
        if (focusedPanel_ == PanelId::Process && !data.topProcesses.empty()) {
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
        std::string hint = Tr("hint.settings");
        hint += " "; hint += Tr("hint.panel");
        if (caps_.inlineLogPage) { hint += " "; hint += Tr("hint.log"); }
        if (updateRequestHandler_) { hint += " "; hint += Tr("hint.update"); }
        if (locationRequestHandler_) { hint += " "; hint += Tr("hint.location"); }
        hint += " "; hint += Tr("hint.help");
        hint += " "; hint += Tr("hint.quit");
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
            mvwprintw(stdscr, 1, row1Left, "%.*s", cols - 3, chip.c_str());
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
                // Centred and clipped in columns: the banner carries update
                // status copy, which is translated (and therefore wide).
                const int bannerW = utf8_display_width(banner);
                int bx = zoneL + (zoneW - bannerW) / 2;
                bx = (std::max)(zoneL, bx);
                mvwprintw(stdscr, 1, bx, "%s",
                          utf8_truncate(banner, zoneR - bx + 1).c_str());
            }
            wattroff(stdscr, COLOR_PAIR(color) | A_BOLD);
        }

        // === Left panels (CPU + GPU + Memory) ===
        // Panels may start one row deeper than before: the bottom status
        // rows (rows-4..rows-2) are the only reserved band now, and content
        // reaching rows-5 is safe — anything below rows-4 gets wiped and
        // repainted by the status rows.
        int maxY = rows - 4;
        panelHits_.clear();   // rebuilt below: only panels drawn this frame are focusable
        int ly = 2;
        if (ly < maxY) {
            int cpuLines = DrawCpuPanel(stdscr, data, ly, lx, leftW);
            RegisterPanel(PanelId::Cpu, ly, lx, leftW, cpuLines);
            ly += cpuLines + 1;
            if (ly < maxY) {
                int h = DrawGpuPanel(stdscr, data, ly, lx, leftW);
                RegisterPanel(PanelId::Gpu, ly, lx, leftW, h);
                ly += h + 1;
            }
            if (ly < maxY) {
                int h = DrawMemoryPanel(stdscr, data, ly, lx, leftW);
                RegisterPanel(PanelId::Memory, ly, lx, leftW, h);
                ly += h;
            }
            if (ly < maxY) {
                ly += 2;  // blank rows before processes
            }
            if (ly < maxY) {
                int h = DrawProcessPanel(stdscr, data, ly, lx, leftW);
                RegisterPanel(PanelId::Process, ly, lx, leftW, h);
                ly += h;
            }
            // Per-core temps/frequencies are a detail-view thing (Space on the
            // CPU panel), not a dashboard panel: a row of C00..C15 numbers on
            // the main screen says nothing a user can act on.
        }
        if (ly > maxY) ly = maxY;

        // === Right panels (storage / network / thermal first) ===
        int ry = 2;
        if (ry < maxY) {
            int h = DrawDiskPanel(stdscr, data, ry, rx, rightW);
            RegisterPanel(PanelId::Disk, ry, rx, rightW, h);
            ry += h;
            if (ry < maxY) {
                ry += DrawPhysicalDiskPanel(stdscr, data, ry, rx, rightW);
            }
            if (ry < maxY) {
                h = DrawNetworkPanel(stdscr, data, ry, rx, rightW);
                RegisterPanel(PanelId::Network, ry, rx, rightW, h);
                ry += h;
            }
            // WiFi & Bluetooth supplementary info after Network
            if (ry < maxY) {
                ry += DrawWifiBluetoothPanel(stdscr, data, ry, rx, rightW);
            }
            // Temps and Power rank above the informational extras: on a short
            // terminal it is Displays/Accel/TPM that silently drop, not the
            // thermal state.
            if (ry < maxY) {
                h = DrawTempPanel(stdscr, data, ry, rx, rightW);
                RegisterPanel(PanelId::Temp, ry, rx, rightW, h);
                ry += h;
            }
            if (ry < maxY) {
                h = DrawPowerPanel(stdscr, data, ry, rx, rightW);
                RegisterPanel(PanelId::Power, ry, rx, rightW, h);
                ry += h;
            }
            if (ry < maxY) {
                h = DrawDisplayPanel(stdscr, data, ry, rx, rightW);
                RegisterPanel(PanelId::Display, ry, rx, rightW, h);
                ry += h;
            }
            if (ry < maxY) {
                h = DrawAccelPanel(stdscr, data, ry, rx, rightW);
                RegisterPanel(PanelId::Accel, ry, rx, rightW, h);
                ry += h;
            }
            if (ry < maxY) {
                h = DrawTpmPanel(stdscr, data, ry, rx, rightW);
                RegisterPanel(PanelId::Tpm, ry, rx, rightW, h);
                ry += h;
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

        // Row rows-4: uptime | load | processes | login user (was System line 2).
        // The user is last so a narrow terminal truncates it before the
        // machine-wide numbers; under sudo it is the only place the login
        // account is visible without leaving the dashboard.
        if (data.uptimeSeconds > 0 || data.loadAvg1 > 0 || data.processCount > 0 ||
            !data.loginUser.empty() || !consoleMode_.empty()) {
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
            if (!data.loginUser.empty()) {
                sysStr += "   User: " + data.loginUser;
                if (data.elevatedViaSudo) sysStr += " (sudo)";
            }
            if (!consoleMode_.empty()) {
                sysStr += "   Mode: " + consoleMode_;
            }
            mvwprintw(stdscr, rows - 4, 2, "%s",
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
                    line += Tr("status.since") + data.connectionSince;
            } else {
                line = Tr("status.noclients");
            }
            std::string pushStr;
            int color = (data.connectionCount > 0) ? 2 : -1;
            if (!data.serverPushEnabled) {
                pushStr = Tr("push.disabled");
            } else {
                std::string age = Tr("push.never");
                if (data.lastPushMs > 0) {
                    int64_t sec = std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count()
                        - data.lastPushMs / 1000;
                    age = std::to_string((std::max)(int64_t(0), sec)) + Tr("push.ago");
                }
                // "connecting..." / "uploading (dev_xxx)" / "error: ...".
                const bool err = data.serverStatus.rfind("error", 0) == 0;
                const bool connecting = data.serverStatus.rfind("connecting", 0) == 0;
                color = err ? 4 : (connecting ? 3 : 2);
                pushStr = Tr("push.prefix") + data.serverStatus + Tr("push.last") + age + Tr("push.last_close");
            }
            if (!pushStr.empty()) line += "   " + pushStr;
            line += "   [c=list]";   // 'c' expands this chip into the client list
            wattron(stdscr, A_REVERSE);
            mvwprintw(stdscr, rows - 3, 1, " Connections ");
            wattroff(stdscr, A_REVERSE);
            const int availW = (cols > 17) ? (cols - 17) : 1;
            if (color >= 0) wattron(stdscr, COLOR_PAIR(color));
            mvwprintw(stdscr, rows - 3, 16, "%s",
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
            mvwprintw(stdscr, rows - 2, 10, "%s",
                      utf8_truncate(Tr("sys.unknown_os"), cols - 12).c_str());
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
