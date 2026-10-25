#include "probe/test_tone.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <cmath>
#include <format>

#include "common/win32.h"

namespace probe {
namespace {

constexpr std::uint32_t kRate = 48'000;
constexpr std::uint16_t kChannels = 2;
constexpr std::uint16_t kBits = 16;

void Append16(std::vector<std::uint8_t>& data, std::uint16_t value) {
    data.push_back(static_cast<std::uint8_t>(value & 0xFF));
    data.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
}

void Append32(std::vector<std::uint8_t>& data, std::uint32_t value) {
    data.push_back(static_cast<std::uint8_t>(value & 0xFF));
    data.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    data.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    data.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
}

}  // namespace

TestTone::~TestTone() {
    Stop();
}

bool TestTone::Start(int seconds, std::wstring* error) {
    Stop();
    if (seconds < 1 || seconds > 120) {
        if (error != nullptr) {
            *error = L"测试音时长无效";
        }
        return false;
    }
    const std::uint32_t totalFrames = static_cast<std::uint32_t>(seconds) * kRate;
    const std::uint32_t dataBytes = totalFrames * kChannels * (kBits / 8);
    wav_.clear();
    wav_.reserve(44 + dataBytes);
    wav_.insert(wav_.end(), {'R', 'I', 'F', 'F'});
    Append32(wav_, 36 + dataBytes);
    wav_.insert(wav_.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    Append32(wav_, 16);
    Append16(wav_, 1);  // PCM
    Append16(wav_, kChannels);
    Append32(wav_, kRate);
    Append32(wav_, kRate * kChannels * (kBits / 8));
    Append16(wav_, kChannels * (kBits / 8));
    Append16(wav_, kBits);
    wav_.insert(wav_.end(), {'d', 'a', 't', 'a'});
    Append32(wav_, dataBytes);

    const std::uint32_t fadeFrames = kRate / 50;  // 20 ms
    for (std::uint32_t frame = 0; frame < totalFrames; ++frame) {
        double envelope = 0.2;
        if (frame < fadeFrames) {
            envelope *= static_cast<double>(frame) / fadeFrames;
        } else if (frame + fadeFrames > totalFrames) {
            envelope *= static_cast<double>(totalFrames - frame) / fadeFrames;
        }
        const double time = static_cast<double>(frame) / kRate;
        const auto value = static_cast<std::int16_t>(
            std::sin(2.0 * 3.14159265358979323846 * 440.0 * time) * envelope * 32767.0);
        for (std::uint16_t channel = 0; channel < kChannels; ++channel) {
            Append16(wav_, static_cast<std::uint16_t>(value));
        }
    }

    const BOOL ok = ::PlaySoundW(reinterpret_cast<LPCWSTR>(wav_.data()), nullptr,
                                 SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    if (!ok) {
        if (error != nullptr) {
            *error = L"测试音播放失败（默认播放设备不可用？）";
        }
        return false;
    }
    return true;
}

void TestTone::Stop() {
    ::PlaySoundW(nullptr, nullptr, 0);
    wav_.clear();
}

}  // namespace probe
