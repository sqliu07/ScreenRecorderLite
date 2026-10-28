#include "recording/recorder.h"
#include "recording/screen_capture.h"
#include "core/recording_format.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace srl::detail {
std::int64_t AdvanceVideoFrameIndex(std::int64_t nextIndex, int fps, std::int64_t elapsed100ns);
std::int64_t VideoFrameTimestamp(std::int64_t frameIndex, int fps);
bool CopyScreenshotRegion(const std::vector<std::uint8_t>& source, Size size,
                          const RECT& region, ScreenshotFrame& frame,
                          std::wstring* error);
void AddAudioPacket(const std::vector<std::int16_t>& samples,
                    std::int64_t packetStart, std::int64_t blockStart,
                    std::int64_t blockEnd, std::int64_t enabledFrom,
                    float gain, std::vector<float>& mix);
void MixAudioPacketTimelineFullLevel(const std::vector<std::int16_t>& samples,
                            std::int64_t packetStart, std::int64_t blockStart,
                            std::int64_t blockEnd, unsigned sourceBit,
                            unsigned initialMask,
                            const std::vector<std::pair<std::int64_t, unsigned>>& transitions,
                            std::vector<float>& mix);
bool AppendAudioTransition(std::deque<std::pair<std::int64_t, unsigned>>& transitions,
                            std::int64_t qpc, unsigned mask);
}

namespace {

int failures = 0;
int checks = 0;

void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", message);
    }
}

void TestScreenshotCrop() {
    std::vector<std::uint8_t> source(4 * 3 * 4);
    for (int y = 0; y < 3; ++y) {
        for (int x = 0; x < 4; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * 4 + x) * 4;
            source[offset] = static_cast<std::uint8_t>(x);
            source[offset + 1] = static_cast<std::uint8_t>(y);
            source[offset + 2] = 100;
            source[offset + 3] = 0;
        }
    }
    srl::ScreenshotFrame frame;
    std::wstring error;
    Check(srl::detail::CopyScreenshotRegion(source, {4, 3}, RECT{1, 1, 4, 3},
                                            frame, &error), "crop is accepted");
    Check(frame.width == 3 && frame.height == 2 && frame.bgra.size() == 24,
          "odd raw screenshot keeps source dimensions without video padding");
    Check(frame.bgra[0] == 1 && frame.bgra[1] == 1 && frame.bgra[3] == 255,
          "crop origin and opaque alpha are correct");
    Check(frame.bgra[20] == 3 && frame.bgra[21] == 2,
          "right and bottom source edges are preserved");
    source.assign(source.size(), 77);
    Check(frame.bgra[0] == 1, "snapshot owns pixels independently of capture updates");
    Check(!srl::detail::CopyScreenshotRegion(source, {4, 3}, RECT{-1, 0, 2, 2},
                                             frame, &error) && frame.bgra.empty(),
          "negative crop is rejected and stale output is cleared");
    Check(!srl::detail::CopyScreenshotRegion(source, {4, 3}, RECT{0, 0, 5, 2},
                                             frame, &error), "out-of-bounds crop is rejected");
    Check(!srl::detail::CopyScreenshotRegion(source, {4, 3}, RECT{3, 0, 2, 2},
                                             frame, &error), "reverse crop is rejected");
    Check(!srl::detail::CopyScreenshotRegion(source, {4, 3}, RECT{0, 0, 0, 2},
                                             frame, &error), "empty crop is rejected");
    Check(!srl::detail::CopyScreenshotRegion({}, {4, 3}, RECT{0, 0, 4, 3},
                                             frame, &error), "missing frame storage is rejected");
    Check(!srl::detail::CopyScreenshotRegion(source,
              {std::numeric_limits<int>::max(), 3}, RECT{0, 0, 4, 3}, frame, &error),
          "unrepresentable source stride is rejected");
}

void TestAudioMuteBoundaries() {
    const std::vector<std::int16_t> packet = {100, 200, 300, 400, 500, 600, 700, 800};
    std::vector<float> mix(8, 0.0f);
    srl::detail::AddAudioPacket(packet, 10, 10, 14, 0, 1.0f, mix);
    Check(mix[0] == 100 && mix[7] == 800, "one active source retains unit gain");
    mix.assign(8, 0.0f);
    srl::detail::MixAudioPacketTimelineFullLevel(packet, 10, 10, 14, 1, 3, {}, mix);
    srl::detail::MixAudioPacketTimelineFullLevel(packet, 10, 10, 14, 2, 3, {}, mix);
    Check(mix[0] == 200 && mix[7] == 1600,
          "enabling a second source preserves both source levels before limiting");
    mix.assign(8, 0.0f);
    srl::detail::AddAudioPacket(packet, 10, 10, 14, 12, 1.0f, mix);
    Check(mix[0] == 0 && mix[3] == 0 && mix[4] == 500 && mix[7] == 800,
          "restoring audio does not replay samples from the muted interval");
    mix.assign(8, 0.0f);
    srl::detail::AddAudioPacket(packet, 10, 10, 14, 14, 1.0f, mix);
    Check(mix == std::vector<float>(8, 0.0f), "future restore point leaves silence");
    srl::detail::AddAudioPacket(packet, 10, 10, 14, 0, 0.0f, mix);
    Check(mix == std::vector<float>(8, 0.0f), "muted source contributes no samples");
    mix.assign(4, 0.0f);
    srl::detail::AddAudioPacket(packet, 10, 11, 13, 0, 1.0f, mix);
    Check(mix == std::vector<float>({300, 400, 500, 600}),
          "audio cropping preserves the common sample timeline");
}

void TestAudioControlTimeline() {
    // This is one delayed 100 ms block: the click happens halfway through
    // samples that have been captured but have not yet reached the encoder.
    const std::vector<std::int16_t> packet(4800 * 2, 1000);
    std::vector<float> mix(packet.size(), 0.0f);
    srl::detail::MixAudioPacketTimelineFullLevel(packet, 0, 0, 4800, 1, 1,
                                        {{2400, 0}}, mix);
    Check(mix[0] == 1000 && mix[4799] == 1000 && mix[4800] == 0 && mix.back() == 0,
          "muting preserves audio before the click in a delayed 100 ms block");
    mix.assign(packet.size(), 0.0f);
    srl::detail::MixAudioPacketTimelineFullLevel(packet, 0, 0, 4800, 1, 1,
                                        {{2400, 0}, {3600, 1}}, mix);
    Check(mix[4799] == 1000 && mix[4800] == 0 && mix[7199] == 0 && mix[7200] == 1000,
          "rapid mute/restore preserves both audible intervals around the silent interval");

    const std::vector<std::pair<std::int64_t, unsigned>> changes = {
        {2, 2}, {4, 1}, {6, 0}};
    const std::vector<std::int16_t> system(16, 1000);
    const std::vector<std::int16_t> microphone(16, 2000);
    mix.assign(16, 0.0f);
    srl::detail::MixAudioPacketTimelineFullLevel(system, 0, 0, 8, 1, 3, changes, mix);
    srl::detail::MixAudioPacketTimelineFullLevel(microphone, 0, 0, 8, 2, 3, changes, mix);
    Check(mix[0] == 3000 && mix[3] == 3000,
          "both enabled sources retain their original levels before an independent switch");
    Check(mix[4] == 2000 && mix[7] == 2000,
          "microphone level remains unchanged at its segment boundary");
    Check(mix[8] == 1000 && mix[11] == 1000,
          "system level remains unchanged at its segment boundary");
    Check(mix[12] == 0 && mix[15] == 0,
          "both disabled sources leave exactly the intended silent interval");
    mix.assign(8, 0.0f);
    srl::detail::MixAudioPacketTimelineFullLevel(system, 0, 2, 6, 1, 0,
                                        {{0, 1}, {3, 0}, {6, 1}}, mix);
    Check(mix[0] == 1000 && mix[1] == 1000 && mix[2] == 0 && mix[7] == 0,
          "earlier transitions establish the block state and an end boundary affects the next block");
}

void TestBoundedAudioTransitions() {
    std::deque<std::pair<std::int64_t, unsigned>> transitions;
    bool accepted = true;
    for (std::int64_t qpc = 1; qpc <= 64; ++qpc) {
        accepted = srl::detail::AppendAudioTransition(transitions, qpc,
                                                      static_cast<unsigned>(qpc % 4)) && accepted;
    }
    Check(accepted && transitions.size() == 64, "audio transition history has a fixed capacity");
    const auto full = transitions;
    Check(!srl::detail::AppendAudioTransition(transitions, 65, 0) && transitions == full,
          "full transition queue rejects a switch without changing accepted history");
    Check(srl::detail::AppendAudioTransition(transitions, 64, 3) &&
              transitions.size() == 64 && transitions.back().second == 3,
          "same-timestamp changes coalesce without exceeding the queue bound");
    Check(!srl::detail::AppendAudioTransition(transitions, 63, 1),
          "transitions cannot reorder the control timeline");
    transitions.pop_front();
    Check(srl::detail::AppendAudioTransition(transitions, 65, 0) && transitions.size() == 64,
          "pruning committed history releases capacity for a later switch");
}

void TestRecordingFormatValidation() {
    srl::RecordingConfig defaults;
    Check(defaults.format == srl::RecordingFormat::Mp4 && defaults.fps == 30,
          "existing recording configuration keeps MP4 at 30 fps by default");
    srl::RecordingConfig gif;
    gif.format = srl::RecordingFormat::Gif;
    gif.fps = 30;
    gif.outputDirectory = L".";
    gif.sourceRect = RECT{0, 0, 64, 64};
    std::wstring error;
    srl::RecordingSession invalidGif;
    Check(!invalidGif.Start(gif, &error) && error.find(L"GIF 帧率") != std::wstring::npos,
          "GIF rejects the MP4-only frame rate before initializing capture");
    gif.fps = 15;
    gif.bitrateMbps = 0;
    gif.systemAudio = true;
    gif.microphone = true;
    gif.systemAudioDeviceId = L"not-an-audio-device";
    gif.microphoneDeviceId = L"not-an-audio-device";
    srl::RecordingSession silentGif;
    error.clear();
    Check(!silentGif.Start(gif, &error) && error.find(L"找不到所选显示器") != std::wstring::npos,
          "GIF validation ignores MP4 bitrate and reaches the missing-monitor check");
    const auto audio = silentGif.audioState();
    Check(!audio.systemAudio && !audio.microphone && !audio.pending,
          "GIF never publishes either requested source as enabled");
    srl::RecordingConfig mp4;
    mp4.fps = 15;
    srl::RecordingSession invalidMp4;
    error.clear();
    Check(!invalidMp4.Start(mp4, &error) && error.find(L"MP4 帧率") != std::wstring::npos,
          "MP4 rejects GIF-only frame rates before dependency or capture checks");
}

void TestVideoSchedulingAfterStall() {
    using srl::detail::AdvanceVideoFrameIndex;
    using srl::detail::VideoFrameTimestamp;
    constexpr auto second = srl::kHundredNanosecondsPerSecond;
    Check(AdvanceVideoFrameIndex(1, 60, 2 * second) == 120,
          "a two-second encoding stall skips missed frame slots");
    Check(AdvanceVideoFrameIndex(121, 60, 2 * second) == 121,
          "the next future frame slot still waits rather than repeating the current one");
    for (int fps : {15, 30, 60}) {
        std::int64_t next = 0;
        std::int64_t now = 0;
        std::int64_t previous = -1;
        bool increasing = true;
        bool boundedWork = true;
        for (int frame = 0; frame < 180; ++frame) {
            if (frame == 20) now += second;  // Slow conversion/encoding.
            const auto index = AdvanceVideoFrameIndex(next, fps, now);
            const auto timestamp = VideoFrameTimestamp(index, fps);
            const auto mp4Timestamp = timestamp * 60'000 / second;
            increasing &= mp4Timestamp > previous;
            boundedWork &= timestamp >= now - second / fps;
            previous = mp4Timestamp;
            next = index + 1;
            now = std::max(now, timestamp) + second / (fps * 4);
        }
        Check(increasing, "video timestamps remain distinct at the native MP4 timescale after a stall");
        Check(boundedWork, "fast encoding after a stall cannot replay an overdue frame backlog");
    }
}

void TestInactiveState() {
    srl::RecordingSession session;
    srl::ScreenshotFrame frame;
    std::wstring error;
    const auto audio = session.audioState();
    Check(!audio.systemAudio && !audio.microphone && !audio.pending && audio.error.empty(),
          "inactive session reports actual sources disabled with no pending command");
    Check(!session.SetAudioEnabled(true, true), "idle session rejects audio changes");
    Check(session.audioState().revision == audio.revision,
          "a rejected idle request does not pretend to change actual audio state");
    Check(!session.CopyScreenshot(frame, &error) && !error.empty(),
          "idle session rejects screenshots with a reason");
    srl::ScreenCapture capture;
    Check(!capture.CopySnapshot(RECT{0, 0, 64, 64}, frame, &error),
          "unstarted capture rejects snapshots");
    capture.Stop();
    Check(!capture.CopySnapshot(RECT{0, 0, 64, 64}, frame, &error),
          "stopped capture rejects snapshots");
}

}  // namespace

int main() {
    TestScreenshotCrop();
    TestAudioMuteBoundaries();
    TestAudioControlTimeline();
    TestBoundedAudioTransitions();
    TestRecordingFormatValidation();
    TestVideoSchedulingAfterStall();
    TestInactiveState();
    std::printf("%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
