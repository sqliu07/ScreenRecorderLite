#include "app/settings.h"

#include <algorithm>
#include <knownfolders.h>
#include <filesystem>
#include <format>
#include <string>

#include "common/win32.h"

namespace srl {
namespace {

constexpr wchar_t kSection[] = L"last";
constexpr wchar_t kFileName[] = L"settings.ini";

std::wstring ReadString(const wchar_t* key, const std::wstring& fallback) {
    wchar_t buffer[32768]{};
    ::GetPrivateProfileStringW(kSection, key, fallback.c_str(), buffer,
                               ARRAYSIZE(buffer), SettingsFilePath().c_str());
    return buffer;
}

int ReadInt(const wchar_t* key, int fallback) {
    return static_cast<int>(::GetPrivateProfileIntW(kSection, key, fallback,
                                                    SettingsFilePath().c_str()));
}

bool WriteString(const wchar_t* key, const std::wstring& value) {
    return ::WritePrivateProfileStringW(kSection, key, value.c_str(),
                                        SettingsFilePath().c_str()) != FALSE;
}

bool WriteInt(const wchar_t* key, int value) {
    const std::wstring text = std::to_wstring(value);
    return WriteString(key, text);
}

}  // namespace

std::wstring SettingsFilePath() {
    try {
        std::filesystem::path directory = KnownFolderPath(FOLDERID_LocalAppData);
        directory /= L"ScreenRecorderLite";
        return (directory / kFileName).wstring();
    } catch (...) {
        return (std::filesystem::path(GetTempDirectory()) / kFileName).wstring();
    }
}

bool LoadAppSettings(AppSettings& settings) {
    const std::wstring path = SettingsFilePath();
    std::error_code existsError;
    if (!std::filesystem::exists(path, existsError)) {
        return false;
    }
    const std::wstring monitor = ReadString(L"monitor", L"");
    const int limit = std::clamp(ReadInt(L"limit", 2), 0, 5);
    const int fps = ReadInt(L"fps", 30);
    const int bitrate = ReadInt(L"bitrate", 8);
    const int customWidth = ReadInt(L"custom_width", 1920);
    const int customHeight = ReadInt(L"custom_height", 1080);
    const int regionLeft = ReadInt(L"region_left", 0);
    const int regionTop = ReadInt(L"region_top", 0);
    const int regionRight = ReadInt(L"region_right", 0);
    const int regionBottom = ReadInt(L"region_bottom", 0);

    settings.monitorDeviceName = monitor;
    settings.format = ReadInt(L"format", 0) == 1 ? RecordingFormat::Gif : RecordingFormat::Mp4;
    settings.limit = static_cast<ResolutionLimit>(limit);
    settings.customBound = Size{
        std::clamp(customWidth, kMinOutputDimension, kMaxOutputDimension),
        std::clamp(customHeight, kMinOutputDimension, kMaxOutputDimension)};
    settings.fps = IsFrameRateSupported(RecordingFormat::Mp4, fps) ? fps : kDefaultMp4FrameRate;
    const int gifFps = ReadInt(L"gif_fps", kDefaultGifFrameRate);
    settings.gifFps = IsFrameRateSupported(RecordingFormat::Gif, gifFps) ? gifFps : kDefaultGifFrameRate;
    settings.gifLimit = static_cast<ResolutionLimit>(std::clamp(
        ReadInt(L"gif_limit", static_cast<int>(ResolutionLimit::P720)), 0, 5));
    settings.gifCustomBound = Size{
        std::clamp(ReadInt(L"gif_custom_width", 1280), kMinOutputDimension, kMaxOutputDimension),
        std::clamp(ReadInt(L"gif_custom_height", 720), kMinOutputDimension, kMaxOutputDimension)};
    settings.bitrateMbps = std::clamp(bitrate, 1, 50);
    settings.systemAudio = ReadInt(L"system_audio", 1) != 0;
    settings.microphone = ReadInt(L"microphone", 0) != 0;
    settings.systemAudioDeviceId = ReadString(L"system_audio_device", L"");
    settings.microphoneDeviceId = ReadString(L"microphone_device", L"");
    settings.outputDirectory = ReadString(L"output_directory", L"");
    settings.uiLanguage = std::clamp(ReadInt(L"ui_language", 0), 0, 2);
    settings.hasRegion = regionRight - regionLeft >= kMinOutputDimension &&
                         regionBottom - regionTop >= kMinOutputDimension;
    if (settings.hasRegion) {
        settings.region = RECT{regionLeft, regionTop, regionRight, regionBottom};
    }
    settings.loaded = true;
    return true;
}

bool SaveAppSettings(const AppSettings& settings) {
    const std::wstring path = SettingsFilePath();
    const std::filesystem::path directory = std::filesystem::path(path).parent_path();
    std::string directoryError;
    if (!EnsureDirectory(directory, &directoryError)) {
        return false;
    }
    bool ok = true;
    ok = WriteString(L"monitor", settings.monitorDeviceName) && ok;
    ok = WriteInt(L"format", static_cast<int>(settings.format)) && ok;
    ok = WriteInt(L"limit", static_cast<int>(settings.limit)) && ok;
    ok = WriteInt(L"fps", settings.fps) && ok;
    ok = WriteInt(L"gif_limit", static_cast<int>(settings.gifLimit)) && ok;
    ok = WriteInt(L"gif_custom_width", settings.gifCustomBound.width) && ok;
    ok = WriteInt(L"gif_custom_height", settings.gifCustomBound.height) && ok;
    ok = WriteInt(L"gif_fps", settings.gifFps) && ok;
    ok = WriteInt(L"bitrate", settings.bitrateMbps) && ok;
    ok = WriteInt(L"custom_width", settings.customBound.width) && ok;
    ok = WriteInt(L"custom_height", settings.customBound.height) && ok;
    ok = WriteInt(L"region_left", settings.region.left) && ok;
    ok = WriteInt(L"region_top", settings.region.top) && ok;
    ok = WriteInt(L"region_right", settings.region.right) && ok;
    ok = WriteInt(L"region_bottom", settings.region.bottom) && ok;
    ok = WriteInt(L"system_audio", settings.systemAudio ? 1 : 0) && ok;
    ok = WriteInt(L"microphone", settings.microphone ? 1 : 0) && ok;
    ok = WriteString(L"system_audio_device", settings.systemAudioDeviceId) && ok;
    ok = WriteString(L"microphone_device", settings.microphoneDeviceId) && ok;
    ok = WriteString(L"output_directory", settings.outputDirectory) && ok;
    ok = WriteInt(L"ui_language", settings.uiLanguage) && ok;
    return ok;
}

}  // namespace srl
