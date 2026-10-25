#pragma once

#include <windows.h>

#include <string>

#include "core/output_size.h"

namespace srl {

struct AppSettings {
    bool loaded = false;
    std::wstring monitorDeviceName;
    bool hasRegion = false;
    RECT region{};
    ResolutionLimit limit = ResolutionLimit::P1080;
    Size customBound{1920, 1080};
    int fps = 30;
    int bitrateMbps = 8;
    bool systemAudio = true;
    bool microphone = false;
    std::wstring systemAudioDeviceId;
    std::wstring microphoneDeviceId;
    std::wstring outputDirectory;
    int uiLanguage = 0;  // 0 follows Windows, 1 Simplified Chinese, 2 English
};

[[nodiscard]] std::wstring SettingsFilePath();
[[nodiscard]] bool LoadAppSettings(AppSettings& settings);
[[nodiscard]] bool SaveAppSettings(const AppSettings& settings);

}  // namespace srl
