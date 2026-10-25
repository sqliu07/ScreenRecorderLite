#include "core/storage.h"

#include <algorithm>
#include <cmath>

namespace srl {
namespace {

std::uint64_t CeilDiv(std::uint64_t numerator, std::uint64_t denominator) {
    return (numerator + denominator - 1) / denominator;
}

}  // namespace

std::uint32_t MbpsToBitsPerSecond(double megabitsPerSecond) {
    if (!(megabitsPerSecond > 0.0)) {
        return 0;
    }
    return static_cast<std::uint32_t>(std::llround(megabitsPerSecond * 1'000'000.0));
}

std::uint64_t EstimateBytesPerSecond(std::uint32_t videoBitrateBps, bool audioEnabled) {
    const std::uint64_t videoBytes = CeilDiv(videoBitrateBps, 8);
    const std::uint64_t audioBytes = audioEnabled ? CeilDiv(kAudioBitrateBps, 8) : 0;
    return videoBytes + audioBytes;
}

std::uint64_t EstimateBytes(std::uint32_t videoBitrateBps, bool audioEnabled,
                            std::uint32_t seconds) {
    return EstimateBytesPerSecond(videoBitrateBps, audioEnabled) * seconds;
}

std::uint64_t StartSpaceRequirement(std::uint32_t videoBitrateBps, bool audioEnabled) {
    constexpr std::uint64_t kMinimumFree = 512 * kMiB;
    constexpr std::uint64_t kSafetyMargin = 128 * kMiB;
    return std::max(kMinimumFree, EstimateBytes(videoBitrateBps, audioEnabled, 60) + kSafetyMargin);
}

std::uint64_t StopSpaceThreshold(std::uint32_t videoBitrateBps, bool audioEnabled) {
    constexpr std::uint64_t kMinimumFree = 128 * kMiB;
    return std::max(kMinimumFree, EstimateBytes(videoBitrateBps, audioEnabled, 10));
}

std::uint64_t EstimateBytesWithAudioTracks(std::uint32_t videoBitrateBps,
                                           std::uint32_t audioTrackCount,
                                           std::uint32_t seconds) {
    const std::uint64_t videoBytes = CeilDiv(videoBitrateBps, 8);
    const std::uint64_t audioBytes = CeilDiv(
        static_cast<std::uint64_t>(kAudioBitrateBps) * audioTrackCount, 8);
    return (videoBytes + audioBytes) * seconds;
}

std::uint64_t StartSpaceRequirementWithAudioTracks(
    std::uint32_t videoBitrateBps, std::uint32_t audioTrackCount) {
    constexpr std::uint64_t kMinimumFree = 512 * kMiB;
    constexpr std::uint64_t kSafetyMargin = 128 * kMiB;
    return std::max(kMinimumFree,
        EstimateBytesWithAudioTracks(videoBitrateBps, audioTrackCount, 60) +
            kSafetyMargin);
}

std::uint64_t StopSpaceThresholdWithAudioTracks(
    std::uint32_t videoBitrateBps, std::uint32_t audioTrackCount) {
    constexpr std::uint64_t kMinimumFree = 128 * kMiB;
    return std::max(kMinimumFree,
        EstimateBytesWithAudioTracks(videoBitrateBps, audioTrackCount, 10));
}

}  // namespace srl
