#include <windows.h>

#include <winrt/base.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <format>
#include <map>
#include <optional>
#include <chrono>
#include <set>
#include <string>
#include <thread>
#include <string_view>
#include <vector>

#include "audio/audio_probe.h"
#include "capture/exclusion_probe.h"
#include "capture/wgc_probe.h"
#include "common/win32.h"
#include "core/output_format.h"
#include "image/png_probe.h"
#include "media/codec_probe.h"
#include "media/mf_util.h"
#include "media/mux_probe.h"
#include "platform/displays.h"
#include "platform/system_info.h"
#include "probe/test_pattern.h"
#include "probe/test_tone.h"
#include "recording/recorder.h"

#ifndef SRL_VERSION
#define SRL_VERSION "0.1.0"
#endif

namespace {

using srl::ConsoleLine;
using srl::HResultError;
using srl::Utf8ToWide;
using srl::WideToUtf8;

std::string JsonString(std::string_view value) {
    std::string result = "\"";
    for (const unsigned char c : value) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (c < 0x20) result += std::format("\\u{:04x}", c);
                else result += static_cast<char>(c);
        }
    }
    return result + '"';
}

void PrintDependencyInfo() {
    struct Library { const char* name; unsigned version; const char* license; const char* configuration; };
    const Library libraries[] = {
        {"avformat", avformat_version(), avformat_license(), avformat_configuration()},
        {"avcodec", avcodec_version(), avcodec_license(), avcodec_configuration()},
        {"avutil", avutil_version(), avutil_license(), avutil_configuration()},
    };
    std::string json = "{\"libraries\":[";
    for (const auto& library : libraries) {
        if (&library != libraries) json += ',';
        const auto version = std::format("{}.{}.{}", AV_VERSION_MAJOR(library.version),
            AV_VERSION_MINOR(library.version), AV_VERSION_MICRO(library.version));
        json += "{\"name\":" + JsonString(library.name) + ",\"version\":" + JsonString(version) +
            ",\"license\":" + JsonString(library.license) +
            ",\"configuration\":" + JsonString(library.configuration) + '}';
    }
    ConsoleLine(json + "]}");
}

enum class Command {
    Info,
    Codec,
    Audio,
    Mux,
    Png,
    Capture,
    Exclude,
    Record,
    Pattern,
    SelfTest,
    Help,
    Unknown,
};

Command ParseCommand(const std::wstring& name) {
    std::wstring lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    if (lower == L"info") return Command::Info;
    if (lower == L"codec") return Command::Codec;
    if (lower == L"audio") return Command::Audio;
    if (lower == L"mux") return Command::Mux;
    if (lower == L"png") return Command::Png;
    if (lower == L"capture") return Command::Capture;
    if (lower == L"exclude") return Command::Exclude;
    if (lower == L"record") return Command::Record;
    if (lower == L"pattern") return Command::Pattern;
    if (lower == L"selftest") return Command::SelfTest;
    if (lower == L"help" || lower == L"--help" || lower == L"-h") return Command::Help;
    return Command::Unknown;
}

class Arguments {
public:
    Arguments(int argc, wchar_t** argv, int firstIndex) {
        for (int i = firstIndex; i < argc; ++i) {
            std::wstring token = argv[i];
            if (token.starts_with(L"--")) {
                if (i + 1 < argc && !std::wstring_view(argv[i + 1]).starts_with(L"--")) {
                    options_.emplace(token, argv[i + 1]);
                    ++i;
                } else {
                    flags_.emplace(token);
                }
            } else {
                positional_.push_back(std::move(token));
            }
        }
    }

    [[nodiscard]] bool has(std::wstring_view name) const {
        const std::wstring key(name);
        return flags_.contains(key) || options_.contains(key);
    }

    [[nodiscard]] std::optional<std::wstring> value(std::wstring_view name) const {
        const auto it = options_.find(std::wstring(name));
        if (it == options_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    [[nodiscard]] int intValue(std::wstring_view name, int fallback, int minValue,
                               int maxValue, bool& valid) const {
        const auto text = value(name);
        if (!text.has_value()) {
            return fallback;
        }
        wchar_t* end = nullptr;
        const long parsed = std::wcstol(text->c_str(), &end, 10);
        if (end == text->c_str() || *end != L'\0' || parsed < minValue || parsed > maxValue) {
            valid = false;
            return fallback;
        }
        return static_cast<int>(parsed);
    }

    [[nodiscard]] const std::vector<std::wstring>& positional() const { return positional_; }

    [[nodiscard]] bool hasUnknownOptions() const {
        static const std::vector<std::wstring> known = {
            L"--width", L"--height", L"--fps", L"--bitrate-mbps", L"--seconds",
            L"--out", L"--out-dir", L"--monitor", L"--timeout-ms", L"--duration-ms",
            L"--no-audio", L"--software", L"--check-loopback", L"--check-mic",
            L"--verify", L"--desktop", L"--pattern",
            L"--tone", L"--audio", L"--mic", L"--limit", L"--format",
        };
        for (const auto& [key, value] : options_) {
            (void)value;
            if (std::find(known.begin(), known.end(), key) == known.end()) {
                return true;
            }
        }
        for (const auto& flag : flags_) {
            if (std::find(known.begin(), known.end(), flag) == known.end()) {
                return true;
            }
        }
        return false;
    }

private:
    std::map<std::wstring, std::wstring> options_;
    std::vector<std::wstring> positional_;
    std::set<std::wstring> flags_;
};

void PrintHelp() {
    ConsoleLine("ScreenRecorderLite diagnostics " SRL_VERSION);
    ConsoleLine();
    ConsoleLine("Usage: srl_probe <command> [options]");
    ConsoleLine();
    ConsoleLine("Commands:");
    ConsoleLine("  info       OS, session, DPI awareness, CPU/RAM/GPU, displays, HDR/advanced color,");
    ConsoleLine("             WGC/MF/WIC availability and default known folders.");
    ConsoleLine("  codec      Enumerate and negotiate H.264 (NV12 -> H.264) and AAC encoders.");
    ConsoleLine("             Options: --width --height --fps --bitrate-mbps");
    ConsoleLine("  audio      Enumerate active audio endpoints and shared-mode mix formats.");
    ConsoleLine("             Options: --check-loopback --check-mic");
    ConsoleLine("  mux        Encode a synthetic NV12/AAC clip to MP4 through SinkWriter,");
    ConsoleLine("             finalize it, then decode it back with MF Source Reader.");
    ConsoleLine("             Options: --out --width --height --fps --bitrate-mbps --seconds");
    ConsoleLine("                      --no-audio --software");
    ConsoleLine("  png        Save and decode a synthetic BGRA image through WIC.");
    ConsoleLine("             Options: --out --width --height");
    ConsoleLine("  capture    Capture one frame from a monitor through Windows Graphics Capture.");
    ConsoleLine("             Options: --monitor --out --timeout-ms");
    ConsoleLine("             WARNING: reads the desktop; never invoked by selftest.");
    ConsoleLine("  exclude    Apply WDA_EXCLUDEFROMCAPTURE to a short-lived topmost window.");
    ConsoleLine("             Options: --monitor --duration-ms --verify (captures one frame");
    ConsoleLine("             and checks that the window is absent from it)");
    ConsoleLine("  pattern    Show a full-monitor animated test pattern for --seconds N");
    ConsoleLine("             (development helper; does not record anything).");
    ConsoleLine("  record     Run one integrated recording through the real recorder.");
    ConsoleLine("             Default: shows an animated test-pattern window and records only");
    ConsoleLine("             that region (safe automated test). Options: --desktop records");
    ConsoleLine("             the selected monitor instead; --seconds --monitor --limit");
    ConsoleLine("             --format mp4|gif --fps --bitrate-mbps --out-dir --no-audio --audio --tone");
    ConsoleLine("  selftest   Run the non-capturing checks (info, codec, audio loopback readiness,");
    ConsoleLine("             hardware+software mux, PNG) and write artifacts to --out-dir.");
    ConsoleLine();
    ConsoleLine("Artifacts default to artifacts/diagnostics_<timestamp>.");
}

std::filesystem::path DefaultArtifactDirectory() {
    return std::filesystem::path(L"artifacts") /
           (L"diagnostics_" + srl::CurrentTimestampForFileName());
}

bool PrepareDirectory(const std::filesystem::path& directory) {
    std::string error;
    if (!srl::EnsureDirectory(directory, &error)) {
        ConsoleLine(std::format("error: {}", error));
        return false;
    }
    return true;
}

int RunInfo() {
    const srl::SystemInfo info = srl::QuerySystemInfo();
    srl::PrintSystemInfo(info);
    ConsoleLine();
    srl::PrintDisplays(srl::EnumerateDisplays());
    return 0;
}

int RunCodec(const Arguments& arguments) {
    bool valid = true;
    srl::CodecProbeOptions options;
    options.width = arguments.intValue(L"--width", 1920, 64, 4096, valid);
    options.height = arguments.intValue(L"--height", 1080, 64, 4096, valid);
    options.fps = arguments.intValue(L"--fps", 30, 1, 240, valid);
    options.bitrateMbps = arguments.intValue(L"--bitrate-mbps", 8, 1, 50, valid);
    if (!valid) {
        ConsoleLine("error: invalid numeric option");
        return 2;
    }
    const srl::CodecProbeResult result = srl::ProbeCodecs(options);
    srl::PrintCodecProbe(result);
    return (result.anyH264Negotiated && result.aac.negotiationOk) ? 0 : 3;
}

int RunAudio(const Arguments& arguments) {
    const srl::AudioProbeResult result =
        srl::QueryAudioProbe(arguments.has(L"--check-loopback"),
                             arguments.has(L"--check-mic"));
    srl::PrintAudioProbe(result);
    return result.fatalError.empty() ? 0 : 1;
}

int RunMux(const Arguments& arguments) {
    bool valid = true;
    srl::MuxProbeOptions options;
    const auto out = arguments.value(L"--out");
    options.outputPath = out.has_value()
                             ? *out
                             : (DefaultArtifactDirectory() / L"synthetic_av.mp4").wstring();
    options.width = arguments.intValue(L"--width", 1920, 64, 4096, valid);
    options.height = arguments.intValue(L"--height", 1080, 64, 4096, valid);
    options.fps = arguments.intValue(L"--fps", 30, 1, 120, valid);
    options.bitrateMbps = arguments.intValue(L"--bitrate-mbps", 8, 1, 50, valid);
    options.seconds = arguments.intValue(L"--seconds", 2, 1, 10, valid);
    options.includeAudio = !arguments.has(L"--no-audio");
    options.hardwareTransforms = !arguments.has(L"--software");
    if (!valid) {
        ConsoleLine("error: invalid numeric option");
        return 2;
    }
    const srl::MuxProbeResult result = srl::RunMuxProbe(options);
    srl::PrintMuxProbe(result);
    return result.ok ? 0 : 1;
}

int RunPng(const Arguments& arguments) {
    bool valid = true;
    const auto out = arguments.value(L"--out");
    const std::wstring path =
        out.has_value() ? *out : (DefaultArtifactDirectory() / L"synthetic.png").wstring();
    const int width = arguments.intValue(L"--width", 320, 16, 4096, valid);
    const int height = arguments.intValue(L"--height", 180, 16, 4096, valid);
    if (!valid) {
        ConsoleLine("error: invalid numeric option");
        return 2;
    }
    const srl::PngProbeResult result = srl::RunPngProbe(path, width, height);
    srl::PrintPngProbe(result);
    return result.ok ? 0 : 1;
}

int RunCapture(const Arguments& arguments) {
    bool valid = true;
    srl::WgcProbeOptions options;
    options.monitorIndex = arguments.intValue(L"--monitor", 0, 0, 64, valid);
    options.timeoutMs = arguments.intValue(L"--timeout-ms", 3000, 100, 20000, valid);
    if (const auto out = arguments.value(L"--out"); out.has_value()) {
        options.savePng = true;
        options.pngPath = *out;
    }
    if (!valid) {
        ConsoleLine("error: invalid numeric option");
        return 2;
    }
    const srl::WgcProbeResult result = srl::RunWgcProbe(options);
    srl::PrintWgcProbe(result);
    return result.ok ? 0 : 1;
}

int RunExclude(const Arguments& arguments) {
    bool valid = true;
    srl::ExclusionProbeOptions options;
    options.monitorIndex = arguments.intValue(L"--monitor", 0, 0, 64, valid);
    options.visibleDurationMs = arguments.intValue(L"--duration-ms", 500, 100, 5000, valid);
    options.verifyWithCapture = arguments.has(L"--verify");
    if (!valid) {
        ConsoleLine("error: invalid numeric option");
        return 2;
    }
    const srl::ExclusionProbeResult result = srl::RunExclusionProbe(options);
    srl::PrintExclusionProbe(result);
    return result.ok ? 0 : 1;
}


std::optional<srl::ResolutionLimit> ParseResolutionLimit(std::wstring_view text) {
    std::wstring lower(text);
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    if (lower == L"original" || lower == L"orig") return srl::ResolutionLimit::Original;
    if (lower == L"720" || lower == L"720p") return srl::ResolutionLimit::P720;
    if (lower == L"1080" || lower == L"1080p") return srl::ResolutionLimit::P1080;
    if (lower == L"1440" || lower == L"1440p") return srl::ResolutionLimit::P1440;
    if (lower == L"2160" || lower == L"2160p") return srl::ResolutionLimit::P2160;
    return std::nullopt;
}

int RunRecord(const Arguments& arguments) {
    const std::wstring formatName = arguments.value(L"--format").value_or(L"mp4");
    if (formatName != L"mp4" && formatName != L"gif") {
        ConsoleLine("error: --format must be mp4 or gif");
        return 2;
    }
    const auto format = formatName == L"gif" ? srl::RecordingFormat::Gif : srl::RecordingFormat::Mp4;
    bool valid = true;
    const int seconds = arguments.intValue(L"--seconds", 3, 1, 120, valid);
    const int monitorIndex = arguments.intValue(L"--monitor", 0, 0, 64, valid);
    const int defaultFps = format == srl::RecordingFormat::Gif
        ? srl::kDefaultGifFrameRate : srl::kDefaultMp4FrameRate;
    const int fps = arguments.intValue(L"--fps", defaultFps, 1, 60, valid);
    const int bitrate = arguments.intValue(L"--bitrate-mbps", 8, 1, 50, valid);
    const bool desktop = arguments.has(L"--desktop");
    const bool patternFlag = arguments.has(L"--pattern");
    const bool tone = arguments.has(L"--tone");
    const bool audioEnabled = format == srl::RecordingFormat::Mp4 && (arguments.has(L"--audio") || tone);
    const bool microphoneEnabled = format == srl::RecordingFormat::Mp4 && arguments.has(L"--mic");
    if (!valid) {
        ConsoleLine("error: invalid numeric option");
        return 2;
    }
    if (!srl::IsFrameRateSupported(format, fps)) {
        ConsoleLine(format == srl::RecordingFormat::Gif
            ? "error: GIF --fps must be 10, 15, 20 or 25" : "error: MP4 --fps must be 30 or 60");
        return 2;
    }
    if (desktop && patternFlag) {
        ConsoleLine("error: use either --desktop or --pattern, not both");
        return 2;
    }

    std::optional<srl::ResolutionLimit> limit;
    if (const auto spec = arguments.value(L"--limit"); spec.has_value()) {
        limit = ParseResolutionLimit(*spec);
        if (!limit.has_value()) {
            ConsoleLine("error: --limit must be original, 720, 1080, 1440 or 2160");
            return 2;
        }
    }

    const auto screens = srl::EnumerateDisplays();
    const srl::MonitorInfo* monitor = nullptr;
    if (monitorIndex == 0) {
        monitor = srl::PrimaryMonitor(screens);
    } else {
        for (const auto& candidate : screens) {
            if (candidate.index == monitorIndex) {
                monitor = &candidate;
                break;
            }
        }
    }
    if (monitor == nullptr) {
        ConsoleLine("error: selected monitor was not found");
        return 1;
    }

    std::filesystem::path outputDirectory;
    if (const auto outDir = arguments.value(L"--out-dir"); outDir.has_value()) {
        outputDirectory = *outDir;
    } else {
        outputDirectory = std::filesystem::path(L"artifacts") /
                          (L"record_" + srl::CurrentTimestampForFileName());
    }
    std::string directoryError;
    if (!srl::EnsureDirectory(outputDirectory, &directoryError)) {
        ConsoleLine(std::format("error: {}", directoryError));
        return 1;
    }
    outputDirectory = std::filesystem::absolute(outputDirectory);

    probe::TestPatternWindow pattern;
    RECT sourceRect{};
    if (!desktop) {
        std::wstring patternError;
        if (!probe::CreateTestPatternWindow(*monitor, pattern, &patternError)) {
            ConsoleLine(std::format("error: {}", srl::WideToUtf8(patternError)));
            return 1;
        }
        probe::PumpTestPatternWindow(pattern);
        ::Sleep(120);
        sourceRect = pattern.monitorLocalRect;
    } else {
        const int monitorWidth = monitor->virtualRect.right - monitor->virtualRect.left;
        const int monitorHeight = monitor->virtualRect.bottom - monitor->virtualRect.top;
        sourceRect = RECT{0, 0, monitorWidth, monitorHeight};
        ConsoleLine("WARNING: --desktop records actual desktop content for the selected region.");
    }

    if (!limit.has_value()) {
        limit = desktop ? srl::ResolutionLimit::P1080 : srl::ResolutionLimit::Original;
    }

    srl::RecordingConfig config;
    config.format = format;
    config.monitor = monitor->handle;
    config.monitorDeviceName = monitor->deviceName;
    config.sourceRect = sourceRect;
    config.limit = *limit;
    config.fps = fps;
    config.bitrateMbps = bitrate;
    config.systemAudio = audioEnabled;
    config.microphone = microphoneEnabled;
    config.outputDirectory = outputDirectory.wstring();

    probe::TestTone testTone;
    if (tone && format == srl::RecordingFormat::Mp4) {
        std::wstring toneError;
        if (!testTone.Start(seconds + 3, &toneError)) {
            ConsoleLine(std::format("error: {}", srl::WideToUtf8(toneError)));
            probe::DestroyTestPatternWindow(pattern);
            return 1;
        }
    }

    srl::RecordingSession session;
    std::wstring startError;
    if (!session.Start(config, &startError)) {
        testTone.Stop();
        probe::DestroyTestPatternWindow(pattern);
        ConsoleLine(std::format("error: {}", srl::WideToUtf8(startError)));
        return 1;
    }

    const std::wstring audioDescription =
        audioEnabled && microphoneEnabled ? L"system+microphone"
        : audioEnabled                   ? L"system"
        : microphoneEnabled              ? L"microphone"
                                         : L"off";
    ConsoleLine(std::format("record: {}x{} source @ {},{}; limit {}; {} fps; {} Mbps; audio {}",
                            sourceRect.right - sourceRect.left,
                            sourceRect.bottom - sourceRect.top, sourceRect.left,
                            sourceRect.top, srl::WideToUtf8(srl::LimitName(*limit)), fps,
                            bitrate, srl::WideToUtf8(audioDescription)));
    ConsoleLine(std::format("output directory: {}",
                            srl::WideToUtf8(outputDirectory.wstring())));
    if (tone) {
        ConsoleLine("test tone: 440 Hz for the recording duration");
    }

    const auto startedAt = std::chrono::steady_clock::now();
    bool recordingSeen = false;
    auto recordingSince = startedAt;
    while (true) {
        if (pattern.window != nullptr) {
            probe::PumpTestPatternWindow(pattern);
        }
        const srl::RecorderState state = session.state();
        if (state == srl::RecorderState::Finished || state == srl::RecorderState::Failed) {
            break;
        }
        if (state == srl::RecorderState::Recording && !recordingSeen) {
            recordingSeen = true;
            recordingSince = std::chrono::steady_clock::now();
            ConsoleLine("state: recording");
        }
        if (recordingSeen &&
            std::chrono::steady_clock::now() - recordingSince >=
                std::chrono::seconds(seconds)) {
            session.Stop();
            break;
        }
        if (!recordingSeen && std::chrono::steady_clock::now() - startedAt >
                                 std::chrono::seconds(8)) {
            ConsoleLine("error: recording did not reach the recording state in time");
            session.Stop();
            break;
        }
        ::Sleep(10);
    }

    // Keep pumping the test window until the worker has finalized and saved.
    const auto finalizeStart = std::chrono::steady_clock::now();
    while (session.state() != srl::RecorderState::Finished &&
           session.state() != srl::RecorderState::Failed) {
        if (pattern.window != nullptr) {
            probe::PumpTestPatternWindow(pattern);
        }
        if (std::chrono::steady_clock::now() - finalizeStart > std::chrono::seconds(20)) {
            ConsoleLine("error: recording did not finish within 20 seconds");
            break;
        }
        ::Sleep(10);
    }

    testTone.Stop();
    probe::DestroyTestPatternWindow(pattern);

    srl::RecorderResult result;
    if (!session.TakeResult(&result)) {
        ConsoleLine("error: recording result is not available");
        return 1;
    }

    ConsoleLine();
    ConsoleLine(std::format("result:             {}", result.success ? "OK" : "FAILED"));
    ConsoleLine(std::format("encoder:            {}", srl::WideToUtf8(result.encoderInfo)));
    ConsoleLine(std::format("output size:        {}x{}", result.outputSize.width,
                            result.outputSize.height));
    ConsoleLine(std::format("duration:           {:.3f} s", result.durationSeconds));
    ConsoleLine(std::format("video frames:       {}", result.videoFrames));
    ConsoleLine(std::format("audio chunks:       {}", result.audioChunks));
    if (!result.finalPath.empty()) {
        ConsoleLine(std::format("file:               {}", srl::WideToUtf8(result.finalPath)));
    }
    if (!result.partialPath.empty()) {
        ConsoleLine(std::format("partial file:       {}",
                                srl::WideToUtf8(result.partialPath)));
    }
    for (const auto& path : result.recoveryPaths) {
        ConsoleLine(std::format("recovery audio:     {}", srl::WideToUtf8(path)));
    }
    if (!result.error.empty()) {
        ConsoleLine(std::format("error:              {}", srl::WideToUtf8(result.error)));
    }
    return result.success ? 0 : 1;
}

int RunPattern(const Arguments& arguments) {
    bool valid = true;
    const int seconds = arguments.intValue(L"--seconds", 10, 1, 120, valid);
    const int monitorIndex = arguments.intValue(L"--monitor", 0, 0, 64, valid);
    if (!valid) {
        ConsoleLine("error: invalid numeric option");
        return 2;
    }
    const auto screens = srl::EnumerateDisplays();
    const srl::MonitorInfo* monitor = monitorIndex == 0 ? srl::PrimaryMonitor(screens) : nullptr;
    if (monitor == nullptr) {
        for (const auto& candidate : screens) {
            if (candidate.index == monitorIndex) {
                monitor = &candidate;
                break;
            }
        }
    }
    if (monitor == nullptr) {
        ConsoleLine("error: selected monitor was not found");
        return 1;
    }
    probe::TestPatternWindow pattern;
    std::wstring error;
    if (!probe::CreateTestPatternWindow(*monitor, pattern, &error)) {
        ConsoleLine(std::format("error: {}", srl::WideToUtf8(error)));
        return 1;
    }
    const int width = monitor->virtualRect.right - monitor->virtualRect.left;
    const int height = monitor->virtualRect.bottom - monitor->virtualRect.top;
    if (!probe::ResizeTestPatternWindow(*monitor, width, height, pattern, &error)) {
        ConsoleLine(std::format("error: {}", srl::WideToUtf8(error)));
        probe::DestroyTestPatternWindow(pattern);
        return 1;
    }
    ConsoleLine(std::format("test pattern: {}x{} at screen ({}, {}), {} seconds",
                            width, height, pattern.screenRect.left,
                            pattern.screenRect.top, seconds));
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(seconds)) {
        probe::PumpTestPatternWindow(pattern);
        ::Sleep(10);
    }
    probe::DestroyTestPatternWindow(pattern);
    return 0;
}

int RunSelfTest(const Arguments& arguments) {
    std::filesystem::path directory;
    if (const auto outDir = arguments.value(L"--out-dir"); outDir.has_value()) {
        directory = *outDir;
    } else {
        directory = DefaultArtifactDirectory();
    }
    if (!PrepareDirectory(directory)) {
        return 1;
    }
    const std::filesystem::path absoluteDirectory = std::filesystem::absolute(directory);
    const std::wstring stamp = srl::CurrentTimestampForFileName();
    ConsoleLine(std::format("Artifacts: {}", WideToUtf8(absoluteDirectory.wstring())));
    ConsoleLine();

    int failures = 0;
    if (RunInfo() != 0) {
        ++failures;
    }
    ConsoleLine();

    srl::CodecProbeOptions codecOptions;
    const srl::CodecProbeResult codec = srl::ProbeCodecs(codecOptions);
    srl::PrintCodecProbe(codec);
    if (!codec.anyH264Negotiated || !codec.aac.negotiationOk) {
        ++failures;
    }
    ConsoleLine();

    const srl::AudioProbeResult audio = srl::QueryAudioProbe(
        /*checkLoopback=*/true, /*checkMicrophone=*/false);
    srl::PrintAudioProbe(audio);
    if (!audio.fatalError.empty()) {
        ++failures;
    }
    ConsoleLine();

    srl::MuxProbeOptions hardwareOptions;
    hardwareOptions.outputPath =
        (absoluteDirectory / (L"mux_hardware_" + stamp + L".mp4")).wstring();
    const srl::MuxProbeResult hardware = srl::RunMuxProbe(hardwareOptions);
    srl::PrintMuxProbe(hardware);
    if (!hardware.ok) {
        ++failures;
    }
    ConsoleLine();

    srl::MuxProbeOptions softwareOptions = hardwareOptions;
    softwareOptions.outputPath =
        (absoluteDirectory / (L"mux_software_" + stamp + L".mp4")).wstring();
    softwareOptions.hardwareTransforms = false;
    const srl::MuxProbeResult software = srl::RunMuxProbe(softwareOptions);
    srl::PrintMuxProbe(software);
    if (!software.ok) {
        ++failures;
    }
    ConsoleLine();

    const std::wstring pngPath =
        (absoluteDirectory / (L"png_wic_" + stamp + L".png")).wstring();
    const srl::PngProbeResult png = srl::RunPngProbe(pngPath);
    srl::PrintPngProbe(png);
    if (!png.ok) {
        ++failures;
    }
    ConsoleLine();
    ConsoleLine(std::format("Self-test: {} failure(s)", failures));
    return failures == 0 ? 0 : 1;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    ::SetConsoleOutputCP(CP_UTF8);
    ::SetConsoleCP(CP_UTF8);
    ::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    // Packaging must be able to inspect dependency licensing on headless runners.
    if (argc == 2 && std::wstring_view(argv[1]) == L"--dependency-info") {
        PrintDependencyInfo();
        return 0;
    }

    bool apartmentInitialized = false;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        apartmentInitialized = true;
    } catch (const winrt::hresult_error& error) {
        ConsoleLine(std::format("warning: WinRT/COM initialization failed: {}",
                                srl::HResultText(error.code())));
        const HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr)) {
            ConsoleLine(std::format("error: CoInitializeEx failed: {}",
                                    srl::HResultText(hr)));
            return 1;
        }
    }

    std::optional<srl::MediaFoundationRuntime> mediaFoundation;
    try {
        mediaFoundation.emplace();
    } catch (const std::exception& error) {
        ConsoleLine(std::format("warning: Media Foundation unavailable: {}", error.what()));
    }

    int exitCode = 1;
    try {
        if (argc < 2) {
            PrintHelp();
            exitCode = 2;
        } else {
            const Command command = ParseCommand(argv[1]);
            Arguments arguments(argc, argv, 2);
            if (command == Command::Help) {
                PrintHelp();
                exitCode = 0;
            } else if (command == Command::Unknown) {
                ConsoleLine(std::format("error: unknown command '{}'", WideToUtf8(argv[1])));
                PrintHelp();
                exitCode = 2;
            } else if (arguments.hasUnknownOptions()) {
                ConsoleLine("error: unknown option");
                exitCode = 2;
            } else {
                ConsoleLine(std::format("ScreenRecorderLite diagnostics {}", SRL_VERSION));
                ConsoleLine();
                switch (command) {
                    case Command::Info:
                        exitCode = RunInfo();
                        break;
                    case Command::Codec:
                        exitCode = RunCodec(arguments);
                        break;
                    case Command::Audio:
                        exitCode = RunAudio(arguments);
                        break;
                    case Command::Mux:
                        exitCode = RunMux(arguments);
                        break;
                    case Command::Png:
                        exitCode = RunPng(arguments);
                        break;
                    case Command::Capture:
                        exitCode = RunCapture(arguments);
                        break;
                    case Command::Exclude:
                        exitCode = RunExclude(arguments);
                        break;
                    case Command::Record:
                        exitCode = RunRecord(arguments);
                        break;
                    case Command::Pattern:
                        exitCode = RunPattern(arguments);
                        break;
                    case Command::SelfTest:
                        exitCode = RunSelfTest(arguments);
                        break;
                    case Command::Help:
                    case Command::Unknown:
                        break;
                }
            }
        }
    } catch (const HResultError& error) {
        ConsoleLine(std::format("error: {}", error.what()));
        exitCode = 1;
    } catch (const winrt::hresult_error& error) {
        ConsoleLine(std::format("error: WinRT {}", srl::HResultText(error.code())));
        exitCode = 1;
    } catch (const std::exception& error) {
        ConsoleLine(std::format("error: {}", error.what()));
        exitCode = 1;
    }

    mediaFoundation.reset();
    if (apartmentInitialized) {
        winrt::uninit_apartment();
    } else {
        ::CoUninitialize();
    }
    return exitCode;
}
