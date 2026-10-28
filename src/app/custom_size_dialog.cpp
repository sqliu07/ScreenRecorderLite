#include "app/custom_size_dialog.h"

#include "app/ui_draw.h"
#include "app/ui_text.h"

#include <commctrl.h>
#include <windowsx.h>

#include <cwchar>
#include <cwctype>
#include <string>
#include <utility>

#include "core/output_size.h"

namespace srl {
namespace {

constexpr wchar_t kDialogClass[] = L"ScreenRecorderLite.CustomSizeDialog";
constexpr int kIdWidth = 2001;
constexpr int kIdHeight = 2002;
constexpr int kIdPreview = 2003;
constexpr int kIdError = 2004;
constexpr int kIdOk = IDOK;
constexpr int kIdCancel = IDCANCEL;

struct State {
    HWND window = nullptr;
    HWND owner = nullptr;
    HWND widthEdit = nullptr;
    HWND heightEdit = nullptr;
    HWND preview = nullptr;
    HWND error = nullptr;
    int sourceWidth = 0;
    int sourceHeight = 0;
    Size initial{};
    Size result{};
    bool accepted = false;
    bool done = false;
};

int Scale(UINT dpi, int value) {
    return MulDiv(value, static_cast<int>(dpi), 96);
}

bool ParseDimension(HWND edit, int& value) {
    wchar_t text[16]{};
    ::GetWindowTextW(edit, text, ARRAYSIZE(text));
    const wchar_t* cursor = text;
    while (std::iswspace(*cursor)) {
        ++cursor;
    }
    if (*cursor == L'\0') {
        return false;
    }
    wchar_t* end = nullptr;
    const long parsed = std::wcstol(cursor, &end, 10);
    if (end == cursor || parsed < kMinOutputDimension || parsed > kMaxOutputDimension) {
        return false;
    }
    while (std::iswspace(*end)) {
        ++end;
    }
    if (*end != L'\0') {
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

void SetFont(HWND control, HFONT font) {
    if (control != nullptr && font != nullptr) {
        ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}

void UpdatePreview(State& state) {
    int width = 0;
    int height = 0;
    if (!ParseDimension(state.widthEdit, width) || !ParseDimension(state.heightEdit, height)) {
        ::SetWindowTextW(state.preview, ui::Text(ui::TextId::OutputSizeUnavailable).c_str());
        ::SetWindowTextW(state.error, ui::Format(ui::TextId::CustomDimensionRange, kMinOutputDimension, kMaxOutputDimension).c_str());
        return;
    }
    if ((width % 2) != 0 || (height % 2) != 0) {
        ::SetWindowTextW(state.preview, ui::Text(ui::TextId::OutputSizeUnavailable).c_str());
        ::SetWindowTextW(state.error, ui::Text(ui::TextId::EvenDimensionsRequired).c_str());
        return;
    }
    std::wstring error;
    const auto output = ComputeOutputSize(
        OutputSizeRequest{state.sourceWidth, state.sourceHeight, ResolutionLimit::Custom,
                          Size{width, height}},
        &error);
    if (!output.has_value()) {
        ::SetWindowTextW(state.preview, ui::Text(ui::TextId::OutputSizeUnavailable).c_str());
        ::SetWindowTextW(state.error, ui::Text(ui::TextId::CustomSizeUnavailable).c_str());
        return;
    }
    const std::wstring text = ui::Format(ui::TextId::OutputSizePreview,
        output->output.width, output->output.height);
    ::SetWindowTextW(state.preview, text.c_str());
    ::SetWindowTextW(state.error, L"");
}

void CloseDialog(State& state) {
    if (state.done) {
        return;
    }
    state.done = true;
    if (state.window != nullptr) {
        ::DestroyWindow(state.window);
        state.window = nullptr;
    }
}

void Accept(State& state) {
    int width = 0;
    int height = 0;
    if (!ParseDimension(state.widthEdit, width) || !ParseDimension(state.heightEdit, height)) {
        ::SetWindowTextW(state.error, ui::Format(ui::TextId::CustomDimensionRange, kMinOutputDimension, kMaxOutputDimension).c_str());
        ::SetFocus(state.widthEdit);
        return;
    }
    if ((width % 2) != 0 || (height % 2) != 0) {
        ::SetWindowTextW(state.error, ui::Text(ui::TextId::EvenDimensionsRequired).c_str());
        return;
    }
    std::wstring error;
    if (!ComputeOutputSize(OutputSizeRequest{state.sourceWidth, state.sourceHeight,
                                             ResolutionLimit::Custom,
                                             Size{width, height}},
                           &error)
             .has_value()) {
        ::SetWindowTextW(state.error, ui::Text(ui::TextId::CustomSizeUnavailable).c_str());
        return;
    }
    state.result = Size{width, height};
    state.accepted = true;
    CloseDialog(state);
}

void Layout(State& state) {
    const UINT dpi = ::GetDpiForWindow(state.window);
    const int margin = Scale(dpi, 20);
    const int labelWidth = Scale(dpi, 44);
    const int rowHeight = Scale(dpi, 30);
    const int fieldWidth = Scale(dpi, 120);
    const int gap = Scale(dpi, 12);
    const int fieldX = margin + labelWidth + gap;
    const int row1 = margin + Scale(dpi, 56);
    const int row2 = row1 + rowHeight + Scale(dpi, 10);
    ::MoveWindow(state.widthEdit, fieldX, row1, fieldWidth, rowHeight, TRUE);
    ::MoveWindow(state.heightEdit, fieldX, row2, fieldWidth, rowHeight, TRUE);
    ::MoveWindow(state.preview, fieldX + fieldWidth + Scale(dpi, 16), row2,
                 Scale(dpi, 200), Scale(dpi, 48), TRUE);
    ::MoveWindow(state.error, margin, row2 + rowHeight + Scale(dpi, 8), Scale(dpi, 380),
                 Scale(dpi, 22), TRUE);
    const int buttonWidth = Scale(dpi, 88);
    const int buttonHeight = Scale(dpi, 32);
    RECT client{};
    ::GetClientRect(state.window, &client);
    const int y = client.bottom - margin - buttonHeight;
    ::MoveWindow(::GetDlgItem(state.window, kIdOk),
                 client.right - margin - buttonWidth, y, buttonWidth, buttonHeight, TRUE);
    ::MoveWindow(::GetDlgItem(state.window, kIdCancel),
                 client.right - margin - buttonWidth * 2 - gap, y, buttonWidth,
                 buttonHeight, TRUE);
}

LRESULT CALLBACK DialogProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<State*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message) {
        case WM_CREATE: {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lParam);
            state = reinterpret_cast<State*>(create->lpCreateParams);
            state->window = window;
            ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
            const HFONT font =
                reinterpret_cast<HFONT>(::SendMessageW(state->owner, WM_GETFONT, 0, 0));
            const UINT dpi = ::GetDpiForWindow(window);
            const int margin = Scale(dpi, 20);
            HWND hint = ::CreateWindowExW(
                0, L"STATIC",
                ui::Text(ui::TextId::CustomSizeHint).c_str(),
                WS_CHILD | WS_VISIBLE | SS_LEFT, margin, margin,
                Scale(dpi, 380), Scale(dpi, 22), window, nullptr, nullptr, nullptr);
            HWND widthLabel = ::CreateWindowExW(
                0, L"STATIC", ui::Text(ui::TextId::Width).c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT, margin,
                margin + Scale(dpi, 56) + Scale(dpi, 4), Scale(dpi, 40),
                Scale(dpi, 24), window, nullptr, nullptr, nullptr);
            state->widthEdit = ::CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_NUMBER |
                    ES_AUTOHSCROLL | WS_TABSTOP,
                0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdWidth)), nullptr, nullptr);
            HWND heightLabel = ::CreateWindowExW(
                0, L"STATIC", ui::Text(ui::TextId::Height).c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT, margin,
                margin + Scale(dpi, 56) + Scale(dpi, 30) + Scale(dpi, 12) +
                    Scale(dpi, 4),
                Scale(dpi, 40), Scale(dpi, 24), window, nullptr, nullptr, nullptr);
            state->heightEdit = ::CreateWindowExW(
                WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_NUMBER |
                    ES_AUTOHSCROLL | WS_TABSTOP,
                0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdHeight)), nullptr, nullptr);
            state->preview = ::CreateWindowExW(
                0, L"STATIC", ui::Text(ui::TextId::OutputSizeUnavailable).c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0,
                0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdPreview)), nullptr, nullptr);
            state->error = ::CreateWindowExW(
                0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdError)), nullptr, nullptr);
            HWND cancel = ::CreateWindowExW(
                0, L"BUTTON", ui::Text(ui::TextId::Cancel).c_str(), WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
                0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdCancel)), nullptr, nullptr);
            HWND ok = ::CreateWindowExW(
                0, L"BUTTON", ui::Text(ui::TextId::Save).c_str(), WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
                0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdOk)),
                nullptr, nullptr);
            for (HWND control : {hint, widthLabel, state->widthEdit, heightLabel,
                                 state->heightEdit, state->preview, state->error, cancel,
                                 ok}) {
                SetFont(control, font);
            }
            const std::wstring widthText = std::to_wstring(state->initial.width);
            const std::wstring heightText = std::to_wstring(state->initial.height);
            ::SetWindowTextW(state->widthEdit, widthText.c_str());
            ::SetWindowTextW(state->heightEdit, heightText.c_str());
            Layout(*state);
            UpdatePreview(*state);
            ::SetFocus(state->widthEdit);
            return 0;
        }
        case WM_COMMAND:
            if (state != nullptr) {
                switch (LOWORD(wParam)) {
                    case kIdOk:
                        Accept(*state);
                        return 0;
                    case kIdCancel:
                        CloseDialog(*state);
                        return 0;
                    case kIdWidth:
                    case kIdHeight:
                        if (HIWORD(wParam) == EN_CHANGE) {
                            UpdatePreview(*state);
                        }
                        return 0;
                    default:
                        break;
                }
            }
            break;
        case WM_CLOSE:
            if (state != nullptr) {
                CloseDialog(*state);
                return 0;
            }
            break;
        case WM_CTLCOLORSTATIC: {
            HDC dc = reinterpret_cast<HDC>(wParam);
            ::SetBkMode(dc, TRANSPARENT);
            if (state != nullptr && reinterpret_cast<HWND>(lParam) == state->error) {
                ::SetTextColor(dc, RGB(184, 38, 32));
            }
            return reinterpret_cast<LRESULT>(::GetSysColorBrush(COLOR_WINDOW));
        }
        case WM_DRAWITEM: {
            const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (item->CtlType == ODT_BUTTON) {
                const ui::ButtonStyle style =
                    item->CtlID == kIdOk ? ui::ButtonStyle::Primary
                                         : ui::ButtonStyle::Secondary;
                ui::DrawModernButton(*item, style, ::GetDpiForWindow(window));
                return TRUE;
            }
            break;
        }
        case WM_ERASEBKGND: {
            RECT client{};
            ::GetClientRect(window, &client);
            ::FillRect(reinterpret_cast<HDC>(wParam), &client,
                       ::GetSysColorBrush(COLOR_WINDOW));
            return 1;
        }
        case WM_SETFOCUS:
            if (state != nullptr && state->widthEdit != nullptr) {
                ::SetFocus(state->widthEdit);
            }
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            const UINT dpi = ::GetDpiForWindow(window);
            RECT rect{0, 0, Scale(dpi, 420), Scale(dpi, 260)};
            ::AdjustWindowRectExForDpi(&rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                       FALSE, WS_EX_DLGMODALFRAME, dpi);
            info->ptMinTrackSize.x = info->ptMaxTrackSize.x = rect.right - rect.left;
            info->ptMinTrackSize.y = info->ptMaxTrackSize.y = rect.bottom - rect.top;
            return 0;
        }
        default:
            break;
    }
    return ::DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace

bool CustomSizeDialog::Show(HWND owner, HINSTANCE instance, int sourceWidth,
                            int sourceHeight, Size initial, Size* result) {
    if (owner == nullptr || result == nullptr) {
        return false;
    }
    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = DialogProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.lpszClassName = kDialogClass;
    if (::RegisterClassExW(&windowClass) == 0 &&
        ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        return false;
    }

    State state;
    state.owner = owner;
    state.sourceWidth = sourceWidth;
    state.sourceHeight = sourceHeight;
    state.initial = initial;
    const UINT dpi = ::GetDpiForWindow(owner);
    RECT windowRect{0, 0, Scale(dpi, 420), Scale(dpi, 260)};
    ::AdjustWindowRectExForDpi(&windowRect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, FALSE,
                               WS_EX_DLGMODALFRAME, dpi);
    RECT ownerRect{};
    ::GetWindowRect(owner, &ownerRect);
    const int width = windowRect.right - windowRect.left;
    const int height = windowRect.bottom - windowRect.top;
    const int x = ownerRect.left + ((ownerRect.right - ownerRect.left) - width) / 2;
    const int y = ownerRect.top + ((ownerRect.bottom - ownerRect.top) - height) / 2;

    ::EnableWindow(owner, FALSE);
    state.window = ::CreateWindowExW(
        WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, kDialogClass,
        ui::Text(ui::TextId::CustomResolutionTitle).c_str(),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, x, y, width, height, owner, nullptr,
        instance, &state);
    if (state.window == nullptr) {
        ::EnableWindow(owner, TRUE);
        return false;
    }
    ::ShowWindow(state.window, SW_SHOW);
    ::UpdateWindow(state.window);

    MSG message{};
    while (!state.done) {
        const BOOL got = ::GetMessageW(&message, nullptr, 0, 0);
        if (got <= 0) {
            break;
        }
        if (message.message == WM_KEYDOWN) {
            if (message.wParam == VK_RETURN) {
                Accept(state);
                continue;
            }
            if (message.wParam == VK_ESCAPE) {
                CloseDialog(state);
                continue;
            }
        }
        if (message.hwnd == state.window || ::IsChild(state.window, message.hwnd)) {
            if (::IsDialogMessageW(state.window, &message) != FALSE) {
                continue;
            }
        }
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }
    if (state.window != nullptr) {
        ::DestroyWindow(state.window);
    }
    ::EnableWindow(owner, TRUE);
    ::SetForegroundWindow(owner);
    if (state.accepted) {
        *result = state.result;
    }
    return state.accepted;
}

}  // namespace srl
