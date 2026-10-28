#include <windows.h>
#include <objidl.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

#include "media/mp4_muxer.h"
#include "core/output_format.h"

// These implementation seams exercise real file semantics without starting
// screen/audio capture or adding test-only options to RecordingSession.
namespace srl::detail {
bool CreateRecordingOutputFile(const std::filesystem::path& directory,
                               const std::wstring& base, std::wstring& finalPath,
                               std::wstring& partialPath, IStream** stream,
                               std::wstring* error,
                               RecordingFormat format = RecordingFormat::Mp4);
bool CommitRecordingOutputFile(const std::wstring& partialPath,
                                const std::wstring& finalPath, std::wstring* error);
void CleanupRecordingAudioFile(const std::wstring& ownedPath, bool preserve,
                                std::vector<std::wstring>& recoveryPaths);
bool CheckRecordingStorage(const std::wstring& directory, std::uint64_t minimumFree,
                            std::wstring* error);
}

namespace {
int failures = 0;
int checks = 0;
void Check(bool condition, const char* description) {
    ++checks;
    if (!condition) {
        ++failures;
        std::printf("FAIL: %s\n", description);
    }
}

std::string Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), {});
}
}

int main() {
    using Microsoft::WRL::ComPtr;
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(com)) {
        std::puts("FAIL: COM initialization");
        if (SUCCEEDED(com)) ::CoUninitialize();
        return 1;
    }
    const std::filesystem::path directory = std::filesystem::temp_directory_path() /
        (L"srl_recorder_storage_" + std::to_wstring(::GetCurrentProcessId()) + L"_" +
         std::to_wstring(::GetTickCount64()));
    std::filesystem::create_directory(directory);
    {
        const std::wstring base = L"Recording_collision";
        const auto occupiedFinal = directory / (base + L".mp4");
        const auto occupiedPartial = directory / (base + L"_1_partial.mp4");
        std::ofstream(occupiedFinal, std::ios::binary) << "existing final";
        std::ofstream(occupiedPartial, std::ios::binary) << "existing partial";

        ComPtr<IStream> stream;
        std::wstring finalPath;
        std::wstring partialPath;
        std::wstring error;
        Check(srl::detail::CreateRecordingOutputFile(directory, base, finalPath,
                  partialPath, stream.GetAddressOf(), &error),
              "create a unique file despite final/partial collisions");
        Check(std::filesystem::path(finalPath).filename() == base + L"_2.mp4",
              "collisions advance the same final/partial filename pair");
        Check(Read(occupiedFinal) == "existing final", "existing final is untouched");
        error.clear();
        Check(!srl::MuxMp4WithAudioTracks(L"missing-video.mp4", L"missing-system.m4a",
                  L"missing-microphone.m4a", occupiedFinal.wstring(), &error),
              "mux refuses to replace an existing final file");
        Check(error.find(L"已存在") != std::wstring::npos,
              "mux detects a filename collision before opening any inputs");
        Check(Read(occupiedFinal) == "existing final", "failed mux preserves existing final contents");
        Check(Read(occupiedPartial) == "existing partial", "existing partial is untouched");
        Check(!std::filesystem::exists(finalPath), "final is unpublished until commit");

        if (stream != nullptr) {
            BYTE marker = 42;
            ULONG written = 0;
            Check(SUCCEEDED(stream->Write(&marker, 1, &written)) && written == 1,
                  "the reserved byte stream is writable");
            HANDLE other = ::CreateFileW(partialPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                         CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            Check(other == INVALID_HANDLE_VALUE, "another writer cannot truncate the active partial");
            if (other != INVALID_HANDLE_VALUE) ::CloseHandle(other);
            stream->Commit(STGC_DEFAULT);
            stream.Reset();
        }
        Check(Read(partialPath) == std::string(1, '*'), "reserved stream retains its own data");

        ComPtr<IStream> second;
        std::wstring secondFinal;
        std::wstring secondPartial;
        Check(srl::detail::CreateRecordingOutputFile(directory, base, secondFinal,
                  secondPartial, second.GetAddressOf(), &error),
              "a second session selects another unoccupied file");
        Check(secondPartial != partialPath, "a second session never reuses an existing partial");
        if (second != nullptr) second->Commit(STGC_DEFAULT);
        second.Reset();

        const std::wstring gifBase = L"动画 录制";
        const auto occupiedGif = directory / (gifBase + L".gif");
        const auto occupiedGifPartial = directory / (gifBase + L"_1_partial.gif");
        std::ofstream(occupiedGif, std::ios::binary) << "existing GIF";
        std::ofstream(occupiedGifPartial, std::ios::binary) << "existing GIF partial";
        ComPtr<IStream> gifStream;
        std::wstring gifFinal;
        std::wstring gifPartial;
        Check(srl::detail::CreateRecordingOutputFile(directory, gifBase, gifFinal,
                  gifPartial, gifStream.GetAddressOf(), &error, srl::RecordingFormat::Gif),
              "GIF reserves an exclusive Unicode file without Media Foundation startup");
        Check(std::filesystem::path(gifFinal).filename() == gifBase + L"_2.gif" &&
                  std::filesystem::path(gifPartial).filename() == gifBase + L"_2_partial.gif",
              "GIF collision handling advances matching final and partial names");
        Check(Read(occupiedGif) == "existing GIF" &&
                  Read(occupiedGifPartial) == "existing GIF partial",
              "GIF reservation preserves existing final and partial files");
        Check(!std::filesystem::exists(gifFinal), "GIF final path remains unpublished while recording");
        if (gifStream != nullptr) {
            const BYTE gifMarker = 17;
            ULONG gifWritten = 0;
            Check(SUCCEEDED(gifStream->Write(&gifMarker, 1, &gifWritten)) && gifWritten == 1,
                  "GIF reserved stream is writable");
            HANDLE competitor = ::CreateFileW(gifPartial.c_str(), GENERIC_WRITE, 0, nullptr,
                                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            Check(competitor == INVALID_HANDLE_VALUE,
                  "another writer cannot truncate an active GIF partial");
            if (competitor != INVALID_HANDLE_VALUE) ::CloseHandle(competitor);
            gifStream->Commit(STGC_DEFAULT);
            gifStream.Reset();
        }
        Check(Read(gifPartial) == std::string(1, '\x11'), "GIF stream retains its reserved content");

        std::ofstream(gifFinal, std::ios::binary) << "competing final GIF";
        error.clear();
        Check(!srl::detail::CommitRecordingOutputFile(gifPartial, gifFinal, &error) && !error.empty(),
              "GIF publish refuses a destination created while recording");
        Check(Read(gifFinal) == "competing final GIF" &&
                  Read(gifPartial) == std::string(1, '\x11'),
              "failed GIF publish preserves the competing final and the recoverable partial");
        std::filesystem::remove(gifFinal);
        Check(srl::detail::CommitRecordingOutputFile(gifPartial, gifFinal, &error),
              "GIF publishes atomically after encoding releases its stream");
        Check(!std::filesystem::exists(gifPartial) &&
                  Read(gifFinal) == std::string(1, '\x11'),
              "successful GIF publish moves the complete reserved file");

        const auto retainedSystem = directory / L"failed_system_partial.m4a";
        const auto retainedMicrophone = directory / L"failed_microphone_partial.m4a";
        const auto unrelatedAudio = directory / L"unrelated.m4a";
        std::ofstream(retainedSystem, std::ios::binary) << "system packets";
        std::ofstream(retainedMicrophone, std::ios::binary) << "microphone packets";
        std::ofstream(unrelatedAudio, std::ios::binary) << "unrelated recording";
        std::vector<std::wstring> recoveryPaths;
        srl::detail::CleanupRecordingAudioFile(retainedSystem.wstring(), true, recoveryPaths);
        srl::detail::CleanupRecordingAudioFile(retainedMicrophone.wstring(), true, recoveryPaths);
        Check(Read(retainedSystem) == "system packets" &&
                  Read(retainedMicrophone) == "microphone packets",
              "failed save preserves both owned source tracks byte-for-byte");
        Check(recoveryPaths == std::vector<std::wstring>{retainedSystem.wstring(),
                                                        retainedMicrophone.wstring()},
              "failed save reports the source files available for recovery");
        srl::detail::CleanupRecordingAudioFile(L"", true, recoveryPaths);
        srl::detail::CleanupRecordingAudioFile((directory / L"missing.m4a").wstring(),
                                              true, recoveryPaths);
        Check(recoveryPaths.size() == 2, "only existing source files are reported for recovery");
        recoveryPaths.clear();
        srl::detail::CleanupRecordingAudioFile(retainedSystem.wstring(), false, recoveryPaths);
        srl::detail::CleanupRecordingAudioFile(retainedMicrophone.wstring(), false, recoveryPaths);
        Check(!std::filesystem::exists(retainedSystem) &&
                  !std::filesystem::exists(retainedMicrophone) && recoveryPaths.empty(),
              "successful save removes owned source tracks without recovery warnings");
        Check(Read(unrelatedAudio) == "unrelated recording",
              "source cleanup never removes another recording");
        const auto emptyTrack = directory / L"unused_partial.m4a";
        std::ofstream(emptyTrack, std::ios::binary);
        srl::detail::CleanupRecordingAudioFile(emptyTrack.wstring(), false, recoveryPaths);
        Check(!std::filesystem::exists(emptyTrack), "a track with no recorded samples is removed");

        ComPtr<IStream> failed;
        std::wstring failedFinal;
        std::wstring failedPartial;
        error.clear();
        Check(!srl::detail::CreateRecordingOutputFile(directory / L"missing", base,
                  failedFinal, failedPartial, failed.GetAddressOf(), &error) &&
                  !error.empty() && failed == nullptr,
              "missing output directory fails with a reason and no stream");
        Check(srl::detail::CheckRecordingStorage(directory.wstring(), 0, &error),
              "available local directory passes a zero minimum");
        error.clear();
        Check(!srl::detail::CheckRecordingStorage(directory.wstring(),
                  (std::numeric_limits<std::uint64_t>::max)(), &error) && !error.empty(),
              "insufficient free space fails with an explanation");
        error.clear();
        Check(!srl::detail::CheckRecordingStorage((directory / L"missing").wstring(),
                  0, &error) && !error.empty(),
              "removed/unavailable directory fails with an explanation");
    }
    std::filesystem::remove_all(directory);
    ::CoUninitialize();
    std::printf("%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
