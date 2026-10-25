#include "capture/wgc_probe.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/base.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <format>
#include <mutex>
#include <vector>

#include "common/win32.h"
#include "platform/displays.h"

namespace srl {
namespace {

using Microsoft::WRL::ComPtr;
namespace Capture = winrt::Windows::Graphics::Capture;
namespace DirectX = winrt::Windows::Graphics::DirectX;
namespace Direct3D11 = winrt::Windows::Graphics::DirectX::Direct3D11;
namespace Foundation = winrt::Windows::Foundation;
namespace Metadata = winrt::Windows::Foundation::Metadata;

bool SameNameCaseInsensitive(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (std::towupper(a[i]) != std::towupper(b[i])) {
            return false;
        }
    }
    return true;
}

ComPtr<IDXGIAdapter1> FindAdapterForMonitor(const std::wstring& deviceName) {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())))) {
        return {};
    }
    for (UINT adapterIndex = 0;; ++adapterIndex) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(adapterIndex, adapter.GetAddressOf()) ==
            DXGI_ERROR_NOT_FOUND) {
            break;
        }
        for (UINT outputIndex = 0;; ++outputIndex) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(outputIndex, output.GetAddressOf()) ==
                DXGI_ERROR_NOT_FOUND) {
                break;
            }
            DXGI_OUTPUT_DESC desc{};
            if (SUCCEEDED(output->GetDesc(&desc)) &&
                SameNameCaseInsensitive(desc.DeviceName, deviceName)) {
                return adapter;
            }
        }
    }
    return {};
}

struct FrameWaitState {
    std::mutex mutex;
    std::condition_variable condition;
    bool arrived = false;
    Capture::Direct3D11CaptureFrame frame{nullptr};
    HRESULT handlerError = S_OK;
};

void SavePng(const std::wstring& path, int width, int height,
             const std::vector<std::uint8_t>& bgra, int stride) {
    ComPtr<IWICImagingFactory> factory;
    CheckHResult(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(factory.GetAddressOf())),
                 "CoCreateInstance(WIC)");
    ComPtr<IWICBitmap> bitmap;
    CheckHResult(factory->CreateBitmap(static_cast<UINT>(width), static_cast<UINT>(height),
                                       GUID_WICPixelFormat32bppBGRA, WICBitmapCacheOnLoad,
                                       bitmap.GetAddressOf()),
                 "CreateBitmap");
    {
        WICRect rect{0, 0, width, height};
        ComPtr<IWICBitmapLock> lock;
        CheckHResult(bitmap->Lock(&rect, WICBitmapLockWrite, lock.GetAddressOf()),
                     "IWICBitmap::Lock");
        UINT writableStride = 0;
        UINT size = 0;
        BYTE* data = nullptr;
        CheckHResult(lock->GetStride(&writableStride), "IWICBitmapLock::GetStride");
        CheckHResult(lock->GetDataPointer(&size, &data), "IWICBitmapLock::GetDataPointer");
        for (int y = 0; y < height; ++y) {
            std::memcpy(data + static_cast<std::size_t>(y) * writableStride,
                        bgra.data() + static_cast<std::size_t>(y) * stride,
                        static_cast<std::size_t>(width) * 4);
        }
    }

    ComPtr<IWICStream> stream;
    CheckHResult(factory->CreateStream(stream.GetAddressOf()), "CreateStream");
    CheckHResult(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE),
                 "IWICStream::InitializeFromFilename");
    ComPtr<IWICBitmapEncoder> encoder;
    CheckHResult(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr,
                                        encoder.GetAddressOf()),
                 "CreateEncoder(PNG)");
    CheckHResult(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache),
                 "IWICBitmapEncoder::Initialize");
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    CheckHResult(encoder->CreateNewFrame(frame.GetAddressOf(), properties.GetAddressOf()),
                 "CreateNewFrame");
    CheckHResult(frame->Initialize(properties.Get()), "IWICBitmapFrameEncode::Initialize");
    CheckHResult(frame->SetSize(static_cast<UINT>(width), static_cast<UINT>(height)),
                 "SetSize");
    CheckHResult(frame->SetResolution(96.0, 96.0), "SetResolution");
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    CheckHResult(frame->SetPixelFormat(&format), "SetPixelFormat");
    CheckHResult(frame->WriteSource(bitmap.Get(), nullptr), "WriteSource");
    CheckHResult(frame->Commit(), "frame Commit");
    CheckHResult(encoder->Commit(), "encoder Commit");
}

}  // namespace

WgcProbeResult RunWgcProbe(const WgcProbeOptions& options) {
    WgcProbeResult result;

    try {
        const std::vector<MonitorInfo> displays = EnumerateDisplays();
        const MonitorInfo* monitor = nullptr;
        if (options.monitorIndex == 0) {
            monitor = PrimaryMonitor(displays);
        } else {
            for (const MonitorInfo& candidate : displays) {
                if (candidate.index == options.monitorIndex) {
                    monitor = &candidate;
                    break;
                }
            }
        }
        if (monitor == nullptr) {
            result.error = L"selected display was not found";
            return result;
        }
        result.monitorName = monitor->deviceName + L" (" + monitor->friendlyName + L")";
        result.advancedColorActive = IsAdvancedColorActive(*monitor);
        if (result.advancedColorActive && !options.allowAdvancedColor) {
            result.error =
                L"target display has HDR/advanced color enabled; SDR capture is blocked "
                L"(pass --allow-advanced-color only for diagnostics)";
            return result;
        }

        if (!Metadata::ApiInformation::IsTypePresent(
                L"Windows.Graphics.Capture.GraphicsCaptureSession")) {
            result.error = L"Windows.Graphics.Capture is not available";
            return result;
        }
        if (Metadata::ApiInformation::IsMethodPresent(
                L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsSupported") &&
            !Capture::GraphicsCaptureSession::IsSupported()) {
            result.error = L"GraphicsCaptureSession.IsSupported() returned false";
            return result;
        }

        ComPtr<IDXGIAdapter1> adapter = FindAdapterForMonitor(monitor->deviceName);
        const D3D_DRIVER_TYPE driverType =
            adapter != nullptr ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
        ComPtr<ID3D11Device> d3dDevice;
        ComPtr<ID3D11DeviceContext> d3dContext;
        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
        const UINT createFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
        HRESULT hr = ::D3D11CreateDevice(
            adapter.Get(), driverType, nullptr, createFlags, nullptr, 0, D3D11_SDK_VERSION,
            d3dDevice.GetAddressOf(), &featureLevel, d3dContext.GetAddressOf());
        if (FAILED(hr)) {
            result.error =
                Utf8ToWide(std::format("D3D11CreateDevice failed: {}", HResultText(hr)));
            return result;
        }

        ComPtr<IDXGIDevice> dxgiDevice;
        CheckHResult(d3dDevice.As(&dxgiDevice), "ID3D11Device::QueryInterface(IDXGIDevice)");
        winrt::com_ptr<IInspectable> inspectableDevice;
        CheckHResult(::CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(),
                                                            inspectableDevice.put()),
                     "CreateDirect3D11DeviceFromDXGIDevice");
        Direct3D11::IDirect3DDevice winrtDevice{nullptr};
        CheckHResult(inspectableDevice->QueryInterface(
                         winrt::guid_of<Direct3D11::IDirect3DDevice>(),
                         winrt::put_abi(winrtDevice)),
                     "QueryInterface(IDirect3DDevice)");

        const auto interop = winrt::get_activation_factory<
            Capture::GraphicsCaptureItem, ::IGraphicsCaptureItemInterop>();
        Capture::GraphicsCaptureItem item{nullptr};
        CheckHResult(interop->CreateForMonitor(
                         monitor->handle, winrt::guid_of<Capture::GraphicsCaptureItem>(),
                         winrt::put_abi(item)),
                     "IGraphicsCaptureItemInterop::CreateForMonitor");

        const auto itemSize = item.Size();
        if (itemSize.Width <= 0 || itemSize.Height <= 0) {
            result.error = L"capture item reported an empty size";
            return result;
        }
        result.width = itemSize.Width;
        result.height = itemSize.Height;

        auto framePool = Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
            winrtDevice, DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, itemSize);
        auto session = framePool.CreateCaptureSession(item);
        try {
            session.IsCursorCaptureEnabled(true);
        } catch (const winrt::hresult_error& error) {
            result.error = Utf8ToWide(
                std::format("cursor capture configuration failed: {}",
                            HResultText(error.code())));
            return result;
        }

        FrameWaitState waitState;
        const auto token = framePool.FrameArrived(
            [&waitState](const Capture::Direct3D11CaptureFramePool& sender,
                         const Foundation::IInspectable&) {
                try {
                    auto frame = sender.TryGetNextFrame();
                    if (frame != nullptr) {
                        std::lock_guard lock(waitState.mutex);
                        waitState.frame = frame;
                        waitState.arrived = true;
                        waitState.condition.notify_one();
                    }
                } catch (const winrt::hresult_error& error) {
                    std::lock_guard lock(waitState.mutex);
                    waitState.handlerError = error.code();
                    waitState.arrived = true;
                    waitState.condition.notify_one();
                }
            });

        session.StartCapture();
        result.sessionStarted = true;

        Capture::Direct3D11CaptureFrame frame{nullptr};
        {
            std::unique_lock lock(waitState.mutex);
            if (!waitState.condition.wait_for(lock,
                                              std::chrono::milliseconds(options.timeoutMs),
                                              [&waitState] { return waitState.arrived; })) {
                result.error = Utf8ToWide(std::format(
                    "no frame arrived within {} ms", options.timeoutMs));
                framePool.FrameArrived(token);
                session.Close();
                framePool.Close();
                return result;
            }
            frame = waitState.frame;
        }
        framePool.FrameArrived(token);
        if (waitState.handlerError != S_OK) {
            result.error = Utf8ToWide(
                std::format("frame pool error: {}", HResultText(waitState.handlerError)));
            session.Close();
            framePool.Close();
            return result;
        }
        if (frame == nullptr) {
            result.error = L"frame pool did not produce a frame";
            session.Close();
            framePool.Close();
            return result;
        }

        const auto surface = frame.Surface();
        const auto access =
            surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        ComPtr<ID3D11Texture2D> texture;
        CheckHResult(access->GetInterface(IID_PPV_ARGS(texture.GetAddressOf())),
                     "IDirect3DDxgiInterfaceAccess::GetInterface(ID3D11Texture2D)");

        D3D11_TEXTURE2D_DESC description{};
        texture->GetDesc(&description);
        if (description.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
            result.pixelFormat = Utf8ToWide(std::format(
                "DXGI_FORMAT_{}", static_cast<int>(description.Format)));
            result.error = L"captured texture is not BGRA8; HDR capture is not supported";
            frame.Close();
            session.Close();
            framePool.Close();
            return result;
        }
        result.pixelFormat = L"B8G8R8A8_UNORM";

        D3D11_TEXTURE2D_DESC stagingDesc = description;
        stagingDesc.Usage = D3D11_USAGE_STAGING;
        stagingDesc.BindFlags = 0;
        stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stagingDesc.MiscFlags = 0;
        stagingDesc.SampleDesc.Count = 1;
        stagingDesc.SampleDesc.Quality = 0;
        stagingDesc.MipLevels = 1;
        stagingDesc.ArraySize = 1;
        ComPtr<ID3D11Texture2D> staging;
        CheckHResult(d3dDevice->CreateTexture2D(&stagingDesc, nullptr,
                                                staging.GetAddressOf()),
                     "CreateTexture2D(staging)");
        d3dContext->CopyResource(staging.Get(), texture.Get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        CheckHResult(d3dContext->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
                     "ID3D11DeviceContext::Map");
        const auto sampleAt = [&](int x, int y) -> std::uint32_t {
            x = std::clamp(x, 0, result.width - 1);
            y = std::clamp(y, 0, result.height - 1);
            const auto* row = static_cast<const std::uint8_t*>(mapped.pData) +
                              static_cast<std::size_t>(y) * mapped.RowPitch;
            const std::uint8_t b = row[x * 4];
            const std::uint8_t g = row[x * 4 + 1];
            const std::uint8_t r = row[x * 4 + 2];
            const std::uint8_t a = row[x * 4 + 3];
            return (static_cast<std::uint32_t>(a) << 24) |
                   (static_cast<std::uint32_t>(r) << 16) |
                   (static_cast<std::uint32_t>(g) << 8) |
                   static_cast<std::uint32_t>(b);
        };

        if (options.samplePoints.empty()) {
            result.sampleX = result.width / 2;
            result.sampleY = result.height / 2;
            result.sampleBgra = sampleAt(result.sampleX, result.sampleY);
            result.samples.push_back(result.sampleBgra);
        } else {
            for (const POINT& point : options.samplePoints) {
                const std::uint32_t value = sampleAt(point.x, point.y);
                result.samples.push_back(value);
                if (result.samples.size() == 1) {
                    result.sampleX = point.x;
                    result.sampleY = point.y;
                    result.sampleBgra = value;
                }
            }
        }

        if (options.savePng && !options.pngPath.empty()) {
            result.pngPath = options.pngPath;
            const std::filesystem::path pngPath(options.pngPath);
            if (pngPath.has_parent_path()) {
                std::string directoryError;
                if (!EnsureDirectory(pngPath.parent_path(), &directoryError)) {
                    result.error = Utf8ToWide(directoryError);
                }
            }
            if (result.error.empty()) {
                const int stride = result.width * 4;
                std::vector<std::uint8_t> pixels(
                    static_cast<std::size_t>(stride) * result.height);
                for (int y = 0; y < result.height; ++y) {
                    std::memcpy(pixels.data() + static_cast<std::size_t>(y) * stride,
                                static_cast<const std::uint8_t*>(mapped.pData) +
                                    static_cast<std::size_t>(y) * mapped.RowPitch,
                                static_cast<std::size_t>(stride));
                }
                try {
                    SavePng(options.pngPath, result.width, result.height, pixels, stride);
                    result.pngSaved = true;
                } catch (const std::exception& error) {
                    result.error =
                        L"PNG save failed: " + Utf8ToWide(error.what());
                }
            }
        }

        d3dContext->Unmap(staging.Get(), 0);
        frame.Close();
        session.Close();
        framePool.Close();

        result.ok = result.error.empty();
        return result;
    } catch (const winrt::hresult_error& error) {
        result.error = Utf8ToWide(std::format("WinRT error: {}", HResultText(error.code())));
        return result;
    } catch (const std::exception& error) {
        result.error = Utf8ToWide(error.what());
        return result;
    }
}

void PrintWgcProbe(const WgcProbeResult& result) {
    ConsoleLine("WGC capture");
    ConsoleLine(std::format("  monitor            {}", WideToUtf8(result.monitorName)));
    if (!result.error.empty()) {
        ConsoleLine(std::format("  error              {}", WideToUtf8(result.error)));
    }
    ConsoleLine(std::format("  advanced color     {}", result.advancedColorActive ? "active" : "no"));
    ConsoleLine(std::format("  session started    {}", result.sessionStarted ? "yes" : "no"));
    if (result.width > 0 && result.height > 0) {
        ConsoleLine(std::format("  frame              {}x{} {}", result.width, result.height,
                                WideToUtf8(result.pixelFormat)));
        ConsoleLine(std::format("  sample @({},{})    0x{:08X}", result.sampleX, result.sampleY,
                                result.sampleBgra));
        if (result.samples.size() > 1) {
            ConsoleLine(std::format("  samples            {}", result.samples.size()));
            for (std::size_t i = 0; i < result.samples.size(); ++i) {
                ConsoleLine(std::format("    #{} 0x{:08X}", i + 1, result.samples[i]));
            }
        }
    }
    if (!result.pngPath.empty()) {
        ConsoleLine(std::format("  png                {} ({})",
                                WideToUtf8(result.pngPath),
                                result.pngSaved ? "saved" : "not saved"));
    }
    ConsoleLine(std::format("  result             {}", result.ok ? "OK" : "FAILED"));
}

}  // namespace srl
