#include "recording/recorder.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
#include <winrt/base.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <format>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "common/win32.h"
#include "core/bgra_to_nv12.h"
#include "core/storage.h"
#include "media/mf_util.h"
#include "media/mp4_muxer.h"
#include "platform/displays.h"
#include "recording/audio_capture.h"
#include "recording/screen_capture.h"

namespace srl {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::int64_t kTicksPerSecond = 10'000'000;
constexpr std::uint32_t kAudioRate = 48'000;
constexpr std::uint32_t kAudioChannels = 2;

struct QueuedAudioPacket {
    std::int64_t start = 0;
    std::vector<std::int16_t> samples;
};

struct LocalAudioSource {
    std::shared_ptr<AudioCapture> capture;
    std::deque<QueuedAudioPacket> packets;
};

struct PendingAudioOpen {
    unsigned sourceBit = 0;
    std::atomic<bool> cancelled{false};
    std::mutex mutex;
    bool finished = false;
    std::shared_ptr<AudioCapture> capture;
    std::wstring error;
    std::int64_t readyQpc = 0;
};

std::array<std::atomic<bool>, 2> g_audioOpenInFlight{};

struct LiveAudioState {
    unsigned currentMask = 0;
    unsigned requestedMask = 0;
    unsigned baseMask = 0;
    std::int64_t zeroQpc = 0;
    std::int64_t mixedThroughSample = 0;
    std::int64_t requestQpc = 0;
    std::deque<std::pair<std::int64_t, unsigned>> transitions;
};

struct SessionError {
    std::wstring message;
};

struct Cancelled {};

std::wstring HrTextWide(HRESULT hr) {
    return Utf8ToWide(HResultText(hr));
}

ComPtr<IMFMediaType> CreateVideoTargetType(const RecordingConfig& config,
                                           const OutputSize& output) {
    ComPtr<IMFMediaType> type;
    if (FAILED(::MFCreateMediaType(type.GetAddressOf()))) {
        return {};
    }
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    type->SetUINT32(MF_MT_AVG_BITRATE,
                    static_cast<std::uint32_t>(config.bitrateMbps) * 1'000'000U);
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    ::MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE,
                         static_cast<UINT32>(output.output.width),
                         static_cast<UINT32>(output.output.height));
    ::MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE,
                          static_cast<UINT32>(config.fps), 1);
    ::MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    return type;
}

ComPtr<IMFMediaType> CreateVideoInputType(const RecordingConfig& config,
                                          const OutputSize& output) {
    ComPtr<IMFMediaType> type;
    if (FAILED(::MFCreateMediaType(type.GetAddressOf()))) {
        return {};
    }
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    ::MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE,
                         static_cast<UINT32>(output.output.width),
                         static_cast<UINT32>(output.output.height));
    ::MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE,
                          static_cast<UINT32>(config.fps), 1);
    ::MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    return type;
}

ComPtr<IMFMediaType> CreateAudioTargetType() {
    ComPtr<IMFMediaType> type;
    if (FAILED(::MFCreateMediaType(type.GetAddressOf()))) {
        return {};
    }
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kAudioRate);
    type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kAudioChannels);
    type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 16'000);
    type->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);
    type->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
    return type;
}

ComPtr<IMFMediaType> CreateAudioInputType() {
    ComPtr<IMFMediaType> type;
    if (FAILED(::MFCreateMediaType(type.GetAddressOf()))) {
        return {};
    }
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kAudioRate);
    type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kAudioChannels);
    type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, kAudioChannels * 2);
    type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kAudioRate * kAudioChannels * 2);
    return type;
}

struct AudioOnlyWriter {
    ComPtr<IMFSinkWriter> writer;
    ComPtr<IMFByteStream> outputFile;
    DWORD stream = 0;
    std::wstring path;
};

bool OpenAudioOnlyWriter(const std::wstring& path, AudioOnlyWriter& audio,
                         std::wstring* error) {
    const HRESULT fileHr = ::MFCreateFile(MF_ACCESSMODE_READWRITE,
        MF_OPENMODE_FAIL_IF_EXIST, MF_FILEFLAGS_NONE, path.c_str(),
        audio.outputFile.GetAddressOf());
    if (FAILED(fileHr)) {
        if (error != nullptr) *error = L"无法创建分轨临时文件：" + HrTextWide(fileHr);
        return false;
    }
    audio.path = path;

    ComPtr<IMFAttributes> attributes;
    HRESULT hr = ::MFCreateAttributes(attributes.GetAddressOf(), 1);
    if (FAILED(hr)) {
        if (error != nullptr) *error = L"创建分轨编码属性失败：" + HrTextWide(hr);
        return false;
    }
    attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, FALSE);
    hr = ::MFCreateSinkWriterFromURL(path.c_str(), audio.outputFile.Get(),
                                     attributes.Get(), audio.writer.GetAddressOf());
    if (FAILED(hr)) {
        if (error != nullptr) *error = L"创建分轨编码器失败：" + HrTextWide(hr);
        return false;
    }
    const ComPtr<IMFMediaType> target = CreateAudioTargetType();
    const ComPtr<IMFMediaType> input = CreateAudioInputType();
    if (target == nullptr || input == nullptr) {
        if (error != nullptr) *error = L"创建分轨音频格式失败";
        return false;
    }
    hr = audio.writer->AddStream(target.Get(), &audio.stream);
    if (FAILED(hr)) {
        if (error != nullptr) *error = L"初始化 AAC 分轨失败：" + HrTextWide(hr);
        return false;
    }
    hr = audio.writer->SetInputMediaType(audio.stream, input.Get(), nullptr);
    if (FAILED(hr)) {
        if (error != nullptr) *error = L"设置 AAC 分轨格式失败：" + HrTextWide(hr);
        return false;
    }
    hr = audio.writer->BeginWriting();
    if (FAILED(hr)) {
        if (error != nullptr) *error = L"启动 AAC 分轨失败：" + HrTextWide(hr);
        return false;
    }
    return true;
}

std::wstring DescribeSinkWriterEncoder(IMFSinkWriter* writer, DWORD videoStream,
                                       bool hardwareRequested) {
    ComPtr<IMFSinkWriterEx> writerEx;
    if (writer == nullptr ||
        FAILED(writer->QueryInterface(IID_PPV_ARGS(writerEx.GetAddressOf())))) {
        return hardwareRequested ? L"硬件编码（已请求）" : L"软件编码";
    }
    for (DWORD index = 0; index < 8; ++index) {
        GUID category{};
        ComPtr<IMFTransform> transform;
        if (FAILED(writerEx->GetTransformForStream(videoStream, index, &category,
                                                    transform.GetAddressOf()))) {
            break;
        }
        if (!::IsEqualGUID(category, MFT_CATEGORY_VIDEO_ENCODER)) {
            continue;
        }
        std::wstring name = TransformFriendlyName(transform.Get());
        std::wstring info = hardwareRequested ? L"硬件编码" : L"软件编码";
        if (TransformSupportsD3D11(transform.Get())) {
            info += L"（D3D11 变换）";
        }
        if (!name.empty()) {
            info += L" - " + name;
        } else {
            info += L" - 编码器未暴露名称";
        }
        return info;
    }
    return hardwareRequested ? L"硬件编码（已请求）" : L"软件编码";
}

bool FileExists(const std::wstring& path) {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES;
}

bool WriteTestFile(const std::filesystem::path& directory, std::wstring* error) {
    const std::filesystem::path testPath =
        directory / (L"SRL_write_test_" + std::to_wstring(::GetCurrentProcessId()) + L"_" +
                     CurrentTimestampForFileName() + L".tmp");
    HANDLE file = ::CreateFileW(testPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error != nullptr) {
            *error = L"无法写入导出目录：" +
                     HrTextWide(HRESULT_FROM_WIN32(::GetLastError()));
        }
        return false;
    }
    DWORD written = 0;
    const char marker = 'S';
    const BOOL ok = ::WriteFile(file, &marker, 1, &written, nullptr);
    const DWORD writeError = ok ? ERROR_WRITE_FAULT : ::GetLastError();
    ::CloseHandle(file);
    if (!ok || written != 1) {
        if (error != nullptr) {
            *error = L"导出目录写入测试失败：" +
                     HrTextWide(HRESULT_FROM_WIN32(writeError));
        }
        return false;
    }
    return true;
}

std::int64_t RelativeSample(std::int64_t qpc, std::int64_t zero) {
    const std::int64_t relative = qpc - zero;
    return relative * static_cast<std::int64_t>(kAudioRate) / kTicksPerSecond;
}

}  // namespace

namespace detail {

void AddAudioPacket(const std::vector<std::int16_t>& samples,
                    std::int64_t packetStart, std::int64_t blockStart,
                    std::int64_t blockEnd, std::int64_t enabledFrom,
                    float gain, std::vector<float>& mix) {
    if (gain <= 0.0f || blockEnd <= blockStart) {
        return;
    }
    const auto frames = static_cast<std::int64_t>(samples.size() / kAudioChannels);
    const auto from = std::max({blockStart, packetStart, enabledFrom});
    const auto to = std::min(blockEnd, packetStart + frames);
    if (to <= from) {
        return;
    }
    const auto sourceOffset = (from - packetStart) * kAudioChannels;
    const auto destinationOffset = (from - blockStart) * kAudioChannels;
    const auto count = (to - from) * kAudioChannels;
    for (std::int64_t i = 0; i < count; ++i) {
        mix[static_cast<std::size_t>(destinationOffset + i)] +=
            static_cast<float>(samples[static_cast<std::size_t>(sourceOffset + i)]) * gain;
    }
}

void MixAudioPacketTimeline(const std::vector<std::int16_t>& samples,
                            std::int64_t packetStart, std::int64_t blockStart,
                            std::int64_t blockEnd, unsigned sourceBit,
                            unsigned initialMask,
                            const std::vector<std::pair<std::int64_t, unsigned>>& transitions,
                            std::vector<float>& mix, float overlapGain) {
    auto cursor = blockStart;
    auto mask = initialMask;
    const auto addSegment = [&](std::int64_t end) {
        const float gain = (mask & sourceBit) == 0 ? 0.0f
                            : (mask & 3U) == 3U ? overlapGain : 1.0f;
        AddAudioPacket(samples, packetStart, blockStart, end, cursor, gain, mix);
        cursor = end;
    };
    for (const auto& [sample, nextMask] : transitions) {
        if (sample <= cursor) {
            mask = nextMask;
            continue;
        }
        if (sample >= blockEnd) {
            break;
        }
        addSegment(sample);
        mask = nextMask;
    }
    addSegment(blockEnd);
}

void MixAudioPacketTimeline(const std::vector<std::int16_t>& samples,
                            std::int64_t packetStart, std::int64_t blockStart,
                            std::int64_t blockEnd, unsigned sourceBit,
                            unsigned initialMask,
                            const std::vector<std::pair<std::int64_t, unsigned>>& transitions,
                            std::vector<float>& mix) {
    MixAudioPacketTimeline(samples, packetStart, blockStart, blockEnd, sourceBit,
                           initialMask, transitions, mix, 0.5f);
}

void MixAudioPacketTimelineFullLevel(
    const std::vector<std::int16_t>& samples, std::int64_t packetStart,
    std::int64_t blockStart, std::int64_t blockEnd, unsigned sourceBit,
    unsigned initialMask,
    const std::vector<std::pair<std::int64_t, unsigned>>& transitions,
    std::vector<float>& mix) {
    MixAudioPacketTimeline(samples, packetStart, blockStart, blockEnd, sourceBit,
                           initialMask, transitions, mix, 1.0f);
}

bool AppendAudioTransition(std::deque<std::pair<std::int64_t, unsigned>>& transitions,
                            std::int64_t qpc, unsigned mask) {
    if (!transitions.empty()) {
        if (qpc < transitions.back().first) {
            return false;
        }
        if (qpc == transitions.back().first) {
            transitions.back().second = mask;
            return true;
        }
    }
    constexpr std::size_t kMaximumTransitions = 64;
    if (transitions.size() >= kMaximumTransitions) {
        return false;
    }
    transitions.emplace_back(qpc, mask);
    return true;
}

// The stream is created atomically and remains open while SinkWriter uses it.
// Never reopen a path selected by an earlier exists() check for writing.
bool CreateRecordingOutputFile(const std::filesystem::path& directory,
                               const std::wstring& base, std::wstring& finalPath,
                               std::wstring& partialPath, IMFByteStream** stream,
                               std::wstring* error) {
    finalPath.clear();
    partialPath.clear();
    if (stream == nullptr) {
        return false;
    }
    *stream = nullptr;
    for (int attempt = 0; attempt < 1000; ++attempt) {
        const std::wstring stem =
            base + (attempt == 0 ? L"" : L"_" + std::to_wstring(attempt));
        const std::filesystem::path finalCandidate = directory / (stem + L".mp4");
        const std::filesystem::path partialCandidate = directory / (stem + L"_partial.mp4");
        if (::GetFileAttributesW(finalCandidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
            continue;
        }
        const DWORD lastError = ::GetLastError();
        if (lastError != ERROR_FILE_NOT_FOUND && lastError != ERROR_PATH_NOT_FOUND) {
            if (error != nullptr) {
                *error = L"无法检查输出文件名：" + HrTextWide(HRESULT_FROM_WIN32(lastError));
            }
            return false;
        }
        const HRESULT hr = ::MFCreateFile(MF_ACCESSMODE_READWRITE, MF_OPENMODE_FAIL_IF_EXIST,
                                           MF_FILEFLAGS_NONE, partialCandidate.c_str(), stream);
        if (SUCCEEDED(hr)) {
            finalPath = finalCandidate.wstring();
            partialPath = partialCandidate.wstring();
            return true;
        }
        if (hr != HRESULT_FROM_WIN32(ERROR_FILE_EXISTS) &&
            hr != HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS) &&
            hr != HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION)) {
            if (error != nullptr) {
                *error = L"无法创建 MP4 临时文件：" + HrTextWide(hr);
            }
            return false;
        }
    }
    if (error != nullptr) {
        *error = L"无法生成不重复的输出文件名";
    }
    return false;
}

bool CheckRecordingStorage(const std::wstring& directory, std::uint64_t minimumFree,
                            std::wstring* error) {
    ULARGE_INTEGER freeBytes{};
    if (!::GetDiskFreeSpaceExW(directory.c_str(), &freeBytes, nullptr, nullptr)) {
        if (error != nullptr) {
            *error = L"无法读取导出目录的可用空间：" +
                     HrTextWide(HRESULT_FROM_WIN32(::GetLastError()));
        }
        return false;
    }
    if (freeBytes.QuadPart < minimumFree) {
        if (error != nullptr) {
            *error = L"导出目录可用空间不足：" + Utf8ToWide(std::format(
                "需要至少 {}，当前剩余 {}。", FormatBytes(minimumFree), FormatBytes(freeBytes.QuadPart)));
        }
        return false;
    }
    return true;
}

}  // namespace detail

struct RecordingSession::Impl {
    RecordingConfig config;
    OutputSize outputInfo{};
    std::wstring finalPath;
    std::wstring partialPath;

    std::atomic<RecorderState> state{RecorderState::Idle};
    std::atomic<std::int64_t> elapsed100ns{0};
    std::atomic<bool> stopRequested{false};
    std::thread worker;

    mutable std::mutex captureMutex;
    std::shared_ptr<ScreenCapture> liveCapture;
    mutable std::mutex audioControlMutex;
    LiveAudioState liveAudio;
    RecordingAudioState audioStatus;
    std::shared_ptr<PendingAudioOpen> pendingAudioOpen;
    std::atomic<float> microphoneLevel{0.0f};

    mutable std::mutex resultMutex;
    RecorderResult result;
    bool resultReady = false;

    bool Prepare(std::wstring* error);
    bool OpenWriter(bool hardware, ComPtr<IMFSinkWriter>& writer,
                    ComPtr<IMFByteStream>& outputFile, DWORD& videoStream,
                    DWORD& audioStream, std::wstring& encoderInfo, std::wstring& error);
    void Run();
};

bool RecordingSession::Impl::Prepare(std::wstring* error) {
    const auto fail = [error](std::wstring message) {
        if (error != nullptr) {
            *error = std::move(message);
        }
        return false;
    };

    if (!CheckMp4MuxerAvailable(error)) return false;

    if (config.fps != 30 && config.fps != 60) {
        return fail(L"帧率只支持 30 或 60");
    }
    if (config.bitrateMbps < 1 || config.bitrateMbps > 50) {
        return fail(L"视频码率需在 1～50 Mbps 之间");
    }
    if (config.outputDirectory.empty()) {
        return fail(L"请选择导出目录");
    }

    const int sourceWidth = config.sourceRect.right - config.sourceRect.left;
    const int sourceHeight = config.sourceRect.bottom - config.sourceRect.top;
    if (sourceWidth < 64 || sourceHeight < 64) {
        return fail(L"录制区域至少需要 64×64 物理像素");
    }

    const std::vector<MonitorInfo> displays = EnumerateDisplays();
    const MonitorInfo* monitor = nullptr;
    for (const MonitorInfo& candidate : displays) {
        if (config.monitor != nullptr && candidate.handle == config.monitor) {
            monitor = &candidate;
            break;
        }
    }
    if (monitor == nullptr && !config.monitorDeviceName.empty()) {
        for (const MonitorInfo& candidate : displays) {
            if (candidate.deviceName == config.monitorDeviceName) {
                monitor = &candidate;
                break;
            }
        }
    }
    if (monitor == nullptr) {
        return fail(L"找不到所选显示器，可能已断开");
    }
    if (IsAdvancedColorActive(*monitor)) {
        return fail(L"目标显示器已启用 HDR/高级色彩。首版只支持 SDR，请切换到 SDR 后重试。");
    }
    const int monitorWidth = monitor->virtualRect.right - monitor->virtualRect.left;
    const int monitorHeight = monitor->virtualRect.bottom - monitor->virtualRect.top;
    if (config.monitorPixelSize.width <= 0 || config.monitorPixelSize.height <= 0) {
        config.monitorPixelSize = Size{monitorWidth, monitorHeight};
    }
    if (config.monitorPixelSize.width > 0 && config.monitorPixelSize.height > 0 &&
        (config.monitorPixelSize.width != monitorWidth ||
         config.monitorPixelSize.height != monitorHeight)) {
        return fail(L"录制目标显示器尺寸已变化，请重新选择显示器或区域");
    }
    if (config.sourceRect.left < 0 || config.sourceRect.top < 0 ||
        config.sourceRect.right > monitorWidth ||
        config.sourceRect.bottom > monitorHeight) {
        return fail(L"录制区域超出目标显示器");
    }

    const OutputSizeRequest sizeRequest{
        .sourceWidth = sourceWidth,
        .sourceHeight = sourceHeight,
        .limit = config.limit,
        .customBound = config.customBound,
    };
    std::wstring sizeError;
    const auto output = ComputeOutputSize(sizeRequest, &sizeError);
    if (!output.has_value()) {
        return fail(L"无法计算输出尺寸：" + sizeError);
    }
    outputInfo = *output;

    std::string directoryError;
    if (!EnsureDirectory(config.outputDirectory, &directoryError)) {
        return fail(Utf8ToWide(directoryError));
    }
    if (!WriteTestFile(config.outputDirectory, error)) {
        return false;
    }

    const std::uint64_t required = StartSpaceRequirementWithAudioTracks(
        MbpsToBitsPerSecond(static_cast<double>(config.bitrateMbps)), 3);
    return detail::CheckRecordingStorage(config.outputDirectory, required, error);
}

bool RecordingSession::Impl::OpenWriter(bool hardware, ComPtr<IMFSinkWriter>& writer,
                                        ComPtr<IMFByteStream>& outputFile,
                                        DWORD& videoStream, DWORD& audioStream,
                                        std::wstring& encoderInfo, std::wstring& error) {
    writer.Reset();
    videoStream = 0;
    audioStream = 0;
    if (outputFile != nullptr) {
        outputFile->Close();
        outputFile.Reset();
        // Only a successful exclusive create assigns partialPath.
        ::DeleteFileW(partialPath.c_str());
    }
    if (!detail::CreateRecordingOutputFile(config.outputDirectory,
            L"Recording_" + CurrentTimestampForFileName(), finalPath, partialPath,
            outputFile.GetAddressOf(), &error)) {
        return false;
    }

    ComPtr<IMFAttributes> attributes;
    HRESULT hr = ::MFCreateAttributes(attributes.GetAddressOf(), 1);
    if (FAILED(hr)) {
        error = L"创建 SinkWriter 属性失败：" + HrTextWide(hr);
        return false;
    }
    attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, hardware ? TRUE : FALSE);

    hr = ::MFCreateSinkWriterFromURL(partialPath.c_str(), outputFile.Get(),
                                     attributes.Get(), writer.GetAddressOf());
    if (FAILED(hr)) {
        error = L"创建 MP4 写入器失败：" + HrTextWide(hr);
        return false;
    }

    const ComPtr<IMFMediaType> videoTarget = CreateVideoTargetType(config, outputInfo);
    const ComPtr<IMFMediaType> videoInput = CreateVideoInputType(config, outputInfo);
    if (videoTarget == nullptr || videoInput == nullptr) {
        error = L"创建视频媒体类型失败";
        writer.Reset();
        return false;
    }
    hr = writer->AddStream(videoTarget.Get(), &videoStream);
    if (FAILED(hr)) {
        error = L"H.264 视频流初始化失败：" + HrTextWide(hr);
        writer.Reset();
        return false;
    }
    hr = writer->SetInputMediaType(videoStream, videoInput.Get(), nullptr);
    if (FAILED(hr)) {
        error = L"视频输入格式（NV12）不被编码器接受：" + HrTextWide(hr);
        writer.Reset();
        return false;
    }

    // The stream layout is fixed once BeginWriting succeeds. Keep an AAC
    // timeline even when both inputs start disabled so either source can be
    // opened later without touching its endpoint before the user's request.
    const ComPtr<IMFMediaType> audioTarget = CreateAudioTargetType();
    const ComPtr<IMFMediaType> audioInput = CreateAudioInputType();
    if (audioTarget == nullptr || audioInput == nullptr) {
        error = L"创建音频媒体类型失败";
        writer.Reset();
        return false;
    }
    hr = writer->AddStream(audioTarget.Get(), &audioStream);
    if (FAILED(hr)) {
        error = L"AAC 音频流初始化失败：" + HrTextWide(hr);
        writer.Reset();
        return false;
    }
    hr = writer->SetInputMediaType(audioStream, audioInput.Get(), nullptr);
    if (FAILED(hr)) {
        error = L"音频输入格式不被 AAC 编码器接受：" + HrTextWide(hr);
        writer.Reset();
        return false;
    }

    hr = writer->BeginWriting();
    if (FAILED(hr)) {
        error = L"SinkWriter 开始写入失败：" + HrTextWide(hr);
        writer.Reset();
        return false;
    }

    encoderInfo = DescribeSinkWriterEncoder(writer.Get(), videoStream, hardware);
    return true;
}

void RecordingSession::Impl::Run() {
    state.store(RecorderState::Starting, std::memory_order_release);

    bool apartmentInitialized = false;
    bool mfStarted = false;
    bool failure = false;
    std::wstring failureMessage;
    bool cleanStop = false;
    std::int64_t zeroQpc = 0;
    std::int64_t stopElapsed = -1;
    int videoFrames = 0;
    int audioChunks = 0;
    std::int64_t audioSamplesWritten = 0;
    float audioLimiterGain = 1.0f;
    std::array<AudioOnlyWriter, 2> sourceAudioWriters;
    std::array<std::int64_t, 2> sourceAudioSamplesWritten{};
    std::array<std::wstring, 2> sourceAudioPaths;
    std::wstring encoderInfo;

    ComPtr<IMFSinkWriter> writer;
    ComPtr<IMFByteStream> outputFile;
    DWORD videoStream = 0;
    DWORD audioStream = 0;
    std::shared_ptr<ScreenCapture> capture;
    LocalAudioSource systemAudioSource;
    LocalAudioSource microphoneSource;

    const auto setFailure = [&failure, &failureMessage](std::wstring message) {
        if (!failure) {
            failure = true;
            failureMessage = std::move(message);
        }
    };

    BgraToNv12Converter converter;
    bool converterReady = false;
    std::vector<std::uint8_t> latestBgra;
    std::vector<std::uint8_t> nv12;
    bool haveFrame = false;

    const auto writeVideoSample = [&](std::int64_t timestamp,
                                      std::int64_t duration) -> bool {
        if (writer == nullptr || nv12.empty()) {
            return false;
        }
        ComPtr<IMFMediaBuffer> buffer;
        HRESULT hr = ::MFCreateMemoryBuffer(static_cast<DWORD>(nv12.size()),
                                            buffer.GetAddressOf());
        if (FAILED(hr)) {
            return false;
        }
        BYTE* destination = nullptr;
        if (FAILED(buffer->Lock(&destination, nullptr, nullptr))) {
            return false;
        }
        std::memcpy(destination, nv12.data(), nv12.size());
        buffer->Unlock();
        buffer->SetCurrentLength(static_cast<DWORD>(nv12.size()));

        ComPtr<IMFSample> sample;
        if (FAILED(::MFCreateSample(sample.GetAddressOf())) ||
            FAILED(sample->AddBuffer(buffer.Get()))) {
            return false;
        }
        sample->SetSampleTime(timestamp);
        sample->SetSampleDuration(duration);
        hr = writer->WriteSample(videoStream, sample.Get());
        return SUCCEEDED(hr);
    };

    static const std::vector<std::int16_t> kSilence(4800 * kAudioChannels, 0);

    const auto writePcmTo = [&](IMFSinkWriter* targetWriter, DWORD targetStream,
                                std::int64_t& writtenSamples,
                                const std::int16_t* data, std::int64_t frames,
                                int* chunksWritten) -> bool {
        if (targetWriter == nullptr) return false;
        if (frames <= 0) {
            return true;
        }
        std::int64_t offset = 0;
        while (offset < frames) {
            const std::int64_t chunk = std::min<std::int64_t>(4800, frames - offset);
            ComPtr<IMFMediaBuffer> buffer;
            HRESULT hr = ::MFCreateMemoryBuffer(
                static_cast<DWORD>(chunk * kAudioChannels * sizeof(std::int16_t)),
                buffer.GetAddressOf());
            if (FAILED(hr)) {
                return false;
            }
            BYTE* destination = nullptr;
            if (FAILED(buffer->Lock(&destination, nullptr, nullptr))) {
                return false;
            }
            std::memcpy(destination, data + offset * kAudioChannels,
                        static_cast<std::size_t>(chunk) * kAudioChannels *
                            sizeof(std::int16_t));
            buffer->Unlock();
            buffer->SetCurrentLength(static_cast<DWORD>(
                chunk * kAudioChannels * sizeof(std::int16_t)));

            ComPtr<IMFSample> sample;
            if (FAILED(::MFCreateSample(sample.GetAddressOf())) ||
                FAILED(sample->AddBuffer(buffer.Get()))) {
                return false;
            }
            const std::int64_t timestamp =
                writtenSamples * kTicksPerSecond / kAudioRate;
            const std::int64_t duration = chunk * kTicksPerSecond / kAudioRate;
            sample->SetSampleTime(timestamp);
            sample->SetSampleDuration(duration);
            hr = targetWriter->WriteSample(targetStream, sample.Get());
            if (FAILED(hr)) {
                return false;
            }
            writtenSamples += chunk;
            if (chunksWritten != nullptr) ++*chunksWritten;
            offset += chunk;
        }
        return true;
    };

    const auto writePcm = [&](const std::int16_t* data, std::int64_t frames) -> bool {
        return writePcmTo(writer.Get(), audioStream, audioSamplesWritten,
                          data, frames, &audioChunks);
    };

    const auto writeSourcePcm = [&](std::size_t index, const std::int16_t* data,
                                    std::int64_t frames) {
        AudioOnlyWriter& track = sourceAudioWriters[index];
        return writePcmTo(track.writer.Get(), track.stream,
                          sourceAudioSamplesWritten[index], data, frames, nullptr);
    };

    const auto padSilenceTo = [&](std::int64_t targetSample) -> bool {
        while (audioSamplesWritten < targetSample) {
            const std::int64_t frames =
                std::min<std::int64_t>(4800, targetSample - audioSamplesWritten);
            if (!writePcm(kSilence.data(), frames)) {
                return false;
            }
            if (!writeSourcePcm(0, kSilence.data(), frames) ||
                !writeSourcePcm(1, kSilence.data(), frames)) return false;
        }
        return true;
    };

    const auto stopAudio = [&] {
        if (systemAudioSource.capture != nullptr) {
            systemAudioSource.capture->Stop();
        }
        if (microphoneSource.capture != nullptr) {
            microphoneSource.capture->Stop();
        }
    };

    const auto joinAudio = [&] {
        if (systemAudioSource.capture != nullptr) {
            systemAudioSource.capture->Join();
        }
        if (microphoneSource.capture != nullptr) {
            microphoneSource.capture->Join();
        }
    };

    const auto pullAudio = [&](LocalAudioSource& source) {
        if (source.capture == nullptr) {
            return;
        }
        AudioPacket packet;
        while (source.capture->TryPop(packet)) {
            const std::int64_t frames =
                static_cast<std::int64_t>(packet.samples.size() / kAudioChannels);
            if (frames <= 0) {
                continue;
            }
            const std::int64_t start = RelativeSample(packet.qpcPosition100ns, zeroQpc);
            source.packets.push_back(
                QueuedAudioPacket{start, std::move(packet.samples)});
        }
    };

    const auto closeSource = [&](LocalAudioSource& source) {
        if (source.capture != nullptr) {
            source.capture->Stop();
            source.capture->Join();
            // Stop does not discard already captured packets. A transition
            // gates them at the requested cutoff while preserving backlog
            // that belongs before the user's click.
            pullAudio(source);
            source.capture.reset();
        }
    };

    const auto cancelPendingAudioOpen = [&] {
        if (pendingAudioOpen == nullptr) return;
        auto slot = std::move(pendingAudioOpen);
        std::shared_ptr<AudioCapture> readyCapture;
        {
            std::lock_guard lock(slot->mutex);
            slot->cancelled.store(true, std::memory_order_release);
            if (slot->finished) readyCapture = slot->capture;
        }
        if (readyCapture != nullptr) {
            readyCapture->Stop();
            readyCapture->Join();
            std::lock_guard lock(slot->mutex);
            slot->capture.reset();
            g_audioOpenInFlight[slot->sourceBit == 1U ? 0U : 1U].store(
                false, std::memory_order_release);
        }
    };

    const auto processAudioRequest = [&] {
        if (stopRequested.load(std::memory_order_relaxed)) {
            cancelPendingAudioOpen();
            std::lock_guard lock(audioControlMutex);
            if (audioStatus.pending) {
                liveAudio.requestedMask = liveAudio.currentMask;
                audioStatus.pending = false;
                audioStatus.error = L"录制正在停止，未更改声音来源";
                ++audioStatus.revision;
            }
            return;
        }
        unsigned requestedMask = 0;
        std::int64_t requestQpc = 0;
        {
            std::lock_guard lock(audioControlMutex);
            if (!audioStatus.pending) return;
            requestedMask = liveAudio.requestedMask;
            requestQpc = liveAudio.requestQpc;
        }
        const auto failRequest = [&](std::wstring message) {
            cancelPendingAudioOpen();
            std::lock_guard lock(audioControlMutex);
            liveAudio.requestedMask = liveAudio.currentMask;
            audioStatus.systemAudio = (liveAudio.currentMask & 1U) != 0;
            audioStatus.microphone = (liveAudio.currentMask & 2U) != 0;
            audioStatus.pending = false;
            audioStatus.error = std::move(message);
            ++audioStatus.revision;
        };

        if (pendingAudioOpen != nullptr) {
            bool finished = false;
            {
                std::lock_guard lock(pendingAudioOpen->mutex);
                finished = pendingAudioOpen->finished;
            }
            if (!finished) return;

            const unsigned sourceBit = pendingAudioOpen->sourceBit;
            std::shared_ptr<AudioCapture> opened;
            std::wstring openError;
            std::int64_t readyQpc = 0;
            {
                std::lock_guard lock(pendingAudioOpen->mutex);
                opened = pendingAudioOpen->capture;
                openError = std::move(pendingAudioOpen->error);
                readyQpc = pendingAudioOpen->readyQpc;
            }
            pendingAudioOpen.reset();
            if (opened == nullptr || !openError.empty()) {
                if (opened != nullptr) {
                    opened->Stop();
                    opened->Join();
                }
                failRequest(openError.empty()
                    ? (sourceBit == 1U ? L"无法开启系统声音" : L"无法开启麦克风")
                    : std::move(openError));
                return;
            }
            LocalAudioSource& source = sourceBit == 1U ? systemAudioSource : microphoneSource;
            source.capture = std::move(opened);
            const std::size_t sourceIndex = sourceBit == 1U ? 0U : 1U;
            g_audioOpenInFlight[sourceIndex].store(false, std::memory_order_release);

            unsigned nextMask = 0;
            {
                std::lock_guard lock(audioControlMutex);
                nextMask = liveAudio.currentMask | sourceBit;
                if (!detail::AppendAudioTransition(liveAudio.transitions, readyQpc, nextMask)) {
                    // Append is rolled back below; keep the just-opened source
                    // out of the published state when its QPC is stale.
                    nextMask = liveAudio.currentMask;
                } else {
                    liveAudio.currentMask = nextMask;
                    audioStatus.systemAudio = (nextMask & 1U) != 0;
                    audioStatus.microphone = (nextMask & 2U) != 0;
                    ++audioStatus.revision;
                }
            }
            if ((nextMask & sourceBit) == 0) {
                closeSource(source);
                failRequest(L"声音切换超过了当前音频时间轴，请重试");
                return;
            }
        }

        unsigned currentMask = 0;
        {
            std::lock_guard lock(audioControlMutex);
            currentMask = liveAudio.currentMask;
        }
        const unsigned missingMask = requestedMask & ~currentMask;
        if (missingMask != 0) {
            const unsigned sourceBit = (missingMask & 1U) != 0 ? 1U : 2U;
            const std::size_t sourceIndex = sourceBit == 1U ? 0U : 1U;
            const std::wstring& deviceId = sourceBit == 1U
                ? config.systemAudioDeviceId : config.microphoneDeviceId;
            bool expected = false;
            if (!g_audioOpenInFlight[sourceIndex].compare_exchange_strong(
                    expected, true, std::memory_order_acq_rel)) {
                failRequest(L"此声音设备仍在准备，请稍后重试");
                return;
            }

            auto slot = std::make_shared<PendingAudioOpen>();
            slot->sourceBit = sourceBit;
            pendingAudioOpen = slot;
            try {
                std::thread opener([slot, sourceBit, deviceId]() mutable {
                    HRESULT apartment = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                    std::shared_ptr<AudioCapture> capture;
                    std::wstring error;
                    std::int64_t readyQpc = 0;
                    if (FAILED(apartment) && apartment != RPC_E_CHANGED_MODE) {
                        error = L"无法初始化音频设备线程：" + FormatHResultMessage(apartment);
                    } else if (!slot->cancelled.load(std::memory_order_acquire)) {
                        try {
                            capture = std::make_shared<AudioCapture>(
                                sourceBit == 1U ? AudioSource::SystemLoopback
                                                : AudioSource::Microphone,
                                deviceId);
                            if (!capture->Start(&error)) {
                                capture.reset();
                            } else {
                                readyQpc = QpcNow100ns();
                            }
                        } catch (const std::exception& exception) {
                            error = L"无法开启音频设备：" + Utf8ToWide(exception.what());
                            capture.reset();
                        } catch (...) {
                            error = L"无法开启音频设备，请稍后重试";
                            capture.reset();
                        }
                    } else {
                        error = L"声音切换已取消";
                    }
                    bool publishedCapture = false;
                    {
                        std::lock_guard lock(slot->mutex);
                        if (error.empty() && capture != nullptr &&
                            !slot->cancelled.load(std::memory_order_acquire)) {
                            slot->capture = std::move(capture);
                            slot->readyQpc = readyQpc;
                            publishedCapture = true;
                        } else {
                            if (error.empty()) error = L"声音切换已取消";
                            slot->error = std::move(error);
                        }
                        slot->finished = true;
                    }
                    if (capture != nullptr) {
                        capture->Stop();
                        capture->Join();
                        capture.reset();
                    }
                    if (!publishedCapture) {
                        g_audioOpenInFlight[sourceBit == 1U ? 0U : 1U].store(
                            false, std::memory_order_release);
                    }
                    slot.reset();
                    if (SUCCEEDED(apartment)) ::CoUninitialize();
                });
                opener.detach();
            } catch (const std::exception& exception) {
                pendingAudioOpen.reset();
                g_audioOpenInFlight[sourceIndex].store(false, std::memory_order_release);
                failRequest(L"无法启动音频设备线程：" + Utf8ToWide(exception.what()));
            }
            return;
        }

        if (requestedMask != currentMask) {
            const std::int64_t transitionQpc = QpcNow100ns();
            if (transitionQpc <= 0 ||
                !detail::AppendAudioTransition(liveAudio.transitions,
                                               transitionQpc, requestedMask)) {
                failRequest(L"声音切换超过了当前音频时间轴，请重试");
                return;
            }
            if ((currentMask & 1U) != 0 && (requestedMask & 1U) == 0) {
                closeSource(systemAudioSource);
            }
            if ((currentMask & 2U) != 0 && (requestedMask & 2U) == 0) {
                closeSource(microphoneSource);
            }
            currentMask = requestedMask;
        }
        {
            std::lock_guard lock(audioControlMutex);
            liveAudio.currentMask = currentMask;
            liveAudio.requestedMask = currentMask;
            audioStatus.systemAudio = (currentMask & 1U) != 0;
            audioStatus.microphone = (currentMask & 2U) != 0;
            audioStatus.pending = false;
            audioStatus.error.clear();
            ++audioStatus.revision;
        }
        (void)requestQpc;
    };

    const auto audioNeedsAttention = [&] {
        const auto sourceIssue = [&](const LocalAudioSource& source) {
            if (source.capture == nullptr) {
                return false;
            }
            if (source.capture->overflowed()) {
                return true;
            }
            return !stopRequested.load(std::memory_order_relaxed) &&
                   source.capture->ended();
        };
        return sourceIssue(systemAudioSource) || sourceIssue(microphoneSource);
    };

    const auto drainAudio = [&](std::int64_t stopSample) -> bool {
        if (writer == nullptr) {
            return true;
        }
        pullAudio(systemAudioSource);
        pullAudio(microphoneSource);

        // Drive the audio timeline from the shared QPC clock instead of how
        // much data the endpoints happened to deliver. WASAPI loopback can be
        // completely idle when nothing is playing; waiting for packets in that
        // state leaves SinkWriter without an audio sample and can block later
        // video writes while it tries to interleave the two streams. A short
        // look-ahead covers normal endpoint latency, and missing data becomes
        // silence as required for a silent source.
        std::int64_t target = 0;
        if (stopSample >= 0) {
            target = stopSample;
        } else {
            const std::int64_t now =
                std::max<std::int64_t>(0, QpcNow100ns() - zeroQpc);
            const std::int64_t wallSample =
                now * static_cast<std::int64_t>(kAudioRate) / kTicksPerSecond;
            target = std::max<std::int64_t>(0, wallSample - kAudioRate / 10);
            // Do not let one slow video iteration produce a huge burst.
            target = std::min(target, audioSamplesWritten + kAudioRate / 2);
        }
        if (target <= audioSamplesWritten) {
            return true;
        }

        while (audioSamplesWritten < target) {
            const std::int64_t frames =
                std::min<std::int64_t>(4800, target - audioSamplesWritten);
            const std::int64_t blockStart = audioSamplesWritten;
            const std::int64_t blockEnd = blockStart + frames;
            const std::size_t blockSampleCount =
                static_cast<std::size_t>(frames) * kAudioChannels;
            std::array<std::vector<float>, 2> sourceMix{
                std::vector<float>(blockSampleCount, 0.0f),
                std::vector<float>(blockSampleCount, 0.0f)};

            unsigned initialMask = 0;
            std::vector<std::pair<std::int64_t, unsigned>> transitions;
            {
                std::lock_guard lock(audioControlMutex);
                initialMask = liveAudio.baseMask;
                transitions.reserve(liveAudio.transitions.size());
                for (const auto& [qpc, mask] : liveAudio.transitions) {
                    transitions.emplace_back(RelativeSample(qpc, zeroQpc), mask);
                }
                // A switch cannot retroactively modify a block handed to the
                // encoder. Normal mixing trails QPC by 100 ms, so UI events
                // fall after this boundary while captured backlog keeps its
                // original control history.
                liveAudio.mixedThroughSample = blockEnd;
            }

            const auto addSource = [&](LocalAudioSource& source, unsigned sourceBit,
                                       std::vector<float>& trackMix) {
                while (!source.packets.empty()) {
                    const QueuedAudioPacket& front = source.packets.front();
                    const std::int64_t end =
                        front.start +
                        static_cast<std::int64_t>(front.samples.size() / kAudioChannels);
                    if (end <= blockStart) {
                        source.packets.pop_front();
                        continue;
                    }
                    break;
                }
                for (const QueuedAudioPacket& packet : source.packets) {
                    if (packet.start >= blockEnd) {
                        break;
                    }
                    detail::MixAudioPacketTimelineFullLevel(
                        packet.samples, packet.start, blockStart, blockEnd,
                        sourceBit, initialMask, transitions, trackMix);
                }
            };
            addSource(systemAudioSource, 1U, sourceMix[0]);
            addSource(microphoneSource, 2U, sourceMix[1]);

            std::vector<float> mix(blockSampleCount, 0.0f);
            for (std::size_t i = 0; i < blockSampleCount; ++i) {
                mix[i] = sourceMix[0][i] + sourceMix[1][i];
            }

            float peak = 0.0f;
            for (const float sample : mix) {
                peak = std::max(peak, std::abs(sample));
            }
            constexpr float kLimiterCeiling = 30'000.0f;
            const float targetGain = peak > kLimiterCeiling
                ? kLimiterCeiling / peak : 1.0f;
            if (targetGain < audioLimiterGain) {
                audioLimiterGain = targetGain;
            } else {
                const float release = std::exp(-static_cast<float>(frames) /
                    (static_cast<float>(kAudioRate) * 0.15f));
                audioLimiterGain = 1.0f - (1.0f - audioLimiterGain) * release;
            }

            std::vector<std::int16_t> pcm(static_cast<std::size_t>(frames) *
                                          kAudioChannels);
            std::array<std::vector<std::int16_t>, 2> sourcePcm{
                std::vector<std::int16_t>(blockSampleCount),
                std::vector<std::int16_t>(blockSampleCount)};
            for (std::size_t i = 0; i < blockSampleCount; ++i) {
                for (std::size_t track = 0; track < sourceMix.size(); ++track) {
                    const float sourceSample = std::clamp(sourceMix[track][i],
                        -32768.0f, 32767.0f);
                    sourcePcm[track][i] = static_cast<std::int16_t>(
                        std::lround(sourceSample));
                }
            }
            if (!writeSourcePcm(0, sourcePcm[0].data(), frames) ||
                !writeSourcePcm(1, sourcePcm[1].data(), frames)) {
                setFailure(L"写入独立音频轨失败");
                return false;
            }
            for (std::size_t i = 0; i < pcm.size(); ++i) {
                const float limited = std::clamp(mix[i] * audioLimiterGain,
                                                  -kLimiterCeiling, kLimiterCeiling);
                pcm[i] = static_cast<std::int16_t>(std::lround(limited));
            }
            if (!writePcm(pcm.data(), frames)) {
                setFailure(L"写入 AAC 音频失败");
                return false;
            }
            {
                std::lock_guard lock(audioControlMutex);
                while (!liveAudio.transitions.empty() &&
                       RelativeSample(liveAudio.transitions.front().first, zeroQpc) <=
                           audioSamplesWritten) {
                    liveAudio.baseMask = liveAudio.transitions.front().second;
                    liveAudio.transitions.pop_front();
                }
            }
        }
        return true;
    };

    try {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            apartmentInitialized = true;
        } catch (const std::exception& error) {
            throw SessionError{L"COM/WinRT 初始化失败：" + Utf8ToWide(error.what())};
        }

        HRESULT hr = ::MFStartup(MF_VERSION, MFSTARTUP_FULL);
        if (FAILED(hr)) {
            throw SessionError{L"Media Foundation 启动失败：" + HrTextWide(hr)};
        }
        mfStarted = true;

        if (stopRequested.load()) {
            throw Cancelled{};
        }

        std::wstring hardwareError;
        if (!OpenWriter(true, writer, outputFile, videoStream, audioStream, encoderInfo,
                        hardwareError)) {
            std::wstring softwareError;
            if (!OpenWriter(false, writer, outputFile, videoStream, audioStream, encoderInfo,
                            softwareError)) {
                throw SessionError{
                    L"H.264/AAC 编码初始化失败。硬件尝试：" + hardwareError +
                    L"；软件尝试：" + softwareError};
            }
            encoderInfo += L"（硬件不可用，已回退软件）";
        }

        const std::filesystem::path outputPath(finalPath);
        const std::wstring stem = outputPath.stem().wstring();
        sourceAudioPaths[0] = (outputPath.parent_path() /
            (stem + L"_system_partial.m4a")).wstring();
        sourceAudioPaths[1] = (outputPath.parent_path() /
            (stem + L"_microphone_partial.m4a")).wstring();
        for (std::size_t i = 0; i < sourceAudioWriters.size(); ++i) {
            std::wstring audioWriterError;
            if (!OpenAudioOnlyWriter(sourceAudioPaths[i], sourceAudioWriters[i],
                                     &audioWriterError)) {
                throw SessionError{L"初始化独立音轨失败：" + audioWriterError};
            }
        }
        encoderInfo += L" + 混合/系统/麦克风三条 AAC 音轨";

        if (stopRequested.load()) {
            throw Cancelled{};
        }

        capture = std::make_shared<ScreenCapture>();
        std::wstring captureError;
        if (!capture->Start(config.monitor, config.fps, &captureError)) {
            throw SessionError{L"屏幕捕获启动失败：" + captureError};
        }
        if (!capture->WaitForFirstFrame(3000, &captureError)) {
            throw SessionError{L"等待首帧失败：" + captureError};
        }
        if (stopRequested.load()) {
            throw Cancelled{};
        }

        zeroQpc = capture->firstFrameQpc100ns();
        if (zeroQpc <= 0) {
            zeroQpc = QpcNow100ns();
        }

        const Size frameSize = capture->frameSize();
        if (frameSize.width <= 0 || frameSize.height <= 0) {
            throw SessionError{L"捕获画面尺寸无效"};
        }
        std::wstring convertError;
        const int monitorWidth = std::max(1, config.monitorPixelSize.width);
        const int monitorHeight = std::max(1, config.monitorPixelSize.height);
        const auto scaleEdge = [](int value, int sourceExtent, int targetExtent) {
            return static_cast<int>((static_cast<std::int64_t>(value) * targetExtent +
                                     sourceExtent / 2) / sourceExtent);
        };
        const int cropLeft = scaleEdge(config.sourceRect.left, monitorWidth,
                                       frameSize.width);
        const int cropTop = scaleEdge(config.sourceRect.top, monitorHeight,
                                      frameSize.height);
        const int cropRight = scaleEdge(config.sourceRect.right, monitorWidth,
                                        frameSize.width);
        const int cropBottom = scaleEdge(config.sourceRect.bottom, monitorHeight,
                                         frameSize.height);
        if (!converter.Configure(frameSize.width, frameSize.height,
                                 cropLeft, cropTop,
                                 cropRight - cropLeft, cropBottom - cropTop,
                                 outputInfo.content.width, outputInfo.content.height,
                                 outputInfo.output.width, outputInfo.output.height,
                                 &convertError)) {
            throw SessionError{L"画面裁剪/缩放配置失败：" + convertError};
        }
        converterReady = true;

        if (config.systemAudio) {
            systemAudioSource.capture = std::make_shared<AudioCapture>(
                AudioSource::SystemLoopback, config.systemAudioDeviceId);
            std::wstring audioError;
            if (!systemAudioSource.capture->Start(&audioError)) {
                systemAudioSource.capture.reset();
                throw SessionError{L"系统声音采集启动失败：" + audioError};
            }
        }
        if (config.microphone) {
            microphoneSource.capture = std::make_shared<AudioCapture>(
                AudioSource::Microphone, config.microphoneDeviceId);
            std::wstring audioError;
            if (!microphoneSource.capture->Start(&audioError)) {
                microphoneSource.capture->Stop();
                microphoneSource.capture->Join();
                microphoneSource.capture.reset();
                if (systemAudioSource.capture != nullptr) {
                    systemAudioSource.capture->Stop();
                    systemAudioSource.capture->Join();
                    systemAudioSource.capture.reset();
                }
                throw SessionError{L"麦克风采集启动失败：" + audioError};
            }
        }
        if (stopRequested.load()) {
            throw Cancelled{};
        }

        {
            std::lock_guard lock(captureMutex);
            liveCapture = capture;
        }
        {
            std::lock_guard lock(audioControlMutex);
            liveAudio.zeroQpc = zeroQpc;
            audioStatus.systemAudio = config.systemAudio;
            audioStatus.microphone = config.microphone;
            ++audioStatus.revision;
        }
        state.store(RecorderState::Recording, std::memory_order_release);
        elapsed100ns.store(0, std::memory_order_relaxed);

        const std::int64_t frameDuration = kTicksPerSecond / config.fps;
        std::int64_t frameIndex = 0;
        std::int64_t lastVideoTimestamp = -1;
        // After a stop request is observed, encode at most one final frame and
        // leave the loop. Without this guard a stop that lands between two
        // frame timestamps would `continue` forever on the same frame index.
        bool finalFrameWritten = false;
        const std::uint64_t stopSpace = StopSpaceThresholdWithAudioTracks(
            MbpsToBitsPerSecond(static_cast<double>(config.bitrateMbps)), 3);
        std::int64_t nextStorageCheck = QpcNow100ns() + 2 * kTicksPerSecond;

        for (;;) {
            processAudioRequest();
            microphoneLevel.store(microphoneSource.capture != nullptr
                    ? microphoneSource.capture->inputLevel() : 0.0f,
                std::memory_order_relaxed);
            const std::int64_t now = QpcNow100ns();
            if (stopElapsed < 0 && now >= nextStorageCheck) {
                nextStorageCheck = now + 2 * kTicksPerSecond;
                std::wstring storageError;
                if (!detail::CheckRecordingStorage(config.outputDirectory, stopSpace,
                                                    &storageError)) {
                    setFailure(storageError + L" 已停止录制并尝试保存已有内容。");
                }
            }
            if (stopRequested.load(std::memory_order_relaxed) && stopElapsed < 0) {
                stopElapsed = std::max<std::int64_t>(0, QpcNow100ns() - zeroQpc);
                state.store(RecorderState::Stopping, std::memory_order_release);
                stopAudio();
            }

            if (capture->HasError()) {
                setFailure(L"屏幕捕获中断：" + capture->Error());
                if (stopElapsed < 0) {
                    stopElapsed = std::max<std::int64_t>(0, QpcNow100ns() - zeroQpc);
                    stopAudio();
                }
            }
            if (systemAudioSource.capture != nullptr) {
                if (systemAudioSource.capture->overflowed()) {
                    setFailure(L"系统声音缓存溢出，已停止录制");
                }
                if (!stopRequested.load() && stopElapsed < 0 &&
                    systemAudioSource.capture->ended()) {
                    const std::wstring audioError = systemAudioSource.capture->error();
                    setFailure(audioError.empty() ? L"系统声音采集意外结束" : audioError);
                }
            }
            if (microphoneSource.capture != nullptr) {
                if (microphoneSource.capture->overflowed()) {
                    setFailure(L"麦克风缓存溢出，已停止录制");
                }
                if (!stopRequested.load() && stopElapsed < 0 &&
                    microphoneSource.capture->ended()) {
                    const std::wstring audioError = microphoneSource.capture->error();
                    setFailure(audioError.empty() ? L"麦克风采集意外结束" : audioError);
                }
            }
            if (failure) {
                if (stopElapsed < 0) {
                    stopElapsed = std::max<std::int64_t>(0, QpcNow100ns() - zeroQpc);
                    stopAudio();
                }
                break;
            }

            const std::int64_t frameTimestamp = frameIndex * kTicksPerSecond / config.fps;
            if (stopElapsed >= 0) {
                if (frameTimestamp >= stopElapsed || finalFrameWritten) {
                    break;
                }
            } else if (stopRequested.load(std::memory_order_relaxed)) {
                // The stop timestamp is established at the top of the loop;
                // this guards against continuing before it becomes visible.
                continue;
            }

            while (QpcNow100ns() - zeroQpc < frameTimestamp) {
                if (stopRequested.load(std::memory_order_relaxed) ||
                    capture->HasError() || audioNeedsAttention()) {
                    break;
                }
                ::Sleep(2);
            }

            const bool stopping = stopElapsed >= 0;
            if (!stopping &&
                (stopRequested.load(std::memory_order_relaxed) ||
                 capture->HasError() || audioNeedsAttention())) {
                continue;
            }

            int width = 0;
            int height = 0;
            std::int64_t frameQpc = 0;
            if (capture->CopyLatestFrame(latestBgra, width, height, frameQpc)) {
                if (width <= 0 || height <= 0 || !converterReady) {
                    setFailure(L"捕获画面尺寸无效");
                    break;
                }
                converter.Convert(latestBgra.data(), width * 4, nv12);
                haveFrame = true;
            }
            if (!haveFrame) {
                if (stopping) {
                    break;
                }
                ::Sleep(2);
                continue;
            }

            // Use the capture timestamp when the encoder is behind so the
            // video does not pack all encoded frames into the first seconds
            // and hold the last frame for the remainder of the recording.
            const std::int64_t captureTimestamp =
                frameQpc > 0 ? std::max<std::int64_t>(0, frameQpc - zeroQpc)
                             : frameTimestamp;
            std::int64_t sampleTimestamp = std::max(frameTimestamp, captureTimestamp);
            sampleTimestamp = std::max(sampleTimestamp, lastVideoTimestamp + 1);
            if (stopping) {
                sampleTimestamp = std::max(sampleTimestamp, stopElapsed);
            }
            if (!writeVideoSample(sampleTimestamp, frameDuration)) {
                setFailure(L"写入 H.264 视频帧失败");
                break;
            }
            lastVideoTimestamp = sampleTimestamp;
            ++videoFrames;
            if (stopping) {
                finalFrameWritten = true;
            }

            const bool drainOk = drainAudio(stopElapsed >= 0
                               ? stopElapsed * kAudioRate / kTicksPerSecond
                               : -1);
            if (!drainOk) {
                break;
            }

            elapsed100ns.store(std::max<std::int64_t>(0, QpcNow100ns() - zeroQpc),
                               std::memory_order_relaxed);
            ++frameIndex;
        }

        if (!failure) {
            cleanStop = true;
        }
        if (stopElapsed < 0) {
            stopElapsed = std::max<std::int64_t>(0, QpcNow100ns() - zeroQpc);
        }
        elapsed100ns.store(stopElapsed, std::memory_order_relaxed);

        cancelPendingAudioOpen();
        stopAudio();
        joinAudio();
        if (!failure) {
            if (systemAudioSource.capture != nullptr) {
                if (systemAudioSource.capture->overflowed()) {
                    setFailure(L"系统声音缓存溢出，已停止录制");
                } else if (!systemAudioSource.capture->error().empty()) {
                    setFailure(systemAudioSource.capture->error());
                }
            }
            if (microphoneSource.capture != nullptr) {
                if (microphoneSource.capture->overflowed()) {
                    setFailure(L"麦克风缓存溢出，已停止录制");
                } else if (!microphoneSource.capture->error().empty()) {
                    setFailure(microphoneSource.capture->error());
                }
            }
        }
        if (capture != nullptr) {
            capture->Stop();
        }

        if (writer != nullptr && videoFrames > 0) {
            state.store(RecorderState::Saving, std::memory_order_release);
            if (!drainAudio(stopElapsed * kAudioRate / kTicksPerSecond)) {
                setFailure(L"无法完成音频收尾");
            }
            hr = writer->Finalize();
            if (FAILED(hr)) {
                setFailure(L"MP4 收尾失败：" + HrTextWide(hr));
            }
            writer.Reset();
            if (outputFile != nullptr) {
                outputFile->Close();
                outputFile.Reset();
            }
            for (AudioOnlyWriter& track : sourceAudioWriters) {
                if (track.writer != nullptr) {
                    hr = track.writer->Finalize();
                    if (FAILED(hr)) {
                        setFailure(L"独立 AAC 音轨收尾失败：" + HrTextWide(hr));
                    }
                    track.writer.Reset();
                }
                if (track.outputFile != nullptr) {
                    track.outputFile->Close();
                    track.outputFile.Reset();
                }
            }
            if (!failure) {
                std::wstring muxError;
                if (!MuxMp4WithAudioTracks(partialPath, sourceAudioPaths[0],
                        sourceAudioPaths[1], finalPath, &muxError)) {
                    setFailure(L"多音轨 MP4 封装失败：" + muxError);
                } else {
                    ::DeleteFileW(partialPath.c_str());
                }
            }
        } else {
            writer.Reset();
            if (outputFile != nullptr) {
                outputFile->Close();
                outputFile.Reset();
            }
            if (!partialPath.empty()) {
                ::DeleteFileW(partialPath.c_str());
            }
            if (videoFrames == 0 && !failure) {
                setFailure(L"没有录制到有效画面");
            }
        }
    } catch (const Cancelled&) {
        setFailure(L"录制已取消");
    } catch (const SessionError& error) {
        setFailure(error.message);
    } catch (const winrt::hresult_error& error) {
        setFailure(L"WinRT 异常：" + HrTextWide(error.code()));
    } catch (const std::exception& error) {
        setFailure(Utf8ToWide(error.what()));
    }

    cancelPendingAudioOpen();
    stopAudio();
    joinAudio();
    {
        std::lock_guard lock(audioControlMutex);
        if (audioStatus.pending) {
            audioStatus.error = L"录制已结束，未更改声音来源";
        }
        audioStatus.systemAudio = false;
        audioStatus.microphone = false;
        audioStatus.pending = false;
        ++audioStatus.revision;
    }
    if (capture != nullptr) {
        capture->Stop();
    }
    {
        std::lock_guard lock(captureMutex);
        liveCapture.reset();
    }
    capture.reset();
    if (writer != nullptr) {
        writer.Reset();
    }
    if (outputFile != nullptr) {
        outputFile->Close();
        outputFile.Reset();
    }
    for (AudioOnlyWriter& track : sourceAudioWriters) {
        track.writer.Reset();
        if (track.outputFile != nullptr) {
            track.outputFile->Close();
            track.outputFile.Reset();
        }
        if (!track.path.empty()) ::DeleteFileW(track.path.c_str());
    }

    bool partialRemains = false;
    if (videoFrames > 0 && FileExists(partialPath)) {
        partialRemains = true;
    }
    if (failure && !partialRemains) {
        ::DeleteFileW(partialPath.c_str());
    }

    RecorderResult finalResult;
    finalResult.success = !failure && videoFrames > 0;
    finalResult.finalPath = finalResult.success ? finalPath : std::wstring();
    finalResult.partialPath = partialRemains ? partialPath : std::wstring();
    finalResult.error = failure ? failureMessage : std::wstring();
    finalResult.encoderInfo = encoderInfo;
    finalResult.outputSize = outputInfo.output;
    finalResult.videoFrames = videoFrames;
    finalResult.audioChunks = audioChunks;
    finalResult.durationSeconds =
        static_cast<double>(std::max<std::int64_t>(0, stopElapsed)) / kTicksPerSecond;
    finalResult.partialRemains = partialRemains;

    {
        std::lock_guard lock(resultMutex);
        result = std::move(finalResult);
        resultReady = true;
    }
    state.store(failure ? RecorderState::Failed : RecorderState::Finished,
                std::memory_order_release);

    if (mfStarted) {
        ::MFShutdown();
    }
    if (apartmentInitialized) {
        winrt::uninit_apartment();
    }
}

RecordingSession::RecordingSession() : impl_(std::make_unique<Impl>()) {}

RecordingSession::~RecordingSession() {
    Stop();
    Wait();
}

bool RecordingSession::Start(const RecordingConfig& config, std::wstring* error) {
    if (impl_ == nullptr) {
        return false;
    }
    if (impl_->state.load() != RecorderState::Idle) {
        if (error != nullptr) {
            *error = L"录制会话已在运行";
        }
        return false;
    }
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }

    impl_->config = config;
    {
        std::lock_guard lock(impl_->audioControlMutex);
        impl_->liveAudio = {};
        impl_->liveAudio.currentMask = (config.systemAudio ? 1U : 0U) |
                                      (config.microphone ? 2U : 0U);
        impl_->liveAudio.requestedMask = impl_->liveAudio.currentMask;
        impl_->liveAudio.baseMask = impl_->liveAudio.currentMask;
        impl_->audioStatus = {};
    }
    impl_->result = {};
    impl_->resultReady = false;
    impl_->stopRequested.store(false);
    impl_->elapsed100ns.store(0);
    impl_->finalPath.clear();
    impl_->partialPath.clear();
    impl_->outputInfo = {};

    std::wstring prepareError;
    if (!impl_->Prepare(&prepareError)) {
        if (error != nullptr) {
            *error = prepareError;
        }
        return false;
    }

    impl_->state.store(RecorderState::Starting, std::memory_order_release);
    try {
        impl_->worker = std::thread([impl = impl_.get()] { impl->Run(); });
    } catch (const std::exception& ex) {
        impl_->state.store(RecorderState::Failed, std::memory_order_release);
        if (error != nullptr) {
            *error = Utf8ToWide(ex.what());
        }
        return false;
    }
    return true;
}

void RecordingSession::Stop() {
    if (impl_ == nullptr) {
        return;
    }
    const RecorderState current = impl_->state.load();
    if (current == RecorderState::Starting || current == RecorderState::Recording ||
        current == RecorderState::Stopping) {
        impl_->stopRequested.store(true);
    }
}

void RecordingSession::Wait() {
    if (impl_ != nullptr && impl_->worker.joinable()) {
        impl_->worker.join();
    }
}

bool RecordingSession::CopyScreenshot(ScreenshotFrame& frame, std::wstring* error) const {
    frame = {};
    if (impl_ == nullptr ||
        impl_->state.load(std::memory_order_acquire) != RecorderState::Recording ||
        impl_->stopRequested.load(std::memory_order_relaxed)) {
        if (error != nullptr) {
            *error = L"仅在录制中可以截取当前画面";
        }
        return false;
    }
    std::shared_ptr<ScreenCapture> capture;
    {
        std::lock_guard lock(impl_->captureMutex);
        capture = impl_->liveCapture;
    }
    if (capture == nullptr) {
        if (error != nullptr) {
            *error = L"当前没有有效的截图画面";
        }
        return false;
    }
    const Size frameSize = capture->frameSize();
    const Size monitorSize = impl_->config.monitorPixelSize;
    if (frameSize.width <= 0 || frameSize.height <= 0 ||
        monitorSize.width <= 0 || monitorSize.height <= 0) {
        if (error != nullptr) *error = L"当前捕获画面尺寸无效";
        return false;
    }
    const auto scaleEdge = [](int value, int sourceExtent, int targetExtent) {
        return static_cast<int>((static_cast<std::int64_t>(value) * targetExtent +
                                 sourceExtent / 2) / sourceExtent);
    };
    const RECT source{
        scaleEdge(impl_->config.sourceRect.left, monitorSize.width, frameSize.width),
        scaleEdge(impl_->config.sourceRect.top, monitorSize.height, frameSize.height),
        scaleEdge(impl_->config.sourceRect.right, monitorSize.width, frameSize.width),
        scaleEdge(impl_->config.sourceRect.bottom, monitorSize.height, frameSize.height),
    };
    return capture->CopySnapshot(source, frame, error);
}

bool RecordingSession::SetAudioEnabled(bool systemAudio, bool microphone) {
    if (impl_ == nullptr ||
        impl_->state.load(std::memory_order_acquire) != RecorderState::Recording ||
        impl_->stopRequested.load(std::memory_order_relaxed)) {
        return false;
    }
    const auto now = QpcNow100ns();
    std::lock_guard lock(impl_->audioControlMutex);
    if (impl_->state.load(std::memory_order_acquire) != RecorderState::Recording ||
        impl_->stopRequested.load(std::memory_order_relaxed) || impl_->audioStatus.pending) {
        return false;
    }
    const unsigned mask = (systemAudio ? 1U : 0U) | (microphone ? 2U : 0U);
    if (mask == impl_->liveAudio.currentMask) {
        return true;
    }
    const unsigned changed = mask ^ impl_->liveAudio.currentMask;
    const std::size_t transitionsNeeded = ((changed & 1U) != 0 ? 1U : 0U) +
                                          ((changed & 2U) != 0 ? 1U : 0U);
    if (now <= 0 || impl_->liveAudio.zeroQpc <= 0 ||
        RelativeSample(now, impl_->liveAudio.zeroQpc) < impl_->liveAudio.mixedThroughSample ||
        impl_->liveAudio.transitions.size() + transitionsNeeded > 64) {
        return false;
    }
    impl_->liveAudio.requestedMask = mask;
    impl_->liveAudio.requestQpc = now;
    impl_->audioStatus.pending = true;
    impl_->audioStatus.error.clear();
    ++impl_->audioStatus.revision;
    return true;
}

RecordingAudioState RecordingSession::audioState() const {
    if (impl_ == nullptr) {
        return {};
    }
    std::lock_guard lock(impl_->audioControlMutex);
    return impl_->audioStatus;
}

float RecordingSession::microphoneLevel() const noexcept {
    return impl_ != nullptr ? impl_->microphoneLevel.load(std::memory_order_relaxed) : 0.0f;
}

RecorderState RecordingSession::state() const noexcept {
    return impl_ != nullptr ? impl_->state.load(std::memory_order_acquire)
                            : RecorderState::Idle;
}

std::int64_t RecordingSession::elapsed100ns() const noexcept {
    return impl_ != nullptr ? impl_->elapsed100ns.load(std::memory_order_relaxed) : 0;
}

bool RecordingSession::TakeResult(RecorderResult* result) {
    if (impl_ == nullptr || result == nullptr) {
        return false;
    }
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    std::lock_guard lock(impl_->resultMutex);
    if (!impl_->resultReady) {
        return false;
    }
    *result = impl_->result;
    impl_->resultReady = false;
    return true;
}

}  // namespace srl
