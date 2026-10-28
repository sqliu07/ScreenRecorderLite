#include "recording/screen_capture.h"

#include <d3d11.h>
#include <dxgi1_2.h>
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
#include <format>
#include <limits>
#include <mutex>

#include "common/win32.h"
#include "core/recording_format.h"
#include "platform/displays.h"
#include "recording/hdr_tone_mapper.h"

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

ComPtr<IDXGIAdapter1> FindAdapterForMonitor(HMONITOR monitor) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!::GetMonitorInfoW(monitor, &info)) {
        return {};
    }

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
                SameNameCaseInsensitive(desc.DeviceName, info.szDevice)) {
                return adapter;
            }
        }
    }
    return {};
}

}  // namespace

namespace detail {

bool CopyScreenshotRegion(const std::vector<std::uint8_t>& source, Size size,
                          const RECT& region, ScreenshotFrame& frame,
                          std::wstring* error) {
    frame = {};
    if (size.width <= 0 || size.height <= 0 ||
        size.width > std::numeric_limits<int>::max() / 4 ||
        region.left < 0 || region.top < 0 || region.right <= region.left ||
        region.bottom <= region.top || region.right > size.width ||
        region.bottom > size.height ||
        source.size() < static_cast<std::size_t>(size.width) * size.height * 4) {
        if (error != nullptr) {
            *error = L"截图区域无效或超出当前画面";
        }
        return false;
    }
    frame.width = region.right - region.left;
    frame.height = region.bottom - region.top;
    const auto stride = static_cast<std::size_t>(frame.width) * 4;
    frame.bgra.resize(stride * frame.height);
    for (int row = 0; row < frame.height; ++row) {
        const auto sourceOffset =
            (static_cast<std::size_t>(region.top + row) * size.width + region.left) * 4;
        auto* destination = frame.bgra.data() + static_cast<std::size_t>(row) * stride;
        std::memcpy(destination, source.data() + sourceOffset, stride);
        for (int column = 0; column < frame.width; ++column) {
            destination[static_cast<std::size_t>(column) * 4 + 3] = 255;
        }
    }
    return true;
}

}  // namespace detail

std::int64_t QpcNow100ns() {
    static const std::int64_t frequency = [] {
        LARGE_INTEGER value{};
        ::QueryPerformanceFrequency(&value);
        return value.QuadPart;
    }();
    LARGE_INTEGER counter{};
    ::QueryPerformanceCounter(&counter);
    if (frequency <= 0) {
        return 0;
    }
    const std::int64_t whole = counter.QuadPart / frequency;
    const std::int64_t remainder = counter.QuadPart % frequency;
    return whole * kHundredNanosecondsPerSecond + remainder * kHundredNanosecondsPerSecond / frequency;
}

struct ScreenCapture::Impl : std::enable_shared_from_this<ScreenCapture::Impl> {
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::vector<std::uint8_t> latest;
    int width = 0;
    int height = 0;
    std::int64_t latestQpc100ns = 0;
    std::int64_t firstQpc100ns = 0;
    std::uint64_t generation = 0;
    std::uint64_t provided = 0;
    bool firstAvailable = false;
    bool stopped = false;
    std::wstring error;

    std::mutex copyMutex;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> staging;
    UINT stagingWidth = 0;
    UINT stagingHeight = 0;
    DXGI_FORMAT stagingFormat = DXGI_FORMAT_UNKNOWN;
    MonitorInfo colorMonitor;
    bool floatCapture = false;
    std::unique_ptr<HdrToneMapper> toneMapper;
    std::int64_t nextColorCheck100ns = 0;

    Capture::Direct3D11CaptureFramePool pool{nullptr};
    Capture::GraphicsCaptureSession session{nullptr};
    Capture::GraphicsCaptureItem item{nullptr};
    winrt::event_token frameToken{};
    winrt::event_token itemClosedToken{};
    bool hasFrameToken = false;
    bool hasItemClosedToken = false;
    Size frameSize{};
    int fps = 30;
    std::int64_t minInterval100ns = 0;

    void SetError(std::wstring message) {
        {
            std::lock_guard lock(mutex);
            if (error.empty()) {
                error = std::move(message);
            }
        }
        condition.notify_all();
    }

    bool HasError() const {
        std::lock_guard lock(mutex);
        return !error.empty();
    }

    std::wstring ErrorText() const {
        std::lock_guard lock(mutex);
        return error;
    }

    void HandleFrame(Capture::Direct3D11CaptureFramePool const& sender) {
        {
            std::lock_guard lock(mutex);
            if (stopped) {
                return;
            }
        }

        try {
            auto frame = sender.TryGetNextFrame();
            if (frame == nullptr) {
                return;
            }

            const auto surface = frame.Surface();
            const auto access =
                surface.as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            ComPtr<ID3D11Texture2D> texture;
            CheckHResult(access->GetInterface(IID_PPV_ARGS(texture.GetAddressOf())),
                         "GetInterface(ID3D11Texture2D)");

            D3D11_TEXTURE2D_DESC desc{};
            texture->GetDesc(&desc);
            const DXGI_FORMAT expected = floatCapture ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                                       : DXGI_FORMAT_B8G8R8A8_UNORM;
            if (desc.Format != expected) {
                SetError(L"captured texture format changed unexpectedly");
                frame.Close();
                return;
            }
            const auto contentSize = frame.ContentSize();
            if (contentSize.Width != frameSize.width ||
                contentSize.Height != frameSize.height ||
                desc.Width != static_cast<UINT>(frameSize.width) ||
                desc.Height != static_cast<UINT>(frameSize.height)) {
                SetError(L"capture frame size changed");
                frame.Close();
                return;
            }

            {
                std::lock_guard copyLock(copyMutex);
                const std::int64_t now = QpcNow100ns();
                {
                    std::lock_guard lock(mutex);
                    if (stopped || (firstAvailable && minInterval100ns > 0 &&
                                    now - latestQpc100ns < minInterval100ns)) {
                        frame.Close();
                        return;
                    }
                }
                if (now >= nextColorCheck100ns) {
                    nextColorCheck100ns = now + kHundredNanosecondsPerSecond;
                    MonitorInfo current = colorMonitor;
                    if (RefreshDisplayColor(current)) {
                        const bool needsFloat = IsAdvancedColorActive(current) || !current.colorInfoAvailable;
                        if (needsFloat != floatCapture || current.hdrEnabled != colorMonitor.hdrEnabled) {
                            SetError(L"display color mode changed; start a new recording");
                            frame.Close();
                            return;
                        }
                        colorMonitor = std::move(current);
                    }
                }
                if (toneMapper) {
                    texture = toneMapper->Convert(texture.Get(),
                        colorMonitor.hdrEnabled ? colorMonitor.sdrWhiteScale : 1.0f,
                        colorMonitor.hdrEnabled);
                    texture->GetDesc(&desc);
                }
                if (staging == nullptr || stagingWidth != desc.Width ||
                    stagingHeight != desc.Height || stagingFormat != desc.Format) {
                    D3D11_TEXTURE2D_DESC stagingDesc = desc;
                    stagingDesc.Usage = D3D11_USAGE_STAGING;
                    stagingDesc.BindFlags = 0;
                    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
                    stagingDesc.MiscFlags = 0;
                    stagingDesc.MipLevels = 1;
                    stagingDesc.ArraySize = 1;
                    stagingDesc.SampleDesc.Count = 1;
                    stagingDesc.SampleDesc.Quality = 0;
                    staging.Reset();
                    CheckHResult(device->CreateTexture2D(&stagingDesc, nullptr,
                                                         staging.GetAddressOf()),
                                 "CreateTexture2D(staging)");
                    stagingWidth = desc.Width;
                    stagingHeight = desc.Height;
                    stagingFormat = desc.Format;
                }

                context->CopyResource(staging.Get(), texture.Get());
                D3D11_MAPPED_SUBRESOURCE mapped{};
                CheckHResult(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped),
                             "ID3D11DeviceContext::Map");

                {
                    std::lock_guard lock(mutex);
                    if (stopped) {
                        context->Unmap(staging.Get(), 0);
                        frame.Close();
                        return;
                    }
                    const bool isFirst = !firstAvailable;
                    latest.resize(static_cast<std::size_t>(desc.Width) * desc.Height * 4);
                    for (UINT y = 0; y < desc.Height; ++y) {
                        std::memcpy(
                            latest.data() + static_cast<std::size_t>(y) * desc.Width * 4,
                            static_cast<const std::uint8_t*>(mapped.pData) +
                                static_cast<std::size_t>(y) * mapped.RowPitch,
                            static_cast<std::size_t>(desc.Width) * 4);
                    }
                    width = static_cast<int>(desc.Width);
                    height = static_cast<int>(desc.Height);
                    latestQpc100ns = now;
                    if (isFirst) {
                        firstQpc100ns = now;
                        firstAvailable = true;
                    }
                    ++generation;
                }
                condition.notify_all();
                context->Unmap(staging.Get(), 0);
            }
            frame.Close();
        } catch (const winrt::hresult_error& error) {
            SetError(Utf8ToWide(std::format("frame handler failed: {}",
                                            HResultText(error.code()))));
        } catch (const std::exception& error) {
            SetError(Utf8ToWide(error.what()));
        }
    }
};

ScreenCapture::ScreenCapture() : impl_(std::make_shared<Impl>()) {}

ScreenCapture::~ScreenCapture() {
    Stop();
}

bool ScreenCapture::Start(HMONITOR monitor, int fps, std::wstring* error) {
    if (impl_ == nullptr) {
        return false;
    }
    impl_->fps = std::max(1, fps);
    impl_->minInterval100ns = kHundredNanosecondsPerSecond / impl_->fps;

    const auto fail = [this, error](std::wstring message) {
        if (error != nullptr) {
            *error = message;
        }
        impl_->SetError(message);
        return false;
    };

    if (!Metadata::ApiInformation::IsTypePresent(
            L"Windows.Graphics.Capture.GraphicsCaptureSession")) {
        return fail(L"Windows.Graphics.Capture is not available");
    }
    if (Metadata::ApiInformation::IsMethodPresent(
            L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsSupported") &&
        !Capture::GraphicsCaptureSession::IsSupported()) {
        return fail(L"GraphicsCaptureSession.IsSupported() returned false");
    }

    try {
        const auto displays = EnumerateDisplays();
        const auto selected = std::find_if(displays.begin(), displays.end(),
            [monitor](const MonitorInfo& display) { return display.handle == monitor; });
        if (selected == displays.end()) return fail(L"selected display is unavailable");
        impl_->colorMonitor = *selected;
        impl_->floatCapture = IsAdvancedColorActive(*selected) || !selected->colorInfoAvailable;
        impl_->nextColorCheck100ns = QpcNow100ns() + kHundredNanosecondsPerSecond;
        ComPtr<IDXGIAdapter1> adapter = FindAdapterForMonitor(monitor);
        const D3D_DRIVER_TYPE driverType =
            adapter != nullptr ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE;
        D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
        HRESULT hr = ::D3D11CreateDevice(
            adapter.Get(), driverType, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
            D3D11_SDK_VERSION, impl_->device.GetAddressOf(), &featureLevel,
            impl_->context.GetAddressOf());
        if (FAILED(hr)) {
            return fail(Utf8ToWide(std::format("D3D11CreateDevice failed: {}",
                                               HResultText(hr))));
        }

        if (impl_->floatCapture)
            impl_->toneMapper = std::make_unique<HdrToneMapper>(impl_->device.Get(), impl_->context.Get());

        ComPtr<IDXGIDevice> dxgiDevice;
        CheckHResult(impl_->device.As(&dxgiDevice), "QueryInterface(IDXGIDevice)");
        winrt::com_ptr<IInspectable> inspectable;
        CheckHResult(::CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(), inspectable.put()),
                     "CreateDirect3D11DeviceFromDXGIDevice");
        Direct3D11::IDirect3DDevice winrtDevice{nullptr};
        CheckHResult(inspectable->QueryInterface(winrt::guid_of<Direct3D11::IDirect3DDevice>(),
                                                 winrt::put_abi(winrtDevice)),
                     "QueryInterface(IDirect3DDevice)");

        const auto interop = winrt::get_activation_factory<
            Capture::GraphicsCaptureItem, ::IGraphicsCaptureItemInterop>();
        CheckHResult(interop->CreateForMonitor(monitor,
                                               winrt::guid_of<Capture::GraphicsCaptureItem>(),
                                               winrt::put_abi(impl_->item)),
                     "CreateForMonitor");

        const auto itemSize = impl_->item.Size();
        if (itemSize.Width <= 0 || itemSize.Height <= 0) {
            return fail(L"capture item reported an empty size");
        }
        impl_->frameSize = Size{itemSize.Width, itemSize.Height};

        impl_->pool = Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
            winrtDevice, impl_->floatCapture ? DirectX::DirectXPixelFormat::R16G16B16A16Float
                                            : DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
            2, itemSize);
        impl_->session = impl_->pool.CreateCaptureSession(impl_->item);
        try {
            impl_->session.IsCursorCaptureEnabled(true);
        } catch (const winrt::hresult_error&) {
            // Older surfaces may not support cursor control; recording can
            // continue without it.
        }

        std::weak_ptr<Impl> weak = impl_;
        impl_->frameToken = impl_->pool.FrameArrived(
            [weak](const Capture::Direct3D11CaptureFramePool& sender,
                   const Foundation::IInspectable&) {
                if (auto state = weak.lock()) {
                    state->HandleFrame(sender);
                }
            });
        impl_->hasFrameToken = true;
        try {
            impl_->itemClosedToken = impl_->item.Closed(
                [weak](const Capture::GraphicsCaptureItem&,
                       const Foundation::IInspectable&) {
                    if (auto state = weak.lock()) {
                        state->SetError(L"capture item was closed by the system");
                    }
                });
            impl_->hasItemClosedToken = true;
        } catch (const winrt::hresult_error&) {
            impl_->hasItemClosedToken = false;
        }

        impl_->session.StartCapture();
        return true;
    } catch (const winrt::hresult_error& captureError) {
        return fail(Utf8ToWide(std::format("WGC initialization failed: {}",
                                           HResultText(captureError.code()))));
    } catch (const std::exception& captureError) {
        return fail(Utf8ToWide(captureError.what()));
    }
}

bool ScreenCapture::WaitForFirstFrame(int timeoutMs, std::wstring* error) {
    if (impl_ == nullptr) {
        return false;
    }
    std::unique_lock lock(impl_->mutex);
    const bool ready = impl_->condition.wait_for(
        lock, std::chrono::milliseconds(timeoutMs), [this] {
            return impl_->firstAvailable || impl_->stopped || !impl_->error.empty();
        });
    if (!ready) {
        if (error != nullptr) {
            *error = L"the first captured frame did not arrive within the timeout";
        }
        return false;
    }
    if (!impl_->error.empty()) {
        if (error != nullptr) {
            *error = impl_->error;
        }
        return false;
    }
    if (!impl_->firstAvailable) {
        if (error != nullptr) {
            *error = L"capture stopped before the first frame";
        }
        return false;
    }
    return true;
}

bool ScreenCapture::CopyLatestFrame(std::vector<std::uint8_t>& bgra, int& width, int& height,
                                    std::int64_t& qpc100ns) {
    if (impl_ == nullptr) {
        return false;
    }
    std::lock_guard lock(impl_->mutex);
    if (impl_->generation <= impl_->provided || impl_->latest.empty()) {
        return false;
    }
    bgra = impl_->latest;
    width = impl_->width;
    height = impl_->height;
    qpc100ns = impl_->latestQpc100ns;
    impl_->provided = impl_->generation;
    return true;
}

bool ScreenCapture::CopySnapshot(const RECT& region, ScreenshotFrame& frame,
                                 std::wstring* error) const {
    frame = {};
    if (impl_ == nullptr) {
        if (error != nullptr) {
            *error = L"屏幕捕获尚未启动";
        }
        return false;
    }
    std::lock_guard lock(impl_->mutex);
    if (impl_->stopped || !impl_->error.empty() || !impl_->firstAvailable) {
        if (error != nullptr) {
            *error = impl_->error.empty() ? L"当前没有有效的截图画面" : impl_->error;
        }
        return false;
    }
    return detail::CopyScreenshotRegion(impl_->latest, {impl_->width, impl_->height},
                                         region, frame, error);
}

Size ScreenCapture::frameSize() const {
    if (impl_ == nullptr) {
        return {};
    }
    std::lock_guard lock(impl_->mutex);
    return impl_->frameSize;
}

std::int64_t ScreenCapture::firstFrameQpc100ns() const {
    if (impl_ == nullptr) {
        return 0;
    }
    std::lock_guard lock(impl_->mutex);
    return impl_->firstQpc100ns;
}

bool ScreenCapture::HasError() const {
    return impl_ != nullptr && impl_->HasError();
}

std::wstring ScreenCapture::Error() const {
    return impl_ != nullptr ? impl_->ErrorText() : std::wstring();
}

void ScreenCapture::Stop() {
    if (impl_ == nullptr) {
        return;
    }
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->stopped) {
            return;
        }
        impl_->stopped = true;
    }
    impl_->condition.notify_all();

    try {
        if (impl_->pool != nullptr && impl_->hasFrameToken) {
            impl_->pool.FrameArrived(impl_->frameToken);
            impl_->hasFrameToken = false;
        }
        if (impl_->item != nullptr && impl_->hasItemClosedToken) {
            impl_->item.Closed(impl_->itemClosedToken);
            impl_->hasItemClosedToken = false;
        }
        if (impl_->session != nullptr) {
            impl_->session.Close();
        }
        if (impl_->pool != nullptr) {
            impl_->pool.Close();
        }
    } catch (...) {
    }

    {
        std::lock_guard lock(impl_->copyMutex);
        impl_->staging.Reset();
        impl_->context.Reset();
        impl_->device.Reset();
    }
    impl_->session = nullptr;
    impl_->item = nullptr;
    impl_->pool = nullptr;
}

}  // namespace srl
