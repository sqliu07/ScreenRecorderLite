#include "recording/hdr_tone_mapper.h"

#include <d3dcompiler.h>

#include <cstring>

#include "common/win32.h"
#include "core/hdr_tone_map.h"

namespace srl {
namespace {
constexpr char kShader[] = R"(
Texture2D<float4> Source : register(t0);
cbuffer Parameters : register(b0) { float InverseWhite; float ShoulderStart; float ShoulderRange; float IsHdr; };
float4 Vertex(uint id : SV_VertexID) : SV_Position {
    return float4(id == 2 ? 3.0 : -1.0, id == 1 ? 3.0 : -1.0, 0.0, 1.0);
}
float Sanitize(float value) {
    // Bit classification stays well-defined even when shader optimizations
    // otherwise assume finite values for floating-point comparisons.
    uint bits = asuint(value);
    if ((bits & 0x7fffffffU) > 0x7f800000U || (bits & 0x80000000U) != 0U) return 0.0;
    return min(value, 65504.0);
}
float Encode(float value) {
    value = saturate(value);
    return value <= 0.0031308 ? 12.92 * value : 1.055 * pow(value, 1.0 / 2.4) - 0.055;
}
float4 Pixel(float4 position : SV_Position) : SV_Target {
    float3 raw = Source.Load(int3(int2(position.xy), 0)).rgb;
    float3 color = float3(Sanitize(raw.r), Sanitize(raw.g), Sanitize(raw.b)) * InverseWhite;
    float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
    if (IsHdr > 0.5 && luminance > ShoulderStart) {
        float mapped = ShoulderStart + ShoulderRange * (1.0 - exp(-(luminance - ShoulderStart) / ShoulderRange));
        color *= mapped / luminance;
    }
    return float4(Encode(color.r), Encode(color.g), Encode(color.b), 1.0);
}
)";

Microsoft::WRL::ComPtr<ID3DBlob> Compile(const char* entry, const char* profile) {
    Microsoft::WRL::ComPtr<ID3DBlob> bytecode;
    CheckHResult(::D3DCompile(kShader, std::strlen(kShader), "HDR to SDR", nullptr,
        nullptr, entry, profile, D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_IEEE_STRICTNESS |
            D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0, bytecode.GetAddressOf(), nullptr), "D3DCompile(HDR to SDR)");
    return bytecode;
}
}

HdrToneMapper::HdrToneMapper(ID3D11Device* device, ID3D11DeviceContext* context)
    : device_(device), context_(context) {
    const auto vertex = Compile("Vertex", "vs_4_0");
    const auto pixel = Compile("Pixel", "ps_4_0");
    CheckHResult(device_->CreateVertexShader(vertex->GetBufferPointer(), vertex->GetBufferSize(),
        nullptr, vertexShader_.GetAddressOf()), "CreateVertexShader(HDR to SDR)");
    CheckHResult(device_->CreatePixelShader(pixel->GetBufferPointer(), pixel->GetBufferSize(),
        nullptr, pixelShader_.GetAddressOf()), "CreatePixelShader(HDR to SDR)");
    D3D11_BUFFER_DESC parameters{};
    parameters.ByteWidth = sizeof(float) * 4;
    parameters.Usage = D3D11_USAGE_DEFAULT;
    parameters.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    CheckHResult(device_->CreateBuffer(&parameters, nullptr, parameters_.GetAddressOf()),
                 "CreateBuffer(HDR to SDR)");
}

ID3D11Texture2D* HdrToneMapper::Convert(ID3D11Texture2D* source,
                                       float sdrWhiteScale, bool hdr) {
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    if (!input_ || width_ != desc.Width || height_ != desc.Height) {
        input_.Reset(); inputView_.Reset(); output_.Reset(); outputView_.Reset();
        D3D11_TEXTURE2D_DESC texture = desc;
        texture.Usage = D3D11_USAGE_DEFAULT;
        texture.CPUAccessFlags = 0;
        texture.MiscFlags = 0;
        texture.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        CheckHResult(device_->CreateTexture2D(&texture, nullptr, input_.GetAddressOf()),
                     "CreateTexture2D(HDR input)");
        CheckHResult(device_->CreateShaderResourceView(input_.Get(), nullptr,
            inputView_.GetAddressOf()), "CreateShaderResourceView(HDR input)");
        texture.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        texture.BindFlags = D3D11_BIND_RENDER_TARGET;
        CheckHResult(device_->CreateTexture2D(&texture, nullptr, output_.GetAddressOf()),
                     "CreateTexture2D(SDR output)");
        CheckHResult(device_->CreateRenderTargetView(output_.Get(), nullptr,
            outputView_.GetAddressOf()), "CreateRenderTargetView(SDR output)");
        width_ = desc.Width;
        height_ = desc.Height;
    }
    context_->CopyResource(input_.Get(), source);
    const float parameters[] = {1.0f / NormalizeSdrWhiteScale(sdrWhiteScale),
        kHdrShoulderStart, kHdrShoulderRange, hdr ? 1.0f : 0.0f};
    context_->UpdateSubresource(parameters_.Get(), 0, nullptr, parameters, 0, 0);
    ID3D11Buffer* buffer = parameters_.Get();
    ID3D11ShaderResourceView* resource = inputView_.Get();
    ID3D11RenderTargetView* target = outputView_.Get();
    const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(width_), static_cast<float>(height_), 0, 1};
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
    context_->PSSetConstantBuffers(0, 1, &buffer);
    context_->PSSetShaderResources(0, 1, &resource);
    context_->OMSetRenderTargets(1, &target, nullptr);
    context_->RSSetViewports(1, &viewport);
    context_->Draw(3, 0);
    // Release pipeline bindings before CopyResource/readback or the next frame.
    resource = nullptr;
    target = nullptr;
    context_->PSSetShaderResources(0, 1, &resource);
    context_->OMSetRenderTargets(1, &target, nullptr);
    return output_.Get();
}
}  // namespace srl
