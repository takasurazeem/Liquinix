// desenho::Pintor sobre Direct2D/DirectWrite (Windows). Desenha num render target que já está em BeginDraw, com um
// pincel sólido emprestado (a cor muda a cada chamada, como o código do Shell sempre fez).
// A fonte vem de quem usa: o Shell passa a dele (ui::Format), que escolhe SF Pro ou Segoe UI pelos ajustes.
#pragma once
#include "../../nucleo/desenho.h"
#include <d2d1.h>
#include <dwrite.h>

class PintorD2D : public desenho::Pintor {
public:
    // cria um formato de texto (centrado na vertical, sem quebra de linha); o pintor solta com Release
    using FonteFn = IDWriteTextFormat* (*)(float tamanho, DWRITE_FONT_WEIGHT peso, DWRITE_TEXT_ALIGNMENT alinhar);
    using LarguraFn = float (*)(IDWriteTextFormat* tf, const std::wstring& s);
    PintorD2D(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* pincel, FonteFn fonte, LarguraFn largura)
        : rt_(rt), br_(pincel), fonte_(fonte), largura_(largura) {}

    void RetArredondado(const desenho::Ret& r, float raio, const desenho::Cor& c) override;
    float LarguraTexto(const std::wstring& s, float tamanho, desenho::Peso p) override;
    void Texto(const std::wstring& s, const desenho::Ret& r, float tamanho, desenho::Peso p, desenho::Alinhar a, const desenho::Cor& c) override;

private:
    ID2D1RenderTarget* rt_;
    ID2D1SolidColorBrush* br_;
    FonteFn fonte_;
    LarguraFn largura_;
};
