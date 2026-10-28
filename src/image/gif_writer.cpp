#include "image/gif_writer.h"

#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <limits>
#include <vector>

#include "common/win32.h"
#include "core/recording_format.h"

namespace srl {
namespace {
using Microsoft::WRL::ComPtr;
constexpr std::int64_t kTicksPerCentisecond = kHundredNanosecondsPerSecond / 100;
constexpr std::uint64_t kMinimumFrameDelay = 2;
constexpr std::uint64_t kMaximumFrameDelay = std::numeric_limits<USHORT>::max();
constexpr UINT kPaletteColors = 256;
constexpr BYTE kKeepFrameDisposal = 1;

bool Fail(std::wstring* error, std::wstring message, HRESULT hr = S_OK) {
    if (error) {
        if (FAILED(hr)) message += FormatHResultMessage(hr);
        *error = std::move(message);
    }
    return false;
}

bool ValidSize(Size size) {
    return size.width > 0 && size.height > 0 &&
        size.width <= std::numeric_limits<int>::max() / 4 &&
        static_cast<std::uint64_t>(size.width) * size.height * 4 <=
            std::numeric_limits<UINT>::max();
}

HRESULT SetVector(IWICMetadataQueryWriter* metadata, const wchar_t* name,
                  BYTE* bytes, UINT count) {
    PROPVARIANT value{};
    value.vt = VT_UI1 | VT_VECTOR;
    value.caub = {count, bytes};
    return metadata->SetMetadataByName(name, &value);
}

std::uint64_t Centiseconds(std::int64_t timestamp) {
    // Division first avoids overflow for a large capture-relative clock value.
    return static_cast<std::uint64_t>(timestamp / kTicksPerCentisecond) +
        (timestamp % kTicksPerCentisecond >= kTicksPerCentisecond / 2 ? 1U : 0U);
}
}  // namespace

struct GifWriter::Impl {
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IStream> stream;
    Size sourceSize{};
    RECT crop{};
    OutputSize output{};
    std::vector<std::uint8_t> pending;
    std::int64_t lastTimestamp = -1;
    std::uint64_t writtenCentiseconds = 0;

    void Close() noexcept {
        encoder.Reset();
        stream.Reset();
        factory.Reset();
        pending.clear();
        lastTimestamp = -1;
        writtenCentiseconds = 0;
    }

    bool FailAndClose(std::wstring* error, std::wstring message, HRESULT hr = S_OK) {
        Close();
        return Fail(error, std::move(message), hr);
    }

    bool EncodePending(USHORT delay, std::wstring* error) {
        ComPtr<IWICBitmap> bitmap;
        const UINT stride = static_cast<UINT>(output.output.width) * 4;
        HRESULT hr = factory->CreateBitmapFromMemory(
            static_cast<UINT>(output.output.width), static_cast<UINT>(output.output.height),
            GUID_WICPixelFormat32bppBGRA, stride, static_cast<UINT>(pending.size()),
            pending.data(), bitmap.GetAddressOf());
        ComPtr<IWICPalette> palette;
        if (SUCCEEDED(hr)) hr = factory->CreatePalette(palette.GetAddressOf());
        if (SUCCEEDED(hr)) hr = palette->InitializeFromBitmap(bitmap.Get(), kPaletteColors, FALSE);
        ComPtr<IWICFormatConverter> indexed;
        if (SUCCEEDED(hr)) hr = factory->CreateFormatConverter(indexed.GetAddressOf());
        if (SUCCEEDED(hr)) hr = indexed->Initialize(bitmap.Get(), GUID_WICPixelFormat8bppIndexed,
            WICBitmapDitherTypeErrorDiffusion, palette.Get(), 0.0, WICBitmapPaletteTypeCustom);
        ComPtr<IWICBitmapFrameEncode> frame;
        ComPtr<IPropertyBag2> properties;
        if (SUCCEEDED(hr)) hr = encoder->CreateNewFrame(frame.GetAddressOf(), properties.GetAddressOf());
        if (SUCCEEDED(hr)) hr = frame->Initialize(properties.Get());
        if (SUCCEEDED(hr)) hr = frame->SetSize(static_cast<UINT>(output.output.width),
                                               static_cast<UINT>(output.output.height));
        WICPixelFormatGUID format = GUID_WICPixelFormat8bppIndexed;
        if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&format);
        if (SUCCEEDED(hr) && !::IsEqualGUID(format, GUID_WICPixelFormat8bppIndexed))
            hr = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
        if (SUCCEEDED(hr)) hr = frame->SetPalette(palette.Get());
        ComPtr<IWICMetadataQueryWriter> metadata;
        if (SUCCEEDED(hr)) hr = frame->GetMetadataQueryWriter(metadata.GetAddressOf());
        PROPVARIANT value{};
        value.vt = VT_UI2;
        value.uiVal = delay;
        if (SUCCEEDED(hr)) hr = metadata->SetMetadataByName(L"/grctlext/Delay", &value);
        value.vt = VT_UI1;
        value.bVal = kKeepFrameDisposal;
        if (SUCCEEDED(hr)) hr = metadata->SetMetadataByName(L"/grctlext/Disposal", &value);
        value.vt = VT_BOOL;
        value.boolVal = VARIANT_FALSE;
        if (SUCCEEDED(hr)) hr = metadata->SetMetadataByName(L"/grctlext/TransparencyFlag", &value);
        if (SUCCEEDED(hr)) hr = frame->WriteSource(indexed.Get(), nullptr);
        if (SUCCEEDED(hr)) hr = frame->Commit();
        if (FAILED(hr)) return FailAndClose(error, L"GIF 帧编码失败：", hr);
        return true;
    }

    bool FlushUntil(std::uint64_t end, std::wstring* error) {
        while (end > writtenCentiseconds) {
            const auto remaining = end - writtenCentiseconds;
            auto delay = std::min(kMaximumFrameDelay, remaining);
            // Leave a representable final frame when a long hold spans two GIF delays.
            if (remaining > kMaximumFrameDelay && remaining - delay < kMinimumFrameDelay)
                delay -= kMinimumFrameDelay - (remaining - delay);
            if (!EncodePending(static_cast<USHORT>(delay), error)) return false;
            writtenCentiseconds += delay;
        }
        return true;
    }

    bool Transform(const std::vector<std::uint8_t>& bgra,
                    std::vector<std::uint8_t>& transformed, std::wstring* error) {
        ComPtr<IWICBitmap> source;
        HRESULT hr = factory->CreateBitmapFromMemory(static_cast<UINT>(sourceSize.width),
            static_cast<UINT>(sourceSize.height), GUID_WICPixelFormat32bppBGRA,
            static_cast<UINT>(sourceSize.width) * 4, static_cast<UINT>(bgra.size()),
            const_cast<BYTE*>(bgra.data()), source.GetAddressOf());
        ComPtr<IWICBitmapClipper> clipper;
        if (SUCCEEDED(hr)) hr = factory->CreateBitmapClipper(clipper.GetAddressOf());
        const WICRect region{crop.left, crop.top, crop.right - crop.left, crop.bottom - crop.top};
        if (SUCCEEDED(hr)) hr = clipper->Initialize(source.Get(), &region);
        ComPtr<IWICBitmapSource> scaled = clipper;
        ComPtr<IWICBitmapScaler> scaler;
        if (SUCCEEDED(hr) && (region.Width != output.content.width || region.Height != output.content.height)) {
            hr = factory->CreateBitmapScaler(scaler.GetAddressOf());
            if (SUCCEEDED(hr)) hr = scaler->Initialize(clipper.Get(),
                static_cast<UINT>(output.content.width), static_cast<UINT>(output.content.height),
                WICBitmapInterpolationModeLinear);
            if (SUCCEEDED(hr)) scaled = scaler;
        }
        const UINT stride = static_cast<UINT>(output.output.width) * 4;
        transformed.assign(static_cast<std::size_t>(stride) * output.output.height, 0);
        if (SUCCEEDED(hr)) hr = scaled->CopyPixels(nullptr, stride,
            static_cast<UINT>(transformed.size()), transformed.data());
        if (FAILED(hr)) return FailAndClose(error, L"GIF 画面转换失败：", hr);
        for (std::size_t i = 3; i < transformed.size(); i += 4) transformed[i] = 255;
        return true;
    }
};

GifWriter::GifWriter() : impl_(std::make_unique<Impl>()) {}
GifWriter::~GifWriter() = default;

bool GifWriter::Open(IStream* stream, Size sourceSize, const RECT& sourceRect,
                      const OutputSize& output, std::wstring* error) {
    impl_->Close();
    if (error) error->clear();
    if (!stream || !ValidSize(sourceSize) || !ValidSize(output.content) || !ValidSize(output.output) ||
        output.output.width > std::numeric_limits<USHORT>::max() ||
        output.output.height > std::numeric_limits<USHORT>::max() ||
        output.content.width > output.output.width || output.content.height > output.output.height ||
        sourceRect.left < 0 || sourceRect.top < 0 || sourceRect.right > sourceSize.width ||
        sourceRect.bottom > sourceSize.height || sourceRect.left >= sourceRect.right ||
        sourceRect.top >= sourceRect.bottom) return Fail(error, L"GIF 录制参数无效");
    STATSTG information{};
    HRESULT hr = stream->Stat(&information, STATFLAG_NONAME);
    ULARGE_INTEGER position{};
    if (SUCCEEDED(hr)) hr = stream->Seek({}, STREAM_SEEK_CUR, &position);
    if (FAILED(hr) || information.cbSize.QuadPart != 0 || position.QuadPart != 0)
        return Fail(error, L"GIF 输出流必须为空");
    impl_->stream = stream;
    impl_->sourceSize = sourceSize;
    impl_->crop = sourceRect;
    impl_->output = output;
    hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(impl_->factory.GetAddressOf()));
    if (SUCCEEDED(hr)) hr = impl_->factory->CreateEncoder(GUID_ContainerFormatGif, nullptr,
                                                          impl_->encoder.GetAddressOf());
    if (SUCCEEDED(hr)) hr = impl_->encoder->Initialize(stream, WICBitmapEncoderNoCache);
    ComPtr<IWICMetadataQueryWriter> metadata;
    if (SUCCEEDED(hr)) hr = impl_->encoder->GetMetadataQueryWriter(metadata.GetAddressOf());
    BYTE application[] = {'N','E','T','S','C','A','P','E','2','.','0'};
    BYTE loop[] = {3,1,0,0,0}; // Extension block size, loop type, infinite loop count, terminator.
    if (SUCCEEDED(hr)) hr = SetVector(metadata.Get(), L"/appext/Application", application, sizeof(application));
    if (SUCCEEDED(hr)) hr = SetVector(metadata.Get(), L"/appext/Data", loop, sizeof(loop));
    if (FAILED(hr)) return impl_->FailAndClose(error, L"无法初始化 GIF 编码器：", hr);
    return true;
}

bool GifWriter::WriteFrame(const std::vector<std::uint8_t>& bgra, int width, int height,
                           std::int64_t timestamp100ns, std::wstring* error) {
    if (!impl_->encoder || width != impl_->sourceSize.width || height != impl_->sourceSize.height ||
        timestamp100ns < 0 || timestamp100ns <= impl_->lastTimestamp ||
        bgra.size() != static_cast<std::uint64_t>(impl_->sourceSize.width) * impl_->sourceSize.height * 4)
        return impl_->FailAndClose(error, L"GIF 画面数据或时间戳无效");
    const auto end = Centiseconds(timestamp100ns);
    impl_->lastTimestamp = timestamp100ns;
    if (!impl_->pending.empty()) {
        // Never emit the sub-20 ms frames that common GIF viewers clamp to 100 ms.
        if (end < impl_->writtenCentiseconds + kMinimumFrameDelay) return true;
        if (!impl_->FlushUntil(end, error)) return false;
    }
    std::vector<std::uint8_t> transformed;
    if (!impl_->Transform(bgra, transformed, error)) return false;
    impl_->pending = std::move(transformed);
    return true;
}

bool GifWriter::Finish(std::int64_t endTimestamp100ns, std::wstring* error) {
    if (!impl_->encoder || impl_->pending.empty() || endTimestamp100ns < impl_->lastTimestamp)
        return impl_->FailAndClose(error, L"GIF 没有有效画面或结束时间无效");
    const auto end = std::max(Centiseconds(endTimestamp100ns),
                               impl_->writtenCentiseconds + kMinimumFrameDelay);
    if (!impl_->FlushUntil(end, error)) return false;
    HRESULT hr = impl_->encoder->Commit();
    if (SUCCEEDED(hr)) hr = impl_->stream->Commit(STGC_DEFAULT);
    if (FAILED(hr)) return impl_->FailAndClose(error, L"GIF 收尾失败：", hr);
    impl_->Close();
    return true;
}

void GifWriter::Close() noexcept { impl_->Close(); }
}  // namespace srl
