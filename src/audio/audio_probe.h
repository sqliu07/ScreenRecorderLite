#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace srl {

struct AudioFormatInfo {
    std::uint16_t formatTag = 0;
    std::uint16_t channels = 0;
    std::uint32_t sampleRate = 0;
    std::uint16_t bitsPerSample = 0;
    std::uint16_t validBitsPerSample = 0;
    std::uint16_t blockAlign = 0;
    std::uint32_t averageBytesPerSecond = 0;
    std::uint32_t channelMask = 0;
    std::wstring subFormat;  // for WAVE_FORMAT_EXTENSIBLE
    bool extensible = false;

    [[nodiscard]] bool isFloat() const;
    [[nodiscard]] std::wstring Describe() const;
};

struct AudioEndpointInfo {
    std::wstring id;
    std::wstring friendlyName;
    std::wstring deviceName;
    bool render = false;
    bool defaultMultimedia = false;
    bool hasMixFormat = false;
    AudioFormatInfo mixFormat;
    std::wstring formatError;
    std::uint64_t defaultPeriod100ns = 0;
    std::uint64_t minimumPeriod100ns = 0;
};

struct AudioProbeResult {
    AudioEndpointInfo defaultRender;
    AudioEndpointInfo defaultCapture;
    bool hasDefaultRender = false;
    bool hasDefaultCapture = false;
    std::vector<AudioEndpointInfo> renderEndpoints;
    std::vector<AudioEndpointInfo> captureEndpoints;

    bool loopbackChecked = false;
    bool loopbackInitializeOk = false;
    std::wstring loopbackDetail;

    bool microphoneChecked = false;
    bool microphoneInitializeOk = false;
    std::wstring microphoneDetail;

    std::wstring fatalError;
};

// Enumerates active endpoints and reads their shared-mode mix formats. Audio
// is never recorded and no samples are read. The optional checks only open and
// immediately close a stream; `checkMicrophone` may activate the microphone.
[[nodiscard]] AudioProbeResult QueryAudioProbe(bool checkLoopback, bool checkMicrophone);
void PrintAudioProbe(const AudioProbeResult& result);

}  // namespace srl
