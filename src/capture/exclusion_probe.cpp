#include "capture/exclusion_probe.h"

#include <dwmapi.h>
#include <winternl.h>

#include <algorithm>
#include <chrono>
#include <format>

#include "common/win32.h"
#include "capture/wgc_probe.h"
#include "platform/displays.h"

namespace srl {
namespace {

constexpr wchar_t kWindowClass[] = L"ScreenRecorderLite.ExclusionProbe";
constexpr wchar_t kWindowTitle[] = L"ScreenRecorderLite exclusion probe";

LRESULT CALLBACK ProbeWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC dc = ::BeginPaint(window, &paint);
            RECT client{};
            ::GetClientRect(window, &client);
            HBRUSH brush = ::CreateSolidBrush(RGB(255, 0, 255));
            ::FillRect(dc, &client, brush);
            ::DeleteObject(brush);
            ::SetBkMode(dc, TRANSPARENT);
            ::SetTextColor(dc, RGB(0, 0, 0));
            ::DrawTextW(dc, L"SRL exclusion probe", -1, &client,
                        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            ::EndPaint(window, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        default:
            return ::DefWindowProcW(window, message, wParam, lParam);
    }
}

}  // namespace

ExclusionProbeResult RunExclusionProbe(const ExclusionProbeOptions& options) {
    ExclusionProbeResult result;
    const std::vector<MonitorInfo> displays = EnumerateDisplays();
    const MonitorInfo* monitor = nullptr;
    if (options.monitorIndex == 0) {
        monitor = PrimaryMonitor(displays);
    } else {
        for (const MonitorInfo& candidate : displays) {
            if (candidate.index == options.monitorIndex) {
                monitor = &candidate;
                break;
            }
        }
    }
    if (monitor == nullptr) {
        result.error = L"selected display was not found";
        return result;
    }

    const DWORD build = [] {
        using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
        const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
        if (ntdll == nullptr) {
            return 0UL;
        }
        const auto rtlGetVersion =
            reinterpret_cast<RtlGetVersionFn>(::GetProcAddress(ntdll, "RtlGetVersion"));
        if (rtlGetVersion == nullptr) {
            return 0UL;
        }
        RTL_OSVERSIONINFOW info{};
        info.dwOSVersionInfoSize = sizeof(info);
        return SUCCEEDED(rtlGetVersion(&info)) ? info.dwBuildNumber : 0UL;
    }();
    result.supportedBuild = build >= 19041;

    HINSTANCE instance = ::GetModuleHandleW(nullptr);
    HBRUSH background = ::CreateSolidBrush(RGB(255, 0, 255));
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = ProbeWindowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = background;
    windowClass.lpszClassName = kWindowClass;
    if (::RegisterClassExW(&windowClass) == 0) {
        result.error = Utf8ToWide(std::format("RegisterClassExW failed: {}",
                                              HResultText(::HRESULT_FROM_WIN32(
                                                  ::GetLastError()))));
        return result;
    }

    const int width = 360;
    const int height = 96;
    const int x = monitor->workRect.left + (monitor->workRect.right -
                                            monitor->workRect.left - width) /
                                               2;
    const int y = monitor->workRect.top + 64;
    HWND window = ::CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kWindowClass, kWindowTitle,
        WS_POPUP, x, y, width, height, nullptr, nullptr, instance, nullptr);
    if (window == nullptr) {
        result.error = Utf8ToWide(std::format("CreateWindowExW failed: {}",
                                              HResultText(::HRESULT_FROM_WIN32(
                                                  ::GetLastError()))));
        ::UnregisterClassW(kWindowClass, instance);
        ::DeleteObject(background);
        return result;
    }
    result.windowCreated = true;
    ::GetWindowRect(window, &result.windowRect);
    ::ShowWindow(window, SW_SHOWNOACTIVATE);
    ::UpdateWindow(window);

    ::SetLastError(ERROR_SUCCESS);
    result.affinitySet =
        ::SetWindowDisplayAffinity(window, WDA_EXCLUDEFROMCAPTURE) != FALSE;
    result.affinityError = result.affinitySet ? ERROR_SUCCESS : ::GetLastError();
    if (!result.affinitySet) {
        result.error = Utf8ToWide(std::format(
            "SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) failed: {}",
            HResultText(::HRESULT_FROM_WIN32(result.affinityError))));
    }

    MSG message{};
    const auto pumpFor = [&message](int milliseconds) {
        const auto start = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - start <
               std::chrono::milliseconds(milliseconds)) {
            while (::PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE) {
                ::TranslateMessage(&message);
                ::DispatchMessageW(&message);
            }
            ::Sleep(10);
        }
    };

    pumpFor(200);
    ::DwmFlush();

    if (options.verifyWithCapture) {
        RECT intersection{};
        if (!::IntersectRect(&intersection, &result.windowRect, &monitor->virtualRect) ||
            ::IsRectEmpty(&intersection)) {
            result.detail = L"the test window is outside the selected monitor";
        } else {
            const int insetX = std::min(24, static_cast<int>((intersection.right - intersection.left) / 3));
            const int insetY = std::min(24, static_cast<int>((intersection.bottom - intersection.top) / 3));
            std::vector<POINT> samplePoints = {
                {intersection.left + insetX - monitor->virtualRect.left,
                 intersection.top + insetY - monitor->virtualRect.top},
                {intersection.right - insetX - monitor->virtualRect.left,
                 intersection.top + insetY - monitor->virtualRect.top},
                {intersection.left + insetX - monitor->virtualRect.left,
                 intersection.bottom - insetY - monitor->virtualRect.top},
                {intersection.right - insetX - monitor->virtualRect.left,
                 intersection.bottom - insetY - monitor->virtualRect.top},
            };
            WgcProbeOptions captureOptions;
            captureOptions.monitorIndex = monitor->index;
            captureOptions.timeoutMs = 3000;
            captureOptions.samplePoints = samplePoints;
            const WgcProbeResult capture = RunWgcProbe(captureOptions);
            result.capturedSamples = capture.samples;
            if (!capture.ok) {
                result.detail = L"end-to-end capture failed: " + capture.error;
            } else if (capture.samples.empty()) {
                result.detail = L"end-to-end capture returned no samples";
            } else {
                bool sawWindowColor = false;
                for (const std::uint32_t sample : capture.samples) {
                    const std::uint8_t b = static_cast<std::uint8_t>(sample & 0xFF);
                    const std::uint8_t g = static_cast<std::uint8_t>((sample >> 8) & 0xFF);
                    const std::uint8_t r = static_cast<std::uint8_t>((sample >> 16) & 0xFF);
                    if (r > 200 && g < 80 && b > 200) {
                        sawWindowColor = true;
                        break;
                    }
                }
                result.captureVerified = !sawWindowColor;
                result.detail = sawWindowColor
                                    ? L"the capture still contains the magenta test window; "
                                      L"WDA_EXCLUDEFROMCAPTURE did not take effect"
                                    : L"the WGC frame does not contain the magenta test window; "
                                      L"exclusion verified end-to-end";
            }
        }
    }

    pumpFor(options.visibleDurationMs);

    ::SetWindowDisplayAffinity(window, WDA_NONE);
    ::DestroyWindow(window);
    ::UnregisterClassW(kWindowClass, instance);
    ::DeleteObject(background);

    result.ok = result.windowCreated && result.affinitySet && result.supportedBuild &&
                (!options.verifyWithCapture || result.captureVerified);
    if (result.affinitySet && !result.supportedBuild) {
        result.detail = L"WDA_EXCLUDEFROMCAPTURE requires Windows 10 2004 (build 19041) or "
                        L"newer; this build is older.";
    } else if (result.affinitySet && !options.verifyWithCapture) {
        result.detail = L"API accepted the exclusion. Pass --verify for the end-to-end WGC "
                        L"pixel check.";
    }
    return result;
}

void PrintExclusionProbe(const ExclusionProbeResult& result) {
    ConsoleLine("Capture exclusion");
    ConsoleLine(std::format("  build >= 19041    {}", result.supportedBuild ? "yes" : "no"));
    ConsoleLine(std::format("  window            {} at ({},{})-({},{})",
                            result.windowCreated ? "created" : "not created",
                            result.windowRect.left, result.windowRect.top,
                            result.windowRect.right, result.windowRect.bottom));
    ConsoleLine(std::format("  WDA_EXCLUDEFROMCAPTURE {}",
                            result.affinitySet ? "accepted" : "FAILED"));
    if (!result.affinitySet) {
        ConsoleLine(std::format("  win32 error       {}", result.affinityError));
    }
    if (result.captureVerified) {
        ConsoleLine("  capture verified  yes (no magenta window pixels in the WGC frame)");
    } else if (!result.capturedSamples.empty()) {
        ConsoleLine("  capture verified  no");
        for (std::size_t i = 0; i < result.capturedSamples.size(); ++i) {
            ConsoleLine(std::format("    sample #{}        0x{:08X}", i + 1,
                                    result.capturedSamples[i]));
        }
    }
    if (!result.detail.empty()) {
        ConsoleLine(std::format("  detail            {}", WideToUtf8(result.detail)));
    }
    if (!result.error.empty()) {
        ConsoleLine(std::format("  error             {}", WideToUtf8(result.error)));
    }
    ConsoleLine(std::format("  result            {}", result.ok ? "API OK" : "FAILED"));
}

}  // namespace srl
