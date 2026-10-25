#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "core/output_size.h"
#include "recording/screen_capture.h"

namespace srl {

enum class RecorderState {
    Idle,
    Starting,
    Recording,
    Stopping,
    Saving,
    Finished,
    Failed,
};

struct RecordingConfig {
    HMONITOR monitor = nullptr;
    std::wstring monitorDeviceName;
    Size monitorPixelSize{};
    RECT sourceRect{};  // monitor-local physical pixels
    ResolutionLimit limit = ResolutionLimit::P1080;
    Size customBound{};
    int fps = 30;
    int bitrateMbps = 8;
    bool systemAudio = true;
    bool microphone = false;
    std::wstring systemAudioDeviceId;
    std::wstring microphoneDeviceId;
    std::wstring outputDirectory;
};

struct RecorderResult {
    bool success = false;
    std::wstring finalPath;
    std::wstring partialPath;
    std::wstring error;
    std::wstring encoderInfo;
    Size outputSize{};
    int videoFrames = 0;
    int audioChunks = 0;
    double durationSeconds = 0.0;
    bool partialRemains = false;
};

struct RecordingAudioState {
    bool systemAudio = false;
    bool microphone = false;
    bool pending = false;
    std::wstring error;
    std::uint64_t revision = 0;
};

// Owns one asynchronous recording session. All capture/encode work happens on
// the worker thread; callers poll state()/elapsed100ns() from the UI thread and
// call TakeResult() after the session reaches Finished or Failed.
class RecordingSession {
public:
    RecordingSession();
    ~RecordingSession();

    RecordingSession(const RecordingSession&) = delete;
    RecordingSession& operator=(const RecordingSession&) = delete;

    bool Start(const RecordingConfig& config, std::wstring* error);
    void Stop();
    void Wait();
    [[nodiscard]] RecorderState state() const noexcept;
    [[nodiscard]] std::int64_t elapsed100ns() const noexcept;
    [[nodiscard]] bool TakeResult(RecorderResult* result);
    [[nodiscard]] bool CopyScreenshot(ScreenshotFrame& frame, std::wstring* error) const;
    // Accepts an asynchronous source change. Poll audioState() for the actual
    // result; previously disabled sources open only in response to a request.
    [[nodiscard]] bool SetAudioEnabled(bool systemAudio, bool microphone);
    [[nodiscard]] RecordingAudioState audioState() const;
    [[nodiscard]] float microphoneLevel() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace srl
