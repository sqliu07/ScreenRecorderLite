#include "media/mp4_muxer.h"

#include <windows.h>

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "common/win32.h"

namespace srl {
namespace {

std::wstring QuoteArgument(std::wstring_view argument) {
    std::wstring quoted(1, L'"');
    std::size_t backslashes = 0;
    for (const wchar_t value : argument) {
        if (value == L'\\') {
            ++backslashes;
        } else if (value == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
            backslashes = 0;
        } else {
            quoted.append(backslashes, L'\\');
            quoted.push_back(value);
            backslashes = 0;
        }
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

std::wstring ReadLog(HANDLE file) {
    LARGE_INTEGER start{};
    ::SetFilePointerEx(file, start, nullptr, FILE_BEGIN);
    std::string bytes;
    std::array<char, 2048> buffer{};
    DWORD read = 0;
    while (::ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()),
                      &read, nullptr) && read != 0) {
        bytes.append(buffer.data(), read);
    }
    if (bytes.empty()) return {};
    return Utf8ToWide(bytes);
}

std::wstring FfmpegPath() {
    std::wstring executable(32768, L'\0');
    const DWORD size = ::GetModuleFileNameW(nullptr, executable.data(),
                                            static_cast<DWORD>(executable.size()));
    if (size == 0 || size >= executable.size()) return {};
    executable.resize(size);
    const auto separator = executable.find_last_of(L"\\/");
    if (separator == std::wstring::npos) return {};
    executable.resize(separator + 1);
    executable += L"ffmpeg.exe";
    return executable;
}

struct MuxWorkingFiles {
    std::wstring directory;
    std::wstring media;
    std::wstring log;
    ~MuxWorkingFiles() {
        if (!media.empty()) ::DeleteFileW(media.c_str());
        if (!log.empty()) ::DeleteFileW(log.c_str());
        if (!directory.empty()) ::RemoveDirectoryW(directory.c_str());
    }
};

bool CreateWorkingFiles(const std::wstring& output, MuxWorkingFiles& files,
                        std::wstring* error) {
    for (int attempt = 0; attempt < 16; ++attempt) {
        const std::wstring candidate = output + L".mux-" +
            std::to_wstring(::GetCurrentProcessId()) + L"-" +
            std::to_wstring(::GetTickCount64()) + L"-" + std::to_wstring(attempt);
        if (::CreateDirectoryW(candidate.c_str(), nullptr)) {
            files.directory = candidate;
            files.media = candidate + L"\\result.mp4";
            files.log = candidate + L"\\mux.log";
            return true;
        }
        if (::GetLastError() != ERROR_ALREADY_EXISTS) break;
    }
    if (error != nullptr) *error = L"无法创建视频收尾临时目录";
    return false;
}

}  // namespace

bool CheckMp4MuxerAvailable(std::wstring* error) {
    const std::wstring ffmpeg = FfmpegPath();
    const DWORD attributes = ffmpeg.empty() ? INVALID_FILE_ATTRIBUTES
        : ::GetFileAttributesW(ffmpeg.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        if (error != nullptr) *error = L"音视频组件不完整，请重新解压完整程序包后再试";
        return false;
    }
    return true;
}

bool MuxMp4WithAudioTracks(const std::wstring& sourceMp4,
                           const std::wstring& systemAudioM4a,
                           const std::wstring& microphoneM4a,
                           const std::wstring& outputMp4,
                           std::wstring* error) {
    const std::wstring ffmpeg = FfmpegPath();
    if (::GetFileAttributesW(outputMp4.c_str()) != INVALID_FILE_ATTRIBUTES) {
        if (error != nullptr) *error = L"输出文件已存在，请选择其他文件名";
        return false;
    }
    if (!CheckMp4MuxerAvailable(error)) return false;

    MuxWorkingFiles working;
    if (!CreateWorkingFiles(outputMp4, working, error)) return false;
    const std::wstring& logPath = working.log;
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE log = ::CreateFileW(logPath.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ, &security, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (log == INVALID_HANDLE_VALUE) {
        if (error != nullptr) *error = L"无法创建 MP4 收尾日志";
        return false;
    }
    HANDLE nullInput = ::CreateFileW(L"NUL", GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nullInput == INVALID_HANDLE_VALUE) {
        ::CloseHandle(log);
        ::DeleteFileW(logPath.c_str());
        if (error != nullptr) *error = L"无法准备 MP4 收尾任务";
        return false;
    }

    const std::vector<std::wstring> arguments = {
        ffmpeg, L"-hide_banner", L"-nostdin", L"-loglevel", L"error",
        L"-i", sourceMp4, L"-i", systemAudioM4a, L"-i", microphoneM4a,
        L"-map", L"0:v:0", L"-map", L"0:a:0", L"-map", L"1:a:0",
        L"-map", L"2:a:0", L"-c", L"copy",
        L"-metadata:s:a:0", L"title=Mix",
        L"-metadata:s:a:0", L"handler_name=Mix",
        L"-metadata:s:a:1", L"title=System audio",
        L"-metadata:s:a:1", L"handler_name=System audio",
        L"-metadata:s:a:2", L"title=Microphone",
        L"-metadata:s:a:2", L"handler_name=Microphone",
        L"-disposition:a:0", L"default", L"-disposition:a:1", L"0",
        L"-disposition:a:2", L"0", L"-movflags", L"+faststart",
        L"-n", working.media,
    };
    std::wstring commandLine;
    for (const auto& argument : arguments) {
        if (!commandLine.empty()) commandLine.push_back(L' ');
        commandLine += QuoteArgument(argument);
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = nullInput;
    startup.hStdOutput = log;
    startup.hStdError = log;
    PROCESS_INFORMATION process{};
    const BOOL created = ::CreateProcessW(ffmpeg.c_str(), commandLine.data(),
        nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    ::CloseHandle(nullInput);
    if (!created) {
        const DWORD code = ::GetLastError();
        ::CloseHandle(log);
        ::DeleteFileW(logPath.c_str());
        if (error != nullptr) *error = L"无法启动 FFmpeg：" +
            FormatHResultMessage(HRESULT_FROM_WIN32(code));
        return false;
    }

    constexpr DWORD kMuxTimeoutMs = 10 * 60 * 1000;
    const DWORD wait = ::WaitForSingleObject(process.hProcess, kMuxTimeoutMs);
    if (wait == WAIT_TIMEOUT) {
        ::TerminateProcess(process.hProcess, ERROR_TIMEOUT);
        ::WaitForSingleObject(process.hProcess, INFINITE);
    }
    DWORD exitCode = 0;
    ::GetExitCodeProcess(process.hProcess, &exitCode);
    ::CloseHandle(process.hThread);
    ::CloseHandle(process.hProcess);
    const std::wstring logText = ReadLog(log);
    ::CloseHandle(log);
    ::DeleteFileW(logPath.c_str());

    if (wait == WAIT_TIMEOUT) {
        if (error != nullptr) *error = L"MP4 多音轨收尾超过 10 分钟，已停止封装";
        return false;
    }
    if (wait != WAIT_OBJECT_0 || exitCode != 0) {
        if (error != nullptr) {
            *error = L"FFmpeg 无法生成多音轨 MP4";
            if (!logText.empty()) *error += L"：\r\n" + logText;
        }
        return false;
    }
    if (!::MoveFileExW(working.media.c_str(), outputMp4.c_str(), MOVEFILE_WRITE_THROUGH)) {
        if (error != nullptr) *error = L"无法保存最终视频：" +
            FormatHResultMessage(HRESULT_FROM_WIN32(::GetLastError()));
        return false;
    }
    return true;
}

}  // namespace srl
