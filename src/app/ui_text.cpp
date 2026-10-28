#include "app/ui_text.h"

#include <windows.h>

#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "core/output_size.h"

namespace srl::ui {
namespace {

constexpr int kAutomaticLanguage = 0;
constexpr int kChineseLanguage = 1;
constexpr int kEnglishLanguage = 2;
bool g_english = false;
int g_languagePreference = kAutomaticLanguage;

struct Translation {
    std::wstring_view source;
    std::wstring_view target;
};

constexpr Translation kResources[] = {
#define SRL_UI_TEXT(id, chinese, english) {chinese, english},
#include "app/ui_text_resources.inc"
#undef SRL_UI_TEXT
};
static_assert(std::size(kResources) == static_cast<std::size_t>(TextId::Count));

template <std::size_t N>
constexpr bool UniqueSources(const Translation (&entries)[N]) {
    for (std::size_t i = 0; i < N; ++i) {
        for (std::size_t j = i + 1; j < N; ++j) {
            if (entries[i].source == entries[j].source) return false;
        }
    }
    return true;
}
// Compatibility with diagnostic messages returned by the recording and OS layers.
// These are deliberately one-way; translated UI text is never used as an input key.
constexpr Translation kDiagnosticMessages[] = {
    {L"截图失败：", L"Screenshot failed: "},
    {L"截图失败，请稍后重试", L"Screenshot failed. Try again."},
    {L"未选择可用显示器", L"No display is selected."},
    {L"所选显示器已不可用，请重新选择", L"The selected display is unavailable. Select another display."},
    {L"截图区域无效或超出目标显示器，请重新选择", L"The screenshot area is invalid or outside the selected display."},
    {L"截图捕获失败：", L"Screenshot capture failed: "},
    {L"等待截图首帧失败：", L"Timed out waiting for the first screenshot frame: "},
    {L"截图画面复制失败：", L"Could not copy the screenshot frame: "},
    {L"截图画面数据无效", L"The screenshot frame is invalid."},
    {L"无法创建截图目录或生成文件名，请检查保存位置", L"Could not create the screenshot folder or a unique filename. Check the save location."},
    {L"截图保存已结束", L"Screenshot saving has ended."},
    {L"无法启动截图保存任务，请稍后重试", L"Could not start the screenshot task. Try again."},
    {L"截图组件初始化失败：", L"Could not initialize screenshot support: "},
    {L"截图组件初始化失败，请稍后重试", L"Could not initialize screenshot support. Try again."},
    {L"此声音设备仍在准备，请稍后重试", L"The audio device is still starting. Try again."},
    {L"声音切换超过了当前音频时间轴，请重试", L"Audio switching missed the current timeline. Try again."},
    {L"无法开启音频设备：", L"Could not start the audio device: "},
    {L"无法开启音频设备，请稍后重试", L"Could not start the audio device. Try again."},
    {L"录制正在停止，未更改声音来源", L"Recording is stopping; audio sources were unchanged."},
    {L"录制已结束，未更改声音来源", L"Recording ended; audio sources were unchanged."},
    {L"输出文件已存在，请选择其他文件名", L"The output file already exists. Choose another filename."},
    {L"音视频组件不完整，请重新解压完整程序包后再试", L"The media components are incomplete. Extract the complete application package and try again."},
    {L"无法创建视频收尾临时目录", L"Could not create the temporary video-finalization folder."},
    {L"创建分轨音频格式失败", L"Could not create the audio-track format."},
    {L"写入独立音频轨失败", L"Could not write an isolated audio track."},
    {L"无法完成音频收尾", L"Could not finish writing audio."},
    {L"录制目标显示器尺寸已变化，请重新选择显示器或区域", L"The display size changed. Select the display or capture area again."},
    {L"当前捕获画面尺寸无效", L"The current capture frame size is invalid."},
    {L"录制素材大小超出支持范围", L"The recorded media exceeds the supported file size."},
    {L"无法查询保存目录的可用空间", L"Could not check free space in the save folder."},
    {L"磁盘空间不足，无法完成多音轨 MP4 封装", L"There is not enough disk space to finish saving the multi-track MP4."},
    {L"多音轨 MP4 封装已取消或超时", L"Saving the multi-track MP4 was cancelled or timed out."},
    {L"录制素材的媒体格式无效", L"The recorded media has an invalid format."},
    {L"录制素材包含无效时间戳或损坏的数据", L"The recorded media contains invalid timestamps or damaged data."},
    {L"无法生成不重复的输出文件名", L"Could not create a unique output filename."},
    {L"帧率只支持 30 或 60", L"The frame rate must be 30 or 60 fps."},
    {L"视频码率需在 1～50 Mbps 之间", L"The video bitrate must be between 1 and 50 Mbps."},
    {L"请选择导出目录", L"Choose an export folder."},
    {L"找不到所选显示器，可能已断开", L"The selected display could not be found. It may have been disconnected."},
    {L"录制区域超出目标显示器", L"The capture area is outside the selected display."},
    {L"创建视频媒体类型失败", L"Could not create the video media type."},
    {L"创建音频媒体类型失败", L"Could not create the audio media type."},
    {L"无法开启系统声音", L"Could not start system audio."},
    {L"无法开启麦克风", L"Could not start the microphone."},
    {L"声音切换已取消", L"Changing the audio source was cancelled."},
    {L"写入 AAC 音频失败", L"Could not write AAC audio."},
    {L"捕获画面尺寸无效", L"The capture frame size is invalid."},
    {L"系统声音缓存溢出，已停止录制", L"The system audio buffer overflowed; recording was stopped."},
    {L"系统声音采集意外结束", L"System audio capture ended unexpectedly."},
    {L"麦克风缓存溢出，已停止录制", L"The microphone buffer overflowed; recording was stopped."},
    {L"麦克风采集意外结束", L"Microphone capture ended unexpectedly."},
    {L"写入 H.264 视频帧失败", L"Could not write the H.264 video frame."},
    {L"没有录制到有效画面", L"No valid video frames were recorded."},
    {L"录制已取消", L"Recording was cancelled."},
    {L"录制会话已在运行", L"A recording session is already running."},
    {L"仅在录制中可以截取当前画面", L"The current frame can only be captured while recording."},
    {L"当前没有有效的截图画面", L"No valid frame is available for a screenshot."},
    {L"截图区域无效或超出当前画面", L"The screenshot area is invalid or outside the current frame."},
    {L"屏幕捕获尚未启动", L"Screen capture has not started."},
    {L"请选择截图保存位置", L"Choose a screenshot folder."},
    {L"截图正在保存，请稍后", L"Screenshots are being saved. Please wait."},
    {L"截图数据无效", L"The screenshot data is invalid."},
    {L"GIF 动画不包含声音", L"GIF has no audio."},
    {L"GIF 帧率只支持 10、15、20 或 25", L"GIF frame rate must be 10, 15, 20 or 25 fps."},
    {L"捕获画面尺寸已变化，请重新开始录制", L"The capture size changed. Start a new recording."},
    {L"GIF 录制参数无效", L"The GIF recording settings are invalid."},
    {L"GIF 输出流必须为空", L"The GIF output stream must be empty."},
    {L"GIF 画面数据或时间戳无效", L"The GIF frame data or timestamp is invalid."},
    {L"GIF 没有有效画面或结束时间无效", L"The GIF has no valid frames or its end time is invalid."},
};
constexpr Translation kChineseDiagnosticPrefixes[] = {
    {L"无法保存最终录制文件：", L"Could not save the final recording: "},
    {L"GIF 初始化失败：", L"Could not initialize GIF recording: "},
    {L"写入 GIF 画面失败：", L"Could not write the GIF frame: "},
    {L"无法写入导出目录：", L"Could not write to the export folder: "},
    {L"无法读取导出目录的可用空间：", L"Could not read free space in the export folder: "},
    {L"导出目录可用空间不足：", L"There is not enough free space in the export folder: "},
    {L"无法创建截图目录或生成文件名", L"Could not create the screenshot folder or a unique filename"},
    {L"截图组件初始化失败：", L"Could not initialize screenshot support: "},
    {L"无法初始化音频设备线程：", L"Could not initialize the audio device thread: "},
    {L"无法启动音频设备线程：", L"Could not start the audio device thread: "},
    {L"无法开启系统声音：", L"Could not start system audio: "},
    {L"无法开启麦克风：", L"Could not start the microphone: "},
    {L"系统声音采集启动失败：", L"Could not start system audio capture: "},
    {L"麦克风采集启动失败：", L"Could not start microphone capture: "},
    {L"截图捕获失败：", L"Screenshot capture failed: "},
    {L"等待截图首帧失败：", L"Timed out waiting for the first screenshot frame: "},
    {L"截图画面复制失败：", L"Could not copy the screenshot frame: "},
    {L"截图失败：", L"Screenshot failed: "},
    {L"无法计算输出画面：", L"Cannot calculate the output frame: "},
    {L"无法生成输出画面：", L"Cannot create the output frame: "},
    {L"无法开始录制：", L"Cannot start recording: "},
    {L"MP4 收尾失败：", L"Could not finalize the MP4: "},
    {L"无法把临时文件改名为正式文件：", L"Could not rename the temporary file: "},
    {L"H.264 视频流初始化失败：", L"Could not initialize the H.264 video stream: "},
    {L"视频输入格式（NV12）不被编码器接受：", L"The encoder rejected the NV12 video format: "},
    {L"AAC 音频流初始化失败：", L"Could not initialize the AAC audio stream: "},
    {L"音频输入格式不被 AAC 编码器接受：", L"The AAC encoder rejected the audio format: "},
    {L"无法检查输出文件名：", L"Could not check the output filename: "},
    {L"无法创建 MP4 临时文件：", L"Could not create the MP4 temporary file: "},
    {L"创建 SinkWriter 属性失败：", L"Could not configure the media writer: "},
    {L"创建 MP4 写入器失败：", L"Could not create the MP4 writer: "},
    {L"SinkWriter 开始写入失败：", L"The media writer could not start: "},
    {L"无法启动截图保存任务：", L"Could not start the screenshot task: "},
    {L"无法计算输出尺寸：", L"Could not calculate the output size: "},
    {L"无法打开录制素材：", L"Could not open the recorded media: "},
    {L"无法读取录制素材：", L"Could not read the recorded media: "},
    {L"缺少所需媒体流：", L"The recorded media is missing a required stream: "},
    {L"无法创建视频封装器：", L"Could not create the video muxer: "},
    {L"无法写入视频文件头：", L"Could not write the video header: "},
    {L"无法写入视频数据：", L"Could not write the video data: "},
    {L"视频收尾失败：", L"Could not finalize the video: "},
    {L"多音轨 MP4 封装失败：", L"Could not create the multi-track MP4: "},
    {L"无法保存最终视频：", L"Could not save the final video: "},
    {L"无法创建分轨临时文件：", L"Could not create the temporary audio track: "},
    {L"创建分轨编码属性失败：", L"Could not configure the audio-track encoder: "},
    {L"创建分轨编码器失败：", L"Could not create the audio-track encoder: "},
    {L"初始化 AAC 分轨失败：", L"Could not initialize the AAC audio track: "},
    {L"设置 AAC 分轨格式失败：", L"Could not configure the AAC audio format: "},
    {L"启动 AAC 分轨失败：", L"Could not start the AAC audio track: "},
    {L"初始化独立音轨失败：", L"Could not initialize the isolated audio tracks: "},
    {L"独立 AAC 音轨收尾失败：", L"Could not finalize the isolated AAC track: "},
    {L"无法检查录制素材：", L"Could not inspect the recorded media: "},
    {L"无法计算录制素材大小：", L"Could not determine the recorded media size: "},
    {L"导出目录写入测试失败：", L"The export folder write check failed: "},
    {L"COM/WinRT 初始化失败：", L"Could not initialize Windows recording components: "},
    {L"Media Foundation 启动失败：", L"Could not start Media Foundation: "},
    {L"屏幕捕获启动失败：", L"Could not start screen capture: "},
    {L"等待首帧失败：", L"Could not receive the first capture frame: "},
    {L"画面裁剪/缩放配置失败：", L"Could not configure frame cropping or scaling: "},
    {L"屏幕捕获中断：", L"Screen capture was interrupted: "},
    {L"WinRT 异常：", L"Windows recording component error: "},
    {L"无法创建 WIC 图像工厂：", L"Could not create the image factory: "},
    {L"无法创建 PNG 临时文件：", L"Could not create the temporary PNG file: "},
    {L"无法初始化 PNG 编码器：", L"Could not initialize the PNG encoder: "},
    {L"PNG 编码失败：", L"PNG encoding failed: "},
    {L"无法保存截图文件：", L"Could not save the screenshot: "},
    {L"GIF 帧编码失败：", L"Could not encode the GIF frame: "},
    {L"GIF 画面转换失败：", L"Could not convert the GIF frame: "},
    {L"无法初始化 GIF 编码器：", L"Could not initialize the GIF encoder: "},
    {L"GIF 收尾失败：", L"Could not finalize the GIF: "},
};
constexpr Translation kEnglishDiagnosticPrefixes[] = {
    {L"display color mode changed; start a new recording", L"显示器色彩模式已变化，请重新开始录制。"},
    {L"captured texture format changed unexpectedly", L"捕获画面的像素格式已变化，请重新开始录制。"},
    {L"selected render endpoint was not found: ", L"找不到所选播放设备："},
    {L"selected capture endpoint was not found: ", L"找不到所选麦克风："},
    {L"no default render endpoint: ", L"没有可用的默认播放设备："},
    {L"no default capture endpoint: ", L"没有可用的默认麦克风："},
    {L"IAudioClient activation failed: ", L"音频设备无法启动："},
    {L"IAudioClient::Initialize(", L"音频设备格式初始化失败："},
    {L"GetMixFormat failed: ", L"无法读取音频设备格式："},
    {L"GetNextPacketSize failed: ", L"读取音频缓冲区失败："},
    {L"WaitForMultipleObjects failed: ", L"等待音频设备失败："},
    {L"audio capture timestamp is invalid; recording stopped to preserve synchronization", L"音频时间戳异常，已停止录制以保持同步。"},
};
static_assert(UniqueSources(kDiagnosticMessages));
static_assert(UniqueSources(kChineseDiagnosticPrefixes));
static_assert(UniqueSources(kEnglishDiagnosticPrefixes));

template <std::size_t N>
const Translation* FindPrefix(std::wstring_view source,
                              const Translation (&entries)[N]) {
    const Translation* best = nullptr;
    for (const Translation& entry : entries) {
        if (source.starts_with(entry.source) &&
            (best == nullptr || entry.source.size() > best->source.size())) {
            best = &entry;
        }
    }
    return best;
}

std::wstring TranslateChineseDiagnostic(std::wstring_view source) {
    constexpr std::wstring_view kEncoderAttempts = L"H.264/AAC 编码初始化失败。硬件尝试：";
    constexpr std::wstring_view kSoftwareAttempt = L"；软件尝试：";
    if (source.starts_with(kEncoderAttempts)) {
        const std::wstring_view attempts = source.substr(kEncoderAttempts.size());
        const std::size_t separator = attempts.find(kSoftwareAttempt);
        if (separator != std::wstring_view::npos) {
            return L"Could not initialize H.264/AAC encoding.\r\nHardware attempt: " +
                TranslateChineseDiagnostic(attempts.substr(0, separator)) +
                L"\r\nSoftware attempt: " +
                TranslateChineseDiagnostic(attempts.substr(separator + kSoftwareAttempt.size()));
        }
    }

    constexpr std::wstring_view kInsufficientSpace = L"导出目录可用空间不足：";
    constexpr std::wstring_view kStorageQueryFailed = L"无法读取导出目录的可用空间：";
    constexpr std::wstring_view kStoppedForStorage = L" 已停止录制并尝试保存已有内容。";
    if ((source.starts_with(kInsufficientSpace) || source.starts_with(kStorageQueryFailed)) &&
        source.ends_with(kStoppedForStorage)) {
        return TranslateChineseDiagnostic(source.substr(0, source.size() - kStoppedForStorage.size())) +
            L" Recording stopped; an attempt was made to save the recorded content.";
    }
    constexpr std::wstring_view kSpaceRequired = L"需要至少 ";
    constexpr std::wstring_view kSpaceAvailable = L"，当前剩余 ";
    if (source.starts_with(kInsufficientSpace)) {
        const std::wstring_view detail = source.substr(kInsufficientSpace.size());
        const std::size_t separator = detail.find(kSpaceAvailable);
        if (detail.starts_with(kSpaceRequired) && detail.ends_with(L"。") &&
            separator != std::wstring_view::npos && separator > kSpaceRequired.size()) {
            return L"There is not enough free space in the export folder: At least " +
                std::wstring(detail.substr(kSpaceRequired.size(), separator - kSpaceRequired.size())) +
                L" is required; " +
                std::wstring(detail.substr(separator + kSpaceAvailable.size(),
                    detail.size() - separator - kSpaceAvailable.size() - 1)) + L" is available.";
        }
    }
    static const std::wstring minimumCaptureDiagnostic = std::format(
        L"录制区域至少需要 {}×{} 物理像素", kMinOutputDimension, kMinOutputDimension);
    if (source == minimumCaptureDiagnostic) {
        return std::format(L"The capture area must be at least {} × {} physical pixels.",
                           kMinOutputDimension, kMinOutputDimension);
    }
    for (const Translation& message : kDiagnosticMessages) {
        if (message.source == source) return std::wstring(message.target);
    }
    const Translation* prefix = FindPrefix(source, kChineseDiagnosticPrefixes);
    if (prefix == nullptr) return std::wstring(source);
    return std::wstring(prefix->target) + TranslateChineseDiagnostic(
        source.substr(prefix->source.size()));
}

std::optional<std::wstring_view> TranslateOutputSizeDiagnostic(std::wstring_view source) {
    // Match only the core's explicit diagnostics. Device names and arbitrary English
    // remain untouched, and numeric limits follow the same constants as validation.
    static const std::pair<std::wstring, std::wstring> messages[] = {
        {L"source dimensions must be positive", L"录制画面的宽高必须大于零。"},
        {std::format(L"source region must be at least {}x{}", kMinOutputDimension, kMinOutputDimension),
         std::format(L"录制区域至少需要 {}×{} 物理像素。", kMinOutputDimension, kMinOutputDimension)},
        {std::format(L"custom bound must be within {}..{} on each axis", kMinOutputDimension, kMaxOutputDimension),
         std::format(L"自定义分辨率的宽高需在 {}～{} 之间。", kMinOutputDimension, kMaxOutputDimension)},
        {L"custom bound must use even dimensions", L"自定义分辨率的宽高必须为偶数。"},
        {L"unknown resolution preset", L"无法识别所选分辨率。"},
        {std::format(L"original size exceeds the {}-pixel axis limit; choose a smaller cap", kMaxOutputDimension),
         std::format(L"原始画面的单边超过 {} 像素，请选择较小的分辨率。", kMaxOutputDimension)},
        {std::format(L"original size exceeds {} pixels; choose a smaller cap", kMaxOutputPixels),
         std::format(L"原始画面超过 {} 像素，请选择较小的分辨率。", kMaxOutputPixels)},
        {std::format(L"output is smaller than {}x{}; choose a larger cap", kMinOutputDimension, kMinOutputDimension),
         std::format(L"输出画面小于 {}×{}，请选择较大的分辨率。", kMinOutputDimension, kMinOutputDimension)},
        {std::format(L"output exceeds the {}-pixel axis limit", kMaxOutputDimension),
         std::format(L"输出画面的单边超过 {} 像素，请选择较小的分辨率。", kMaxOutputDimension)},
        {std::format(L"output exceeds {} pixels", kMaxOutputPixels),
         std::format(L"输出画面超过 {} 像素，请选择较小的分辨率。", kMaxOutputPixels)},
    };
    for (const auto& [technical, localized] : messages) {
        if (technical == source) return localized;
    }
    return std::nullopt;
}

}  // namespace

void InitializeLanguage(int preference) {
    SetLanguagePreference(preference);
}

void SetLanguagePreference(int preference) {
    g_languagePreference = preference >= kAutomaticLanguage && preference <= kEnglishLanguage
        ? preference : kAutomaticLanguage;
    g_english = g_languagePreference == kEnglishLanguage ||
        (g_languagePreference == kAutomaticLanguage &&
         PRIMARYLANGID(::GetUserDefaultUILanguage()) != LANG_CHINESE);
}

int LanguagePreference() noexcept { return g_languagePreference; }
bool IsEnglishLanguage() noexcept { return g_english; }

std::wstring_view View(TextId id) noexcept {
    const std::size_t index = static_cast<std::size_t>(id);
    if (index >= std::size(kResources)) return {};
    return g_english ? kResources[index].target : kResources[index].source;
}

std::wstring Text(TextId id) { return std::wstring(View(id)); }

std::wstring LocalizeMessage(std::wstring_view source) {
    if (source.empty()) return {};
    if (g_english) {
        return TranslateChineseDiagnostic(source);
    }
    if (const auto localized = TranslateOutputSizeDiagnostic(source)) {
        return std::wstring(*localized);
    }
    if (const Translation* prefix = FindPrefix(source, kEnglishDiagnosticPrefixes)) {
        return std::wstring(prefix->target) + std::wstring(source.substr(prefix->source.size()));
    }
    if (const Translation* prefix = FindPrefix(source, kChineseDiagnosticPrefixes)) {
        return std::wstring(prefix->source) + LocalizeMessage(source.substr(prefix->source.size()));
    }
    return std::wstring(source);
}

}  // namespace srl::ui
