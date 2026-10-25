#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace srl {

struct CodecProbeOptions {
    int width = 1920;
    int height = 1080;
    int fps = 30;
    int bitrateMbps = 8;

    [[nodiscard]] std::uint32_t bitrateBps() const {
        return static_cast<std::uint32_t>(bitrateMbps) * 1'000'000U;
    }
};

enum class ProbeOutcome {
    NotAttempted,
    Success,
    Failed,
};

struct H264EncoderProbe {
    std::wstring friendlyName;
    std::wstring clsid;
    bool hardware = false;
    bool d3d11Aware = false;
    bool async = false;
    ProbeOutcome activation = ProbeOutcome::NotAttempted;
    ProbeOutcome negotiation = ProbeOutcome::NotAttempted;
    std::wstring failureStage;
    std::wstring failureDetail;
    bool rateControlSupported = false;
    bool rateControlSet = false;
    bool meanBitrateSet = false;
    bool gopSizeSet = false;
    std::wstring rateControlReadback;
    std::wstring meanBitrateReadback;
    std::wstring gopSizeReadback;
    std::wstring inputSummary;
    std::wstring outputSummary;
    std::wstring outputStreamInfo;
    bool streamStarted = false;
    std::wstring note;
};

struct AacEncoderProbe {
    bool found = false;
    std::wstring friendlyName;
    std::wstring clsid;
    bool negotiationOk = false;
    std::wstring failureStage;
    std::wstring failureDetail;
    std::wstring inputSummary;
    std::wstring outputSummary;
};

struct CodecProbeResult {
    CodecProbeOptions requested;
    std::vector<H264EncoderProbe> h264;
    AacEncoderProbe aac;
    std::wstring fatalError;
    std::wstring d3dDetail;
    bool anyH264Negotiated = false;
    bool anyHardwareNegotiated = false;
};

[[nodiscard]] CodecProbeResult ProbeCodecs(const CodecProbeOptions& options);
void PrintCodecProbe(const CodecProbeResult& result);

}  // namespace srl
