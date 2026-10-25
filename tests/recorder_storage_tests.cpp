#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>

#include "media/mp4_muxer.h"

// These implementation seams exercise real file semantics without starting
// screen/audio capture or adding test-only options to RecordingSession.
namespace srl::detail {
bool CreateRecordingOutputFile(const std::filesystem::path& directory,
                               const std::wstring& base, std::wstring& finalPath,
                               std::wstring& partialPath, IMFByteStream** stream,
                               std::wstring* error);
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
    if (FAILED(com) || FAILED(::MFStartup(MF_VERSION, MFSTARTUP_FULL))) {
        std::puts("FAIL: COM/Media Foundation initialization");
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

        ComPtr<IMFByteStream> stream;
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
              "mux detects a filename collision before opening inputs or launching FFmpeg");
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
            stream->Close();
            stream.Reset();
        }
        Check(Read(partialPath) == std::string(1, '*'), "reserved stream retains its own data");

        ComPtr<IMFByteStream> second;
        std::wstring secondFinal;
        std::wstring secondPartial;
        Check(srl::detail::CreateRecordingOutputFile(directory, base, secondFinal,
                  secondPartial, second.GetAddressOf(), &error),
              "a second session selects another unoccupied file");
        Check(secondPartial != partialPath, "a second session never reuses an existing partial");
        if (second != nullptr) second->Close();
        second.Reset();

        ComPtr<IMFByteStream> failed;
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
    ::MFShutdown();
    ::CoUninitialize();
    std::printf("%d checks, %d failure(s)\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
