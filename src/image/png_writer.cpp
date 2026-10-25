#include "image/png_writer.h"

#include <wincodec.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include <filesystem>
#include <format>
#include <limits>
#include <vector>

#include "common/win32.h"

namespace srl {
namespace {

using Microsoft::WRL::ComPtr;

bool Fail(std::wstring* error, std::wstring message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

}  // namespace

bool SaveBgraPng(const std::wstring& path, const std::uint8_t* bgra, int width, int height,
                 int stride, std::wstring* error) {
    if (path.empty() || bgra == nullptr || width <= 0 || height <= 0 ||
        width > std::numeric_limits<int>::max() / 4 || stride < width * 4 ||
        static_cast<std::uint64_t>(stride) * static_cast<unsigned>(height) >
            std::numeric_limits<UINT>::max()) {
        return Fail(error, L"截图数据无效");
    }
    const std::filesystem::path outputPath(path);
    if (outputPath.has_parent_path()) {
        std::string directoryError;
        if (!EnsureDirectory(outputPath.parent_path(), &directoryError)) {
            return Fail(error, Utf8ToWide(directoryError));
        }
    }
    const std::filesystem::path temporaryPath = outputPath.wstring() + L".tmp";

    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(factory.GetAddressOf()));
    if (FAILED(hr)) {
        return Fail(error, L"无法创建 WIC 图像工厂：" + FormatHResultMessage(hr));
    }

    // CREATE_NEW semantics: an existing temporary file belongs to another
    // operation and must never be truncated or deleted by this save.
    ComPtr<IStream> stream;
    hr = ::SHCreateStreamOnFileEx(temporaryPath.c_str(),
        STGM_WRITE | STGM_SHARE_EXCLUSIVE | STGM_FAILIFTHERE,
        FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, stream.GetAddressOf());
    if (FAILED(hr)) {
        return Fail(error, L"无法创建 PNG 临时文件：" + FormatHResultMessage(hr));
    }

    ComPtr<IWICBitmapEncoder> encoder;
    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                encoder.GetAddressOf());
    if (SUCCEEDED(hr)) {
        hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    }
    if (FAILED(hr)) {
        encoder.Reset();
        stream.Reset();
        ::DeleteFileW(temporaryPath.c_str());
        return Fail(error, L"无法初始化 PNG 编码器：" + FormatHResultMessage(hr));
    }

    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    hr = encoder->CreateNewFrame(frame.GetAddressOf(), properties.GetAddressOf());
    if (SUCCEEDED(hr)) {
        hr = frame->Initialize(properties.Get());
    }
    if (SUCCEEDED(hr)) {
        hr = frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height));
    }
    if (SUCCEEDED(hr)) {
        hr = frame->SetResolution(96.0, 96.0);
    }
    if (SUCCEEDED(hr)) {
        WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
        hr = frame->SetPixelFormat(&format);
        if (SUCCEEDED(hr) && !::IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA)) {
            hr = WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
        }
    }
    if (SUCCEEDED(hr)) {
        hr = frame->WritePixels(static_cast<UINT>(height), static_cast<UINT>(stride),
                                static_cast<UINT>(stride) * static_cast<UINT>(height),
                                const_cast<BYTE*>(bgra));
    }
    if (SUCCEEDED(hr)) {
        hr = frame->Commit();
    }
    if (SUCCEEDED(hr)) {
        hr = encoder->Commit();
    }
    if (SUCCEEDED(hr)) {
        hr = stream->Commit(STGC_DEFAULT);
    }
    if (FAILED(hr)) {
        frame.Reset();
        encoder.Reset();
        stream.Reset();
        ::DeleteFileW(temporaryPath.c_str());
        return Fail(error, L"PNG 编码失败：" + FormatHResultMessage(hr));
    }
    frame.Reset();
    encoder.Reset();
    stream.Reset();

    if (!::MoveFileExW(temporaryPath.c_str(), outputPath.c_str(), MOVEFILE_WRITE_THROUGH)) {
        const DWORD lastError = ::GetLastError();
        ::DeleteFileW(temporaryPath.c_str());
        return Fail(error, L"无法保存截图文件：" +
                               FormatHResultMessage(HRESULT_FROM_WIN32(lastError)));
    }
    return true;
}

}  // namespace srl
