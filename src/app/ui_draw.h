#pragma once

#include <windows.h>

#include <cstdint>
#include <string_view>

namespace srl::ui {

enum class ButtonStyle {
    Primary,
    Secondary,
    Danger,
};

enum class ControlIcon { SystemAudio, Microphone, Screenshot, Stop, Cancel };

void DrawIconButton(const DRAWITEMSTRUCT& item, ControlIcon icon, bool active,
                    bool hovered, float microphoneLevel, float animationPhase, UINT dpi);
void DrawLayeredPanel(HDC dc, std::uint32_t* pixels, int width, int height,
                      const RECT& rect, std::wstring_view text, HFONT font, bool warning,
                      UINT dpi);
void DrawLayeredButton(HDC dc, std::uint32_t* pixels, int width, int height,
                       const RECT& rect, std::wstring_view text, HFONT font,
                       ButtonStyle style, UINT dpi, bool disabled);

void DrawModernButton(const DRAWITEMSTRUCT& item, ButtonStyle style, UINT dpi);
void DrawRoundedPanel(HDC dc, const RECT& rect, COLORREF fill, COLORREF border,
                      int radiusPx);

}  // namespace srl::ui
