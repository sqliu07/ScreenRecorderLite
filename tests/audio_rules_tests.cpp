#include "core/audio_rules.h"

#include <cstdio>
#include <limits>

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

void TestQueueBudget() {
    Check(srl::AudioFrameCount(960) == 480,
          "10 ms stereo PCM16 contains 480 frames, not 240");

    std::uint32_t queued = 0;
    for (int packet = 0; packet < 50; ++packet) {
        Check(srl::AudioQueueFits(queued, 960), "500 ms of ten-ms packets fits");
        queued += static_cast<std::uint32_t>(srl::AudioFrameCount(960));
    }
    Check(!srl::AudioQueueFits(queued, 2), "one frame beyond 500 ms is rejected");
    queued -= static_cast<std::uint32_t>(srl::AudioFrameCount(960));
    Check(srl::AudioQueueFits(queued, 960), "dequeue releases exactly one packet");
    Check(!srl::AudioQueueFits(0, 48'002), "oversized single packet is rejected");
    Check(!srl::AudioQueueFits(0, 3), "partial stereo frame is rejected");
    Check(!srl::AudioQueueFits(std::numeric_limits<std::uint32_t>::max(), 2),
          "queue capacity check cannot wrap");
    Check(!srl::AudioQueueFits(0, std::numeric_limits<std::size_t>::max() - 1),
          "sample count cannot overflow before capacity check");
}

void TestTimestampValidation() {
    constexpr std::int64_t now = 10'000'000;
    Check(srl::ValidAudioTimestamp(now, now, 0, false),
          "first packet with no predecessor has a valid clock");
    Check(srl::ValidAudioTimestamp(now, now, now - 100'000, false),
          "forward timestamp remains valid after silence or discontinuity");
    Check(!srl::ValidAudioTimestamp(now, now, 0, true),
          "TIMESTAMP_ERROR is rejected even with a plausible QPC value");
    Check(!srl::ValidAudioTimestamp(0, now, 0, false), "zero timestamp is rejected");
    Check(!srl::ValidAudioTimestamp(-1, now, 0, false), "negative timestamp is rejected");
    Check(!srl::ValidAudioTimestamp(now - 1, now, now, false),
          "backwards clock cannot enter the mixing queue");
    Check(!srl::ValidAudioTimestamp(now, now, now, false),
          "duplicate packet timestamps cannot overlap in the mixer");
    Check(!srl::ValidAudioTimestamp(now + 1'000'001, now, 0, false),
          "future packet cannot accumulate indefinitely in the mixer");
    Check(!srl::ValidAudioTimestamp(now - 5'000'001, now, 0, false),
          "packet older than the queue budget is rejected");
    Check(srl::ValidAudioTimestamp(now - 5'000'000, now, 0, false),
          "packet exactly at the queue age limit is accepted");
    Check(srl::ValidAudioTimestamp(now + 1'000'000, now, 0, false),
          "bounded QPC timestamp jitter is accepted");
    Check(!srl::ValidAudioTimestamp(std::numeric_limits<std::int64_t>::max(),
                                  now, 0, false),
          "extreme future timestamp is rejected without arithmetic overflow");
    Check(!srl::ValidAudioTimestamp(1, std::numeric_limits<std::int64_t>::max(),
                                  0, false),
          "extreme stale timestamp is rejected without arithmetic overflow");
}

}  // namespace

int main() {
    TestQueueBudget();
    TestTimestampValidation();
    std::printf("%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
