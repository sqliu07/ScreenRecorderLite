#pragma once

#include <string>
#include <string_view>

namespace srl::ui {

void InitializeLanguage(int preference = 0);
void SetLanguagePreference(int preference);
[[nodiscard]] int LanguagePreference() noexcept;
[[nodiscard]] bool IsEnglishLanguage() noexcept;
[[nodiscard]] std::wstring Text(std::wstring_view source);

}  // namespace srl::ui
