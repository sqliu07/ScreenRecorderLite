#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <vector>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/mathematics.h>
}

#include "common/win32.h"
#include "core/recording_format.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "media/mp4_muxer.h"

namespace {
int checks = 0;
int failures = 0;
void Check(bool value, const char* description) {
    ++checks;
    if (!value) { ++failures; std::printf("FAIL: %s\n", description); }
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
}

struct EncodedPacket {
    std::string bytes;
    std::int64_t dts, pts, duration;
};
struct Track {
    AVRational timeBase{};
    std::vector<EncodedPacket> packets;
    std::string handler;
    int disposition = 0;
};
std::vector<Track> ReadTracks(const std::filesystem::path& path) {
    AVFormatContext* context = nullptr;
    if (avformat_open_input(&context, srl::WideToUtf8(path.wstring()).c_str(), nullptr, nullptr) < 0)
        return {};
    std::vector<Track> tracks(context->nb_streams);
    for (unsigned i = 0; i < context->nb_streams; ++i) {
        const auto* stream = context->streams[i];
        tracks[i].timeBase = stream->time_base;
        tracks[i].disposition = stream->disposition;
        const auto* handler = av_dict_get(stream->metadata, "handler_name", nullptr, 0);
        if (handler) tracks[i].handler = handler->value;
    }
    AVPacket* packet = av_packet_alloc();
    while (packet && av_read_frame(context, packet) >= 0) {
        tracks[packet->stream_index].packets.push_back({
            std::string(reinterpret_cast<const char*>(packet->data), packet->size),
            packet->dts, packet->pts, packet->duration});
        av_packet_unref(packet);
    }
    av_packet_free(&packet);
    avformat_close_input(&context);
    return tracks;
}
std::uintmax_t LastSamplePosition(const std::filesystem::path& path) {
    AVFormatContext* context = nullptr;
    if (avformat_open_input(&context, srl::WideToUtf8(path.wstring()).c_str(), nullptr, nullptr) < 0)
        return 0;
    std::uintmax_t lastPosition = 0;
    for (unsigned stream = 0; stream < context->nb_streams; ++stream) {
        auto* track = context->streams[stream];
        for (int i = 0; i < avformat_index_get_entries_count(track); ++i) {
            const auto* entry = avformat_index_get_entry(track, i);
            if (entry && entry->pos > 0)
                lastPosition = std::max(lastPosition, static_cast<std::uintmax_t>(entry->pos));
        }
    }
    avformat_close_input(&context);
    return lastPosition;
}
void CompareTrack(const Track& source, const Track& output, bool allowTimestampCorrection = false) {
    Check(!source.packets.empty() && source.packets.size() == output.packets.size(),
          "remux retains every encoded packet");
    if (source.packets.size() != output.packets.size()) return;
    bool payloadsMatch = true, timestampsMatch = true, monotonic = true;
    std::int64_t maximumCorrection = 0;
    for (std::size_t i = 0; i < source.packets.size(); ++i) {
        const auto& before = source.packets[i];
        const auto& after = output.packets[i];
        payloadsMatch &= before.bytes == after.bytes;
        // Compare in the destination timescale, allowing its rounding precision.
        const auto dts = av_rescale_q(before.dts, source.timeBase, output.timeBase);
        const auto pts = av_rescale_q(before.pts, source.timeBase, output.timeBase);
        const auto correction = after.dts - dts;
        maximumCorrection = std::max(maximumCorrection, correction);
        if (allowTimestampCorrection) {
            timestampsMatch &= correction >= 0 && after.pts - pts == correction;
        } else {
            timestampsMatch &= correction == 0 && after.pts == pts;
        }
        if (i > 0) monotonic &= after.dts > output.packets[i-1].dts;
    }
    Check(payloadsMatch, "streamcopy does not re-encode or alter packet payloads");
    Check(timestampsMatch, "common timestamps preserve B-frame ordering and AAC priming");
    Check(monotonic, "each output track has monotonic decode timestamps");
    if (allowTimestampCorrection)
        Check(maximumCorrection > 0 && maximumCorrection < static_cast<std::int64_t>(source.packets.size()),
              "duplicate source timestamps receive only bounded one-tick adjustments");
}
void CheckEncodedContent(const std::array<std::filesystem::path, 3>& sources,
                         const std::filesystem::path& output, bool delayed = false,
                         bool allowTimestampCorrection = false) {
    const auto primary = ReadTracks(sources[0]);
    const auto system = ReadTracks(sources[1]);
    const auto microphone = ReadTracks(sources[2]);
    const auto saved = ReadTracks(output);
    Check(primary.size() == 2 && system.size() == 1 && microphone.size() == 1 && saved.size() == 4,
          "all expected encoded input and output streams exist");
    if (primary.size() != 2 || system.size() != 1 || microphone.size() != 1 || saved.size() != 4) return;
    CompareTrack(primary[0], saved[0], allowTimestampCorrection);
    CompareTrack(primary[1], saved[1]);
    CompareTrack(system[0], saved[2]);
    CompareTrack(microphone[0], saved[3]);
    Check(saved[1].handler == "Mix" && saved[2].handler == "System audio" &&
          saved[3].handler == "Microphone", "audio track names identify each source");
    Check((saved[1].disposition & AV_DISPOSITION_DEFAULT) &&
          !(saved[2].disposition & AV_DISPOSITION_DEFAULT) &&
          !(saved[3].disposition & AV_DISPOSITION_DEFAULT), "mixed track alone is the default audio");
    bool reorderedVideo = false;
    for (const auto& packet : primary[0].packets) reorderedVideo |= packet.pts != packet.dts;
    if (delayed) {
        Check(av_compare_ts(primary[1].packets.front().dts, primary[1].timeBase,
                            system[0].packets.front().dts, system[0].timeBase) > 0,
              "delayed fixture exercises different track starts on one common timeline");
    } else {
        Check(reorderedVideo && primary[1].packets.front().dts < 0,
              "fixtures exercise B-frames and negative AAC priming timestamps");
    }
}

void CheckMediaLayout(const std::filesystem::path& path) {
    Microsoft::WRL::ComPtr<IMFSourceReader> reader;
    const HRESULT opened = ::MFCreateSourceReaderFromURL(path.c_str(), nullptr,
                                                         reader.GetAddressOf());
    Check(SUCCEEDED(opened), "saved MP4 can be opened by the Windows source reader");
    if (FAILED(opened)) return;
    unsigned videoStreams = 0;
    unsigned audioStreams = 0;
    for (DWORD stream = 0; stream < 8; ++stream) {
        Microsoft::WRL::ComPtr<IMFMediaType> type;
        const HRESULT hr = reader->GetNativeMediaType(stream, 0, type.GetAddressOf());
        if (hr == MF_E_INVALIDSTREAMNUMBER) break;
        Check(SUCCEEDED(hr), "every output stream has a readable native media type");
        if (FAILED(hr)) break;
        GUID major{};
        GUID subtype{};
        type->GetGUID(MF_MT_MAJOR_TYPE, &major);
        type->GetGUID(MF_MT_SUBTYPE, &subtype);
        if (major == MFMediaType_Video) {
            ++videoStreams;
            UINT32 width = 0, height = 0;
            ::MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &width, &height);
            Check(subtype == MFVideoFormat_H264 && width == 64 && height == 64,
                  "H.264 codec and source frame dimensions survive remuxing");
        } else if (major == MFMediaType_Audio) {
            ++audioStreams;
            UINT32 rate = 0, channels = 0;
            type->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
            type->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
            Check(subtype == MFAudioFormat_AAC && rate == srl::kAudioOutputRate && channels == srl::kAudioOutputChannels,
                  "all three AAC tracks retain their native audio format");
        }
    }
    Check(videoStreams == 1 && audioStreams == 3, "output contains H.264 and three AAC tracks");
}
}

int main() {
    const HRESULT apartment = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(apartment) || FAILED(::MFStartup(MF_VERSION))) return 1;
    const std::filesystem::path directory = std::filesystem::temp_directory_path() /
        (L"srl_mux_中文 空格_" + std::to_wstring(::GetCurrentProcessId()) + L"_" +
         std::to_wstring(::GetTickCount64()));
    std::filesystem::create_directory(directory);
    // Generated test data: blue H.264 with B-frames, and distinct AAC sine tones.
    const std::filesystem::path fixtures(SRL_MUX_FIXTURE_DIR);
    const std::array<std::filesystem::path, 3> inputs{
        directory / L"video mix.mp4", directory / L"system.m4a",
        directory / L"microphone.m4a"};
    const std::array<const wchar_t*, 3> names{
        L"video-mix.mp4", L"system.m4a", L"microphone.m4a"};
    std::array<std::string, 3> originalBytes;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        std::filesystem::copy_file(fixtures / names[i], inputs[i]);
        originalBytes[i] = ReadFile(inputs[i]);
    }
    std::wstring error;
    const auto final = directory / L"result.mp4";
    const bool saved = srl::MuxMp4WithAudioTracks(inputs[0].wstring(), inputs[1].wstring(),
        inputs[2].wstring(), final.wstring(), &error);
    Check(saved && std::filesystem::exists(final), "Unicode/space paths save successfully");
    if (!saved) std::wprintf(L"Mux diagnostic: %ls\n", error.c_str());
    if (saved) { CheckMediaLayout(final); CheckEncodedContent(inputs, final); }

    const auto delayedPrimary = directory / L"delayed-primary.mp4";
    std::filesystem::copy_file(fixtures / L"delayed-video-mix.mp4", delayedPrimary);
    const auto delayedOutput = directory / L"delayed-result.mp4";
    const std::array<std::filesystem::path, 3> delayedInputs{
        delayedPrimary, inputs[1], inputs[2]};
    const bool delayedSaved = srl::MuxMp4WithAudioTracks(delayedPrimary.wstring(),
        inputs[1].wstring(), inputs[2].wstring(), delayedOutput.wstring(), &error);
    Check(delayedSaved, "different capture-relative starts save without independently zeroing tracks");
    if (delayedSaved) CheckEncodedContent(delayedInputs, delayedOutput, true);

    // Synthetic H.264 fixture: zero deltas in its video stts table reproduce
    // duplicate decode timestamps emitted by the native writer under load.
    const auto duplicateDts = directory / L"duplicate-dts.mp4";
    std::filesystem::copy_file(fixtures / L"video-mix-duplicate-dts.mp4", duplicateDts);
    const auto duplicateDtsOutput = directory / L"duplicate-dts-result.mp4";
    const bool duplicateDtsSaved = srl::MuxMp4WithAudioTracks(duplicateDts.wstring(),
        inputs[1].wstring(), inputs[2].wstring(), duplicateDtsOutput.wstring(), &error);
    Check(duplicateDtsSaved, "duplicate H.264 decode timestamps are repaired for MP4 muxing");
    if (duplicateDtsSaved) {
        const std::array<std::filesystem::path, 3> duplicateInputs{
            duplicateDts, inputs[1], inputs[2]};
        CheckEncodedContent(duplicateInputs, duplicateDtsOutput, false, true);
    }

    const std::string savedBytes = ReadFile(final);
    error.clear();
    Check(!srl::MuxMp4WithAudioTracks(inputs[0].wstring(), inputs[1].wstring(),
        inputs[2].wstring(), final.wstring(), &error), "existing output is rejected");
    Check(ReadFile(final) == savedBytes, "existing output is unchanged after collision");

    const auto missingOutput = directory / L"missing-result.mp4";
    Check(!srl::MuxMp4WithAudioTracks((directory/L"missing.mp4").wstring(),
        inputs[1].wstring(), inputs[2].wstring(), missingOutput.wstring(), &error),
        "missing input is rejected");
    Check(!std::filesystem::exists(missingOutput), "missing input never publishes output");
    const auto corrupt = directory / L"corrupt.mp4";
    std::ofstream(corrupt, std::ios::binary) << "not an MP4";
    const auto corruptOutput = directory / L"corrupt-result.mp4";
    Check(!srl::MuxMp4WithAudioTracks(corrupt.wstring(), inputs[1].wstring(),
        inputs[2].wstring(), corruptOutput.wstring(), &error), "corrupt input is rejected");
    Check(!std::filesystem::exists(corruptOutput), "corrupt input never publishes output");
    const auto truncated = directory / L"truncated.mp4";
    std::filesystem::copy_file(inputs[0], truncated);
    const auto sampleBoundary = LastSamplePosition(truncated);
    Check(sampleBoundary > 0, "fixture exposes indexed sample boundaries");
    if (sampleBoundary > 0) {
        std::filesystem::resize_file(truncated, sampleBoundary);
        const auto truncatedOutput = directory / L"truncated-result.mp4";
        Check(!srl::MuxMp4WithAudioTracks(truncated.wstring(), inputs[1].wstring(),
            inputs[2].wstring(), truncatedOutput.wstring(), &error) &&
            !std::filesystem::exists(truncatedOutput),
            "boundary-truncated media cannot silently publish a shortened recording");
    }
    std::atomic<bool> cancelled{true};
    srl::Mp4MuxOptions cancelledOptions;
    cancelledOptions.cancellation = &cancelled;
    const auto cancelledOutput = directory / L"cancelled.mp4";
    Check(!srl::MuxMp4WithAudioTracks(inputs[0].wstring(), inputs[1].wstring(),
        inputs[2].wstring(), cancelledOutput.wstring(), &error, cancelledOptions) &&
        !std::filesystem::exists(cancelledOutput), "cancelled mux never publishes output");
    srl::Mp4MuxOptions expiredOptions;
    expiredOptions.timeout = std::chrono::milliseconds(0);
    Check(!srl::MuxMp4WithAudioTracks(inputs[0].wstring(), inputs[1].wstring(),
        inputs[2].wstring(), cancelledOutput.wstring(), &error, expiredOptions),
        "expired finalization deadline fails without touching sources");
    Check(!srl::MuxMp4WithAudioTracks(L"https://127.0.0.1/untrusted.mp4",
        inputs[1].wstring(), inputs[2].wstring(), cancelledOutput.wstring(), &error),
        "remote input cannot trigger network protocol access");
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        Check(ReadFile(inputs[i]) == originalBytes[i], "source recording remains unchanged");
    }
    bool leakedWorkDirectory = false;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        leakedWorkDirectory |= entry.is_directory();
    }
    Check(!leakedWorkDirectory, "success and failure both remove owned staging directories");
    std::filesystem::remove_all(directory);
    ::MFShutdown();
    ::CoUninitialize();
    std::printf("%d checks, %d failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}
