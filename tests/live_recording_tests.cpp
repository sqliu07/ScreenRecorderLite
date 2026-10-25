#include "recording/recorder.h"
#include "recording/screen_capture.h"

#include <cstdio>
#include <deque>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace srl::detail {
bool CopyScreenshotRegion(const std::vector<std::uint8_t>& source, Size size,
                          const RECT& region, ScreenshotFrame& frame,
                          std::wstring* error);
void AddAudioPacket(const std::vector<std::int16_t>& samples,
                    std::int64_t packetStart, std::int64_t blockStart,
                    std::int64_t blockEnd, std::int64_t enabledFrom,
                    float gain, std::vector<float>& mix);
void MixAudioPacketTimeline(const std::vector<std::int16_t>& samples,
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
    srl::detail::AddAudioPacket(packet, 10, 10, 14, 0, 0.5f, mix);
    srl::detail::AddAudioPacket(packet, 10, 10, 14, 0, 0.5f, mix);
    Check(mix[0] == 100 && mix[7] == 800, "two active sources mix at half gain");
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
    srl::detail::MixAudioPacketTimeline(packet, 0, 0, 4800, 1, 1,
                                        {{2400, 0}}, mix);
    Check(mix[0] == 1000 && mix[4799] == 1000 && mix[4800] == 0 && mix.back() == 0,
          "muting preserves audio before the click in a delayed 100 ms block");
    mix.assign(packet.size(), 0.0f);
    srl::detail::MixAudioPacketTimeline(packet, 0, 0, 4800, 1, 1,
                                        {{2400, 0}, {3600, 1}}, mix);
    Check(mix[4799] == 1000 && mix[4800] == 0 && mix[7199] == 0 && mix[7200] == 1000,
          "rapid mute/restore preserves both audible intervals around the silent interval");

    const std::vector<std::pair<std::int64_t, unsigned>> changes = {
        {2, 2}, {4, 1}, {6, 0}};
    const std::vector<std::int16_t> system(16, 1000);
    const std::vector<std::int16_t> microphone(16, 2000);
    mix.assign(16, 0.0f);
    srl::detail::MixAudioPacketTimeline(system, 0, 0, 8, 1, 3, changes, mix);
    srl::detail::MixAudioPacketTimeline(microphone, 0, 0, 8, 2, 3, changes, mix);
    Check(mix[0] == 1500 && mix[3] == 1500,
          "both enabled sources use half gain before an independent switch");
    Check(mix[4] == 2000 && mix[7] == 2000,
          "microphone alone regains unit gain at its segment boundary");
    Check(mix[8] == 1000 && mix[11] == 1000,
          "system source alone regains unit gain at its segment boundary");
    Check(mix[12] == 0 && mix[15] == 0,
          "both disabled sources leave exactly the intended silent interval");
    mix.assign(8, 0.0f);
    srl::detail::MixAudioPacketTimeline(system, 0, 2, 6, 1, 0,
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
    TestInactiveState();
    std::printf("%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
