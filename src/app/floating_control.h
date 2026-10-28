#pragma once

#include <windows.h>
#include <commctrl.h>

#include <array>
#include <cstdint>
#include <functional>
#include <string>

#include "platform/displays.h"
#include "app/ui_text.h"

namespace srl {

class FloatingControl {
public:
    using Action = std::function<void()>;
    using AudioAction = std::function<void(bool systemAudio)>;
    using MicrophoneLevel = std::function<float()>;

    FloatingControl();
    ~FloatingControl();

    bool Create(HINSTANCE instance, Action action, Action closeAction,
                Action screenshotAction, AudioAction audioAction,
                MicrophoneLevel microphoneLevel,
                std::wstring* error);
    void Destroy();

    void SetContent(std::wstring_view status, bool showAction, ui::TextId action);
    void SetContent(ui::TextId status, bool showAction, ui::TextId action);
    void SetRecordingControls(bool recording, bool audioPending,
                              bool systemEnabled, bool micEnabled,
                              bool audioSupported = true);
    void SetMicrophoneLevel(float level);
    void SetPosition(const MonitorInfo& monitor, const RECT* avoidScreenRect);
    void Show();
    void Hide();
    [[nodiscard]] HWND hwnd() const noexcept { return window_; }
    [[nodiscard]] bool exclusionApplied() const noexcept { return exclusionApplied_; }
    [[nodiscard]] DWORD exclusionError() const noexcept { return exclusionError_; }

private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam,
                                       LPARAM lParam);
    void Layout();
    void OnPaint();
    void UpdateFonts();
    void RecreateFonts();
    void SetToolTip(HWND control, ui::TextId text);
    void SetRegionOutline(const RECT* screenRect);
    void SetRegionOutlineVisible(bool visible);
    void DestroyRegionOutline();
    static LRESULT CALLBACK ButtonProc(HWND window, UINT message, WPARAM wParam,
        LPARAM lParam, UINT_PTR subclassId, DWORD_PTR reference);

    HWND window_ = nullptr;
    HWND statusLabel_ = nullptr;
    HWND actionButton_ = nullptr;
    HWND screenshotButton_ = nullptr;
    HWND audioButton_ = nullptr;
    HWND microphoneButton_ = nullptr;
    HWND tooltips_ = nullptr;
    HWND hoveredButton_ = nullptr;
    std::array<HWND, 4> regionOutline_{};
    bool regionOutlineVisible_ = false;
    HFONT font_ = nullptr;
    HFONT statusFont_ = nullptr;
    Action action_;
    Action closeAction_;
    Action screenshotAction_;
    AudioAction audioAction_;
    bool exclusionApplied_ = false;
    DWORD exclusionError_ = ERROR_SUCCESS;
    UINT dpi_ = 96;
    bool actionVisible_ = false;
    std::wstring actionText_;
    ui::TextId actionId_ = ui::TextId::Empty;
    std::wstring statusText_;
    bool recording_ = false;
    bool audioPending_ = false;
    bool audioSupported_ = true;
    bool systemEnabled_ = false;
    bool micEnabled_ = false;
    bool meterTimerRunning_ = false;
    float microphoneLevel_ = 0.0f;
    float microphoneAnimationPhase_ = 0.0f;
    MicrophoneLevel microphoneLevelProvider_;
};

}  // namespace srl
