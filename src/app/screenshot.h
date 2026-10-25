#pragma once

#include <windows.h>

#include <cstddef>
#include <memory>
#include <string>

#include "recording/screen_capture.h"

namespace srl {

struct ScreenshotResult {
    bool ok = false;
    std::wstring path;
    std::wstring error;
};

// Captures the selected monitor, crops the monitor-relative region and writes a
// PNG into `<outputDirectory>\Screenshots`. The caller is responsible for
// hiding its own windows before calling this function.
[[nodiscard]] ScreenshotResult CaptureScreenshotPng(HMONITOR monitor, const RECT& region,
                                                    const std::wstring& outputDirectory);

struct ScreenshotEvent {
    enum class Type { Captured, Completed };
    Type type = Type::Completed;
    bool idleCapture = false;
    ScreenshotResult result;
};

// Owns at most two accepted screenshots, including results awaiting the UI.
// Captured tells the UI that an idle capture has stopped and its window may be
// restored. Completed reports the PNG result and releases a slot when read.
// Only one idle capture may be pending, so restoring the UI cannot appear in a
// second queued capture. Recording snapshots may fill both slots.
class ScreenshotWorker {
public:
    ScreenshotWorker();
    ~ScreenshotWorker();

    ScreenshotWorker(const ScreenshotWorker&) = delete;
    ScreenshotWorker& operator=(const ScreenshotWorker&) = delete;

    [[nodiscard]] bool SubmitCapture(HMONITOR monitor, const RECT& region,
                                     const std::wstring& outputDirectory,
                                     std::wstring* error);
    [[nodiscard]] bool SubmitSnapshot(ScreenshotFrame frame,
                                      const std::wstring& outputDirectory,
                                      std::wstring* error);
    [[nodiscard]] bool TryPop(ScreenshotEvent& event);
    [[nodiscard]] std::size_t pending() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace srl
