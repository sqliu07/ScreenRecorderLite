#include "media/mf_util.h"

#include <codecapi.h>

#include <format>

#include "common/win32.h"

namespace srl {

MediaFoundationRuntime::MediaFoundationRuntime() {
    const HRESULT hr = ::MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(hr)) {
        ThrowHResult(hr, "MFStartup");
    }
    started_ = true;
}

MediaFoundationRuntime::~MediaFoundationRuntime() {
    if (started_) {
        ::MFShutdown();
    }
}

std::wstring GuidToWString(REFGUID guid) {
    wchar_t buffer[64]{};
    if (::StringFromGUID2(guid, buffer, ARRAYSIZE(buffer)) == 0) {
        return L"{?}";
    }
    return buffer;
}

std::string MediaMajorTypeName(REFGUID guid) {
    if (::IsEqualGUID(guid, MFMediaType_Video)) {
        return "video";
    }
    if (::IsEqualGUID(guid, MFMediaType_Audio)) {
        return "audio";
    }
    return WideToUtf8(GuidToWString(guid));
}

std::string MediaSubtypeName(REFGUID guid) {
    struct KnownSubtype {
        const GUID* guid;
        const char* name;
    };
    const KnownSubtype known[] = {
        {&MFVideoFormat_H264, "H.264"},
        {&MFVideoFormat_NV12, "NV12"},
        {&MFVideoFormat_YUY2, "YUY2"},
        {&MFVideoFormat_ARGB32, "ARGB32"},
        {&MFVideoFormat_RGB32, "RGB32"},
        {&MFVideoFormat_IYUV, "IYUV"},
        {&MFVideoFormat_H265, "HEVC"},
        {&MFAudioFormat_PCM, "PCM"},
        {&MFAudioFormat_Float, "IEEE float"},
        {&MFAudioFormat_AAC, "AAC"},
    };
    for (const KnownSubtype& subtype : known) {
        if (::IsEqualGUID(guid, *subtype.guid)) {
            return subtype.name;
        }
    }
    return WideToUtf8(GuidToWString(guid));
}

std::string MftCategoryName(REFGUID guid) {
    struct KnownCategory {
        const GUID* guid;
        const char* name;
    };
    const KnownCategory known[] = {
        {&MFT_CATEGORY_VIDEO_ENCODER, "video encoder"},
        {&MFT_CATEGORY_AUDIO_ENCODER, "audio encoder"},
        {&MFT_CATEGORY_VIDEO_DECODER, "video decoder"},
        {&MFT_CATEGORY_AUDIO_DECODER, "audio decoder"},
        {&MFT_CATEGORY_VIDEO_PROCESSOR, "video processor"},
        {&MFT_CATEGORY_MULTIPLEXER, "multiplexer"},
        {&MFT_CATEGORY_DEMULTIPLEXER, "demultiplexer"},
    };
    for (const KnownCategory& category : known) {
        if (::IsEqualGUID(guid, *category.guid)) {
            return category.name;
        }
    }
    return WideToUtf8(GuidToWString(guid));
}

std::wstring AttributeString(IMFAttributes* attributes, REFGUID key) {
    if (attributes == nullptr) {
        return {};
    }
    LPWSTR raw = nullptr;
    UINT32 length = 0;
    if (FAILED(attributes->GetAllocatedString(key, &raw, &length)) || raw == nullptr) {
        return {};
    }
    std::wstring value(raw);
    ::CoTaskMemFree(raw);
    return value;
}

std::wstring ActivateFriendlyName(IMFActivate* activate) {
    std::wstring name = AttributeString(activate, MFT_FRIENDLY_NAME_Attribute);
    if (!name.empty()) {
        return name;
    }
    GUID clsid{};
    if (activate != nullptr && SUCCEEDED(activate->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &clsid))) {
        return L"CLSID " + GuidToWString(clsid);
    }
    return L"<unnamed MFT>";
}

std::wstring TransformFriendlyName(IMFTransform* transform) {
    if (transform == nullptr) {
        return {};
    }
    Microsoft::WRL::ComPtr<IMFAttributes> attributes;
    if (SUCCEEDED(transform->GetAttributes(attributes.GetAddressOf()))) {
        return AttributeString(attributes.Get(), MFT_FRIENDLY_NAME_Attribute);
    }
    return {};
}

bool ActivateIsHardware(IMFActivate* activate) {
    if (activate == nullptr) {
        return false;
    }
    if (!AttributeString(activate, MFT_ENUM_HARDWARE_URL_Attribute).empty()) {
        return true;
    }
    return !AttributeString(activate, MFT_ENUM_HARDWARE_VENDOR_ID_Attribute).empty();
}

bool TransformSupportsD3D11(IMFTransform* transform) {
    if (transform == nullptr) {
        return false;
    }
    Microsoft::WRL::ComPtr<IMFAttributes> attributes;
    if (FAILED(transform->GetAttributes(attributes.GetAddressOf()))) {
        return false;
    }
    UINT32 aware = 0;
    return SUCCEEDED(attributes->GetUINT32(MF_SA_D3D11_AWARE, &aware)) && aware != 0;
}

bool TransformIsAsync(IMFTransform* transform) {
    if (transform == nullptr) {
        return false;
    }
    Microsoft::WRL::ComPtr<IMFAttributes> attributes;
    if (FAILED(transform->GetAttributes(attributes.GetAddressOf()))) {
        return false;
    }
    UINT32 async = 0;
    return SUCCEEDED(attributes->GetUINT32(MF_TRANSFORM_ASYNC, &async)) && async != 0;
}

std::wstring MediaTypeSummary(IMFMediaType* mediaType) {
    if (mediaType == nullptr) {
        return L"<null media type>";
    }

    GUID major{};
    GUID subtype{};
    mediaType->GetGUID(MF_MT_MAJOR_TYPE, &major);
    mediaType->GetGUID(MF_MT_SUBTYPE, &subtype);

    std::wstring result = Utf8ToWide(MediaMajorTypeName(major)) + L" / " +
                          Utf8ToWide(MediaSubtypeName(subtype));
    const auto append = [&result](std::string text) { result += Utf8ToWide(text); };

    if (::IsEqualGUID(major, MFMediaType_Video)) {
        UINT32 width = 0;
        UINT32 height = 0;
        if (SUCCEEDED(::MFGetAttributeSize(mediaType, MF_MT_FRAME_SIZE, &width, &height))) {
            append(std::format(" {}x{}", width, height));
        }
        UINT32 numerator = 0;
        UINT32 denominator = 0;
        if (SUCCEEDED(::MFGetAttributeRatio(mediaType, MF_MT_FRAME_RATE, &numerator,
                                             &denominator)) &&
            denominator != 0) {
            append(std::format(" @ {:.3g} fps",
                               static_cast<double>(numerator) / denominator));
        }
        UINT32 bitrate = 0;
        if (SUCCEEDED(mediaType->GetUINT32(MF_MT_AVG_BITRATE, &bitrate))) {
            append(std::format(" @ {:.2f} Mbps", static_cast<double>(bitrate) / 1'000'000.0));
        }
        UINT32 interlaced = 0;
        if (SUCCEEDED(mediaType->GetUINT32(MF_MT_INTERLACE_MODE, &interlaced)) &&
            interlaced == MFVideoInterlace_Progressive) {
            result += L" progressive";
        }
    } else if (::IsEqualGUID(major, MFMediaType_Audio)) {
        UINT32 sampleRate = 0;
        if (SUCCEEDED(mediaType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sampleRate))) {
            append(std::format(" {} Hz", sampleRate));
        }
        UINT32 channels = 0;
        if (SUCCEEDED(mediaType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels))) {
            append(std::format(", {} ch", channels));
        }
        UINT32 bits = 0;
        if (SUCCEEDED(mediaType->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits))) {
            append(std::format(", {} bit", bits));
        }
        UINT32 bytesPerSecond = 0;
        if (SUCCEEDED(mediaType->GetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND,
                                            &bytesPerSecond))) {
            append(std::format(", {:.0f} kbps",
                               static_cast<double>(bytesPerSecond) * 8.0 / 1000.0));
        }
    }
    return result;
}

}  // namespace srl
