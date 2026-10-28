#include "capture/wgc_probe.h"

#include <algorithm>
#include <filesystem>
#include <format>

#include "common/win32.h"
#include "image/png_writer.h"
#include "platform/displays.h"
#include "recording/screen_capture.h"

namespace srl {

WgcProbeResult RunWgcProbe(const WgcProbeOptions& options) {
    WgcProbeResult result;
    try {
        const auto displays = EnumerateDisplays();
        const MonitorInfo* monitor = options.monitorIndex == 0 ? PrimaryMonitor(displays) : nullptr;
        for (const auto& candidate : displays) {
            if (candidate.index == options.monitorIndex) monitor = &candidate;
        }
        if (!monitor) { result.error = L"selected display was not found"; return result; }
        result.monitorName = monitor->deviceName + L" (" + monitor->friendlyName + L")";
        result.advancedColorActive = IsAdvancedColorActive(*monitor);
        ScreenCapture capture;
        if (!capture.Start(monitor->handle, 30, &result.error)) return result;
        result.sessionStarted = true;
        if (!capture.WaitForFirstFrame(options.timeoutMs, &result.error)) return result;
        const Size size = capture.frameSize();
        ScreenshotFrame frame;
        if (!capture.CopySnapshot(RECT{0, 0, size.width, size.height}, frame, &result.error))
            return result;
        capture.Stop();
        result.width = frame.width;
        result.height = frame.height;
        result.pixelFormat = L"B8G8R8A8_UNORM (SDR)";
        const auto sampleAt = [&](int x, int y) {
            x = std::clamp(x, 0, frame.width - 1);
            y = std::clamp(y, 0, frame.height - 1);
            const auto offset = (static_cast<std::size_t>(y) * frame.width + x) * 4;
            return (static_cast<std::uint32_t>(frame.bgra[offset + 3]) << 24) |
                   (static_cast<std::uint32_t>(frame.bgra[offset + 2]) << 16) |
                   (static_cast<std::uint32_t>(frame.bgra[offset + 1]) << 8) |
                    static_cast<std::uint32_t>(frame.bgra[offset]);
        };
        if (options.samplePoints.empty()) {
            result.sampleX = frame.width / 2;
            result.sampleY = frame.height / 2;
            result.sampleBgra = sampleAt(result.sampleX, result.sampleY);
            result.samples.push_back(result.sampleBgra);
        } else {
            for (const auto& point : options.samplePoints) {
                const auto value = sampleAt(point.x, point.y);
                if (result.samples.empty()) {
                    result.sampleX = point.x;
                    result.sampleY = point.y;
                    result.sampleBgra = value;
                }
                result.samples.push_back(value);
            }
        }
        if (options.savePng && !options.pngPath.empty()) {
            const std::filesystem::path path(options.pngPath);
            std::string error;
            if (path.has_parent_path() && !EnsureDirectory(path.parent_path(), &error)) {
                result.error = Utf8ToWide(error);
                return result;
            }
            result.pngPath = options.pngPath;
            result.pngSaved = SaveBgraPng(options.pngPath, frame.bgra.data(), frame.width,
                                          frame.height, frame.width * 4, &result.error);
        }
        result.ok = result.error.empty();
    } catch (const std::exception& error) {
        result.error = Utf8ToWide(error.what());
    }
    return result;
}

void PrintWgcProbe(const WgcProbeResult& result) {
    ConsoleLine("Windows Graphics Capture");
    ConsoleLine(std::format("  monitor            {}", WideToUtf8(result.monitorName)));
    ConsoleLine(std::format("  advanced color     {}", result.advancedColorActive ? "active" : "no"));
    ConsoleLine(std::format("  session started    {}", result.sessionStarted ? "yes" : "no"));
    ConsoleLine(std::format("  frame              {}x{} {}", result.width, result.height,
                            WideToUtf8(result.pixelFormat)));
    ConsoleLine(std::format("  sample ({}, {})   0x{:08X}", result.sampleX, result.sampleY,
                            result.sampleBgra));
    if (result.pngSaved) ConsoleLine(std::format("  PNG                {}", WideToUtf8(result.pngPath)));
    if (!result.error.empty()) ConsoleLine(std::format("  error              {}", WideToUtf8(result.error)));
    ConsoleLine(std::format("  result             {}", result.ok ? "OK" : "FAILED"));
}
}  // namespace srl
