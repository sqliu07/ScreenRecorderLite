#include "media/codec_probe.h"

#include <codecapi.h>
#include <icodecapi.h>
#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdlib>
#include <format>
#include <set>
#include <string_view>

#include "common/win32.h"
#include "media/mf_util.h"

namespace srl {
namespace {

using Microsoft::WRL::ComPtr;

constexpr std::uint32_t kUnconstrainedVbr =
    static_cast<std::uint32_t>(eAVEncCommonRateControlMode_UnconstrainedVBR);

std::wstring ReadCodecValue(ICodecAPI* codec, const GUID& key) {
    if (codec == nullptr) {
        return L"(no ICodecAPI)";
    }
    VARIANT value{};
    ::VariantInit(&value);
    const HRESULT hr = codec->GetValue(&key, &value);
    std::wstring result;
    if (FAILED(hr)) {
        result = Utf8ToWide(std::format("n/a ({})", HResultText(hr)));
    } else {
        switch (value.vt) {
            case VT_UI4:
                result = std::to_wstring(value.ulVal);
                break;
            case VT_UI8:
                result = std::to_wstring(value.ullVal);
                break;
            case VT_I4:
                result = std::to_wstring(value.lVal);
                break;
            case VT_BOOL:
                result = value.boolVal ? L"true" : L"false";
                break;
            default:
                result = L"(unsupported VARIANT)";
                break;
        }
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

bool CodecSupports(ICodecAPI* codec, const GUID& key) {
    if (codec == nullptr) {
        return false;
    }
    return SUCCEEDED(codec->IsSupported(&key));
}

std::wstring RateControlModeName(std::uint32_t mode) {
    switch (mode) {
        case eAVEncCommonRateControlMode_CBR:
            return L"CBR(0)";
        case eAVEncCommonRateControlMode_PeakConstrainedVBR:
            return L"PeakConstrainedVBR(1)";
        case eAVEncCommonRateControlMode_UnconstrainedVBR:
            return L"UnconstrainedVBR(2)";
        case eAVEncCommonRateControlMode_Quality:
            return L"Quality(3)";
        case eAVEncCommonRateControlMode_LowDelayVBR:
            return L"LowDelayVBR(4)";
        case eAVEncCommonRateControlMode_GlobalVBR:
            return L"GlobalVBR(5)";
        default:
            return Utf8ToWide(std::format("mode({})", mode));
    }
}

ComPtr<IMFMediaType> CreateH264OutputType(const CodecProbeOptions& options) {
    const std::uint32_t width = static_cast<std::uint32_t>(options.width);
    const std::uint32_t height = static_cast<std::uint32_t>(options.height);
    const std::uint32_t fps = static_cast<std::uint32_t>(options.fps);

    ComPtr<IMFMediaType> mediaType;
    if (FAILED(::MFCreateMediaType(mediaType.GetAddressOf()))) {
        return {};
    }
    mediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    mediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    mediaType->SetUINT32(MF_MT_AVG_BITRATE, options.bitrateBps());
    mediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    ::MFSetAttributeSize(mediaType.Get(), MF_MT_FRAME_SIZE, width, height);
    ::MFSetAttributeRatio(mediaType.Get(), MF_MT_FRAME_RATE, fps, 1);
    ::MFSetAttributeRatio(mediaType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    mediaType->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    mediaType->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    mediaType->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    mediaType->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    return mediaType;
}

ComPtr<IMFMediaType> CreateNv12InputType(const CodecProbeOptions& options) {
    const std::uint32_t width = static_cast<std::uint32_t>(options.width);
    const std::uint32_t height = static_cast<std::uint32_t>(options.height);
    const std::uint32_t fps = static_cast<std::uint32_t>(options.fps);

    ComPtr<IMFMediaType> mediaType;
    if (FAILED(::MFCreateMediaType(mediaType.GetAddressOf()))) {
        return {};
    }
    mediaType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    mediaType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    mediaType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    ::MFSetAttributeSize(mediaType.Get(), MF_MT_FRAME_SIZE, width, height);
    ::MFSetAttributeRatio(mediaType.Get(), MF_MT_FRAME_RATE, fps, 1);
    ::MFSetAttributeRatio(mediaType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    mediaType->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    mediaType->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
    mediaType->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    mediaType->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    return mediaType;
}

std::wstring OutputStreamInfoText(const MFT_OUTPUT_STREAM_INFO& info) {
    std::wstring text = std::to_wstring(info.cbSize);
    text += L" byte minimum, alignment ";
    text += std::to_wstring(info.cbAlignment);
    if ((info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0) {
        text += L", MFT provides samples";
    }
    if ((info.dwFlags & MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES) != 0) {
        text += L", MFT can provide samples";
    }
    if ((info.dwFlags & MFT_OUTPUT_STREAM_WHOLE_SAMPLES) != 0) {
        text += L", whole samples";
    }
    if ((info.dwFlags & MFT_OUTPUT_STREAM_FIXED_SAMPLE_SIZE) != 0) {
        text += L", fixed sample size";
    }
    return text;
}

struct D3DProbeContext {
    ComPtr<ID3D11Device> device;
    ComPtr<IMFDXGIDeviceManager> manager;
    UINT resetToken = 0;
    std::wstring detail;

    bool Create() {
        if (device.Get() != nullptr) {
            return true;
        }
        const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
        HRESULT hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                                         nullptr, 0, D3D11_SDK_VERSION,
                                         device.GetAddressOf(), &featureLevel, nullptr);
        if (FAILED(hr)) {
            hr = ::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                     D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                     D3D11_SDK_VERSION, device.GetAddressOf(), &featureLevel,
                                     nullptr);
        }
        if (FAILED(hr)) {
            detail = Utf8ToWide(std::format("D3D11CreateDevice failed: {}", HResultText(hr)));
            return false;
        }
        hr = ::MFCreateDXGIDeviceManager(&resetToken, manager.GetAddressOf());
        if (FAILED(hr)) {
            detail = Utf8ToWide(std::format("MFCreateDXGIDeviceManager failed: {}",
                                            HResultText(hr)));
            return false;
        }
        hr = manager->ResetDevice(device.Get(), resetToken);
        if (FAILED(hr)) {
            detail = Utf8ToWide(std::format("IMFDXGIDeviceManager::ResetDevice failed: {}",
                                            HResultText(hr)));
            return false;
        }
        detail = L"D3D11 device and device manager ready";
        return true;
    }
};

struct NegotiationOutcome {
    bool activated = false;
    bool negotiated = false;
    bool async = false;
    bool d3d11Aware = false;
    bool d3dManagerSet = false;
    bool rateControlSupported = false;
    bool rateControlSet = false;
    bool meanBitrateSet = false;
    bool gopSizeSet = false;
    bool streamStarted = false;
    std::wstring rateControlReadback;
    std::wstring meanBitrateReadback;
    std::wstring gopSizeReadback;
    std::wstring inputSummary;
    std::wstring outputSummary;
    std::wstring outputStreamInfo;
    std::wstring failureStage;
    std::wstring failureDetail;
    std::wstring note;
};

NegotiationOutcome TryNegotiate(IMFActivate* activate, const CodecProbeOptions& options,
                                bool inputFirst, IMFDXGIDeviceManager* deviceManager) {
    NegotiationOutcome outcome;

    ComPtr<IMFTransform> transform;
    HRESULT hr = activate->ActivateObject(IID_PPV_ARGS(transform.GetAddressOf()));
    if (FAILED(hr)) {
        outcome.failureStage = L"ActivateObject";
        outcome.failureDetail = Utf8ToWide(HResultText(hr));
        return outcome;
    }
    outcome.activated = true;
    outcome.async = TransformIsAsync(transform.Get());
    outcome.d3d11Aware = TransformSupportsD3D11(transform.Get());

    ComPtr<IMFAttributes> attributes;
    if (SUCCEEDED(transform->GetAttributes(attributes.GetAddressOf()))) {
        const HRESULT unlock = attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        if (FAILED(unlock)) {
            outcome.note = Utf8ToWide(
                std::format("MF_TRANSFORM_ASYNC_UNLOCK failed: {}", HResultText(unlock)));
        }
    }

    if (outcome.d3d11Aware && deviceManager != nullptr) {
        const HRESULT d3dHr = transform->ProcessMessage(
            MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(deviceManager));
        outcome.d3dManagerSet = SUCCEEDED(d3dHr);
        if (FAILED(d3dHr)) {
            outcome.note = Utf8ToWide(
                std::format("SET_D3D_MANAGER failed: {}", HResultText(d3dHr)));
        }
    }

    const ComPtr<IMFMediaType> inputType = CreateNv12InputType(options);
    const ComPtr<IMFMediaType> outputType = CreateH264OutputType(options);
    if (inputType == nullptr || outputType == nullptr) {
        outcome.failureStage = L"MFCreateMediaType";
        outcome.failureDetail = L"could not allocate media types";
        return outcome;
    }

    ComPtr<ICodecAPI> codec;
    hr = transform.As(&codec);
    if (SUCCEEDED(hr)) {
        outcome.rateControlSupported = CodecSupports(codec.Get(), CODECAPI_AVEncCommonRateControlMode);
        if (outcome.rateControlSupported) {
            outcome.rateControlSet = SetCodecValue(
                codec.Get(), CODECAPI_AVEncCommonRateControlMode, kUnconstrainedVbr);
        }
        if (CodecSupports(codec.Get(), CODECAPI_AVEncCommonMeanBitRate)) {
            outcome.meanBitrateSet =
                SetCodecValue(codec.Get(), CODECAPI_AVEncCommonMeanBitRate, options.bitrateBps());
        }
        if (CodecSupports(codec.Get(), CODECAPI_AVEncMPVGOPSize)) {
            outcome.gopSizeSet = SetCodecValue(
                codec.Get(), CODECAPI_AVEncMPVGOPSize,
                static_cast<std::uint32_t>(std::max(1, options.fps * 2)));
        }
    }

    if (inputFirst) {
        hr = transform->SetInputType(0, inputType.Get(), 0);
        if (SUCCEEDED(hr)) {
            hr = transform->SetOutputType(0, outputType.Get(), 0);
        }
        if (FAILED(hr)) {
            outcome.failureStage = L"SetInputType then SetOutputType";
            outcome.failureDetail = Utf8ToWide(HResultText(hr));
            return outcome;
        }
    } else {
        hr = transform->SetOutputType(0, outputType.Get(), 0);
        if (SUCCEEDED(hr)) {
            hr = transform->SetInputType(0, inputType.Get(), 0);
        }
        if (FAILED(hr)) {
            outcome.failureStage = L"SetOutputType then SetInputType";
            outcome.failureDetail = Utf8ToWide(HResultText(hr));
            return outcome;
        }
    }

    outcome.negotiated = true;

    DWORD inputStreamCount = 0;
    DWORD outputStreamCount = 0;
    if (SUCCEEDED(transform->GetStreamCount(&inputStreamCount, &outputStreamCount))) {
        if (inputStreamCount != 1 || outputStreamCount != 1) {
            outcome.note += L"streams " + std::to_wstring(inputStreamCount) + L"/" +
                            std::to_wstring(outputStreamCount) + L"; ";
        }
    }

    MFT_OUTPUT_STREAM_INFO streamInfo{};
    if (SUCCEEDED(transform->GetOutputStreamInfo(0, &streamInfo))) {
        outcome.outputStreamInfo = OutputStreamInfoText(streamInfo);
    }

    ComPtr<IMFMediaType> currentInput;
    if (SUCCEEDED(transform->GetInputCurrentType(0, currentInput.GetAddressOf()))) {
        outcome.inputSummary = MediaTypeSummary(currentInput.Get());
    }
    ComPtr<IMFMediaType> currentOutput;
    if (SUCCEEDED(transform->GetOutputCurrentType(0, currentOutput.GetAddressOf()))) {
        outcome.outputSummary = MediaTypeSummary(currentOutput.Get());
    }

    if (codec != nullptr) {
        outcome.rateControlReadback =
            ReadCodecValue(codec.Get(), CODECAPI_AVEncCommonRateControlMode);
        outcome.meanBitrateReadback =
            ReadCodecValue(codec.Get(), CODECAPI_AVEncCommonMeanBitRate);
        outcome.gopSizeReadback = ReadCodecValue(codec.Get(), CODECAPI_AVEncMPVGOPSize);
    }

    const HRESULT beginStreaming =
        transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    const HRESULT startOfStream =
        SUCCEEDED(beginStreaming)
            ? transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0)
            : beginStreaming;
    outcome.streamStarted = SUCCEEDED(startOfStream);
    if (outcome.streamStarted) {
        transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    } else {
        outcome.note += L"stream start failed: " + Utf8ToWide(HResultText(startOfStream)) +
                        L"; ";
    }
    return outcome;
}

std::vector<ComPtr<IMFActivate>> EnumerateEncoders(REFGUID category,
                                                    const MFT_REGISTER_TYPE_INFO& inputInfo,
                                                    const MFT_REGISTER_TYPE_INFO& outputInfo) {
    std::vector<ComPtr<IMFActivate>> result;
    std::set<std::wstring> seenClsids;

    const DWORD flagSets[] = {
        MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT | MFT_ENUM_FLAG_HARDWARE |
            MFT_ENUM_FLAG_SORTANDFILTER,
        MFT_ENUM_FLAG_ALL,
    };
    for (const DWORD flags : flagSets) {
        IMFActivate** raw = nullptr;
        UINT32 count = 0;
        const HRESULT hr = ::MFTEnumEx(category, flags, &inputInfo, &outputInfo, &raw, &count);
        if (FAILED(hr) || raw == nullptr) {
            continue;
        }
        for (UINT32 i = 0; i < count; ++i) {
            IMFActivate* activate = raw[i];
            if (activate == nullptr) {
                continue;
            }
            GUID clsid{};
            std::wstring key = Utf8ToWide(std::format("<no-clsid-{}>", i));
            if (SUCCEEDED(activate->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &clsid))) {
                key = GuidToWString(clsid);
            }
            if (seenClsids.insert(key).second) {
                result.emplace_back();
                result.back().Attach(activate);
            } else {
                activate->Release();
            }
        }
        ::CoTaskMemFree(raw);
        if (!result.empty()) {
            break;
        }
    }
    return result;
}

H264EncoderProbe ProbeH264Encoder(IMFActivate* activate, const CodecProbeOptions& options,
                                  D3DProbeContext& d3d) {
    H264EncoderProbe probe;
    probe.friendlyName = ActivateFriendlyName(activate);
    GUID clsid{};
    if (SUCCEEDED(activate->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &clsid))) {
        probe.clsid = GuidToWString(clsid);
    }
    probe.hardware = ActivateIsHardware(activate);

    // A first pass reports KSPROPERTY style flags; they are also reported by
    // the activation object and repeated here for completeness.
    NegotiationOutcome outputFirst = TryNegotiate(
        activate, options, /*inputFirst=*/false,
        probe.hardware && d3d.device.Get() != nullptr ? d3d.manager.Get() : nullptr);
    probe.d3d11Aware = outputFirst.d3d11Aware;
    probe.async = outputFirst.async;
    if (!outputFirst.activated) {
        probe.activation = ProbeOutcome::Failed;
        probe.failureStage = outputFirst.failureStage;
        probe.failureDetail = outputFirst.failureDetail;
        return probe;
    }
    probe.activation = ProbeOutcome::Success;

    NegotiationOutcome outcome = outputFirst;
    if (!outcome.negotiated) {
        NegotiationOutcome inputFirst = TryNegotiate(
            activate, options, /*inputFirst=*/true,
            probe.hardware && d3d.device.Get() != nullptr ? d3d.manager.Get() : nullptr);
        if (inputFirst.negotiated) {
            outcome = inputFirst;
            probe.note += L"negotiated with input-first order; ";
        } else {
            probe.note += L"output-first: " + outputFirst.failureDetail + L"; input-first: " +
                          inputFirst.failureDetail + L"; ";
            outcome = inputFirst.activated ? inputFirst : outputFirst;
        }
    }

    if (!outcome.negotiated) {
        probe.negotiation = ProbeOutcome::Failed;
        probe.failureStage = outcome.failureStage;
        probe.failureDetail = outcome.failureDetail;
        probe.note += outcome.note;
        return probe;
    }

    probe.negotiation = ProbeOutcome::Success;
    probe.rateControlSupported = outcome.rateControlSupported;
    probe.rateControlSet = outcome.rateControlSet;
    probe.meanBitrateSet = outcome.meanBitrateSet;
    probe.gopSizeSet = outcome.gopSizeSet;
    probe.rateControlReadback = outcome.rateControlReadback;
    probe.meanBitrateReadback = outcome.meanBitrateReadback;
    probe.gopSizeReadback = outcome.gopSizeReadback;
    probe.inputSummary = outcome.inputSummary;
    probe.outputSummary = outcome.outputSummary;
    probe.outputStreamInfo = outcome.outputStreamInfo;
    probe.streamStarted = outcome.streamStarted;
    probe.note += outcome.note;
    if (!outcome.d3dManagerSet && probe.hardware) {
        probe.note += L"D3D manager not set; ";
    }
    return probe;
}

AacEncoderProbe ProbeAacEncoder(const std::vector<ComPtr<IMFActivate>>& candidates) {
    AacEncoderProbe probe;
    if (candidates.empty()) {
        return probe;
    }
    probe.found = true;
    probe.friendlyName = ActivateFriendlyName(candidates.front().Get());
    GUID clsid{};
    if (SUCCEEDED(candidates.front()->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &clsid))) {
        probe.clsid = GuidToWString(clsid);
    }

    auto makeOutput = [] {
        ComPtr<IMFMediaType> type;
        if (FAILED(::MFCreateMediaType(type.GetAddressOf()))) {
            return type;
        }
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48'000);
        type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 16'000);
        type->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);  // raw AAC payload
        type->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);  // AAC-LC
        return type;
    };
    auto makeInput = [] {
        ComPtr<IMFMediaType> type;
        if (FAILED(::MFCreateMediaType(type.GetAddressOf()))) {
            return type;
        }
        type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        type->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        type->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48'000);
        type->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        type->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        type->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        type->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 192'000);
        return type;
    };

    const auto outputType = makeOutput();
    const auto inputType = makeInput();
    if (outputType == nullptr || inputType == nullptr) {
        probe.failureStage = L"MFCreateMediaType";
        probe.failureDetail = L"could not allocate media types";
        return probe;
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        ComPtr<IMFTransform> transform;
        HRESULT hr = candidates.front()->ActivateObject(
            IID_PPV_ARGS(transform.GetAddressOf()));
        if (FAILED(hr)) {
            probe.failureStage = L"ActivateObject";
            probe.failureDetail = Utf8ToWide(HResultText(hr));
            return probe;
        }
        ComPtr<IMFAttributes> attributes;
        if (SUCCEEDED(transform->GetAttributes(attributes.GetAddressOf()))) {
            attributes->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        }

        const bool outputFirst = attempt == 0;
        hr = outputFirst ? transform->SetOutputType(0, outputType.Get(), 0)
                         : transform->SetInputType(0, inputType.Get(), 0);
        if (SUCCEEDED(hr)) {
            hr = outputFirst ? transform->SetInputType(0, inputType.Get(), 0)
                             : transform->SetOutputType(0, outputType.Get(), 0);
        }
        if (SUCCEEDED(hr)) {
            probe.negotiationOk = true;
            probe.failureStage.clear();
            probe.failureDetail.clear();
            ComPtr<IMFMediaType> currentInput;
            if (SUCCEEDED(transform->GetInputCurrentType(0, currentInput.GetAddressOf()))) {
                probe.inputSummary = MediaTypeSummary(currentInput.Get());
            }
            ComPtr<IMFMediaType> currentOutput;
            if (SUCCEEDED(transform->GetOutputCurrentType(0, currentOutput.GetAddressOf()))) {
                probe.outputSummary = MediaTypeSummary(currentOutput.Get());
            }
            return probe;
        }
        probe.failureStage =
            outputFirst ? L"SetOutputType then SetInputType" : L"SetInputType then SetOutputType";
        probe.failureDetail = Utf8ToWide(HResultText(hr));
    }
    return probe;
}

}  // namespace

CodecProbeResult ProbeCodecs(const CodecProbeOptions& options) {
    CodecProbeResult result;
    result.requested = options;

    const MFT_REGISTER_TYPE_INFO videoIn{MFMediaType_Video, MFVideoFormat_NV12};
    const MFT_REGISTER_TYPE_INFO videoOut{MFMediaType_Video, MFVideoFormat_H264};
    const auto h264Candidates =
        EnumerateEncoders(MFT_CATEGORY_VIDEO_ENCODER, videoIn, videoOut);

    D3DProbeContext d3d;
    const bool hasHardwareCandidate = std::any_of(
        h264Candidates.begin(), h264Candidates.end(),
        [](const ComPtr<IMFActivate>& activate) { return ActivateIsHardware(activate.Get()); });
    if (hasHardwareCandidate) {
        d3d.Create();
    }
    if (!d3d.detail.empty()) {
        result.d3dDetail = d3d.detail;
    }

    for (const ComPtr<IMFActivate>& activate : h264Candidates) {
        H264EncoderProbe probe = ProbeH264Encoder(activate.Get(), options, d3d);
        if (probe.negotiation == ProbeOutcome::Success) {
            result.anyH264Negotiated = true;
            if (probe.hardware) {
                result.anyHardwareNegotiated = true;
            }
        }
        result.h264.push_back(std::move(probe));
    }

    const MFT_REGISTER_TYPE_INFO audioIn{MFMediaType_Audio, MFAudioFormat_PCM};
    const MFT_REGISTER_TYPE_INFO audioOut{MFMediaType_Audio, MFAudioFormat_AAC};
    const auto aacCandidates =
        EnumerateEncoders(MFT_CATEGORY_AUDIO_ENCODER, audioIn, audioOut);
    result.aac = ProbeAacEncoder(aacCandidates);
    return result;
}

void PrintCodecProbe(const CodecProbeResult& result) {
    const CodecProbeOptions& options = result.requested;
    ConsoleLine("Codecs");
    ConsoleLine(std::format("  Requested H.264 {}x{} @ {} fps, target {} Mbps VBR",
                            options.width, options.height, options.fps, options.bitrateMbps));
    if (!result.d3dDetail.empty()) {
        ConsoleLine(std::format("  D3D                {}", WideToUtf8(result.d3dDetail)));
    }
    if (result.h264.empty()) {
        ConsoleLine("  H.264 encoders     (none enumerated for NV12 -> H.264)");
    } else {
        ConsoleLine(std::format("  H.264 encoders     {}", result.h264.size()));
    }

    for (std::size_t i = 0; i < result.h264.size(); ++i) {
        const H264EncoderProbe& encoder = result.h264[i];
        ConsoleLine(std::format("    #{} [{}] {}", i + 1,
                                encoder.hardware ? "HARDWARE" : "SOFTWARE",
                                WideToUtf8(encoder.friendlyName)));
        if (!encoder.clsid.empty()) {
            ConsoleLine(std::format("        clsid {}", WideToUtf8(encoder.clsid)));
        }
        ConsoleLine(std::format("        flags async={} d3d11Aware={}",
                                encoder.async ? "yes" : "no",
                                encoder.d3d11Aware ? "yes" : "no"));
        if (encoder.activation == ProbeOutcome::Failed) {
            ConsoleLine(std::format("        activation FAILED at {}: {}",
                                    WideToUtf8(encoder.failureStage),
                                    WideToUtf8(encoder.failureDetail)));
            continue;
        }
        if (encoder.negotiation == ProbeOutcome::Failed) {
            ConsoleLine(std::format("        negotiation FAILED at {}: {}",
                                    WideToUtf8(encoder.failureStage),
                                    WideToUtf8(encoder.failureDetail)));
        } else {
            ConsoleLine("        negotiation OK");
            std::wstring rateDisplay = encoder.rateControlReadback;
            wchar_t* end = nullptr;
            const unsigned long mode =
                std::wcstoul(encoder.rateControlReadback.c_str(), &end, 10);
            if (end != nullptr && end != encoder.rateControlReadback.c_str() && *end == L'\0') {
                rateDisplay = RateControlModeName(static_cast<std::uint32_t>(mode));
            }
            ConsoleLine(std::format("        rate control supported={} set={} readback={}",
                                    encoder.rateControlSupported ? "yes" : "no",
                                    encoder.rateControlSet ? "yes" : "no",
                                    WideToUtf8(rateDisplay)));
            ConsoleLine(std::format("        mean bitrate  set={} readback={}",
                                    encoder.meanBitrateSet ? "yes" : "no",
                                    WideToUtf8(encoder.meanBitrateReadback)));
            ConsoleLine(std::format("        GOP size      set={} readback={}",
                                    encoder.gopSizeSet ? "yes" : "no",
                                    WideToUtf8(encoder.gopSizeReadback)));
            ConsoleLine(std::format("        output        {}", WideToUtf8(encoder.outputSummary)));
            ConsoleLine(std::format("        input         {}", WideToUtf8(encoder.inputSummary)));
            if (!encoder.outputStreamInfo.empty()) {
                ConsoleLine(std::format("        output stream {}",
                                        WideToUtf8(encoder.outputStreamInfo)));
            }
            ConsoleLine(std::format("        start stream  {}",
                                    encoder.streamStarted ? "OK" : "FAILED"));
        }
        if (!encoder.note.empty()) {
            ConsoleLine(std::format("        note          {}", WideToUtf8(encoder.note)));
        }
    }

    ConsoleLine(std::format("  H.264 result       {} negotiated{}",
                            result.anyH264Negotiated ? "at least one" : "NONE",
                            result.anyHardwareNegotiated ? ", hardware negotiated" : ""));

    if (!result.aac.found) {
        ConsoleLine("  AAC encoder        (none enumerated for PCM -> AAC)");
    } else {
        ConsoleLine(std::format("  AAC encoder        {}", WideToUtf8(result.aac.friendlyName)));
        if (!result.aac.clsid.empty()) {
            ConsoleLine(std::format("    clsid {}", WideToUtf8(result.aac.clsid)));
        }
        if (result.aac.negotiationOk) {
            ConsoleLine("    negotiation OK");
            ConsoleLine(std::format("    output {}", WideToUtf8(result.aac.outputSummary)));
            ConsoleLine(std::format("    input  {}", WideToUtf8(result.aac.inputSummary)));
        } else {
            ConsoleLine(std::format("    negotiation FAILED at {}: {}",
                                    WideToUtf8(result.aac.failureStage),
                                    WideToUtf8(result.aac.failureDetail)));
        }
    }
}

}  // namespace srl
