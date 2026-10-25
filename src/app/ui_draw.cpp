#include "app/ui_draw.h"

#include <algorithm>

#include <windows.h>
// GDI+ declarations use COM stream/property interfaces that windows.h does
// not provide under WIN32_LEAN_AND_MEAN.
#include <objidl.h>
#include <propidl.h>
#include <gdiplus.h>
#include <uxtheme.h>

#include "common/win32.h"

namespace srl::ui {
namespace {

ULONG_PTR g_gdiplusToken = 0;
bool g_gdiplusReady = false;

struct GdiplusRuntime {
    GdiplusRuntime() {
        Gdiplus::GdiplusStartupInput input;
        g_gdiplusReady = Gdiplus::GdiplusStartup(&g_gdiplusToken, &input, nullptr) ==
                         Gdiplus::Ok;
    }
    ~GdiplusRuntime() {
        if (g_gdiplusReady) {
            Gdiplus::GdiplusShutdown(g_gdiplusToken);
        }
    }
};

GdiplusRuntime g_gdiplusRuntime;

void AddRoundedRectPath(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& rect,
                        float radius) {
    const float diameter = std::min(radius * 2.0f, std::min(rect.Width, rect.Height));
    if (radius <= 0.0f) {
        path.AddRectangle(rect);
        return;
    }
    path.AddArc(rect.X, rect.Y, diameter, diameter, 180.0f, 90.0f);
    path.AddArc(rect.X + rect.Width - diameter, rect.Y, diameter, diameter, 270.0f, 90.0f);
    path.AddArc(rect.X + rect.Width - diameter, rect.Y + rect.Height - diameter, diameter,
                diameter, 0.0f, 90.0f);
    path.AddArc(rect.X, rect.Y + rect.Height - diameter, diameter, diameter, 90.0f, 90.0f);
    path.CloseFigure();
}

COLORREF Blend(COLORREF a, COLORREF b, int percentB) {
    const int percentA = 100 - percentB;
    return RGB((GetRValue(a) * percentA + GetRValue(b) * percentB) / 100,
               (GetGValue(a) * percentA + GetGValue(b) * percentB) / 100,
               (GetBValue(a) * percentA + GetBValue(b) * percentB) / 100);
}

bool HighContrast() {
    HIGHCONTRASTW hc{};
    hc.cbSize = sizeof(hc);
    return ::SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) &&
           (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

struct StyleColors {
    COLORREF fill;
    COLORREF border;
    COLORREF text;
};

StyleColors ColorsFor(ButtonStyle style, bool disabled, bool pressed) {
    if (HighContrast()) {
        return StyleColors{
            ::GetSysColor(disabled ? COLOR_3DFACE : COLOR_BTNFACE),
            ::GetSysColor(COLOR_WINDOWFRAME),
            ::GetSysColor(disabled ? COLOR_GRAYTEXT : COLOR_BTNTEXT),
        };
    }
    if (disabled) {
        return StyleColors{RGB(243, 243, 243), RGB(224, 224, 224), RGB(140, 140, 140)};
    }
    switch (style) {
        case ButtonStyle::Primary: {
            const COLORREF base = pressed ? RGB(12, 90, 160) : RGB(15, 108, 189);
            return StyleColors{base, base, RGB(255, 255, 255)};
        }
        case ButtonStyle::Danger: {
            const COLORREF base = pressed ? RGB(164, 38, 26) : RGB(196, 43, 28);
            return StyleColors{base, base, RGB(255, 255, 255)};
        }
        case ButtonStyle::Secondary:
        default:
            return StyleColors{pressed ? RGB(242, 242, 242) : RGB(255, 255, 255),
                               RGB(200, 200, 200), RGB(32, 32, 32)};
    }
}

void DrawButtonWithGdi(const DRAWITEMSTRUCT& item, const StyleColors& colors) {
    HBRUSH brush = ::CreateSolidBrush(colors.fill);
    ::FillRect(item.hDC, &item.rcItem, brush);
    ::DeleteObject(brush);
    HPEN pen = ::CreatePen(PS_SOLID, 1, colors.border);
    HGDIOBJ oldPen = ::SelectObject(item.hDC, pen);
    HGDIOBJ oldBrush = ::SelectObject(item.hDC, ::GetStockObject(HOLLOW_BRUSH));
    ::RoundRect(item.hDC, item.rcItem.left, item.rcItem.top, item.rcItem.right,
                item.rcItem.bottom, 12, 12);
    ::SelectObject(item.hDC, oldBrush);
    ::SelectObject(item.hDC, oldPen);
    ::DeleteObject(pen);

    wchar_t text[128]{};
    ::GetWindowTextW(item.hwndItem, text, ARRAYSIZE(text));
    HGDIOBJ oldFont = nullptr;
    const auto font = reinterpret_cast<HFONT>(::SendMessageW(item.hwndItem, WM_GETFONT, 0, 0));
    if (font != nullptr) {
        oldFont = ::SelectObject(item.hDC, font);
    }
    ::SetBkMode(item.hDC, TRANSPARENT);
    ::SetTextColor(item.hDC, colors.text);
    RECT textRect = item.rcItem;
    ::DrawTextW(item.hDC, text, -1, &textRect,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (oldFont != nullptr) {
        ::SelectObject(item.hDC, oldFont);
    }
}

}  // namespace

void DrawModernButton(const DRAWITEMSTRUCT& item, ButtonStyle style, UINT dpi) {
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const StyleColors colors = ColorsFor(style, disabled, pressed);

    if (!g_gdiplusReady) {
        DrawButtonWithGdi(item, colors);
        return;
    }

    Gdiplus::Graphics graphics(item.hDC);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    const float radius = static_cast<float>(MulDiv(8, static_cast<int>(dpi), 96));
    const Gdiplus::RectF rect(static_cast<float>(item.rcItem.left),
                              static_cast<float>(item.rcItem.top),
                              static_cast<float>(item.rcItem.right - item.rcItem.left),
                              static_cast<float>(item.rcItem.bottom - item.rcItem.top));
    Gdiplus::GraphicsPath path;
    AddRoundedRectPath(path, rect, radius);
    Gdiplus::SolidBrush fill(Gdiplus::Color(255, GetRValue(colors.fill),
                                            GetGValue(colors.fill), GetBValue(colors.fill)));
    graphics.FillPath(&fill, &path);
    Gdiplus::Pen border(Gdiplus::Color(255, GetRValue(colors.border),
                                       GetGValue(colors.border), GetBValue(colors.border)),
                        1.0f);
    graphics.DrawPath(&border, &path);

    if ((item.itemState & ODS_FOCUS) != 0) {
        const float inset = 1.5f;
        Gdiplus::GraphicsPath focusPath;
        AddRoundedRectPath(
            focusPath,
            Gdiplus::RectF(rect.X + inset, rect.Y + inset, rect.Width - inset * 2,
                           rect.Height - inset * 2),
            std::max(0.0f, radius - inset));
        Gdiplus::Pen focus(Gdiplus::Color(255, 255, 255, 255), 1.0f);
        graphics.DrawPath(&focus, &focusPath);
    }

    wchar_t text[128]{};
    ::GetWindowTextW(item.hwndItem, text, ARRAYSIZE(text));
    HGDIOBJ oldFont = nullptr;
    const auto font = reinterpret_cast<HFONT>(::SendMessageW(item.hwndItem, WM_GETFONT, 0, 0));
    if (font != nullptr) {
        oldFont = ::SelectObject(item.hDC, font);
    }
    ::SetBkMode(item.hDC, TRANSPARENT);
    ::SetTextColor(item.hDC, colors.text);
    RECT textRect = item.rcItem;
    ::DrawTextW(item.hDC, text, -1, &textRect,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (oldFont != nullptr) {
        ::SelectObject(item.hDC, oldFont);
    }
}

void DrawIconButton(const DRAWITEMSTRUCT& item, ControlIcon icon, bool active,
                    bool hovered, float microphoneLevel, float animationPhase, UINT dpi) {
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool focused = (item.itemState & ODS_FOCUS) != 0;
    const bool highContrast = HighContrast();
    const bool soundDetected = icon == ControlIcon::Microphone && active &&
                               microphoneLevel >= 0.006f;
    ::FillRect(item.hDC, &item.rcItem, ::GetSysColorBrush(COLOR_WINDOW));
    COLORREF foreground = disabled ? ::GetSysColor(COLOR_GRAYTEXT)
        : highContrast ? ::GetSysColor(COLOR_WINDOWTEXT)
        : icon == ControlIcon::Stop ? RGB(196, 43, 28)
        : soundDetected ? RGB(35, 148, 80)
        : active ? RGB(0, 103, 192) : RGB(105, 112, 122);
    if (!g_gdiplusReady) {
        const wchar_t* glyph = icon == ControlIcon::Stop ? L"■"
            : icon == ControlIcon::Cancel ? L"×"
            : icon == ControlIcon::Screenshot ? L"▣"
            : icon == ControlIcon::Microphone ? L"M" : L"♫";
        const auto font = reinterpret_cast<HFONT>(::SendMessageW(item.hwndItem, WM_GETFONT, 0, 0));
        const auto previous = font != nullptr ? ::SelectObject(item.hDC, font) : nullptr;
        ::SetTextColor(item.hDC, foreground);
        ::SetBkMode(item.hDC, TRANSPARENT);
        RECT text = item.rcItem;
        ::DrawTextW(item.hDC, glyph, -1, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        if (previous != nullptr) ::SelectObject(item.hDC, previous);
        return;
    }
    Gdiplus::Graphics graphics(item.hDC);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    const float scale = static_cast<float>(dpi) / 96.0f;
    const float cx = static_cast<float>(item.rcItem.left + item.rcItem.right) / 2.0f;
    const float cy = static_cast<float>(item.rcItem.top + item.rcItem.bottom) / 2.0f;
    if (!disabled && (hovered || pressed)) {
        const COLORREF color = highContrast ? ::GetSysColor(COLOR_HIGHLIGHT)
            : pressed ? RGB(224, 230, 237) : RGB(238, 242, 247);
        Gdiplus::SolidBrush background(Gdiplus::Color(255, GetRValue(color), GetGValue(color), GetBValue(color)));
        graphics.FillEllipse(&background, cx - 17.0f * scale, cy - 17.0f * scale,
                              34.0f * scale, 34.0f * scale);
        if (highContrast) foreground = ::GetSysColor(COLOR_HIGHLIGHTTEXT);
    }
    if (focused) {
        Gdiplus::Pen focus(Gdiplus::Color(255, 0, 103, 192), scale);
        graphics.DrawEllipse(&focus, cx - 17.0f * scale, cy - 17.0f * scale,
                              34.0f * scale, 34.0f * scale);
    }
    graphics.TranslateTransform(cx - 12.0f * scale, cy - 12.0f * scale);
    graphics.ScaleTransform(scale, scale);
    const Gdiplus::Color color(255, GetRValue(foreground), GetGValue(foreground), GetBValue(foreground));
    Gdiplus::Pen pen(color, 1.8f);
    pen.SetStartCap(Gdiplus::LineCapRound);
    pen.SetEndCap(Gdiplus::LineCapRound);
    pen.SetLineJoin(Gdiplus::LineJoinRound);
    Gdiplus::SolidBrush fill(color);
    switch (icon) {
        case ControlIcon::SystemAudio: {
            const Gdiplus::PointF speaker[] = {{3, 9}, {7, 9}, {12, 5}, {12, 19}, {7, 15}, {3, 15}};
            graphics.DrawPolygon(&pen, speaker, 6);
            if (active) {
                graphics.DrawArc(&pen, 11.0f, 7.0f, 8.0f, 10.0f, -55.0f, 110.0f);
                graphics.DrawArc(&pen, 10.0f, 3.0f, 14.0f, 18.0f, -50.0f, 100.0f);
            }
            break;
        }
        case ControlIcon::Microphone: {
            Gdiplus::GraphicsPath capsule;
            AddRoundedRectPath(capsule, Gdiplus::RectF(9, 2, 6, 13), 3);
            graphics.DrawPath(&pen, &capsule);
            graphics.DrawArc(&pen, 6.0f, 6.0f, 12.0f, 13.0f, 0.0f, 180.0f);
            graphics.DrawLine(&pen, 12.0f, 19.0f, 12.0f, 22.0f);
            graphics.DrawLine(&pen, 8.0f, 22.0f, 16.0f, 22.0f);
            if (soundDetected) {
                const float amplitude = std::min(7.0f, microphoneLevel * 32.0f);
                for (int bar = 0; bar < 3; ++bar) {
                    const float pulse = 0.65f + 0.35f *
                        std::abs(std::sin(animationPhase - static_cast<float>(bar) * 0.7f));
                    const float halfHeight = 1.5f + amplitude * pulse;
                    const float x = 17.0f + static_cast<float>(bar) * 2.0f;
                    graphics.DrawLine(&pen, x, 12.0f - halfHeight,
                                      x, 12.0f + halfHeight);
                }
            }
            break;
        }
        case ControlIcon::Screenshot: {
            const Gdiplus::PointF camera[] = {{3, 7}, {7, 7}, {9, 4}, {15, 4}, {17, 7}, {21, 7}, {21, 20}, {3, 20}};
            graphics.DrawPolygon(&pen, camera, 8);
            graphics.DrawEllipse(&pen, 8.0f, 9.0f, 8.0f, 8.0f);
            break;
        }
        case ControlIcon::Stop: {
            Gdiplus::GraphicsPath stop;
            AddRoundedRectPath(stop, Gdiplus::RectF(6, 6, 12, 12), 2);
            graphics.FillPath(&fill, &stop);
            break;
        }
        case ControlIcon::Cancel:
            graphics.DrawLine(&pen, 6.0f, 6.0f, 18.0f, 18.0f);
            graphics.DrawLine(&pen, 18.0f, 6.0f, 6.0f, 18.0f);
            break;
    }
    if (!active && (icon == ControlIcon::SystemAudio || icon == ControlIcon::Microphone)) {
        Gdiplus::Pen gap(Gdiplus::Color(255, GetRValue(::GetSysColor(COLOR_WINDOW)),
            GetGValue(::GetSysColor(COLOR_WINDOW)), GetBValue(::GetSysColor(COLOR_WINDOW))), 4.5f);
        graphics.DrawLine(&gap, 3.0f, 3.0f, 21.0f, 21.0f);
        graphics.DrawLine(&pen, 3.0f, 3.0f, 21.0f, 21.0f);
    }
}

void DrawLayeredButton(HDC dc, std::uint32_t* pixels, int width, int height,
                       const RECT& rect, std::wstring_view text, HFONT font,
                       ButtonStyle style, UINT dpi, bool disabled) {
    const StyleColors colors = ColorsFor(style, disabled, false);
    const int left = std::clamp(static_cast<int>(rect.left), 0, width);
    const int right = std::clamp(static_cast<int>(rect.right), 0, width);
    const int top = std::clamp(static_cast<int>(rect.top), 0, height);
    const int bottom = std::clamp(static_cast<int>(rect.bottom), 0, height);
    for (int y = top; y < bottom; ++y) {
        std::fill(pixels + static_cast<std::size_t>(y) * width + left,
                  pixels + static_cast<std::size_t>(y) * width + right, 0x78000000u);
    }

    if (g_gdiplusReady) {
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        const float scale = static_cast<float>(dpi) / 96.0f;
        const float radius = 7.0f * scale;
        const Gdiplus::RectF shape(static_cast<float>(left), static_cast<float>(top),
            static_cast<float>(right - left), static_cast<float>(bottom - top));
        Gdiplus::GraphicsPath path;
        AddRoundedRectPath(path, shape, radius);
        Gdiplus::SolidBrush fill(Gdiplus::Color(255, GetRValue(colors.fill),
            GetGValue(colors.fill), GetBValue(colors.fill)));
        graphics.FillPath(&fill, &path);
        Gdiplus::Pen border(Gdiplus::Color(255, GetRValue(colors.border),
            GetGValue(colors.border), GetBValue(colors.border)), scale);
        graphics.DrawPath(&border, &path);
    } else {
        HBRUSH fill = ::CreateSolidBrush(colors.fill);
        HPEN border = ::CreatePen(PS_SOLID, 1, colors.border);
        HGDIOBJ oldBrush = ::SelectObject(dc, fill);
        HGDIOBJ oldPen = ::SelectObject(dc, border);
        const int radius = MulDiv(14, static_cast<int>(dpi), 96);
        ::RoundRect(dc, left, top, right, bottom, radius, radius);
        ::SelectObject(dc, oldPen);
        ::SelectObject(dc, oldBrush);
        ::DeleteObject(border);
        ::DeleteObject(fill);
    }

    HGDIOBJ previousFont = font != nullptr ? ::SelectObject(dc, font) : nullptr;
    ::SetBkMode(dc, TRANSPARENT);
    ::SetTextColor(dc, colors.text);
    RECT textRect{left + 4, top, right - 4, bottom};
    ::DrawTextW(dc, text.data(), static_cast<int>(text.size()), &textRect,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (previousFont != nullptr) ::SelectObject(dc, previousFont);

    const int radius = std::min({MulDiv(7, static_cast<int>(dpi), 96),
        (right - left) / 2, (bottom - top) / 2});
    for (int y = top; y < bottom; ++y) {
        for (int x = left; x < right; ++x) {
            const int cx = std::clamp(x, left + radius, right - radius - 1);
            const int cy = std::clamp(y, top + radius, bottom - radius - 1);
            const int dx = x - cx;
            const int dy = y - cy;
            auto& pixel = pixels[static_cast<std::size_t>(y) * width + x];
            if (dx * dx + dy * dy > radius * radius) pixel = 0x78000000u;
            else pixel |= 0xff000000u;
        }
    }
}

namespace {

void MakeLayeredRoundedOpaque(std::uint32_t* pixels, int width, int height,
                              const RECT& rect, int radius) {
    const int left = std::clamp(static_cast<int>(rect.left), 0, width);
    const int right = std::clamp(static_cast<int>(rect.right), 0, width);
    const int top = std::clamp(static_cast<int>(rect.top), 0, height);
    const int bottom = std::clamp(static_cast<int>(rect.bottom), 0, height);
    const int corner = std::clamp(radius, 0, std::min((right-left)/2,(bottom-top)/2));
    for (int y=top;y<bottom;++y) {
        for (int x=left;x<right;++x) {
            const int cx=std::clamp(x,left+corner,right-corner-1);
            const int cy=std::clamp(y,top+corner,bottom-corner-1);
            const int dx=x-cx,dy=y-cy;
            auto& pixel=pixels[static_cast<std::size_t>(y)*width+x];
            if(dx*dx+dy*dy>corner*corner) pixel=0x78000000u;
            else pixel|=0xff000000u;
        }
    }
}

}  // namespace

void DrawLayeredPanel(HDC dc, std::uint32_t* pixels, int width, int height,
                      const RECT& rect, std::wstring_view text, HFONT font, bool warning,
                      UINT dpi) {
    const RECT clipped{std::clamp<LONG>(rect.left,0,width),std::clamp<LONG>(rect.top,0,height),
                       std::clamp<LONG>(rect.right,0,width),std::clamp<LONG>(rect.bottom,0,height)};
    const COLORREF fill=warning?RGB(255,245,244):RGB(255,255,255);
    const COLORREF border=warning?RGB(215,82,70):RGB(225,228,233);
    const COLORREF foreground=warning?RGB(160,35,28):RGB(31,35,40);
    HBRUSH backdrop=::CreateSolidBrush(RGB(0,0,0));
    ::FillRect(dc,&clipped,backdrop);::DeleteObject(backdrop);
    const int radius=MulDiv(6,static_cast<int>(dpi),96);
    if(g_gdiplusReady){
        Gdiplus::Graphics graphics(dc);graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        const Gdiplus::RectF area(static_cast<float>(clipped.left),static_cast<float>(clipped.top),
            static_cast<float>(clipped.right-clipped.left),static_cast<float>(clipped.bottom-clipped.top));
        Gdiplus::GraphicsPath path;AddRoundedRectPath(path,area,static_cast<float>(radius));
        Gdiplus::SolidBrush brush(Gdiplus::Color(255,GetRValue(fill),GetGValue(fill),GetBValue(fill)));
        graphics.FillPath(&brush,&path);
        Gdiplus::Pen pen(Gdiplus::Color(255,GetRValue(border),GetGValue(border),GetBValue(border)),1.0f);
        graphics.DrawPath(&pen,&path);
        HGDIOBJ old=font?::SelectObject(dc,font):nullptr;
        ::SetBkMode(dc,TRANSPARENT);::SetTextColor(dc,foreground);
        RECT label=clipped;
        ::DrawTextW(dc,text.data(),static_cast<int>(text.size()),&label,DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
        if(old)::SelectObject(dc,old);
    }else{
        HBRUSH brush=::CreateSolidBrush(fill);::FillRect(dc,&clipped,brush);::DeleteObject(brush);
        HGDIOBJ old=font?::SelectObject(dc,font):nullptr;
        ::SetBkMode(dc,TRANSPARENT);::SetTextColor(dc,foreground);
        ::DrawTextW(dc,text.data(),static_cast<int>(text.size()),const_cast<RECT*>(&clipped),DT_CENTER|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
        if(old)::SelectObject(dc,old);
    }
    MakeLayeredRoundedOpaque(pixels,width,height,clipped,radius);
}

void DrawRoundedPanel(HDC dc, const RECT& rect, COLORREF fill, COLORREF border,
                      int radiusPx) {
    if (!g_gdiplusReady) {
        HBRUSH brush = ::CreateSolidBrush(fill);
        ::FillRect(dc, &rect, brush);
        ::DeleteObject(brush);
        return;
    }
    Gdiplus::Graphics graphics(dc);
    graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Gdiplus::GraphicsPath path;
    AddRoundedRectPath(path,
                       Gdiplus::RectF(static_cast<float>(rect.left),
                                      static_cast<float>(rect.top),
                                      static_cast<float>(rect.right - rect.left),
                                      static_cast<float>(rect.bottom - rect.top)),
                       static_cast<float>(radiusPx));
    Gdiplus::SolidBrush brush(Gdiplus::Color(255, GetRValue(fill), GetGValue(fill),
                                             GetBValue(fill)));
    graphics.FillPath(&brush, &path);
    Gdiplus::Pen pen(Gdiplus::Color(255, GetRValue(border), GetGValue(border),
                                    GetBValue(border)),
                     1.0f);
    graphics.DrawPath(&pen, &path);
}

}  // namespace srl::ui
