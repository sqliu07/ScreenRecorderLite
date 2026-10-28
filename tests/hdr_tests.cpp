#include <windows.h>
#include <d3d11.h>
#include <DirectXPackedVector.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

#include "common/win32.h"
#include "core/hdr_tone_map.h"
#include "platform/displays.h"
#include "recording/hdr_tone_mapper.h"

namespace {
int checks = 0;
int failures = 0;
void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) { ++failures; std::printf("FAIL: %s\n", message); }
}
void CheckReference() {
    const auto black = srl::ToneMapScRgbToSrgb({0, 0, 0}, 1, true);
    Check(black == std::array<float, 3>{0, 0, 0}, "HDR black stays black");
    const auto white = srl::ToneMapScRgbToSrgb({1, 1, 1}, 1, true);
    Check(white[0] > 0.96f && white[0] < 1.0f && white[0] == white[1] && white[1] == white[2],
          "SDR white remains close to white with headroom for HDR highlights");
    const auto boosted = srl::ToneMapScRgbToSrgb({2, 2, 2}, 2, true);
    Check(boosted == white, "per-target SDR white level removes Windows brightness boost");
    const auto gray = srl::ToneMapScRgbToSrgb({0.18f, 0.18f, 0.18f}, 1, true);
    const auto sdrGray = srl::ToneMapScRgbToSrgb({0.18f, 0.18f, 0.18f}, 1, false);
    Check(gray == sdrGray && gray[0] > 0.46f && gray[0] < 0.47f,
          "SDR midtones are unchanged below the highlight shoulder");
    const auto highlight = srl::ToneMapScRgbToSrgb({2, 2, 2}, 1, true);
    Check(highlight[0] > white[0] && highlight[0] <= 1.0f,
          "HDR highlights compress above SDR white instead of clipping at capture");
    float previous = 0;
    bool monotonic = true;
    for (int step = 0; step <= 100; ++step) {
        const float input = static_cast<float>(step) / 25.0f;
        const float output = srl::ToneMapScRgbToSrgb({input, input, input}, 1, true)[0];
        monotonic &= output >= previous && output >= 0 && output <= 1;
        previous = output;
    }
    Check(monotonic, "the HDR shoulder is monotonic and bounded");
    const auto invalid = srl::ToneMapScRgbToSrgb(
        {std::numeric_limits<float>::quiet_NaN(), -1.0f, std::numeric_limits<float>::infinity()},
        std::numeric_limits<float>::quiet_NaN(), true);
    Check(invalid[0] == 0 && invalid[1] == 0 && invalid[2] == 1,
          "nonfinite and negative capture values cannot poison the output");
    Check(srl::NormalizeSdrWhiteScale(0) == 1 && srl::NormalizeSdrWhiteScale(-1) == 1 &&
          srl::NormalizeSdrWhiteScale(2.5f) == 2.5f, "invalid display white levels have a safe reference");

    srl::MonitorInfo primary;
    primary.primary = true;
    primary.advancedColorSupported = true;
    srl::MonitorInfo secondary;
    secondary.advancedColorSupported = true;
    secondary.advancedColorEnabled = true;
    secondary.hdrEnabled = true;
    const std::vector<srl::MonitorInfo> mixed{primary, secondary};
    Check(srl::PrimaryMonitor(mixed) == &mixed[0] && !srl::IsAdvancedColorActive(mixed[0]) &&
          !srl::IsHdrActive(mixed[0]) && srl::IsHdrActive(mixed[1]),
          "HDR on a separate secondary display does not classify the SDR primary as HDR");
    primary.advancedColorEnabled = true;
    primary.wideColorEnforced = true;
    Check(srl::IsAdvancedColorActive(primary) && !srl::IsHdrActive(primary),
          "wide-color SDR is distinct from HDR and remains capturable");
    primary.advancedColorEnabled = false;
    primary.wideColorEnforced = false;
    Check(!srl::IsHdrActive(primary), "HDR capability alone does not mean HDR is active");
}

void CheckGpu() {
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    srl::CheckHResult(::D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
        device.GetAddressOf(), &level, context.GetAddressOf()), "Create WARP test device");
    srl::HdrToneMapper converter(device.Get(), context.Get());
    const std::array<std::array<float, 3>, 6> colors{{
        {0, 0, 0}, {1, 1, 1}, {2, 2, 2}, {0.5f, 0.25f, 0.125f},
        {-1, std::numeric_limits<float>::quiet_NaN(), 0},
        {std::numeric_limits<float>::infinity(), 0, 0}}};
    for (const bool hdr : {false, true}) {
        for (const float whiteScale : {1.0f, 2.0f}) {
            // Padding and a second row exercise the real texture/readback stride.
            std::array<std::uint16_t, 64> half{};
            constexpr UINT width = static_cast<UINT>(colors.size());
            constexpr UINT height = 2;
            constexpr UINT sourceStride = 64;
            for (UINT row = 0; row < height; ++row) {
                for (UINT pixel = 0; pixel < width; ++pixel) {
                    const auto& color = colors[row == 0 ? pixel : width - pixel - 1];
                    auto* destination = half.data() + row * sourceStride / sizeof(std::uint16_t) + pixel * 4;
                    for (int channel = 0; channel < 3; ++channel)
                        destination[channel] = DirectX::PackedVector::XMConvertFloatToHalf(color[channel]);
                    destination[3] = DirectX::PackedVector::XMConvertFloatToHalf(1.0f);
                }
            }
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = width; desc.Height = height;
            desc.MipLevels = 1; desc.ArraySize = 1;
            desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            const D3D11_SUBRESOURCE_DATA data{half.data(), sourceStride, 0};
            ComPtr<ID3D11Texture2D> source;
            srl::CheckHResult(device->CreateTexture2D(&desc, &data, source.GetAddressOf()), "Create FP16 test texture");
            ID3D11Texture2D* converted = converter.Convert(source.Get(), whiteScale, hdr);
            converted->GetDesc(&desc);
            Check(desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM && desc.Width == width && desc.Height == height,
                  "GPU conversion produces SDR BGRA with the original dimensions");
            desc.Usage = D3D11_USAGE_STAGING;
            desc.BindFlags = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> readback;
            srl::CheckHResult(device->CreateTexture2D(&desc, nullptr, readback.GetAddressOf()), "Create HDR test readback");
            context->CopyResource(readback.Get(), converted);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            srl::CheckHResult(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map HDR test readback");
            bool pixelsMatch = true;
            for (UINT row = 0; row < height; ++row) {
                const auto* bytes = static_cast<const std::uint8_t*>(mapped.pData) + row * mapped.RowPitch;
                for (UINT pixel = 0; pixel < width; ++pixel) {
                    const auto expected = srl::ToneMapScRgbToSrgb(
                        colors[row == 0 ? pixel : width - pixel - 1], whiteScale, hdr);
                    for (int channel = 0; channel < 3; ++channel) {
                        const int wanted = static_cast<int>(std::lround(expected[channel] * 255.0f));
                        const int actual = bytes[pixel * 4 + 2 - channel];
                        if (std::abs(actual - wanted) > 1)
                            std::printf("HDR mismatch hdr=%d white=%.1f xy=%u,%u channel=%d got=%d expected=%d\n",
                                hdr, whiteScale, pixel, row, channel, actual, wanted);
                        pixelsMatch &= std::abs(actual - wanted) <= 1;
                    }
                    pixelsMatch &= bytes[pixel * 4 + 3] == 255;
                }
            }
            context->Unmap(readback.Get(), 0);
            Check(pixelsMatch, "actual GPU shader matches reference tone mapping, normalization and BGRA channel order");
        }
    }
}
}

int main() {
    try { CheckReference(); CheckGpu(); }
    catch (const std::exception& error) {
        std::printf("FAIL: HDR test failed: %s\n", error.what());
        ++failures;
    }
    std::printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
