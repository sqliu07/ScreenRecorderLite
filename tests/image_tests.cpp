#include "common/win32.h"
#include "image/png_writer.h"

#include <objbase.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

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

std::string Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

}  // namespace

int main() {
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) {
        return 1;
    }
    const auto directory = std::filesystem::path(srl::GetTempDirectory()) /
        (L"SRL_image_tests_" + std::to_wstring(::GetCurrentProcessId()) + L"_" +
         srl::CurrentTimestampForFileName());
    std::filesystem::create_directories(directory);
    const std::array<std::uint8_t, 16> pixels = {
        0, 0, 255, 255, 0, 255, 0, 255,
        255, 0, 0, 255, 255, 255, 255, 255};
    std::wstring error;

    const auto path = directory / L"test.png";
    Check(srl::SaveBgraPng(path.wstring(), pixels.data(), 2, 2, 8, &error),
          "PNG save succeeds");
    Check(!std::filesystem::exists(path.wstring() + L".tmp"),
          "successful PNG save removes temporary name");
    const auto original = Read(path);
    Check(!srl::SaveBgraPng(path.wstring(), pixels.data(), 2, 2, 8, &error),
          "existing final PNG is never replaced");
    Check(Read(path) == original, "existing final PNG remains identical");

    // The PNG must be readable and preserve the source dimensions and colors.
    {
        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        HRESULT hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr,
            CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf()));
        Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
        if (SUCCEEDED(hr)) {
            hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf());
        }
        Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
        if (SUCCEEDED(hr)) {
            hr = decoder->GetFrame(0, frame.GetAddressOf());
        }
        UINT width = 0;
        UINT height = 0;
        if (SUCCEEDED(hr)) {
            hr = frame->GetSize(&width, &height);
        }
        Check(SUCCEEDED(hr) && width == 2 && height == 2, "PNG dimensions round trip");
        Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
        if (SUCCEEDED(hr)) {
            hr = factory->CreateFormatConverter(converter.GetAddressOf());
        }
        if (SUCCEEDED(hr)) {
            hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
                WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
        }
        std::array<std::uint8_t, 16> decoded{};
        if (SUCCEEDED(hr)) {
            hr = converter->CopyPixels(nullptr, 8, static_cast<UINT>(decoded.size()),
                                       decoded.data());
        }
        Check(SUCCEEDED(hr) && decoded == pixels, "PNG pixels round trip losslessly");
    }

    const auto blocked = directory / L"blocked.png";
    const std::filesystem::path temporary = blocked.wstring() + L".tmp";
    const std::string marker = "temporary file owned by another operation";
    std::ofstream(temporary, std::ios::binary) << marker;
    Check(!srl::SaveBgraPng(blocked.wstring(), pixels.data(), 2, 2, 8, &error),
          "existing temporary PNG is not opened for writing");
    Check(Read(temporary) == marker, "another operation's temporary file is preserved");
    Check(!std::filesystem::exists(blocked), "temporary collision creates no final PNG");
    Check(!error.empty(), "temporary collision explains the failure");
    Check(!srl::SaveBgraPng((directory / L"invalid.png").wstring(), pixels.data(),
                           2, 2, 7, &error), "short PNG stride is rejected");

    std::filesystem::remove_all(directory);
    ::CoUninitialize();
    std::printf("%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
