#include "media/mux_probe.h"

#include <codecapi.h>
#include <icodecapi.h>
#include <mfapi.h>
#include <propvarutil.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <vector>

#include "common/win32.h"
#include "core/recording_format.h"
#include "media/mf_util.h"

namespace srl {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::uint32_t kAudioChunkMilliseconds = 10;
constexpr std::uint32_t kAudioChunkFrames = kAudioOutputRate * kAudioChunkMilliseconds / 1'000;
constexpr std::int64_t kAudioChunkDuration =
    kAudioChunkMilliseconds * kHundredNanosecondsPerMillisecond;

std::wstring HrTextWide(HRESULT hr) {
    return Utf8ToWide(HResultText(hr));
}

ComPtr<IMFMediaType> CreateVideoTargetType(const MuxProbeOptions& options) {
    ComPtr<IMFMediaType> type;
    if (FAILED(::MFCreateMediaType(type.GetAddressOf()))) {
        return {};
    }
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    type->SetUINT32(MF_MT_AVG_BITRATE,
                    static_cast<std::uint32_t>(options.bitrateMbps) * 1'000'000U);
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    ::MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE,
                         static_cast<UINT32>(options.width),
                         static_cast<UINT32>(options.height));
    ::MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE,
                          static_cast<UINT32>(options.fps), 1);
    ::MFSetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    type->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    type->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    type->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    type->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    return type;
}

ComPtr<IMFMediaType> CreateVideoInputType(const MuxProbeOptions& options) {
    ComPtr<IMFMediaType> type;
    if (FAILED(::MFCreateMediaType(type.GetAddressOf()))) {
        return {};
    }
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    ::MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE,
                         static_cast<UINT32>(options.width),
                         static_cast<UINT32>(options.height));
    ::MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE,
                          static_cast<UINT32>(options.fps), 1);
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
    type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kAudioOutputRate);
    type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kAudioOutputChannels);
    type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, kAudioPcmBitsPerSample);
    type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kAudioEncodedBytesPerSecond);
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
    type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kAudioOutputRate);
    type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, kAudioOutputChannels);
    type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, kAudioPcmBitsPerSample);
    type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT,
                    kAudioOutputChannels * kAudioPcmBytesPerSample);
    type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                    kAudioOutputRate * kAudioOutputChannels * kAudioPcmBytesPerSample);
    return type;
}

ComPtr<IMFSample> CreateSampleFromBytes(const std::uint8_t* data, std::size_t size,
                                        LONGLONG timestamp, LONGLONG duration) {
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(::MFCreateMemoryBuffer(static_cast<DWORD>(size), buffer.GetAddressOf()))) {
        return {};
    }
    BYTE* destination = nullptr;
    if (FAILED(buffer->Lock(&destination, nullptr, nullptr))) {
        return {};
    }
    std::memcpy(destination, data, size);
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(size));

    ComPtr<IMFSample> sample;
    if (FAILED(::MFCreateSample(sample.GetAddressOf()))) {
        return {};
    }
    if (FAILED(sample->AddBuffer(buffer.Get()))) {
        return {};
    }
    sample->SetSampleTime(timestamp);
    sample->SetSampleDuration(duration);
    return sample;
}

void FillNv12Frame(std::vector<std::uint8_t>& frame, int width, int height, int frameIndex) {
    static constexpr std::array<std::uint8_t, 8> kY = {16, 40, 64, 96, 128, 160, 192, 220};
    static constexpr std::array<std::uint8_t, 8> kU = {128, 110, 120, 90, 128, 150, 160, 128};
    static constexpr std::array<std::uint8_t, 8> kV = {128, 150, 120, 160, 128, 90, 110, 128};

    const int shift = frameIndex * 4;
    std::uint8_t* yPlane = frame.data();
    std::uint8_t* uvPlane = frame.data() + static_cast<std::size_t>(width) * height;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int bar = (((x + shift) * 8) / width) % 8;
            yPlane[static_cast<std::size_t>(y) * width + x] = kY[bar];
        }
    }

    // A moving white block keeps the encode workload non-static.
    const int blockX = (frameIndex * 8) % std::max(1, width - 160);
    const int blockY = height / 2 - 40;
    for (int y = blockY; y < blockY + 80 && y < height; ++y) {
        for (int x = blockX; x < blockX + 160 && x < width; ++x) {
            yPlane[static_cast<std::size_t>(y) * width + x] = 235;
        }
    }

    for (int y = 0; y < height; y += 2) {
        for (int x = 0; x < width; x += 2) {
            const int bar = (((x + shift) * 8) / width) % 8;
            const std::size_t offset =
                static_cast<std::size_t>(y / 2) * width + static_cast<std::size_t>(x);
            uvPlane[offset] = kU[bar];
            uvPlane[offset + 1] = kV[bar];
        }
    }
}

void FillPcmChunk(std::vector<std::uint8_t>& chunk, std::uint32_t chunkIndex) {
    auto* samples = reinterpret_cast<std::int16_t*>(chunk.data());
    const std::uint32_t frames = kAudioChunkFrames;
    for (std::uint32_t i = 0; i < frames; ++i) {
        const double time =
            static_cast<double>(chunkIndex * frames + i) / kAudioOutputRate;
        const auto value = static_cast<std::int16_t>(
            std::sin(2.0 * 3.14159265358979323846 * 440.0 * time) * 0.2 * 32767.0);
        samples[i * 2] = value;
        samples[i * 2 + 1] = value;
    }
}

std::wstring ReadCodecValue(ICodecAPI* codec, const GUID& key) {
    if (codec == nullptr) {
        return L"(no ICodecAPI)";
    }
    VARIANT value{};
    ::VariantInit(&value);
    std::wstring result;
    if (FAILED(codec->GetValue(&key, &value))) {
        result = L"n/a";
    } else if (value.vt == VT_UI4) {
        result = std::to_wstring(value.ulVal);
    } else if (value.vt == VT_I4) {
        result = std::to_wstring(value.lVal);
    } else if (value.vt == VT_UI8) {
        result = std::to_wstring(value.ullVal);
    } else {
        result = L"(unsupported)";
    }
    ::VariantClear(&value);
    return result;
}

bool SetCodecValue(ICodecAPI* codec, const GUID& key, std::uint32_t value) {
    if (codec == nullptr) {
        return false;
    }
    VARIANT var{};
    ::VariantInit(&var);
    var.vt = VT_UI4;
    var.ulVal = value;
    const HRESULT hr = codec->SetValue(&key, &var);
    ::VariantClear(&var);
    return SUCCEEDED(hr);
}

bool TrySetRateControl(IMFSinkWriter* writer, DWORD videoStream, int fps, int bitrateMbps) {
    ComPtr<IMFSinkWriterEx> writerEx;
    if (FAILED(writer->QueryInterface(IID_PPV_ARGS(writerEx.GetAddressOf())))) {
        return false;
    }
    bool applied = false;
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
        ComPtr<ICodecAPI> codec;
        if (FAILED(transform.As(&codec))) {
            continue;
        }
        const std::uint32_t gop = static_cast<std::uint32_t>(std::max(1, fps * 2));
        const std::uint32_t bitrate =
            static_cast<std::uint32_t>(bitrateMbps) * 1'000'000U;
        bool set = SetCodecValue(codec.Get(), CODECAPI_AVEncCommonRateControlMode,
                                 static_cast<std::uint32_t>(
                                     eAVEncCommonRateControlMode_UnconstrainedVBR));
        set = SetCodecValue(codec.Get(), CODECAPI_AVEncCommonMeanBitRate, bitrate) || set;
        set = SetCodecValue(codec.Get(), CODECAPI_AVEncMPVGOPSize, gop) || set;
        applied = applied || set;
    }
    return applied;
}

void CollectVideoTransforms(IMFSinkWriter* writer, DWORD videoStream, MuxProbeResult& result) {
    ComPtr<IMFSinkWriterEx> writerEx;
    if (FAILED(writer->QueryInterface(IID_PPV_ARGS(writerEx.GetAddressOf())))) {
        return;
    }
    for (DWORD index = 0; index < 8; ++index) {
        GUID category{};
        ComPtr<IMFTransform> transform;
        if (FAILED(writerEx->GetTransformForStream(videoStream, index, &category,
                                                    transform.GetAddressOf()))) {
            break;
        }
        TransformEvidence evidence;
        evidence.index = index;
        evidence.category = Utf8ToWide(MftCategoryName(category));
        evidence.friendlyName = TransformFriendlyName(transform.Get());
        evidence.d3d11Aware = TransformSupportsD3D11(transform.Get());
        evidence.async = TransformIsAsync(transform.Get());

        ComPtr<ICodecAPI> codec;
        if (SUCCEEDED(transform.As(&codec))) {
            evidence.rateControlReadback =
                ReadCodecValue(codec.Get(), CODECAPI_AVEncCommonRateControlMode);
            evidence.meanBitrateReadback =
                ReadCodecValue(codec.Get(), CODECAPI_AVEncCommonMeanBitRate);
            evidence.gopSizeReadback =
                ReadCodecValue(codec.Get(), CODECAPI_AVEncMPVGOPSize);
        }

        ComPtr<IMFMediaType> currentOutput;
        if (SUCCEEDED(transform->GetOutputCurrentType(0, currentOutput.GetAddressOf()))) {
            evidence.currentOutputType = MediaTypeSummary(currentOutput.Get());
        }

        if (::IsEqualGUID(category, MFT_CATEGORY_VIDEO_ENCODER)) {
            result.videoEncoderFriendlyName = evidence.friendlyName.empty()
                                                  ? evidence.category
                                                  : evidence.friendlyName;
            if (!evidence.currentOutputType.empty()) {
                result.actualVideoType = evidence.currentOutputType;
            }
        }
        result.transforms.push_back(std::move(evidence));
    }
}

bool ValidateFile(MuxProbeResult& result, const MuxProbeOptions& options) {
    ComPtr<IMFSourceReader> reader;
    HRESULT hr = ::MFCreateSourceReaderFromURL(result.outputPath.c_str(), nullptr,
                                               reader.GetAddressOf());
    if (FAILED(hr)) {
        result.validationError =
            Utf8ToWide(std::format("MFCreateSourceReaderFromURL failed: {}", HResultText(hr)));
        return false;
    }

    PROPVARIANT duration{};
    ::PropVariantInit(&duration);
    if (SUCCEEDED(reader->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE),
                                                    MF_PD_DURATION, &duration)) &&
        duration.vt == VT_UI8) {
        result.containerDurationSeconds =
            static_cast<double>(duration.uhVal.QuadPart) / kHundredNanosecondsPerSecond;
    }
    ::PropVariantClear(&duration);

    reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);

    hr = reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
    if (FAILED(hr)) {
        result.validationError = L"no video stream in the produced MP4";
        return false;
    }
    ComPtr<IMFMediaType> nativeVideo;
    if (SUCCEEDED(reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0,
                                              nativeVideo.GetAddressOf()))) {
        result.actualVideoType = MediaTypeSummary(nativeVideo.Get());
    }

    for (;;) {
        DWORD actualStream = 0;
        DWORD flags = 0;
        LONGLONG timestamp = 0;
        ComPtr<IMFSample> sample;
        hr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0, &actualStream, &flags,
                                &timestamp, sample.GetAddressOf());
        if (FAILED(hr)) {
            result.validationError = Utf8ToWide(
                std::format("video ReadSample failed: {}", HResultText(hr)));
            return false;
        }
        if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
            break;
        }
        if ((flags & MF_SOURCE_READERF_STREAMTICK) != 0) {
            continue;
        }
        if (sample != nullptr) {
            ++result.videoSamplesRead;
            result.lastVideoTimestampSeconds = std::max(
                result.lastVideoTimestampSeconds,
                static_cast<double>(timestamp) / kHundredNanosecondsPerSecond);
        }
    }

    if (options.includeAudio) {
        // The first reader has already reached EOS on the video stream. Read
        // the audio stream with a fresh reader so the EOS state cannot make the
        // audio pass return immediately.
        reader.Reset();
        hr = ::MFCreateSourceReaderFromURL(result.outputPath.c_str(), nullptr,
                                           reader.GetAddressOf());
        if (FAILED(hr)) {
            result.validationError = Utf8ToWide(std::format(
                "audio source reader creation failed: {}", HResultText(hr)));
            return false;
        }
        reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        hr = reader->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), TRUE);
        if (FAILED(hr)) {
            result.validationError = L"no audio stream in the produced MP4";
            return false;
        }
        ComPtr<IMFMediaType> nativeAudio;
        if (SUCCEEDED(reader->GetNativeMediaType(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0,
                                                  nativeAudio.GetAddressOf()))) {
            result.actualAudioType = MediaTypeSummary(nativeAudio.Get());
        }
        for (;;) {
            DWORD actualStream = 0;
            DWORD flags = 0;
            LONGLONG timestamp = 0;
            ComPtr<IMFSample> sample;
            hr = reader->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_FIRST_AUDIO_STREAM), 0, &actualStream,
                                    &flags, &timestamp, sample.GetAddressOf());
            if (FAILED(hr)) {
                result.validationError = Utf8ToWide(
                    std::format("audio ReadSample failed: {}", HResultText(hr)));
                return false;
            }
            if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) != 0) {
                break;
            }
            if ((flags & MF_SOURCE_READERF_STREAMTICK) != 0) {
                continue;
            }
            if (sample != nullptr) {
                ++result.audioSamplesRead;
                result.lastAudioTimestampSeconds = std::max(
                    result.lastAudioTimestampSeconds,
                    static_cast<double>(timestamp) / kHundredNanosecondsPerSecond);
            }
        }
    }

    const int expectedFrames = options.seconds * options.fps;
    if (std::abs(result.videoSamplesRead - expectedFrames) > 2) {
        result.validationError = Utf8ToWide(std::format(
            "decoded {} video frames, expected {}", result.videoSamplesRead, expectedFrames));
        return false;
    }
    if (options.includeAudio && result.audioSamplesRead == 0) {
        result.validationError = L"no audio samples decoded";
        return false;
    }
    if (result.containerDurationSeconds > 0.0 &&
        std::abs(result.containerDurationSeconds - options.seconds) > 0.15) {
        result.validationError = Utf8ToWide(std::format(
            "container duration {:.3f}s differs from requested {}s",
            result.containerDurationSeconds, options.seconds));
        return false;
    }
    if (!result.actualVideoType.empty() &&
        result.actualVideoType.find(L"H.264") == std::wstring::npos) {
        result.validationError = L"video stream is not H.264";
        return false;
    }
    if (options.includeAudio && !result.actualAudioType.empty() &&
        result.actualAudioType.find(L"AAC") == std::wstring::npos) {
        result.validationError = L"audio stream is not AAC";
        return false;
    }
    return true;
}

}  // namespace

MuxProbeResult RunMuxProbe(const MuxProbeOptions& options) {
    MuxProbeResult result;
    result.outputPath = options.outputPath;
    result.requestedSummary = Utf8ToWide(std::format(
        "{}x{} @ {} fps, H.264 target {} Mbps{}; {} s; hardware transforms {}",
        options.width, options.height, options.fps, options.bitrateMbps,
        options.includeAudio ? " + AAC-LC 128 kbps" : " (no audio)", options.seconds,
        options.hardwareTransforms ? "enabled" : "disabled"));

    if (options.outputPath.empty()) {
        result.error = L"output path is empty";
        return result;
    }
    if (options.width < 64 || options.height < 64 || options.width > 4096 ||
        options.height > 4096 || (options.width % 2) != 0 || (options.height % 2) != 0) {
        result.error = L"probe dimensions must be even and within 64..4096";
        return result;
    }
    if (options.fps <= 0 || options.seconds <= 0 || options.bitrateMbps <= 0) {
        result.error = L"fps, seconds and bitrate must be positive";
        return result;
    }

    const std::filesystem::path outputPath(options.outputPath);
    if (outputPath.has_parent_path()) {
        std::string directoryError;
        if (!EnsureDirectory(outputPath.parent_path(), &directoryError)) {
            result.error = Utf8ToWide(directoryError);
            return result;
        }
    }

    ComPtr<IMFAttributes> attributes;
    HRESULT hr = ::MFCreateAttributes(attributes.GetAddressOf(), 1);
    if (FAILED(hr)) {
        result.error = HrTextWide(hr);
        return result;
    }
    attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS,
                          options.hardwareTransforms ? TRUE : FALSE);

    ComPtr<IMFSinkWriter> writer;
    hr = ::MFCreateSinkWriterFromURL(options.outputPath.c_str(), nullptr, attributes.Get(),
                                     writer.GetAddressOf());
    if (FAILED(hr)) {
        result.error =
            Utf8ToWide(std::format("MFCreateSinkWriterFromURL failed: {}", HResultText(hr)));
        return result;
    }

    const ComPtr<IMFMediaType> videoTarget = CreateVideoTargetType(options);
    const ComPtr<IMFMediaType> videoInput = CreateVideoInputType(options);
    const ComPtr<IMFMediaType> audioTarget = CreateAudioTargetType();
    const ComPtr<IMFMediaType> audioInput = CreateAudioInputType();
    if (videoTarget == nullptr || videoInput == nullptr || audioTarget == nullptr ||
        audioInput == nullptr) {
        result.error = L"failed to create media types";
        return result;
    }

    DWORD videoStream = 0;
    hr = writer->AddStream(videoTarget.Get(), &videoStream);
    if (FAILED(hr)) {
        result.error =
            Utf8ToWide(std::format("AddStream(video) failed: {}", HResultText(hr)));
        return result;
    }
    DWORD audioStream = 0;
    if (options.includeAudio) {
        hr = writer->AddStream(audioTarget.Get(), &audioStream);
        if (FAILED(hr)) {
            result.error =
                Utf8ToWide(std::format("AddStream(audio) failed: {}", HResultText(hr)));
            return result;
        }
    }

    hr = writer->SetInputMediaType(videoStream, videoInput.Get(), nullptr);
    if (FAILED(hr)) {
        result.error =
            Utf8ToWide(std::format("SetInputMediaType(video) failed: {}", HResultText(hr)));
        return result;
    }
    if (options.includeAudio) {
        hr = writer->SetInputMediaType(audioStream, audioInput.Get(), nullptr);
        if (FAILED(hr)) {
            result.error =
                Utf8ToWide(std::format("SetInputMediaType(audio) failed: {}", HResultText(hr)));
            return result;
        }
    }

    result.rateControlAppliedBeforeBegin =
        TrySetRateControl(writer.Get(), videoStream, options.fps, options.bitrateMbps);

    hr = writer->BeginWriting();
    if (FAILED(hr)) {
        result.error = Utf8ToWide(std::format("BeginWriting failed: {}", HResultText(hr)));
        return result;
    }
    if (!result.rateControlAppliedBeforeBegin) {
        result.rateControlAppliedAfterBegin =
            TrySetRateControl(writer.Get(), videoStream, options.fps, options.bitrateMbps);
    }
    CollectVideoTransforms(writer.Get(), videoStream, result);

    const int width = options.width;
    const int height = options.height;
    const std::size_t frameBytes = static_cast<std::size_t>(width) * height * 3 / 2;
    std::vector<std::uint8_t> frame(frameBytes);
    const LONGLONG videoDuration = kHundredNanosecondsPerSecond / options.fps;
    const int totalFrames = options.seconds * options.fps;
    const int totalAudioChunks =
        options.includeAudio ? options.seconds * 1000 / 10 : 0;
    std::vector<std::uint8_t> audioChunk(kAudioChunkFrames * kAudioOutputChannels *
                                         kAudioPcmBytesPerSample);

    int audioChunksWritten = 0;
    for (int frameIndex = 0; frameIndex < totalFrames; ++frameIndex) {
        FillNv12Frame(frame, width, height, frameIndex);
        const LONGLONG timestamp = static_cast<LONGLONG>(frameIndex) * videoDuration;
        const ComPtr<IMFSample> videoSample =
            CreateSampleFromBytes(frame.data(), frame.size(), timestamp, videoDuration);
        if (videoSample == nullptr) {
            result.error = L"failed to allocate a video sample";
            return result;
        }
        hr = writer->WriteSample(videoStream, videoSample.Get());
        if (FAILED(hr)) {
            result.error = Utf8ToWide(
                std::format("WriteSample(video, frame {}) failed: {}", frameIndex,
                            HResultText(hr)));
            return result;
        }
        ++result.videoFramesWritten;

        if (options.includeAudio) {
            const LONGLONG videoEnd = timestamp + videoDuration;
            while (audioChunksWritten < totalAudioChunks &&
                   static_cast<LONGLONG>(audioChunksWritten + 1) * kAudioChunkDuration <=
                       videoEnd) {
                FillPcmChunk(audioChunk, static_cast<std::uint32_t>(audioChunksWritten));
                const LONGLONG audioTimestamp =
                    static_cast<LONGLONG>(audioChunksWritten) * kAudioChunkDuration;
                const ComPtr<IMFSample> audioSample = CreateSampleFromBytes(
                    audioChunk.data(), audioChunk.size(), audioTimestamp, kAudioChunkDuration);
                if (audioSample == nullptr) {
                    result.error = L"failed to allocate an audio sample";
                    return result;
                }
                hr = writer->WriteSample(audioStream, audioSample.Get());
                if (FAILED(hr)) {
                    result.error = Utf8ToWide(std::format(
                        "WriteSample(audio, chunk {}) failed: {}", audioChunksWritten,
                        HResultText(hr)));
                    return result;
                }
                ++audioChunksWritten;
            }
        }
    }

    if (options.includeAudio) {
        while (audioChunksWritten < totalAudioChunks) {
            FillPcmChunk(audioChunk, static_cast<std::uint32_t>(audioChunksWritten));
            const LONGLONG audioTimestamp =
                static_cast<LONGLONG>(audioChunksWritten) * kAudioChunkDuration;
            const ComPtr<IMFSample> audioSample = CreateSampleFromBytes(
                audioChunk.data(), audioChunk.size(), audioTimestamp, kAudioChunkDuration);
            if (audioSample == nullptr) {
                result.error = L"failed to allocate an audio sample";
                return result;
            }
            hr = writer->WriteSample(audioStream, audioSample.Get());
            if (FAILED(hr)) {
                result.error = Utf8ToWide(std::format(
                    "WriteSample(audio, trailing chunk {}) failed: {}", audioChunksWritten,
                    HResultText(hr)));
                return result;
            }
            ++audioChunksWritten;
        }
    }
    result.audioChunksWritten = audioChunksWritten;

    hr = writer->Finalize();
    if (FAILED(hr)) {
        result.error = Utf8ToWide(std::format("Finalize failed: {}", HResultText(hr)));
        return result;
    }
    result.finalized = true;
    writer.Reset();

    std::error_code sizeError;
    const auto fileSize = std::filesystem::file_size(outputPath, sizeError);
    if (!sizeError) {
        result.fileSize = fileSize;
    }

    result.validated = ValidateFile(result, options);
    result.ok = result.validated;
    return result;
}

void PrintMuxProbe(const MuxProbeResult& result) {
    ConsoleLine("MP4 pipeline");
    ConsoleLine(std::format("  output             {}", WideToUtf8(result.outputPath)));
    ConsoleLine(std::format("  request            {}", WideToUtf8(result.requestedSummary)));
    if (!result.error.empty()) {
        ConsoleLine(std::format("  error              {}", WideToUtf8(result.error)));
    }
    if (!result.transforms.empty()) {
        ConsoleLine(std::format("  sink writer transforms {}", result.transforms.size()));
        for (const TransformEvidence& transform : result.transforms) {
            ConsoleLine(std::format(
                "    #{} {} - {}{}", transform.index, WideToUtf8(transform.category),
                WideToUtf8(transform.friendlyName.empty() ? L"<unnamed>" : transform.friendlyName),
                transform.d3d11Aware ? " [D3D11-aware]" : ""));
            if (!transform.currentOutputType.empty()) {
                ConsoleLine(std::format("        output {}", WideToUtf8(transform.currentOutputType)));
            }
            if (!transform.rateControlReadback.empty()) {
                ConsoleLine(std::format(
                    "        codecapi rateControl={} meanBitrate={} gop={}",
                    WideToUtf8(transform.rateControlReadback),
                    WideToUtf8(transform.meanBitrateReadback),
                    WideToUtf8(transform.gopSizeReadback)));
            }
        }
    }
    if (!result.videoEncoderFriendlyName.empty()) {
        ConsoleLine(std::format("  actual V encoder  {}",
                                WideToUtf8(result.videoEncoderFriendlyName)));
    } else {
        ConsoleLine("  actual V encoder  (not exposed by sink writer)");
    }
    ConsoleLine(std::format("  rate control      before BeginWriting={}, after={}",
                            result.rateControlAppliedBeforeBegin ? "set" : "not-set",
                            result.rateControlAppliedAfterBegin ? "set" : "not-set"));
    if (!result.actualVideoType.empty()) {
        ConsoleLine(std::format("  video type         {}", WideToUtf8(result.actualVideoType)));
    }
    if (!result.actualAudioType.empty()) {
        ConsoleLine(std::format("  audio type         {}", WideToUtf8(result.actualAudioType)));
    }
    ConsoleLine(std::format("  written            {} video frames, {} audio chunks",
                            result.videoFramesWritten, result.audioChunksWritten));
    ConsoleLine(std::format("  finalize           {}", result.finalized ? "OK" : "not completed"));
    if (result.fileSize != 0) {
        ConsoleLine(std::format("  file size          {}", FormatBytes(result.fileSize)));
    }
    if (!result.validationError.empty()) {
        ConsoleLine(std::format("  validation         FAILED: {}",
                                WideToUtf8(result.validationError)));
    } else {
        ConsoleLine(std::format(
            "  validation         OK (container {:.3f} s, {} video samples, {} audio samples)",
            result.containerDurationSeconds, result.videoSamplesRead, result.audioSamplesRead));
        ConsoleLine(std::format("  video timeline     last timestamp {:.3f} s",
                                result.lastVideoTimestampSeconds));
        ConsoleLine(std::format("  audio timeline     last timestamp {:.3f} s",
                                result.lastAudioTimestampSeconds));
    }
}

}  // namespace srl
