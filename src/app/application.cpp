#include "app/application.h"

#include <commctrl.h>
#include <knownfolders.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <format>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "app/custom_size_dialog.h"
#include "app/floating_control.h"
#include "app/region_selector.h"
#include "app/resource.h"
#include "app/screenshot.h"
#include "app/settings.h"
#include "app/ui_draw.h"
#include "app/ui_text.h"
#include "audio/audio_probe.h"
#include "common/win32.h"
#include "core/output_size.h"
#include "core/storage.h"
#include "platform/displays.h"
#include "recording/recorder.h"

namespace srl {
namespace {

using Microsoft::WRL::ComPtr;

enum ControlId : int {
    kModeFull = 100,
    kModeRegion,
    kMonitorCombo,
    kSelectRegionButton,
    kResolutionCombo,
    kFpsCombo,
    kQualityCombo,
    kAudioCheck,
    kMicCheck,
    kSystemDeviceCombo,
    kMicDeviceCombo,
    kOutputPath,
    kBrowseButton,
    kStartButton,
    kOpenFolderButton,
    kScreenshotButton,
    kRegionSummary,
    kOutputSummary,
    kStatusLabel,
    kTitleLabel,
    kSubtitleLabel,
    kSectionRange,
    kSectionQuality,
    kSectionAudio,
    kSectionOutput,
    kLabelMonitor,
    kLabelResolution,
    kLabelFps,
    kLabelQuality,
    kQualityHint,
    kOutputLabel,
    kLabelOutput,
    kLanguageLabel,
    kLanguageCombo,
};

constexpr wchar_t kMainClass[] = L"ScreenRecorderLite.MainWindow";
constexpr wchar_t kSingleInstanceName[] = L"Local\\ScreenRecorderLite.SingleInstance";
constexpr UINT_PTR kUiTimerId = 1;
constexpr int kHotkeyStartStop = 1;
constexpr int kHotkeyScreenshot = 2;
constexpr int kHotkeyFloating = 3;
constexpr int kDefaultClientWidth = 640;
constexpr int kDefaultClientHeight = 700;
constexpr int kMinimumClientWidth = 600;
constexpr int kMinimumClientHeight = 700;
constexpr UINT kCancelCountdownMessage = WM_APP + 42;

constexpr COLORREF kColorBackground = RGB(244, 246, 249);
constexpr COLORREF kColorCard = RGB(255, 255, 255);
constexpr COLORREF kColorCardBorder = RGB(225, 228, 233);
constexpr COLORREF kColorText = RGB(31, 35, 40);
constexpr COLORREF kColorMuted = RGB(96, 103, 112);
constexpr COLORREF kColorAccent = RGB(0, 103, 192);
constexpr COLORREF kColorSeparator = RGB(234, 237, 241);
constexpr COLORREF kColorError = RGB(184, 38, 32);

struct QualityOption {
    const wchar_t* name;
    int bitrateMbps;
};

constexpr QualityOption kQualityOptions[] = {
    {L"极高", 20},
    {L"高", 12},
    {L"中", 8},
    {L"低", 4},
};
constexpr int kQualityCount = static_cast<int>(std::size(kQualityOptions));
constexpr int kDefaultQualityIndex = 2;

int Scale(UINT dpi, int value) {
    return MulDiv(value, static_cast<int>(dpi), 96);
}

int FontHeight(UINT dpi, int points) {
    return -MulDiv(points, static_cast<int>(dpi), 72);
}

std::wstring FormatDuration(std::int64_t hundredNanoseconds) {
    const std::int64_t totalSeconds =
        std::max<std::int64_t>(0, hundredNanoseconds) / 10'000'000;
    const std::int64_t hours = totalSeconds / 3600;
    const std::int64_t minutes = (totalSeconds / 60) % 60;
    const std::int64_t seconds = totalSeconds % 60;
    return Utf8ToWide(std::format("{:02}:{:02}:{:02}", hours, minutes, seconds));
}

std::wstring Trim(std::wstring value) {
    while (!value.empty() && std::iswspace(value.front())) {
        value.erase(value.begin());
    }
    while (!value.empty() && std::iswspace(value.back())) {
        value.pop_back();
    }
    return value;
}

std::wstring DefaultOutputDirectory() {
    try {
        std::filesystem::path path = KnownFolderPath(FOLDERID_Videos);
        path /= L"ScreenRecorderLite";
        std::string error;
        if (!EnsureDirectory(path, &error)) {
            return KnownFolderPath(FOLDERID_Videos);
        }
        return path.wstring();
    } catch (...) {
        return GetTempDirectory();
    }
}

bool PickFolder(HWND owner, std::wstring& selected) {
    ComPtr<IFileDialog> dialog;
    HRESULT hr = ::CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(dialog.GetAddressOf()));
    if (FAILED(hr)) {
        return false;
    }
    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) {
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                           FOS_PATHMUSTEXIST);
    }
    const std::wstring title = ui::Text(L"选择导出目录");
    dialog->SetTitle(title.c_str());
    if (FAILED(dialog->Show(owner))) {
        return false;
    }
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(item.GetAddressOf()))) {
        return false;
    }
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || path == nullptr) {
        return false;
    }
    selected = path;
    ::CoTaskMemFree(path);
    return true;
}

std::wstring FileNameOf(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

bool HighContrast() {
    HIGHCONTRASTW hc{};
    hc.cbSize = sizeof(hc);
    return ::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) != FALSE &&
           (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

int MessageBoxUi(HWND owner, std::wstring_view message, std::wstring_view title,
                 UINT flags) {
    const std::wstring localizedMessage = ui::Text(message);
    const std::wstring localizedTitle = ui::Text(title);
    return ::MessageBoxW(owner, localizedMessage.c_str(), localizedTitle.c_str(), flags);
}

void ThemeControl(HWND control) {
    if (control != nullptr) {
        ::SetWindowTheme(control, L"Explorer", nullptr);
    }
}

HICON LoadAppIcon(HINSTANCE instance, int width, int height) {
    HICON icon = static_cast<HICON>(::LoadImageW(
        instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, width, height,
        LR_DEFAULTCOLOR));
    if (icon == nullptr) {
        icon = ::LoadIconW(nullptr, IDI_APPLICATION);
    }
    return icon;
}

}  // namespace

class Application {
public:
    explicit Application(HINSTANCE instance) : instance_(instance) {}
    ~Application() {
        StopAndCleanup();
        if (titleFont_ != nullptr) {
            ::DeleteObject(titleFont_);
        }
        if (headingFont_ != nullptr) {
            ::DeleteObject(headingFont_);
        }
        if (bodyFont_ != nullptr) {
            ::DeleteObject(bodyFont_);
        }
        if (smallFont_ != nullptr) {
            ::DeleteObject(smallFont_);
        }
        if (cardBrush_ != nullptr) {
            ::DeleteObject(cardBrush_);
        }
        if (backgroundBrush_ != nullptr) {
            ::DeleteObject(backgroundBrush_);
        }
        if (comInitialized_) {
            ::CoUninitialize();
        }
    }

    int Run(int showCommand) {
        AppSettings startupSettings;
        (void)LoadAppSettings(startupSettings);
        ui::InitializeLanguage(startupSettings.uiLanguage);
        const HRESULT comResult = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(comResult)) {
            MessageBoxUi(nullptr, L"无法初始化 COM，程序无法启动。", L"轻录",
                          MB_ICONERROR | MB_OK);
            return 1;
        }
        comInitialized_ = true;

        INITCOMMONCONTROLSEX commonControls{};
        commonControls.dwSize = sizeof(commonControls);
        commonControls.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
        ::InitCommonControlsEx(&commonControls);

        if (!CreateMainWindow(showCommand)) {
            return 1;
        }

        MSG message{};
        while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (floating_ != nullptr &&
                (message.hwnd == floating_->hwnd() || ::IsChild(floating_->hwnd(), message.hwnd)) &&
                ::IsDialogMessageW(floating_->hwnd(), &message)) {
                continue;
            }
            ::TranslateMessage(&message);
            ::DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }

private:
    static LRESULT CALLBACK StaticWindowProc(HWND window, UINT message, WPARAM wParam,
                                             LPARAM lParam) {
        auto* self = reinterpret_cast<Application*>(
            ::GetWindowLongPtrW(window, GWLP_USERDATA));
        if (self != nullptr) {
            return self->WindowProc(window, message, wParam, lParam);
        }
        return ::DefWindowProcW(window, message, wParam, lParam);
    }

    bool CreateMainWindow(int showCommand) {
        cardBrush_ = ::CreateSolidBrush(kColorCard);
        backgroundBrush_ = ::CreateSolidBrush(kColorBackground);

        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.style = CS_HREDRAW | CS_VREDRAW;
        windowClass.lpfnWndProc = StaticWindowProc;
        windowClass.hInstance = instance_;
        const int largeIconWidth = ::GetSystemMetrics(SM_CXICON);
        const int largeIconHeight = ::GetSystemMetrics(SM_CYICON);
        const int smallIconWidth = ::GetSystemMetrics(SM_CXSMICON);
        const int smallIconHeight = ::GetSystemMetrics(SM_CYSMICON);
        HICON largeIcon = LoadAppIcon(instance_, largeIconWidth, largeIconHeight);
        HICON smallIcon = LoadAppIcon(instance_, smallIconWidth, smallIconHeight);
        windowClass.hIcon = largeIcon;
        windowClass.hIconSm = smallIcon;
        windowClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        windowClass.hbrBackground = backgroundBrush_;
        windowClass.lpszClassName = kMainClass;
        if (::RegisterClassExW(&windowClass) == 0 &&
            ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return false;
        }

        const DWORD style = WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX;
        const std::wstring windowTitle = ui::Text(L"轻录");
        window_ = ::CreateWindowExW(0, kMainClass, windowTitle.c_str(), style, CW_USEDEFAULT,
                                    CW_USEDEFAULT, 420, 420, nullptr, nullptr, instance_,
                                    nullptr);
        if (window_ == nullptr) {
            return false;
        }
        if (largeIcon != nullptr) {
            ::SendMessageW(window_, WM_SETICON, ICON_BIG,
                           reinterpret_cast<LPARAM>(largeIcon));
        }
        if (smallIcon != nullptr) {
            ::SendMessageW(window_, WM_SETICON, ICON_SMALL,
                           reinterpret_cast<LPARAM>(smallIcon));
        }
        ::SetWindowLongPtrW(window_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

        dpi_ = ::GetDpiForWindow(window_);
        RECT desiredClient{0, 0, Scale(dpi_, kDefaultClientWidth),
                           Scale(dpi_, kDefaultClientHeight)};
        ::AdjustWindowRectExForDpi(&desiredClient, style, FALSE, 0, dpi_);
        ::SetWindowPos(window_, nullptr, 0, 0,
                       desiredClient.right - desiredClient.left,
                       desiredClient.bottom - desiredClient.top,
                       SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        RecreateFont();
        CreateControls();

        RECT work{};
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        ::GetMonitorInfoW(::MonitorFromWindow(window_, MONITOR_DEFAULTTOPRIMARY), &info);
        work = info.rcWork;
        RECT current{};
        ::GetWindowRect(window_, &current);
        const int width = current.right - current.left;
        const int height = current.bottom - current.top;
        ::SetWindowPos(window_, nullptr,
                       work.left + (work.right - work.left - width) / 2,
                       work.top + (work.bottom - work.top - height) / 2, 0, 0,
                       SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

        RebuildMonitorList();
        RecreateFont();
        RefreshAudioDevices();
        outputDirectory_ = DefaultOutputDirectory();
        ::SendMessageW(resolutionCombo_, CB_SETCURSEL, 2, 0);
        ::SendMessageW(fpsCombo_, CB_SETCURSEL, 0, 0);
        ::SendMessageW(qualityCombo_, CB_SETCURSEL, kDefaultQualityIndex, 0);
        ::SendMessageW(audioCheck_, BM_SETCHECK, BST_CHECKED, 0);
        ::SendMessageW(micCheck_, BM_SETCHECK, BST_UNCHECKED, 0);
        ::SetWindowTextW(outputPath_, outputDirectory_.c_str());

        AppSettings settings;
        if (LoadAppSettings(settings)) {
            ApplySettings(settings);
        }
        UpdateDerivedState(true);
        UpdateUiState();
        Layout();

        ::SetTimer(window_, kUiTimerId, 200, nullptr);
        hotkeyStartStop_ =
            ::RegisterHotKey(window_, kHotkeyStartStop, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT,
                             VK_F9) != FALSE;
        hotkeyScreenshot_ =
            ::RegisterHotKey(window_, kHotkeyScreenshot, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT,
                             VK_F10) != FALSE;
        hotkeyFloating_ =
            ::RegisterHotKey(window_, kHotkeyFloating, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT,
                             VK_F11) != FALSE;
        UpdateDerivedState(true);
        if (!hotkeyScreenshot_) {
            SetStatus(L"截图快捷键 Ctrl+Alt+F10 不可用；仍可使用截图按钮。");
        } else if (!hotkeyStartStop_) {
            SetStatus(L"快捷键 Ctrl+Alt+F9 不可用；仍可使用开始/停止按钮。");
        }

        ::ShowWindow(window_, showCommand == SW_SHOWMINIMIZED ? SW_SHOWNORMAL : showCommand);
        ::UpdateWindow(window_);
        return true;
    }

    void RecreateFont() {
        if (titleFont_ != nullptr) {
            ::DeleteObject(titleFont_);
        }
        if (headingFont_ != nullptr) {
            ::DeleteObject(headingFont_);
        }
        if (bodyFont_ != nullptr) {
            ::DeleteObject(bodyFont_);
        }
        if (smallFont_ != nullptr) {
            ::DeleteObject(smallFont_);
        }
        titleFont_ = ::CreateFontW(
            FontHeight(dpi_, 20), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Display");
        headingFont_ = ::CreateFontW(
            FontHeight(dpi_, 11), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
        bodyFont_ = ::CreateFontW(
            FontHeight(dpi_, 10), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
        smallFont_ = ::CreateFontW(
            FontHeight(dpi_, 9), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI Variable Text");
        ApplyFontToControls();
    }

    void ApplyFontToControls() {
        if (bodyFont_ == nullptr) {
            return;
        }
        const auto set = [](HWND control, HFONT font) {
            if (control != nullptr && font != nullptr) {
                ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
            }
        };
        set(titleLabel_, titleFont_);
        set(languageLabel_, bodyFont_);
        set(languageCombo_, bodyFont_);
        set(subtitleLabel_, smallFont_);
        for (HWND section : {sectionRange_, sectionQuality_, sectionAudio_, sectionOutput_}) {
            set(section, headingFont_);
        }
        const HWND bodyControls[] = {
            fullRadio_, regionRadio_, monitorLabel_, monitorCombo_, regionButton_,
            regionSummary_, resolutionLabel_, resolutionCombo_, fpsLabel_, fpsCombo_,
            qualityLabel_, qualityCombo_, qualityHint_, audioCheck_, micCheck_,
            systemDeviceCombo_, micDeviceCombo_, outputLabel_, outputPath_, browseButton_,
            outputSummary_, statusLabel_,
            startButton_, openFolderButton_, screenshotButton_,
        };
        for (HWND control : bodyControls) {
            set(control, bodyFont_);
        }
        set(subtitleLabel_, smallFont_);
        set(qualityHint_, smallFont_);
        set(outputSummary_, smallFont_);
        set(statusLabel_, bodyFont_);
    }

    HWND CreateControl(const wchar_t* className, const wchar_t* text, DWORD style,
                       int id = 0, DWORD exStyle = 0) {
        const std::wstring localizedText = ui::Text(text);
        HWND control = ::CreateWindowExW(
            exStyle, className, localizedText.c_str(), WS_CHILD | WS_VISIBLE | style,
            0, 0, 0, 0, window_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        if (control != nullptr && bodyFont_ != nullptr) {
            ::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(bodyFont_), TRUE);
        }
        return control;
    }

    void PopulateLanguageCombo() {
        ::SendMessageW(languageCombo_, CB_RESETCONTENT, 0, 0);
        const wchar_t* choices[] = {L"自动（跟随系统）", L"简体中文", L"English"};
        for (int i = 0; i < 3; ++i) {
            const std::wstring label = ui::Text(choices[i]);
            ::SendMessageW(languageCombo_, CB_ADDSTRING, 0,
                           reinterpret_cast<LPARAM>(label.c_str()));
        }
        ::SendMessageW(languageCombo_, CB_SETCURSEL, ui::LanguagePreference(), 0);
    }

    void RefreshLocalizedControls() {
        const auto set = [](HWND control, std::wstring_view source) {
            const std::wstring label = ui::Text(source);
            ::SetWindowTextW(control, label.c_str());
        };
        ::SetWindowTextW(window_, ui::Text(L"轻录").c_str());
        set(titleLabel_, L"轻录");
        set(languageLabel_, L"语言");
        set(subtitleLabel_, L"轻量、直接的屏幕录制");
        set(sectionRange_, L"录制范围");
        set(sectionQuality_, L"画质");
        set(sectionAudio_, L"声音");
        set(sectionOutput_, L"保存位置");
        set(fullRadio_, L"全屏");
        set(regionRadio_, L"区域");
        set(monitorLabel_, L"显示器");
        set(regionButton_, L"选择区域…");
        set(resolutionLabel_, L"分辨率");
        set(fpsLabel_, L"帧率");
        set(qualityLabel_, L"画质");
        set(qualityHint_, L"画质越高，文件越大");
        set(audioCheck_, L"系统声音");
        set(micCheck_, L"麦克风");
        set(outputLabel_, L"保存到");
        set(browseButton_, L"更改");
        set(openFolderButton_, L"打开文件夹");
        set(screenshotButton_, L"截图");
        PopulateLanguageCombo();

        const int resolutionSelection = static_cast<int>(::SendMessageW(
            resolutionCombo_, CB_GETCURSEL, 0, 0));
        ::SendMessageW(resolutionCombo_, CB_RESETCONTENT, 0, 0);
        const wchar_t* resolutions[] = {L"原始尺寸", L"720p", L"1080p", L"1440p",
                                        L"2160p", L"自定义…"};
        for (int i = 0; i < 6; ++i) {
            const std::wstring label = ui::Text(resolutions[i]);
            const LRESULT item = ::SendMessageW(resolutionCombo_, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(label.c_str()));
            ::SendMessageW(resolutionCombo_, CB_SETITEMDATA, item, i);
        }
        ::SendMessageW(resolutionCombo_, CB_SETCURSEL, resolutionSelection, 0);

        const int qualitySelection = static_cast<int>(::SendMessageW(
            qualityCombo_, CB_GETCURSEL, 0, 0));
        ::SendMessageW(qualityCombo_, CB_RESETCONTENT, 0, 0);
        for (int i = 0; i < kQualityCount; ++i) {
            const std::wstring label = ui::Text(kQualityOptions[i].name);
            const LRESULT item = ::SendMessageW(qualityCombo_, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(label.c_str()));
            ::SendMessageW(qualityCombo_, CB_SETITEMDATA, item, i);
        }
        ::SendMessageW(qualityCombo_, CB_SETCURSEL, qualitySelection, 0);
        UpdateDerivedState(true);
        UpdateUiState();
        Layout();
    }

    void CreateControls() {
        titleLabel_ = CreateControl(L"STATIC", L"轻录", SS_LEFT, kTitleLabel);
        languageLabel_ = CreateControl(L"STATIC", L"语言",
            SS_LEFT | SS_CENTERIMAGE, kLanguageLabel);
        languageCombo_ = CreateControl(L"COMBOBOX", L"",
            CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kLanguageCombo);
        ThemeControl(languageCombo_);
        PopulateLanguageCombo();
        subtitleLabel_ = CreateControl(L"STATIC", L"轻量、直接的屏幕录制", SS_LEFT,
                                       kSubtitleLabel);

        sectionRange_ = CreateControl(L"STATIC", L"录制范围", SS_LEFT, kSectionRange);
        sectionQuality_ = CreateControl(L"STATIC", L"画质", SS_LEFT, kSectionQuality);
        sectionAudio_ = CreateControl(L"STATIC", L"声音", SS_LEFT, kSectionAudio);
        sectionOutput_ = CreateControl(L"STATIC", L"保存位置", SS_LEFT, kSectionOutput);

        fullRadio_ = CreateControl(L"BUTTON", L"全屏",
                                   BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, kModeFull);
        regionRadio_ = CreateControl(L"BUTTON", L"区域", BS_AUTORADIOBUTTON | WS_TABSTOP,
                                     kModeRegion);
        ::CheckRadioButton(window_, kModeFull, kModeRegion, kModeFull);

        monitorLabel_ = CreateControl(L"STATIC", L"显示器", SS_LEFT | SS_CENTERIMAGE,
                                      kLabelMonitor);
        monitorCombo_ = CreateControl(L"COMBOBOX", L"",
                                      CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP | WS_GROUP,
                                      kMonitorCombo);
        ThemeControl(monitorCombo_);
        regionButton_ = CreateControl(L"BUTTON", L"选择区域…",
                                      BS_OWNERDRAW | WS_TABSTOP, kSelectRegionButton);
        regionSummary_ = CreateControl(L"STATIC", L"未选择区域",
                                       SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS,
                                       kRegionSummary);

        resolutionLabel_ = CreateControl(L"STATIC", L"分辨率", SS_LEFT | SS_CENTERIMAGE,
                                          kLabelResolution);
        resolutionCombo_ = CreateControl(L"COMBOBOX", L"",
                                          CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP | WS_GROUP,
                                          kResolutionCombo);
        ThemeControl(resolutionCombo_);
        const wchar_t* resolutions[] = {L"原始尺寸", L"720p", L"1080p", L"1440p", L"2160p",
                                        L"自定义…"};
        for (int i = 0; i < 6; ++i) {
            const std::wstring localized = ui::Text(resolutions[i]);
            const int index =
                static_cast<int>(::SendMessageW(resolutionCombo_, CB_ADDSTRING, 0,
                                                reinterpret_cast<LPARAM>(localized.c_str())));
            ::SendMessageW(resolutionCombo_, CB_SETITEMDATA, index, i);
        }

        fpsLabel_ = CreateControl(L"STATIC", L"帧率", SS_LEFT | SS_CENTERIMAGE, kLabelFps);
        fpsCombo_ = CreateControl(L"COMBOBOX", L"",
                                  CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP | WS_GROUP,
                                  kFpsCombo);
        ThemeControl(fpsCombo_);
        ::SendMessageW(fpsCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"30 fps"));
        ::SendMessageW(fpsCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"60 fps"));

        qualityLabel_ = CreateControl(L"STATIC", L"画质", SS_LEFT | SS_CENTERIMAGE,
                                      kLabelQuality);
        qualityCombo_ = CreateControl(L"COMBOBOX", L"",
                                      CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP | WS_GROUP,
                                      kQualityCombo);
        ThemeControl(qualityCombo_);
        for (int i = 0; i < kQualityCount; ++i) {
            const int index =
                static_cast<int>(::SendMessageW(qualityCombo_, CB_ADDSTRING, 0,
                    reinterpret_cast<LPARAM>(ui::Text(kQualityOptions[i].name).c_str())));
            ::SendMessageW(qualityCombo_, CB_SETITEMDATA, index, i);
        }
        qualityHint_ = CreateControl(L"STATIC", L"画质越高，文件越大", SS_LEFT | SS_CENTERIMAGE,
                                     kQualityHint);

        audioCheck_ = CreateControl(L"BUTTON", L"系统声音",
                                    BS_AUTOCHECKBOX | WS_TABSTOP | WS_GROUP, kAudioCheck);
        micCheck_ = CreateControl(L"BUTTON", L"麦克风",
                                  BS_AUTOCHECKBOX | WS_TABSTOP, kMicCheck);
        systemDeviceCombo_ = CreateControl(
            L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kSystemDeviceCombo);
        micDeviceCombo_ = CreateControl(
            L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kMicDeviceCombo);
        ThemeControl(systemDeviceCombo_);
        ThemeControl(micDeviceCombo_);

        outputLabel_ = CreateControl(L"STATIC", L"保存到", SS_LEFT | SS_CENTERIMAGE,
                                     kLabelOutput);
        outputPath_ = CreateControl(L"STATIC", L"", SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS,
                                    kOutputPath);
        browseButton_ = CreateControl(L"BUTTON", L"更改", BS_OWNERDRAW | WS_TABSTOP,
                                      kBrowseButton);
        outputSummary_ = CreateControl(L"STATIC", L"", SS_LEFT | SS_NOPREFIX, kOutputSummary);

        statusLabel_ = CreateControl(L"STATIC", L"就绪", SS_LEFT | SS_CENTERIMAGE,
                                     kStatusLabel);
        openFolderButton_ = CreateControl(L"BUTTON", L"打开文件夹",
                                          BS_OWNERDRAW | WS_TABSTOP, kOpenFolderButton);
        screenshotButton_ = CreateControl(L"BUTTON", L"截图", BS_OWNERDRAW | WS_TABSTOP,
                                          kScreenshotButton);
        startButton_ = CreateControl(L"BUTTON", L"开始录制", BS_OWNERDRAW | WS_TABSTOP,
                                     kStartButton);
        ::EnableWindow(openFolderButton_, FALSE);
        ApplyFontToControls();
    }

    void Layout() {
        if (window_ == nullptr) {
            return;
        }
        RECT client{};
        ::GetClientRect(window_, &client);
        const int width = std::max<int>(client.right - client.left, 1);
        const int height = std::max<int>(client.bottom - client.top, 1);
        const int margin = Scale(dpi_, 16);
        const int cardTop = Scale(dpi_, 76);
        const int cardBottom = std::max(height - margin, cardTop + Scale(dpi_, 608));
        cardRect_ = RECT{margin, cardTop, width - margin, cardBottom};

        const int inner = Scale(dpi_, 28);
        const int left = cardRect_.left + inner;
        const int right = cardRect_.right - inner;
        const int labelY1 = cardTop + Scale(dpi_, 20);
        const int radioY = cardTop + Scale(dpi_, 48);
        const int monitorY = cardTop + Scale(dpi_, 82);
        const int regionY = cardTop + Scale(dpi_, 116);
        const int sep1 = cardTop + Scale(dpi_, 154);
        const int labelY2 = cardTop + Scale(dpi_, 174);
        const int qualityRowY = cardTop + Scale(dpi_, 202);
        const int qualityRow2Y = cardTop + Scale(dpi_, 238);
        const int sep2 = cardTop + Scale(dpi_, 276);
        const int labelY3 = cardTop + Scale(dpi_, 296);
        const int audioY = cardTop + Scale(dpi_, 324);
        const int sep3 = cardTop + Scale(dpi_, 392);
        const int labelY4 = cardTop + Scale(dpi_, 412);
        const int outputY = cardTop + Scale(dpi_, 440);
        const int outputSummaryY = cardTop + Scale(dpi_, 476);
        const int statusY = cardBottom - Scale(dpi_, 96);
        const int buttonY = cardBottom - Scale(dpi_, 52);
        const int rowHeight = Scale(dpi_, 30);
        const int buttonHeight = Scale(dpi_, 38);

        ::MoveWindow(titleLabel_, margin, Scale(dpi_, 12), width - margin * 2,
                     Scale(dpi_, 36), TRUE);
        ::MoveWindow(languageLabel_, width - margin - Scale(dpi_, 174),
                     Scale(dpi_, 14), Scale(dpi_, 48), Scale(dpi_, 28), TRUE);
        ::MoveWindow(languageCombo_, width - margin - Scale(dpi_, 124),
                     Scale(dpi_, 14), Scale(dpi_, 124), Scale(dpi_, 28), TRUE);
        ::MoveWindow(subtitleLabel_, margin, Scale(dpi_, 48), width - margin * 2,
                     Scale(dpi_, 22), TRUE);
        ::MoveWindow(sectionRange_, left, labelY1, right - left, Scale(dpi_, 24), TRUE);
        ::MoveWindow(sectionQuality_, left, labelY2, right - left, Scale(dpi_, 24), TRUE);
        ::MoveWindow(sectionAudio_, left, labelY3, right - left, Scale(dpi_, 24), TRUE);
        ::MoveWindow(sectionOutput_, left, labelY4, right - left, Scale(dpi_, 24), TRUE);

        ::MoveWindow(fullRadio_, left, radioY, Scale(dpi_, 68), Scale(dpi_, 26), TRUE);
        ::MoveWindow(regionRadio_, left + Scale(dpi_, 82), radioY, Scale(dpi_, 68),
                     Scale(dpi_, 26), TRUE);
        ::MoveWindow(monitorLabel_, left, monitorY, Scale(dpi_, 48), Scale(dpi_, 24), TRUE);
        ::MoveWindow(monitorCombo_, left + Scale(dpi_, 56), monitorY, Scale(dpi_, 294),
                     Scale(dpi_, 200), TRUE);
        ::MoveWindow(regionButton_, left, regionY, Scale(dpi_, 112), rowHeight, TRUE);
        ::MoveWindow(regionSummary_, left + Scale(dpi_, 124), regionY,
                     right - (left + Scale(dpi_, 124)), Scale(dpi_, 28), TRUE);

        ::MoveWindow(resolutionLabel_, left, qualityRowY, Scale(dpi_, 48), Scale(dpi_, 24),
                     TRUE);
        ::MoveWindow(resolutionCombo_, left + Scale(dpi_, 56), qualityRowY, Scale(dpi_, 190),
                     Scale(dpi_, 200), TRUE);
        ::MoveWindow(fpsLabel_, left + Scale(dpi_, 264), qualityRowY, Scale(dpi_, 40),
                     Scale(dpi_, 24), TRUE);
        ::MoveWindow(fpsCombo_, left + Scale(dpi_, 308), qualityRowY, Scale(dpi_, 100),
                     Scale(dpi_, 200), TRUE);
        ::MoveWindow(qualityLabel_, left, qualityRow2Y, Scale(dpi_, 48), Scale(dpi_, 24),
                     TRUE);
        ::MoveWindow(qualityCombo_, left + Scale(dpi_, 56), qualityRow2Y, Scale(dpi_, 190),
                     Scale(dpi_, 200), TRUE);
        ::MoveWindow(qualityHint_, left + Scale(dpi_, 264), qualityRow2Y,
                     right - (left + Scale(dpi_, 264)), Scale(dpi_, 24), TRUE);

        const int microphoneY = audioY + Scale(dpi_, 32);
        const int deviceX = left + Scale(dpi_, 136);
        const int deviceWidth = right - deviceX;
        ::MoveWindow(audioCheck_, left, audioY, Scale(dpi_, 128), Scale(dpi_, 26), TRUE);
        ::MoveWindow(systemDeviceCombo_, deviceX, audioY - Scale(dpi_, 2), deviceWidth,
                     Scale(dpi_, 200), TRUE);
        ::MoveWindow(micCheck_, left, microphoneY, Scale(dpi_, 128), Scale(dpi_, 26), TRUE);
        ::MoveWindow(micDeviceCombo_, deviceX, microphoneY - Scale(dpi_, 2), deviceWidth,
                     Scale(dpi_, 200), TRUE);

        ::MoveWindow(outputLabel_, left, outputY, Scale(dpi_, 56), Scale(dpi_, 24), TRUE);
        const int browseWidth = Scale(dpi_, 84);
        const int outputGap = Scale(dpi_, 12);
        ::MoveWindow(outputPath_, left + Scale(dpi_, 64), outputY,
                     right - (left + Scale(dpi_, 64)) - browseWidth - outputGap,
                     Scale(dpi_, 26), TRUE);
        ::MoveWindow(browseButton_, right - browseWidth, outputY, browseWidth,
                     Scale(dpi_, 30), TRUE);
        ::MoveWindow(outputSummary_, left, outputSummaryY, right - left, Scale(dpi_, 40),
                     TRUE);

        ::MoveWindow(statusLabel_, left, statusY, right - left, Scale(dpi_, 24), TRUE);
        const int startWidth = Scale(dpi_, 136);
        const int screenshotWidth = Scale(dpi_, 88);
        const int openWidth = Scale(dpi_, 104);
        const int gap = Scale(dpi_, 10);
        ::MoveWindow(openFolderButton_, left, buttonY, openWidth, buttonHeight, TRUE);
        ::MoveWindow(startButton_, right - startWidth, buttonY, startWidth, buttonHeight,
                     TRUE);
        ::MoveWindow(screenshotButton_, right - startWidth - gap - screenshotWidth, buttonY,
                     screenshotWidth, buttonHeight, TRUE);

        separators_ = {sep1, sep2, sep3};
        sectionTops_ = {labelY1, labelY2, labelY3, labelY4};
        ::InvalidateRect(window_, nullptr, FALSE);
    }

    void PaintWindow(HDC target) {
        RECT client{};
        ::GetClientRect(window_, &client);
        const int width = std::max<int>(client.right - client.left, 1);
        const int height = std::max<int>(client.bottom - client.top, 1);
        HDC memory = ::CreateCompatibleDC(target);
        HBITMAP bitmap = ::CreateCompatibleBitmap(target, width, height);
        HGDIOBJ oldBitmap = ::SelectObject(memory, bitmap);

        if (HighContrast()) {
            ::FillRect(memory, &client, ::GetSysColorBrush(COLOR_WINDOW));
        } else {
            ::FillRect(memory, &client, backgroundBrush_);
            ::srl::ui::DrawRoundedPanel(memory, cardRect_, kColorCard, kColorCardBorder,
                                        Scale(dpi_, 14));
            for (int y : separators_) {
                HPEN pen = ::CreatePen(PS_SOLID, 1, kColorSeparator);
                HGDIOBJ oldPen = ::SelectObject(memory, pen);
                ::MoveToEx(memory, cardRect_.left + Scale(dpi_, 28), y, nullptr);
                ::LineTo(memory, cardRect_.right - Scale(dpi_, 28), y);
                ::SelectObject(memory, oldPen);
                ::DeleteObject(pen);
            }
            for (int top : sectionTops_) {
                RECT bar{cardRect_.left + Scale(dpi_, 14), top + Scale(dpi_, 5),
                         cardRect_.left + Scale(dpi_, 17), top + Scale(dpi_, 19)};
                HBRUSH brush = ::CreateSolidBrush(kColorAccent);
                ::FillRect(memory, &bar, brush);
                ::DeleteObject(brush);
            }
        }
        ::BitBlt(target, 0, 0, width, height, memory, 0, 0, SRCCOPY);
        ::SelectObject(memory, oldBitmap);
        ::DeleteObject(bitmap);
        ::DeleteDC(memory);
    }

    LRESULT OnCtlColor(HDC dc, HWND control) {
        const bool header = control == titleLabel_ || control == subtitleLabel_ ||
                            control == languageLabel_;
        const bool highContrast = HighContrast();
        HBRUSH brush = highContrast ? ::GetSysColorBrush(COLOR_WINDOW)
                                    : (header ? backgroundBrush_ : cardBrush_);
        ::SetBkMode(dc, TRANSPARENT);
        ::SetBkColor(dc, highContrast ? ::GetSysColor(COLOR_WINDOW)
                                      : (header ? kColorBackground : kColorCard));
        COLORREF color = highContrast ? ::GetSysColor(COLOR_WINDOWTEXT) : kColorText;
        if (!highContrast) {
            if (control == subtitleLabel_ || control == languageLabel_ ||
                control == qualityHint_ ||
                control == outputSummary_ ||
                control == regionSummary_ || control == statusLabel_) {
                color = kColorMuted;
            }
            if (control == statusLabel_ && statusError_) {
                color = kColorError;
            }
        }
        ::SetTextColor(dc, color);
        return reinterpret_cast<LRESULT>(brush);
    }

    void PopulateDeviceCombo(HWND combo, const wchar_t* defaultLabel,
                             const std::vector<AudioEndpointInfo>& endpoints,
                             std::vector<std::wstring>& ids) {
        ids.clear();
        ::SendMessageW(combo, CB_RESETCONTENT, 0, 0);
        int index = static_cast<int>(
            ::SendMessageW(combo, CB_ADDSTRING, 0,
                            reinterpret_cast<LPARAM>(ui::Text(defaultLabel).c_str())));
        ::SendMessageW(combo, CB_SETITEMDATA, index, 0);
        ids.emplace_back();
        for (const AudioEndpointInfo& endpoint : endpoints) {
            std::wstring name = endpoint.friendlyName.empty() ? endpoint.deviceName
                                                              : endpoint.friendlyName;
            if (name.empty()) {
                name = ui::Text(L"音频设备");
            }
            if (!endpoint.deviceName.empty() && endpoint.deviceName != name) {
                name += L" · " + endpoint.deviceName;
            }
            index = static_cast<int>(::SendMessageW(
                combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str())));
            ::SendMessageW(combo, CB_SETITEMDATA, index,
                           static_cast<LPARAM>(ids.size()));
            ids.push_back(endpoint.id);
        }
        ::SendMessageW(combo, CB_SETCURSEL, 0, 0);
    }

    void RefreshAudioDevices() {
        const AudioProbeResult probe = QueryAudioProbe(false, false);
        PopulateDeviceCombo(systemDeviceCombo_, L"默认播放设备", probe.renderEndpoints,
                            systemDeviceIds_);
        PopulateDeviceCombo(micDeviceCombo_, L"默认输入设备", probe.captureEndpoints,
                            microphoneDeviceIds_);
    }

    void SelectDeviceCombo(HWND combo, const std::vector<std::wstring>& ids,
                           const std::wstring& id) {
        int selected = 0;
        if (!id.empty()) {
            for (std::size_t i = 0; i < ids.size(); ++i) {
                if (ids[i] == id) {
                    selected = static_cast<int>(i);
                    break;
                }
            }
        }
        SetComboSelection(combo, selected);
    }

    std::wstring SelectedDeviceId(HWND combo,
                                  const std::vector<std::wstring>& ids) const {
        const int index = ComboSelection(combo);
        if (index >= 0 && static_cast<std::size_t>(index) < ids.size()) {
            return ids[static_cast<std::size_t>(index)];
        }
        return {};
    }

    void RebuildMonitorList() {
        ::SendMessageW(monitorCombo_, CB_RESETCONTENT, 0, 0);
        displays_ = EnumerateDisplays();
        int selected = 0;
        for (std::size_t i = 0; i < displays_.size(); ++i) {
            const MonitorInfo& monitor = displays_[i];
            const int width = monitor.virtualRect.right - monitor.virtualRect.left;
            const int height = monitor.virtualRect.bottom - monitor.virtualRect.top;
            const std::wstring text =
                Utf8ToWide(std::format("显示器 {} · {}×{} · {}", monitor.index, width,
                                       height, WideToUtf8(monitor.friendlyName)));
            const int index = static_cast<int>(
                ::SendMessageW(monitorCombo_, CB_ADDSTRING, 0,
                               reinterpret_cast<LPARAM>(text.c_str())));
            ::SendMessageW(monitorCombo_, CB_SETITEMDATA, index, i);
            if (monitor.primary) {
                selected = static_cast<int>(i);
            }
        }
        if (!displays_.empty()) {
            ::SendMessageW(monitorCombo_, CB_SETCURSEL, selected, 0);
            selectedMonitor_ = selected;
        } else {
            selectedMonitor_ = 0;
        }
        regionAvailable_ = false;
        regionRect_ = {};
    }

    void ApplySettings(const AppSettings& settings) {
        ::SendMessageW(languageCombo_, CB_SETCURSEL, ui::LanguagePreference(), 0);
        if (!displays_.empty()) {
            int index = 0;
            if (!settings.monitorDeviceName.empty()) {
                for (std::size_t i = 0; i < displays_.size(); ++i) {
                    if (displays_[i].deviceName == settings.monitorDeviceName) {
                        index = static_cast<int>(i);
                        break;
                    }
                }
            }
            selectedMonitor_ = index;
            ::SendMessageW(monitorCombo_, CB_SETCURSEL, index, 0);
        }
        if (settings.hasRegion) {
            const MonitorInfo* monitor = SelectedMonitor();
            const int monitorWidth = monitor != nullptr
                                         ? monitor->virtualRect.right -
                                               monitor->virtualRect.left
                                         : 0;
            const int monitorHeight = monitor != nullptr
                                          ? monitor->virtualRect.bottom -
                                                monitor->virtualRect.top
                                          : 0;
            const int width = settings.region.right - settings.region.left;
            const int height = settings.region.bottom - settings.region.top;
            if (monitor != nullptr && width >= kMinOutputDimension &&
                height >= kMinOutputDimension && settings.region.left >= 0 &&
                settings.region.top >= 0 && settings.region.right <= monitorWidth &&
                settings.region.bottom <= monitorHeight) {
                regionRect_ = settings.region;
                regionAvailable_ = true;
            }
        }
        customBound_ = settings.customBound;
        const int limitIndex = std::clamp(static_cast<int>(settings.limit), 0, 5);
        ::SendMessageW(resolutionCombo_, CB_SETCURSEL, limitIndex, 0);
        lastResolutionIndex_ = limitIndex;
        ::SendMessageW(fpsCombo_, CB_SETCURSEL, settings.fps == 60 ? 1 : 0, 0);
        int qualityIndex = kDefaultQualityIndex;
        for (int i = 0; i < kQualityCount; ++i) {
            if (kQualityOptions[i].bitrateMbps == settings.bitrateMbps) {
                qualityIndex = i;
                break;
            }
        }
        ::SendMessageW(qualityCombo_, CB_SETCURSEL, qualityIndex, 0);
        ::SendMessageW(audioCheck_, BM_SETCHECK,
                       settings.systemAudio ? BST_CHECKED : BST_UNCHECKED, 0);
        ::SendMessageW(micCheck_, BM_SETCHECK,
                       settings.microphone ? BST_CHECKED : BST_UNCHECKED, 0);
        SelectDeviceCombo(systemDeviceCombo_, systemDeviceIds_,
                          settings.systemAudioDeviceId);
        SelectDeviceCombo(micDeviceCombo_, microphoneDeviceIds_,
                          settings.microphoneDeviceId);
        if (!settings.outputDirectory.empty()) {
            outputDirectory_ = settings.outputDirectory;
        }
        ::SetWindowTextW(outputPath_, outputDirectory_.c_str());
    }

    [[nodiscard]] const MonitorInfo* SelectedMonitor() const {
        if (selectedMonitor_ < 0 ||
            static_cast<std::size_t>(selectedMonitor_) >= displays_.size()) {
            return nullptr;
        }
        return &displays_[selectedMonitor_];
    }

    int ComboSelection(HWND combo) const {
        return static_cast<int>(::SendMessageW(combo, CB_GETCURSEL, 0, 0));
    }

    void SetComboSelection(HWND combo, int index) {
        if (index >= 0) {
            ::SendMessageW(combo, CB_SETCURSEL, index, 0);
        }
    }

    ResolutionLimit SelectedLimit() const {
        const int index = ComboSelection(resolutionCombo_);
        if (index >= 0 && index <= 5) {
            return static_cast<ResolutionLimit>(index);
        }
        return ResolutionLimit::P1080;
    }

    int SelectedFps() const {
        return ComboSelection(fpsCombo_) == 1 ? 60 : 30;
    }

    int SelectedQualityIndex() const {
        const int index = ComboSelection(qualityCombo_);
        return index >= 0 && index < kQualityCount ? index : kDefaultQualityIndex;
    }

    int SelectedBitrate() const {
        return kQualityOptions[SelectedQualityIndex()].bitrateMbps;
    }

    bool RegionMode() const {
        return ::SendMessageW(regionRadio_, BM_GETCHECK, 0, 0) == BST_CHECKED;
    }

    RECT CurrentSourceRect() const {
        const MonitorInfo* monitor = SelectedMonitor();
        if (monitor == nullptr) {
            return {};
        }
        if (RegionMode()) {
            return regionRect_;
        }
        return RECT{0, 0, monitor->virtualRect.right - monitor->virtualRect.left,
                    monitor->virtualRect.bottom - monitor->virtualRect.top};
    }

    std::optional<OutputSize> ComputeCurrentOutput(std::wstring* error) const {
        const MonitorInfo* monitor = SelectedMonitor();
        if (monitor == nullptr) {
            if (error != nullptr) {
                *error = L"未检测到可用显示器";
            }
            return std::nullopt;
        }
        if (RegionMode() && !regionAvailable_) {
            if (error != nullptr) {
                *error = L"请先选择录制区域";
            }
            return std::nullopt;
        }
        const RECT source = CurrentSourceRect();
        const int width = source.right - source.left;
        const int height = source.bottom - source.top;
        if (width < kMinOutputDimension || height < kMinOutputDimension) {
            if (error != nullptr) {
                *error = L"录制区域至少需要 64×64 物理像素";
            }
            return std::nullopt;
        }
        return ComputeOutputSize(
            OutputSizeRequest{width, height, SelectedLimit(), customBound_}, error);
    }

    void PromptCustomResolution() {
        const RECT source = CurrentSourceRect();
        const int sourceWidth = source.right - source.left;
        const int sourceHeight = source.bottom - source.top;
        if (sourceWidth < kMinOutputDimension || sourceHeight < kMinOutputDimension) {
            SetStatus(L"请先选择有效的录制区域", true);
            SetComboSelection(resolutionCombo_, lastResolutionIndex_);
            return;
        }
        Size chosen{};
        if (!CustomSizeDialog::Show(window_, instance_, sourceWidth, sourceHeight,
                                    customBound_, &chosen)) {
            SetComboSelection(resolutionCombo_, lastResolutionIndex_);
            return;
        }
        customBound_ = chosen;
        lastResolutionIndex_ = 5;
        SetComboSelection(resolutionCombo_, 5);
        UpdateDerivedState(true);
        UpdateUiState();
    }

    std::wstring AudioDescription() const {
        const bool audio = ::SendMessageW(audioCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        const bool microphone =
            ::SendMessageW(micCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (audio && microphone) return ui::Text(L"系统声音 + 麦克风");
        if (audio) return ui::Text(L"系统声音");
        if (microphone) return ui::Text(L"麦克风");
        return ui::Text(L"声音已关闭（录制中可开启）");
    }

    void UpdateDerivedState(bool updateStatus) {
        const MonitorInfo* monitor = SelectedMonitor();
        const RECT source = CurrentSourceRect();
        const int sourceWidth = source.right - source.left;
        const int sourceHeight = source.bottom - source.top;

        if (monitor == nullptr) {
            ::SetWindowTextW(regionSummary_, ui::Text(L"未检测到显示器").c_str());
        } else if (RegionMode()) {
            if (regionAvailable_) {
                const std::wstring text = Utf8ToWide(std::format(
                    "{} {} · {} {}×{} · {} ({}, {})", WideToUtf8(ui::Text(L"显示器")),
                    monitor->index, WideToUtf8(ui::Text(L"源区域")), sourceWidth,
                    sourceHeight, WideToUtf8(ui::Text(L"起点")), source.left, source.top));
                const std::wstring localized = ui::Text(text);
                ::SetWindowTextW(regionSummary_, localized.c_str());
            } else {
                ::SetWindowTextW(regionSummary_, ui::Text(L"未选择区域").c_str());
            }
        } else {
            const std::wstring text = Utf8ToWide(
                std::format("{} · {} {} · {}×{}", WideToUtf8(ui::Text(L"全屏")),
                            WideToUtf8(ui::Text(L"显示器")), monitor->index,
                            sourceWidth, sourceHeight));
            const std::wstring localized = ui::Text(text);
            ::SetWindowTextW(regionSummary_, localized.c_str());
        }

        std::wstring sizeError;
        const auto output = ComputeCurrentOutput(&sizeError);
        const int bitrate = SelectedBitrate();
        std::wstring statusError;
        bool valid = true;
        std::wstring outputText;
        if (monitor == nullptr) {
            valid = false;
            statusError = L"未检测到可用显示器";
        } else if (RegionMode() && !regionAvailable_) {
            valid = false;
            statusError = L"请先选择录制区域";
        } else if (!output.has_value()) {
            valid = false;
            statusError = L"无法生成输出画面：" + sizeError;
        }
        if (outputDirectory_.empty()) {
            valid = false;
            statusError = L"请选择导出目录";
        }
        if (output.has_value()) {
            const std::wstring quality = kQualityOptions[SelectedQualityIndex()].name;
            if (ui::IsEnglishLanguage()) {
                outputText = std::format(
                    L"{}×{} · {} fps · {} quality · est. {} / 60 sec",
                    output->output.width, output->output.height, SelectedFps(),
                    ui::Text(quality),
                    Utf8ToWide(FormatBytes(EstimateBytesWithAudioTracks(
                        MbpsToBitsPerSecond(static_cast<double>(bitrate)), 3, 60))));
            } else {
                outputText = Utf8ToWide(std::format(
                    "{}×{} · {} fps · {}画质 · 预计 60 秒约 {}", output->output.width,
                    output->output.height, SelectedFps(), WideToUtf8(quality),
                    FormatBytes(EstimateBytesWithAudioTracks(
                        MbpsToBitsPerSecond(static_cast<double>(bitrate)), 3, 60))));
            }
            outputText += L"\r\nH.264 / MP4";
            outputText += ui::IsEnglishLanguage()
                ? L" + 3 AAC tracks (mix / system / mic)"
                : L" + 3 条 AAC 音轨（混音 / 系统声 / 麦克风）";
            const std::wstring localized = ui::Text(outputText);
            ::SetWindowTextW(outputSummary_, localized.c_str());
        } else {
            outputText = L"请先选择有效的录制范围和画质";
            if (!sizeError.empty()) {
                outputText = sizeError;
            }
            const std::wstring localized = ui::Text(outputText);
            ::SetWindowTextW(outputSummary_, localized.c_str());
        }
        configValid_ = valid;
        if (phase_ == Phase::Idle) {
            ::EnableWindow(startButton_, valid ? TRUE : FALSE);
        }
        if (updateStatus && phase_ == Phase::Idle) {
            if (!valid) {
                SetStatus(statusError, true);
            } else {
                std::wstring line = ui::Text(L"就绪 · ") + ui::Text(AudioDescription());
                if (hotkeyStartStop_) {
                    line += ui::Text(L" · Ctrl+Alt+F9 开始/停止");
                } else {
                    line += ui::Text(L" · 使用按钮开始/停止");
                }
                SetStatus(line);
            }
        }
    }

    void UpdateUiState() {
        const bool idle = phase_ == Phase::Idle;
        const bool ready = idle && screenshotWorker_.pending() == 0;
        const BOOL enableSettings = ready ? TRUE : FALSE;
        ::EnableWindow(fullRadio_, enableSettings);
        ::EnableWindow(regionRadio_, enableSettings);
        ::EnableWindow(monitorCombo_, enableSettings);
        ::EnableWindow(regionButton_, enableSettings);
        ::EnableWindow(resolutionCombo_, enableSettings);
        ::EnableWindow(fpsCombo_, enableSettings);
        ::EnableWindow(qualityCombo_, enableSettings);
        ::EnableWindow(audioCheck_, enableSettings);
        ::EnableWindow(micCheck_, enableSettings);
        const bool systemAudioOn =
            ::SendMessageW(audioCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        const bool microphoneOn =
            ::SendMessageW(micCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        ::EnableWindow(systemDeviceCombo_,
                       ready && systemAudioOn ? TRUE : FALSE);
        ::EnableWindow(micDeviceCombo_, ready && microphoneOn ? TRUE : FALSE);
        ::EnableWindow(browseButton_, enableSettings);
        ::EnableWindow(startButton_, ready && configValid_ ? TRUE : FALSE);
        ::EnableWindow(openFolderButton_, !lastOutputFolder_.empty() ? TRUE : FALSE);
        ::EnableWindow(screenshotButton_,
                       (ready || phase_ == Phase::Recording) ? TRUE : FALSE);
        if (idle) {
            if (configValid_) {
                UpdateDerivedState(false);
            }
            ::SetWindowTextW(startButton_, ui::Text(L"开始录制").c_str());
        } else {
            ::EnableWindow(startButton_, FALSE);
            const std::wstring label = ui::Text(phase_ == Phase::Screenshot
                                                   ? L"正在截图…" : L"录制中…");
            ::SetWindowTextW(startButton_, label.c_str());
        }
    }

    void SetStatus(std::wstring_view text, bool error = false) {
        statusError_ = error;
        const std::wstring localized = ui::Text(text);
        ::SetWindowTextW(statusLabel_, localized.c_str());
        ::InvalidateRect(statusLabel_, nullptr, TRUE);
    }

    bool ValidateConfig(RecordingConfig& config, std::wstring& error) {
        const MonitorInfo* monitor = SelectedMonitor();
        if (monitor == nullptr) {
            error = L"未检测到可用显示器";
            return false;
        }
        if (RegionMode() && !regionAvailable_) {
            error = L"请先选择录制区域";
            return false;
        }
        const RECT source = CurrentSourceRect();
        const int width = source.right - source.left;
        const int height = source.bottom - source.top;
        if (width < kMinOutputDimension || height < kMinOutputDimension) {
            error = L"录制区域至少需要 64×64 物理像素";
            return false;
        }
        if (outputDirectory_.empty()) {
            error = L"请选择导出目录";
            return false;
        }
        std::wstring sizeError;
        if (!ComputeCurrentOutput(&sizeError).has_value()) {
            error = sizeError;
            return false;
        }
        config.monitor = monitor->handle;
        config.monitorDeviceName = monitor->deviceName;
        config.monitorPixelSize = Size{
            monitor->virtualRect.right - monitor->virtualRect.left,
            monitor->virtualRect.bottom - monitor->virtualRect.top};
        config.sourceRect = source;
        config.limit = SelectedLimit();
        config.customBound = customBound_;
        config.fps = SelectedFps();
        config.bitrateMbps = SelectedBitrate();
        config.systemAudio =
            ::SendMessageW(audioCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        config.microphone =
            ::SendMessageW(micCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        config.systemAudioDeviceId =
            SelectedDeviceId(systemDeviceCombo_, systemDeviceIds_);
        config.microphoneDeviceId =
            SelectedDeviceId(micDeviceCombo_, microphoneDeviceIds_);
        config.outputDirectory = outputDirectory_;
        return true;
    }

    void StartRecording() {
        if (phase_ != Phase::Idle || screenshotWorker_.pending() != 0 || closingPending_) {
            return;
        }
        stopPressed_ = false;
        RecordingConfig config;
        std::wstring error;
        if (!ValidateConfig(config, error)) {
            SetStatus(error, true);
            return;
        }

        std::wstring floatingError;
        if (floating_ == nullptr) {
            floating_ = std::make_unique<FloatingControl>();
            if (!floating_->Create(instance_, [this] { OnFloatingAction(); },
                                   [this] {
                                       ::PostMessageW(window_, WM_CLOSE, 0, 0);
                                   },
                                   [this] { TakeScreenshot(); },
                                   [this](bool systemAudio) { ToggleRecordingAudio(systemAudio); },
                                   [this] {
                                       return session_ != nullptr ? session_->microphoneLevel() : 0.0f;
                                   },
                                   &floatingError)) {
                floating_.reset();
                SetStatus(floatingError, true);
                MessageBoxUi(window_, floatingError.c_str(), L"无法开始录制",
                              MB_ICONERROR | MB_OK);
                return;
            }
        }

        const MonitorInfo* monitor = SelectedMonitor();
        if (monitor == nullptr) {
            SetStatus(L"未检测到可用显示器", true);
            return;
        }

        RECT selectionScreen{};
        if (RegionMode()) {
            selectionScreen = RECT{monitor->virtualRect.left + regionRect_.left,
                                   monitor->virtualRect.top + regionRect_.top,
                                   monitor->virtualRect.left + regionRect_.right,
                                   monitor->virtualRect.top + regionRect_.bottom};
        }
        floating_->SetPosition(*monitor, RegionMode() ? &selectionScreen : nullptr);

        if (!floating_->exclusionApplied()) {
            bool outside = false;
            if (RegionMode()) {
                RECT floatingRect{};
                ::GetWindowRect(floating_->hwnd(), &floatingRect);
                RECT intersection{};
                outside = !::IntersectRect(&intersection, &floatingRect, &selectionScreen);
            }
            if (!outside) {
                floating_->Destroy();
                floating_.reset();
                SetStatus(L"无法从录制画面中排除悬浮窗，已取消开始", true);
                MessageBoxUi(window_,
                              L"系统拒绝了悬浮窗的捕获排除设置，且当前范围无法把控制窗放在录制区域外。"
                              L"请换用区域录制或检查系统版本。",
                              L"无法开始录制", MB_ICONERROR | MB_OK);
                return;
            }
        }

        config_ = config;
        screenshotFailure_.clear();
        systemAudioEnabled_ = config.systemAudio;
        microphoneEnabled_ = config.microphone;
        audioRevision_ = 0;
        pendingAudioSource_.clear();
        phase_ = Phase::Countdown;
        countdownActive_ = true;
        countdownEnd_ = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        lastCountdownValue_ = -1;
        UpdateUiState();
        SetStatus(L"即将开始录制…");
        floating_->SetContent(L"3 秒后开始", true, L"取消");
        ::ShowWindow(window_, SW_HIDE);
        floating_->Show();
    }

    void OnTimer() {
        PollScreenshots();
        if (!::IsWindow(window_)) {
            return;
        }
        if (countdownActive_) {
            const auto now = std::chrono::steady_clock::now();
            if (now >= countdownEnd_) {
                countdownActive_ = false;
                StartSession();
            } else {
                const int remaining = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::seconds>(countdownEnd_ - now)
                        .count()) +
                    1;
                const int value = std::max(1, remaining);
                if (value != lastCountdownValue_) {
                    lastCountdownValue_ = value;
                    floating_->SetContent(
                        Utf8ToWide(std::format("{} 秒后开始", value)), true, L"取消");
                }
            }
            return;
        }

        if (session_ == nullptr) {
            return;
        }
        const RecorderState state = session_->state();
        if (stopPressed_) {
            if (state == RecorderState::Finished || state == RecorderState::Failed) {
                FinishSession(state == RecorderState::Finished);
                return;
            }
            std::wstring text = L"正在保存…";
            const auto savingSeconds = std::chrono::duration_cast<std::chrono::seconds>(
                                           std::chrono::steady_clock::now() -
                                           stopPressedAt_)
                                           .count();
            if (savingSeconds >= 10) {
                text += ui::IsEnglishLanguage()
                    ? L" Elapsed: " + std::to_wstring(savingSeconds) + L" seconds"
                    : L" 已用 " + std::to_wstring(savingSeconds) + L" 秒";
            }
            if (floating_ != nullptr) {
                floating_->SetContent(text, false, L"");
            }
            return;
        }
        switch (state) {
            case RecorderState::Starting:
                phase_ = Phase::Starting;
                floating_->SetContent(L"正在启动…", true, L"取消");
                break;
            case RecorderState::Recording: {
                phase_ = Phase::Recording;
                const auto audio = session_->audioState();
                systemAudioEnabled_ = audio.systemAudio;
                microphoneEnabled_ = audio.microphone;
                if (audio.revision != audioRevision_) {
                    audioRevision_ = audio.revision;
                    if (!audio.error.empty()) {
                        pendingAudioSource_.clear();
                        ShowRecordingMessage(audio.error, true);
                    } else if (!audio.pending && !pendingAudioSource_.empty()) {
                        ShowRecordingMessage(pendingAudioSource_ +
                            (pendingAudioEnabled_ ? L"已开启" : L"已关闭"));
                        pendingAudioSource_.clear();
                    }
                }
                std::wstring text = L"录制中  " + FormatDuration(session_->elapsed100ns());
                if (std::chrono::steady_clock::now() < transientMessageUntil_) {
                    text = transientMessage_;
                }
                floating_->SetContent(text, true, L"停止");
                floating_->SetRecordingControls(true, audio.pending,
                                                systemAudioEnabled_, microphoneEnabled_);
                floating_->SetMicrophoneLevel(microphoneEnabled_
                    ? session_->microphoneLevel() : 0.0f);
                break;
            }
            case RecorderState::Stopping:
                phase_ = Phase::Saving;
                floating_->SetContent(L"正在停止…", false, L"");
                break;
            case RecorderState::Saving:
                phase_ = Phase::Saving;
                floating_->SetContent(L"正在保存…", false, L"");
                break;
            case RecorderState::Finished:
            case RecorderState::Failed:
                FinishSession(state == RecorderState::Finished);
                break;
            case RecorderState::Idle:
                break;
        }
    }

    void StartSession() {
        stopPressed_ = false;
        session_ = std::make_unique<RecordingSession>();
        std::wstring error;
        if (!session_->Start(config_, &error)) {
            session_.reset();
            phase_ = Phase::Idle;
            DestroyFloating();
            ShowMainWindow();
            SetStatus(L"无法开始录制：" + error, true);
            MessageBoxUi(window_, error.c_str(), L"无法开始录制", MB_ICONERROR | MB_OK);
            UpdateUiState();
            return;
        }
        phase_ = Phase::Starting;
        floating_->SetContent(L"正在启动…", true, L"取消");
    }

    void OnFloatingAction() {
        if (countdownActive_) {
            ::PostMessageW(window_, kCancelCountdownMessage, 0, 0);
            return;
        }
        if (session_ == nullptr || stopPressed_) {
            return;
        }
        const RecorderState state = session_->state();
        if (state == RecorderState::Starting || state == RecorderState::Recording ||
            state == RecorderState::Stopping) {
            stopPressed_ = true;
            stopPressedAt_ = std::chrono::steady_clock::now();
            session_->Stop();
            phase_ = Phase::Saving;
            if (floating_ != nullptr) {
                floating_->SetContent(L"正在保存…", false, L"");
            }
        }
    }

    void CancelCountdown() {
        if (!countdownActive_) {
            return;
        }
        countdownActive_ = false;
        stopPressed_ = false;
        phase_ = Phase::Idle;
        DestroyFloating();
        ShowMainWindow();
        SetStatus(L"已取消录制");
        UpdateUiState();
    }

    void PersistSettings() {
        const MonitorInfo* monitor = SelectedMonitor();
        if (monitor == nullptr) {
            return;
        }
        AppSettings settings;
        settings.monitorDeviceName = monitor->deviceName;
        settings.hasRegion = regionAvailable_;
        settings.region = regionRect_;
        settings.limit = SelectedLimit();
        settings.customBound = customBound_;
        settings.fps = SelectedFps();
        settings.bitrateMbps = SelectedBitrate();
        settings.systemAudio =
            ::SendMessageW(audioCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        settings.microphone =
            ::SendMessageW(micCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        settings.systemAudioDeviceId =
            SelectedDeviceId(systemDeviceCombo_, systemDeviceIds_);
        settings.microphoneDeviceId =
            SelectedDeviceId(micDeviceCombo_, microphoneDeviceIds_);
        settings.outputDirectory = outputDirectory_;
        settings.uiLanguage = ui::LanguagePreference();
        (void)SaveAppSettings(settings);
    }

    void FinishSession(bool finishedStateSuccess) {
        if (session_ == nullptr) {
            return;
        }
        stopPressed_ = false;
        RecorderResult result;
        const bool haveResult = session_->TakeResult(&result);
        session_.reset();
        DestroyFloating();
        ShowMainWindow();
        phase_ = Phase::Idle;

        if (haveResult && result.success) {
            lastOutputPath_ = result.finalPath;
            lastOutputFolder_ = std::filesystem::path(result.finalPath).parent_path().wstring();
            ::EnableWindow(openFolderButton_, TRUE);
            PersistSettings();
        } else {
            std::wstring message = result.error;
            if (message.empty()) {
                message = finishedStateSuccess ? L"录制已结束，但结果不可用" : L"录制失败";
            }
            if (!result.partialPath.empty()) {
                message += ui::Text(L"\r\n未完成文件：") + result.partialPath;
            }
            if (!screenshotFailure_.empty()) {
                message += ui::Text(L"\r\n截图失败：") + screenshotFailure_;
            }
            SetStatus(message, true);
            MessageBoxUi(window_, message.c_str(), L"录制结束", MB_ICONERROR | MB_OK);
        }
        UpdateDerivedState(false);
        UpdateUiState();
        if (haveResult && result.success) {
            SetStatus(screenshotFailure_.empty()
                          ? L"已保存：" + FileNameOf(result.finalPath)
                          : L"视频已保存；截图失败：" + screenshotFailure_,
                      !screenshotFailure_.empty());
            if (!screenshotFailure_.empty()) {
                MessageBoxUi(window_, screenshotFailure_.c_str(), L"截图保存失败",
                              MB_ICONWARNING | MB_OK);
            }
        }
        if (closingPending_) {
            if (!haveResult || !result.success) {
                closingPending_ = false;
            } else if (screenshotWorker_.pending() == 0) {
                closingPending_ = false;
                ::DestroyWindow(window_);
            }
        }
    }

    void DestroyFloating() {
        if (floating_ != nullptr) {
            floating_->Destroy();
            floating_.reset();
        }
    }

    void ShowMainWindow() {
        ::ShowWindow(window_, SW_SHOW);
        ::SetForegroundWindow(window_);
    }

    void SelectRegion() {
        if (phase_ != Phase::Idle || screenshotWorker_.pending() != 0) {
            return;
        }
        ::ShowWindow(window_, SW_HIDE);
        const auto selection = ::srl::SelectRegion(window_, displays_);
        ::ShowWindow(window_, SW_SHOW);
        ::SetForegroundWindow(window_);
        if (!selection.has_value()) {
            UpdateDerivedState(true);
            return;
        }
        int selectedIndex = -1;
        for (std::size_t i = 0; i < displays_.size(); ++i) {
            if (displays_[i].index == selection->monitorIndex) {
                selectedIndex = static_cast<int>(i);
                break;
            }
        }
        if (selectedIndex < 0) {
            SetStatus(L"选择的显示器已不可用", true);
            return;
        }
        selectedMonitor_ = selectedIndex;
        ::SendMessageW(monitorCombo_, CB_SETCURSEL, selectedIndex, 0);
        regionRect_ = selection->rect;
        regionAvailable_ = true;
        ::CheckRadioButton(window_, kModeFull, kModeRegion, kModeRegion);
        UpdateDerivedState(true);
        UpdateUiState();
    }

    void BrowseOutput() {
        if (phase_ != Phase::Idle || screenshotWorker_.pending() != 0) {
            return;
        }
        std::wstring selected;
        if (PickFolder(window_, selected)) {
            outputDirectory_ = std::move(selected);
            ::SetWindowTextW(outputPath_, outputDirectory_.c_str());
            UpdateDerivedState(true);
            UpdateUiState();
        }
    }

    void OpenOutputFolder() {
        if (lastOutputFolder_.empty()) {
            return;
        }
        ::ShellExecuteW(window_, L"open", lastOutputFolder_.c_str(), nullptr, nullptr,
                        SW_SHOWNORMAL);
    }

    void ShowRecordingMessage(const std::wstring& message, bool error = false) {
        if (phase_ == Phase::Recording && floating_ != nullptr) {
            transientMessage_ = message.starts_with(L"已截图：") ? L"截图已保存" : message;
            transientMessageUntil_ = std::chrono::steady_clock::now() + std::chrono::seconds(3);
            floating_->SetContent(transientMessage_, true, L"停止");
        }
        SetStatus(message, error);
    }

    void ToggleRecordingAudio(bool systemAudio) {
        if (phase_ != Phase::Recording || session_ == nullptr || stopPressed_) return;
        const bool systemEnabled = systemAudio ? !systemAudioEnabled_ : systemAudioEnabled_;
        const bool micEnabled = systemAudio ? microphoneEnabled_ : !microphoneEnabled_;
        if (!session_->SetAudioEnabled(systemEnabled, micEnabled)) {
            ShowRecordingMessage(L"当前无法更改录制声音", true);
            return;
        }
        pendingAudioSource_ = systemAudio ? L"系统声音" : L"麦克风";
        pendingAudioEnabled_ = systemAudio ? systemEnabled : micEnabled;
        floating_->SetRecordingControls(true, true, systemAudioEnabled_, microphoneEnabled_);
        ShowRecordingMessage((pendingAudioEnabled_ ? L"正在开启" : L"正在关闭") +
                             pendingAudioSource_ + L"…");
    }

    void PollScreenshots() {
        ScreenshotEvent event;
        bool completed = false;
        while (screenshotWorker_.TryPop(event)) {
            if (event.type == ScreenshotEvent::Type::Captured) {
                if (phase_ == Phase::Screenshot) ShowMainWindow();
                continue;
            }
            completed = true;
            if (event.idleCapture && phase_ == Phase::Screenshot) {
                phase_ = Phase::Idle;
                ShowMainWindow();
            }
            if (event.result.ok) {
                lastOutputFolder_ = std::filesystem::path(event.result.path).parent_path().wstring();
                if (screenshotFailure_.empty()) {
                    ShowRecordingMessage(L"已截图：" + FileNameOf(event.result.path));
                } else {
                    ShowRecordingMessage(L"截图失败：" + screenshotFailure_, true);
                }
            } else {
                closingPending_ = false;
                if (!screenshotFailure_.empty()) screenshotFailure_ += L"\r\n";
                screenshotFailure_ += event.result.error;
                ShowRecordingMessage(L"截图失败：" + screenshotFailure_, true);
            }
        }
        if (completed) UpdateUiState();
        if (completed && closingPending_ && session_ == nullptr &&
            screenshotWorker_.pending() == 0) {
            closingPending_ = false;
            ::DestroyWindow(window_);
        }
    }

    void TakeScreenshot() {
        if (closingPending_ || stopPressed_ ||
            (phase_ != Phase::Idle && phase_ != Phase::Recording)) {
            ShowRecordingMessage(L"当前状态不能截图", true);
            return;
        }
        if (screenshotWorker_.pending() >= 2 ||
            (phase_ == Phase::Idle && screenshotWorker_.pending() != 0)) {
            ShowRecordingMessage(L"截图正在保存，请稍后", true);
            return;
        }
        std::wstring error;
        const bool firstRequest = screenshotWorker_.pending() == 0;
        if (phase_ == Phase::Recording) {
            ScreenshotFrame frame;
            if (session_ == nullptr || !session_->CopyScreenshot(frame, &error) ||
                !screenshotWorker_.SubmitSnapshot(std::move(frame), config_.outputDirectory, &error)) {
                ShowRecordingMessage(L"截图失败：" + error, true);
                return;
            }
            if (firstRequest) screenshotFailure_.clear();
            ShowRecordingMessage(L"正在保存截图…");
            return;
        }
        const MonitorInfo* monitor = SelectedMonitor();
        if (monitor == nullptr || (RegionMode() && !regionAvailable_)) {
            SetStatus(L"请先选择有效的截图范围", true);
            return;
        }
        if (outputDirectory_.empty()) {
            SetStatus(L"请选择截图保存位置", true);
            return;
        }
        const RECT source = CurrentSourceRect();
        phase_ = Phase::Screenshot;
        UpdateUiState();
        ::ShowWindow(window_, SW_HIDE);
        if (!screenshotWorker_.SubmitCapture(monitor->handle, source, outputDirectory_, &error)) {
            phase_ = Phase::Idle;
            ShowMainWindow();
            SetStatus(L"截图失败：" + error, true);
            UpdateUiState();
        } else {
            screenshotFailure_.clear();
        }
    }

    void OnCommand(int id, int notification) {
        switch (id) {
            case kLanguageCombo:
                if (notification == CBN_SELCHANGE) {
                    ui::SetLanguagePreference(static_cast<int>(::SendMessageW(
                        languageCombo_, CB_GETCURSEL, 0, 0)));
                    RefreshLocalizedControls();
                    PersistSettings();
                }
                break;
            case kModeFull:
            case kModeRegion:
                if (notification == BN_CLICKED) {
                    ::CheckRadioButton(window_, kModeFull, kModeRegion, id);
                    UpdateDerivedState(true);
                    UpdateUiState();
                }
                break;
            case kMonitorCombo:
                if (notification == CBN_SELCHANGE) {
                    const LRESULT data =
                        ::SendMessageW(monitorCombo_, CB_GETITEMDATA,
                                       ::SendMessageW(monitorCombo_, CB_GETCURSEL, 0, 0), 0);
                    if (data >= 0 && static_cast<std::size_t>(data) < displays_.size()) {
                        selectedMonitor_ = static_cast<int>(data);
                    }
                    regionAvailable_ = false;
                    regionRect_ = {};
                    UpdateDerivedState(true);
                    UpdateUiState();
                }
                break;
            case kResolutionCombo:
                if (notification == CBN_SELCHANGE) {
                    const int index = ComboSelection(resolutionCombo_);
                    if (index == 5) {
                        PromptCustomResolution();
                    } else {
                        lastResolutionIndex_ = index;
                        UpdateDerivedState(true);
                        UpdateUiState();
                    }
                }
                break;
            case kFpsCombo:
            case kQualityCombo:
                if (notification == CBN_SELCHANGE) {
                    UpdateDerivedState(true);
                    UpdateUiState();
                }
                break;
            case kAudioCheck:
            case kMicCheck:
                if (notification == BN_CLICKED) {
                    UpdateDerivedState(true);
                    UpdateUiState();
                }
                break;
            case kSystemDeviceCombo:
            case kMicDeviceCombo:
                if (notification == CBN_SELCHANGE) {
                    UpdateDerivedState(true);
                    UpdateUiState();
                }
                break;
            case kSelectRegionButton:
                SelectRegion();
                break;
            case kBrowseButton:
                BrowseOutput();
                break;
            case kStartButton:
                StartRecording();
                break;
            case kOpenFolderButton:
                OpenOutputFolder();
                break;
            case kScreenshotButton:
                TakeScreenshot();
                break;
            default:
                break;
        }
    }

    LRESULT WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        switch (message) {
            case WM_COMMAND:
                OnCommand(LOWORD(wParam), HIWORD(wParam));
                return 0;
            case kCancelCountdownMessage:
                CancelCountdown();
                return 0;
            case WM_HOTKEY:
                if (wParam == kHotkeyStartStop) {
                    if (countdownActive_) {
                        CancelCountdown();
                    } else if (session_ != nullptr) {
                        OnFloatingAction();
                    } else {
                        StartRecording();
                    }
                } else if (wParam == kHotkeyScreenshot) {
                    TakeScreenshot();
                } else if (wParam == kHotkeyFloating && phase_ == Phase::Recording &&
                           floating_ != nullptr && hotkeyStartStop_) {
                    if (::IsWindowVisible(floating_->hwnd())) {
                        ::ShowWindow(floating_->hwnd(), SW_HIDE);
                    } else {
                        ::ShowWindow(floating_->hwnd(), SW_SHOWNA);
                    }
                }
                return 0;
            case WM_TIMER:
                if (wParam == kUiTimerId) {
                    OnTimer();
                }
                return 0;
            case WM_SIZE:
                Layout();
                return 0;
            case WM_GETFONT:
                return reinterpret_cast<LRESULT>(bodyFont_);
            case WM_ERASEBKGND:
                return 1;
            case WM_PAINT: {
                PAINTSTRUCT paint{};
                HDC dc = ::BeginPaint(window, &paint);
                PaintWindow(dc);
                ::EndPaint(window, &paint);
                return 0;
            }
            case WM_CTLCOLORSTATIC:
                return OnCtlColor(reinterpret_cast<HDC>(wParam),
                                  reinterpret_cast<HWND>(lParam));
            case WM_CTLCOLORBTN:
                return OnCtlColor(reinterpret_cast<HDC>(wParam),
                                  reinterpret_cast<HWND>(lParam));
            case WM_DRAWITEM: {
                const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
                if (item->CtlType == ODT_BUTTON) {
                    ui::ButtonStyle style = ui::ButtonStyle::Secondary;
                    if (item->CtlID == kStartButton) {
                        style = ui::ButtonStyle::Primary;
                    }
                    ui::DrawModernButton(*item, style, dpi_);
                    return TRUE;
                }
                break;
            }
            case WM_GETMINMAXINFO: {
                auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
                RECT rect{0, 0, Scale(dpi_, kMinimumClientWidth),
                          Scale(dpi_, kMinimumClientHeight)};
                ::AdjustWindowRectExForDpi(&rect, WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
                                           FALSE, 0, dpi_);
                info->ptMinTrackSize.x = rect.right - rect.left;
                info->ptMinTrackSize.y = rect.bottom - rect.top;
                return 0;
            }
            case WM_DPICHANGED: {
                dpi_ = HIWORD(wParam);
                RecreateFont();
                const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
                ::SetWindowPos(window_, nullptr, suggested->left, suggested->top,
                               suggested->right - suggested->left,
                               suggested->bottom - suggested->top,
                               SWP_NOZORDER | SWP_NOACTIVATE);
                Layout();
                return 0;
            }
            case WM_CLOSE:
                if (countdownActive_) {
                    CancelCountdown();
                    ::DestroyWindow(window_);
                } else if (session_ != nullptr) {
                    closingPending_ = true;
                    if (!stopPressed_) {
                        stopPressed_ = true;
                        stopPressedAt_ = std::chrono::steady_clock::now();
                        session_->Stop();
                    }
                    if (floating_ != nullptr) {
                        floating_->SetContent(L"正在保存…", false, L"");
                    }
                } else if (screenshotWorker_.pending() != 0) {
                    closingPending_ = true;
                    SetStatus(L"正在保存截图，完成后退出…");
                } else {
                    ::DestroyWindow(window_);
                }
                return 0;
            case WM_DESTROY:
                ::KillTimer(window_, kUiTimerId);
                if (hotkeyStartStop_) {
                    ::UnregisterHotKey(window_, kHotkeyStartStop);
                    hotkeyStartStop_ = false;
                }
                if (hotkeyScreenshot_) {
                    ::UnregisterHotKey(window_, kHotkeyScreenshot);
                    hotkeyScreenshot_ = false;
                }
                if (hotkeyFloating_) {
                    ::UnregisterHotKey(window_, kHotkeyFloating);
                    hotkeyFloating_ = false;
                }
                ::PostQuitMessage(0);
                return 0;
            default:
                return ::DefWindowProcW(window, message, wParam, lParam);
        }
        return ::DefWindowProcW(window, message, wParam, lParam);
    }

    void StopAndCleanup() {
        if (session_ != nullptr) {
            session_->Stop();
            session_.reset();
        }
        DestroyFloating();
        if (window_ != nullptr) {
            ::DestroyWindow(window_);
            window_ = nullptr;
        }
    }

    enum class Phase { Idle, Countdown, Starting, Recording, Saving, Screenshot };

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    UINT dpi_ = 96;
    HFONT titleFont_ = nullptr;
    HFONT headingFont_ = nullptr;
    HFONT bodyFont_ = nullptr;
    HFONT smallFont_ = nullptr;
    HBRUSH cardBrush_ = nullptr;
    HBRUSH backgroundBrush_ = nullptr;
    bool comInitialized_ = false;
    bool hotkeyStartStop_ = false;
    bool hotkeyScreenshot_ = false;
    bool hotkeyFloating_ = false;
    bool statusError_ = false;

    HWND titleLabel_ = nullptr;
    HWND languageLabel_ = nullptr;
    HWND languageCombo_ = nullptr;
    HWND subtitleLabel_ = nullptr;
    HWND sectionRange_ = nullptr;
    HWND sectionQuality_ = nullptr;
    HWND sectionAudio_ = nullptr;
    HWND sectionOutput_ = nullptr;
    HWND fullRadio_ = nullptr;
    HWND regionRadio_ = nullptr;
    HWND monitorLabel_ = nullptr;
    HWND monitorCombo_ = nullptr;
    HWND regionButton_ = nullptr;
    HWND regionSummary_ = nullptr;
    HWND resolutionLabel_ = nullptr;
    HWND resolutionCombo_ = nullptr;
    HWND fpsLabel_ = nullptr;
    HWND fpsCombo_ = nullptr;
    HWND qualityLabel_ = nullptr;
    HWND qualityCombo_ = nullptr;
    HWND qualityHint_ = nullptr;
    HWND audioCheck_ = nullptr;
    HWND micCheck_ = nullptr;
    HWND systemDeviceCombo_ = nullptr;
    HWND micDeviceCombo_ = nullptr;
    HWND outputLabel_ = nullptr;
    HWND outputPath_ = nullptr;
    HWND browseButton_ = nullptr;
    HWND outputSummary_ = nullptr;
    HWND statusLabel_ = nullptr;
    HWND startButton_ = nullptr;
    HWND openFolderButton_ = nullptr;
    HWND screenshotButton_ = nullptr;

    std::vector<MonitorInfo> displays_;
    int selectedMonitor_ = 0;
    bool regionAvailable_ = false;
    RECT regionRect_{};
    Size customBound_{1920, 1080};
    int lastResolutionIndex_ = 2;
    bool configValid_ = false;
    std::wstring outputDirectory_;
    std::vector<std::wstring> systemDeviceIds_;
    std::vector<std::wstring> microphoneDeviceIds_;
    RECT cardRect_{};
    std::vector<int> separators_;
    std::vector<int> sectionTops_;

    Phase phase_ = Phase::Idle;
    bool stopPressed_ = false;
    std::chrono::steady_clock::time_point stopPressedAt_{};
    bool countdownActive_ = false;
    std::chrono::steady_clock::time_point countdownEnd_{};
    int lastCountdownValue_ = -1;
    std::wstring transientMessage_;
    std::chrono::steady_clock::time_point transientMessageUntil_{};
    RecordingConfig config_;
    std::unique_ptr<RecordingSession> session_;
    std::unique_ptr<FloatingControl> floating_;
    bool closingPending_ = false;
    bool systemAudioEnabled_ = false;
    bool microphoneEnabled_ = false;
    std::uint64_t audioRevision_ = 0;
    std::wstring pendingAudioSource_;
    bool pendingAudioEnabled_ = false;
    ScreenshotWorker screenshotWorker_;
    std::wstring screenshotFailure_;
    std::wstring lastOutputPath_;
    std::wstring lastOutputFolder_;
};

BOOL CALLBACK FindMainWindowProc(HWND window, LPARAM parameter) {
    wchar_t className[128]{};
    if (::GetClassNameW(window, className, ARRAYSIZE(className)) != 0 &&
        std::wstring_view(className) == kMainClass) {
        *reinterpret_cast<HWND*>(parameter) = window;
        return FALSE;
    }
    return TRUE;
}

int RunApplication(HINSTANCE instance, int showCommand) {
    HANDLE mutex = ::CreateMutexW(nullptr, FALSE, kSingleInstanceName);
    if (mutex == nullptr) {
        return 1;
    }
    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = nullptr;
        ::EnumWindows(FindMainWindowProc, reinterpret_cast<LPARAM>(&existing));
        if (existing != nullptr && ::IsWindowVisible(existing)) {
            ::ShowWindow(existing, SW_RESTORE);
            ::SetForegroundWindow(existing);
        }
        ::CloseHandle(mutex);
        return 0;
    }

    int result = 0;
    {
        Application application(instance);
        result = application.Run(showCommand);
    }
    ::ReleaseMutex(mutex);
    ::CloseHandle(mutex);
    return result;
}

}  // namespace srl
