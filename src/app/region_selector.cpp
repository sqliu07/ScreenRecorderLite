#include "app/region_selector.h"

#include <windowsx.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "app/ui_draw.h"
#include "app/ui_text.h"
#include "core/output_size.h"

namespace srl {
namespace {

constexpr wchar_t kOverlayClass[] = L"ScreenRecorderLite.RegionOverlay";
constexpr UINT kSelectionCompleteMessage = WM_APP + 1;

struct Overlay;

struct SelectionState {
    std::vector<std::unique_ptr<Overlay>>* overlays = nullptr;
    bool done = false;
    bool confirmed = false;
    bool dragging = false;
    bool hasSelection = false;
    bool valid = false;
    int selectionMonitor = 0;
    POINT dragStartScreen{};
    POINT currentScreen{};
    RECT selectionScreen{};
    std::optional<RegionSelection> result;
};

struct Overlay {
    HWND hwnd = nullptr;
    SelectionState* state = nullptr;
    MonitorInfo monitor;
    HDC memoryDc = nullptr;
    HBITMAP bitmap = nullptr;
    void* bits = nullptr;
    int width = 0;
    int height = 0;
    HFONT font = nullptr;
    RECT confirmButtonScreen{};
    RECT cancelButtonScreen{};
};

RECT NormalizeRect(POINT a, POINT b) {
    return RECT{std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x),
                std::max(a.y, b.y)};
}

POINT ToScreen(const Overlay& overlay, LPARAM lParam) {
    POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    point.x += overlay.monitor.virtualRect.left;
    point.y += overlay.monitor.virtualRect.top;
    return point;
}

POINT ClampToMonitor(const MonitorInfo& monitor, POINT point) {
    point.x = std::clamp(point.x, static_cast<LONG>(monitor.virtualRect.left),
                         static_cast<LONG>(monitor.virtualRect.right - 1));
    point.y = std::clamp(point.y, static_cast<LONG>(monitor.virtualRect.top),
                         static_cast<LONG>(monitor.virtualRect.bottom - 1));
    return point;
}

void DrawLabel(Overlay& overlay, int centerX, int topY, std::wstring_view text,
               bool warning) {
    if (overlay.bits == nullptr || overlay.memoryDc == nullptr || text.empty()) {
        return;
    }
    HDC dc = overlay.memoryDc;
    const HGDIOBJ oldFont = ::SelectObject(dc, overlay.font);
    SIZE textSize{};
    ::GetTextExtentPoint32W(dc, text.data(), static_cast<int>(text.size()), &textSize);
    const int dpi = static_cast<int>(overlay.monitor.dpiX);
    const int padding = MulDiv(10, dpi, 96);
    const int boxWidth = textSize.cx + padding * 2;
    const int boxHeight = textSize.cy + padding;
    const int left = std::clamp(centerX - boxWidth / 2, 0, std::max(0, overlay.width - boxWidth));
    const int top = std::clamp(topY, 0, std::max(0, overlay.height - boxHeight));
    RECT box{left, top, left + boxWidth, top + boxHeight};

    ::SelectObject(dc, oldFont);
    ui::DrawLayeredPanel(dc, static_cast<std::uint32_t*>(overlay.bits),
        overlay.width, overlay.height, box, text, overlay.font, warning,
        static_cast<UINT>(dpi));
}

void DrawButton(Overlay& overlay, RECT local, std::wstring_view text,
                ui::ButtonStyle style, bool disabled = false) {
    if (overlay.bits == nullptr || overlay.memoryDc == nullptr || text.empty()) {
        return;
    }
    ui::DrawLayeredButton(overlay.memoryDc, static_cast<std::uint32_t*>(overlay.bits),
        overlay.width, overlay.height, local, text, overlay.font,
        style, overlay.monitor.dpiX, disabled);
}

void RenderOverlay(Overlay& overlay, SelectionState& state) {
    if (overlay.bits == nullptr) {
        return;
    }
    auto* pixels = static_cast<std::uint32_t*>(overlay.bits);
    std::fill_n(pixels, static_cast<std::size_t>(overlay.width) * overlay.height,
                0x78000000u);  // premultiplied black, alpha 120

    const int selectionWidth = state.selectionScreen.right - state.selectionScreen.left;
    const int selectionHeight = state.selectionScreen.bottom - state.selectionScreen.top;
    if (state.hasSelection && selectionWidth > 0 && selectionHeight > 0) {
        RECT local{
            state.selectionScreen.left - overlay.monitor.virtualRect.left,
            state.selectionScreen.top - overlay.monitor.virtualRect.top,
            state.selectionScreen.right - overlay.monitor.virtualRect.left,
            state.selectionScreen.bottom - overlay.monitor.virtualRect.top,
        };
        RECT clipped{};
        const RECT bounds{0, 0, overlay.width, overlay.height};
        if (::IntersectRect(&clipped, &local, &bounds)) {
            for (int y = clipped.top; y < clipped.bottom; ++y) {
                // Alpha 1 instead of 0 keeps the selection hole visually
                // transparent while still receiving mouse input, so the user
                // can redraw the rectangle and right-click to cancel.
                std::fill_n(pixels + static_cast<std::size_t>(y) * overlay.width + clipped.left,
                            clipped.right - clipped.left, 0x01000000u);
            }
            const std::uint32_t color = state.valid ? 0xFF0078D7u : 0xFFFF3030u;
            const int thickness = std::max(2, MulDiv(2, static_cast<int>(overlay.monitor.dpiX), 96));
            const int x0 = std::max(0, static_cast<int>(local.left));
            const int y0 = std::max(0, static_cast<int>(local.top));
            const int x1 = std::min(overlay.width, static_cast<int>(local.right));
            const int y1 = std::min(overlay.height, static_cast<int>(local.bottom));
            for (int y = y0; y < y1; ++y) {
                for (int x = x0; x < std::min(x1, x0 + thickness); ++x) {
                    pixels[static_cast<std::size_t>(y) * overlay.width + x] = color;
                }
                for (int x = std::max(x0, x1 - thickness); x < x1; ++x) {
                    pixels[static_cast<std::size_t>(y) * overlay.width + x] = color;
                }
            }
            for (int y = y0; y < std::min(y1, y0 + thickness); ++y) {
                for (int x = x0; x < x1; ++x) {
                    pixels[static_cast<std::size_t>(y) * overlay.width + x] = color;
                }
            }
            for (int y = std::max(y0, y1 - thickness); y < y1; ++y) {
                for (int x = x0; x < x1; ++x) {
                    pixels[static_cast<std::size_t>(y) * overlay.width + x] = color;
                }
            }
        }
    }

    const int dpi = static_cast<int>(overlay.monitor.dpiX);
    std::wstring instruction;
    if (!state.hasSelection) {
        instruction = ui::Text(ui::TextId::SelectRegionInstruction);
    } else if (state.dragging) {
        instruction = state.valid ? ui::Text(ui::TextId::AdjustRegionInstruction)
            : ui::Format(ui::TextId::KeepDraggingMinimum, kMinOutputDimension, kMinOutputDimension);
    } else {
        instruction = state.valid ? ui::Text(ui::TextId::ConfirmRegionInstruction)
            : ui::Format(ui::TextId::InvalidRegionInstruction, kMinOutputDimension, kMinOutputDimension);
    }
    DrawLabel(overlay,
              (overlay.monitor.virtualRect.right - overlay.monitor.virtualRect.left) / 2,
              MulDiv(40, dpi, 96),
              instruction, false);

    if (state.dragging && state.hasSelection &&
        state.selectionMonitor == overlay.monitor.index) {
        const std::wstring sizeText = std::to_wstring(selectionWidth) + L" × " +
                                      std::to_wstring(selectionHeight) + L" px";
        const std::wstring text = state.valid
            ? sizeText : sizeText + ui::Format(ui::TextId::MinimumRegionSuffix, kMinOutputDimension, kMinOutputDimension);
        const int centerX =
            state.currentScreen.x - overlay.monitor.virtualRect.left;
        int topY = state.currentScreen.y - overlay.monitor.virtualRect.top + MulDiv(24, dpi, 96);
        if (topY > overlay.height - MulDiv(150, dpi, 96)) {
            topY = state.currentScreen.y - overlay.monitor.virtualRect.top -
                   MulDiv(64, dpi, 96);
        }
        DrawLabel(overlay, centerX, topY, text, !state.valid);
    }

    // Persistent mouse-confirm buttons: the user should not have to discover
    // that Enter confirms the selection.
    const int buttonWidth = MulDiv(150, dpi, 96);
    const int buttonHeight = MulDiv(40, dpi, 96);
    const int buttonGap = MulDiv(16, dpi, 96);
    const int totalWidth = buttonWidth * 2 + buttonGap;
    const int buttonLeft = std::max(0, (overlay.width - totalWidth) / 2);
    // Keep confirmation controls with the instruction at the top, where they
    // remain visible on every display and never compete with the taskbar.
    const int buttonTop = MulDiv(88, dpi, 96);
    RECT confirmButton{buttonLeft, buttonTop, buttonLeft + buttonWidth,
                       buttonTop + buttonHeight};
    RECT cancelButton{confirmButton.right + buttonGap, buttonTop,
                      confirmButton.right + buttonGap + buttonWidth,
                      buttonTop + buttonHeight};
    overlay.confirmButtonScreen = RECT{
        confirmButton.left + overlay.monitor.virtualRect.left,
        confirmButton.top + overlay.monitor.virtualRect.top,
        confirmButton.right + overlay.monitor.virtualRect.left,
        confirmButton.bottom + overlay.monitor.virtualRect.top,
    };
    overlay.cancelButtonScreen = RECT{
        cancelButton.left + overlay.monitor.virtualRect.left,
        cancelButton.top + overlay.monitor.virtualRect.top,
        cancelButton.right + overlay.monitor.virtualRect.left,
        cancelButton.bottom + overlay.monitor.virtualRect.top,
    };
    const bool confirmEnabled = state.hasSelection && state.valid;
    DrawButton(overlay, confirmButton, ui::Text(ui::TextId::ConfirmRegionButton),
               ui::ButtonStyle::Primary, !confirmEnabled);
    DrawButton(overlay, cancelButton, ui::Text(ui::TextId::CancelRegionButton), ui::ButtonStyle::Secondary);

    std::wstring footer;
    if (state.hasSelection && state.valid && !state.dragging) {
        footer = ui::Format(ui::TextId::SelectedRegionSize, selectionWidth, selectionHeight);
    }
    DrawLabel(overlay,
              (overlay.monitor.virtualRect.right - overlay.monitor.virtualRect.left) / 2,
              buttonTop + buttonHeight + MulDiv(8, dpi, 96), footer, false);

    POINT destination{overlay.monitor.virtualRect.left, overlay.monitor.virtualRect.top};
    SIZE size{overlay.width, overlay.height};
    POINT source{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    HDC screen = ::GetDC(nullptr);
    ::UpdateLayeredWindow(overlay.hwnd, screen, &destination, &size, overlay.memoryDc,
                          &source, 0, &blend, ULW_ALPHA);
    ::ReleaseDC(nullptr, screen);
}

void RenderAll(SelectionState& state) {
    for (auto& overlay : *state.overlays) {
        RenderOverlay(*overlay, state);
    }
}

void UpdateSelection(SelectionState& state, Overlay& overlay) {
    state.currentScreen = ClampToMonitor(overlay.monitor, state.currentScreen);
    state.selectionScreen = NormalizeRect(state.dragStartScreen, state.currentScreen);
    state.hasSelection = true;
    state.valid = state.selectionScreen.right - state.selectionScreen.left >= kMinOutputDimension &&
                  state.selectionScreen.bottom - state.selectionScreen.top >= kMinOutputDimension;
    RenderAll(state);
}

void CompleteSelection(SelectionState& state, bool confirm) {
    if (state.done) {
        return;
    }
    if (confirm) {
        if (!state.hasSelection || !state.valid || state.overlays == nullptr) {
            return;
        }
        for (const auto& overlay : *state.overlays) {
            if (overlay->monitor.index != state.selectionMonitor) {
                continue;
            }
            RECT local{
                state.selectionScreen.left - overlay->monitor.virtualRect.left,
                state.selectionScreen.top - overlay->monitor.virtualRect.top,
                state.selectionScreen.right - overlay->monitor.virtualRect.left,
                state.selectionScreen.bottom - overlay->monitor.virtualRect.top,
            };
            state.result = RegionSelection{
                .monitorIndex = overlay->monitor.index,
                .monitor = overlay->monitor.handle,
                .rect = local,
            };
            break;
        }
        if (!state.result.has_value()) {
            return;
        }
    }
    state.confirmed = confirm;
    state.done = true;
    for (const auto& overlay : *state.overlays) {
        ::PostMessageW(overlay->hwnd, kSelectionCompleteMessage, 0, 0);
    }
}

void MoveSelection(SelectionState& state, int dx, int dy) {
    if (!state.hasSelection || state.dragging || state.overlays == nullptr) {
        return;
    }
    const MonitorInfo* monitor = nullptr;
    for (const auto& overlay : *state.overlays) {
        if (overlay->monitor.index == state.selectionMonitor) {
            monitor = &overlay->monitor;
            break;
        }
    }
    if (monitor == nullptr) {
        return;
    }
    RECT moved = state.selectionScreen;
    const int width = moved.right - moved.left;
    const int height = moved.bottom - moved.top;
    int left = std::clamp<int>(moved.left + dx, monitor->virtualRect.left,
                               monitor->virtualRect.right - width);
    int top = std::clamp<int>(moved.top + dy, monitor->virtualRect.top,
                              monitor->virtualRect.bottom - height);
    state.selectionScreen = RECT{left, top, left + width, top + height};
    state.currentScreen = POINT{moved.right + dx, moved.bottom + dy};
    state.currentScreen = ClampToMonitor(*monitor, state.currentScreen);
    RenderAll(state);
}

LRESULT CALLBACK OverlayProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* overlay = reinterpret_cast<Overlay*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
    if (overlay == nullptr) {
        return ::DefWindowProcW(window, message, wParam, lParam);
    }
    SelectionState& state = *overlay->state;

    switch (message) {
        case WM_SETCURSOR: {
            POINT point{};
            ::GetCursorPos(&point);
            const bool overButton = ::PtInRect(&overlay->confirmButtonScreen, point) ||
                                    ::PtInRect(&overlay->cancelButtonScreen, point);
            ::SetCursor(::LoadCursorW(nullptr, overButton ? IDC_HAND : IDC_CROSS));
            return TRUE;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            ::BeginPaint(window, &paint);
            ::EndPaint(window, &paint);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            const POINT screen = ClampToMonitor(overlay->monitor, ToScreen(*overlay, lParam));
            const int hitInflation = MulDiv(12, static_cast<int>(overlay->monitor.dpiX), 96);
            RECT confirmHit = overlay->confirmButtonScreen;
            RECT cancelHit = overlay->cancelButtonScreen;
            ::InflateRect(&confirmHit, hitInflation, hitInflation);
            ::InflateRect(&cancelHit, hitInflation, hitInflation);
            if (::PtInRect(&confirmHit, screen)) {
                if (state.hasSelection && state.valid) {
                    CompleteSelection(state, true);
                }
                return 0;
            }
            if (::PtInRect(&cancelHit, screen)) {
                CompleteSelection(state, false);
                return 0;
            }
            ::SetForegroundWindow(window);
            ::SetActiveWindow(window);
            ::SetFocus(window);
            ::SetCapture(window);
            state.dragging = true;
            state.hasSelection = false;
            state.valid = false;
            state.selectionMonitor = overlay->monitor.index;
            state.dragStartScreen = screen;
            state.currentScreen = screen;
            state.selectionScreen = RECT{screen.x, screen.y, screen.x, screen.y};
            RenderAll(state);
            return 0;
        }
        case WM_MOUSEMOVE:
            if (state.dragging && ::GetCapture() == window) {
                state.currentScreen = ClampToMonitor(overlay->monitor, ToScreen(*overlay, lParam));
                UpdateSelection(state, *overlay);
            }
            return 0;
        case WM_LBUTTONUP:
            if (state.dragging) {
                state.currentScreen = ClampToMonitor(overlay->monitor, ToScreen(*overlay, lParam));
                state.dragging = false;
                if (::GetCapture() == window) {
                    ::ReleaseCapture();
                }
                UpdateSelection(state, *overlay);
            }
            return 0;
        case WM_RBUTTONDOWN:
            CompleteSelection(state, false);
            return 0;
        case WM_KEYDOWN:
            switch (wParam) {
                case VK_RETURN:
                    CompleteSelection(state, true);
                    return 0;
                case VK_ESCAPE:
                    CompleteSelection(state, false);
                    return 0;
                case VK_LEFT:
                    MoveSelection(state, (::GetKeyState(VK_SHIFT) & 0x8000) ? -10 : -1, 0);
                    return 0;
                case VK_RIGHT:
                    MoveSelection(state, (::GetKeyState(VK_SHIFT) & 0x8000) ? 10 : 1, 0);
                    return 0;
                case VK_UP:
                    MoveSelection(state, 0, (::GetKeyState(VK_SHIFT) & 0x8000) ? -10 : -1);
                    return 0;
                case VK_DOWN:
                    MoveSelection(state, 0, (::GetKeyState(VK_SHIFT) & 0x8000) ? 10 : 1);
                    return 0;
                default:
                    break;
            }
            return 0;
        case WM_CLOSE:
            CompleteSelection(state, false);
            return 0;
        case kSelectionCompleteMessage:
            return 0;
        default:
            return ::DefWindowProcW(window, message, wParam, lParam);
    }
}

bool CreateOverlay(Overlay& overlay, SelectionState& state, HINSTANCE instance) {
    overlay.state = &state;
    const int width = overlay.monitor.virtualRect.right - overlay.monitor.virtualRect.left;
    const int height = overlay.monitor.virtualRect.bottom - overlay.monitor.virtualRect.top;
    overlay.width = width;
    overlay.height = height;
    overlay.hwnd = ::CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kOverlayClass, L"",
        WS_POPUP, overlay.monitor.virtualRect.left, overlay.monitor.virtualRect.top, width,
        height, nullptr, nullptr, instance, nullptr);
    if (overlay.hwnd == nullptr) {
        return false;
    }
    ::SetWindowLongPtrW(overlay.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&overlay));

    HDC screen = ::GetDC(nullptr);
    overlay.memoryDc = ::CreateCompatibleDC(screen);
    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    overlay.bitmap = ::CreateDIBSection(screen, &bitmapInfo, DIB_RGB_COLORS, &overlay.bits,
                                        nullptr, 0);
    ::ReleaseDC(nullptr, screen);
    if (overlay.memoryDc == nullptr || overlay.bitmap == nullptr || overlay.bits == nullptr) {
        return false;
    }
    ::SelectObject(overlay.memoryDc, overlay.bitmap);
    const int fontHeight = -MulDiv(10, static_cast<int>(overlay.monitor.dpiX), 96);
    overlay.font = ::CreateFontW(fontHeight, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                 L"Segoe UI");
    return true;
}

void DestroyOverlay(Overlay& overlay) {
    if (overlay.hwnd != nullptr) {
        ::DestroyWindow(overlay.hwnd);
        overlay.hwnd = nullptr;
    }
    if (overlay.font != nullptr) {
        ::DeleteObject(overlay.font);
        overlay.font = nullptr;
    }
    if (overlay.bitmap != nullptr) {
        ::DeleteObject(overlay.bitmap);
        overlay.bitmap = nullptr;
    }
    if (overlay.memoryDc != nullptr) {
        ::DeleteDC(overlay.memoryDc);
        overlay.memoryDc = nullptr;
    }
}

}  // namespace

std::optional<RegionSelection> SelectRegion(HWND owner,
                                            const std::vector<MonitorInfo>& displays) {
    (void)owner;
    if (displays.empty()) {
        return std::nullopt;
    }

    HINSTANCE instance = ::GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = OverlayProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_CROSS);
    windowClass.lpszClassName = kOverlayClass;
    if (::RegisterClassExW(&windowClass) == 0 &&
        ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return std::nullopt;
    }

    std::vector<std::unique_ptr<Overlay>> overlays;
    SelectionState state;
    state.overlays = &overlays;
    overlays.reserve(displays.size());
    HWND focusWindow = nullptr;

    for (const MonitorInfo& display : displays) {
        auto overlay = std::make_unique<Overlay>();
        overlay->monitor = display;
        if (!CreateOverlay(*overlay, state, instance)) {
            DestroyOverlay(*overlay);
            for (auto& created : overlays) {
                DestroyOverlay(*created);
            }
            return std::nullopt;
        }
        if (focusWindow == nullptr || display.primary) {
            focusWindow = overlay->hwnd;
        }
        overlays.push_back(std::move(overlay));
    }

    for (auto& overlay : overlays) {
        RenderOverlay(*overlay, state);
        ::ShowWindow(overlay->hwnd, SW_SHOW);
    }
    ::SetForegroundWindow(focusWindow);
    ::BringWindowToTop(focusWindow);
    ::SetActiveWindow(focusWindow);
    ::SetFocus(focusWindow);

    MSG message{};
    while (!state.done) {
        const BOOL result = ::GetMessageW(&message, nullptr, 0, 0);
        if (result <= 0) {
            break;
        }
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }

    for (auto& overlay : overlays) {
        DestroyOverlay(*overlay);
    }
    if (state.confirmed && state.result.has_value()) {
        return state.result;
    }
    return std::nullopt;
}

}  // namespace srl
