#include "probe/test_pattern.h"

#include <algorithm>
#include <array>

#include "common/win32.h"

namespace probe {
namespace {

constexpr wchar_t kTestPatternClass[] = L"ScreenRecorderLite.TestPattern";

LRESULT CALLBACK PatternWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = ::BeginPaint(window, &paint);
            RECT client{};
            ::GetClientRect(window, &client);
            const int width = client.right - client.left;
            const int height = client.bottom - client.top;
            const ULONGLONG now = ::GetTickCount64();
            const int scroll = static_cast<int>((now / 16) % std::max(1, width));
            static constexpr std::array<COLORREF, 8> kBars = {
                RGB(255, 0, 0),   RGB(255, 255, 0), RGB(0, 255, 0),
                RGB(0, 255, 255), RGB(0, 0, 255),   RGB(255, 0, 255),
                RGB(255, 255, 255), RGB(0, 0, 0)};
            const int barWidth = std::max(1, width / 8);
            for (int bar = -1; bar < 9; ++bar) {
                RECT rect{};
                rect.left = bar * barWidth - (scroll % barWidth);
                rect.right = rect.left + barWidth;
                rect.top = 0;
                rect.bottom = height;
                if (rect.right <= 0 || rect.left >= width) {
                    continue;
                }
                HBRUSH brush = ::CreateSolidBrush(kBars[(bar + 8) % 8]);
                ::FillRect(dc, &rect, brush);
                ::DeleteObject(brush);
            }
            const int blockWidth = 120;
            const int blockX = static_cast<int>((now / 10) % std::max(1, width - blockWidth));
            RECT block{blockX, height / 2 - 60, blockX + blockWidth, height / 2 + 60};
            HBRUSH white = static_cast<HBRUSH>(::GetStockObject(WHITE_BRUSH));
            ::FillRect(dc, &block, white);
            const std::wstring text = L"SRL M1 TEST PATTERN  " + std::to_wstring(width) +
                                      L"  " + std::to_wstring(height);
            ::SetBkMode(dc, TRANSPARENT);
            ::SetTextColor(dc, RGB(0, 0, 0));
            RECT textRect{0, height / 2 - 20, width, height / 2 + 20};
            ::DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &textRect,
                        DT_CENTER | DT_SINGLELINE | DT_VCENTER);
            ::EndPaint(window, &paint);
            return 0;
        }
        case WM_DESTROY:
            return 0;
        default:
            return ::DefWindowProcW(window, message, wParam, lParam);
    }
}

}  // namespace

bool CreateTestPatternWindow(const srl::MonitorInfo& monitor, TestPatternWindow& window,
                             std::wstring* error) {
    HINSTANCE instance = ::GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = PatternWindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = static_cast<HBRUSH>(::GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = kTestPatternClass;
    ::RegisterClassExW(&windowClass);

    const int workWidth = monitor.workRect.right - monitor.workRect.left;
    const int workHeight = monitor.workRect.bottom - monitor.workRect.top;
    int width = 640;
    int height = 360;
    if (width > workWidth) {
        width = std::max(64, workWidth * 2 / 3);
    }
    if (height > workHeight) {
        height = std::max(64, workHeight * 2 / 3);
    }
    width &= ~1;
    height &= ~1;
    const int x = monitor.workRect.left + (workWidth - width) / 2;
    const int y = monitor.workRect.top + (workHeight - height) / 2;

    window.window = ::CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kTestPatternClass,
        L"ScreenRecorderLite M1 test pattern", WS_POPUP, x, y, width, height, nullptr,
        nullptr, instance, nullptr);
    if (window.window == nullptr) {
        if (error != nullptr) {
            *error = L"无法创建测试图窗口：";
            *error += srl::Utf8ToWide(srl::HResultText(
                HRESULT_FROM_WIN32(::GetLastError())));
        }
        return false;
    }
    ::GetWindowRect(window.window, &window.screenRect);
    window.monitorLocalRect = RECT{
        window.screenRect.left - monitor.virtualRect.left,
        window.screenRect.top - monitor.virtualRect.top,
        window.screenRect.right - monitor.virtualRect.left,
        window.screenRect.bottom - monitor.virtualRect.top,
    };
    ::ShowWindow(window.window, SW_SHOWNOACTIVATE);
    ::UpdateWindow(window.window);
    return true;
}

bool ResizeTestPatternWindow(const srl::MonitorInfo& monitor, int width, int height,
                             TestPatternWindow& window, std::wstring* error) {
    if (window.window == nullptr) {
        if (error != nullptr) {
            *error = L"测试图窗口尚未创建";
        }
        return false;
    }
    width &= ~1;
    height &= ~1;
    const int x = monitor.virtualRect.left +
                  ((monitor.virtualRect.right - monitor.virtualRect.left) - width) / 2;
    const int y = monitor.virtualRect.top +
                  ((monitor.virtualRect.bottom - monitor.virtualRect.top) - height) / 2;
    if (!::SetWindowPos(window.window, HWND_TOPMOST, x, y, width, height,
                        SWP_SHOWWINDOW | SWP_NOACTIVATE)) {
        if (error != nullptr) {
            *error = L"无法调整测试图窗口大小";
        }
        return false;
    }
    ::GetWindowRect(window.window, &window.screenRect);
    window.monitorLocalRect = RECT{
        window.screenRect.left - monitor.virtualRect.left,
        window.screenRect.top - monitor.virtualRect.top,
        window.screenRect.right - monitor.virtualRect.left,
        window.screenRect.bottom - monitor.virtualRect.top,
    };
    ::InvalidateRect(window.window, nullptr, FALSE);
    return true;
}

void PumpTestPatternWindow(const TestPatternWindow& window) {
    MSG message{};
    while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }
    if (window.window != nullptr) {
        ::InvalidateRect(window.window, nullptr, FALSE);
    }
}

void DestroyTestPatternWindow(TestPatternWindow& window) {
    if (window.window != nullptr) {
        ::DestroyWindow(window.window);
        window.window = nullptr;
    }
}

}  // namespace probe
