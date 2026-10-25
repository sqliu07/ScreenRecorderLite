#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

namespace srl {

struct PngProbeResult {
    bool ok = false;
    std::wstring path;
    std::wstring error;
    int width = 0;
    int height = 0;
    std::uint64_t fileSize = 0;
    bool decoded = false;
    std::wstring decodedPixelFormat;
    bool orientationAndColorsOk = false;
};

// Writes a synthetic BGRA pattern through WIC to prove the PNG encode/decode
// path without touching the desktop.
[[nodiscard]] PngProbeResult RunPngProbe(const std::wstring& path, int width = 320,
                                         int height = 180);
void PrintPngProbe(const PngProbeResult& result);

}  // namespace srl
