#pragma once

#include <cstddef>
#include <format>
#include <string>
#include <string_view>

namespace srl::ui {

enum class TextId : std::size_t {
#define SRL_UI_TEXT(id, chinese, english) id,
#include "app/ui_text_resources.inc"
#undef SRL_UI_TEXT
    Count,
};

void InitializeLanguage(int preference = 0);
void SetLanguagePreference(int preference);
[[nodiscard]] int LanguagePreference() noexcept;
[[nodiscard]] bool IsEnglishLanguage() noexcept;
[[nodiscard]] std::wstring_view View(TextId id) noexcept;
[[nodiscard]] std::wstring Text(TextId id);

template <typename... Args>
[[nodiscard]] std::wstring Format(TextId id, Args&&... args) {
    return std::vformat(View(id), std::make_wformat_args(args...));
}

// Presentation adapter for messages supplied by recorder/OS/device APIs.
// Fixed UI labels must use TextId; arbitrary English input is never reverse-guessed.
[[nodiscard]] std::wstring LocalizeMessage(std::wstring_view source);

}  // namespace srl::ui
