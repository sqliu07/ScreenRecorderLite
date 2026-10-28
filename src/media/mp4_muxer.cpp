#include "media/mp4_muxer.h"

#include <windows.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

#include <algorithm>
#include <array>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

#include "common/win32.h"
#include "core/recording_format.h"

namespace srl {
namespace {
constexpr int kWorkingDirectoryAttempts = 16;
constexpr int kIndexScanInterruptInterval = 4'096;
constexpr std::uint64_t kMuxSpaceMarginBytes = 16ULL * 1024 * 1024;
constexpr std::int64_t kMaxInterleaveMicroseconds = 1'000'000;
constexpr std::int64_t kOneOutputTimestampTick = 1;
constexpr std::array<const char*, kAudioTrackCount> kTrackNames{
    "Mix", "System audio", "Microphone"};

bool Fail(std::wstring* error, std::wstring message, int code = 0) {
    if (error) {
        if (code < 0) {
            std::array<char, AV_ERROR_MAX_STRING_SIZE> detail{};
            av_strerror(code, detail.data(), detail.size());
            message += Utf8ToWide(detail.data());
        }
        *error = std::move(message);
    }
    return false;
}

struct MuxDeadline {
    const Mp4MuxOptions& options;
    ULONGLONG started = ::GetTickCount64();
    bool Interrupted() const {
        return (options.cancellation && options.cancellation->load()) ||
            options.timeout.count() <= 0 ||
            ::GetTickCount64() - started >= static_cast<ULONGLONG>(options.timeout.count());
    }
    static int Interrupt(void* opaque) {
        return static_cast<MuxDeadline*>(opaque)->Interrupted() ? 1 : 0;
    }
};

struct Dictionary {
    AVDictionary* value = nullptr;
    ~Dictionary() { av_dict_free(&value); }
};

struct MuxInput {
    AVFormatContext* context = nullptr;
    AVPacket* packet = av_packet_alloc();
    std::vector<int> streamMap;
    std::vector<std::uint64_t> packetCounts;
    bool hasPacket = false;
    bool exhausted = false;
    ~MuxInput() {
        av_packet_free(&packet);
        avformat_close_input(&context);
    }
};

struct MuxOutput {
    AVFormatContext* context = nullptr;
    ~MuxOutput() {
        if (context) {
            if (context->pb) avio_closep(&context->pb);
            avformat_free_context(context);
        }
    }
};

struct MuxWorkingFiles {
    std::wstring directory;
    std::wstring media;
    ~MuxWorkingFiles() {
        if (!media.empty()) ::DeleteFileW(media.c_str());
        if (!directory.empty()) ::RemoveDirectoryW(directory.c_str());
    }
};

bool CreateWorkingFiles(const std::wstring& output, MuxWorkingFiles& files,
                        std::wstring* error) {
    for (int attempt = 0; attempt < kWorkingDirectoryAttempts; ++attempt) {
        const std::wstring candidate = output + L".mux-" +
            std::to_wstring(::GetCurrentProcessId()) + L"-" +
            std::to_wstring(::GetTickCount64()) + L"-" + std::to_wstring(attempt);
        if (::CreateDirectoryW(candidate.c_str(), nullptr)) {
            files.directory = candidate;
            files.media = candidate + L"\\result.mp4";
            return true;
        }
        if (::GetLastError() != ERROR_ALREADY_EXISTS) break;
    }
    return Fail(error, L"无法创建视频收尾临时目录");
}

bool CheckSpace(const std::array<std::wstring, 3>& sources,
                const std::wstring& destination, std::wstring* error) {
    std::uint64_t total = 0;
    for (const auto& source : sources) {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (!::GetFileAttributesExW(source.c_str(), GetFileExInfoStandard, &data) ||
            (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            return Fail(error, L"无法打开录制素材：" + source);
        }
        const auto size = (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32) |
                           data.nFileSizeLow;
        if (size > std::numeric_limits<std::uint64_t>::max() - total)
            return Fail(error, L"录制素材大小超出支持范围");
        total += size;
    }
    const std::uint64_t margin = std::max(kMuxSpaceMarginBytes, total / 100);
    ULARGE_INTEGER free{};
    if (!::GetDiskFreeSpaceExW(std::filesystem::path(destination).parent_path().c_str(),
                               &free, nullptr, nullptr)) {
        return Fail(error, L"无法查询保存目录的可用空间");
    }
    if (total > free.QuadPart || margin > free.QuadPart - total)
        return Fail(error, L"磁盘空间不足，无法完成多音轨 MP4 封装");
    return true;
}

bool OpenInput(const std::wstring& path, MuxInput& input, MuxDeadline& deadline,
               std::wstring* error) {
    if (!input.packet || !(input.context = avformat_alloc_context()))
        return Fail(error, L"无法打开录制素材：", AVERROR(ENOMEM));
    input.context->interrupt_callback = {MuxDeadline::Interrupt, &deadline};
    Dictionary options;
    av_dict_set(&options.value, "protocol_whitelist", "file", 0);
    // Inputs are our local MP4/M4A files; never auto-detect a playlist or URL.
    const int opened = avformat_open_input(&input.context, WideToUtf8(path).c_str(),
                                           av_find_input_format("mov"), &options.value);
    if (opened < 0) return Fail(error, L"无法打开录制素材：", opened);
    const int probed = avformat_find_stream_info(input.context, nullptr);
    if (probed < 0) return Fail(error, L"无法读取录制素材：", probed);
    input.streamMap.assign(input.context->nb_streams, -1);
    input.packetCounts.assign(input.context->nb_streams, 0);
    return true;
}

bool AddStream(MuxInput& input, AVFormatContext* output, AVMediaType type,
               int audioTrack, MuxDeadline& deadline, std::wstring* error) {
    AVStream* source = nullptr;
    for (unsigned i = 0; i < input.context->nb_streams; ++i) {
        if (input.context->streams[i]->codecpar->codec_type == type) {
            source = input.context->streams[i];
            break;
        }
    }
    if (!source) return Fail(error, L"缺少所需媒体流：" +
        std::wstring(type == AVMEDIA_TYPE_VIDEO ? L"H.264" : L"AAC"));
    const AVCodecParameters* codec = source->codecpar;
    const bool valid = source->time_base.num > 0 && source->time_base.den > 0 &&
        codec->extradata_size > 0 &&
        (type == AVMEDIA_TYPE_VIDEO
            ? codec->codec_id == AV_CODEC_ID_H264 && codec->width > 0 && codec->height > 0
            : codec->codec_id == AV_CODEC_ID_AAC &&
              codec->sample_rate == static_cast<int>(kAudioOutputRate) &&
              codec->ch_layout.nb_channels == static_cast<int>(kAudioOutputChannels));
    if (!valid) return Fail(error, L"录制素材的媒体格式无效");
    const std::int64_t fileSize = avio_size(input.context->pb);
    const int entries = avformat_index_get_entries_count(source);
    if (fileSize <= 0 || entries <= 0)
        return Fail(error, L"录制素材的媒体格式无效");
    // MOV can report clean EOF at a missing sample boundary. Check its sample
    // table against the opened local file before accepting a shortened recording.
    for (int i = 0; i < entries; ++i) {
        if (i % kIndexScanInterruptInterval == 0 && deadline.Interrupted())
            return Fail(error, L"多音轨 MP4 封装已取消或超时");
        const AVIndexEntry* entry = avformat_index_get_entry(source, i);
        if (!entry || entry->pos < 0 || entry->size <= 0 || entry->pos > fileSize ||
            entry->size > fileSize - entry->pos)
            return Fail(error, L"录制素材包含无效时间戳或损坏的数据");
    }
    AVStream* target = avformat_new_stream(output, nullptr);
    if (!target) return Fail(error, L"无法创建视频封装器：", AVERROR(ENOMEM));
    const int copied = avcodec_parameters_copy(target->codecpar, codec);
    if (copied < 0) return Fail(error, L"无法创建视频封装器：", copied);
    target->codecpar->codec_tag = 0;
    target->time_base = source->time_base;
    target->avg_frame_rate = source->avg_frame_rate;
    target->sample_aspect_ratio = source->sample_aspect_ratio;
    if (audioTrack >= 0) {
        av_dict_set(&target->metadata, "title", kTrackNames[audioTrack], 0);
        av_dict_set(&target->metadata, "handler_name", kTrackNames[audioTrack], 0);
        target->disposition = audioTrack == 0 ? AV_DISPOSITION_DEFAULT : 0;
    }
    input.streamMap[source->index] = target->index;
    return true;
}

bool ReadNext(MuxInput& input, MuxDeadline& deadline, std::wstring* error) {
    while (!input.hasPacket && !input.exhausted) {
        if (deadline.Interrupted()) return Fail(error, L"多音轨 MP4 封装已取消或超时");
        const int read = av_read_frame(input.context, input.packet);
        if (read == AVERROR_EOF) {
            if (input.context->pb && input.context->pb->error < 0 &&
                input.context->pb->error != AVERROR_EOF)
                return Fail(error, L"无法读取录制素材：", input.context->pb->error);
            for (std::size_t stream = 0; stream < input.streamMap.size(); ++stream) {
                if (input.streamMap[stream] >= 0 && input.packetCounts[stream] == 0)
                    return Fail(error, L"缺少所需媒体流：" + std::wstring(L"empty track"));
            }
            input.exhausted = true;
            break;
        }
        if (read < 0) return Fail(error, L"无法读取录制素材：", read);
        const auto stream = static_cast<std::size_t>(input.packet->stream_index);
        if (stream < input.streamMap.size() && input.streamMap[stream] >= 0) {
            if (input.packet->dts == AV_NOPTS_VALUE ||
                (input.packet->flags & AV_PKT_FLAG_CORRUPT))
                return Fail(error, L"录制素材包含无效时间戳或损坏的数据");
            input.hasPacket = true;
            ++input.packetCounts[stream];
        } else {
            av_packet_unref(input.packet);
        }
    }
    return true;
}
}  // namespace

bool CheckMp4MuxerAvailable(std::wstring* error) {
    if (av_guess_format("mp4", nullptr, nullptr) && av_find_input_format("mov")) return true;
    return Fail(error, L"音视频组件不完整，请重新解压完整程序包后再试");
}

bool MuxMp4WithAudioTracks(const std::wstring& sourceMp4,
                           const std::wstring& systemAudioM4a,
                           const std::wstring& microphoneM4a,
                           const std::wstring& outputMp4,
                           std::wstring* error, const Mp4MuxOptions& options) {
    if (error) error->clear();
    MuxDeadline deadline{options};
    if (deadline.Interrupted()) return Fail(error, L"多音轨 MP4 封装已取消或超时");
    std::error_code pathError;
    const auto output = std::filesystem::absolute(outputMp4, pathError);
    if (pathError) return Fail(error, L"无法保存最终视频：" + outputMp4);
    if (::GetFileAttributesW(output.c_str()) != INVALID_FILE_ATTRIBUTES)
        return Fail(error, L"输出文件已存在，请选择其他文件名");
    const std::array<std::wstring, 3> sources{sourceMp4, systemAudioM4a, microphoneM4a};
    if (!CheckMp4MuxerAvailable(error) || !CheckSpace(sources, output.wstring(), error))
        return false;
    MuxWorkingFiles working;
    if (!CreateWorkingFiles(output.wstring(), working, error)) return false;
    std::array<MuxInput, 3> inputs;
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        if (!OpenInput(sources[i], inputs[i], deadline, error)) return false;
    }
    MuxOutput result;
    const std::string temporary = WideToUtf8(working.media);
    const int allocated = avformat_alloc_output_context2(&result.context, nullptr,
                                                          "mp4", temporary.c_str());
    if (allocated < 0 || !result.context)
        return Fail(error, L"无法创建视频封装器：", allocated < 0 ? allocated : AVERROR(ENOMEM));
    auto* outputContext = result.context;
    outputContext->interrupt_callback = {MuxDeadline::Interrupt, &deadline};
    outputContext->max_interleave_delta = kMaxInterleaveMicroseconds;
    // All four tracks share the recorder's origin. Preserve AAC priming and B-frame DTS.
    outputContext->avoid_negative_ts = AVFMT_AVOID_NEG_TS_DISABLED;
    if (!AddStream(inputs[0], outputContext, AVMEDIA_TYPE_VIDEO, -1, deadline, error) ||
        !AddStream(inputs[0], outputContext, AVMEDIA_TYPE_AUDIO, 0, deadline, error) ||
        !AddStream(inputs[1], outputContext, AVMEDIA_TYPE_AUDIO, 1, deadline, error) ||
        !AddStream(inputs[2], outputContext, AVMEDIA_TYPE_AUDIO, 2, deadline, error)) return false;
    Dictionary ioOptions;
    av_dict_set(&ioOptions.value, "protocol_whitelist", "file", 0);
    const int opened = avio_open2(&outputContext->pb, temporary.c_str(), AVIO_FLAG_WRITE,
                                  &outputContext->interrupt_callback, &ioOptions.value);
    if (opened < 0) return Fail(error, L"无法保存最终视频：", opened);
    Dictionary muxOptions;
    av_dict_set(&muxOptions.value, "movflags", "+faststart", 0);
    const int header = avformat_write_header(outputContext, &muxOptions.value);
    if (header < 0) return Fail(error, L"无法写入视频文件头：", header);
    std::vector<std::int64_t> lastOutputDts(outputContext->nb_streams, AV_NOPTS_VALUE);
    while (true) {
        MuxInput* next = nullptr;
        for (auto& input : inputs) {
            if (!ReadNext(input, deadline, error)) return false;
            if (!input.hasPacket) continue;
            if (!next || av_compare_ts(input.packet->dts,
                    input.context->streams[input.packet->stream_index]->time_base,
                    next->packet->dts,
                    next->context->streams[next->packet->stream_index]->time_base) < 0)
                next = &input;
        }
        if (!next) break;
        auto* packet = next->packet;
        auto* source = next->context->streams[packet->stream_index];
        const int mapped = next->streamMap[packet->stream_index];
        // Header writing can change the output time base.
        av_packet_rescale_ts(packet, source->time_base, outputContext->streams[mapped]->time_base);
        auto& lastDts = lastOutputDts[static_cast<std::size_t>(mapped)];
        // Native MF files may contain repeated decode timestamps. The MP4
        // muxer requires strict DTS ordering; retain the composition offset
        // while moving only conflicting packets by the minimum output ticks.
        if (lastDts != AV_NOPTS_VALUE && packet->dts <= lastDts) {
            if (lastDts == std::numeric_limits<std::int64_t>::max())
                return Fail(error, L"录制素材包含无效时间戳或损坏的数据");
            const std::int64_t nextDts = lastDts + kOneOutputTimestampTick;
            if (packet->dts < 0 && nextDts >
                std::numeric_limits<std::int64_t>::max() + packet->dts)
                return Fail(error, L"录制素材包含无效时间戳或损坏的数据");
            const std::int64_t correction = nextDts - packet->dts;
            if (packet->pts != AV_NOPTS_VALUE && packet->pts >
                std::numeric_limits<std::int64_t>::max() - correction)
                return Fail(error, L"录制素材包含无效时间戳或损坏的数据");
            packet->dts = nextDts;
            if (packet->pts != AV_NOPTS_VALUE) packet->pts += correction;
        }
        lastDts = packet->dts;
        packet->stream_index = mapped;
        packet->pos = -1;
        const auto packetDts = packet->dts;
        const auto packetPts = packet->pts;
        const auto packetTimeBase = outputContext->streams[mapped]->time_base;
        const int written = av_interleaved_write_frame(outputContext, packet);
        next->hasPacket = false; // write_frame consumes the packet reference, including on error.
        if (written < 0) {
            const std::wstring packetInfo = L"(stream=" + std::to_wstring(mapped) +
                L", dts=" + std::to_wstring(packetDts) + L", pts=" +
                std::to_wstring(packetPts) + L", time_base=" +
                std::to_wstring(packetTimeBase.num) + L"/" +
                std::to_wstring(packetTimeBase.den) + L"): ";
            return Fail(error, L"无法写入视频数据：" + packetInfo, written);
        }
    }
    if (deadline.Interrupted()) return Fail(error, L"多音轨 MP4 封装已取消或超时");
    const int trailer = av_write_trailer(outputContext);
    if (trailer < 0) return Fail(error, L"视频收尾失败：", trailer);
    avio_flush(outputContext->pb);
    const int ioError = outputContext->pb->error;
    const int closed = avio_closep(&outputContext->pb);
    if (ioError < 0 || closed < 0)
        return Fail(error, L"视频收尾失败：", ioError < 0 ? ioError : closed);
    if (deadline.Interrupted()) return Fail(error, L"多音轨 MP4 封装已取消或超时");
    if (!::MoveFileExW(working.media.c_str(), output.c_str(), MOVEFILE_WRITE_THROUGH))
        return Fail(error, L"无法保存最终视频：" +
            FormatHResultMessage(HRESULT_FROM_WIN32(::GetLastError())));
    return true;
}
}  // namespace srl
