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

// Desenha o botão k do Golden Gate só com Direct2D (sem bitmap): 0 fechar, 1 minimizar, 2 zoom, 3 inativo claro,
// 4 inativo escuro. Degradê de cima para baixo, aro escuro forte só dos lados e, nos coloridos, um brilho fino da
// própria cor em volta (mais forte embaixo). dark > 0 escurece (botão apertado). Os símbolos (×, −, +) ficam com quem
// chama, como nos outros temas.
inline void DrawGolden(ID2D1RenderTarget* rt, int k, D2D1_POINT_2F c, float r, float dark) {
    struct Cor { int top[3], bot[3], rim[3]; };
    static const Cor cores[5] = {
        { { 254, 111, 100 }, { 243, 139, 131 }, { 196, 9, 10 } },   // fechar
        { { 251, 185, 39 }, { 254, 211, 66 }, { 157, 9, 9 } },     // minimizar
        { { 96, 208, 40 }, { 152, 218, 120 }, { 9, 85, 9 } },      // zoom
        { { 221, 221, 221 }, { 209, 209, 209 }, { 150, 150, 150 } }, // inativo, fundo claro
        { { 84, 83, 83 }, { 74, 73, 73 }, { 0, 0, 0 } },           // inativo, fundo escuro (o aro é claro, abaixo)
    };
    k = (std::max)(0, (std::min)(4, k));
    float d = 1 - (std::max)(0.f, (std::min)(0.6f, dark));
    auto C = [&](const int* v, float a = 1) { return D2D1::ColorF(v[0] * d / 255.f, v[1] * d / 255.f, v[2] * d / 255.f, a); };
    const Cor& p = cores[k];
    const bool colorido = k < 3;
    auto linear = [&](const D2D1_GRADIENT_STOP* st, UINT n, float y0, float y1, auto&& draw) {
        ID2D1GradientStopCollection* gs = nullptr;
        ID2D1LinearGradientBrush* lb = nullptr;
        if (SUCCEEDED(rt->CreateGradientStopCollection(st, n, &gs)) &&
            SUCCEEDED(rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ c.x, y0 }, { c.x, y1 }), gs, &lb)))
            draw(lb);
        if (lb) lb->Release();
        if (gs) gs->Release();
    };
    {   // o corpo: chapado no alto, a cor de baixo a partir de ~80 % da altura
        D2D1_GRADIENT_STOP st[3] = { { 0, C(p.top) }, { 0.29f, C(p.top) }, { 0.80f, C(p.bot) } };
        linear(st, 3, c.y - r, c.y + r, [&](ID2D1Brush* b) { rt->FillEllipse(D2D1::Ellipse(c, r, r), b); });
    }
    if (k == 4) {
        // inativo escuro: sem aro escuro, só um fio claro em cima e outro mais claro embaixo
        D2D1_GRADIENT_STOP rs[4] = { { 0, D2D1::ColorF(1, 1, 1, 0.28f) }, { 0.25f, D2D1::ColorF(1, 1, 1, 0) }, { 0.75f, D2D1::ColorF(1, 1, 1, 0) }, { 1, D2D1::ColorF(1, 1, 1, 0.5f) } };
        linear(rs, 4, c.y - r, c.y + r, [&](ID2D1Brush* b) { rt->DrawEllipse(D2D1::Ellipse(c, r - 0.25f, r - 0.25f), b, 0.5f); });
        return;
    }
    {
        // aro: a força depende da distância horizontal ao centro (pincel radial bem alto = |dx| / r): forte dos
        // lados, fraco em cima e embaixo. No inativo claro, bem mais leve.
        float m = colorido ? 1.f : 0.45f;
        D2D1_GRADIENT_STOP st[3] = { { 0, C(p.rim, 0.10f * m) }, { 0.6f, C(p.rim, 0.35f * m) }, { 0.92f, C(p.rim, m) } };
        ID2D1GradientStopCollection* gs = nullptr;
        ID2D1RadialGradientBrush* rb = nullptr;
        if (SUCCEEDED(rt->CreateGradientStopCollection(st, 3, &gs)) &&
            SUCCEEDED(rt->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(c, { 0, 0 }, r, r * 60), gs, &rb)))
            rt->DrawEllipse(D2D1::Ellipse(c, r - 0.4f, r - 0.4f), rb, 0.8f);
        if (rb) rb->Release();
        if (gs) gs->Release();
    }
    if (colorido) {  // o brilho colorido em volta, mais forte embaixo
        D2D1_GRADIENT_STOP st[2] = { { 0, C(p.top, 0.03f) }, { 1, C(p.top, 0.20f) } };
        linear(st, 2, c.y - r, c.y + r + 1, [&](ID2D1Brush* b) { rt->DrawEllipse(D2D1::Ellipse(c, r + 0.6f, r + 0.6f), b, 1.2f); });
    }
}

// Chame logo depois de pintar o círculo do botão (FillEllipse), com o mesmo pincel: pinta por cima o degradê e o aro
// do Golden Gate (DrawGolden) ou o gel do Leopard, escolhidos pela cor que está no pincel (vermelho, amarelo, verde;
// cinza/apagado vira o botão inativo). Não mexe na cor do pincel. Fora desses dois temas não faz nada.
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
    // Golden Gate: vermelho, amarelo ou verde (também apertado, mais escuro), desenhados por DrawGolden
    bool solid = base.a >= 0.9f;
    float mx = (std::max)(base.r, (std::max)(base.g, base.b));
    if (solid && mx > 0.5f) {
        float nr = base.r / mx, ng = base.g / mx, nb = base.b / mx;
        int k = nr > 0.9f && ng < 0.5f ? 0 : nr > 0.9f && ng >= 0.5f && nb < 0.5f ? 1 : ng > 0.9f && nr < 0.45f ? 2 : -1;
        static const float ref[3] = { 1.0f, 0.996f, 0.784f };  // o canal mais forte da cor normal de cada um
        if (k >= 0) { DrawGolden(rt, k, c, r, (std::min)(0.6f, (std::max)(0.f, 1 - mx / ref[k]))); return; }
    }
    // cinza (janela inativa, Grafite): o botão inativo claro ou escuro (pelo fundo que o app pediu)
    bool darkUi = solid ? (0.2126f * base.r + 0.7152f * base.g + 0.0722f * base.b) < 0.5f : base.r > 0.5f;
    DrawGolden(rt, darkUi ? 4 : 3, c, r, 0);
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
