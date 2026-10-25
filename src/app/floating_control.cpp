#include "app/floating_control.h"

#include <windowsx.h>
#include <dwmapi.h>

#include <algorithm>
#include <string_view>

#include "app/resource.h"
#include "app/ui_draw.h"
#include "app/ui_text.h"
#include "common/win32.h"

namespace srl {
namespace {

constexpr wchar_t kFloatingClass[] = L"ScreenRecorderLite.FloatingControl";
constexpr wchar_t kRegionOutlineClass[] = L"ScreenRecorderLite.RecordingOutline";
constexpr int kActionButtonId = 1001;
constexpr int kScreenshotButtonId = 1002;
constexpr int kAudioButtonId = 1003;
constexpr int kMicrophoneButtonId = 1004;
constexpr UINT_PTR kMicrophoneMeterTimerId = 2;
constexpr int kBaseWidth = 344;
constexpr int kBaseHeight = 64;

int Scale(UINT dpi, int value) {
    return MulDiv(value, static_cast<int>(dpi), 96);
}

LRESULT CALLBACK RegionOutlineProc(HWND window, UINT message, WPARAM wParam,
                                   LPARAM lParam) {
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC dc = ::BeginPaint(window, &paint);
        RECT bounds{};
        ::GetClientRect(window, &bounds);
        ::SetDCBrushColor(dc, RGB(0, 103, 192));
        ::FillRect(dc, &bounds, static_cast<HBRUSH>(::GetStockObject(DC_BRUSH)));
        ::EndPaint(window, &paint);
        return 0;
    }
    return ::DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace

FloatingControl::FloatingControl() = default;

FloatingControl::~FloatingControl() {
    Destroy();
}

bool FloatingControl::Create(HINSTANCE instance, Action action, Action closeAction,
                             Action screenshotAction, AudioAction audioAction,
                             MicrophoneLevel microphoneLevel,
                             std::wstring* error) {
    action_ = std::move(action);
    closeAction_ = std::move(closeAction);
    screenshotAction_ = std::move(screenshotAction);
    audioAction_ = std::move(audioAction);
    microphoneLevelProvider_ = std::move(microphoneLevel);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    // The floating control is the taskbar entry while the main window is
    // hidden. Keep the class icons alive for the process: Windows does not
    // own icons loaded without LR_SHARED, and RegisterClassEx just keeps the
    // handles supplied by the first registration.
    static HICON largeIcon = static_cast<HICON>(::LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, ::GetSystemMetrics(SM_CXICON),
        ::GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR));
    static HICON smallIcon = static_cast<HICON>(::LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
        ::GetSystemMetrics(SM_CXSMICON), ::GetSystemMetrics(SM_CYSMICON),
        LR_DEFAULTCOLOR));
    windowClass.hIcon = largeIcon != nullptr ? largeIcon : ::LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hIconSm =
        smallIcon != nullptr ? smallIcon : ::LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = static_cast<HBRUSH>(::GetStockObject(WHITE_BRUSH));
    windowClass.lpszClassName = kFloatingClass;
    if (::RegisterClassExW(&windowClass) == 0 &&
        ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        if (error != nullptr) {
            *error = L"无法注册悬浮窗类";
        }
        return false;
    }

    window_ = ::CreateWindowExW(
        WS_EX_TOPMOST, kFloatingClass, ui::Text(L"轻录 - 录制控制").c_str(),
        WS_POPUP, 0, 0, Scale(96, kBaseWidth), Scale(96, kBaseHeight), nullptr, nullptr,
        instance, this);
    if (window_ == nullptr) {
        if (error != nullptr) {
            *error = L"无法创建悬浮控制窗";
        }
        return false;
    }
    ::SetWindowLongPtrW(window_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    dpi_ = ::GetDpiForWindow(window_);
    statusLabel_ = ::CreateWindowExW(0, L"STATIC", L"3", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS |
                                         SS_CENTERIMAGE, 0, 0, 0, 0, window_, nullptr,
                                     instance, nullptr);
    actionButton_ = ::CreateWindowExW(0, L"BUTTON", ui::Text(L"取消").c_str(),
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                      0, 0, 0, 0, window_,
                                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(
                                          kActionButtonId)),
                                      instance, nullptr);
    screenshotButton_ = ::CreateWindowExW(0, L"BUTTON", ui::Text(L"截图").c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kScreenshotButtonId)), instance, nullptr);
    audioButton_ = ::CreateWindowExW(0, L"BUTTON", ui::Text(L"系统声音").c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAudioButtonId)), instance, nullptr);
    microphoneButton_ = ::CreateWindowExW(0, L"BUTTON", ui::Text(L"麦克风").c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(kMicrophoneButtonId)), instance, nullptr);
    RecreateFonts();
    tooltips_ = ::CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT, window_, nullptr, instance, nullptr);
    for (HWND button : {audioButton_, microphoneButton_, screenshotButton_, actionButton_}) {
        ::SetWindowSubclass(button, ButtonProc, 1, reinterpret_cast<DWORD_PTR>(this));
        if (tooltips_ != nullptr) {
            TOOLINFOW info{};
            info.cbSize = sizeof(info);
            info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            info.hwnd = window_;
            info.uId = reinterpret_cast<UINT_PTR>(button);
            info.lpszText = const_cast<wchar_t*>(L"");
            ::SendMessageW(tooltips_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
        }
    }
    HWND previous = HWND_TOP;
    for (HWND button : {audioButton_, microphoneButton_, screenshotButton_, actionButton_}) {
        ::SetWindowPos(button, previous, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        previous = button;
    }
    SetToolTip(screenshotButton_, ui::Text(L"截图当前画面").c_str());
    SetToolTip(actionButton_, ui::Text(L"取消录制").c_str());
    SetRecordingControls(false, false, false, false);
    const DWM_WINDOW_CORNER_PREFERENCE corners = DWMWCP_ROUND;
    ::DwmSetWindowAttribute(window_, DWMWA_WINDOW_CORNER_PREFERENCE, &corners, sizeof(corners));
    Layout();
    ::SetLastError(ERROR_SUCCESS);
    exclusionApplied_ =
        ::SetWindowDisplayAffinity(window_, WDA_EXCLUDEFROMCAPTURE) != FALSE;
    exclusionError_ = exclusionApplied_ ? ERROR_SUCCESS : ::GetLastError();
    return true;
}

void FloatingControl::Destroy() {
    DestroyRegionOutline();
    if (window_ != nullptr) {
        ::DestroyWindow(window_);
        window_ = nullptr;
    }
    if (font_ != nullptr) {
        ::DeleteObject(font_);
        font_ = nullptr;
    }
    if (statusFont_ != nullptr) {
        ::DeleteObject(statusFont_);
        statusFont_ = nullptr;
    }
}

void FloatingControl::RecreateFonts() {
    const HFONT oldBody = font_;
    const HFONT oldStatus = statusFont_;
    const auto create = [this](int weight) {
        return ::CreateFontW(-MulDiv(10, static_cast<int>(dpi_), 72), 0, 0, 0,
            weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
            L"Segoe UI Variable Text");
    };
    font_ = create(FW_NORMAL);
    statusFont_ = create(FW_SEMIBOLD);
    UpdateFonts();
    if (oldBody != nullptr) ::DeleteObject(oldBody);
    if (oldStatus != nullptr) ::DeleteObject(oldStatus);
}

void FloatingControl::UpdateFonts() {
    ::SendMessageW(statusLabel_, WM_SETFONT, reinterpret_cast<WPARAM>(statusFont_), TRUE);
    for (HWND button : {audioButton_, microphoneButton_, screenshotButton_, actionButton_}) {
        ::SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    }
}

void FloatingControl::SetToolTip(HWND control, const wchar_t* text) {
    if (tooltips_ == nullptr) return;
    TOOLINFOW info{};
    info.cbSize = sizeof(info);
    info.hwnd = window_;
    info.uId = reinterpret_cast<UINT_PTR>(control);
    info.lpszText = const_cast<wchar_t*>(text);
    ::SendMessageW(tooltips_, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&info));
}

void FloatingControl::Layout() {
    if (window_ == nullptr) return;
    RECT client{};
    ::GetClientRect(window_, &client);
    const int padding = Scale(dpi_, 12);
    const int gap = Scale(dpi_, 4);
    const int buttonSize = Scale(dpi_, 36);
    const int buttonX = client.right - padding - buttonSize * 4 - gap * 3;
    const int buttonY = (client.bottom - buttonSize) / 2;
    const int labelX = Scale(dpi_, 28);
    ::MoveWindow(statusLabel_, labelX, padding, buttonX - labelX - gap,
        client.bottom - padding * 2, TRUE);
    int x = buttonX;
    for (HWND button : {audioButton_, microphoneButton_, screenshotButton_, actionButton_}) {
        ::MoveWindow(button, x, buttonY, buttonSize, buttonSize, TRUE);
        x += buttonSize + gap;
    }
}

void FloatingControl::SetContent(std::wstring_view status, bool showAction,
                                 std::wstring_view actionText) {
    if (window_ == nullptr) return;
    const std::wstring localizedStatus = ui::Text(status);
    if (statusText_ != localizedStatus) {
        statusText_ = localizedStatus;
        ::SetWindowTextW(statusLabel_, statusText_.c_str());
    }
    const std::wstring newAction = ui::Text(showAction ? actionText : L"保存中");
    if (showAction != actionVisible_ || newAction != actionText_) {
        actionVisible_ = showAction;
        actionText_ = newAction;
        ::SetWindowTextW(actionButton_, actionText_.c_str());
        ::EnableWindow(actionButton_, showAction ? TRUE : FALSE);
        SetToolTip(actionButton_, ui::Text(showAction && actionText == L"停止"
            ? L"停止并保存" : showAction ? L"取消录制" : L"正在保存").c_str());
        ::InvalidateRect(actionButton_, nullptr, TRUE);
    }
    if (!showAction || actionText != L"停止") {
        SetRecordingControls(false, audioPending_, systemEnabled_, micEnabled_);
    }
}

void FloatingControl::SetRecordingControls(bool recording, bool audioPending,
                                           bool systemEnabled, bool micEnabled) {
    SetRegionOutlineVisible(recording);
    const bool changed = recording_ != recording || audioPending_ != audioPending ||
        systemEnabled_ != systemEnabled || micEnabled_ != micEnabled;
    recording_ = recording;
    audioPending_ = audioPending;
    systemEnabled_ = systemEnabled;
    micEnabled_ = micEnabled;
    if (window_ == nullptr) return;
    if (changed) {
        ::SetWindowTextW(audioButton_, ui::Text(systemEnabled ? L"关闭系统声音" : L"开启系统声音").c_str());
        ::SetWindowTextW(microphoneButton_, ui::Text(micEnabled ? L"关闭麦克风" : L"开启麦克风").c_str());
        SetToolTip(audioButton_, ui::Text(audioPending ? L"正在切换声音…"
            : systemEnabled ? L"关闭系统声音" : L"开启系统声音").c_str());
        SetToolTip(microphoneButton_, ui::Text(audioPending ? L"正在切换声音…"
            : micEnabled ? L"关闭麦克风" : L"开启麦克风").c_str());
        ::InvalidateRect(audioButton_, nullptr, TRUE);
        ::InvalidateRect(microphoneButton_, nullptr, TRUE);
    }
    ::EnableWindow(audioButton_, recording && !audioPending ? TRUE : FALSE);
    ::EnableWindow(microphoneButton_, recording && !audioPending ? TRUE : FALSE);
    ::EnableWindow(screenshotButton_, recording ? TRUE : FALSE);
    const bool animateMicrophone = recording && !audioPending && micEnabled;
    if (animateMicrophone && !meterTimerRunning_) {
        ::SetTimer(window_, kMicrophoneMeterTimerId, 50, nullptr);
        meterTimerRunning_ = true;
    } else if (!animateMicrophone && meterTimerRunning_) {
        ::KillTimer(window_, kMicrophoneMeterTimerId);
        meterTimerRunning_ = false;
        microphoneLevel_ = 0.0f;
        ::InvalidateRect(microphoneButton_, nullptr, TRUE);
    }
    if (changed) ::InvalidateRect(window_, nullptr, FALSE);
}

void FloatingControl::SetMicrophoneLevel(float level) {
    if (!meterTimerRunning_ || window_ == nullptr) return;
    const float value = std::clamp(level, 0.0f, 1.0f);
    microphoneLevel_ = value >= microphoneLevel_ ? value : microphoneLevel_ * 0.82f;
}

void FloatingControl::SetPosition(const MonitorInfo& monitor, const RECT* avoidScreenRect) {
    if (window_ == nullptr) {
        return;
    }
    if (dpi_ != monitor.dpiX) {
        dpi_ = monitor.dpiX;
        RecreateFonts();
    }
    SetRegionOutline(avoidScreenRect);
    const int width = Scale(dpi_, kBaseWidth);
    const int height = Scale(dpi_, kBaseHeight);
    const int margin = Scale(dpi_, 16);
    const RECT work = monitor.workRect;
    int x = work.left + (work.right - work.left - width) / 2;
    int y = work.top + margin;

    if (avoidScreenRect != nullptr) {
        const RECT& avoid = *avoidScreenRect;
        const int gap = Scale(dpi_, 8);
        if (avoid.bottom + gap + height <= work.bottom) {
            y = avoid.bottom + gap;
            x = std::clamp(avoid.left, work.left, work.right - width);
        } else if (avoid.top - gap - height >= work.top) {
            y = avoid.top - gap - height;
            x = std::clamp(avoid.left, work.left, work.right - width);
        } else if (avoid.right + gap + width <= work.right) {
            x = avoid.right + gap;
            y = std::clamp(avoid.bottom - height, work.top, work.bottom - height);
        } else if (avoid.left - gap - width >= work.left) {
            x = avoid.left - gap - width;
            y = std::clamp(avoid.bottom - height, work.top, work.bottom - height);
        }
    }

    x = std::clamp(x, static_cast<int>(work.left),
                   std::max(static_cast<int>(work.left),
                            static_cast<int>(work.right) - width));
    y = std::clamp(y, static_cast<int>(work.top),
                   std::max(static_cast<int>(work.top),
                            static_cast<int>(work.bottom) - height));
    ::SetWindowPos(window_, HWND_TOPMOST, x, y, width, height,
                   SWP_NOACTIVATE | SWP_SHOWWINDOW);
    Layout();
    ::InvalidateRect(window_, nullptr, TRUE);
}

void FloatingControl::DestroyRegionOutline() {
    for (HWND& edge : regionOutline_) {
        if (edge != nullptr) ::DestroyWindow(edge);
        edge = nullptr;
    }
    regionOutlineVisible_ = false;
}

void FloatingControl::SetRegionOutlineVisible(bool visible) {
    if (visible == regionOutlineVisible_) return;
    for (HWND edge : regionOutline_) {
        if (edge != nullptr) ::ShowWindow(edge, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
    }
    regionOutlineVisible_ = visible;
}

void FloatingControl::SetRegionOutline(const RECT* screenRect) {
    DestroyRegionOutline();
    if (screenRect == nullptr || ::IsRectEmpty(screenRect)) return;
    HINSTANCE instance = ::GetModuleHandleW(nullptr);
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = RegionOutlineProc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kRegionOutlineClass;
    if (::RegisterClassW(&windowClass) == 0 &&
        ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;

    const RECT& area = *screenRect;
    const int thickness = std::max(2, Scale(dpi_, 2));
    const std::array<RECT, 4> edges{{
        {area.left, area.top, area.right, area.top + thickness},
        {area.left, area.bottom - thickness, area.right, area.bottom},
        {area.left, area.top, area.left + thickness, area.bottom},
        {area.right - thickness, area.top, area.right, area.bottom},
    }};
    for (std::size_t i = 0; i < edges.size(); ++i) {
        const RECT& edge = edges[i];
        regionOutline_[i] = ::CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT |
                WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kRegionOutlineClass, nullptr, WS_POPUP, edge.left, edge.top,
            edge.right - edge.left, edge.bottom - edge.top,
            nullptr, nullptr, instance, nullptr);
        if (regionOutline_[i] == nullptr ||
            !::SetLayeredWindowAttributes(regionOutline_[i], 0, 255, LWA_ALPHA) ||
            !::SetWindowDisplayAffinity(regionOutline_[i], WDA_EXCLUDEFROMCAPTURE)) {
            // Only show an indicator when Windows confirms it is excluded.
            DestroyRegionOutline();
            return;
        }
    }
    SetRegionOutlineVisible(recording_);
}

void FloatingControl::Show() {
    if (window_ != nullptr) {
        ::ShowWindow(window_, SW_SHOWNOACTIVATE);
    }
}

void FloatingControl::Hide() {
    if (window_ != nullptr) {
        ::ShowWindow(window_, SW_HIDE);
    }
}

void FloatingControl::OnPaint() {
    PAINTSTRUCT paint{};
    HDC dc = ::BeginPaint(window_, &paint);
    RECT client{};
    ::GetClientRect(window_, &client);
    ::FillRect(dc, &client, ::GetSysColorBrush(COLOR_WINDOW));
    ui::DrawRoundedPanel(dc, client, ::GetSysColor(COLOR_WINDOW),
        ::GetSysColor(COLOR_3DSHADOW), Scale(dpi_, 12));
    HBRUSH dot = ::CreateSolidBrush(recording_ ? RGB(196, 43, 28) : RGB(110, 116, 126));
    HGDIOBJ oldBrush = ::SelectObject(dc, dot);
    HGDIOBJ oldPen = ::SelectObject(dc, ::GetStockObject(NULL_PEN));
    ::Ellipse(dc, Scale(dpi_, 12), Scale(dpi_, 28), Scale(dpi_, 20), Scale(dpi_, 36));
    ::SelectObject(dc, oldPen);
    ::SelectObject(dc, oldBrush);
    ::DeleteObject(dot);
    ::EndPaint(window_, &paint);
}

LRESULT CALLBACK FloatingControl::ButtonProc(HWND window, UINT message, WPARAM wParam,
    LPARAM lParam, UINT_PTR subclassId, DWORD_PTR reference) {
    auto* self = reinterpret_cast<FloatingControl*>(reference);
    if (message == WM_MOUSEMOVE && self->hoveredButton_ != window) {
        if (self->hoveredButton_ != nullptr) ::InvalidateRect(self->hoveredButton_, nullptr, TRUE);
        self->hoveredButton_ = window;
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
        ::TrackMouseEvent(&tracking);
        ::InvalidateRect(window, nullptr, TRUE);
    } else if (message == WM_MOUSELEAVE && self->hoveredButton_ == window) {
        self->hoveredButton_ = nullptr;
        ::InvalidateRect(window, nullptr, TRUE);
    } else if (message == WM_NCDESTROY) {
        ::RemoveWindowSubclass(window, ButtonProc, subclassId);
    }
    return ::DefSubclassProc(window, message, wParam, lParam);
}

LRESULT CALLBACK FloatingControl::WindowProc(HWND window, UINT message, WPARAM wParam,
                                             LPARAM lParam) {
    auto* self = reinterpret_cast<FloatingControl*>(
        ::GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
        case WM_COMMAND:
            if (self != nullptr && HIWORD(wParam) == BN_CLICKED) {
                switch (LOWORD(wParam)) {
                    case kActionButtonId:
                        if (self->action_) self->action_();
                        break;
                    case kScreenshotButtonId:
                        if (self->recording_ && self->screenshotAction_) self->screenshotAction_();
                        break;
                    case kAudioButtonId:
                        if (self->recording_ && !self->audioPending_ && self->audioAction_) self->audioAction_(true);
                        break;
                    case kMicrophoneButtonId:
                        if (self->recording_ && !self->audioPending_ && self->audioAction_) self->audioAction_(false);
                        break;
                }
            }
            return 0;
        case WM_NCHITTEST: {
            const LRESULT hit = ::DefWindowProcW(window, message, wParam, lParam);
            // Without capture exclusion the region fallback is safe only at
            // the position checked before starting. Do not allow dragging it
            // back into the recorded region.
            return hit == HTCLIENT && self != nullptr && self->exclusionApplied_
                       ? HTCAPTION : hit;
        }
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            ::SetBkMode(dc, TRANSPARENT);
            ::SetTextColor(dc, ::GetSysColor(COLOR_WINDOWTEXT));
            return reinterpret_cast<LRESULT>(::GetSysColorBrush(COLOR_WINDOW));
        }
        case WM_DRAWITEM: {
            const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (self != nullptr && item->CtlType == ODT_BUTTON) {
                const ui::ControlIcon icon = item->CtlID == kAudioButtonId ? ui::ControlIcon::SystemAudio
                    : item->CtlID == kMicrophoneButtonId ? ui::ControlIcon::Microphone
                    : item->CtlID == kScreenshotButtonId ? ui::ControlIcon::Screenshot
                    : self->actionText_ == ui::Text(L"取消")
                        ? ui::ControlIcon::Cancel : ui::ControlIcon::Stop;
                const bool active = item->CtlID == kAudioButtonId ? self->systemEnabled_
                    : item->CtlID == kMicrophoneButtonId ? self->micEnabled_ : true;
                ui::DrawIconButton(*item, icon, active,
                    self->hoveredButton_ == item->hwndItem,
                    item->CtlID == kMicrophoneButtonId ? self->microphoneLevel_ : 0.0f,
                    self->microphoneAnimationPhase_, self->dpi_);
                return TRUE;
            }
            break;
        }
        case WM_DPICHANGED:
            if (self != nullptr) {
                self->dpi_ = HIWORD(wParam);
                self->RecreateFonts();
                const auto* rect = reinterpret_cast<const RECT*>(lParam);
                ::SetWindowPos(window, nullptr, rect->left, rect->top,
                    rect->right - rect->left, rect->bottom - rect->top,
                    SWP_NOZORDER | SWP_NOACTIVATE);
                self->Layout();
            }
            return 0;
        case WM_TIMER:
            if (self != nullptr && wParam == kMicrophoneMeterTimerId &&
                self->meterTimerRunning_) {
                if (self->microphoneLevelProvider_ != nullptr) {
                    self->SetMicrophoneLevel(self->microphoneLevelProvider_());
                }
                self->microphoneAnimationPhase_ += 0.42f;
                if (self->microphoneAnimationPhase_ >= 6.2831853f)
                    self->microphoneAnimationPhase_ -= 6.2831853f;
                ::InvalidateRect(self->microphoneButton_, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT:
            if (self != nullptr) {
                self->OnPaint();
            }
            return 0;
        case WM_SETCURSOR:
            ::SetCursor(::LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        case WM_CLOSE:
            if (self != nullptr && self->closeAction_) {
                self->closeAction_();
            } else if (self != nullptr && self->action_) {
                self->action_();
            }
            return 0;
        case WM_DESTROY:
            return 0;
        default:
            return ::DefWindowProcW(window, message, wParam, lParam);
    }
    return ::DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace srl
