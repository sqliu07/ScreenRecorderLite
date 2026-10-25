#pragma once

#include <cstdint>

namespace srl {

constexpr std::uint64_t kMiB = 1024ULL * 1024ULL;
constexpr std::uint32_t kAudioBitrateBps = 128'000;  // AAC-LC, fixed by FR-03/FR-10

// Decimal Mbps to bits per second (FR-03 uses decimal units).
[[nodiscard]] std::uint32_t MbpsToBitsPerSecond(double megabitsPerSecond);

// Estimated bytes on disk for one second of recording. Audio contributes the
// fixed 128 kbps track only when at least one source is enabled; two sources
// still produce one mixed track (FR-06).
[[nodiscard]] std::uint64_t EstimateBytesPerSecond(std::uint32_t videoBitrateBps,
                                                   bool audioEnabled);

[[nodiscard]] std::uint64_t EstimateBytes(std::uint32_t videoBitrateBps, bool audioEnabled,
                                          std::uint32_t seconds);

// FR-06: max(512 MiB, 60 s estimate + 128 MiB).
[[nodiscard]] std::uint64_t StartSpaceRequirement(std::uint32_t videoBitrateBps,
                                                  bool audioEnabled);

// FR-06: max(128 MiB, 10 s estimate).
[[nodiscard]] std::uint64_t StopSpaceThreshold(std::uint32_t videoBitrateBps, bool audioEnabled);

// Multi-track recordings contain a mixed AAC track plus one AAC track per source.
[[nodiscard]] std::uint64_t EstimateBytesWithAudioTracks(
    std::uint32_t videoBitrateBps, std::uint32_t audioTrackCount,
    std::uint32_t seconds);
[[nodiscard]] std::uint64_t StartSpaceRequirementWithAudioTracks(
    std::uint32_t videoBitrateBps, std::uint32_t audioTrackCount);
[[nodiscard]] std::uint64_t StopSpaceThresholdWithAudioTracks(
    std::uint32_t videoBitrateBps, std::uint32_t audioTrackCount);

}  // namespace srl
