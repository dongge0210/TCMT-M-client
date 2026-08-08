// LogWindow.cpp — see LogWindow.h

#include "LogWindow.h"
#include "LogBuffer.h"

#include <algorithm>

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

}  // namespace

LogWindow::~LogWindow() {
    Shutdown();
}

bool LogWindow::Create(LogBuffer* buffer, const std::wstring& title) {
    buffer_ = buffer;

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSW wc = {};
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    hwnd_ = CreateWindowExW(0, kWindowClass, title.c_str(), WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT, CW_USEDEFAULT, 780, 520,
                            nullptr, nullptr, hInst, this);
    if (!hwnd_) {
        return false;
    }

    ShowWindow(hwnd_, SW_SHOW);
    UpdateWindow(hwnd_);

    thread_ = std::thread([this]() {
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    });
    return true;
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
    case WM_TIMER:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_PAINT:
        if (self) {
            self->OnPaint(hwnd);
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
    case WM_KEYDOWN: {
        if (!self) {
            break;
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

    FillRect(hdc, &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

    if (!font_) {
        font_ = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    }
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, font_));
    SetBkMode(hdc, TRANSPARENT);

    TEXTMETRICW tm = {};
    GetTextMetricsW(hdc, &tm);
    const int rowHeight = (std::max)(1, static_cast<int>(tm.tmHeight + tm.tmExternalLeading + 1));
    const int visibleRows = (std::max)(1, static_cast<int>(rc.bottom - rc.top) / rowHeight);
    const int maxChars = (std::max)(1, static_cast<int>(rc.right - rc.left) /
                                        (std::max)(1, static_cast<int>(tm.tmAveCharWidth)));

    std::vector<std::string> lines;
    if (buffer_) {
        lines = buffer_->GetRecent(LogBuffer::MAX_LINES);
    }

    const int total = static_cast<int>(lines.size());
    if (follow_) {
        scrollOffset_ = 0;
    }
    const int start = (std::max)(0, total - scrollOffset_ - visibleRows);
    const int count = (std::min)(visibleRows, total - start);

    for (int i = 0; i < count; ++i) {
        const std::string& line = lines[start + i];
        const int y = (visibleRows - count + i) * rowHeight;
        SetTextColor(hdc, SeverityColor(line));

        int len = static_cast<int>(line.size());
        if (len > maxChars) {
            len = maxChars;
        }
        TextOutA(hdc, 2, y, line.c_str(), len);
    }

    SelectObject(hdc, oldFont);
    EndPaint(hwnd, &ps);
}

}  // namespace tcmt
