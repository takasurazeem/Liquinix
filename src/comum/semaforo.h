#pragma once
// Cores dos botões da janela (fechar, minimizar, zoom). Com a cor de destaque Grafite (Ajustes > Aparência,
// `destaque=grafite` em %APPDATA%\Shell\ajustes.txt) os três ficam cinza, como no Mac.
// Uso: static const D2D1_COLOR_F fill_c[3] = {...}; const D2D1_COLOR_F* fill = semaforo::Pick(fill_c, semaforo::FILL);
// O arquivo é relido quando muda (no máximo a cada 0,5 s): trocar a cor no Ajustes chega no próximo desenho.
#include <windows.h>
#include <d2d1.h>
#include <cstdio>
#include <cwchar>
#include <algorithm>
#include <cmath>
#include <string>

namespace semaforo {

enum Kind { FILL, RING, GLYPH };

// lê destaque= e o tema (golden_gate=1 junto com liquid_glass=1 = macOS Golden Gate) do ajustes.txt
struct Estado { bool grafite = false, aqua = false, liquid = false, leopard = false; };
inline const Estado& Ler(bool force = false) {  // force: relê já (o Shell avisou que os ajustes mudaram)
    static Estado st;
    static DWORD lastCheck = 0;
    static FILETIME lastWrite{};
    DWORD now = GetTickCount();
    if (lastCheck && now - lastCheck < 500 && !force) return st;
    lastCheck = now;
    wchar_t ad[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", ad, MAX_PATH);
    if (!n || n >= MAX_PATH) return st;
    std::wstring path = std::wstring(ad) + L"\\Shell\\ajustes.txt";
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return st;
    if (!CompareFileTime(&fa.ftLastWriteTime, &lastWrite)) return st;
    lastWrite = fa.ftLastWriteTime;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r, ccs=UTF-8") || !f) return st;
    bool g = false, liquid = false, golden = false, leo = false;
    wchar_t line[512];
    while (fgetws(line, 512, f)) {
        if (wcsncmp(line, L"destaque=", 9) == 0) g = wcsncmp(line + 9, L"grafite", 7) == 0;
        if (wcsncmp(line, L"liquid_glass=", 13) == 0) liquid = line[13] == L'1';
        if (wcsncmp(line, L"golden_gate=", 12) == 0) golden = line[12] == L'1';
        if (wcsncmp(line, L"leopard=", 8) == 0) leo = line[8] == L'1';
    }
    fclose(f);
    st.grafite = g;
    st.aqua = liquid && golden;
    st.liquid = liquid;
    st.leopard = leo && !liquid;  // Mac OS X Leopard: só sem o Liquid Glass (o Sequoia por baixo)
    return st;
}

inline bool Grafite() { return Ler().grafite; }

// Tema macOS Golden Gate (macOS 27, beta 6 em diante): os botões ganharam volume de novo. Medido nas capturas do
// 512 Pixels (Finder, 2x, botão de 28 px): o círculo é um degradê de cima para baixo — mais saturado no alto, mais
// claro embaixo, chapado a partir de ~78 % da altura — e tem um aro escuro de 0,5 pt que só aparece dos lados (some
// em cima e fica fraco embaixo). Nada de reflexo branco. Cores lidas (topo -> base; aro):
//   fechar  (254,111,100) -> (243,139,131); aro (196,9,10)
//   minim.  (251,185,39)  -> (254,211,66);  aro (157,9,9)
//   zoom    (96,208,40)   -> (152,218,120); aro (9,85,9)
inline bool Aqua() { return Ler().aqua; }
inline bool Liquid() { return Ler().liquid; }  // Tahoe ou Golden Gate (cantos de 16)
inline bool Leopard() { return Ler().leopard; }  // tema Mac OS X Leopard (leopard=1, sem liquid_glass)

// Mac OS X Leopard (10.5): os botões de gel do Aqua. Cada um é uma bolha de vidro colorido: o miolo mais claro
// embaixo (a luz que atravessa o gel e sai por baixo), a borda saturada e escura, um aro fino ainda mais escuro e o
// reflexo branco em meia-lua na metade de cima. Cores medidas no Font Book ativo de uma captura do 10.5 a 1x (botão
// de 14 px, 21 px entre centros; borda -> miolo; aro):
//   fechar  (184,43,44)  -> (253,93,93);   aro (140,42,44)
//   minim.  (207,131,59) -> (252,212,106); aro (165,83,34)
//   zoom    (99,153,62)  -> (167,217,100); aro (53,88,35)
//   inativo (190,190,190)-> (209,209,209); aro (163,163,163) (Finder atrás)
inline void DrawGel(ID2D1RenderTarget* rt, int k, D2D1_POINT_2F c, float r, float dark) {
    auto C = [](int R, int G, int B, float a = 1) { return D2D1::ColorF(R / 255.f, G / 255.f, B / 255.f, a); };
    static const int edge[4][3] = { { 184, 43, 44 }, { 207, 131, 59 }, { 99, 153, 62 }, { 190, 190, 190 } };
    static const int core[4][3] = { { 255, 128, 126 }, { 255, 226, 130 }, { 190, 232, 128 }, { 222, 222, 222 } };
    static const int ring[4][3] = { { 140, 42, 44 }, { 165, 83, 34 }, { 53, 88, 35 }, { 163, 163, 163 } };
    k = (std::max)(0, (std::min)(3, k));
    float d = 1 - (std::max)(0.f, (std::min)(0.6f, dark));  // apertado: o gel inteiro escurece
    auto M = [&](const int* v, float a = 1) { return C((int)(v[0] * d), (int)(v[1] * d), (int)(v[2] * d), a); };
    D2D1_ELLIPSE disc = D2D1::Ellipse(c, r, r);
    {   // o corpo: degradê radial com o centro embaixo (o brilho de dentro do gel)
        D2D1_GRADIENT_STOP st[3] = { { 0, M(core[k]) }, { 0.55f, M(core[k]) }, { 1, M(edge[k]) } };
        st[1].color.r = (st[0].color.r + st[2].color.r) / 2; st[1].color.g = (st[0].color.g + st[2].color.g) / 2; st[1].color.b = (st[0].color.b + st[2].color.b) / 2;
        ID2D1GradientStopCollection* gs = nullptr; ID2D1RadialGradientBrush* rb = nullptr;
        if (SUCCEEDED(rt->CreateGradientStopCollection(st, 3, &gs)) &&
            SUCCEEDED(rt->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties({ c.x, c.y + r * 0.55f }, { 0, 0 }, r * 1.05f, r * 1.15f), gs, &rb)))
            rt->FillEllipse(disc, rb);
        if (rb) rb->Release();
        if (gs) gs->Release();
    }
    {   // aro escuro fino em volta
        ID2D1SolidColorBrush* b = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(M(ring[k], 0.95f), &b))) { rt->DrawEllipse(D2D1::Ellipse(c, r - 0.35f, r - 0.35f), b, 0.7f); b->Release(); }
    }
    {   // o reflexo em meia-lua: branco forte em cima que some antes do meio
        D2D1_ELLIPSE hl = D2D1::Ellipse({ c.x, c.y - r * 0.50f }, r * 0.55f, r * 0.36f);  // o brilho é pequeno e bem no alto
        D2D1_GRADIENT_STOP st[2] = { { 0, D2D1::ColorF(1, 1, 1, 0.95f * d) }, { 1, D2D1::ColorF(1, 1, 1, 0.08f) } };
        ID2D1GradientStopCollection* gs = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
        if (SUCCEEDED(rt->CreateGradientStopCollection(st, 2, &gs)) &&
            SUCCEEDED(rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ c.x, c.y - r * 0.88f }, { c.x, c.y + r * 0.02f }), gs, &lb)))
            rt->FillEllipse(hl, lb);
        if (lb) lb->Release();
        if (gs) gs->Release();
    }
    {   // o fio de luz na borda de baixo (a luz saindo do gel)
        D2D1_GRADIENT_STOP st[2] = { { 0, D2D1::ColorF(1, 1, 1, 0) }, { 1, D2D1::ColorF(1, 1, 1, 0.45f) } };
        ID2D1GradientStopCollection* gs = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
        if (SUCCEEDED(rt->CreateGradientStopCollection(st, 2, &gs)) &&
            SUCCEEDED(rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ c.x, c.y + r * 0.3f }, { c.x, c.y + r }), gs, &lb)))
            rt->DrawEllipse(D2D1::Ellipse(c, r - 1.1f, r - 1.1f), lb, 0.8f);
        if (lb) lb->Release();
        if (gs) gs->Release();
    }
}

#include "semaforo_mac27.inc"  // os três botões do macOS 27 tirados da captura 2x (tools/semaforo-mac27.py)

// Desenha o botão k (0 fechar, 1 minimizar, 2 zoom) do macOS 27 pixel a pixel: a imagem tirada da captura do Mac
// (36 px = o botão de 28 px e o brilho em volta), escalada para o raio pedido. dark > 0 escurece (botão apertado).
inline void DrawMac27(ID2D1RenderTarget* rt, int k, D2D1_POINT_2F c, float r, float dark) {
    // escolhe a imagem mais perto do tamanho na tela (36 px = 2x, 18 px = 1x, já reduzida com Lanczos): encolher na hora
    // com o filtro simples serrilhava a borda. Em escala inteira, o centro vai para o meio de um pixel (sem borrão).
    float dpiX = 96, dpiY = 96; rt->GetDpi(&dpiX, &dpiY);
    float k1 = dpiX / 96.f, sizePx = 2 * r * 36.f / 28.f * k1;  // lado da imagem na tela, em px
    bool one = sizePx < 27;
    UINT n = one ? 18 : 36;
    ID2D1Bitmap* bmp = nullptr;
    D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpiX, dpiY);
    if (FAILED(rt->CreateBitmap(D2D1::SizeU(n, n), one ? MAC27_LIGHTS_1X[k] : MAC27_LIGHTS[k], n * 4, bp, &bmp)) || !bmp) return;
    float h = r * 36.f / 28.f;  // a imagem tem 36 px para um botão de 28 px
    D2D1_RECT_F dst = D2D1::RectF(c.x - h, c.y - h, c.x + h, c.y + h);
    if (std::fabs(sizePx - (float)n) < 0.6f) {  // 1:1 na tela: encaixa na grade de pixels
        float x0 = std::round((c.x - h) * k1) / k1, y0 = std::round((c.y - h) * k1) / k1;
        dst = D2D1::RectF(x0, y0, x0 + n / k1, y0 + n / k1);
    }
    rt->DrawBitmap(bmp, dst, 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    bmp->Release();
    if (dark > 0.01f) {
        ID2D1SolidColorBrush* b = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, dark), &b))) { rt->FillEllipse(D2D1::Ellipse(c, r, r), b); b->Release(); }
    }
}

// Chame logo depois de pintar o círculo do botão (FillEllipse), com o mesmo pincel: pinta por cima o degradê e o aro
// do Golden Gate, escolhidos pela cor que está no pincel (vermelho, amarelo, verde; cinza/apagado fica com um degradê
// leve da própria cor). Não mexe na cor do pincel. Fora do Golden Gate não faz nada.
inline void Gloss(ID2D1RenderTarget* rt, D2D1_POINT_2F c, float r, ID2D1SolidColorBrush* fillBrush) {
    if (!rt || !fillBrush || !(Aqua() || Leopard())) return;
    D2D1_COLOR_F base = fillBrush->GetColor();
    if (Leopard()) {  // Leopard: o gel do Aqua, pela cor do pincel (vermelho, amarelo, verde; o resto fica cinza)
        bool solid = base.a >= 0.9f;
        float mx = (std::max)(base.r, (std::max)(base.g, base.b));
        if (solid && mx > 0.5f) {
            float nr = base.r / mx, ng = base.g / mx, nb = base.b / mx;
            int k = nr > 0.9f && ng < 0.5f ? 0 : nr > 0.9f && ng >= 0.5f && nb < 0.5f ? 1 : ng > 0.9f && nr < 0.45f ? 2 : -1;
            static const float ref[3] = { 1.0f, 0.996f, 0.784f };
            if (k >= 0) { DrawGel(rt, k, c, r, (std::min)(0.6f, (std::max)(0.f, 1 - mx / ref[k]))); return; }
        }
        DrawGel(rt, 3, c, r, 0);
        return;
    }
    auto C = [](int R, int G, int B, float a = 1) { return D2D1::ColorF(R / 255.f, G / 255.f, B / 255.f, a); };
    D2D1_COLOR_F top, bot, rim;
    bool solid = base.a >= 0.9f;
    {   // vermelho, amarelo ou verde (também apertado, mais escuro): a imagem do Mac
        float mx = (std::max)(base.r, (std::max)(base.g, base.b));
        if (solid && mx > 0.5f) {
            float nr = base.r / mx, ng = base.g / mx, nb = base.b / mx;
            int k = nr > 0.9f && ng < 0.5f ? 0 : nr > 0.9f && ng >= 0.5f && nb < 0.5f ? 1 : ng > 0.9f && nr < 0.45f ? 2 : -1;
            static const float ref[3] = { 1.0f, 0.996f, 0.784f };  // o canal mais forte da cor normal de cada um
            if (k >= 0) { DrawMac27(rt, k, c, r, (std::min)(0.6f, (std::max)(0.f, 1 - mx / ref[k]))); return; }
        }
        // cinza (janela inativa, Grafite): o botão inativo do Mac, o claro ou o escuro (pelo fundo que o app pediu)
        bool darkUi = solid ? (0.2126f * base.r + 0.7152f * base.g + 0.0722f * base.b) < 0.5f : base.r > 0.5f;
        DrawMac27(rt, darkUi ? 4 : 3, c, r, 0);
        return;
    }
    if (solid && base.r > 0.85f && base.g < 0.5f) { top = C(254, 111, 100); bot = C(243, 139, 131); rim = C(196, 9, 10); }
    else if (solid && base.r > 0.85f && base.g >= 0.5f) { top = C(251, 185, 39); bot = C(254, 211, 66); rim = C(157, 9, 9); }
    else if (solid && base.g > 0.55f && base.r < 0.45f) { top = C(96, 208, 40); bot = C(152, 218, 120); rim = C(9, 85, 9); }
    else {
        // cinza (janela inativa, Grafite) ou apagado. Medido na janela inativa do modo escuro: (84,83,83) em cima ->
        // (74,73,73) embaixo, com um fio claro na borda de cima (128) e outro mais claro embaixo (171), sem aro escuro
        auto mix = [](D2D1_COLOR_F a, float k, float t) { return D2D1::ColorF(a.r + (k - a.r) * t, a.g + (k - a.g) * t, a.b + (k - a.b) * t, a.a); };
        top = mix(base, 1, 0.05f); bot = mix(base, 0, 0.06f);
        D2D1_GRADIENT_STOP st[3] = { { 0, top }, { 0.2f, top }, { 0.9f, bot } };
        ID2D1GradientStopCollection* gs = nullptr;
        ID2D1LinearGradientBrush* lb = nullptr;
        if (SUCCEEDED(rt->CreateGradientStopCollection(st, 3, &gs)) &&
            SUCCEEDED(rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ c.x, c.y - r }, { c.x, c.y + r }), gs, &lb)))
            rt->FillEllipse(D2D1::Ellipse(c, r, r), lb);
        if (lb) lb->Release();
        if (gs) gs->Release();
        D2D1_GRADIENT_STOP rs[4] = { { 0, D2D1::ColorF(1, 1, 1, 0.28f) }, { 0.25f, D2D1::ColorF(1, 1, 1, 0) }, { 0.75f, D2D1::ColorF(1, 1, 1, 0) }, { 1, D2D1::ColorF(1, 1, 1, 0.5f) } };
        gs = nullptr; lb = nullptr;
        if (SUCCEEDED(rt->CreateGradientStopCollection(rs, 4, &gs)) &&
            SUCCEEDED(rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ c.x, c.y - r }, { c.x, c.y + r }), gs, &lb)))
            rt->DrawEllipse(D2D1::Ellipse(c, r - 0.25f, r - 0.25f), lb, 0.5f);
        if (lb) lb->Release();
        if (gs) gs->Release();
        return;
    }
    D2D1_ELLIPSE disc = D2D1::Ellipse(c, r, r);
    {
        D2D1_GRADIENT_STOP st[3] = { { 0, top }, { 0.29f, top }, { 0.80f, bot } };  // medido: chapado até 29 %, a cor de baixo em 80 %
        ID2D1GradientStopCollection* gs = nullptr;
        ID2D1LinearGradientBrush* lb = nullptr;
        if (SUCCEEDED(rt->CreateGradientStopCollection(st, 3, &gs)) &&
            SUCCEEDED(rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ c.x, c.y - r }, { c.x, c.y + r }), gs, &lb)))
            rt->FillEllipse(disc, lb);
        if (lb) lb->Release();
        if (gs) gs->Release();
    }
    {
        // aro: a força depende da distância horizontal ao centro (pincel radial bem alto = |dx| / r): forte dos lados,
        // fraco em cima e embaixo (medido pixel a pixel na captura 2x do Finder)
        D2D1_COLOR_F a0 = rim; a0.a = 0.10f;
        D2D1_COLOR_F a1 = rim; a1.a = 0.35f;
        D2D1_COLOR_F a2 = rim; a2.a = 1;
        D2D1_GRADIENT_STOP st[3] = { { 0, a0 }, { 0.6f, a1 }, { 0.92f, a2 } };
        ID2D1GradientStopCollection* gs = nullptr;
        ID2D1RadialGradientBrush* rb = nullptr;
        if (SUCCEEDED(rt->CreateGradientStopCollection(st, 3, &gs)) &&
            SUCCEEDED(rt->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(c, { 0, 0 }, r, r * 60), gs, &rb)))
            rt->DrawEllipse(D2D1::Ellipse(c, r - 0.4f, r - 0.4f), rb, 0.8f);
        if (rb) rb->Release();
        if (gs) gs->Release();
    }
    // o brilho colorido que o botão solta em volta, mais forte embaixo (medido: ~12 % da cor logo abaixo)
    if (solid) {
        D2D1_COLOR_F a0 = top; a0.a = 0.03f;
        D2D1_COLOR_F a1 = top; a1.a = 0.20f;
        D2D1_GRADIENT_STOP st[2] = { { 0, a0 }, { 1, a1 } };
        ID2D1GradientStopCollection* gs = nullptr;
        ID2D1LinearGradientBrush* lb = nullptr;
        if (SUCCEEDED(rt->CreateGradientStopCollection(st, 2, &gs)) &&
            SUCCEEDED(rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ c.x, c.y - r }, { c.x, c.y + r + 1 }), gs, &lb)))
            rt->DrawEllipse(D2D1::Ellipse(c, r + 0.6f, r + 0.6f), lb, 1.2f);
        if (lb) lb->Release();
        if (gs) gs->Release();
    }
}

inline const D2D1_COLOR_F* Pick(const D2D1_COLOR_F* normal, Kind k) {
    // Golden Gate: o aro é o do Gloss (escuro só dos lados); o contorno claro antigo some
    static const D2D1_COLOR_F none[3] = { { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 } };
    if (k == RING && (Aqua() || Leopard())) return none;  // Leopard: o aro é o do gel
    if (!Grafite()) return normal;
    static const D2D1_COLOR_F fill[3] = { { 0.58f, 0.58f, 0.59f, 1 }, { 0.58f, 0.58f, 0.59f, 1 }, { 0.58f, 0.58f, 0.59f, 1 } };
    static const D2D1_COLOR_F ring[3] = { { 0.47f, 0.47f, 0.48f, 1 }, { 0.47f, 0.47f, 0.48f, 1 }, { 0.47f, 0.47f, 0.48f, 1 } };
    static const D2D1_COLOR_F glyph[3] = { { 0.20f, 0.20f, 0.21f, 1 }, { 0.20f, 0.20f, 0.21f, 1 }, { 0.20f, 0.20f, 0.21f, 1 } };
    return k == FILL ? fill : k == RING ? ring : glyph;
}

inline D2D1_COLOR_F Close() { static const D2D1_COLOR_F c[3] = { { 1.0f, 0.373f, 0.341f, 1 } }; return Pick(c, FILL)[0]; }

}  // namespace semaforo
