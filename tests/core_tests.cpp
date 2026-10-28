#include "core/bgra_to_nv12.h"
#include "core/output_size.h"
#include "core/storage.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool condition, std::string_view what) {
    ++g_checks;
    if (!condition) {
        ++g_failures;
        std::printf("FAIL: %.*s\n", static_cast<int>(what.size()), what.data());
    }
}

void CheckRule(const srl::OutputSizeRequest& request, const srl::Size& content,
               const srl::Size& output, int padRight, int padBottom, bool scaledDown,
               std::string_view what) {
    std::wstring error;
    const auto result = srl::ComputeOutputSize(request, &error);
    if (!result.has_value()) {
        ++g_failures;
        std::printf("FAIL: %.*s (unexpected error)\n", static_cast<int>(what.size()),
                    what.data());
        return;
    }
    ++g_checks;
    if (result->content != content || result->output != output ||
        result->padRight != padRight || result->padBottom != padBottom ||
        result->scaledDown != scaledDown) {
        ++g_failures;
        std::printf(
            "FAIL: %.*s (content %dx%d output %dx%d pad %d/%d scaled %d; "
            "expected content %dx%d output %dx%d pad %d/%d scaled %d)\n",
            static_cast<int>(what.size()), what.data(), result->content.width,
            result->content.height, result->output.width, result->output.height,
            result->padRight, result->padBottom, result->scaledDown ? 1 : 0, content.width,
            content.height, output.width, output.height, padRight, padBottom,
            scaledDown ? 1 : 0);
    }
}

void CheckRejected(const srl::OutputSizeRequest& request, std::string_view what) {
    std::wstring error;
    const auto result = srl::ComputeOutputSize(request, &error);
    ++g_checks;
    if (result.has_value()) {
        ++g_failures;
        std::printf("FAIL: %.*s (expected rejection, got %dx%d)\n",
                    static_cast<int>(what.size()), what.data(), result->output.width,
                    result->output.height);
    } else if (error.empty()) {
        ++g_failures;
        std::printf("FAIL: %.*s (rejected without an explanation)\n",
                    static_cast<int>(what.size()), what.data());
    }
}

srl::OutputSizeRequest Request(int width, int height, srl::ResolutionLimit limit,
                               srl::Size custom = {}) {
    return srl::OutputSizeRequest{
        .sourceWidth = width,
        .sourceHeight = height,
        .limit = limit,
        .customBound = custom,
    };
}

void TestOutputSize() {
    using enum srl::ResolutionLimit;

    // FR-02 examples.
    CheckRule(Request(3840, 2160, P1080), {1920, 1080}, {1920, 1080}, 0, 0, true, "4K -> 1080p");
    CheckRule(Request(1600, 1000, P1080), {1600, 1000}, {1600, 1000}, 0, 0, false,
              "1600x1000 stays native");
    CheckRule(Request(1001, 701, Original), {1001, 701}, {1002, 702}, 1, 1, false,
              "1001x701 original pads to even");

    // Caps and no upscaling.
    CheckRule(Request(1920, 1080, P720), {1280, 720}, {1280, 720}, 0, 0, true, "1080p -> 720p");
    CheckRule(Request(1000, 1000, P1080), {1000, 1000}, {1000, 1000}, 0, 0, false,
              "1000x1000 is not upscaled");
    CheckRule(Request(100, 100, P1080), {100, 100}, {100, 100}, 0, 0, false,
              "small source is not upscaled");
    CheckRule(Request(1281, 720, P720), {1280, 719}, {1280, 720}, 0, 1, true,
              "scale then pad without cropping");
    CheckRule(Request(64, 64, Original), {64, 64}, {64, 64}, 0, 0, false, "minimum source");

    // Orientation.
    CheckRule(Request(1080, 1920, P1080), {1080, 1920}, {1080, 1920}, 0, 0, false,
              "portrait source matches portrait bound");
    CheckRule(Request(2160, 3840, P1080), {1080, 1920}, {1080, 1920}, 0, 0, true,
              "portrait 4K -> 1080p portrait");
    CheckRule(Request(3000, 3000, P1080), {1080, 1080}, {1080, 1080}, 0, 0, true,
              "square uses landscape bound");
    CheckRule(Request(4096, 2160, P2160), {3840, 2025}, {3840, 2026}, 0, 1, true,
              "2160p cap fits 4096-wide source");

    // Custom bounds.
    CheckRule(Request(1600, 800, Custom, {1280, 720}), {1280, 640}, {1280, 640}, 0, 0, true,
              "custom width binds");
    CheckRule(Request(1000, 1000, Custom, {4096, 4096}), {1000, 1000}, {1000, 1000}, 0, 0,
              false, "custom bound does not upscale");
    CheckRule(Request(2560, 3240, Original), {2560, 3240}, {2560, 3240}, 0, 0, false,
              "original at the exact pixel budget");

    // Rejections.
    CheckRejected(Request(5000, 1000, Original), "original axis limit");
    CheckRejected(Request(4096, 2025, Original), "original padded pixels exceed cap");
    CheckRejected(Request(2560, 3241, Original), "original pixel cap");
    CheckRejected(Request(63, 100, Original), "source below minimum");
    CheckRejected(Request(1000, 100, Custom, {64, 64}), "custom output below minimum");
    CheckRejected(Request(1000, 100, Custom, {100, 100}), "custom height below minimum");
    CheckRejected(Request(1000, 1000, Custom, {1280, 719}), "odd custom bound");
    CheckRejected(Request(1000, 1000, Custom, {32, 32}), "custom bound below minimum");
    CheckRejected(Request(1000, 1000, Custom, {8192, 4320}), "custom bound above maximum");
    CheckRejected(Request(4000, 3000, Custom, {4096, 4096}), "custom output above pixel budget");
}

void FillBgra(std::vector<std::uint8_t>& buffer, int width, int height, std::uint8_t b,
              std::uint8_t g, std::uint8_t r) {
    buffer.resize(static_cast<std::size_t>(width) * height * 4);
    for (std::size_t i = 0; i < buffer.size(); i += 4) {
        buffer[i] = b;
        buffer[i + 1] = g;
        buffer[i + 2] = r;
        buffer[i + 3] = 255;
    }
}

void TestBgraToNv12() {
    std::wstring error;

    // Solid red 2x2 -> NV12 limited-range values.
    {
        std::vector<std::uint8_t> source;
        FillBgra(source, 2, 2, 0, 0, 255);
        srl::BgraToNv12Converter converter;
        Check(converter.Configure(2, 2, 0, 0, 2, 2, 2, 2, 2, 2, &error),
              "configure solid red");
        std::vector<std::uint8_t> nv12;
        converter.Convert(source.data(), 8, nv12);
        Check(nv12.size() == 6, "NV12 size for 2x2");
        Check(std::abs(static_cast<int>(nv12[0]) - 63) <= 2, "red luma 63");
        Check(std::abs(static_cast<int>(nv12[4]) - 103) <= 2, "red U 103");
        Check(std::abs(static_cast<int>(nv12[5]) - 240) <= 2, "red V 240");
    }

    // Solid white and black limited-range levels.
    {
        std::vector<std::uint8_t> white;
        FillBgra(white, 2, 2, 255, 255, 255);
        srl::BgraToNv12Converter converter;
        Check(converter.Configure(2, 2, 0, 0, 2, 2, 2, 2, 2, 2, &error),
              "configure white");
        std::vector<std::uint8_t> nv12;
        converter.Convert(white.data(), 8, nv12);
        Check(std::abs(static_cast<int>(nv12[0]) - 235) <= 2, "white luma 235");
        Check(std::abs(static_cast<int>(nv12[4]) - 128) <= 2, "white U 128");
        Check(std::abs(static_cast<int>(nv12[5]) - 128) <= 2, "white V 128");

        std::vector<std::uint8_t> black;
        FillBgra(black, 2, 2, 0, 0, 0);
        converter.Convert(black.data(), 8, nv12);
        Check(std::abs(static_cast<int>(nv12[0]) - 16) <= 2, "black luma 16");
        Check(std::abs(static_cast<int>(nv12[4]) - 128) <= 2, "black U 128");
    }

    // Three-pixel content padded to even 4x4: edges stay black.
    {
        std::vector<std::uint8_t> source;
        FillBgra(source, 3, 3, 0, 0, 255);
        srl::BgraToNv12Converter converter;
        Check(converter.Configure(3, 3, 0, 0, 3, 3, 3, 3, 4, 4, &error),
              "configure padded 3x3");
        std::vector<std::uint8_t> nv12;
        converter.Convert(source.data(), 12, nv12);
        Check(nv12.size() == 24, "NV12 size for 4x4");
        Check(nv12[3] == 16, "right padding luma is black");
        Check(nv12[3 * 4] == 16, "bottom padding luma is black");
        Check(nv12[3 * 4 + 3] == 16, "corner padding luma is black");
    }

    // Crop scale: left half red, right half blue.
    {
        std::vector<std::uint8_t> source(4 * 4 * 4, 0);
        for (int y = 0; y < 4; ++y) {
            for (int x = 0; x < 4; ++x) {
                const std::size_t offset = (static_cast<std::size_t>(y) * 4 + x) * 4;
                if (x < 2) {
                    source[offset + 2] = 255;
                } else {
                    source[offset] = 255;
                }
                source[offset + 3] = 255;
            }
        }
        srl::BgraToNv12Converter converter;
        Check(converter.Configure(4, 4, 0, 0, 4, 4, 2, 2, 2, 2, &error),
              "configure crop scale");
        std::vector<std::uint8_t> nv12;
        converter.Convert(source.data(), 16, nv12);
        Check(nv12.size() == 6, "NV12 size for scaled 2x2");
        Check(std::abs(static_cast<int>(nv12[0]) - 63) <= 3, "scaled left red");
        Check(std::abs(static_cast<int>(nv12[1]) - 32) <= 3, "scaled right blue luma");
        // One chroma block covers the whole 2x2 output, so it blends the red
        // and blue halves instead of matching either side.
        Check(std::abs(static_cast<int>(nv12[4]) - 172) <= 5, "blended U");
        Check(std::abs(static_cast<int>(nv12[5]) - 179) <= 5, "blended V");
    }

    // Rejections.
    {
        srl::BgraToNv12Converter converter;
        Check(!converter.Configure(4, 4, 1, 1, 5, 2, 2, 2, 2, 2, &error),
              "crop outside source rejected");
        Check(!converter.Configure(4, 4, 0, 0, 4, 4, 2, 2, 3, 2, &error),
              "odd NV12 width rejected");
        Check(!converter.Configure(4, 4, 0, 0, 4, 4, 3, 3, 2, 2, &error),
              "content larger than output rejected");
        Check(!converter.Configure(4, 4, 0, 0, 4, 4, 2, 2, 4, 4, &error),
              "padding larger than one pixel rejected");
        Check(!converter.Configure(4, 4, std::numeric_limits<int>::max(), 0,
                                   2, 2, 2, 2, 2, 2, &error),
              "overflowing crop coordinates rejected");
    }
    {
        srl::BgraToNv12Converter converter;
        Check(converter.Configure(2, 2, 0, 0, 2, 2, 2, 2, 2, 2, &error),
              "configure stride validation");
        std::vector<std::uint8_t> source;
        FillBgra(source, 2, 2, 255, 255, 255);
        std::vector<std::uint8_t> nv12{1, 2, 3};
        converter.Convert(source.data(), 7, nv12);
        Check(nv12.empty(), "short BGRA stride produces no output");
        converter.Convert(source.data(), 0, nv12);
        Check(nv12.empty(), "zero BGRA stride produces no output");
    }
}

void TestStorage() {
    Check(srl::MbpsToBitsPerSecond(8.0) == 8'000'000, "8 Mbps decimal conversion");
    Check(srl::MbpsToBitsPerSecond(0.5) == 500'000, "0.5 Mbps conversion");
    Check(srl::MbpsToBitsPerSecond(0.0) == 0, "zero Mbps");
    Check(srl::MbpsToBitsPerSecond(-1.0) == 0, "negative Mbps clamped to zero");

    Check(srl::EstimateBytesWithAudioTracks(8'000'000, 3, 60) == 62'880'000,
          "recording estimate includes three independent 128 kbps AAC tracks");
    Check(srl::EstimateBytesWithAudioTracks(8'000'000, 0, 60) == 60'000'000,
          "zero audio tracks contributes only the video payload");
    Check(srl::EstimateBytesWithAudioTracks(8'000'000, 1, 60) == 60'960'000,
          "one audio track contributes 128 kbps");
    Check(srl::EstimateBytesWithAudioTracks(8'000'000, 3, 0) == 0,
          "zero duration contributes no recording bytes");
    Check(srl::EstimateBytesWithAudioTracks(1, 3, 1) == 48'001,
          "video bits round up independently of exact AAC byte rates");
    Check(srl::StartSpaceRequirementWithAudioTracks(8'000'000, 3) == 512 * srl::kMiB,
          "three audio tracks preserve the minimum start-space floor");
    Check(srl::StopSpaceThresholdWithAudioTracks(8'000'000, 3) == 128 * srl::kMiB,
          "three audio tracks preserve the minimum stop-space floor");
    Check(srl::StartSpaceRequirementWithAudioTracks(100'000'000, 3) ==
              752'880'000 + 128 * srl::kMiB,
          "start-space estimate adds the safety margin above its floor");
    Check(srl::StopSpaceThresholdWithAudioTracks(1'000'000'000, 3) == 1'250'480'000,
          "stop-space threshold uses the ten-second estimate above its floor");
}

}  // namespace

int main() {
    TestOutputSize();
    TestBgraToNv12();
    TestStorage();
    std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
