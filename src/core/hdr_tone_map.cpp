#include "core/hdr_tone_map.h"

#include <algorithm>
#include <cmath>

namespace srl {
namespace {
float Sanitize(float value) noexcept {
    return std::isnan(value) || value < 0.0f ? 0.0f : std::min(value, kMaxHalfFloat);
}
float EncodeSrgb(float linear) noexcept {
    const double value = std::clamp(static_cast<double>(linear), 0.0, 1.0);
    const double encoded = value <= 0.0031308 ? 12.92 * value
        : 1.055 * std::pow(value, 1.0 / 2.4) - 0.055;
    return static_cast<float>(std::clamp(encoded, 0.0, 1.0));
}
}

float NormalizeSdrWhiteScale(float scale) noexcept {
    return std::isfinite(scale) && scale > 0.0f ? scale : 1.0f;
}

std::array<float, 3> ToneMapScRgbToSrgb(std::array<float, 3> linearRgb,
                                       float sdrWhiteScale, bool hdr) noexcept {
    const float scale = NormalizeSdrWhiteScale(sdrWhiteScale);
    for (float& channel : linearRgb) channel = Sanitize(channel) / scale;
    const double luminance = 0.2126 * linearRgb[0] + 0.7152 * linearRgb[1] +
                             0.0722 * linearRgb[2];
    if (hdr && luminance > kHdrShoulderStart) {
        // Keep midtones unchanged and reserve a smooth shoulder for highlights.
        // Normalized SDR white remains close to white rather than being halved.
        const double mapped = kHdrShoulderStart + kHdrShoulderRange *
            (1.0f - std::exp(-(luminance - kHdrShoulderStart) / kHdrShoulderRange));
        for (float& channel : linearRgb) channel = static_cast<float>(channel * mapped / luminance);
    }
    for (float& channel : linearRgb) channel = EncodeSrgb(channel);
    return linearRgb;
}
}  // namespace srl
