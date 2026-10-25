#include "app/ui_text.h"

#include <windows.h>

#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace srl::ui {
namespace {

bool g_english = false;
int g_languagePreference = 0;

const auto& Phrases() {
    static const std::unordered_map<std::wstring_view, std::wstring_view> values = {
        {L"轻录", L"ScreenRecorder Lite"},
        {L"自动（跟随系统）", L"Auto (follow Windows)"},
        {L"语言", L"Language"}, {L"简体中文", L"简体中文"},
        {L"English", L"English"},
        {L"轻录 - 录制控制", L"ScreenRecorder Lite — Recording controls"},
        {L"轻量、直接的屏幕录制", L"Lightweight desktop recording"},
        {L"显示器 {} · 源区域 {}×{} @ ({}, {})", L"Display {} · Source area {}×{} @ ({}, {})"},
        {L"截图快捷键 Ctrl+Alt+F10 不可用；仍可使用截图按钮。", L"Ctrl+Alt+F10 is unavailable; use the Screenshot button instead."},
        {L"快捷键 Ctrl+Alt+F9 不可用；仍可使用开始/停止按钮。", L"Ctrl+Alt+F9 is unavailable; use the Start and Stop buttons instead."},
        {L" · Ctrl+Alt+F9 开始/停止", L" · Ctrl+Alt+F9 to start/stop"},
        {L" · 使用按钮开始/停止", L" · Use the buttons to start/stop"},
        {L"录制范围", L"Capture area"}, {L"画质", L"Quality"}, {L"声音", L"Audio"},
        {L"保存位置", L"Save location"}, {L"全屏", L"Full screen"}, {L"区域", L"Region"},
        {L"选择导出目录", L"Select an export folder"},
        {L"显示器", L"Display"}, {L"源区域", L"Source area"},
        {L"起点", L"Origin"},
        {L"选择区域…", L"Select area…"}, {L"未选择区域", L"No area selected"},
        {L"分辨率", L"Resolution"}, {L"原始尺寸", L"Original size"}, {L"自定义…", L"Custom…"},
        {L"帧率", L"Frame rate"}, {L"极高", L"Very high"}, {L"高", L"High"},
        {L"中", L"Medium"}, {L"低", L"Low"},
        {L"画质越高，文件越大", L"Higher quality creates larger files"},
        {L"系统声音", L"System audio"}, {L"麦克风", L"Microphone"},
        {L"窗口", L"Window"}, {L"扬声器", L"Speakers"},
        {L"麦克风阵列", L"Microphone array"},
        {L"保存到", L"Save to"}, {L"更改", L"Browse"}, {L"就绪", L"Ready"},
        {L"打开文件夹", L"Open folder"}, {L"截图", L"Screenshot"},
        {L"开始录制", L"Start recording"}, {L"音频设备", L"Audio devices"},
        {L"默认播放设备", L"Default playback device"}, {L"默认输入设备", L"Default input device"},
        {L"系统声音 + 麦克风", L"System audio + microphone"},
        {L"声音已关闭（录制中可开启）", L"Audio is off (you can turn it on while recording)"},
        {L"未检测到可用显示器", L"No display is available."},
        {L"未检测到显示器", L"No display detected."},
        {L"请先选择录制区域", L"Select a capture area first."},
        {L"请先选择有效的录制区域", L"Select a valid capture area first."},
        {L"选择的显示器已不可用", L"The selected display is unavailable."},
        {L"录制区域至少需要 64×64 物理像素", L"The capture area must be at least 64 × 64 physical pixels."},
        {L"无法生成输出画面：", L"Cannot create the output frame: "},
        {L"请选择导出目录", L"Choose an export folder."},
        {L"请先选择有效的录制范围和画质", L"Select a valid capture area and quality."},
        {L"声音可在录制中开启", L"Audio can be enabled while recording"},
        {L"就绪 · ", L"Ready · "}, {L"使用按钮开始/停止", L"Use the buttons to start or stop"},
        {L"录制中…", L"Recording…"}, {L"正在截图…", L"Capturing screenshot…"},
        {L"正在截图", L"Capturing screenshot"}, {L"正在启动…", L"Starting…"},
        {L"即将开始录制…", L"Recording is about to start…"},
        {L"3 秒后开始", L"Starting in 3 seconds"}, {L"取消", L"Cancel"},
        {L"正在保存…", L"Saving…"}, {L"已开启", L"enabled"}, {L"已关闭", L"disabled"},
        {L"停止", L"Stop"}, {L"正在停止…", L"Finishing…"},
        {L"已取消录制", L"Recording cancelled."},
        {L"录制失败", L"Recording failed."}, {L"录制结束", L"Recording finished"},
        {L"录制已结束，但结果不可用", L"Recording ended, but no usable result is available."},
        {L"已保存：", L"Saved: "}, {L"未完成文件：", L"Incomplete file: "},
        {L"\r\n未完成文件：", L"\r\nIncomplete file: "},
        {L"视频已保存；截图失败：", L"Video saved; screenshot failed: "},
        {L"截图保存失败", L"Screenshot could not be saved"},
        {L"截图失败：", L"Screenshot failed: "}, {L"\r\n截图失败：", L"\r\nScreenshot failed: "},
        {L"截图失败，请稍后重试", L"Screenshot failed. Try again."},
        {L"已截图：", L"Screenshot saved: "}, {L"截图已保存", L"Screenshot saved"},
        {L"当前状态不能截图", L"A screenshot is unavailable in the current state."},
        {L"请选择截图保存位置", L"Choose a screenshot folder."},
        {L"请先选择有效的截图范围", L"Select a valid capture area for the screenshot."},
        {L"正在保存截图…", L"Saving screenshot…"},
        {L"正在保存截图，请稍后", L"Screenshots are being saved. Please wait."},
        {L"正在保存截图，完成后退出…", L"Saving screenshots before closing…"},
        {L"音频已恢复", L"Audio restored"}, {L"音频已关闭", L"Audio muted"},
        {L"已静音，画面继续录制", L"Audio muted; video recording continues"},
        {L"当前无法更改录制声音", L"Could not change the audio sources."},
        {L"无法开启系统声音：", L"Could not start system audio: "},
        {L"无法开启麦克风：", L"Could not start the microphone: "},
        {L"正在开启", L"Enabling "}, {L"正在关闭", L"Disabling "},
        {L"系统声音已开启", L"System audio enabled"}, {L"系统声音已关闭", L"System audio disabled"},
        {L"麦克风已开启", L"Microphone enabled"}, {L"麦克风已关闭", L"Microphone disabled"},
        {L"未选择可用显示器", L"No display is selected."},
        {L"所选显示器已不可用，请重新选择", L"The selected display is unavailable. Select another display."},
        {L"目标显示器已启用 HDR/高级色彩。首版只支持 SDR，请切换到 SDR 后重试。", L"HDR is enabled on this display. This version supports SDR only. Switch to SDR or select another display."},
        {L"截图区域无效或超出目标显示器，请重新选择", L"The screenshot area is invalid or outside the selected display."},
        {L"截图捕获失败：", L"Screenshot capture failed: "},
        {L"等待截图首帧失败：", L"Timed out waiting for the first screenshot frame: "},
        {L"截图画面复制失败：", L"Could not copy the screenshot frame: "},
        {L"截图画面复制失败", L"Could not copy the screenshot frame."},
        {L"截图画面数据无效", L"The screenshot frame is invalid."},
        {L"无法创建截图目录或生成文件名，请检查保存位置", L"Could not create the screenshot folder or a unique filename. Check the save location."},
        {L"截图保存已结束", L"Screenshot saving has ended."},
        {L"无法启动截图保存任务，请稍后重试", L"Could not start the screenshot task. Try again."},
        {L"截图组件初始化失败：", L"Could not initialize screenshot support: "},
        {L"截图组件初始化失败，请稍后重试", L"Could not initialize screenshot support. Try again."},
        {L"自定义分辨率", L"Custom resolution"}, {L"实际输出：—", L"Output size: —"},
        {L"请输入 64～4096 之间的偶数宽高。", L"Enter even width and height values between 64 and 4096."},
        {L"宽高需为偶数。", L"Width and height must be even."},
        {L"该尺寸不可用，请检查输入。", L"This size is unavailable. Check the values."},
        {L"输入上限宽高，画面保持原始比例且不会放大。", L"Set an output size limit. Keep the aspect ratio; do not upscale."},
        {L"宽度", L"Width"}, {L"高度", L"Height"}, {L"保存", L"Save"},
        {L"拖动选择区域；松开后点“确定选择”或按 Enter；Esc 取消", L"Drag to select. Release, then click Confirm or press Enter. Press Esc to cancel."},
        {L"确定选择  Enter", L"Confirm  Enter"}, {L"取消  Esc", L"Cancel  Esc"},
        {L"拖动选择区域", L"Drag to select a region"},
        {L"继续拖动调整区域", L"Keep dragging to adjust the region"},
        {L"已选择区域", L"Area selected"},
        {L"继续拖动微调区域", L"Keep dragging to refine the area"},
        {L"拖动以调整区域", L"Drag to adjust the area"},
        {L"继续拖动，选区至少 64×64", L"Keep dragging; the minimum area is 64 × 64."},
        {L"拖动选择区域 · Enter 确认 · Esc 取消", L"Drag to select · Enter confirms · Esc cancels"},
        {L"按 Enter 确认，按 Esc 取消", L"Press Enter to confirm or Esc to cancel."},
        {L"就绪 · ", L"Ready · "},
        {L"使用按钮开始/停止", L"Use the buttons to start or stop"},
        {L"截图正在保存，请稍后", L"Screenshots are being saved. Please wait."},
        {L"按住鼠标左键拖动选择区域", L"Drag with the left mouse button to select an area."},
        {L"选区至少 64×64，请继续拖动调整", L"The selection must be at least 64 × 64. Keep dragging to adjust it."},
        {L"未检测到显示器", L"No display detected."}, {L"未选择区域", L"No area selected"},
        {L"录制中", L"Recording"}, {L"正在保存截图", L"Saving screenshot"},
        {L"无法注册悬浮窗类", L"Could not register the floating control."},
        {L"无法创建悬浮控制窗", L"Could not create the floating control."},
        {L"截图当前画面", L"Capture the current frame"}, {L"取消录制", L"Cancel recording"},
        {L"保存中", L"Saving"}, {L"停止并保存", L"Stop and save"},
        {L"正在切换声音…", L"Changing audio source…"},
        {L"关闭系统声音", L"Mute system audio"}, {L"开启系统声音", L"Enable system audio"},
        {L"关闭麦克风", L"Mute microphone"}, {L"开启麦克风", L"Enable microphone"},
        {L"无法更改声音来源：", L"Could not change the audio source: "},
        {L"此声音设备仍在准备，请稍后重试", L"The audio device is still starting. Try again."},
        {L"声音切换超过了当前音频时间轴，请重试", L"Audio switching missed the current timeline. Try again."},
        {L"无法开启音频设备：", L"Could not start the audio device: "},
        {L"无法开启音频设备，请稍后重试", L"Could not start the audio device. Try again."},
        {L"录制正在停止，未更改声音来源", L"Recording is stopping; audio sources were unchanged."},
        {L"录制已结束，未更改声音来源", L"Recording ended; audio sources were unchanged."},
        {L"无法初始化 COM，程序无法启动。", L"Could not initialize Windows components."},
        {L"无法开始录制", L"Cannot start recording"},
        {L"无法从录制画面中排除悬浮窗，已取消开始", L"Could not exclude the floating controls; recording was cancelled."},
        {L"系统拒绝了悬浮窗的捕获排除设置，且当前范围无法把控制窗放在录制区域外。", L"Windows could not exclude the floating controls, and there is no room outside the capture area."},
        {L"请换用区域录制或检查系统版本。", L"Try region capture or check Windows support."},
        {L"（至少 64×64）", L" (minimum 64 × 64)"},
        {L"系统拒绝了悬浮窗的捕获排除设置，且当前范围无法把控制窗放在录制区域外。请换用区域录制或检查系统版本。", L"Windows could not exclude the floating controls, and there is no room to place them outside the capture area. Try a smaller capture area or check Windows support."},
        {L"录制结束", L"Recording finished"},
        {L"H.264 / MP4", L"H.264 / MP4"},
        {L"不录声音", L"Audio off"},
        {L"声音可在录制中开启", L"Audio can be enabled while recording"},
        {L"就绪", L"Ready"},
        {L"录制结束", L"Recording finished"},
        {L"确认提示", L"Confirmation"},
    };
    return values;
}

struct PrefixTranslation { std::wstring_view chinese; std::wstring_view english; };

constexpr PrefixTranslation kPrefixes[] = {
    {L" + 3 条 AAC 音轨（混音 / 系统声 / 麦克风）",
     L" + 3 AAC tracks (mix / system / mic)"},
    {L"麦克风阵列", L"Microphone array"},
    {L"麦克风", L"Microphone"},
    {L"扬声器", L"Speakers"},
    {L"录制中  ", L"Recording  "},
    {L"正在开启", L"Starting "},
    {L"正在关闭", L"Stopping "},
    {L"视频已保存；截图失败：", L"Video saved; screenshot failed: "},
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
    {L"已截图：", L"Screenshot saved: "},
    {L"已保存：", L"Saved: "},
    {L"未完成文件：", L"Incomplete file: "},
    {L"全屏 · 显示器 ", L"Full screen · Display "},
    {L"显示器 ", L"Display "},
    {L"无法计算输出尺寸：", L"Could not calculate the output size: "},
};

constexpr PrefixTranslation kEnglishPrefixes[] = {
    {L" + 3 AAC tracks (mix / system / mic)",
     L" + 3 条 AAC 音轨（混音 / 系统声 / 麦克风）"},
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

}  // namespace

void InitializeLanguage(int preference) {
    SetLanguagePreference(preference);
}

void SetLanguagePreference(int preference) {
    g_languagePreference = preference >= 0 && preference <= 2 ? preference : 0;
    g_english = g_languagePreference == 2 ||
        (g_languagePreference == 0 &&
         PRIMARYLANGID(::GetUserDefaultUILanguage()) != LANG_CHINESE);
}

int LanguagePreference() noexcept { return g_languagePreference; }

bool IsEnglishLanguage() noexcept { return g_english; }

std::wstring Text(std::wstring_view source) {
    if (source.empty()) return {};
    if (g_english) {
        if (const auto found = Phrases().find(source); found != Phrases().end()) {
            return std::wstring(found->second);
        }
    } else {
        for (const auto& [chinese, english] : Phrases()) {
            if (source == english) return std::wstring(chinese);
        }
        for (const auto& prefix : kEnglishPrefixes) {
            if (source.starts_with(prefix.chinese)) {
                return std::wstring(prefix.english) + std::wstring(source.substr(prefix.chinese.size()));
            }
        }
        return std::wstring(source);
    }
    for (const auto& prefix : kPrefixes) {
        if (source.starts_with(prefix.chinese)) {
            return std::wstring(prefix.english) + std::wstring(source.substr(prefix.chinese.size()));
        }
    }
    return std::wstring(source);
}

}  // namespace srl::ui
