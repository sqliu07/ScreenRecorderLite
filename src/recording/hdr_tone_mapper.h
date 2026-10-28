#pragma once

#include <d3d11.h>
#include <wrl/client.h>

namespace srl {

// Converts WGC's linear FP16/scRGB surface to displayable SDR BGRA on the GPU.
// The returned texture belongs to this converter and is reused on the next call.
class HdrToneMapper {
public:
    HdrToneMapper(ID3D11Device* device, ID3D11DeviceContext* context);
    [[nodiscard]] ID3D11Texture2D* Convert(ID3D11Texture2D* source,
                                          float sdrWhiteScale, bool hdr);
private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertexShader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixelShader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> parameters_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> input_;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> inputView_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> output_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> outputView_;
    UINT width_ = 0;
    UINT height_ = 0;
};
}  // namespace srl
