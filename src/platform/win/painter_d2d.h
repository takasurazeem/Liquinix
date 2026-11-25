// drawing::Painter over Direct2D/DirectWrite (Windows). Draws into a render target that's already in BeginDraw, with
// a borrowed solid brush (its colour changes on each call, as the shell's code always did).
// The font comes from the caller: the shell passes its own (ui::Format), which picks SF Pro or Segoe UI from settings.
#pragma once
#include "../../core/drawing.h"
#include <d2d1.h>
#include <dwrite.h>

class PainterD2D : public drawing::Painter {
public:
    // creates a text format (centred vertically, no wrapping); the painter releases it
    using FontFn = IDWriteTextFormat* (*)(float size, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align);
    using WidthFn = float (*)(IDWriteTextFormat* tf, const std::wstring& s);
    PainterD2D(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, FontFn font, WidthFn width)
        : rt_(rt), br_(brush), font_(font), width_(width) {}

    void FillRoundedRect(const drawing::Rect& r, float radius, const drawing::Color& c) override;
    float TextWidth(const std::wstring& s, float size, drawing::Weight w) override;
    void Text(const std::wstring& s, const drawing::Rect& r, float size, drawing::Weight w, drawing::Align a, const drawing::Color& c) override;

private:
    ID2D1RenderTarget* rt_;
    ID2D1SolidColorBrush* br_;
    FontFn font_;
    WidthFn width_;
};
