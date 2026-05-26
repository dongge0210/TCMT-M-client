#include <curses.h>
#include "TuiApp.h"
#include <ctime>
#include <cstring>
#include <algorithm>
#include <iomanip>
#include <sstream>
#include <chrono>

namespace tcmt {

// ============================================================================
// TuiApp
// ============================================================================

TuiApp::TuiApp() {
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
    if (!running_.load()) return;
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
}

LogBuffer& TuiApp::GetLogBuffer() {
    return defaultBuffer_;
}

void TuiApp::SetLogBuffer(LogBuffer* buf) {
    logBuf_ = buf ? buf : &defaultBuffer_;
}

void TuiApp::InitColors() {
    if (!has_colors()) return;
    start_color();
    use_default_colors();

    init_pair(1, COLOR_CYAN, -1);    // Header
    init_pair(2, COLOR_GREEN, -1);  // Normal
    init_pair(3, COLOR_YELLOW, -1); // Warning
    init_pair(4, COLOR_RED, -1);   // Error/Critical
    init_pair(5, COLOR_WHITE, -1); // Label
    init_pair(6, COLOR_BLUE, -1);  // Bar
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

std::string TuiApp::FormatBar(double pct, int width) {
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    int filled = static_cast<int>(pct * width / 100.0);
    std::string bar;
    for (int i = 0; i < width; ++i) {
        bar += (i < filled) ? '=' : '-';
    }
    return bar;
}

std::string TuiApp::TrimRight(const std::string& s, size_t maxLen) {
    if (s.size() <= maxLen) return s;
    return s.substr(0, maxLen);
}

void TuiApp::DrawHeader(WINDOW* win, const TuiData& data) {
    int rows, cols;
    getmaxyx(win, rows, cols);
    wattron(win, COLOR_PAIR(1) | A_BOLD);
    std::string res = "[" + std::to_string(cols) + "x" + std::to_string(rows) + "]";
    std::string title = "TCMT Monitor  " + data.timestamp + "  " + res;
    int x = (cols - static_cast<int>(title.size())) / 2;
    mvwprintw(win, 0, std::max(0, x), "%s", title.c_str());
    wattroff(win, COLOR_PAIR(1) | A_BOLD);
}

int TuiApp::DrawCpuPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW) {
    if (maxW < 10) return 0;
    int bw = std::min(maxW - 14, 30);
    bw = std::max(bw, 4);
    int lines = 0;

    wattron(win, COLOR_PAIR(5) | A_BOLD);
    mvwprintw(win, y + lines, x0, "%.*s", maxW, "CPU");
    wattroff(win, COLOR_PAIR(5) | A_BOLD);
    lines++;

    auto name = TrimRight(data.cpuName, maxW - 4);
    mvwprintw(win, y + lines, x0 + 2, "%.*s", maxW - 2, name.c_str());
    lines++;

    mvwprintw(win, y + lines, x0 + 2, "Use:");
    wattron(win, COLOR_PAIR(6));
    mvwprintw(win, y + lines, x0 + 8, "%.*s", maxW - 8, FormatBar(data.cpuUsage, bw).c_str());
    wattroff(win, COLOR_PAIR(6));
    mvwprintw(win, y + lines, x0 + 9 + bw, "%.1f%%", data.cpuUsage);
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
        mvwprintw(win, y + lines, x0 + 2, "%.*s", maxW - 2, ss.str().c_str());
    } else if (data.physicalCores > 0) {
        mvwprintw(win, y + lines, x0 + 2, "Cores: %d", data.physicalCores);
    }
    lines++;

    return lines;
}

int TuiApp::DrawMemoryPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW) {
    if (maxW < 10) return 0;
    int bw = std::min(maxW - 16, 30);
    bw = std::max(bw, 4);
    int lines = 0;

    wattron(win, COLOR_PAIR(5) | A_BOLD);
    mvwprintw(win, y + lines, x0, "%.*s", maxW, "RAM");
    wattroff(win, COLOR_PAIR(5) | A_BOLD);
    lines++;

    double upct = (data.totalMemory > 0) ? 100.0 * data.usedMemory / data.totalMemory : 0;
    mvwprintw(win, y + lines, x0 + 2, "Used:");
    wattron(win, COLOR_PAIR(6));
    mvwprintw(win, y + lines, x0 + 8, "%.*s", maxW - 8, FormatBar(upct, bw).c_str());
    wattroff(win, COLOR_PAIR(6));
    auto usedStr = FormatSize(data.usedMemory);
    auto totalStr = FormatSize(data.totalMemory);
    mvwprintw(win, y + lines, x0 + 9 + bw, "%.*s / %.*s",
              maxW - 9 - bw, usedStr.c_str(),
              maxW - 10 - bw - static_cast<int>(usedStr.size()), totalStr.c_str());
    lines++;

    auto availStr = FormatSize(data.availableMemory);
    mvwprintw(win, y + lines, x0 + 2, "Avail: %.*s", maxW - 8, availStr.c_str());
    lines++;

    if (data.compressedMemory > 0) {
        auto compStr = FormatSize(data.compressedMemory);
        mvwprintw(win, y + lines, x0 + 2, "Compressed: %.*s", maxW - 12, compStr.c_str());
        lines++;
    }
    if (data.swapTotal > 0) {
        auto usedStr = FormatSize(data.swapUsed);
        auto totalStr = FormatSize(data.swapTotal);
        mvwprintw(win, y + lines, x0 + 2, "Swap: %.*s / %.*s",
            maxW - 14, usedStr.c_str(), maxW - 18, totalStr.c_str());
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
    int bw = std::min(maxW - 16, 30);
    bw = std::max(bw, 4);
    int lines = 0;

    wattron(win, COLOR_PAIR(5) | A_BOLD);
    mvwprintw(win, y + lines, x0, "%.*s", maxW, "GPU");
    wattroff(win, COLOR_PAIR(5) | A_BOLD);
    lines++;

#ifndef TCMT_MACOS
    auto name = TrimRight(data.gpuName, maxW - 4);
    mvwprintw(win, y + lines, x0 + 2, "%.*s", maxW - 2, name.c_str());
    lines++;
#endif

    mvwprintw(win, y + lines, x0 + 2, "Use:");
    wattron(win, COLOR_PAIR(6));
    mvwprintw(win, y + lines, x0 + 8, "%.*s", maxW - 8, FormatBar(data.gpuUsage, bw).c_str());
    wattroff(win, COLOR_PAIR(6));
    mvwprintw(win, y + lines, x0 + 9 + bw, "%.1f%%", data.gpuUsage);
    lines++;

#ifndef TCMT_MACOS
    if (data.gpuMemory > 0) {
        auto memStr = FormatSize(data.gpuMemory);
        mvwprintw(win, y + lines, x0 + 2, "VRAM: %.*s", maxW - 8, memStr.c_str());
        lines++;
    }
    if (data.gpuMemoryPercent > 1 && data.gpuMemory > 0) {
        uint64_t used = (uint64_t)(data.gpuMemory * data.gpuMemoryPercent / 100.0);
        auto usedStr = FormatSize(used);
        auto totalStr = FormatSize(data.gpuMemory);
        mvwprintw(win, y + lines, x0 + 2, "VRAM: %.*s / %.*s",
            maxW - 8, usedStr.c_str(), maxW - 14, totalStr.c_str());
        lines++;
    }
#endif

    if (data.gpuFreq > 0) {
        if (data.gpuMaxFreq > 0 && static_cast<int>(data.gpuMaxFreq) != static_cast<int>(data.gpuFreq))
            mvwprintw(win, y + lines, x0 + 2, "Freq: %d/%d MHz", static_cast<int>(data.gpuFreq), static_cast<int>(data.gpuMaxFreq));
        else
            mvwprintw(win, y + lines, x0 + 2, "Freq: %d MHz", static_cast<int>(data.gpuFreq));
        lines++;
    }
    return lines;
}

int TuiApp::DrawDiskPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW) {
    if (maxW < 10) return 0;

    wattron(win, COLOR_PAIR(5) | A_BOLD);
    mvwprintw(win, y, x0, "%.*s", maxW, "Disks");
    wattroff(win, COLOR_PAIR(5) | A_BOLD);
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
    wattron(win, COLOR_PAIR(5) | A_BOLD);
    mvwprintw(win, y, x0, "%.*s", maxW, "Network");
    wattroff(win, COLOR_PAIR(5) | A_BOLD);
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
            auto d = n.downloadSpeed > 0 ? FormatSize(n.downloadSpeed) : "0 B";
            mvwprintw(win, y + lines, x0 + 4, "D: %.*s/s", maxW - 8, d.c_str());
            lines++;
            auto u = n.uploadSpeed > 0 ? FormatSize(n.uploadSpeed) : "0 B";
            mvwprintw(win, y + lines, x0 + 4, "U: %.*s/s", maxW - 8, u.c_str());
            lines++;
        }
    }
    return lines;
}

int TuiApp::DrawWifiBluetoothPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW) {
    if (maxW < 10) return 0;
    int lines = 0;

    if (data.hasWiFi) {
        bool hasData = !data.wifiSSID.empty() || data.wifiRSSI < 0 || data.wifiChannel > 0;
        std::string wifiStr = hasData ? "On" : "Disconnected";
        if (!data.wifiSSID.empty()) wifiStr += "  SSID: " + data.wifiSSID;
        if (!data.wifiBSSID.empty()) wifiStr += "  BSSID: " + data.wifiBSSID;
        if (data.wifiChannel > 0) wifiStr += "  Ch: " + std::to_string(data.wifiChannel);
        if (data.wifiRSSI < 0) wifiStr += "  RSSI: " + std::to_string(data.wifiRSSI) + " dBm";
        if (!data.wifiSecurity.empty()) wifiStr += "  " + data.wifiSecurity;
        if (!data.wifiBand.empty()) wifiStr += "  " + data.wifiBand;
        if (!data.wifiGen.empty()) wifiStr += "  " + data.wifiGen;
        if (data.wifiTxRate > 0) wifiStr += "  Tx: " + std::to_string(static_cast<int>(data.wifiTxRate)) + "Mbps";
        wifiStr = TrimRight(wifiStr, maxW - 8);
        wattron(win, COLOR_PAIR(5));
        mvwprintw(win, y + lines, x0 + 2, "WiFi:");
        wattroff(win, COLOR_PAIR(5));
        mvwprintw(win, y + lines, x0 + 8, "%.*s", maxW - 10, wifiStr.c_str());
        lines++;
    } else {
        wattron(win, COLOR_PAIR(5));
        mvwprintw(win, y + lines, x0 + 2, "WiFi:");
        wattroff(win, COLOR_PAIR(5));
        mvwprintw(win, y + lines, x0 + 8, "%.*s", maxW - 10, "Off");
        lines++;
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

int TuiApp::DrawTpmPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW) {
    if (maxW < 10) return 0;
    if (data.tpmInfo.empty() || data.tpmInfo == "No TPM") return 0;
    wattron(win, COLOR_PAIR(5) | A_BOLD);
    mvwprintw(win, y, x0, "%.*s", maxW, "TPM");
    wattroff(win, COLOR_PAIR(5) | A_BOLD);
    mvwprintw(win, y + 1, x0 + 2, "%.*s", maxW - 2, data.tpmInfo.c_str());
    return 2;
}

int TuiApp::DrawPhysicalDiskPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW) {
    if (maxW < 10 || data.physicalDisks.empty()) return 0;
    int lines = 0;

    for (const auto& pd : data.physicalDisks) {
        if (y + lines >= LINES - 5) break;
        std::string line = pd.model;
        if (!pd.diskType.empty()) line += " " + pd.diskType;
        if (pd.smartSupported) { char b[8]; snprintf(b, sizeof(b), " %d%%", pd.healthPct); line += b; }
        line = TrimRight(line, maxW - 2);
        mvwprintw(win, y + lines++, x0, "%.*s", maxW, line.c_str());
    }
    return lines;
}

int TuiApp::DrawTempPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW) {
    if (maxW < 10) return 0;
    wattron(win, COLOR_PAIR(5) | A_BOLD);
    mvwprintw(win, y, x0, "%.*s", maxW, "Temps");
    wattroff(win, COLOR_PAIR(5) | A_BOLD);
    int lines = 1;

    int halfW = maxW / 2;
    const int CONTENT_ROWS = 2;      // 4 sensors per page (2 per row)
    const int PANEL_ROWS    = 4;     // fixed: 1 header + 2 content + 1 page indicator

    // Collect sensors to display: skip per-core CPU sensors
    std::vector<std::pair<std::string, double>> displayTemps;
    for (const auto& [name, temp] : data.temperatures) {
        if (name.find("CPU Core ") == std::string::npos)
            displayTemps.push_back({name, temp});
    }

    int perPage = CONTENT_ROWS * 2;
    int totalPages = (std::max)(1, (static_cast<int>(displayTemps.size()) + perPage - 1) / perPage);
    bool needPaging = totalPages > 1;

    static int currentPage = 0;
    static auto lastPageFlip = std::chrono::steady_clock::now();
    if (needPaging) {
        auto tNow = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(tNow - lastPageFlip).count() >= 3) {
            currentPage = (currentPage + 1) % totalPages;
            lastPageFlip = tNow;
        }
    } else {
        currentPage = 0;
    }

    int startIdx = currentPage * CONTENT_ROWS * 2;
    for (int p = 0; p < CONTENT_ROWS; p++) {
        int leftIdx = startIdx + p * 2;
        if (leftIdx >= static_cast<int>(displayTemps.size())) break;

        auto& [nameL, tempL] = displayTemps[leftIdx];
        auto labelL = TrimRight(nameL, halfW - 9);
        int tcL = (tempL > 80) ? 4 : (tempL > 60) ? 3 : 2;
        wattron(win, COLOR_PAIR(tcL));
        mvwprintw(win, y + lines, x0 + 2, "%.*s %.1f C", halfW - 9, labelL.c_str(), tempL);
        wattroff(win, COLOR_PAIR(tcL));

        int rightIdx = leftIdx + 1;
        if (rightIdx < static_cast<int>(displayTemps.size())) {
            auto& [nameR, tempR] = displayTemps[rightIdx];
            auto labelR = TrimRight(nameR, halfW - 9);
            int tcR = (tempR > 80) ? 4 : (tempR > 60) ? 3 : 2;
            wattron(win, COLOR_PAIR(tcR));
            mvwprintw(win, y + lines, x0 + 2 + halfW, "%.*s %.1f C", halfW - 9, labelR.c_str(), tempR);
            wattroff(win, COLOR_PAIR(tcR));
        }
        lines++;
    }

    // Fill remaining content rows (fixed panel height, no bounce)
    while (lines < 1 + CONTENT_ROWS)
        lines++;

    // Page indicator (always at row 4)
    if (needPaging)
        mvwprintw(win, y + PANEL_ROWS - 1, x0 + 2, "%.*s", maxW - 2,
                  ("[" + std::to_string(currentPage + 1) + "/" + std::to_string(totalPages) + "]").c_str());
    else
        mvwprintw(win, y + PANEL_ROWS - 1, x0 + 2, "%.*s", maxW - 2, "");

    return PANEL_ROWS;
}

int TuiApp::DrawPowerPanel(WINDOW* win, const TuiData& data, int y, int x0, int maxW) {
    if (maxW < 10) return 0;
    // Only show if any power data is available
    bool hasData = (data.cpuPower > 0 || data.gpuPower > 0 || data.anePower > 0);
    if (!hasData) return 0;

    wattron(win, COLOR_PAIR(5) | A_BOLD);
    mvwprintw(win, y, x0, "%.*s", maxW, "Power");
    wattroff(win, COLOR_PAIR(5) | A_BOLD);
    int lines = 1;

    double totalPower = 0.0;
    if (data.cpuPower > 0)
        mvwprintw(win, y + lines++, x0 + 2, "CPU:   %.2f W", data.cpuPower / 1000.0);
    if (data.gpuPower > 0)
        mvwprintw(win, y + lines++, x0 + 2, "GPU:   %.2f W", data.gpuPower / 1000.0);
    mvwprintw(win, y + lines++, x0 + 2, "ANE:   %.2f W", data.anePower / 1000.0);
    totalPower = (data.cpuPower + data.gpuPower + data.anePower) / 1000.0;
    mvwprintw(win, y + lines++, x0 + 4, "Total: %.2f W", totalPower);

    return lines;
}

void TuiApp::Run() {
    setlocale(LC_ALL, "");

    initscr();
    cursesActive_ = true;
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
        if (ch == 'q' || ch == 'Q' || ch == 27) {
            running_ = false;
            break;
        }

        if (rows < 24 || cols < 80) {
            clear();
            mvprintw(0, 0, "Terminal too small. Current: %dx%d", cols, rows);
            refresh();
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }

        TuiData data;
        {
            std::lock_guard<std::mutex> lock(dataMutex_);
            data = data_;
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

        // Header separator
        std::string headerSep(cols - 2, '-');
        mvwprintw(stdscr, 1, 1, "%s", headerSep.c_str());

        // Vertical divider (only in content area, not log area)
        int maxContentRow = rows - 5;
        for (int r = 2; r <= maxContentRow; r++) {
            mvwprintw(stdscr, r, divCol, "|");
        }

        // === Header ===
        DrawHeader(stdscr, data);

        // === Left panels (CPU + GPU + Memory) ===
        int maxY = rows - 5;
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
        }
        if (ly > maxY) ly = maxY;

        // === Right panels (Disk, Net, TPM, Temp) ===
        int ry = 2;
        if (ry < maxY) {
            ry += DrawDiskPanel(stdscr, data, ry, rx, rightW);
            if (ry < maxY) {
                ry += DrawPhysicalDiskPanel(stdscr, data, ry, rx, rightW);
            }
            if (ry < maxY) {
                ry += DrawNetworkPanel(stdscr, data, ry, rx, rightW);
            }
            // WiFi & Bluetooth supplementary info after Network
            if (ry < maxY) {
                ry += DrawWifiBluetoothPanel(stdscr, data, ry, rx, rightW);
            }
            if (ry < maxY) {
                ry += DrawTpmPanel(stdscr, data, ry, rx, rightW);
            }
            if (ry < maxY) {
                ry += DrawTempPanel(stdscr, data, ry, rx, rightW);
            }
            if (ry < maxY) {
                ry += DrawPowerPanel(stdscr, data, ry, rx, rightW);
            }
        }
        if (ry > maxY) ry = maxY;

        // === Bottom panels: Connections → Log → System ===
        // System(3 rows) + Connections(2 rows) are reserved at bottom.
        // Content must not overflow into them.
        int sysTop = rows - 3;
        int connTop = sysTop - 2;
        int contentEnd = ly > ry ? ly : ry;
        // Clip content to not overwrite reserved panels
        if (contentEnd >= connTop) contentEnd = connTop - 1;
        std::string logSep(cols - 2, '-');

        // === Connections panel ===
        bool showConn = (connTop > contentEnd + 1) && data.connectionCount >= 0;
        if (showConn) {
            mvwprintw(stdscr, connTop - 1, 1, "%.*s", cols - 2, logSep.c_str());
            wattron(stdscr, COLOR_PAIR(5) | A_BOLD);
            mvwprintw(stdscr, connTop, 1, "Connections");
            wattroff(stdscr, COLOR_PAIR(5) | A_BOLD);
            if (data.connectionCount > 0) {
                int avaloniaCount = 0, mcpCount = 0, unknownCount = 0;
                for (auto t : data.clientTypes) {
                    if (t == 1) avaloniaCount++;
                    else if (t == 2) mcpCount++;
                    else unknownCount++;
                }
                std::string parts;
                if (avaloniaCount > 0) parts += "Avalonia x" + std::to_string(avaloniaCount) + " ";
                if (mcpCount > 0) parts += "MCP x" + std::to_string(mcpCount) + " ";
                if (unknownCount > 0) parts += "? x" + std::to_string(unknownCount) + " ";
                auto connStr = "IPC: " + parts;
                if (!data.connectionSince.empty())
                    connStr += "since " + data.connectionSince;
                int color = 2;
                wattron(stdscr, COLOR_PAIR(color));
                mvwprintw(stdscr, connTop, 14, "%.*s", cols - 16, connStr.c_str());
                wattroff(stdscr, COLOR_PAIR(color));
            } else {
                mvwprintw(stdscr, connTop, 14, "no clients connected");
            }
        }

        int logEnd = showConn ? connTop - 1 : sysTop;
        int logSpace = logEnd - contentEnd - 2;
        // === Log panel (sacrificial) ===
        if (logSpace > 1) {
            int logTop = contentEnd + 1;
            mvwprintw(stdscr, logTop - 1, 1, "%.*s", cols - 2, logSep.c_str());
            mvwprintw(stdscr, logTop, 1, "Log");
            int logLinesAvail = std::max(0, logEnd - logTop - 1);
            if (logLinesAvail > 0) {
                auto logEntries = logBuf_->GetRecent(logLinesAvail);
                for (size_t i = 0; i < logEntries.size() && static_cast<int>(i) < logLinesAvail; ++i) {
                    const auto& entry = logEntries[i];
                    int color = 2;
                    if (entry.find("[ERROR]") != std::string::npos) color = 4;
                    else if (entry.find("[WARN]") != std::string::npos) color = 3;
                    else if (entry.find("[DEBUG]") != std::string::npos) color = 6;
                    wattron(stdscr, COLOR_PAIR(color));
                    mvwprintw(stdscr, logTop + 1 + static_cast<int>(i), 2, "%.*s",
                              cols - 4, entry.c_str());
                    wattroff(stdscr, COLOR_PAIR(color));
                }
            }
        }

        // === OS / System frame ===
        mvwprintw(stdscr, sysTop - 1, 1, "%.*s", cols - 2, logSep.c_str());
        wattron(stdscr, COLOR_PAIR(5) | A_BOLD);
        mvwprintw(stdscr, sysTop, 1, "System");
        wattroff(stdscr, COLOR_PAIR(5) | A_BOLD);
        if (!data.osVersion.empty()) {
            mvwprintw(stdscr, sysTop, 10, "%.*s", cols - 30, data.osVersion.c_str());
        } else {
            mvwprintw(stdscr, sysTop, 10, "Unknown OS");
        }
        if (data.batteryPercent >= 0 && data.batteryPercent <= 100) {
            auto batStr = (data.acOnline ? "AC" : "BAT") + std::string(" ") + std::to_string(data.batteryPercent) + "%";
            int color = data.batteryPercent < 20 ? 4 : (data.batteryPercent < 50 ? 3 : 2);
            wattron(stdscr, COLOR_PAIR(color));
            mvwprintw(stdscr, sysTop, cols - static_cast<int>(batStr.size()) - 2, "%s", batStr.c_str());
            wattroff(stdscr, COLOR_PAIR(color));
        }

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
