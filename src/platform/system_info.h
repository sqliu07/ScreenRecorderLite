#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace srl {

struct OsInfo {
    std::wstring productName;
    std::wstring displayVersion;
    std::wstring build;
    std::uint32_t major = 0;
    std::uint32_t minor = 0;
    std::uint32_t buildNumber = 0;
    std::uint32_t updateBuildRevision = 0;
    std::wstring architecture;
};

struct CpuInfo {
    std::wstring brand;
    std::uint32_t physicalCores = 0;
    std::uint32_t logicalCores = 0;
};

struct MemoryInfo {
    std::uint64_t totalBytes = 0;
};

struct GpuInfo {
    std::wstring name;
    std::uint32_t vendorId = 0;
    std::uint32_t deviceId = 0;
    std::uint64_t dedicatedVideoMemory = 0;
    std::uint64_t sharedSystemMemory = 0;
    bool software = false;
    std::int64_t luid = 0;
};

struct SystemInfo {
    OsInfo os;
    CpuInfo cpu;
    MemoryInfo memory;
    std::vector<GpuInfo> gpus;
    std::wstring videosFolder;
    bool wgcSupported = false;
    std::wstring wgcDetail;
    bool mediaFoundationAvailable = false;
    bool wicAvailable = false;
    std::wstring dpiAwareness;
    DWORD sessionId = 0;
    bool remoteSession = false;
};

[[nodiscard]] SystemInfo QuerySystemInfo();
void PrintSystemInfo(const SystemInfo& info);

}  // namespace srl
