#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace probe {

// Plays a short 440 Hz stereo tone through the default playback device so the
// loopback recorder has deterministic audio to capture.
class TestTone {
public:
    ~TestTone();
    bool Start(int seconds, std::wstring* error);
    void Stop();

private:
    std::vector<std::uint8_t> wav_;
};

}  // namespace probe
