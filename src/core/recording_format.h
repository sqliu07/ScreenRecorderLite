#pragma once

#include <climits>
#include <cstdint>

namespace srl {

inline constexpr std::int64_t kHundredNanosecondsPerSecond = 10'000'000;
inline constexpr std::int64_t kHundredNanosecondsPerMillisecond =
    kHundredNanosecondsPerSecond / 1'000;

inline constexpr std::uint32_t kAudioOutputRate = 48'000;
inline constexpr std::uint32_t kAudioOutputChannels = 2;
inline constexpr std::uint32_t kAudioPcmBitsPerSample = 16;
inline constexpr std::uint32_t kAudioPcmBytesPerSample = kAudioPcmBitsPerSample / CHAR_BIT;
inline constexpr std::uint32_t kAudioPcmBlockAlignment =
    kAudioOutputChannels * kAudioPcmBytesPerSample;
inline constexpr std::uint32_t kAudioPcmBytesPerSecond =
    kAudioOutputRate * kAudioPcmBlockAlignment;
inline constexpr std::uint32_t kAudioBitrateBps = 128'000;
inline constexpr std::uint32_t kAudioEncodedBytesPerSecond = kAudioBitrateBps / CHAR_BIT;

enum class AudioTrack : std::uint32_t {
    Mixed,
    System,
    Microphone,
    Count,
};

inline constexpr std::uint32_t kAudioTrackCount = static_cast<std::uint32_t>(AudioTrack::Count);

static_assert(kAudioPcmBytesPerSample == sizeof(std::int16_t));
static_assert(kAudioPcmBitsPerSample % CHAR_BIT == 0);
static_assert(kAudioBitrateBps % CHAR_BIT == 0);

}  // namespace srl
