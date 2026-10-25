#include "image/png_probe.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <format>
#include <vector>

#include "common/win32.h"

namespace srl {
namespace {

using Microsoft::WRL::ComPtr;

std::wstring GuidText(REFGUID guid) {
    wchar_t buffer[64]{};
    if (::StringFromGUID2(guid, buffer, ARRAYSIZE(buffer)) == 0) {
        return L"{?}";
    }
    return buffer;
}

std::wstring PixelFormatName(REFGUID format) {
    if (::IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA)) {
        return L"32bppBGRA";
    }
    if (::IsEqualGUID(format, GUID_WICPixelFormat32bppBGR)) {
        return L"32bppBGR";
    }
    if (::IsEqualGUID(format, GUID_WICPixelFormat24bppBGR)) {
        return L"24bppBGR";
    }
    return GuidText(format);
}

void FillPattern(std::vector<std::uint8_t>& pixels, int width, int height, int stride) {
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            // Quadrants: red, green, blue, white (BGRA byte order).
            std::uint8_t b = 0;
            std::uint8_t g = 0;
            std::uint8_t r = 0;
            if (x < width / 2 && y < height / 2) {
                r = 255;
            } else if (x >= width / 2 && y < height / 2) {
                g = 255;
            } else if (x < width / 2 && y >= height / 2) {
                b = 255;
            } else {
                r = 255;
                g = 255;
                b = 255;
            }
            const std::size_t offset = static_cast<std::size_t>(y) * stride +
                                       static_cast<std::size_t>(x) * 4;
            pixels[offset] = b;
            pixels[offset + 1] = g;
            pixels[offset + 2] = r;
            pixels[offset + 3] = 255;
        }
    }
}

bool QuadrantColorsMatch(const std::vector<std::uint8_t>& pixels, int width, int height,
                         int stride) {
    const auto sample = [&pixels, stride](int x, int y) {
        const std::size_t offset = static_cast<std::size_t>(y) * stride +
                                   static_cast<std::size_t>(x) * 4;
        return std::array<std::uint8_t, 4>{pixels[offset], pixels[offset + 1],
                                           pixels[offset + 2], pixels[offset + 3]};
    };
    const auto red = sample(width / 4, height / 4);
    const auto green = sample(width * 3 / 4, height / 4);
    const auto blue = sample(width / 4, height * 3 / 4);
    const auto white = sample(width * 3 / 4, height * 3 / 4);
    return red[2] > 200 && red[1] < 60 && red[0] < 60 &&
           green[1] > 200 && green[2] < 60 && green[0] < 60 &&
           blue[0] > 200 && blue[1] < 60 && blue[2] < 60 &&
           white[0] > 200 && white[1] > 200 && white[2] > 200;
}

}  // namespace

PngProbeResult RunPngProbe(const std::wstring& path, int width, int height) {
    PngProbeResult result;
    result.path = path;
    result.width = width;
    result.height = height;

    if (path.empty()) {
        result.error = L"output path is empty";
        return result;
    }
    if (width < 16 || height < 16) {
        result.error = L"probe dimensions are too small";
        return result;
    }

    const std::filesystem::path outputPath(path);
    if (outputPath.has_parent_path()) {
        std::string directoryError;
        if (!EnsureDirectory(outputPath.parent_path(), &directoryError)) {
            result.error = Utf8ToWide(directoryError);
            return result;
        }
    }

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr)) {
        result.error = Utf8ToWide(std::format("WIC factory creation failed: {}",
                                              HResultText(hr)));
        return result;
    }

    ComPtr<IWICBitmap> bitmap;
    hr = factory->CreateBitmap(static_cast<UINT>(width), static_cast<UINT>(height),
                               GUID_WICPixelFormat32bppBGRA, WICBitmapCacheOnLoad,
                               bitmap.GetAddressOf());
    if (FAILED(hr)) {
        result.error = Utf8ToWide(std::format("CreateBitmap failed: {}", HResultText(hr)));
        return result;
    }

    {
        WICRect rect{0, 0, width, height};
        ComPtr<IWICBitmapLock> lock;
        hr = bitmap->Lock(&rect, WICBitmapLockWrite, lock.GetAddressOf());
        if (FAILED(hr)) {
            result.error = Utf8ToWide(std::format("IWICBitmap::Lock failed: {}",
                                                  HResultText(hr)));
            return result;
        }
        UINT stride = 0;
        UINT size = 0;
        BYTE* data = nullptr;
        hr = lock->GetStride(&stride);
        if (SUCCEEDED(hr)) {
            hr = lock->GetDataPointer(&size, &data);
        }
        if (FAILED(hr) || data == nullptr) {
            result.error = Utf8ToWide(std::format("IWICBitmapLock data failed: {}",
                                                  HResultText(hr)));
            return result;
        }
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(stride) * height);
        FillPattern(pixels, width, height, static_cast<int>(stride));
        std::copy(pixels.begin(), pixels.end(), data);
    }

    ComPtr<IWICStream> stream;
    hr = factory->CreateStream(stream.GetAddressOf());
    if (SUCCEEDED(hr)) {
        hr = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
    }
    if (FAILED(hr)) {
        result.error = Utf8ToWide(std::format("IWICStream::InitializeFromFilename failed: {}",
                                              HResultText(hr)));
        return result;
    }

    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                encoder.GetAddressOf());
    if (SUCCEEDED(hr)) {
        hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    }
    if (FAILED(hr)) {
        result.error = Utf8ToWide(std::format("PNG encoder initialization failed: {}",
                                              HResultText(hr)));
        return result;
    }

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    hr = encoder->CreateNewFrame(frame.GetAddressOf(), properties.GetAddressOf());
    if (FAILED(hr)) {
        result.error = Utf8ToWide(std::format("CreateNewFrame failed: {}", HResultText(hr)));
        return result;
    }
    hr = frame->Initialize(properties.Get());
    if (SUCCEEDED(hr)) {
        hr = frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height));
    }
    if (SUCCEEDED(hr)) {
        hr = frame->SetResolution(96.0, 96.0);
    }
    if (SUCCEEDED(hr)) {
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        hr = frame->SetPixelFormat(&format);
    }
    if (SUCCEEDED(hr)) {
        hr = frame->WriteSource(bitmap.Get(), nullptr);
    }
    if (SUCCEEDED(hr)) {
        hr = frame->Commit();
    }
    if (SUCCEEDED(hr)) {
        hr = encoder->Commit();
    }
    if (FAILED(hr)) {
        result.error = Utf8ToWide(std::format("PNG encode/commit failed: {}",
                                              HResultText(hr)));
        return result;
    }
    stream->Commit(STGC_DEFAULT);
    frame.Reset();
    encoder.Reset();
    stream.Reset();
    bitmap.Reset();

    std::error_code sizeError;
    const auto fileSize = std::filesystem::file_size(outputPath, sizeError);
    if (!sizeError) {
        result.fileSize = fileSize;
    }

    ComPtr<IWICBitmapDecoder> decoder;
    hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                            WICDecodeMetadataCacheOnDemand,
                                            decoder.GetAddressOf());
    if (FAILED(hr)) {
        result.error = Utf8ToWide(std::format("PNG decoder creation failed: {}",
                                              HResultText(hr)));
        return result;
    }
    ComPtr<IWICBitmapFrameDecode> decodedFrame;
    hr = decoder->GetFrame(0, decodedFrame.GetAddressOf());
    if (SUCCEEDED(hr)) {
        UINT decodedWidth = 0;
        UINT decodedHeight = 0;
        hr = decodedFrame->GetSize(&decodedWidth, &decodedHeight);
        if (SUCCEEDED(hr)) {
            result.width = static_cast<int>(decodedWidth);
            result.height = static_cast<int>(decodedHeight);
            result.decoded = decodedWidth == static_cast<UINT>(width) &&
                             decodedHeight == static_cast<UINT>(height);
        }
    }
    if (FAILED(hr)) {
        result.error = Utf8ToWide(std::format("PNG decode failed: {}", HResultText(hr)));
        return result;
    }

    WICPixelFormatGUID decodedFormat{};
    decodedFrame->GetPixelFormat(&decodedFormat);
    result.decodedPixelFormat = PixelFormatName(decodedFormat);

    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(converter.GetAddressOf());
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(decodedFrame.Get(), GUID_WICPixelFormat32bppBGRA,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom);
    }
    if (SUCCEEDED(hr)) {
        const UINT stride = static_cast<UINT>(width) * 4;
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(stride) * height);
        hr = converter->CopyPixels(nullptr, stride, static_cast<UINT>(pixels.size()),
                                   pixels.data());
        if (SUCCEEDED(hr)) {
            result.orientationAndColorsOk =
                QuadrantColorsMatch(pixels, width, height, static_cast<int>(stride));
        }
    }
    if (FAILED(hr)) {
        result.error = Utf8ToWide(std::format("PNG pixel verification failed: {}",
                                              HResultText(hr)));
        return result;
    }

    result.ok = result.decoded && result.orientationAndColorsOk;
    return result;
}

void PrintPngProbe(const PngProbeResult& result) {
    ConsoleLine("PNG (WIC)");
    ConsoleLine(std::format("  output             {}", WideToUtf8(result.path)));
    if (!result.error.empty()) {
        ConsoleLine(std::format("  error              {}", WideToUtf8(result.error)));
    }
    ConsoleLine(std::format("  image              {}x{}", result.width, result.height));
    if (result.fileSize != 0) {
        ConsoleLine(std::format("  file size          {}", FormatBytes(result.fileSize)));
    }
    ConsoleLine(std::format("  decoded            {}{}", result.decoded ? "OK" : "FAILED",
                            result.decodedPixelFormat.empty()
                                ? ""
                                : " (" + WideToUtf8(result.decodedPixelFormat) + ")"));
    ConsoleLine(std::format("  quadrants          {}",
                            result.orientationAndColorsOk ? "OK" : "FAILED"));
}

}  // namespace srl
