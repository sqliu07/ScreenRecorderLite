#pragma once

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "core/output_size.h"

namespace srl {

struct ScreenshotFrame {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> bgra;
};

// Windows Graphics Capture session that keeps the most recent desktop frame in
// CPU SDR BGRA form. Advanced-color/HDR surfaces are captured in FP16 and
// converted on the GPU before readback. CopyLatestFrame() only returns true when a newer frame is
// available, so the recorder can reuse its last converted NV12 frame for a
// static desktop.
class ScreenCapture {
public:
    ScreenCapture();
    ~ScreenCapture();

    ScreenCapture(const ScreenCapture&) = delete;
    ScreenCapture& operator=(const ScreenCapture&) = delete;

    bool Start(HMONITOR monitor, int fps, std::wstring* error);
    bool WaitForFirstFrame(int timeoutMs, std::wstring* error);
    bool CopyLatestFrame(std::vector<std::uint8_t>& bgra, int& width, int& height,
                         std::int64_t& qpc100ns);
    // Copies an owned, unscaled snapshot without consuming the recorder's
    // latest-frame generation. A healthy static frame remains valid.
    [[nodiscard]] bool CopySnapshot(const RECT& region, ScreenshotFrame& frame,
                                     std::wstring* error) const;

    [[nodiscard]] Size frameSize() const;
    [[nodiscard]] std::int64_t firstFrameQpc100ns() const;
    [[nodiscard]] bool HasError() const;
    [[nodiscard]] std::wstring Error() const;
    void Stop();

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

[[nodiscard]] std::int64_t QpcNow100ns();

}  // namespace srl
