#include "common/win32.h"

#include <shlobj.h>

#include <algorithm>
#include <cstdio>
#include <format>
#include <vector>

namespace srl {

HResultError::HResultError(HRESULT hr, std::string_view context)
    : std::runtime_error(std::string(context) + " failed: " + HResultText(hr)), hr_(hr) {}

std::wstring FormatHResultMessage(HRESULT hr) {
    LPWSTR buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        static_cast<DWORD>(hr),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);

    std::wstring message;
    if (length != 0 && buffer != nullptr) {
        message.assign(buffer, length);
        while (!message.empty() &&
               (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ' ||
                message.back() == L'.')) {
            message.pop_back();
        }
    }
    if (buffer != nullptr) {
        ::LocalFree(buffer);
    }
    if (message.empty()) {
        message = L"Unknown error";
    }
    return message;
}

std::string HResultText(HRESULT hr) {
    return std::format("0x{:08X} ({})", static_cast<std::uint32_t>(hr),
                       WideToUtf8(FormatHResultMessage(hr)));
}

void ThrowHResult(HRESULT hr, std::string_view context) {
    throw HResultError(hr, context);
}

std::string WideToUtf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int required = ::WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                               static_cast<int>(value.size()), nullptr, 0,
                                               nullptr, nullptr);
    if (required <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                          result.data(), required, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(std::string_view value) {
    if (value.empty()) {
        return {};
    }
    const int required = ::MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                               static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                          result.data(), required);
    return result;
}

std::wstring KnownFolderPath(const GUID& knownFolderId) {
    PWSTR rawPath = nullptr;
    const HRESULT hr = ::SHGetKnownFolderPath(knownFolderId, KF_FLAG_DEFAULT, nullptr, &rawPath);
    CheckHResult(hr, "SHGetKnownFolderPath");
    std::wstring path(rawPath);
    ::CoTaskMemFree(rawPath);
    return path;
}

std::wstring GetTempDirectory() {
    const DWORD required = ::GetTempPathW(0, nullptr);
    if (required == 0) {
        CheckHResult(::HRESULT_FROM_WIN32(::GetLastError()), "GetTempPathW");
    }
    std::vector<wchar_t> buffer(required + 1, L'\0');
    const DWORD written = ::GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
    if (written == 0) {
        CheckHResult(::HRESULT_FROM_WIN32(::GetLastError()), "GetTempPathW");
    }
    return std::wstring(buffer.data(), written);
}

std::wstring CurrentTimestampForFileName() {
    SYSTEMTIME localTime{};
    ::GetLocalTime(&localTime);
    wchar_t buffer[32]{};
    ::swprintf_s(buffer, L"%04u%02u%02u_%02u%02u%02u_%03u", localTime.wYear, localTime.wMonth,
                 localTime.wDay, localTime.wHour, localTime.wMinute, localTime.wSecond,
                 localTime.wMilliseconds);
    return buffer;
}

bool EnsureDirectory(const std::filesystem::path& path, std::string* error) {
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        if (!std::filesystem::is_directory(path, ec)) {
            if (error != nullptr) {
                *error = "path exists but is not a directory: " +
                         WideToUtf8(path.wstring());
            }
            return false;
        }
        return true;
    }
    std::filesystem::create_directories(path, ec);
    if (ec) {
        if (error != nullptr) {
            *error = "cannot create directory '" + WideToUtf8(path.wstring()) +
                     "': " + ec.message();
        }
        return false;
    }
    return true;
}

std::string FormatBytes(std::uint64_t bytes) {
    constexpr double kKiB = 1024.0;
    constexpr double kMiB = kKiB * 1024.0;
    constexpr double kGiB = kMiB * 1024.0;
    const double value = static_cast<double>(bytes);
    if (value >= kGiB) {
        return std::format("{:.2f} GiB", value / kGiB);
    }
    if (value >= kMiB) {
        return std::format("{:.2f} MiB", value / kMiB);
    }
    if (value >= kKiB) {
        return std::format("{:.2f} KiB", value / kKiB);
    }
    return std::format("{} B", bytes);
}

void ConsoleWrite(std::string_view text) {
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
}

void ConsoleLine(std::string_view text) {
    ConsoleWrite(text);
    ConsoleWrite("\n");
}

void ConsoleLine(std::wstring_view text) {
    ConsoleLine(WideToUtf8(text));
}

}  // namespace srl
