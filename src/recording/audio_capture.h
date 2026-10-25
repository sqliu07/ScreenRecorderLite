#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace srl {

struct AudioPacket {
    std::int64_t qpcPosition100ns = 0;         // absolute QPC position of the first frame
    std::vector<std::int16_t> samples;         // 48 kHz stereo interleaved
};

enum class AudioSource {
    SystemLoopback,
    Microphone,
};

// Shared-mode WASAPI capture from the default playback loopback or the default
// microphone endpoint. Both sources are converted to the same 48 kHz / stereo
// PCM16 format and keep at most 500 ms of audio queued.
class AudioCapture {
public:
    explicit AudioCapture(AudioSource source, std::wstring deviceId = {});
    ~AudioCapture();

    AudioCapture(const AudioCapture&) = delete;
    AudioCapture& operator=(const AudioCapture&) = delete;

    // Initializes the endpoint and starts the capture thread. Start is called
    // from the recording worker thread (MTA).
    bool Start(std::wstring* error);
    void Stop();
    void Join();

    [[nodiscard]] bool TryPop(AudioPacket& packet);
    [[nodiscard]] bool ended() const noexcept;
    [[nodiscard]] bool overflowed() const noexcept;
    [[nodiscard]] float inputLevel() const noexcept;
    [[nodiscard]] std::wstring error() const;
    [[nodiscard]] std::int64_t lastPacketQpc100ns() const noexcept;
    [[nodiscard]] AudioSource source() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace srl
