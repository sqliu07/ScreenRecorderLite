#pragma once

#include <cstdint>

#include "core/recording_format.h"

namespace srl {

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;

// Decimal Mbps to bits per second.
[[nodiscard]] std::uint32_t MbpsToBitsPerSecond(double megabitsPerSecond);

// Each AAC output track contributes the fixed bitrate in recording_format.h.
[[nodiscard]] std::uint64_t EstimateBytesWithAudioTracks(
    std::uint32_t videoBitrateBps, std::uint32_t audioTrackCount,
    std::uint32_t seconds);
[[nodiscard]] std::uint64_t StartSpaceRequirementWithAudioTracks(
    std::uint32_t videoBitrateBps, std::uint32_t audioTrackCount);
[[nodiscard]] std::uint64_t StopSpaceThresholdWithAudioTracks(
    std::uint32_t videoBitrateBps, std::uint32_t audioTrackCount);

}  // namespace srl
