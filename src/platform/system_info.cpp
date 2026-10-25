#include "platform/system_info.h"

#include <knownfolders.h>
#include <mfapi.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <dxgi1_2.h>

#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/base.h>

#include <cstddef>
#include <format>
#include <optional>
#include <set>
#include <string_view>
#include <vector>

#include "common/win32.h"

namespace srl {
namespace {

using Microsoft::WRL::ComPtr;

std::wstring ReadRegistryString(HKEY root, const wchar_t* subKey, const wchar_t* valueName) {
    DWORD type = 0;
    DWORD bytes = 0;
    if (::RegGetValueW(root, subKey, valueName, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, &type,
                       nullptr, &bytes) != ERROR_SUCCESS ||
        bytes == 0) {
        return {};
    }
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (::RegGetValueW(root, subKey, valueName, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, &type,
                       value.data(), &bytes) != ERROR_SUCCESS) {
        return {};
    }
    while (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    return value;
}

std::optional<DWORD> ReadRegistryDword(HKEY root, const wchar_t* subKey, const wchar_t* valueName) {
    DWORD type = 0;
    DWORD value = 0;
    DWORD bytes = sizeof(value);
    if (::RegGetValueW(root, subKey, valueName, RRF_RT_REG_DWORD, &type, &value, &bytes) !=
        ERROR_SUCCESS) {
        return std::nullopt;
    }
    return value;
}

OsInfo QueryOsInfo() {
    constexpr const wchar_t* kCurrentVersion =
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";

    OsInfo os;
    os.productName = ReadRegistryString(HKEY_LOCAL_MACHINE, kCurrentVersion, L"ProductName");
    os.displayVersion = ReadRegistryString(HKEY_LOCAL_MACHINE, kCurrentVersion, L"DisplayVersion");

    if (const auto major = ReadRegistryDword(HKEY_LOCAL_MACHINE, kCurrentVersion,
                                              L"CurrentMajorVersionNumber");
        major.has_value()) {
        os.major = *major;
    }
    if (const auto minor = ReadRegistryDword(HKEY_LOCAL_MACHINE, kCurrentVersion,
                                              L"CurrentMinorVersionNumber");
        minor.has_value()) {
        os.minor = *minor;
    }
    const std::wstring buildText =
        ReadRegistryString(HKEY_LOCAL_MACHINE, kCurrentVersion, L"CurrentBuildNumber");
    if (!buildText.empty()) {
        wchar_t* end = nullptr;
        const unsigned long parsed = std::wcstoul(buildText.c_str(), &end, 10);
        if (end != nullptr && *end == L'\0') {
            os.buildNumber = static_cast<std::uint32_t>(parsed);
        }
    }
    if (const auto ubr = ReadRegistryDword(HKEY_LOCAL_MACHINE, kCurrentVersion, L"UBR");
        ubr.has_value()) {
        os.updateBuildRevision = *ubr;
    }

    if (os.major == 0) {
        os.major = 10;
    }
    if (os.buildNumber == 0) {
        os.buildNumber = 19045;
    }
    if (os.buildNumber >= 22000) {
        // ProductName still says "Windows 10" on Windows 11.
        const std::wstring_view name = os.productName;
        if (name.starts_with(L"Windows 10")) {
            os.productName = L"Windows 11" + std::wstring(name.substr(10));
        } else if (os.productName.empty()) {
            os.productName = L"Windows 11";
        }
    } else if (os.productName.empty()) {
        os.productName = L"Windows 10";
    }
    os.build = std::to_wstring(os.buildNumber) + L"." +
               std::to_wstring(os.updateBuildRevision);

    SYSTEM_INFO native{};
    ::GetNativeSystemInfo(&native);
    switch (native.wProcessorArchitecture) {
        case PROCESSOR_ARCHITECTURE_AMD64:
            os.architecture = L"x64";
            break;
        case PROCESSOR_ARCHITECTURE_ARM64:
            os.architecture = L"ARM64";
            break;
        case PROCESSOR_ARCHITECTURE_INTEL:
            os.architecture = L"x86";
            break;
        default:
            os.architecture = L"unknown";
            break;
    }
    return os;
}

CpuInfo QueryCpuInfo() {
    CpuInfo cpu;
    cpu.brand = ReadRegistryString(
        HKEY_LOCAL_MACHINE,
        L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
        L"ProcessorNameString");

    DWORD bufferBytes = 0;
    ::GetLogicalProcessorInformation(nullptr, &bufferBytes);
    if (bufferBytes != 0) {
        std::vector<std::byte> buffer(bufferBytes);
        if (::GetLogicalProcessorInformation(
                reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION>(buffer.data()),
                &bufferBytes)) {
            const auto count =
                bufferBytes / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION);
            const auto* entries = reinterpret_cast<
                const SYSTEM_LOGICAL_PROCESSOR_INFORMATION*>(buffer.data());
            for (DWORD i = 0; i < count; ++i) {
                if (entries[i].Relationship == RelationProcessorCore) {
                    ++cpu.physicalCores;
                }
            }
        }
    }

    cpu.logicalCores = ::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    if (cpu.brand.empty()) {
        cpu.brand = L"unknown CPU";
    }
    return cpu;
}

std::vector<GpuInfo> QueryGpus() {
    std::vector<GpuInfo> gpus;
    std::set<std::int64_t> seenLuids;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return gpus;
    }

    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(index, adapter.GetAddressOf()) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) {
            continue;
        }
        GpuInfo info;
        info.name = desc.Description;
        info.vendorId = desc.VendorId;
        info.deviceId = desc.DeviceId;
        info.dedicatedVideoMemory = desc.DedicatedVideoMemory;
        info.sharedSystemMemory = desc.SharedSystemMemory;
        info.software = (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        LARGE_INTEGER luid{};
        luid.LowPart = desc.AdapterLuid.LowPart;
        luid.HighPart = static_cast<LONG>(desc.AdapterLuid.HighPart);
        info.luid = luid.QuadPart;
        if (!seenLuids.insert(info.luid).second) {
            continue;
        }
        gpus.push_back(std::move(info));
    }
    return gpus;
}

std::wstring QueryDpiAwareness() {
    const DPI_AWARENESS_CONTEXT context = ::GetThreadDpiAwarenessContext();
    const DPI_AWARENESS awareness = ::GetAwarenessFromDpiAwarenessContext(context);
    if (::AreDpiAwarenessContextsEqual(context, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) {
        return L"PerMonitorV2";
    }
    if (::AreDpiAwarenessContextsEqual(context, DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE)) {
        return L"PerMonitor";
    }
    switch (awareness) {
        case DPI_AWARENESS_SYSTEM_AWARE:
            return L"System";
        case DPI_AWARENESS_PER_MONITOR_AWARE:
            return L"PerMonitor";
        case DPI_AWARENESS_UNAWARE:
        default:
            return L"Unaware";
    }
}

bool QueryWgcSupport(const OsInfo& os, std::wstring& detail) {
    using winrt::Windows::Foundation::Metadata::ApiInformation;

    const bool typePresent = ApiInformation::IsTypePresent(
        L"Windows.Graphics.Capture.GraphicsCaptureSession");
    if (!typePresent) {
        detail = L"Windows.Graphics.Capture is not present";
        return false;
    }

    if (os.buildNumber < 18362) {
        detail = Utf8ToWide(std::format(
            "monitor capture requires Windows 10 1903+ (build 18362); this system is build {}",
            os.buildNumber));
        return false;
    }

    const bool isSupportedPresent = ApiInformation::IsMethodPresent(
        L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsSupported");
    if (isSupportedPresent) {
        try {
            if (!winrt::Windows::Graphics::Capture::GraphicsCaptureSession::IsSupported()) {
                detail = L"GraphicsCaptureSession.IsSupported() returned false";
                return false;
            }
        } catch (const winrt::hresult_error& error) {
            detail = Utf8ToWide(std::format(
                "GraphicsCaptureSession.IsSupported() failed: {}", HResultText(error.code())));
            return false;
        }
    }

    detail = isSupportedPresent ? L"WinRT type present; IsSupported()=true"
                                : L"WinRT type present (IsSupported unavailable)";
    return true;
}

bool QueryMediaFoundation() {
    const HRESULT hr = ::MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(hr)) {
        return false;
    }
    ::MFShutdown();
    return true;
}

bool QueryWic() {
    ComPtr<IWICImagingFactory> factory;
    return SUCCEEDED(::CoCreateInstance(CLSID_WICImagingFactory2, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&factory))) ||
           SUCCEEDED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&factory)));
}

}  // namespace

SystemInfo QuerySystemInfo() {
    SystemInfo info;
    info.os = QueryOsInfo();
    info.cpu = QueryCpuInfo();

    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (::GlobalMemoryStatusEx(&memory)) {
        info.memory.totalBytes = memory.ullTotalPhys;
    }

    info.gpus = QueryGpus();
    info.videosFolder = KnownFolderPath(FOLDERID_Videos);
    info.dpiAwareness = QueryDpiAwareness();
    info.wgcSupported = QueryWgcSupport(info.os, info.wgcDetail);
    info.mediaFoundationAvailable = QueryMediaFoundation();
    info.wicAvailable = QueryWic();

    ::ProcessIdToSessionId(::GetCurrentProcessId(), &info.sessionId);
    info.remoteSession = ::GetSystemMetrics(SM_REMOTESESSION) != 0;
    return info;
}

void PrintSystemInfo(const SystemInfo& info) {
    ConsoleLine("System");
    ConsoleLine(std::format("  OS                 {} {} ({}.{}.{}.{}) {}",
                            WideToUtf8(info.os.productName),
                            WideToUtf8(info.os.displayVersion), info.os.major,
                            info.os.minor, info.os.buildNumber,
                            info.os.updateBuildRevision,
                            WideToUtf8(info.os.architecture)));
    ConsoleLine(std::format("  Session            {} ({}, {})", info.sessionId,
                            info.remoteSession ? "remote" : "local",
                            info.remoteSession ? "RDP" : "console"));
    ConsoleLine(std::format("  DPI awareness      {}", WideToUtf8(info.dpiAwareness)));
    ConsoleLine(std::format("  CPU                {} ({} cores / {} threads)",
                            WideToUtf8(info.cpu.brand), info.cpu.physicalCores,
                            info.cpu.logicalCores));
    ConsoleLine(std::format("  Memory             {:.1f} GiB",
                            static_cast<double>(info.memory.totalBytes) / (1024.0 * 1024.0 * 1024.0)));
    if (info.gpus.empty()) {
        ConsoleLine("  GPU                (none reported by DXGI)");
    } else {
        for (std::size_t i = 0; i < info.gpus.size(); ++i) {
            const GpuInfo& gpu = info.gpus[i];
            ConsoleLine(std::format(
                "  GPU #{}              {} [vendor 0x{:04X}, device 0x{:04X}, "
                "dedicated {}]{}{}",
                i + 1, WideToUtf8(gpu.name), gpu.vendorId, gpu.deviceId,
                FormatBytes(gpu.dedicatedVideoMemory), gpu.software ? " (software)" : "",
                i == 0 ? " (default)" : ""));
        }
    }
    ConsoleLine(std::format("  Videos folder      {}", WideToUtf8(info.videosFolder)));
    ConsoleLine(std::format("  WGC                {} - {}", info.wgcSupported ? "supported" : "UNAVAILABLE",
                            WideToUtf8(info.wgcDetail)));
    ConsoleLine(std::format("  Media Foundation   {}",
                            info.mediaFoundationAvailable ? "available" : "UNAVAILABLE"));
    ConsoleLine(std::format("  WIC                {}", info.wicAvailable ? "available" : "UNAVAILABLE"));
}

}  // namespace srl
