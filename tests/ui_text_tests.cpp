#include "app/ui_text.h"
#include "core/output_size.h"

#include <windows.h>

#include <cstdio>
#include <format>
#include <string>
#include <string_view>

namespace {
int failures = 0;
int checks = 0;

void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", message);
    }
}

std::size_t PlaceholderCount(std::wstring_view pattern) {
    std::size_t count = 0;
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        if (pattern[i] != L'{') continue;
        if (i + 1 < pattern.size() && pattern[i + 1] == L'{') {
            ++i;
        } else {
            ++count;
        }
    }
    return count;
}

bool IsValidFormat(std::wstring_view pattern) {
    int value = 1;
    try {
        (void)std::vformat(pattern, std::make_wformat_args(
            value, value, value, value, value, value, value, value));
        return true;
    } catch (const std::format_error&) {
        return false;
    }
}

void CheckOutputSizeFailure(const srl::OutputSizeRequest& request,
                            std::wstring_view chinese, const char* message) {
    std::wstring diagnostic;
    Check(!srl::ComputeOutputSize(request, &diagnostic).has_value(), message);
    Check(!diagnostic.empty(), "output-size rejection includes a diagnostic");
    srl::ui::SetLanguagePreference(1);
    Check(srl::ui::LocalizeMessage(diagnostic) == chinese,
          "output-size boundary failure is localized for Chinese UI");
    Check(srl::ui::LocalizeMessage(L"无法计算输出尺寸：" + diagnostic) ==
          L"无法计算输出尺寸：" + std::wstring(chinese),
          "wrapped core failure retains its Chinese context and translates its detail");
    srl::ui::SetLanguagePreference(2);
    Check(srl::ui::LocalizeMessage(diagnostic) == diagnostic,
          "English UI retains the core technical diagnostic");
}
}

int main() {
    using srl::ui::Format;
    using srl::ui::LocalizeMessage;
    using srl::ui::Text;
    using srl::ui::TextId;

    srl::ui::InitializeLanguage(1);
    Check(!srl::ui::IsEnglishLanguage(), "Chinese preference is applied");
    Check(srl::ui::LanguagePreference() == 1, "Chinese preference is retained");
    Check(Text(TextId::AppName) == L"轻录", "Chinese app title is preserved");
    Check(LocalizeMessage(L"Ready") == L"Ready", "English UI text is not reverse-guessed");
    Check(Format(TextId::OutputSizePreview, 640, 360) == L"实际输出：640×360",
          "Chinese output preview retains dimensions");
    Check(LocalizeMessage(L"selected capture endpoint was not found: USB microphone") ==
          L"找不到所选麦克风：USB microphone", "technical device failure is localized at the presentation boundary");

    Check(LocalizeMessage(L"屏幕捕获启动失败：display color mode changed; start a new recording") ==
        L"屏幕捕获启动失败：显示器色彩模式已变化，请重新开始录制。", "HDR mode changes are localized in nested errors");
    srl::ui::SetLanguagePreference(2);
    Check(LocalizeMessage(L"写入 GIF 画面失败：GIF 画面数据或时间戳无效") ==
        L"Could not write the GIF frame: The GIF frame data or timestamp is invalid.", "GIF errors translate at the presentation boundary");
    Check(LocalizeMessage(L"无法保存最终录制文件：0x80070005") ==
        L"Could not save the final recording: 0x80070005", "final GIF save errors retain OS diagnostics");
    Check(srl::ui::IsEnglishLanguage(), "English preference is applied immediately");
    Check(Text(TextId::AppName) == L"ScreenRecorder Lite", "English app title is available");
    Check(Text(TextId::LanguageLabel) == L"Language", "language control has an explicit label");
    Check(Format(TextId::OutputSizePreview, 640, 360) == L"Output size: 640×360",
          "custom output preview is translated with its dimensions");
    Check(Format(TextId::MonitorChoice, 2, 1920, 1080, L"显示器 USB") ==
          L"Display 2 · 1920×1080 · 显示器 USB", "display label translates while device names remain data");
    Check(LocalizeMessage(L"麦克风 (USB device)") == L"麦克风 (USB device)",
          "device names are not treated as translation keys");
    Check(LocalizeMessage(L"麦克风") == L"麦克风", "a device named like a UI label is preserved");
    Check(LocalizeMessage(L"Unexpected diagnostic: {device}") == L"Unexpected diagnostic: {device}",
          "unknown diagnostics are preserved without formatting");
    Check(LocalizeMessage(L"多音轨 MP4 封装失败：无法写入视频数据：Disk full") ==
          L"Could not create the multi-track MP4: Could not write the video data: Disk full",
          "nested media failures retain their detail and translate both prefixes");
    Check(LocalizeMessage(L"输出文件已存在，请选择其他文件名") ==
          L"The output file already exists. Choose another filename.", "known diagnostic is translated");
    Check(LocalizeMessage(std::format(L"录制区域至少需要 {}×{} 物理像素",
            srl::kMinOutputDimension, srl::kMinOutputDimension)) ==
          std::format(L"The capture area must be at least {} × {} physical pixels.",
            srl::kMinOutputDimension, srl::kMinOutputDimension),
          "capture minimum diagnostic uses the shared dimension limit");
    Check(LocalizeMessage(L"磁盘空间不足，无法完成多音轨 MP4 封装") ==
          L"There is not enough disk space to finish saving the multi-track MP4.",
          "media free-space failure is localized");
    Check(LocalizeMessage(L"多音轨 MP4 封装失败：磁盘空间不足，无法完成多音轨 MP4 封装") ==
          L"Could not create the multi-track MP4: There is not enough disk space to finish saving the multi-track MP4.",
          "nested mux failure also localizes its fixed detail");
    Check(LocalizeMessage(L"多音轨 MP4 封装已取消或超时") ==
          L"Saving the multi-track MP4 was cancelled or timed out.", "mux cancellation is localized");
    Check(Format(TextId::AudioRecoveryFile, L"D:\\录制\\{mic}.m4a") ==
          L"\r\nAudio recovery file: D:\\录制\\{mic}.m4a", "recovery file paths are preserved verbatim");
    Check(Format(TextId::RegionSummary, 1, 1280, 720, 22, 0) ==
          L"Display 1 · Source area 1280×720 · Origin (22, 0)", "region summary labels its coordinates");
    Check(LocalizeMessage(L"找不到所选显示器，可能已断开") ==
          L"The selected display could not be found. It may have been disconnected.",
          "disconnected display failure is localized");
    Check(LocalizeMessage(L"系统声音采集意外结束") == L"System audio capture ended unexpectedly.",
          "unexpected system audio failure is localized");
    Check(LocalizeMessage(L"麦克风采集意外结束") == L"Microphone capture ended unexpectedly.",
          "unexpected microphone failure is localized");
    Check(LocalizeMessage(L"麦克风缓存溢出，已停止录制") ==
          L"The microphone buffer overflowed; recording was stopped.", "audio overflow failure is localized");
    Check(LocalizeMessage(L"截图失败：无法保存截图文件：D:\\录制\\{截图}.png (0x80070005)") ==
          L"Screenshot failed: Could not save the screenshot: D:\\录制\\{截图}.png (0x80070005)",
          "nested screenshot failure preserves its path and technical code");
    Check(LocalizeMessage(L"截图失败：PNG 编码失败：0x80004005 (驱动程序原始错误)") ==
          L"Screenshot failed: PNG encoding failed: 0x80004005 (驱动程序原始错误)",
          "PNG failure translates deterministic context and preserves driver detail");
    Check(LocalizeMessage(L"屏幕捕获启动失败：当前没有有效的截图画面") ==
          L"Could not start screen capture: No valid frame is available for a screenshot.",
          "capture startup translates its deterministic nested cause");
    Check(LocalizeMessage(L"H.264/AAC 编码初始化失败。硬件尝试：创建视频媒体类型失败；软件尝试：H.264 视频流初始化失败：0x80004005") ==
          L"Could not initialize H.264/AAC encoding.\r\nHardware attempt: Could not create the video media type.\r\nSoftware attempt: Could not initialize the H.264 video stream: 0x80004005",
          "encoder fallback error translates both attempts and preserves HRESULT");
    Check(LocalizeMessage(L"导出目录可用空间不足：需要至少 1.50 GiB，当前剩余 100.00 MiB。 已停止录制并尝试保存已有内容。") ==
          L"There is not enough free space in the export folder: At least 1.50 GiB is required; 100.00 MiB is available. Recording stopped; an attempt was made to save the recorded content.",
          "storage failure translates its size detail and save attempt notice");

    CheckOutputSizeFailure({0, 1080}, L"录制画面的宽高必须大于零。", "nonpositive source is rejected");
    CheckOutputSizeFailure({srl::kMinOutputDimension - 1, 1080},
        std::format(L"录制区域至少需要 {}×{} 物理像素。", srl::kMinOutputDimension, srl::kMinOutputDimension),
        "source below the minimum is rejected");
    CheckOutputSizeFailure({1920, 1080, srl::ResolutionLimit::Custom, {srl::kMinOutputDimension - 2, 720}},
        std::format(L"自定义分辨率的宽高需在 {}～{} 之间。", srl::kMinOutputDimension, srl::kMaxOutputDimension),
        "custom bounds below the minimum are rejected");
    CheckOutputSizeFailure({1920, 1080, srl::ResolutionLimit::Custom, {srl::kMaxOutputDimension + 2, 720}},
        std::format(L"自定义分辨率的宽高需在 {}～{} 之间。", srl::kMinOutputDimension, srl::kMaxOutputDimension),
        "custom bounds above the maximum are rejected");
    CheckOutputSizeFailure({1920, 1080, srl::ResolutionLimit::Custom, {1281, 720}},
        L"自定义分辨率的宽高必须为偶数。", "odd custom bounds are rejected");
    CheckOutputSizeFailure({1920, 1080, static_cast<srl::ResolutionLimit>(255)},
        L"无法识别所选分辨率。", "unknown preset is rejected");
    CheckOutputSizeFailure({srl::kMaxOutputDimension + 2, 1080, srl::ResolutionLimit::Original},
        std::format(L"原始画面的单边超过 {} 像素，请选择较小的分辨率。", srl::kMaxOutputDimension),
        "original axis above the limit is rejected");
    CheckOutputSizeFailure({srl::kMaxOutputDimension, 2160, srl::ResolutionLimit::Original},
        std::format(L"原始画面超过 {} 像素，请选择较小的分辨率。", srl::kMaxOutputPixels),
        "original pixel count above the limit is rejected");
    CheckOutputSizeFailure({10000, srl::kMinOutputDimension, srl::ResolutionLimit::P720},
        std::format(L"输出画面小于 {}×{}，请选择较大的分辨率。", srl::kMinOutputDimension, srl::kMinOutputDimension),
        "extreme aspect ratio below minimum output is rejected");
    CheckOutputSizeFailure({4000, 3000, srl::ResolutionLimit::Custom, {4096, 4096}},
        std::format(L"输出画面超过 {} 像素，请选择较小的分辨率。", srl::kMaxOutputPixels),
        "custom output above the pixel budget is rejected");
    CheckOutputSizeFailure({4096, 2025, srl::ResolutionLimit::Original},
        std::format(L"输出画面超过 {} 像素，请选择较小的分辨率。", srl::kMaxOutputPixels),
        "padding across the pixel budget is rejected");

    for (std::size_t i = 0; i < static_cast<std::size_t>(TextId::Count); ++i) {
        const TextId id = static_cast<TextId>(i);
        srl::ui::SetLanguagePreference(1);
        const std::wstring chinese = Text(id);
        srl::ui::SetLanguagePreference(2);
        const std::wstring english = Text(id);
        Check(id == TextId::Empty || (!chinese.empty() && !english.empty()),
              "every UI resource has both languages");
        Check(PlaceholderCount(chinese) == PlaceholderCount(english),
              "both translations require the same number of values");
        Check(IsValidFormat(chinese) && IsValidFormat(english),
              "every translation is a valid runtime format string");
    }
    Check(Text(TextId::Count).empty(), "invalid resource IDs have a safe empty fallback");
    srl::ui::SetLanguagePreference(-1);
    Check(srl::ui::LanguagePreference() == 0, "invalid saved preference falls back to automatic");
    Check(srl::ui::IsEnglishLanguage() ==
          (PRIMARYLANGID(::GetUserDefaultUILanguage()) != LANG_CHINESE),
          "automatic language follows Windows UI language");
    srl::ui::SetLanguagePreference(3);
    Check(srl::ui::LanguagePreference() == 0, "unsupported preference falls back to automatic");
    std::printf("%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
