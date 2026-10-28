#pragma once

#include <array>

namespace srl {

// Windows scRGB represents 80 nits with linear RGB (1, 1, 1).
inline constexpr float kScRgbReferenceWhiteNits = 80.0f;
inline constexpr float kHdrShoulderStart = 0.8f;
inline constexpr float kHdrShoulderRange = 1.0f - kHdrShoulderStart;
inline constexpr float kMaxHalfFloat = 65'504.0f;

[[nodiscard]] float NormalizeSdrWhiteScale(float scale) noexcept;
[[nodiscard]] std::array<float, 3> ToneMapScRgbToSrgb(
    std::array<float, 3> linearRgb, float sdrWhiteScale, bool hdr) noexcept;

}  // namespace srl
