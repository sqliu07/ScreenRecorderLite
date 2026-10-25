#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace srl {

enum class ResolutionLimit : std::uint8_t {
    Original,
    P720,
    P1080,
    P1440,
    P2160,
    Custom,
};

struct Size {
    int width = 0;
    int height = 0;

    [[nodiscard]] constexpr bool operator==(const Size&) const noexcept = default;
};

struct OutputSize {
    Size content;      // scaled content, before 4:2:0 padding
    Size output;       // final even dimensions (content + at most 1px right/bottom)
    int padRight = 0;  // black padding in physical pixels, 0 or 1
    int padBottom = 0;
    bool scaledDown = false;
};

constexpr int kMinOutputDimension = 64;
constexpr int kMaxOutputDimension = 4096;
constexpr std::uint64_t kMaxOutputPixels = 8'294'400;  // 3840x2160

struct OutputSizeRequest {
    int sourceWidth = 0;
    int sourceHeight = 0;
    ResolutionLimit limit = ResolutionLimit::P1080;
    Size customBound{};
};

// Applies FR-02: fit inside a bound without upscaling, round, then pad odd
// width/height to even on the right/bottom. Returns std::nullopt and fills
// `error` when the requested combination is not allowed.
[[nodiscard]] std::optional<OutputSize> ComputeOutputSize(const OutputSizeRequest& request,
                                                          std::wstring* error = nullptr);

// Bound for a preset. Square sources use the landscape bound (docs/03 4.3).
[[nodiscard]] std::optional<Size> PresetBound(ResolutionLimit limit, bool portrait);

[[nodiscard]] std::wstring_view LimitName(ResolutionLimit limit);

}  // namespace srl
