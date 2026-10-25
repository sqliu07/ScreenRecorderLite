#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/output_size.h"

namespace srl {

// CPU BGRA -> NV12 converter with independent crop, bilinear scaling and
// BT.709 limited-range conversion. The output is padded with black to the
// requested even width/height; source edges are never cropped away.
class BgraToNv12Converter {
public:
    BgraToNv12Converter() = default;

    // sourceWidth/sourceHeight describe the captured BGRA buffer; the crop
    // rectangle is expressed in that buffer's top-left coordinates.
    // contentWidth/Height is the scaled picture before 4:2:0 padding, and
    // outputWidth/Height is the even-sized video frame.
    bool Configure(int sourceWidth, int sourceHeight, int cropX, int cropY,
                   int cropWidth, int cropHeight, int contentWidth, int contentHeight,
                   int outputWidth, int outputHeight, std::wstring* error = nullptr);

    void Convert(const std::uint8_t* bgra, int sourceStride,
                 std::vector<std::uint8_t>& nv12) const;

    [[nodiscard]] Size outputSize() const { return outputSize_; }
    [[nodiscard]] Size contentSize() const { return contentSize_; }
    [[nodiscard]] bool configured() const { return configured_; }

private:
    struct AxisMap {
        std::vector<int> index0;
        std::vector<int> index1;
        std::vector<int> weight;  // 0..256, index1 gets 256 - weight
    };

    [[nodiscard]] static AxisMap BuildMap(int cropStart, int cropSize, int outputSize);
    [[nodiscard]] static AxisMap BuildChromaMap(int cropStart, int cropSize, int contentSize,
                                                int outputSize);
    void Reset();

    bool configured_ = false;
    Size outputSize_{};
    Size contentSize_{};
    AxisMap xMap_;
    AxisMap yMap_;
    AxisMap xChroma_;
    AxisMap yChroma_;
    int sourceWidth_ = 0;
    int sourceHeight_ = 0;
    int cropX_ = 0;
    int cropY_ = 0;
    int cropWidth_ = 0;
    int cropHeight_ = 0;
};

}  // namespace srl
