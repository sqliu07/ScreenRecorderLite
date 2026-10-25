#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace srl {

// Error carrying the failing HRESULT plus a human-readable context.
class HResultError : public std::runtime_error {
public:
    HResultError(HRESULT hr, std::string_view context);
    [[nodiscard]] HRESULT code() const noexcept { return hr_; }

private:
    HRESULT hr_;
};

[[nodiscard]] std::wstring FormatHResultMessage(HRESULT hr);
[[nodiscard]] std::string HResultText(HRESULT hr);
[[noreturn]] void ThrowHResult(HRESULT hr, std::string_view context);
inline void CheckHResult(HRESULT hr, std::string_view context) {
    if (FAILED(hr)) {
        ThrowHResult(hr, context);
    }
}

[[nodiscard]] std::string WideToUtf8(std::wstring_view value);
[[nodiscard]] std::wstring Utf8ToWide(std::string_view value);

[[nodiscard]] std::wstring KnownFolderPath(const GUID& knownFolderId);
[[nodiscard]] std::wstring GetTempDirectory();
[[nodiscard]] std::wstring CurrentTimestampForFileName();
[[nodiscard]] bool EnsureDirectory(const std::filesystem::path& path, std::string* error);
[[nodiscard]] std::string FormatBytes(std::uint64_t bytes);

void ConsoleWrite(std::string_view text);
void ConsoleLine(std::string_view text = {});
void ConsoleLine(std::wstring_view text);

}  // namespace srl
