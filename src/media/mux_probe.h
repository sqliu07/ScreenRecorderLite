#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace srl {

struct MuxProbeOptions {
    std::wstring outputPath;
    int width = 1920;
    int height = 1080;
    int fps = 30;
    int bitrateMbps = 8;
    int seconds = 2;
    bool includeAudio = true;
    bool hardwareTransforms = true;
};

struct TransformEvidence {
    DWORD index = 0;
    std::wstring category;
    std::wstring friendlyName;
    bool d3d11Aware = false;
    bool async = false;
    std::wstring rateControlReadback;
    std::wstring meanBitrateReadback;
    std::wstring gopSizeReadback;
    std::wstring currentOutputType;
};

struct MuxProbeResult {
    bool ok = false;
    std::wstring error;
    std::wstring outputPath;
    std::uint64_t fileSize = 0;
    std::vector<TransformEvidence> transforms;
    std::wstring videoEncoderFriendlyName;
    std::wstring requestedSummary;
    std::wstring actualVideoType;
    std::wstring actualAudioType;
    bool rateControlAppliedBeforeBegin = false;
    bool rateControlAppliedAfterBegin = false;
    int videoFramesWritten = 0;
    int audioChunksWritten = 0;
    bool finalized = false;

    bool validated = false;
    std::wstring validationError;
    double containerDurationSeconds = 0.0;
    int videoSamplesRead = 0;
    int audioSamplesRead = 0;
    double lastVideoTimestampSeconds = 0.0;
    double lastAudioTimestampSeconds = 0.0;
};

[[nodiscard]] MuxProbeResult RunMuxProbe(const MuxProbeOptions& options);
void PrintMuxProbe(const MuxProbeResult& result);

}  // namespace srl
