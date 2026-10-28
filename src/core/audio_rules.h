#pragma once

#include <cstddef>
#include <cstdint>

#include "core/recording_format.h"

namespace srl {

inline constexpr std::uint32_t kAudioQueueDurationMs = 500;
inline constexpr std::uint32_t kAudioMaximumFutureSkewMs = 100;
inline constexpr std::uint32_t kAudioMaxQueuedFrames =
    kAudioOutputRate * kAudioQueueDurationMs / 1'000;

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
    constexpr std::int64_t kMaximumAge =
        kAudioQueueDurationMs * kHundredNanosecondsPerMillisecond;
    constexpr std::int64_t kMaximumFutureSkew =
        kAudioMaximumFutureSkewMs * kHundredNanosecondsPerMillisecond;
    return qpc <= now ? now - qpc <= kMaximumAge
                      : qpc - now <= kMaximumFutureSkew;
}

}  // namespace srl
