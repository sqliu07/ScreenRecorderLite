#include "core/bgra_to_nv12.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <limits>

namespace srl {
namespace {

int ClampInt(int value, int low, int high) {
    return std::max(low, std::min(high, value));
}

struct Bgr {
    int b = 0;
    int g = 0;
    int r = 0;
};

inline Bgr SampleBilinear(const std::uint8_t* row0, const std::uint8_t* row1, int x0, int x1,
                          int weightX, int weightY) {
    const int wx1 = 256 - weightX;
    const int wy1 = 256 - weightY;
    Bgr result;
    result.b = ((row0[x0 * 4] * wx1 + row0[x1 * 4] * weightX) * wy1 +
                (row1[x0 * 4] * wx1 + row1[x1 * 4] * weightX) * weightY) >>
               16;
    result.g = ((row0[x0 * 4 + 1] * wx1 + row0[x1 * 4 + 1] * weightX) * wy1 +
                (row1[x0 * 4 + 1] * wx1 + row1[x1 * 4 + 1] * weightX) * weightY) >>
               16;
    result.r = ((row0[x0 * 4 + 2] * wx1 + row0[x1 * 4 + 2] * weightX) * wy1 +
                (row1[x0 * 4 + 2] * wx1 + row1[x1 * 4 + 2] * weightX) * weightY) >>
               16;
    return result;
}

inline int Luma709Limited(const Bgr& bgr) {
    // BT.709 luma, then limited range 16..235.
    const int luma = (55 * bgr.r + 183 * bgr.g + 19 * bgr.b + 128) >> 8;
    return ClampInt(16 + (luma * 219 + 127) / 255, 16, 235);
}

inline int ChromaBlue(const Bgr& bgr) {
    const int cb = (-29 * bgr.r - 99 * bgr.g + 128 * bgr.b + 128) >> 8;
    return ClampInt(128 + cb * 224 / 255, 16, 240);
}

inline int ChromaRed(const Bgr& bgr) {
    const int cr = (128 * bgr.r - 116 * bgr.g - 12 * bgr.b + 128) >> 8;
    return ClampInt(128 + cr * 224 / 255, 16, 240);
}

}  // namespace

BgraToNv12Converter::AxisMap BgraToNv12Converter::BuildMap(int cropStart, int cropSize,
                                                           int outputSize) {
    AxisMap map;
    map.index0.resize(static_cast<std::size_t>(std::max(0, outputSize)));
    map.index1.resize(static_cast<std::size_t>(std::max(0, outputSize)));
    map.weight.resize(static_cast<std::size_t>(std::max(0, outputSize)));

    const int cropEnd = cropStart + cropSize - 1;
    for (int i = 0; i < outputSize; ++i) {
        const double position =
            static_cast<double>(cropStart) +
            (static_cast<double>(i) + 0.5) * static_cast<double>(cropSize) /
                static_cast<double>(outputSize) -
            0.5;
        const double clamped = std::clamp(position, static_cast<double>(cropStart),
                                          static_cast<double>(cropEnd));
        const int base = static_cast<int>(std::floor(clamped));
        const int next = std::min(base + 1, cropEnd);
        const int weight = static_cast<int>(std::llround((clamped - base) * 256.0));
        map.index0[static_cast<std::size_t>(i)] = base;
        map.index1[static_cast<std::size_t>(i)] = next;
        map.weight[static_cast<std::size_t>(i)] = std::clamp(weight, 0, 256);
    }
    return map;
}

bool BgraToNv12Converter::Configure(int sourceWidth, int sourceHeight, int cropX, int cropY,
                                    int cropWidth, int cropHeight, int contentWidth,
                                    int contentHeight, int outputWidth, int outputHeight,
                                    std::wstring* error) {
    Reset();
    const auto fail = [error](std::wstring message) {
        if (error != nullptr) {
            *error = std::move(message);
        }
        return false;
    };

    if (sourceWidth <= 0 || sourceHeight <= 0 ||
        sourceWidth > std::numeric_limits<int>::max() / 4) {
        return fail(L"source size must be positive");
    }
    if (cropWidth <= 0 || cropHeight <= 0 || cropX < 0 || cropY < 0 ||
        cropX > sourceWidth || cropY > sourceHeight ||
        cropWidth > sourceWidth - cropX || cropHeight > sourceHeight - cropY) {
        return fail(L"crop rectangle is outside the source image");
    }
    if (contentWidth <= 0 || contentHeight <= 0 || outputWidth <= 0 || outputHeight <= 0) {
        return fail(L"output sizes must be positive");
    }
    if ((outputWidth % 2) != 0 || (outputHeight % 2) != 0) {
        return fail(L"NV12 output dimensions must be even");
    }
    if (outputWidth < contentWidth || outputHeight < contentHeight ||
        outputWidth - contentWidth > 1 || outputHeight - contentHeight > 1) {
        return fail(L"output must equal content or add exactly one padding pixel");
    }
    if (outputWidth > 4096 || outputHeight > 4096) {
        return fail(L"output exceeds 4096 pixels on an axis");
    }

    sourceWidth_ = sourceWidth;
    sourceHeight_ = sourceHeight;
    cropX_ = cropX;
    cropY_ = cropY;
    cropWidth_ = cropWidth;
    cropHeight_ = cropHeight;
    contentSize_ = Size{contentWidth, contentHeight};
    outputSize_ = Size{outputWidth, outputHeight};

    xMap_ = BuildMap(cropX, cropWidth, contentWidth);
    yMap_ = BuildMap(cropY, cropHeight, contentHeight);
    xChroma_ = BuildChromaMap(cropX, cropWidth, contentWidth, outputWidth / 2);
    yChroma_ = BuildChromaMap(cropY, cropHeight, contentHeight, outputHeight / 2);
    configured_ = true;
    return true;
}

BgraToNv12Converter::AxisMap BgraToNv12Converter::BuildChromaMap(int cropStart, int cropSize,
                                                                 int contentSize,
                                                                 int outputSize) {
    AxisMap map;
    const int count = std::max(0, outputSize);
    map.index0.resize(static_cast<std::size_t>(count));
    map.index1.resize(static_cast<std::size_t>(count));
    map.weight.resize(static_cast<std::size_t>(count));
    const int cropEnd = cropStart + cropSize - 1;

    for (int i = 0; i < count; ++i) {
        const double position =
            static_cast<double>(cropStart) +
            (2.0 * static_cast<double>(i) + 1.0) * static_cast<double>(cropSize) /
                static_cast<double>(contentSize) -
            0.5;
        const double clamped = std::clamp(position, static_cast<double>(cropStart),
                                          static_cast<double>(cropEnd));
        const int base = static_cast<int>(std::floor(clamped));
        const int next = std::min(base + 1, cropEnd);
        const int weight = static_cast<int>(std::llround((clamped - base) * 256.0));
        map.index0[static_cast<std::size_t>(i)] = base;
        map.index1[static_cast<std::size_t>(i)] = next;
        map.weight[static_cast<std::size_t>(i)] = std::clamp(weight, 0, 256);
    }
    return map;
}

void BgraToNv12Converter::Reset() {
    configured_ = false;
    xMap_ = {};
    yMap_ = {};
    xChroma_ = {};
    yChroma_ = {};
}

void BgraToNv12Converter::Convert(const std::uint8_t* bgra, int sourceStride,
                                  std::vector<std::uint8_t>& nv12) const {
    if (!configured_ || bgra == nullptr || sourceStride < sourceWidth_ * 4) {
        nv12.clear();
        return;
    }

    const int width = outputSize_.width;
    const int height = outputSize_.height;
    const std::size_t yPlaneSize = static_cast<std::size_t>(width) * height;
    nv12.resize(yPlaneSize + yPlaneSize / 2);
    std::uint8_t* yPlane = nv12.data();
    std::uint8_t* uvPlane = nv12.data() + yPlaneSize;

    std::memset(yPlane, 16, yPlaneSize);
    std::memset(uvPlane, 128, yPlaneSize / 2);

    const int contentW = contentSize_.width;
    const int contentH = contentSize_.height;
    for (int y = 0; y < contentH; ++y) {
        const int sy0 = yMap_.index0[static_cast<std::size_t>(y)];
        const int sy1 = yMap_.index1[static_cast<std::size_t>(y)];
        const int wy = yMap_.weight[static_cast<std::size_t>(y)];
        const auto* row0 = bgra + static_cast<std::size_t>(sy0) * sourceStride;
        const auto* row1 = bgra + static_cast<std::size_t>(sy1) * sourceStride;
        std::uint8_t* out = yPlane + static_cast<std::size_t>(y) * width;
        for (int x = 0; x < contentW; ++x) {
            const Bgr bgr = SampleBilinear(row0, row1, xMap_.index0[static_cast<std::size_t>(x)],
                                           xMap_.index1[static_cast<std::size_t>(x)],
                                           xMap_.weight[static_cast<std::size_t>(x)], wy);
            out[x] = static_cast<std::uint8_t>(Luma709Limited(bgr));
        }
    }

    const int chromaH = height / 2;
    const int chromaW = width / 2;
    for (int cy = 0; cy < chromaH; ++cy) {
        if (cy * 2 >= contentH) {
            break;
        }
        const int sy0 = yChroma_.index0[static_cast<std::size_t>(cy)];
        const int sy1 = yChroma_.index1[static_cast<std::size_t>(cy)];
        const int wy = yChroma_.weight[static_cast<std::size_t>(cy)];
        const auto* row0 = bgra + static_cast<std::size_t>(sy0) * sourceStride;
        const auto* row1 = bgra + static_cast<std::size_t>(sy1) * sourceStride;
        std::uint8_t* out = uvPlane + static_cast<std::size_t>(cy) * width;
        for (int cx = 0; cx < chromaW; ++cx) {
            if (cx * 2 >= contentW) {
                break;
            }
            const Bgr bgr =
                SampleBilinear(row0, row1, xChroma_.index0[static_cast<std::size_t>(cx)],
                               xChroma_.index1[static_cast<std::size_t>(cx)],
                               xChroma_.weight[static_cast<std::size_t>(cx)], wy);
            out[cx * 2] = static_cast<std::uint8_t>(ChromaBlue(bgr));
            out[cx * 2 + 1] = static_cast<std::uint8_t>(ChromaRed(bgr));
        }
    }
}

}  // namespace srl
