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
#include <span>
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
#include "core/recording_format.h"
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
    kFormatLabel,
    kFormatCombo,
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
    ui::TextId name;
    int bitrateMbps;
};

constexpr QualityOption kQualityOptions[] = {
    {ui::TextId::QualityVeryHigh, 20},
    {ui::TextId::QualityHigh, 12},
    {ui::TextId::QualityMedium, 8},
    {ui::TextId::QualityLow, 4},
};
constexpr int kQualityCount = static_cast<int>(std::size(kQualityOptions));
constexpr int kDefaultQualityIndex = 2;
constexpr ui::TextId kResolutionChoices[] = {
    ui::TextId::OriginalSize, ui::TextId::Resolution720p, ui::TextId::Resolution1080p,
    ui::TextId::Resolution1440p, ui::TextId::Resolution2160p, ui::TextId::CustomSize,
};
constexpr UINT kUiRefreshIntervalMs = 200;
constexpr std::uint32_t kEstimateDurationSeconds = 60;
constexpr auto kCountdownDuration = std::chrono::seconds(3);
constexpr auto kTransientMessageDuration = std::chrono::seconds(3);
constexpr auto kSavingProgressDelay = std::chrono::seconds(10);
constexpr std::size_t kMaxPendingScreenshots = 2;

int Scale(UINT dpi, int value) {
    return MulDiv(value, static_cast<int>(dpi), 96);
}

int FontHeight(UINT dpi, int points) {
    return -MulDiv(points, static_cast<int>(dpi), 72);
}

std::wstring FormatDuration(std::int64_t hundredNanoseconds) {
    const std::int64_t totalSeconds =
        std::max<std::int64_t>(0, hundredNanoseconds) / kHundredNanosecondsPerSecond;
    const std::int64_t hours = totalSeconds / 3600;
    const std::int64_t minutes = (totalSeconds / 60) % 60;
    const std::int64_t seconds = totalSeconds % 60;
    return std::format(L"{:02}:{:02}:{:02}", hours, minutes, seconds);
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
    const std::wstring title = ui::Text(ui::TextId::SelectExportFolder);
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

int MessageBoxUi(HWND owner, std::wstring_view message, ui::TextId title,
                 UINT flags) {
    const std::wstring localizedMessage = ui::LocalizeMessage(message);
    const std::wstring localizedTitle = ui::Text(title);
    return ::MessageBoxW(owner, localizedMessage.c_str(), localizedTitle.c_str(), flags);
}

int MessageBoxUi(HWND owner, ui::TextId message, ui::TextId title, UINT flags) {
    return MessageBoxUi(owner, ui::Text(message), title, flags);
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
            MessageBoxUi(nullptr, ui::TextId::ComInitializationFailed, ui::TextId::AppName,
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
        const std::wstring windowTitle = ui::Text(ui::TextId::AppName);
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

        ::SetTimer(window_, kUiTimerId, kUiRefreshIntervalMs, nullptr);
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
            SetStatus(ui::TextId::ScreenshotHotkeyUnavailable);
        } else if (!hotkeyStartStop_) {
            SetStatus(ui::TextId::RecordingHotkeyUnavailable);
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
            formatLabel_, formatCombo_,
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

    HWND CreateControl(const wchar_t* className, ui::TextId text, DWORD style,
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
        const ui::TextId choices[] = {ui::TextId::LanguageAutomatic,
            ui::TextId::LanguageChinese, ui::TextId::LanguageEnglish};
        for (std::size_t i = 0; i < std::size(choices); ++i) {
            const std::wstring label = ui::Text(choices[i]);
            ::SendMessageW(languageCombo_, CB_ADDSTRING, 0,
                           reinterpret_cast<LPARAM>(label.c_str()));
        }
        ::SendMessageW(languageCombo_, CB_SETCURSEL, ui::LanguagePreference(), 0);
    }

    void PopulateResolutionChoices(int selected) {
        ::SendMessageW(resolutionCombo_, CB_RESETCONTENT, 0, 0);
        for (std::size_t i = 0; i < std::size(kResolutionChoices); ++i) {
            const std::wstring label = ui::Text(kResolutionChoices[i]);
            const LRESULT item = ::SendMessageW(resolutionCombo_, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(label.c_str()));
            ::SendMessageW(resolutionCombo_, CB_SETITEMDATA, item, static_cast<LPARAM>(i));
        }
        ::SendMessageW(resolutionCombo_, CB_SETCURSEL, selected, 0);
    }

    void PopulateQualityChoices(int selected) {
        ::SendMessageW(qualityCombo_, CB_RESETCONTENT, 0, 0);
        for (int i = 0; i < kQualityCount; ++i) {
            const std::wstring label = ui::Text(kQualityOptions[i].name);
            const LRESULT item = ::SendMessageW(qualityCombo_, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(label.c_str()));
            ::SendMessageW(qualityCombo_, CB_SETITEMDATA, item, i);
        }
        ::SendMessageW(qualityCombo_, CB_SETCURSEL, selected, 0);
    }

    std::span<const int> FrameRateOptions() const {
        return format_ == RecordingFormat::Gif ? std::span<const int>(kGifFrameRates)
                                               : std::span<const int>(kMp4FrameRates);
    }

    void PopulateFrameRateChoices(int selectedFps) {
        ::SendMessageW(fpsCombo_, CB_RESETCONTENT, 0, 0);
        int selected = 0;
        int index = 0;
        for (const int fps : FrameRateOptions()) {
            const std::wstring label = ui::Format(ui::TextId::FrameRateValue, fps);
            ::SendMessageW(fpsCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            if (fps == selectedFps) selected = index;
            ++index;
        }
        SetComboSelection(fpsCombo_, selected);
    }

    void RememberFormatOptions() {
        if (format_ == RecordingFormat::Gif) {
            gifLimit_ = SelectedLimit();
            gifCustomBound_ = customBound_;
            gifFps_ = SelectedFps();
        } else {
            mp4Limit_ = SelectedLimit();
            mp4CustomBound_ = customBound_;
            mp4Fps_ = SelectedFps();
        }
    }

    void ApplyFormatOptions() {
        const bool gif = format_ == RecordingFormat::Gif;
        customBound_ = gif ? gifCustomBound_ : mp4CustomBound_;
        lastResolutionIndex_ = static_cast<int>(gif ? gifLimit_ : mp4Limit_);
        SetComboSelection(resolutionCombo_, lastResolutionIndex_);
        PopulateFrameRateChoices(gif ? gifFps_ : mp4Fps_);
    }

    void ChangeFormat() {
        const RecordingFormat selected = ComboSelection(formatCombo_) == 1
            ? RecordingFormat::Gif : RecordingFormat::Mp4;
        if (selected == format_) return;
        RememberFormatOptions();
        format_ = selected;
        ApplyFormatOptions();
        UpdateDerivedState(true);
        UpdateUiState();
        PersistSettings();
    }

    void RefreshLocalizedControls() {
        const auto set = [](HWND control, ui::TextId source) {
            const std::wstring label = ui::Text(source);
            ::SetWindowTextW(control, label.c_str());
        };
        ::SetWindowTextW(window_, ui::Text(ui::TextId::AppName).c_str());
        set(titleLabel_, ui::TextId::AppName);
        set(languageLabel_, ui::TextId::LanguageLabel);
        set(subtitleLabel_, ui::TextId::AppSubtitle);
        set(sectionRange_, ui::TextId::CaptureArea);
        set(sectionQuality_, ui::TextId::Quality);
        set(sectionAudio_, ui::TextId::Audio);
        set(sectionOutput_, ui::TextId::SaveLocation);
        set(fullRadio_, ui::TextId::FullScreen);
        set(regionRadio_, ui::TextId::Region);
        set(monitorLabel_, ui::TextId::Display);
        set(regionButton_, ui::TextId::SelectRegion);
        set(resolutionLabel_, ui::TextId::Resolution);
        set(fpsLabel_, ui::TextId::FrameRate);
        set(qualityLabel_, ui::TextId::Quality);
        set(formatLabel_, ui::TextId::FormatLabel);
        set(qualityHint_, format_ == RecordingFormat::Gif ? ui::TextId::GifSilent : ui::TextId::QualityHint);
        set(audioCheck_, ui::TextId::SystemAudio);
        set(micCheck_, ui::TextId::Microphone);
        set(outputLabel_, ui::TextId::SaveTo);
        set(browseButton_, ui::TextId::Browse);
        set(openFolderButton_, ui::TextId::OpenFolder);
        set(screenshotButton_, ui::TextId::Screenshot);
        PopulateLanguageCombo();
        PopulateMonitorChoices(ComboSelection(monitorCombo_));
        RelabelDefaultDeviceChoice(systemDeviceCombo_, ui::TextId::DefaultPlaybackDevice);
        RelabelDefaultDeviceChoice(micDeviceCombo_, ui::TextId::DefaultInputDevice);

        PopulateResolutionChoices(ComboSelection(resolutionCombo_));
        PopulateQualityChoices(ComboSelection(qualityCombo_));
        UpdateDerivedState(true);
        UpdateUiState();
        Layout();
    }

    void CreateControls() {
        titleLabel_ = CreateControl(L"STATIC", ui::TextId::AppName, SS_LEFT, kTitleLabel);
        languageLabel_ = CreateControl(L"STATIC", ui::TextId::LanguageLabel,
            SS_LEFT | SS_CENTERIMAGE, kLanguageLabel);
        languageCombo_ = CreateControl(L"COMBOBOX", ui::TextId::Empty,
            CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kLanguageCombo);
        ThemeControl(languageCombo_);
        PopulateLanguageCombo();
        subtitleLabel_ = CreateControl(L"STATIC", ui::TextId::AppSubtitle, SS_LEFT,
                                       kSubtitleLabel);

        sectionRange_ = CreateControl(L"STATIC", ui::TextId::CaptureArea, SS_LEFT, kSectionRange);
        sectionQuality_ = CreateControl(L"STATIC", ui::TextId::Quality, SS_LEFT, kSectionQuality);
        sectionAudio_ = CreateControl(L"STATIC", ui::TextId::Audio, SS_LEFT, kSectionAudio);
        sectionOutput_ = CreateControl(L"STATIC", ui::TextId::SaveLocation, SS_LEFT, kSectionOutput);

        fullRadio_ = CreateControl(L"BUTTON", ui::TextId::FullScreen,
                                   BS_AUTORADIOBUTTON | WS_GROUP | WS_TABSTOP, kModeFull);
        regionRadio_ = CreateControl(L"BUTTON", ui::TextId::Region, BS_AUTORADIOBUTTON | WS_TABSTOP,
                                     kModeRegion);
        ::CheckRadioButton(window_, kModeFull, kModeRegion, kModeFull);

        monitorLabel_ = CreateControl(L"STATIC", ui::TextId::Display, SS_LEFT | SS_CENTERIMAGE,
                                      kLabelMonitor);
        monitorCombo_ = CreateControl(L"COMBOBOX", ui::TextId::Empty,
                                      CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP | WS_GROUP,
                                      kMonitorCombo);
        ThemeControl(monitorCombo_);
        regionButton_ = CreateControl(L"BUTTON", ui::TextId::SelectRegion,
                                      BS_OWNERDRAW | WS_TABSTOP, kSelectRegionButton);
        regionSummary_ = CreateControl(L"STATIC", ui::TextId::NoRegionSelected,
                                       SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS,
                                       kRegionSummary);

        resolutionLabel_ = CreateControl(L"STATIC", ui::TextId::Resolution, SS_LEFT | SS_CENTERIMAGE,
                                          kLabelResolution);
        resolutionCombo_ = CreateControl(L"COMBOBOX", ui::TextId::Empty,
                                          CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP | WS_GROUP,
                                          kResolutionCombo);
        ThemeControl(resolutionCombo_);
        PopulateResolutionChoices(-1);

        fpsLabel_ = CreateControl(L"STATIC", ui::TextId::FrameRate, SS_LEFT | SS_CENTERIMAGE, kLabelFps);
        fpsCombo_ = CreateControl(L"COMBOBOX", ui::TextId::Empty,
                                  CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP | WS_GROUP,
                                  kFpsCombo);
        ThemeControl(fpsCombo_);
        PopulateFrameRateChoices(kDefaultMp4FrameRate);

        qualityLabel_ = CreateControl(L"STATIC", ui::TextId::Quality, SS_LEFT | SS_CENTERIMAGE,
                                      kLabelQuality);
        qualityCombo_ = CreateControl(L"COMBOBOX", ui::TextId::Empty,
                                      CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP | WS_GROUP,
                                      kQualityCombo);
        ThemeControl(qualityCombo_);
        PopulateQualityChoices(-1);
        qualityHint_ = CreateControl(L"STATIC", ui::TextId::QualityHint, SS_LEFT | SS_CENTERIMAGE,
                                     kQualityHint);
        formatLabel_ = CreateControl(L"STATIC", ui::TextId::FormatLabel,
            SS_LEFT | SS_CENTERIMAGE, kFormatLabel);
        formatCombo_ = CreateControl(L"COMBOBOX", ui::TextId::Empty,
            CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kFormatCombo);
        ThemeControl(formatCombo_);
        for (ui::TextId choice : {ui::TextId::FormatMp4, ui::TextId::FormatGif}) {
            const std::wstring label = ui::Text(choice);
            ::SendMessageW(formatCombo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        }
        SetComboSelection(formatCombo_, 0);

        audioCheck_ = CreateControl(L"BUTTON", ui::TextId::SystemAudio,
                                    BS_AUTOCHECKBOX | WS_TABSTOP | WS_GROUP, kAudioCheck);
        micCheck_ = CreateControl(L"BUTTON", ui::TextId::Microphone,
                                  BS_AUTOCHECKBOX | WS_TABSTOP, kMicCheck);
        systemDeviceCombo_ = CreateControl(
            L"COMBOBOX", ui::TextId::Empty, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kSystemDeviceCombo);
        micDeviceCombo_ = CreateControl(
            L"COMBOBOX", ui::TextId::Empty, CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kMicDeviceCombo);
        ThemeControl(systemDeviceCombo_);
        ThemeControl(micDeviceCombo_);

        outputLabel_ = CreateControl(L"STATIC", ui::TextId::SaveTo, SS_LEFT | SS_CENTERIMAGE,
                                     kLabelOutput);
        outputPath_ = CreateControl(L"STATIC", ui::TextId::Empty, SS_LEFT | SS_CENTERIMAGE | SS_ENDELLIPSIS,
                                    kOutputPath);
        browseButton_ = CreateControl(L"BUTTON", ui::TextId::Browse, BS_OWNERDRAW | WS_TABSTOP,
                                      kBrowseButton);
        outputSummary_ = CreateControl(L"STATIC", ui::TextId::Empty, SS_LEFT | SS_NOPREFIX, kOutputSummary);

        statusLabel_ = CreateControl(L"STATIC", ui::TextId::Ready, SS_LEFT | SS_CENTERIMAGE,
                                     kStatusLabel);
        openFolderButton_ = CreateControl(L"BUTTON", ui::TextId::OpenFolder,
                                          BS_OWNERDRAW | WS_TABSTOP, kOpenFolderButton);
        screenshotButton_ = CreateControl(L"BUTTON", ui::TextId::Screenshot, BS_OWNERDRAW | WS_TABSTOP,
                                          kScreenshotButton);
        startButton_ = CreateControl(L"BUTTON", ui::TextId::StartRecording, BS_OWNERDRAW | WS_TABSTOP,
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

        const auto labelWidth = [&](HWND control) -> int {
            std::wstring text(static_cast<std::size_t>(::GetWindowTextLengthW(control)) + 1, L'\0');
            const int length = ::GetWindowTextW(control, text.data(), static_cast<int>(text.size()));
            text.resize(static_cast<std::size_t>(length));
            HDC dc = ::GetDC(control);
            const auto font = reinterpret_cast<HFONT>(::SendMessageW(control, WM_GETFONT, 0, 0));
            const auto previous = ::SelectObject(dc, font);
            SIZE size{};
            ::GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
            ::SelectObject(dc, previous);
            ::ReleaseDC(control, dc);
            return size.cx + Scale(dpi_, 4);
        };
        const int languageWidth = std::max(Scale(dpi_, 48), labelWidth(languageLabel_));
        const int fullRadioWidth = std::max(Scale(dpi_, 68), labelWidth(fullRadio_) + Scale(dpi_, 22));
        const int fieldLabelWidth = std::max({Scale(dpi_, 48), labelWidth(resolutionLabel_), labelWidth(qualityLabel_)});
        const int fieldX = left + fieldLabelWidth + Scale(dpi_, 8);
        const int secondColumnX = left + Scale(dpi_, 264);
        const int fieldWidth = secondColumnX - fieldX - Scale(dpi_, 18);
        const int secondLabelWidth = std::max({Scale(dpi_, 48), labelWidth(fpsLabel_), labelWidth(formatLabel_)});
        const int secondFieldX = secondColumnX + secondLabelWidth + Scale(dpi_, 8);

        ::MoveWindow(titleLabel_, margin, Scale(dpi_, 12), width - margin * 2,
                     Scale(dpi_, 36), TRUE);
        ::MoveWindow(languageLabel_, width - margin - Scale(dpi_, 124) - Scale(dpi_, 8) - languageWidth,
                     Scale(dpi_, 14), languageWidth, Scale(dpi_, 28), TRUE);
        ::MoveWindow(languageCombo_, width - margin - Scale(dpi_, 124),
                     Scale(dpi_, 14), Scale(dpi_, 124), Scale(dpi_, 28), TRUE);
        ::MoveWindow(subtitleLabel_, margin, Scale(dpi_, 48), width - margin * 2,
                     Scale(dpi_, 22), TRUE);
        ::MoveWindow(sectionRange_, left, labelY1, right - left, Scale(dpi_, 24), TRUE);
        ::MoveWindow(sectionQuality_, left, labelY2, Scale(dpi_, 80), Scale(dpi_, 24), TRUE);
        ::MoveWindow(sectionAudio_, left, labelY3, right - left, Scale(dpi_, 24), TRUE);
        ::MoveWindow(sectionOutput_, left, labelY4, right - left, Scale(dpi_, 24), TRUE);

        ::MoveWindow(fullRadio_, left, radioY, fullRadioWidth, Scale(dpi_, 26), TRUE);
        ::MoveWindow(regionRadio_, left + fullRadioWidth + Scale(dpi_, 14), radioY, Scale(dpi_, 68),
                     Scale(dpi_, 26), TRUE);
        ::MoveWindow(monitorLabel_, left, monitorY, Scale(dpi_, 48), Scale(dpi_, 24), TRUE);
        ::MoveWindow(monitorCombo_, left + Scale(dpi_, 56), monitorY, Scale(dpi_, 294),
                     Scale(dpi_, 200), TRUE);
        ::MoveWindow(regionButton_, left, regionY, Scale(dpi_, 112), rowHeight, TRUE);
        ::MoveWindow(regionSummary_, left + Scale(dpi_, 124), regionY,
                     right - (left + Scale(dpi_, 124)), Scale(dpi_, 28), TRUE);

        ::MoveWindow(resolutionLabel_, left, qualityRowY, fieldLabelWidth, Scale(dpi_, 24),
                     TRUE);
        ::MoveWindow(resolutionCombo_, fieldX, qualityRowY, fieldWidth,
                     Scale(dpi_, 200), TRUE);
        ::MoveWindow(fpsLabel_, secondColumnX, qualityRowY, secondLabelWidth,
                     Scale(dpi_, 24), TRUE);
        ::MoveWindow(fpsCombo_, secondFieldX, qualityRowY, Scale(dpi_, 100),
                     Scale(dpi_, 200), TRUE);
        ::MoveWindow(qualityLabel_, left, qualityRow2Y, fieldLabelWidth, Scale(dpi_, 24),
                     TRUE);
        ::MoveWindow(qualityCombo_, fieldX, qualityRow2Y, fieldWidth,
                     Scale(dpi_, 200), TRUE);
        ::MoveWindow(qualityHint_, left + Scale(dpi_, 96), labelY2,
                     right - (left + Scale(dpi_, 96)), Scale(dpi_, 24), TRUE);
        ::MoveWindow(formatLabel_, secondColumnX, qualityRow2Y,
                     secondLabelWidth, Scale(dpi_, 24), TRUE);
        ::MoveWindow(formatCombo_, secondFieldX, qualityRow2Y,
                     Scale(dpi_, 100), Scale(dpi_, 160), TRUE);

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

    void RelabelDefaultDeviceChoice(HWND combo, ui::TextId label) {
        if (::SendMessageW(combo, CB_GETCOUNT, 0, 0) <= 0) return;
        const LRESULT selected = ::SendMessageW(combo, CB_GETCURSEL, 0, 0);
        const LRESULT defaultData = ::SendMessageW(combo, CB_GETITEMDATA, 0, 0);
        const std::wstring text = ui::Text(label);
        const LRESULT inserted = ::SendMessageW(combo, CB_INSERTSTRING, 0,
            reinterpret_cast<LPARAM>(text.c_str()));
        if (inserted >= 0) {
            ::SendMessageW(combo, CB_SETITEMDATA, inserted, defaultData);
            ::SendMessageW(combo, CB_DELETESTRING, inserted + 1, 0);
            ::SendMessageW(combo, CB_SETCURSEL, selected, 0);
        }
    }

    void PopulateDeviceCombo(HWND combo, ui::TextId defaultLabel,
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
                name = ui::Text(ui::TextId::AudioDevice);
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
        PopulateDeviceCombo(systemDeviceCombo_, ui::TextId::DefaultPlaybackDevice, probe.renderEndpoints,
                            systemDeviceIds_);
        PopulateDeviceCombo(micDeviceCombo_, ui::TextId::DefaultInputDevice, probe.captureEndpoints,
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

    void PopulateMonitorChoices(int selected) {
        ::SendMessageW(monitorCombo_, CB_RESETCONTENT, 0, 0);
        for (std::size_t i = 0; i < displays_.size(); ++i) {
            const MonitorInfo& monitor = displays_[i];
            const int width = monitor.virtualRect.right - monitor.virtualRect.left;
            const int height = monitor.virtualRect.bottom - monitor.virtualRect.top;
            const std::wstring text = ui::Format(ui::TextId::MonitorChoice,
                monitor.index, width, height, monitor.friendlyName);
            const int index = static_cast<int>(
                ::SendMessageW(monitorCombo_, CB_ADDSTRING, 0,
                               reinterpret_cast<LPARAM>(text.c_str())));
            ::SendMessageW(monitorCombo_, CB_SETITEMDATA, index, i);
        }
        ::SendMessageW(monitorCombo_, CB_SETCURSEL, selected, 0);
    }

    void RebuildMonitorList() {
        displays_ = EnumerateDisplays();
        int selected = 0;
        for (std::size_t i = 0; i < displays_.size(); ++i) {
            if (displays_[i].primary) selected = static_cast<int>(i);
        }
        PopulateMonitorChoices(selected);
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
        format_ = settings.format;
        mp4Limit_ = settings.limit;
        mp4CustomBound_ = settings.customBound;
        mp4Fps_ = settings.fps;
        gifLimit_ = settings.gifLimit;
        gifCustomBound_ = settings.gifCustomBound;
        gifFps_ = settings.gifFps;
        SetComboSelection(formatCombo_, format_ == RecordingFormat::Gif ? 1 : 0);
        ApplyFormatOptions();
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
        const int index = ComboSelection(fpsCombo_);
        const std::span<const int> options = FrameRateOptions();
        return index >= 0 && static_cast<std::size_t>(index) < options.size()
            ? options[index] : format_ == RecordingFormat::Gif ? kDefaultGifFrameRate : kDefaultMp4FrameRate;
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
                *error = ui::Text(ui::TextId::NoDisplayAvailable);
            }
            return std::nullopt;
        }
        if (RegionMode() && !regionAvailable_) {
            if (error != nullptr) {
                *error = ui::Text(ui::TextId::SelectCaptureAreaFirst);
            }
            return std::nullopt;
        }
        const RECT source = CurrentSourceRect();
        const int width = source.right - source.left;
        const int height = source.bottom - source.top;
        if (width < kMinOutputDimension || height < kMinOutputDimension) {
            if (error != nullptr) {
                *error = ui::Format(ui::TextId::CaptureAreaMinimum, kMinOutputDimension, kMinOutputDimension);
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
            SetStatus(ui::TextId::SelectValidCaptureArea, true);
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
        if (format_ == RecordingFormat::Gif) return ui::Text(ui::TextId::GifSilent);
        const bool audio = ::SendMessageW(audioCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        const bool microphone =
            ::SendMessageW(micCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        if (audio && microphone) return ui::Text(ui::TextId::SystemAndMicrophone);
        if (audio) return ui::Text(ui::TextId::SystemAudio);
        if (microphone) return ui::Text(ui::TextId::Microphone);
        return ui::Text(ui::TextId::AudioOffDescription);
    }

    void UpdateDerivedState(bool updateStatus) {
        const MonitorInfo* monitor = SelectedMonitor();
        const RECT source = CurrentSourceRect();
        const int sourceWidth = source.right - source.left;
        const int sourceHeight = source.bottom - source.top;

        if (monitor == nullptr) {
            ::SetWindowTextW(regionSummary_, ui::Text(ui::TextId::NoDisplayDetected).c_str());
        } else if (RegionMode()) {
            if (regionAvailable_) {
                const std::wstring text = ui::Format(ui::TextId::RegionSummary,
                    monitor->index, sourceWidth, sourceHeight, source.left, source.top);
                ::SetWindowTextW(regionSummary_, text.c_str());
            } else {
                ::SetWindowTextW(regionSummary_, ui::Text(ui::TextId::NoRegionSelected).c_str());
            }
        } else {
            const std::wstring text = ui::Format(ui::TextId::FullScreenSummary,
                monitor->index, sourceWidth, sourceHeight);
            ::SetWindowTextW(regionSummary_, text.c_str());
        }

        std::wstring sizeError;
        const auto output = ComputeCurrentOutput(&sizeError);
        const int bitrate = SelectedBitrate();
        std::wstring statusError;
        bool valid = true;
        std::wstring outputText;
        if (monitor == nullptr) {
            valid = false;
            statusError = ui::Text(ui::TextId::NoDisplayAvailable);
        } else if (RegionMode() && !regionAvailable_) {
            valid = false;
            statusError = ui::Text(ui::TextId::SelectCaptureAreaFirst);
        } else if (!output.has_value()) {
            valid = false;
            statusError = ui::Format(ui::TextId::CannotCreateOutputFrame, ui::LocalizeMessage(sizeError));
        }
        if (outputDirectory_.empty()) {
            valid = false;
            statusError = ui::Text(ui::TextId::ChooseExportFolder);
        }
        if (output.has_value() && format_ == RecordingFormat::Gif) {
            outputText = ui::Format(ui::TextId::GifOutputSummary,
                output->output.width, output->output.height, SelectedFps());
            ::SetWindowTextW(outputSummary_, outputText.c_str());
        } else if (output.has_value()) {
            const std::wstring quality = ui::Text(kQualityOptions[SelectedQualityIndex()].name);
            outputText = ui::Format(ui::TextId::OutputSummary,
                output->output.width, output->output.height, SelectedFps(), quality,
                kEstimateDurationSeconds,
                Utf8ToWide(FormatBytes(EstimateBytesWithAudioTracks(
                    MbpsToBitsPerSecond(static_cast<double>(bitrate)),
                    kAudioTrackCount, kEstimateDurationSeconds))));
            outputText += L"\r\n" + ui::Format(ui::TextId::OutputCodecSummary, kAudioTrackCount);
            ::SetWindowTextW(outputSummary_, outputText.c_str());
        } else {
            outputText = ui::Text(ui::TextId::SelectValidAreaAndQuality);
            if (!sizeError.empty()) {
                outputText = sizeError;
            }
            const std::wstring localized = ui::LocalizeMessage(outputText);
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
                std::wstring line = ui::Format(ui::TextId::ReadyDescription, AudioDescription());
                if (hotkeyStartStop_) {
                    line += ui::Text(ui::TextId::RecordingHotkeyHint);
                } else {
                    line += ui::Text(ui::TextId::RecordingButtonsHint);
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
        ::EnableWindow(formatCombo_, enableSettings);
        const bool audioSupported = format_ == RecordingFormat::Mp4;
        ::EnableWindow(qualityLabel_, ready && audioSupported ? TRUE : FALSE);
        ::EnableWindow(qualityCombo_, ready && audioSupported ? TRUE : FALSE);
        ::EnableWindow(audioCheck_, ready && audioSupported ? TRUE : FALSE);
        ::EnableWindow(micCheck_, ready && audioSupported ? TRUE : FALSE);
        ::SetWindowTextW(qualityHint_, ui::Text(audioSupported
            ? ui::TextId::QualityHint : ui::TextId::GifSilent).c_str());
        const bool systemAudioOn =
            ::SendMessageW(audioCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        const bool microphoneOn =
            ::SendMessageW(micCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        ::EnableWindow(systemDeviceCombo_,
                       ready && audioSupported && systemAudioOn ? TRUE : FALSE);
        ::EnableWindow(micDeviceCombo_, ready && audioSupported && microphoneOn ? TRUE : FALSE);
        ::EnableWindow(browseButton_, enableSettings);
        ::EnableWindow(startButton_, ready && configValid_ ? TRUE : FALSE);
        ::EnableWindow(openFolderButton_, !lastOutputFolder_.empty() ? TRUE : FALSE);
        ::EnableWindow(screenshotButton_,
                       (ready || phase_ == Phase::Recording) ? TRUE : FALSE);
        if (idle) {
            if (configValid_) {
                UpdateDerivedState(false);
            }
            ::SetWindowTextW(startButton_, ui::Text(ui::TextId::StartRecording).c_str());
        } else {
            ::EnableWindow(startButton_, FALSE);
            const std::wstring label = ui::Text(phase_ == Phase::Screenshot
                ? ui::TextId::CapturingScreenshot : ui::TextId::RecordingBusy);
            ::SetWindowTextW(startButton_, label.c_str());
        }
    }

    void SetStatus(ui::TextId id, bool error = false) {
        SetStatus(ui::Text(id), error);
    }

    void SetStatus(std::wstring_view text, bool error = false) {
        statusError_ = error;
        const std::wstring localized = ui::LocalizeMessage(text);
        ::SetWindowTextW(statusLabel_, localized.c_str());
        ::InvalidateRect(statusLabel_, nullptr, TRUE);
    }

    bool ValidateConfig(RecordingConfig& config, std::wstring& error) {
        const MonitorInfo* monitor = SelectedMonitor();
        if (monitor == nullptr) {
            error = ui::Text(ui::TextId::NoDisplayAvailable);
            return false;
        }
        if (RegionMode() && !regionAvailable_) {
            error = ui::Text(ui::TextId::SelectCaptureAreaFirst);
            return false;
        }
        const RECT source = CurrentSourceRect();
        const int width = source.right - source.left;
        const int height = source.bottom - source.top;
        if (width < kMinOutputDimension || height < kMinOutputDimension) {
            error = ui::Format(ui::TextId::CaptureAreaMinimum, kMinOutputDimension, kMinOutputDimension);
            return false;
        }
        if (outputDirectory_.empty()) {
            error = ui::Text(ui::TextId::ChooseExportFolder);
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
        config.format = format_;
        config.limit = SelectedLimit();
        config.customBound = customBound_;
        config.fps = SelectedFps();
        config.bitrateMbps = SelectedBitrate();
        config.systemAudio = format_ == RecordingFormat::Mp4 &&
            ::SendMessageW(audioCheck_, BM_GETCHECK, 0, 0) == BST_CHECKED;
        config.microphone = format_ == RecordingFormat::Mp4 &&
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
                MessageBoxUi(window_, floatingError.c_str(), ui::TextId::CannotStartRecordingTitle,
                              MB_ICONERROR | MB_OK);
                return;
            }
        }

        const MonitorInfo* monitor = SelectedMonitor();
        if (monitor == nullptr) {
            SetStatus(ui::TextId::NoDisplayAvailable, true);
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
                SetStatus(ui::TextId::FloatingExclusionFailed, true);
                MessageBoxUi(window_,
                              ui::TextId::FloatingExclusionAdvice,
                              ui::TextId::CannotStartRecordingTitle, MB_ICONERROR | MB_OK);
                return;
            }
        }

        config_ = config;
        screenshotFailure_.clear();
        systemAudioEnabled_ = config.systemAudio;
        microphoneEnabled_ = config.microphone;
        audioRevision_ = 0;
        pendingAudioSource_ = ui::TextId::Empty;
        phase_ = Phase::Countdown;
        countdownActive_ = true;
        countdownEnd_ = std::chrono::steady_clock::now() + kCountdownDuration;
        lastCountdownValue_ = -1;
        UpdateUiState();
        SetStatus(ui::TextId::RecordingAboutToStart);
        floating_->SetContent(ui::Format(ui::TextId::Countdown, kCountdownDuration.count()), true, ui::TextId::Cancel);
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
                        ui::Format(ui::TextId::Countdown, value), true, ui::TextId::Cancel);
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
            std::wstring text = ui::Text(ui::TextId::Saving);
            const auto savingSeconds = std::chrono::duration_cast<std::chrono::seconds>(
                                           std::chrono::steady_clock::now() -
                                           stopPressedAt_)
                                           .count();
            if (savingSeconds >= kSavingProgressDelay.count()) {
                text = ui::Format(ui::TextId::SavingElapsed, savingSeconds);
            }
            if (floating_ != nullptr) {
                floating_->SetContent(text, false, ui::TextId::Empty);
            }
            return;
        }
        switch (state) {
            case RecorderState::Starting:
                phase_ = Phase::Starting;
                floating_->SetContent(ui::TextId::Starting, true, ui::TextId::Cancel);
                break;
            case RecorderState::Recording: {
                phase_ = Phase::Recording;
                const auto audio = session_->audioState();
                systemAudioEnabled_ = audio.systemAudio;
                microphoneEnabled_ = audio.microphone;
                if (audio.revision != audioRevision_) {
                    audioRevision_ = audio.revision;
                    if (!audio.error.empty()) {
                        pendingAudioSource_ = ui::TextId::Empty;
                        ShowRecordingMessage(audio.error, true);
                    } else if (!audio.pending && pendingAudioSource_ != ui::TextId::Empty) {
                        ShowRecordingMessage(ui::Format(pendingAudioEnabled_
                            ? ui::TextId::SourceEnabled : ui::TextId::SourceDisabled,
                            ui::Text(pendingAudioSource_)));
                        pendingAudioSource_ = ui::TextId::Empty;
                    }
                }
                std::wstring text = ui::Format(ui::TextId::RecordingTime, FormatDuration(session_->elapsed100ns()));
                if (std::chrono::steady_clock::now() < transientMessageUntil_) {
                    text = transientMessage_;
                }
                floating_->SetContent(text, true, ui::TextId::Stop);
                floating_->SetRecordingControls(true, audio.pending,
                    systemAudioEnabled_, microphoneEnabled_, config_.format == RecordingFormat::Mp4);
                floating_->SetMicrophoneLevel(microphoneEnabled_
                    ? session_->microphoneLevel() : 0.0f);
                break;
            }
            case RecorderState::Stopping:
                phase_ = Phase::Saving;
                floating_->SetContent(ui::TextId::Stopping, false, ui::TextId::Empty);
                break;
            case RecorderState::Saving:
                phase_ = Phase::Saving;
                floating_->SetContent(ui::TextId::Saving, false, ui::TextId::Empty);
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
            SetStatus(ui::Format(ui::TextId::CannotStartRecording, ui::LocalizeMessage(error)), true);
            MessageBoxUi(window_, error.c_str(), ui::TextId::CannotStartRecordingTitle, MB_ICONERROR | MB_OK);
            UpdateUiState();
            return;
        }
        phase_ = Phase::Starting;
        floating_->SetContent(ui::TextId::Starting, true, ui::TextId::Cancel);
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
                floating_->SetContent(ui::TextId::Saving, false, ui::TextId::Empty);
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
        SetStatus(ui::TextId::RecordingCancelled);
        UpdateUiState();
    }

    void PersistSettings() {
        const MonitorInfo* monitor = SelectedMonitor();
        if (monitor == nullptr) {
            return;
        }
        AppSettings settings;
        RememberFormatOptions();
        settings.monitorDeviceName = monitor->deviceName;
        settings.hasRegion = regionAvailable_;
        settings.region = regionRect_;
        settings.format = format_;
        settings.limit = mp4Limit_;
        settings.customBound = mp4CustomBound_;
        settings.fps = mp4Fps_;
        settings.gifLimit = gifLimit_;
        settings.gifCustomBound = gifCustomBound_;
        settings.gifFps = gifFps_;
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
            std::wstring message = ui::LocalizeMessage(result.error);
            if (message.empty()) {
                message = ui::Text(finishedStateSuccess ? ui::TextId::RecordingResultUnavailable : ui::TextId::RecordingFailed);
            }
            if (!result.partialPath.empty()) {
                message += ui::Format(ui::TextId::IncompleteFile, result.partialPath);
            }
            for (const std::wstring& path : result.recoveryPaths) {
                if (path != result.partialPath) message += ui::Format(ui::TextId::AudioRecoveryFile, path);
            }
            if (!screenshotFailure_.empty()) {
                message += ui::Format(ui::TextId::ScreenshotFailureDetails,
                    ui::LocalizeMessage(screenshotFailure_));
            }
            SetStatus(message, true);
            MessageBoxUi(window_, message.c_str(), ui::TextId::RecordingFinished, MB_ICONERROR | MB_OK);
        }
        UpdateDerivedState(false);
        UpdateUiState();
        if (haveResult && result.success) {
            SetStatus(screenshotFailure_.empty()
                          ? ui::Format(ui::TextId::SavedFile, FileNameOf(result.finalPath))
                          : ui::Format(ui::TextId::SavedVideoScreenshotFailed,
                                ui::LocalizeMessage(screenshotFailure_)),
                      !screenshotFailure_.empty());
            if (!screenshotFailure_.empty()) {
                MessageBoxUi(window_, screenshotFailure_.c_str(), ui::TextId::ScreenshotSaveFailedTitle,
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
            SetStatus(ui::TextId::SelectedDisplayUnavailable, true);
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

    void ShowRecordingMessage(ui::TextId id, bool error = false) {
        ShowRecordingMessage(ui::Text(id), error);
    }

    void ShowRecordingMessage(const std::wstring& message, bool error = false,
                              ui::TextId brief = ui::TextId::Empty) {
        if (phase_ == Phase::Recording && floating_ != nullptr) {
            transientMessage_ = brief == ui::TextId::Empty ? ui::LocalizeMessage(message) : ui::Text(brief);
            transientMessageUntil_ = std::chrono::steady_clock::now() + kTransientMessageDuration;
            floating_->SetContent(transientMessage_, true, ui::TextId::Stop);
        }
        SetStatus(message, error);
    }

    void ToggleRecordingAudio(bool systemAudio) {
        if (phase_ != Phase::Recording || session_ == nullptr || stopPressed_ ||
            config_.format != RecordingFormat::Mp4) return;
        const bool systemEnabled = systemAudio ? !systemAudioEnabled_ : systemAudioEnabled_;
        const bool micEnabled = systemAudio ? microphoneEnabled_ : !microphoneEnabled_;
        if (!session_->SetAudioEnabled(systemEnabled, micEnabled)) {
            ShowRecordingMessage(ui::TextId::CannotChangeRecordingAudio, true);
            return;
        }
        pendingAudioSource_ = systemAudio ? ui::TextId::SystemAudio : ui::TextId::Microphone;
        pendingAudioEnabled_ = systemAudio ? systemEnabled : micEnabled;
        floating_->SetRecordingControls(true, true, systemAudioEnabled_, microphoneEnabled_);
        ShowRecordingMessage(ui::Format(pendingAudioEnabled_
            ? ui::TextId::SourceEnabling : ui::TextId::SourceDisabling,
            ui::Text(pendingAudioSource_)));
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
                    ShowRecordingMessage(ui::Format(ui::TextId::ScreenshotSavedFile, FileNameOf(event.result.path)),
                                         false, ui::TextId::ScreenshotSaved);
                } else {
                    ShowRecordingMessage(ui::Format(ui::TextId::ScreenshotFailure, ui::LocalizeMessage(screenshotFailure_)), true);
                }
            } else {
                closingPending_ = false;
                if (!screenshotFailure_.empty()) screenshotFailure_ += L"\r\n";
                screenshotFailure_ += ui::LocalizeMessage(event.result.error);
                ShowRecordingMessage(ui::Format(ui::TextId::ScreenshotFailure, ui::LocalizeMessage(screenshotFailure_)), true);
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
            ShowRecordingMessage(ui::TextId::ScreenshotUnavailable, true);
            return;
        }
        if (screenshotWorker_.pending() >= kMaxPendingScreenshots ||
            (phase_ == Phase::Idle && screenshotWorker_.pending() != 0)) {
            ShowRecordingMessage(ui::TextId::ScreenshotSavingWait, true);
            return;
        }
        std::wstring error;
        const bool firstRequest = screenshotWorker_.pending() == 0;
        if (phase_ == Phase::Recording) {
            ScreenshotFrame frame;
            if (session_ == nullptr || !session_->CopyScreenshot(frame, &error) ||
                !screenshotWorker_.SubmitSnapshot(std::move(frame), config_.outputDirectory, &error)) {
                ShowRecordingMessage(ui::Format(ui::TextId::ScreenshotFailure, ui::LocalizeMessage(error)), true);
                return;
            }
            if (firstRequest) screenshotFailure_.clear();
            ShowRecordingMessage(ui::TextId::SavingScreenshot);
            return;
        }
        const MonitorInfo* monitor = SelectedMonitor();
        if (monitor == nullptr || (RegionMode() && !regionAvailable_)) {
            SetStatus(ui::TextId::SelectValidScreenshotArea, true);
            return;
        }
        if (outputDirectory_.empty()) {
            SetStatus(ui::TextId::ChooseScreenshotFolder, true);
            return;
        }
        const RECT source = CurrentSourceRect();
        phase_ = Phase::Screenshot;
        UpdateUiState();
        ::ShowWindow(window_, SW_HIDE);
        if (!screenshotWorker_.SubmitCapture(monitor->handle, source, outputDirectory_, &error)) {
            phase_ = Phase::Idle;
            ShowMainWindow();
            SetStatus(ui::Format(ui::TextId::ScreenshotFailure, ui::LocalizeMessage(error)), true);
            UpdateUiState();
        } else {
            screenshotFailure_.clear();
        }
    }

    void OnCommand(int id, int notification) {
        switch (id) {
            case kFormatCombo:
                if (notification == CBN_SELCHANGE) ChangeFormat();
                break;
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
                        floating_->SetContent(ui::TextId::Saving, false, ui::TextId::Empty);
                    }
                } else if (screenshotWorker_.pending() != 0) {
                    closingPending_ = true;
                    SetStatus(ui::TextId::SavingScreenshotsBeforeClosing);
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
    HWND formatLabel_ = nullptr;
    HWND formatCombo_ = nullptr;
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
    RecordingFormat format_ = RecordingFormat::Mp4;
    ResolutionLimit mp4Limit_ = ResolutionLimit::P1080;
    Size mp4CustomBound_{1920, 1080};
    int mp4Fps_ = kDefaultMp4FrameRate;
    ResolutionLimit gifLimit_ = ResolutionLimit::P720;
    Size gifCustomBound_{1280, 720};
    int gifFps_ = kDefaultGifFrameRate;
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
    ui::TextId pendingAudioSource_ = ui::TextId::Empty;
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
