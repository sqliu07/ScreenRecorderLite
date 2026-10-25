#pragma once

#include <cstddef>
#include <cstdint>

namespace srl {

inline constexpr std::uint32_t kAudioOutputRate = 48'000;
inline constexpr std::uint32_t kAudioOutputChannels = 2;
inline constexpr std::uint32_t kAudioMaxQueuedFrames = kAudioOutputRate / 2;

// The buffer contains PCM16 elements, so the channel count alone determines
// the frame count. sizeof(int16_t) is relevant only when measuring bytes.
[[nodiscard]] constexpr std::size_t AudioFrameCount(std::size_t samples) noexcept {
    return samples / kAudioOutputChannels;
}

[[nodiscard]] constexpr bool AudioQueueFits(std::uint32_t queuedFrames,
                                           std::size_t samples) noexcept {
    return samples % kAudioOutputChannels == 0 &&
           queuedFrames <= kAudioMaxQueuedFrames &&
           AudioFrameCount(samples) <= kAudioMaxQueuedFrames - queuedFrames;
}

// WASAPI QPC positions are already in 100 ns units. Reject unreliable or
// unbounded positions before they can retain packets in the mixing queue.
// A discontinuity alone is not an invalid clock: startup and idle loopback
// transitions can carry that flag while reporting a usable absolute QPC.
[[nodiscard]] constexpr bool ValidAudioTimestamp(std::int64_t qpc,
                                                std::int64_t now,
                                                std::int64_t previousQpc,
                                                bool timestampError) noexcept {
    if (timestampError || qpc <= 0 || now <= 0 ||
        (previousQpc > 0 && qpc <= previousQpc)) {
        return false;
    }
    constexpr std::int64_t kMaximumAge = 5'000'000;       // 500 ms queue budget
    constexpr std::int64_t kMaximumFutureSkew = 1'000'000;  // 100 ms
    return qpc <= now ? now - qpc <= kMaximumAge
                      : qpc - now <= kMaximumFutureSkew;
}

}  // namespace srl
