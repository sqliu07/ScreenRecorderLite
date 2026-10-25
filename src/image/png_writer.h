#pragma once

#include <cstdint>
#include <string>

namespace srl {

// Saves one tightly packed BGRA image as PNG. The file is written to a
// temporary name first and only then renamed, so a failed encode never leaves
// a half-written screenshot behind.
[[nodiscard]] bool SaveBgraPng(const std::wstring& path, const std::uint8_t* bgra, int width,
                               int height, int stride, std::wstring* error);

}  // namespace srl
