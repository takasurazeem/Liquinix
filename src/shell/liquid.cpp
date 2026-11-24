// Desenho D2D do tema macOS Tahoe (Liquid Glass) nos painéis do Shell: Central de Controle,
// Central de Notificações, Launchpad/Apps e Spotlight. O vidro de fundo é o do backdrop.cpp; aqui
// ficam só os detalhes por cima dele: contorno especular, cápsulas e círculos de vidro.
// Tema = Ajustes do Sistema > Aparência > Tema (liquid_glass). Desligado, ninguém chama nada daqui.
#include "shell.h"
#include <algorithm>
#include "../comum/vidro.h"

bool liquid::On() { return backdrop::Current().liquid; }
bool liquid::Golden() { return backdrop::Current().golden; }
bool liquid::Leopard() { return backdrop::Current().leopard; }

// Contorno do Liquid Glass: a borda escurecida e o brilho especular (comum/vidro.h, o mesmo dos menus)
void liquid::Rim(ID2D1DeviceContext* dc, D2D1_RECT_F r, float rad, bool dark, float strength, float spec, float lx, float ly) {
    vidro::Borda(dc, r, rad, 1, dark, strength, true, spec, lx, ly);  // o vidro "clear": fio branco em volta toda, sem borda escura
}

// Bloco de vidro sobre um painel de vidro (módulos da Central, cartões): corpo claro translúcido,
// reflexo suave no alto e o contorno especular.
void liquid::Tile(ID2D1DeviceContext* dc, D2D1_RECT_F r, float rad, bool dark, float fill) {
    ID2D1SolidColorBrush* br = nullptr;
    if (FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, (dark ? 0.10f : 0.42f) * fill), &br))) return;
    auto rr = D2D1::RoundedRect(r, rad, rad);
    dc->FillRoundedRectangle(rr, br);
    br->Release();
    D2D1_GRADIENT_STOP gs[2] = { { 0, D2D1::ColorF(1, 1, 1, 0) }, { 1, D2D1::ColorF(1, 1, 1, 0) } };  // sem reflexo branco (Tahoe)
    ID2D1GradientStopCollection* stops = nullptr;
    if (SUCCEEDED(dc->CreateGradientStopCollection(gs, 2, &stops))) {
        ID2D1LinearGradientBrush* lg = nullptr;
        if (SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, r.top }, { 0, r.top + (r.bottom - r.top) * 0.6f }), stops, &lg))) {
            dc->FillRoundedRectangle(rr, lg);
            lg->Release();
        }
        stops->Release();
    }
    Rim(dc, r, rad, dark, 0.8f);
}
