#include "platform/displays.h"

#include <shellscalingapi.h>
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
    bool advancedColorSupported = false;
    bool advancedColorEnabled = false;
    bool wideColorEnforced = false;
    std::uint32_t bitsPerColorChannel = 0;
    DISPLAYCONFIG_COLOR_ENCODING colorEncoding = DISPLAYCONFIG_COLOR_ENCODING_RGB;
    bool colorInfoAvailable = false;
    std::string outputTechnology;
};

std::unordered_map<std::wstring, TargetDisplayInfo> QueryTargetDisplays() {
    std::unordered_map<std::wstring, TargetDisplayInfo> result;

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

        DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO colorInfo{};
        colorInfo.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
        colorInfo.header.size = sizeof(colorInfo);
        colorInfo.header.adapterId = path.targetInfo.adapterId;
        colorInfo.header.id = path.targetInfo.id;
        if (::DisplayConfigGetDeviceInfo(&colorInfo.header) == ERROR_SUCCESS) {
            target.colorInfoAvailable = true;
            target.advancedColorSupported = colorInfo.advancedColorSupported != 0;
            target.advancedColorEnabled = colorInfo.advancedColorEnabled != 0;
            target.wideColorEnforced = colorInfo.wideColorEnforced != 0;
            target.bitsPerColorChannel = colorInfo.bitsPerColorChannel;
            target.colorEncoding = colorInfo.colorEncoding;
        }

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
    if (::EnumDisplayDevicesW(info.szDevice, 0, &displayDevice, 0)) {
        entry.friendlyName = displayDevice.DeviceString;
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

        const auto found = targets.find(ToUpper(monitor.deviceName));
        if (found == targets.end()) {
            continue;
        }
        const TargetDisplayInfo& target = found->second;
        if (!target.friendlyName.empty()) {
            monitor.friendlyName = target.friendlyName;
        }
        monitor.devicePath = target.devicePath;
        monitor.outputTechnology = Utf8ToWide(target.outputTechnology);
        monitor.advancedColorSupported = target.advancedColorSupported;
        monitor.advancedColorEnabled = target.advancedColorEnabled;
        monitor.wideColorEnforced = target.wideColorEnforced;
        monitor.bitsPerColorChannel = target.bitsPerColorChannel;
        monitor.colorEncoding = Utf8ToWide(ColorEncodingName(target.colorEncoding));
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
    return (monitor.advancedColorSupported && monitor.advancedColorEnabled) ||
           monitor.wideColorEnforced;
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
            IsAdvancedColorActive(monitor) ? "  [SDR capture blocked]" : "  [SDR]"));
        if (!monitor.devicePath.empty()) {
            ConsoleLine(std::format("      path {}", WideToUtf8(monitor.devicePath)));
        }
    }
}

}  // namespace srl
