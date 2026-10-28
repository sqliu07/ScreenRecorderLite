#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace srl {

enum class RecordingFormat : std::uint8_t { Mp4, Gif };

inline constexpr std::array kMp4FrameRates{30, 60};
inline constexpr std::array kGifFrameRates{10, 15, 20, 25};
inline constexpr int kDefaultMp4FrameRate = 30;
inline constexpr int kDefaultGifFrameRate = 15;

[[nodiscard]] constexpr bool IsFrameRateSupported(RecordingFormat format, int fps) {
    if (format == RecordingFormat::Gif) {
        for (int rate : kGifFrameRates) if (fps == rate) return true;
    } else if (format == RecordingFormat::Mp4) {
        for (int rate : kMp4FrameRates) if (fps == rate) return true;
    }
    return false;
}

[[nodiscard]] constexpr std::wstring_view RecordingExtension(RecordingFormat format) {
    return format == RecordingFormat::Gif ? L".gif" : L".mp4";
}

}  // namespace srl
