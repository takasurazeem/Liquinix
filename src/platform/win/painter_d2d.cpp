// drawing::Painter over Direct2D (see painter_d2d.h): the same calls the shell's code used to make directly.
#include "painter_d2d.h"

namespace {
D2D1_RECT_F R(const drawing::Rect& r) { return D2D1::RectF(r.left, r.top, r.right, r.bottom); }
D2D1_COLOR_F C(const drawing::Color& c) { return D2D1::ColorF(c.r, c.g, c.b, c.a); }
DWRITE_TEXT_ALIGNMENT A(drawing::Align a) {
    return a == drawing::Align::Center ? DWRITE_TEXT_ALIGNMENT_CENTER : a == drawing::Align::Trailing ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING;
}
}  // namespace

void PainterD2D::FillRoundedRect(const drawing::Rect& r, float radius, const drawing::Color& c) {
    br_->SetColor(C(c));
    rt_->FillRoundedRectangle(D2D1::RoundedRect(R(r), radius, radius), br_);
}

float PainterD2D::TextWidth(const std::wstring& s, float size, drawing::Weight w) {
    IDWriteTextFormat* tf = font_ ? font_(size, (DWRITE_FONT_WEIGHT)w, DWRITE_TEXT_ALIGNMENT_CENTER) : nullptr;
    if (!tf) return -1;
    float width = width_ ? width_(tf, s) : 0;
    tf->Release();
    return width;
}

void PainterD2D::Text(const std::wstring& s, const drawing::Rect& r, float size, drawing::Weight w, drawing::Align a, const drawing::Color& c) {
    IDWriteTextFormat* tf = font_ ? font_(size, (DWRITE_FONT_WEIGHT)w, A(a)) : nullptr;
    if (!tf) return;
    tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    br_->SetColor(C(c));
    rt_->DrawTextW(s.c_str(), (UINT32)s.size(), tf, R(r), br_);
    tf->Release();
}
