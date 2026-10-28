#include "core/storage.h"

#include <algorithm>
#include <cmath>

namespace srl {
namespace {

constexpr std::uint64_t kStartSpaceFloor = 512 * kMiB;
constexpr std::uint64_t kStartSafetyMargin = 128 * kMiB;
constexpr std::uint64_t kStopSpaceFloor = 128 * kMiB;
constexpr std::uint32_t kStartSpaceEstimateSeconds = 60;
constexpr std::uint32_t kStopSpaceEstimateSeconds = 10;

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

std::uint64_t EstimateBytesWithAudioTracks(std::uint32_t videoBitrateBps,
                                           std::uint32_t audioTrackCount,
                                           std::uint32_t seconds) {
    const std::uint64_t videoBytes = CeilDiv(videoBitrateBps, CHAR_BIT);
    const std::uint64_t audioBytes =
        static_cast<std::uint64_t>(kAudioEncodedBytesPerSecond) * audioTrackCount;
    return (videoBytes + audioBytes) * seconds;
}

std::uint64_t StartSpaceRequirementWithAudioTracks(
    std::uint32_t videoBitrateBps, std::uint32_t audioTrackCount) {
    return std::max(kStartSpaceFloor,
        EstimateBytesWithAudioTracks(videoBitrateBps, audioTrackCount,
                                     kStartSpaceEstimateSeconds) + kStartSafetyMargin);
}

std::uint64_t StopSpaceThresholdWithAudioTracks(
    std::uint32_t videoBitrateBps, std::uint32_t audioTrackCount) {
    return std::max(kStopSpaceFloor,
        EstimateBytesWithAudioTracks(videoBitrateBps, audioTrackCount,
                                     kStopSpaceEstimateSeconds));
}

}  // namespace srl
