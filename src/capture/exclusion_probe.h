#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace srl {

struct ExclusionProbeOptions {
    int monitorIndex = 0;  // 0 = primary
    int visibleDurationMs = 500;
    bool verifyWithCapture = false;
};

struct ExclusionProbeResult {
    bool ok = false;
    std::wstring error;
    bool windowCreated = false;
    bool affinitySet = false;
    DWORD affinityError = ERROR_SUCCESS;
    bool supportedBuild = false;
    RECT windowRect{};
    std::wstring detail;
    bool captureVerified = false;
    std::vector<std::uint32_t> capturedSamples;
};

// Creates a short-lived topmost window, applies WDA_EXCLUDEFROMCAPTURE and
// reports the API result. It does not verify the captured pixels; run the WGC
// capture probe separately for the end-to-end check.
[[nodiscard]] ExclusionProbeResult RunExclusionProbe(const ExclusionProbeOptions& options);
void PrintExclusionProbe(const ExclusionProbeResult& result);

}  // namespace srl
