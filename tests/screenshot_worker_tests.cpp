#include "app/screenshot.h"
#include "common/win32.h"

#include <wincodec.h>
#include <wrl/client.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include <utility>

namespace {

int failures = 0;
int checks = 0;

void Check(bool condition, const char* description) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", description);
    }
}

srl::ScreenshotFrame RedFrame() {
    srl::ScreenshotFrame frame;
    frame.width = 2;
    frame.height = 2;
    frame.bgra = {0, 0, 255, 255, 0, 0, 255, 255,
                  0, 0, 255, 255, 0, 0, 255, 255};
    return frame;
}

bool WaitForEvent(srl::ScreenshotWorker& worker, srl::ScreenshotEvent& event) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        if (worker.TryPop(event)) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

bool IsRedPng(const std::wstring& path) {
    using Microsoft::WRL::ComPtr;
    ComPtr<IWICImagingFactory> factory;
    HRESULT hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.GetAddressOf()));
    ComPtr<IWICBitmapDecoder> decoder;
    if (SUCCEEDED(hr)) {
        hr = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
            WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf());
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (SUCCEEDED(hr)) {
        hr = decoder->GetFrame(0, frame.GetAddressOf());
    }
    UINT width = 0;
    UINT height = 0;
    if (SUCCEEDED(hr)) {
        hr = frame->GetSize(&width, &height);
    }
    if (FAILED(hr) || width != 2 || height != 2) {
        return false;
    }
    ComPtr<IWICFormatConverter> converter;
    hr = factory->CreateFormatConverter(converter.GetAddressOf());
    if (SUCCEEDED(hr)) {
        hr = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    }
    std::array<std::uint8_t, 16> pixels{};
    if (SUCCEEDED(hr)) {
        hr = converter->CopyPixels(nullptr, 8, static_cast<UINT>(pixels.size()),
                                   pixels.data());
    }
    return SUCCEEDED(hr) &&
        pixels == std::array<std::uint8_t, 16>{0, 0, 255, 255, 0, 0, 255, 255,
                                             0, 0, 255, 255, 0, 0, 255, 255};
}

}  // namespace

int main() {
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) {
        return 1;
    }
    const auto directory = std::filesystem::path(srl::GetTempDirectory()) /
        (L"SRL_screenshot_worker_" + std::to_wstring(::GetCurrentProcessId()) + L"_" +
         srl::CurrentTimestampForFileName());
    std::filesystem::create_directories(directory);
    {
        srl::ScreenshotWorker worker;
        srl::ScreenshotFrame source = RedFrame();
        std::wstring selectedDirectory = directory.wstring();
        std::wstring error;
        Check(worker.SubmitSnapshot(source, selectedDirectory, &error),
              "first snapshot is accepted");
        source.bgra.assign(source.bgra.size(), 0);
        selectedDirectory = (directory / L"changed_after_submit").wstring();
        Check(worker.SubmitSnapshot(RedFrame(), directory.wstring(), &error),
              "second snapshot is accepted");
        Check(worker.pending() == 2, "two accepted results occupy the queue budget");
        Check(!worker.SubmitSnapshot(RedFrame(), directory.wstring(), &error) &&
              !error.empty(), "third unacknowledged snapshot is rejected with feedback");
        srl::ScreenshotEvent event;
        const bool first = WaitForEvent(worker, event);
        Check(first && event.type == srl::ScreenshotEvent::Type::Completed &&
              !event.idleCapture && event.result.ok, "snapshot completes without a capture event");
        const std::wstring firstPath = event.result.path;
        Check(first && std::filesystem::path(firstPath).parent_path() == directory / L"Screenshots",
              "snapshot retains the submitted output directory");
        Check(first && IsRedPng(firstPath), "snapshot owns its original pixels and dimensions");
        Check(worker.pending() == 1, "acknowledging a completed result releases one slot");
        const bool second = WaitForEvent(worker, event);
        Check(second && event.result.ok && event.result.path != firstPath,
              "second snapshot receives an independent final filename");
        Check(worker.pending() == 0, "all acknowledged snapshots release the queue budget");

        source = RedFrame();
        source.bgra.resize(3);
        Check(!worker.SubmitSnapshot(std::move(source), directory.wstring(), &error),
              "incomplete pixel data is rejected before queuing");
        Check(!worker.SubmitSnapshot(RedFrame(), L"", &error),
              "empty output directory is rejected before queuing");

        const auto blocked = directory / L"not_a_directory";
        std::ofstream(blocked) << "preserve";
        Check(worker.SubmitSnapshot(RedFrame(), blocked.wstring(), &error),
              "filesystem failure is handled by the background worker");
        Check(WaitForEvent(worker, event) && !event.result.ok &&
              !event.result.error.empty(), "failed save publishes a completed error result");
        Check(worker.pending() == 0, "failed save also releases its slot after acknowledgement");

        Check(worker.SubmitCapture(reinterpret_cast<HMONITOR>(1), RECT{0, 0, 2, 2},
                                   directory.wstring(), &error),
              "idle capture request owns its validation on the worker");
        Check(!worker.SubmitCapture(reinterpret_cast<HMONITOR>(1), RECT{0, 0, 2, 2},
                                    directory.wstring(), &error),
              "a second idle capture cannot race restoration of the first");
        Check(WaitForEvent(worker, event) &&
              event.type == srl::ScreenshotEvent::Type::Completed &&
              event.idleCapture && !event.result.ok,
              "invalid monitor publishes completion without screen capture");
        Check(worker.pending() == 0, "failed capture releases its slot after acknowledgement");
    }
    const auto drainDirectory = directory / L"drain";
    {
        srl::ScreenshotWorker worker;
        std::wstring error;
        Check(worker.SubmitSnapshot(RedFrame(), drainDirectory.wstring(), &error),
              "snapshot is accepted immediately before destruction");
    }
    std::size_t saved = 0;
    if (std::filesystem::is_directory(drainDirectory / L"Screenshots")) {
        for (const auto& entry : std::filesystem::directory_iterator(drainDirectory / L"Screenshots")) {
            if (entry.path().extension() == L".png") {
                ++saved;
            }
        }
    }
    Check(saved == 1, "destruction drains an accepted PNG save");
    std::filesystem::remove_all(directory);
    ::CoUninitialize();
    std::printf("%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
