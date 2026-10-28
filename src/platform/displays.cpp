#include "platform/displays.h"

#include <shellscalingapi.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <winrt/base.h>

#include <algorithm>
#include <cwctype>
#include <format>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/win32.h"

namespace srl {
namespace {

std::wstring ToUpper(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towupper(ch)); });
    return value;
}

std::string ColorEncodingName(DISPLAYCONFIG_COLOR_ENCODING encoding) {
    switch (encoding) {
        case DISPLAYCONFIG_COLOR_ENCODING_RGB:
            return "RGB";
        case DISPLAYCONFIG_COLOR_ENCODING_YCBCR444:
            return "YCbCr444";
        case DISPLAYCONFIG_COLOR_ENCODING_YCBCR422:
            return "YCbCr422";
        case DISPLAYCONFIG_COLOR_ENCODING_YCBCR420:
            return "YCbCr420";
        case DISPLAYCONFIG_COLOR_ENCODING_INTENSITY:
            return "Intensity";
        default:
            return std::format("unknown({})", static_cast<int>(encoding));
    }
}

std::string OutputTechnologyName(DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY technology) {
    switch (technology) {
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HD15:
            return "VGA";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_SVIDEO:
            return "S-Video";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_COMPOSITE_VIDEO:
            return "Composite";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_COMPONENT_VIDEO:
            return "Component";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DVI:
            return "DVI";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_HDMI:
            return "HDMI";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_LVDS:
            return "LVDS";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_D_JPN:
            return "D-JPN";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_SDI:
            return "SDI";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EXTERNAL:
            return "DisplayPort (external)";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED:
            return "DisplayPort (embedded)";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EXTERNAL:
            return "UDI (external)";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED:
            return "UDI (embedded)";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_SDTVDONGLE:
            return "SDTV dongle";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_MIRACAST:
            return "Miracast";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_WIRED:
            return "Indirect wired";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INDIRECT_VIRTUAL:
            return "Indirect virtual";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL:
            return "Internal";
        case DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_USB_TUNNEL:
            return "DisplayPort over USB";
        default:
            return std::format("other({})", static_cast<int>(technology));
    }
}

struct TargetDisplayInfo {
    std::wstring friendlyName;
    std::wstring devicePath;
    LUID adapterId{};
    UINT32 targetId = 0;
    bool advancedColorSupported = false;
    bool advancedColorEnabled = false;
    bool wideColorEnforced = false;
    std::uint32_t bitsPerColorChannel = 0;
    DISPLAYCONFIG_COLOR_ENCODING colorEncoding = DISPLAYCONFIG_COLOR_ENCODING_RGB;
    bool colorInfoAvailable = false;
    bool hdrEnabled = false;
    bool hdrStateKnown = false;
    float sdrWhiteScale = 1.0f;
    std::string outputTechnology;
};

// INFO_2 is a runtime-optional Win11 query. Keep its SDK wire layout local so
// builds with the supported Windows 10 SDK can still run on newer systems.
struct AdvancedColorInfo2 {
    DISPLAYCONFIG_DEVICE_INFO_HEADER header;
    UINT32 flags;
    DISPLAYCONFIG_COLOR_ENCODING colorEncoding;
    UINT32 bitsPerColorChannel;
    UINT32 activeColorMode;
};
constexpr auto kGetAdvancedColorInfo2 = static_cast<DISPLAYCONFIG_DEVICE_INFO_TYPE>(15);
constexpr UINT32 kAdvancedColorSupported = 1U << 0;
constexpr UINT32 kAdvancedColorActive = 1U << 1;
constexpr UINT32 kColorModeWideGamut = 1;
constexpr UINT32 kColorModeHdr = 2;
constexpr float kSdrWhiteFixedPointDivisor = 1'000.0f;
static_assert(sizeof(AdvancedColorInfo2) == 36);

bool QueryTargetColor(TargetDisplayInfo& target) {
    DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO legacy{};
    legacy.header = {DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO,
        sizeof(legacy), target.adapterId, target.targetId};
    if (::DisplayConfigGetDeviceInfo(&legacy.header) == ERROR_SUCCESS) {
        target.colorInfoAvailable = true;
        target.advancedColorSupported = legacy.advancedColorSupported != 0;
        target.advancedColorEnabled = legacy.advancedColorEnabled != 0;
        target.wideColorEnforced = legacy.wideColorEnforced != 0;
        target.bitsPerColorChannel = legacy.bitsPerColorChannel;
        target.colorEncoding = legacy.colorEncoding;
    }
    AdvancedColorInfo2 current{};
    current.header = {kGetAdvancedColorInfo2, sizeof(current), target.adapterId, target.targetId};
    if (::DisplayConfigGetDeviceInfo(&current.header) == ERROR_SUCCESS) {
        target.colorInfoAvailable = true;
        target.hdrStateKnown = true;
        target.advancedColorSupported = (current.flags & kAdvancedColorSupported) != 0;
        target.advancedColorEnabled = (current.flags & kAdvancedColorActive) != 0;
        target.wideColorEnforced = current.activeColorMode == kColorModeWideGamut;
        target.hdrEnabled = current.activeColorMode == kColorModeHdr;
        target.bitsPerColorChannel = current.bitsPerColorChannel;
        target.colorEncoding = current.colorEncoding;
    }
    DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
    white.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL,
        sizeof(white), target.adapterId, target.targetId};
    if (::DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS && white.SDRWhiteLevel > 0)
        target.sdrWhiteScale = static_cast<float>(white.SDRWhiteLevel) / kSdrWhiteFixedPointDivisor;
    return target.colorInfoAvailable;
}

std::optional<bool> QueryDxgiHdr(HMONITOR monitor) {
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(factory.GetAddressOf())))) return std::nullopt;
    for (UINT adapterIndex = 0;; ++adapterIndex) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(adapterIndex, adapter.GetAddressOf()) == DXGI_ERROR_NOT_FOUND) break;
        if (!adapter) continue;
        for (UINT outputIndex = 0;; ++outputIndex) {
            Microsoft::WRL::ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(outputIndex, output.GetAddressOf()) == DXGI_ERROR_NOT_FOUND) break;
            if (!output) continue;
            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc)) || desc.Monitor != monitor) continue;
            Microsoft::WRL::ComPtr<IDXGIOutput6> output6;
            DXGI_OUTPUT_DESC1 color{};
            if (SUCCEEDED(output.As(&output6)) && SUCCEEDED(output6->GetDesc1(&color)))
                return color.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
            return std::nullopt;
        }
    }
    return std::nullopt;
}

void CopyTargetColor(const TargetDisplayInfo& target, MonitorInfo& monitor) {
    monitor.advancedColorSupported = target.advancedColorSupported;
    monitor.advancedColorEnabled = target.advancedColorEnabled;
    monitor.wideColorEnforced = target.wideColorEnforced;
    monitor.hdrEnabled = target.hdrEnabled;
    monitor.hdrStateKnown = target.hdrStateKnown;
    monitor.colorInfoAvailable = target.colorInfoAvailable;
    monitor.sdrWhiteScale = target.sdrWhiteScale;
    monitor.bitsPerColorChannel = target.bitsPerColorChannel;
    monitor.colorEncoding = Utf8ToWide(ColorEncodingName(target.colorEncoding));
}

std::unordered_multimap<std::wstring, TargetDisplayInfo> QueryTargetDisplays() {
    std::unordered_multimap<std::wstring, TargetDisplayInfo> result;

    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (::GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) !=
        ERROR_SUCCESS) {
        return result;
    }

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);

    LONG status = ERROR_INSUFFICIENT_BUFFER;
    for (int attempt = 0; attempt < 3 && status == ERROR_INSUFFICIENT_BUFFER; ++attempt) {
        status = ::QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount,
                                      modes.data(), nullptr);
        if (status == ERROR_INSUFFICIENT_BUFFER) {
            if (::GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) !=
                ERROR_SUCCESS) {
                return result;
            }
            paths.resize(pathCount);
            modes.resize(modeCount);
        }
    }
    if (status != ERROR_SUCCESS) {
        return result;
    }
    paths.resize(pathCount);

    for (const DISPLAYCONFIG_PATH_INFO& path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName{};
        sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        sourceName.header.size = sizeof(sourceName);
        sourceName.header.adapterId = path.sourceInfo.adapterId;
        sourceName.header.id = path.sourceInfo.id;
        if (::DisplayConfigGetDeviceInfo(&sourceName.header) != ERROR_SUCCESS) {
            continue;
        }

        DISPLAYCONFIG_TARGET_DEVICE_NAME targetName{};
        targetName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        targetName.header.size = sizeof(targetName);
        targetName.header.adapterId = path.targetInfo.adapterId;
        targetName.header.id = path.targetInfo.id;
        const bool hasTargetName =
            ::DisplayConfigGetDeviceInfo(&targetName.header) == ERROR_SUCCESS;

        TargetDisplayInfo target;
        target.adapterId = path.targetInfo.adapterId;
        target.targetId = path.targetInfo.id;
        target.outputTechnology =
            OutputTechnologyName(static_cast<DISPLAYCONFIG_VIDEO_OUTPUT_TECHNOLOGY>(
                path.targetInfo.outputTechnology));
        if (hasTargetName) {
            target.friendlyName = targetName.monitorFriendlyDeviceName;
            target.devicePath = targetName.monitorDevicePath;
            if (target.friendlyName.empty()) {
                target.friendlyName = targetName.monitorDevicePath;
            }
        }

        QueryTargetColor(target);

        result.emplace(ToUpper(sourceName.viewGdiDeviceName), std::move(target));
    }
    return result;
}

struct MonitorEnumerationContext {
    std::vector<MonitorInfo> monitors;
};

BOOL CALLBACK MonitorEnumProc(HMONITOR monitor, HDC, LPRECT, LPARAM context) {
    auto* collected = reinterpret_cast<MonitorEnumerationContext*>(context);

    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!::GetMonitorInfoW(monitor, &info)) {
        return TRUE;
    }

    MonitorInfo entry;
    entry.handle = monitor;
    entry.deviceName = info.szDevice;
    entry.virtualRect = info.rcMonitor;
    entry.workRect = info.rcWork;
    entry.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;

    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (::GetMonitorInfoW(monitor, &mi)) {
        entry.workRect = mi.rcWork;
    }

    if (SUCCEEDED(::GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &entry.dpiX, &entry.dpiY))) {
        entry.scale = static_cast<double>(entry.dpiX) / 96.0;
    }

    DISPLAY_DEVICEW displayDevice{};
    displayDevice.cb = sizeof(displayDevice);
    if (::EnumDisplayDevicesW(info.szDevice, 0, &displayDevice, EDD_GET_DEVICE_INTERFACE_NAME)) {
        entry.friendlyName = displayDevice.DeviceString;
        entry.devicePath = displayDevice.DeviceID;
    }
    if (entry.friendlyName.empty()) {
        entry.friendlyName = info.szDevice;
    }

    collected->monitors.push_back(std::move(entry));
    return TRUE;
}

}  // namespace

std::vector<MonitorInfo> EnumerateDisplays() {
    MonitorEnumerationContext context;
    ::EnumDisplayMonitors(nullptr, nullptr, MonitorEnumProc,
                          reinterpret_cast<LPARAM>(&context));

    auto& monitors = context.monitors;
    std::sort(monitors.begin(), monitors.end(), [](const MonitorInfo& a, const MonitorInfo& b) {
        if (a.virtualRect.left != b.virtualRect.left) {
            return a.virtualRect.left < b.virtualRect.left;
        }
        if (a.virtualRect.top != b.virtualRect.top) {
            return a.virtualRect.top < b.virtualRect.top;
        }
        return a.deviceName < b.deviceName;
    });

    const auto targets = QueryTargetDisplays();
    for (std::size_t i = 0; i < monitors.size(); ++i) {
        MonitorInfo& monitor = monitors[i];
        monitor.index = static_cast<int>(i) + 1;

        const auto [first, last] = targets.equal_range(ToUpper(monitor.deviceName));
        auto found = first;
        // Cloned targets can share one source name. Prefer the target interface
        // reported for this HMONITOR instead of silently taking a different one.
        for (auto candidate = first; candidate != last; ++candidate) {
            if (!monitor.devicePath.empty() &&
                ToUpper(candidate->second.devicePath) == ToUpper(monitor.devicePath)) {
                found = candidate;
                break;
            }
        }
        if (found != last) {
            const TargetDisplayInfo& target = found->second;
            if (!target.friendlyName.empty()) monitor.friendlyName = target.friendlyName;
            monitor.devicePath = target.devicePath;
            monitor.outputTechnology = Utf8ToWide(target.outputTechnology);
            monitor.targetAdapterId = target.adapterId;
            monitor.targetId = target.targetId;
            monitor.hasDisplayConfigTarget = true;
            CopyTargetColor(target, monitor);
        }
        if (!monitor.hdrStateKnown) {
            if (const auto hdr = QueryDxgiHdr(monitor.handle)) {
                monitor.hdrStateKnown = true;
                monitor.hdrEnabled = *hdr;
            }
        }

    }
    return monitors;
}

const MonitorInfo* PrimaryMonitor(const std::vector<MonitorInfo>& displays) {
    for (const MonitorInfo& monitor : displays) {
        if (monitor.primary) {
            return &monitor;
        }
    }
    return displays.empty() ? nullptr : &displays.front();
}

bool IsAdvancedColorActive(const MonitorInfo& monitor) {
    return monitor.advancedColorEnabled || monitor.wideColorEnforced || monitor.hdrEnabled;
}

bool IsHdrActive(const MonitorInfo& monitor) { return monitor.hdrEnabled; }

bool RefreshDisplayColor(MonitorInfo& monitor) {
    if (!monitor.hasDisplayConfigTarget) return false;
    TargetDisplayInfo target;
    target.adapterId = monitor.targetAdapterId;
    target.targetId = monitor.targetId;
    if (!QueryTargetColor(target)) return false;
    CopyTargetColor(target, monitor);
    if (!monitor.hdrStateKnown) {
        if (const auto hdr = QueryDxgiHdr(monitor.handle)) {
            monitor.hdrStateKnown = true;
            monitor.hdrEnabled = *hdr;
        }
    }
    return true;
}

void PrintDisplays(const std::vector<MonitorInfo>& displays) {
    ConsoleLine("Displays");
    if (displays.empty()) {
        ConsoleLine("  (no active displays reported)");
        return;
    }

    for (const MonitorInfo& monitor : displays) {
        ConsoleLine(std::format("  #{} {}{}  {}", monitor.index,
                                WideToUtf8(monitor.deviceName),
                                monitor.primary ? " [primary]" : "",
                                WideToUtf8(monitor.friendlyName)));
        ConsoleLine(std::format(
            "      virtual {}x{} at ({}, {})  work {}x{} at ({}, {})",
            monitor.virtualRect.right - monitor.virtualRect.left,
            monitor.virtualRect.bottom - monitor.virtualRect.top, monitor.virtualRect.left,
            monitor.virtualRect.top, monitor.workRect.right - monitor.workRect.left,
            monitor.workRect.bottom - monitor.workRect.top, monitor.workRect.left,
            monitor.workRect.top));
        const std::string outputTechnology =
            monitor.outputTechnology.empty() ? "unknown"
                                             : WideToUtf8(monitor.outputTechnology);
        ConsoleLine(std::format("      dpi {}x{} ({:.0f}%)  output {}", monitor.dpiX,
                                monitor.dpiY, monitor.scale * 100.0, outputTechnology));
        const bool fractionalBits = monitor.bitsPerColorChannel != 0;
        const std::string colorEncoding =
            monitor.colorEncoding.empty() ? "unknown" : WideToUtf8(monitor.colorEncoding);
        const std::string bitDepth =
            fractionalBits ? std::format(" bits={}", monitor.bitsPerColorChannel)
                           : std::string();
        ConsoleLine(std::format(
            "      color {}  advancedColorSupported={} enabled={} wideColorEnforced={}{}{}",
            colorEncoding, monitor.advancedColorSupported ? 1 : 0,
            monitor.advancedColorEnabled ? 1 : 0, monitor.wideColorEnforced ? 1 : 0,
            bitDepth,
            monitor.hdrEnabled ? "  [HDR, tone-mapped to SDR]"
                : IsAdvancedColorActive(monitor) ? "  [WCG/advanced color]" : "  [SDR]"));
        if (!monitor.devicePath.empty()) {
            ConsoleLine(std::format("      path {}", WideToUtf8(monitor.devicePath)));
        }
    }
}

}  // namespace srl
