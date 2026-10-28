#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace srl {

struct WgcProbeOptions {
    int monitorIndex = 0;  // 0 = primary, otherwise 1-based index from EnumerateDisplays()
    int timeoutMs = 3000;
    bool savePng = false;
    std::wstring pngPath;
    // Monitor-relative physical pixel positions. Empty means the frame center.
    std::vector<POINT> samplePoints;
};

struct WgcProbeResult {
    bool ok = false;
    std::wstring error;
    std::wstring monitorName;
    int width = 0;
    int height = 0;
    std::wstring pixelFormat;
    bool sessionStarted = false;
    bool advancedColorActive = false;
    int sampleX = 0;
    int sampleY = 0;
    std::uint32_t sampleBgra = 0;
    std::vector<std::uint32_t> samples;
    bool pngSaved = false;
    std::wstring pngPath;
};

// Captures a single frame from the selected monitor through Windows Graphics
// Capture. This command reads the desktop; it is never run implicitly by the
// application. Frames stay in memory unless savePng is requested.
[[nodiscard]] WgcProbeResult RunWgcProbe(const WgcProbeOptions& options);
void PrintWgcProbe(const WgcProbeResult& result);

}  // namespace srl
