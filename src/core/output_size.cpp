#include "core/output_size.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace srl {
namespace {

std::optional<Size> CustomBound(const Size& custom, std::wstring* error) {
    if (custom.width < kMinOutputDimension || custom.width > kMaxOutputDimension ||
        custom.height < kMinOutputDimension || custom.height > kMaxOutputDimension) {
        if (error != nullptr) {
            *error = L"custom bound must be within 64..4096 on each axis";
        }
        return std::nullopt;
    }
    if ((custom.width % 2) != 0 || (custom.height % 2) != 0) {
        if (error != nullptr) {
            *error = L"custom bound must use even dimensions";
        }
        return std::nullopt;
    }
    return custom;
}

}  // namespace

std::optional<Size> PresetBound(ResolutionLimit limit, bool portrait) {
    switch (limit) {
        case ResolutionLimit::P720:
            return portrait ? Size{720, 1280} : Size{1280, 720};
        case ResolutionLimit::P1080:
            return portrait ? Size{1080, 1920} : Size{1920, 1080};
        case ResolutionLimit::P1440:
            return portrait ? Size{1440, 2560} : Size{2560, 1440};
        case ResolutionLimit::P2160:
            return portrait ? Size{2160, 3840} : Size{3840, 2160};
        case ResolutionLimit::Original:
        case ResolutionLimit::Custom:
            return std::nullopt;
    }
    return std::nullopt;
}

std::wstring_view LimitName(ResolutionLimit limit) {
    switch (limit) {
        case ResolutionLimit::Original:
            return L"Original";
        case ResolutionLimit::P720:
            return L"720p cap";
        case ResolutionLimit::P1080:
            return L"1080p cap";
        case ResolutionLimit::P1440:
            return L"1440p cap";
        case ResolutionLimit::P2160:
            return L"2160p cap";
        case ResolutionLimit::Custom:
            return L"Custom cap";
    }
    return L"Unknown";
}

std::optional<OutputSize> ComputeOutputSize(const OutputSizeRequest& request,
                                            std::wstring* error) {
    const auto fail = [error](std::wstring message) -> std::optional<OutputSize> {
        if (error != nullptr) {
            *error = std::move(message);
        }
        return std::nullopt;
    };

    if (request.sourceWidth <= 0 || request.sourceHeight <= 0) {
        return fail(L"source dimensions must be positive");
    }
    if (request.sourceWidth < kMinOutputDimension ||
        request.sourceHeight < kMinOutputDimension) {
        return fail(L"source region must be at least 64x64");
    }

    Size bound;
    switch (request.limit) {
        case ResolutionLimit::Original:
            bound = Size{request.sourceWidth, request.sourceHeight};
            break;
        case ResolutionLimit::Custom: {
            const auto custom = CustomBound(request.customBound, error);
            if (!custom.has_value()) {
                return std::nullopt;
            }
            bound = *custom;
            break;
        }
        default: {
            const bool portrait = request.sourceHeight > request.sourceWidth;
            const auto preset = PresetBound(request.limit, portrait);
            if (!preset.has_value()) {
                return fail(L"unknown resolution preset");
            }
            bound = *preset;
            break;
        }
    }

    if (request.limit == ResolutionLimit::Original) {
        if (request.sourceWidth > kMaxOutputDimension ||
            request.sourceHeight > kMaxOutputDimension) {
            return fail(L"original size exceeds the 4096-pixel axis limit; choose a smaller cap");
        }
        const std::uint64_t sourcePixels = static_cast<std::uint64_t>(request.sourceWidth) *
                                           static_cast<std::uint64_t>(request.sourceHeight);
        if (sourcePixels > kMaxOutputPixels) {
            return fail(L"original size exceeds 8,294,400 pixels; choose a smaller cap");
        }
    }

    // Never enlarge the source. The bound is an upper limit, not a target.
    const double scale = std::min({1.0,
                                   static_cast<double>(bound.width) / request.sourceWidth,
                                   static_cast<double>(bound.height) / request.sourceHeight});

    int contentWidth = request.sourceWidth;
    int contentHeight = request.sourceHeight;
    bool scaledDown = false;
    if (scale < 1.0) {
        contentWidth = static_cast<int>(std::llround(request.sourceWidth * scale));
        contentHeight = static_cast<int>(std::llround(request.sourceHeight * scale));
        scaledDown = true;
    }

    contentWidth = std::clamp(contentWidth, 1, kMaxOutputDimension);
    contentHeight = std::clamp(contentHeight, 1, kMaxOutputDimension);

    if (contentWidth < kMinOutputDimension || contentHeight < kMinOutputDimension) {
        return fail(L"output is smaller than 64x64; choose a larger cap");
    }

    const int padRight = contentWidth % 2;
    const int padBottom = contentHeight % 2;
    const int outputWidth = contentWidth + padRight;
    const int outputHeight = contentHeight + padBottom;

    if (outputWidth < kMinOutputDimension || outputHeight < kMinOutputDimension) {
        return fail(L"output is smaller than 64x64; choose a larger cap");
    }
    if (outputWidth > kMaxOutputDimension || outputHeight > kMaxOutputDimension) {
        return fail(L"output exceeds the 4096-pixel axis limit");
    }
    const std::uint64_t outputPixels = static_cast<std::uint64_t>(outputWidth) *
                                       static_cast<std::uint64_t>(outputHeight);
    if (outputPixels > kMaxOutputPixels) {
        return fail(L"output exceeds 8,294,400 pixels");
    }

    return OutputSize{
        .content = Size{contentWidth, contentHeight},
        .output = Size{outputWidth, outputHeight},
        .padRight = padRight,
        .padBottom = padBottom,
        .scaledDown = scaledDown,
    };
}

}  // namespace srl
