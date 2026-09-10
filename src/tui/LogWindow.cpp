// LogWindow.cpp — see LogWindow.h

#include "LogWindow.h"
#include "LogBuffer.h"

#include <windowsx.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace tcmt {
namespace {

constexpr wchar_t kWindowClass[] = L"TCMT_LogWindow_Class";
constexpr UINT_PTR kRefreshTimer = 1;
constexpr DWORD kRefreshMs = 300;

COLORREF SeverityColor(const std::string& line) {
    if (line.find("[ERROR]") != std::string::npos ||
        line.find("[FATAL]") != std::string::npos ||
        line.find("[CRITICAL]") != std::string::npos) {
        return RGB(255, 90, 90);
    }
    if (line.find("[WARN]") != std::string::npos) {
        return RGB(255, 200, 60);
    }
    return RGB(200, 200, 200);
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int len = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                        nullptr, 0);
    if (len <= 0) {
        return {};
    }
    std::wstring result(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        result.data(), len);
    return result;
}

}  // namespace

LogWindow::~LogWindow() {
    Shutdown();
}

bool LogWindow::Create(LogBuffer* buffer, const std::wstring& title) {
    buffer_ = buffer;

    // The window must be created on the same thread that runs its message loop:
    // window messages are delivered to the thread that created the window.
    std::promise<bool> created;
    thread_ = std::thread([this, title, &created]() {
        HINSTANCE hInst = GetModuleHandleW(nullptr);
        WNDCLASSW wc = {};
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = kWindowClass;
        if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            created.set_value(false);
            return;
        }

        hwnd_ = CreateWindowExW(0, kWindowClass, title.c_str(), WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, 780, 520,
                                nullptr, nullptr, hInst, this);
        if (!hwnd_) {
            created.set_value(false);
            return;
        }

        ShowWindow(hwnd_, SW_SHOW);
        UpdateWindow(hwnd_);
        created.set_value(true);

        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    });
    return created.get_future().get();
}

void LogWindow::Shutdown() {
    if (hwnd_) {
        PostMessageW(hwnd_, WM_CLOSE, 0, 0);
        hwnd_ = nullptr;
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    if (font_) {
        DeleteObject(font_);
        font_ = nullptr;
    }
}

LogWindow::LogView LogWindow::ComputeView(HWND hwnd) {
    LogView v;
    RECT rc;
    GetClientRect(hwnd, &rc);

    HDC dc = GetDC(hwnd);
    if (!font_) {
        font_ = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    }
    HFONT oldFont = static_cast<HFONT>(SelectObject(dc, font_));
    TEXTMETRICW tm = {};
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, oldFont);
    ReleaseDC(hwnd, dc);

    v.rowHeight = (std::max)(1, static_cast<int>(tm.tmHeight + tm.tmExternalLeading + 1));
    v.visibleRows = (std::max)(1, static_cast<int>(rc.bottom - rc.top) / v.rowHeight);
    v.charWidth = (std::max)(1, static_cast<int>(tm.tmAveCharWidth));
    v.maxChars = (std::max)(1, static_cast<int>(rc.right - rc.left) / v.charWidth);

    if (buffer_) {
        v.lines = buffer_->GetRecent(LogBuffer::MAX_LINES);
    }
    v.total = static_cast<int>(v.lines.size());
    if (follow_) {
        scrollOffset_ = 0;
    }
    v.start = (std::max)(0, v.total - scrollOffset_ - v.visibleRows);
    v.count = (std::min)(v.visibleRows, v.total - v.start);
    return v;
}

bool LogWindow::HitTest(const LogView& view, int x, int y, int& line, int& col) const {
    const int topPad = (view.count < view.visibleRows) ? 0 : (view.visibleRows - view.count);
    const int disp = y / view.rowHeight - topPad;
    if (disp < 0) {
        return false;
    }
    // Walk wrapped display rows to find the logical line + column.
    int row = 0;
    for (int i = 0; i < view.count; ++i) {
        const int lineIdx = view.start + i;
        const std::wstring wline = Utf8ToWide(view.lines[lineIdx]);
        const int len = static_cast<int>(wline.size());
        const int rows = (std::max)(1, (len + view.maxChars - 1) / (std::max)(1, view.maxChars));
        if (disp < row + rows) {
            line = lineIdx;
            const int rInLine = disp - row;
            col = (std::max)(0, (x - 2) / view.charWidth) + rInLine * view.maxChars;
            col = (std::min)(col, len);
            return true;
        }
        row += rows;
    }
    return false;
}

std::wstring LogWindow::BuildSelectionText(const LogView& view) const {
    if (selAnchorLine_ < 0 || selActiveLine_ < 0) {
        return {};
    }
    const int l1 = (std::min)(selAnchorLine_, selActiveLine_);
    const int l2 = (std::max)(selAnchorLine_, selActiveLine_);
    const int c1 = (l1 == selAnchorLine_) ? selAnchorCol_ : selActiveCol_;
    const int c2 = (l2 == selActiveLine_) ? selActiveCol_ : selAnchorCol_;

    std::wstring result;
    for (int li = l1; li <= l2; ++li) {
        if (li < 0 || li >= view.total) {
            continue;
        }
        const std::wstring wline = Utf8ToWide(view.lines[li]);
        const int startC = (li == l1) ? (std::min)(c1, static_cast<int>(wline.size())) : 0;
        const int endC = (li == l2) ? (std::min)(c2, static_cast<int>(wline.size()))
                                    : static_cast<int>(wline.size());
        if (endC <= startC) {
            continue;
        }
        if (!result.empty()) {
            result += L"\r\n";
        }
        result.append(wline, static_cast<size_t>(startC), static_cast<size_t>(endC - startC));
    }
    return result;
}

std::wstring LogWindow::BuildAllText(const LogView& view) const {
    std::wstring result;
    for (const std::string& line : view.lines) {
        if (!result.empty()) {
            result += L"\r\n";
        }
        result += Utf8ToWide(line);
    }
    return result;
}

void LogWindow::CopyToClipboard(const std::wstring& text) {
    if (text.empty() || !hwnd_) {
        return;
    }
    if (!OpenClipboard(hwnd_)) {
        return;
    }
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (hMem) {
        void* dst = GlobalLock(hMem);
        if (dst) {
            memcpy(dst, text.c_str(), bytes);
            GlobalUnlock(hMem);
        }
        SetClipboardData(CF_UNICODETEXT, hMem);
    }
    CloseClipboard();
}

LRESULT CALLBACK LogWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    LogWindow* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        self = static_cast<LogWindow*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<LogWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    switch (msg) {
    case WM_CREATE:
        SetTimer(hwnd, kRefreshTimer, kRefreshMs, nullptr);
        return 0;
    case WM_TIMER: {
        // Repaint only when new log lines arrived (or follow state changed).
        if (self && self->buffer_ && self->lastRenderCount_ != self->buffer_->Size()) {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_ERASEBKGND:
        // Background is fully covered by the double-buffered WM_PAINT.
        return 1;
    case WM_PAINT:
        if (self) {
            self->OnPaint(hwnd);
        }
        return 0;
    case WM_LBUTTONDOWN: {
        if (!self) {
            break;
        }
        self->selecting_ = true;
        SetCapture(hwnd);
        const LogView v = self->ComputeView(hwnd);
        int line = -1, col = 0;
        if (self->HitTest(v, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), line, col)) {
            self->selAnchorLine_ = line;
            self->selAnchorCol_ = col;
            self->selActiveLine_ = line;
            self->selActiveCol_ = col;
        } else {
            self->selAnchorLine_ = self->selActiveLine_ = -1;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!self || !self->selecting_) {
            break;
        }
        const LogView v = self->ComputeView(hwnd);
        int line = -1, col = 0;
        if (self->HitTest(v, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam), line, col)) {
            self->selActiveLine_ = line;
            self->selActiveCol_ = col;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (self) {
            self->selecting_ = false;
            ReleaseCapture();
        }
        return 0;
    case WM_MOUSEWHEEL: {
        if (!self) {
            break;
        }
        short delta = GET_WHEEL_DELTA_WPARAM(wParam);
        if (delta < 0) {
            self->scrollOffset_ += 3;
            self->follow_ = false;
        } else {
            self->scrollOffset_ = (std::max)(0, self->scrollOffset_ - 3);
            if (self->scrollOffset_ == 0) {
                self->follow_ = true;
            }
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_COPY: {
        if (!self) {
            break;
        }
        const LogView v = self->ComputeView(hwnd);
        std::wstring text = self->BuildSelectionText(v);
        if (text.empty()) {
            text = self->BuildAllText(v);
        }
        self->CopyToClipboard(text);
        return 0;
    }
    case WM_KEYDOWN: {
        if (!self) {
            break;
        }
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (ctrl && wParam == 'C') {
            const LogView v = self->ComputeView(hwnd);
            std::wstring text = self->BuildSelectionText(v);
            if (text.empty()) {
                text = self->BuildAllText(v);
            }
            self->CopyToClipboard(text);
            return 0;
        }
        if (ctrl && wParam == 'A') {
            const LogView v = self->ComputeView(hwnd);
            self->selAnchorLine_ = 0;
            self->selAnchorCol_ = 0;
            if (v.total > 0) {
                self->selActiveLine_ = v.total - 1;
                self->selActiveCol_ = static_cast<int>(Utf8ToWide(v.lines[v.total - 1]).size());
            } else {
                self->selActiveLine_ = -1;
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        switch (wParam) {
        case VK_UP:
            self->scrollOffset_++;
            self->follow_ = false;
            break;
        case VK_DOWN:
            self->scrollOffset_ = (std::max)(0, self->scrollOffset_ - 1);
            if (self->scrollOffset_ == 0) {
                self->follow_ = true;
            }
            break;
        case VK_PRIOR:
            self->scrollOffset_ += 10;
            self->follow_ = false;
            break;
        case VK_NEXT:
            self->scrollOffset_ = (std::max)(0, self->scrollOffset_ - 10);
            if (self->scrollOffset_ == 0) {
                self->follow_ = true;
            }
            break;
        case VK_HOME:
        case VK_END:
        case 'F':
            self->follow_ = true;
            self->scrollOffset_ = 0;
            break;
        default:
            break;
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, kRefreshTimer);
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void LogWindow::OnPaint(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int w = (std::max)(1, static_cast<int>(rc.right - rc.left));
    const int h = (std::max)(1, static_cast<int>(rc.bottom - rc.top));

    const LogView view = ComputeView(hwnd);

    // Double buffer: draw everything off-screen, then blit once.
    HDC memDC = CreateCompatibleDC(hdc);
    HBITMAP memBmp = CreateCompatibleBitmap(hdc, w, h);
    HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(memDC, memBmp));

    FillRect(memDC, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

    HFONT oldFont = static_cast<HFONT>(SelectObject(memDC, font_));
    SetBkMode(memDC, TRANSPARENT);

    const bool hasSel = selAnchorLine_ >= 0 && selActiveLine_ >= 0;
    const int selL1 = (std::min)(selAnchorLine_, selActiveLine_);
    const int selL2 = (std::max)(selAnchorLine_, selActiveLine_);

    // Wrapped rendering: long lines flow onto the next display row instead
    // of truncating; when content is shorter than the viewport, rows are
    // top-aligned instead of hugging the bottom.
    const int topPad = (view.count < view.visibleRows) ? 0 : (view.visibleRows - view.count);
    for (int i = 0, row = 0; i < view.count && row < view.visibleRows; ++i) {
        const int lineIdx = view.start + i;
        const std::wstring wline = Utf8ToWide(view.lines[lineIdx]);
        const bool selLine = hasSel && lineIdx >= selL1 && lineIdx <= selL2;
        const COLORREF lineColor = SeverityColor(view.lines[lineIdx]);
        const int len = static_cast<int>(wline.size());
        if (len == 0) {
            ++row; // blank line occupies one row
            continue;
        }
        for (int off = 0; off < len && row < view.visibleRows; off += view.maxChars) {
            const int y = (topPad + row) * view.rowHeight;
            const int chunk = (std::min)(view.maxChars, len - off);
            if (selLine) {
                SetBkMode(memDC, OPAQUE);
                SetBkColor(memDC, RGB(38, 79, 120));
                SetTextColor(memDC, RGB(255, 255, 255));
                TextOutW(memDC, 2, y, wline.c_str() + off, chunk);
                SetBkMode(memDC, TRANSPARENT);
                SetTextColor(memDC, lineColor);
            } else {
                SetTextColor(memDC, lineColor);
                TextOutW(memDC, 2, y, wline.c_str() + off, chunk);
            }
            ++row;
        }
    }

    SelectObject(memDC, oldFont);
    BitBlt(hdc, 0, 0, w, h, memDC, 0, 0, SRCCOPY);

    SelectObject(memDC, oldBmp);
    DeleteObject(memBmp);
    DeleteDC(memDC);

    lastRenderCount_ = buffer_ ? buffer_->Size() : 0;
    EndPaint(hwnd, &ps);
}

}  // namespace tcmt
