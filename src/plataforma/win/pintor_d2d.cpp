// desenho::Pintor sobre Direct2D (ver pintor_d2d.h): as mesmas chamadas que o código do Shell fazia direto.
#include "pintor_d2d.h"

namespace {
D2D1_RECT_F R(const desenho::Ret& r) { return D2D1::RectF(r.esq, r.topo, r.dir, r.base); }
D2D1_COLOR_F C(const desenho::Cor& c) { return D2D1::ColorF(c.r, c.g, c.b, c.a); }
DWRITE_TEXT_ALIGNMENT A(desenho::Alinhar a) {
    return a == desenho::Alinhar::Centro ? DWRITE_TEXT_ALIGNMENT_CENTER : a == desenho::Alinhar::Direita ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING;
}
}  // namespace

void PintorD2D::RetArredondado(const desenho::Ret& r, float raio, const desenho::Cor& c) {
    br_->SetColor(C(c));
    rt_->FillRoundedRectangle(D2D1::RoundedRect(R(r), raio, raio), br_);
}

float PintorD2D::LarguraTexto(const std::wstring& s, float tamanho, desenho::Peso p) {
    IDWriteTextFormat* tf = fonte_ ? fonte_(tamanho, (DWRITE_FONT_WEIGHT)p, DWRITE_TEXT_ALIGNMENT_CENTER) : nullptr;
    if (!tf) return -1;
    float w = largura_ ? largura_(tf, s) : 0;
    tf->Release();
    return w;
}

void PintorD2D::Texto(const std::wstring& s, const desenho::Ret& r, float tamanho, desenho::Peso p, desenho::Alinhar a, const desenho::Cor& c) {
    IDWriteTextFormat* tf = fonte_ ? fonte_(tamanho, (DWRITE_FONT_WEIGHT)p, A(a)) : nullptr;
    if (!tf) return;
    tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    br_->SetColor(C(c));
    rt_->DrawTextW(s.c_str(), (UINT32)s.size(), tf, R(r), br_);
    tf->Release();
}
