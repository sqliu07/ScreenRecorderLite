#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace srl {

struct MonitorInfo {
    int index = 0;  // 1-based, ordered by virtual desktop position
    HMONITOR handle = nullptr;
    std::wstring deviceName;    // \\.\DISPLAY1
    std::wstring friendlyName;  // monitor friendly name when available
    std::wstring devicePath;    // target monitor device path
    RECT virtualRect{};
    RECT workRect{};
    bool primary = false;

    UINT dpiX = 96;
    UINT dpiY = 96;
    double scale = 1.0;

    bool advancedColorSupported = false;
    bool advancedColorEnabled = false;
    bool wideColorEnforced = false;
    bool hdrEnabled = false;
    bool hdrStateKnown = false;
    bool colorInfoAvailable = false;
    float sdrWhiteScale = 1.0f;  // scRGB linear SDR white / 80 nits
    LUID targetAdapterId{};
    UINT32 targetId = 0;
    bool hasDisplayConfigTarget = false;
    std::uint32_t bitsPerColorChannel = 0;
    std::wstring colorEncoding;
    std::wstring outputTechnology;
};

[[nodiscard]] std::vector<MonitorInfo> EnumerateDisplays();
[[nodiscard]] const MonitorInfo* PrimaryMonitor(const std::vector<MonitorInfo>& displays);
[[nodiscard]] bool IsAdvancedColorActive(const MonitorInfo& monitor);
[[nodiscard]] bool IsHdrActive(const MonitorInfo& monitor);
// Updates this target only; other monitors' HDR state never participates.
[[nodiscard]] bool RefreshDisplayColor(MonitorInfo& monitor);
void PrintDisplays(const std::vector<MonitorInfo>& displays);

}  // namespace srl
