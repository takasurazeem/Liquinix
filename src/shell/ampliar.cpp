// "Ampliar" do Mac nas janelas de outros apps, animado. A MacJanela.dll (dentro do app) pede ao Shell; o
// Shell tira uma foto da janela e a mostra numa camada por cima de tudo, andando e crescendo do retângulo de
// agora até o novo (ease-in-out, ~0,3 s, como o zoom do macOS). A DLL esconde a janela de verdade (cloak) e
// muda o tamanho dela; uns 110 ms depois, com o app já redesenhado no tamanho novo, pede uma segunda foto,
// que entra por cima da primeira (dissolve) ainda em movimento: no fim, a foto já é a janela como ela vai
// ficar, a DLL mostra a janela de verdade e a foto some de uma vez (sem "pulo" de layout nem fantasma).
// Os três botões e o título da barra são desenhados pela DLL numa camada à parte (a foto não pega): o Shell
// desenha os mesmos nas fotos.
#include "shell.h"
#include <dcomp.h>
#include <map>
#include <dwmapi.h>
#include <cmath>
#include "../comum/semaforo.h"

namespace {

HWND g_wnd;
IDCompositionTarget* g_target;
IDCompositionVisual* g_root;
const UINT_PTR T_CLEAN = 1, T_SHOW = 2, T_RETAKE = 3, T_FADE = 4;
HWND g_showFor;  // janela que reaparece quando a foto chegar no fim
// A janela reaparece em ~0,31 s (timer da DLL); a foto fica parada por cima mais um pouco e some de uma vez
const float MOVE_S = 0.30f, HOLD_S = 0.05f, FADE_S = 0.016f, DISSOLVE_S = 0.10f;
const float ZOOM_S = 0.24f;  // maximizar/restaurar (flags 4): mais rápido, como no Mac

struct Layer {
    IDCompositionVisual* v = nullptr;
    IDCompositionScaleTransform* sc = nullptr;
    IDCompositionEffectGroup* fx = nullptr;
    IDCompositionSurface* surf = nullptr;
    IDCompositionRectangleClip* clip = nullptr;  // flags 4: o recorte que cresce (a foto não estica)
};
struct Anim {
    HWND h = nullptr;
    RECT from{}, to{};
    int flags = 0;     // 1: barra padrão (desenha botões e título); 2: janela de plug-in (botões menores)
    double start = 0;  // anim::Now() do começo
    Layer a, b;        // a: foto de antes (parada); b: foto já no tamanho novo (a que anima)
    UINT wa = 0, ha = 0;
    D2D1_COLOR_F bar{ 0, 0, 0, 0 };  // cor da barra na primeira foto (a segunda vem com a barra preta: janela escondida)
    DWORD end = 0;
    DWORD showAt = 0;  // quando a janela de verdade deve reaparecer
    DWORD retakeAt = 0; int retakes = 0;
    DWORD fadeAt = 0;  // a janela já reapareceu: a foto some neste instante
    DWORD pollStart = 0;  // flags 4: conferindo se a janela de verdade já está desenhada
    DWORD minHold = 0;    // a foto não some antes disto (fim do movimento)
    std::vector<BYTE> sigFirst, sigLast;
    bool changed = false;  // foto nova tirada de novo durante o movimento (o app ainda pintando)
    bool cut = false;  // as fotos já estão sumindo
    std::vector<BYTE> px;  // flags 4: a foto de antes (para estender as bordas quando a janela cresce)
    float rad = 0;         // raio dos cantos (janela não maximizada)
};
std::vector<Anim> g_anims;

// As fotos somem só depois que a janela de verdade já está na tela ("ShellAmpliarPronto" da DLL/dos nossos
// apps). Antes era por tempo: se a janela demorava um pouco para reaparecer (Electron, apps ocupados),
// a foto sumia antes e o fundo aparecia por um quadro (piscava).
void Fade(Anim& an) {
    if (an.cut) return;
    an.cut = true;
    const int F = 8;
    std::vector<float> op(F);
    for (int k = 0; k < F; k++) { float x = (float)k / (F - 1); op[k] = 1 - x * x * (3 - 2 * x); }
    for (Layer* l : { &an.a, &an.b })
        if (l->fx) if (auto* c = ui::CompCurve(op, (an.flags & 4) ? 0.05f : 0.09f)) { l->fx->SetOpacity(c); c->Release(); }
    an.end = GetTickCount() + 170;
}

void Free(Layer& l) {
    if (g_root && l.v) g_root->RemoveVisual(l.v);
    if (l.v) l.v->Release();
    if (l.sc) l.sc->Release();
    if (l.fx) l.fx->Release();
    if (l.surf) l.surf->Release();
    if (l.clip) l.clip->Release();
    l = {};
}

bool MakeLayer(Layer& l, HWND h, RECT vis, int flags, UINT& w, UINT& hh, D2D1_COLOR_F& bar, std::vector<BYTE>* keep = nullptr);

// A foto nova sai às vezes com o app ainda pintando o tamanho novo (faixa branca na borda do Claude/Chrome,
// área vazia): aparecia por um quadro. Durante o movimento tira de novo e troca o conteúdo (mesmo tamanho).
void Retake() {
    DWORD now = GetTickCount();
    bool more = false;
    for (auto& an : g_anims) {
        if (!an.retakeAt || an.cut || !an.b.v) continue;
        if ((int)(now - an.retakeAt) < 0) { more = true; continue; }
        Layer t; UINT w = 0, hh = 0;
        if (MakeLayer(t, an.h, an.to, an.flags, w, hh, an.bar)) {
            an.b.v->SetContent(t.surf);
            std::swap(an.b.surf, t.surf);
            t.v->SetContent(nullptr);
            if (t.v) t.v->Release(); if (t.sc) t.sc->Release(); if (t.fx) t.fx->Release(); if (t.surf) t.surf->Release();
            ui::Comp()->Commit();
        }
        if (++an.retakes < 2) { an.retakeAt = now + 80; more = true; } else an.retakeAt = 0;
    }
    if (!more) KillTimer(g_wnd, T_RETAKE);
}

// Assinatura pequena (tons de cinza numa grade) da janela de verdade, já visível
bool Signature(HWND h, RECT vis, std::vector<BYTE>& sig) {
    RECT wr; GetWindowRect(h, &wr);
    RECT src{ vis.left - wr.left, vis.top - wr.top, vis.right - wr.left, vis.bottom - wr.top };
    std::vector<BYTE> px; UINT w = 0, hh = 0;
    if (!foto::Window(h, src, 0.f, px, w, hh) || !w || !hh) return false;
    const int GX = 64, GY = 32;
    sig.assign(GX * GY, 0);
    for (int y = 0; y < GY; y++)
        for (int x = 0; x < GX; x++) {
            size_t i = (((size_t)((y + 0.5f) * hh / GY)) * w + (size_t)((x + 0.5f) * w / GX)) * 4;
            sig[y * GX + x] = (BYTE)((px[i] * 114 + px[i + 1] * 587 + px[i + 2] * 299) / 1000);
        }
    return true;
}
int SigDiff(const std::vector<BYTE>& a, const std::vector<BYTE>& b) {
    if (a.size() != b.size() || a.empty()) return 255;
    int n = 0;
    for (size_t i = 0; i < a.size(); i++) if (abs((int)a[i] - (int)b[i]) > 10) n++;
    return n * 1000 / (int)a.size();  // por mil pontos que mudaram
}
// A janela de verdade (que reapareceu por baixo da foto) já está desenhada? O Chrome/Electron mostram
// primeiro a imagem velha e só depois pintam o tamanho novo: espera mudar e parar; os nossos apps já
// aparecem prontos (espera só parar). Limite de 450 ms.
bool CheckReady(Anim& an, DWORD now) {
    if (an.minHold && (int)(now - an.minHold) < 0) return false;  // ainda se mexendo
    DWORD el = now - an.pollStart;
    if (el >= 350) return true;
    std::vector<BYTE> sig;
    if (!Signature(an.h, an.to, sig)) return el >= 100;
    bool first = an.sigFirst.empty();
    if (first) { an.sigFirst = sig; an.sigLast = sig; return false; }
    if (SigDiff(sig, an.sigFirst) > 20) an.changed = true;
    bool stable = SigDiff(sig, an.sigLast) <= 3;
    an.sigLast = sig;
    bool leve = (an.flags & 8) != 0;
    return stable && (an.changed || (!leve && el >= 45) || el >= 220);
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_TIMER:
        if (w == T_SHOW) {  // a foto chegou: a janela de verdade aparece embaixo dela
            KillTimer(h, T_SHOW);
            static UINT msg = RegisterWindowMessageW(L"MacJanelaMostrar");
            if (g_showFor) PostMessageW(g_showFor, msg, 0, 0);
            g_showFor = nullptr;
            return 0;
        }
        if (w == T_RETAKE) { Retake(); return 0; }
        if (w == T_FADE) {
            DWORD now = GetTickCount();
            bool more = false, any = false;
            for (auto& an : g_anims)
                if (an.pollStart && !an.cut) {
                    if (CheckReady(an, now)) { Fade(an); any = true; } else more = true;
                } else if (an.fadeAt && !an.cut) {
                    if ((int)(now - an.fadeAt) >= 0) { Fade(an); any = true; } else more = true;
                }
            if (any) ui::Comp()->Commit();
            if (!more) KillTimer(h, T_FADE);
            return 0;
        }
        if (w == T_CLEAN) {
            DWORD now = GetTickCount();
            for (auto& an : g_anims)  // a janela não avisou (app antigo, ocupado): some assim mesmo
                if (!an.cut && !an.fadeAt && !an.pollStart && an.showAt && (int)(now - an.showAt) > 600) Fade(an);
            for (size_t i = 0; i < g_anims.size();)
                if ((int)(now - g_anims[i].end) >= 0) { Free(g_anims[i].a); Free(g_anims[i].b); g_anims.erase(g_anims.begin() + i); }
                else i++;
            if (ui::Comp()) ui::Comp()->Commit();
            if (g_anims.empty()) { KillTimer(h, T_CLEAN); ShowWindow(h, SW_HIDE); }
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

bool Ready() {
    IDCompositionDevice* dev = ui::Comp();
    if (!dev) return false;
    if (g_wnd) return true;
    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = shell::inst; wc.lpszClassName = L"ShellAmpliar";
    RegisterClassW(&wc);
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int w = GetSystemMetrics(SM_CXVIRTUALSCREEN), hh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    g_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_NOREDIRECTIONBITMAP,
                            wc.lpszClassName, L"", WS_POPUP, x, y, w, hh, nullptr, nullptr, shell::inst, nullptr);
    if (!g_wnd) return false;
    if (FAILED(dev->CreateTargetForHwnd(g_wnd, TRUE, &g_target)) || FAILED(dev->CreateVisual(&g_root))) return false;
    g_target->SetRoot(g_root);
    dev->Commit();
    return true;
}

float EaseInOut(float t) { return t < 0.5f ? 4 * t * t * t : 1 - std::pow(-2 * t + 2, 3.f) / 2; }

RECT At(const Anim& an, float e) {
    auto L = [e](LONG a, LONG b) { return (LONG)std::lround(a + (b - a) * e); };
    return { L(an.from.left, an.to.left), L(an.from.top, an.to.top), L(an.from.right, an.to.right), L(an.from.bottom, an.to.bottom) };
}

// Os três botões e o título, como a DLL desenha na barra (a foto não pega a camada dela)
void DrawChrome(ID2D1DeviceContext* dc, HWND h, UINT w, UINT hh, const std::vector<BYTE>& px, int flags, RECT vis) {
    if (!(flags & 3)) return;
    POINT c{ 0, 0 }; ClientToScreen(h, &c);
    float sc = GetDpiForWindow(h) / 96.f; if (sc <= 0) sc = 1;
    float cap = (float)(c.y - vis.top);
    if (cap < 16 * sc || cap > 100 * sc) return;
    // barra clara ou escura: o pixel do meio da barra
    size_t i = ((size_t)(cap / 2) * w + w / 2) * 4;
    bool dark = i + 2 < px.size() && (px[i + 2] * 299 + px[i + 1] * 587 + px[i] * 114) / 1000 < 128;
    ID2D1SolidColorBrush* b = nullptr;
    dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &b);
    if (!b) return;
    bool tool = (flags & 2) != 0;
    float x0 = (tool ? 14.f : 19.f) * sc, pitch = (tool ? 20.f : 23.f) * sc, r = (tool ? 6.f : 7.f) * sc;
    static const D2D1_COLOR_F on_c[3] = { D2D1::ColorF(1.0f, 0.373f, 0.341f), D2D1::ColorF(0.996f, 0.737f, 0.18f), D2D1::ColorF(0.157f, 0.784f, 0.251f) }; const D2D1_COLOR_F* on = semaforo::Pick(on_c, semaforo::FILL);
    for (int k = 0; k < 3; k++) {
        D2D1_POINT_2F p{ x0 + k * pitch, cap / 2 };
        b->SetColor(on[k]);
        dc->FillEllipse(D2D1::Ellipse(p, r, r), b);
        semaforo::Gloss(dc, p, r, b);
        b->SetColor(D2D1::ColorF(0, 0, 0, 0.12f));
        dc->DrawEllipse(D2D1::Ellipse(p, r - 0.25f * sc, r - 0.25f * sc), b, 0.5f * sc);
    }
    wchar_t t[256] = {}; GetWindowTextW(h, t, 256);
    if (t[0]) {
        IDWriteTextFormat* tf = ui::Format((tool ? 12.f : 13.f) * sc, tool ? DWRITE_FONT_WEIGHT_NORMAL : DWRITE_FONT_WEIGHT_SEMI_BOLD, false, DWRITE_TEXT_ALIGNMENT_CENTER);
        if (tf) {
            tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            b->SetColor(tool ? D2D1::ColorF(1, 1, 1, 0.62f) : dark ? D2D1::ColorF(1, 1, 1, 0.86f) : D2D1::ColorF(0, 0, 0, 0.85f));
            float m = (tool ? 70.f : 80.f) * sc;
            dc->DrawTextW(t, (UINT32)wcslen(t), tf, D2D1::RectF(m, 0, w - m, cap), b);
            tf->Release();
        }
    }
    b->Release();
}

// Altura da barra de título (px) na foto do retângulo `vis`, ou 0
int CaptionPx(HWND h, RECT vis) {
    POINT c{ 0, 0 }; ClientToScreen(h, &c);
    float sc = GetDpiForWindow(h) / 96.f; if (sc <= 0) sc = 1;
    int cap = c.y - vis.top;
    return cap >= 16 * sc && cap <= 100 * sc ? cap : 0;
}

// Foto da janela no retângulo `vis` (tela), com os botões e o título, numa camada nova (ainda sem movimento).
// bar: cor da barra a usar (a segunda foto, da janela escondida, vem com a barra preta); a == 0: lê da foto
bool MakeLayer(Layer& l, HWND h, RECT vis, int flags, UINT& w, UINT& hh, D2D1_COLOR_F& bar, std::vector<BYTE>* keep) {
    IDCompositionDevice* dev = ui::Comp();
    RECT wr; GetWindowRect(h, &wr);
    RECT src{ vis.left - wr.left, vis.top - wr.top, vis.right - wr.left, vis.bottom - wr.top };
    UINT dpi = GetDpiForWindow(h); if (!dpi) dpi = 96;
    std::vector<BYTE> px;
    if (!foto::Window(h, src, IsZoomed(h) ? 0.f : 12.f * dpi / 96, px, w, hh) || !w || !hh) return false;
    {   // foto vazia (janela desenhada pela placa de vídeo, escondida: o Windows não entrega o conteúdo)
        size_t lit = 0, n = 0;
        for (size_t i = 0; i + 3 < px.size(); i += 4 * 97, n++) if (px[i] > 8 || px[i + 1] > 8 || px[i + 2] > 8) lit++;
        if (n && lit * 10 < n) { Log(L"ampliar: foto vazia (%zu de %zu pontos com cor)", lit, n); return false; }
    }
    {   // Apps Electron/Chromium entregam a foto com partes transparentes (alfa 0 no conteúdo): no meio da
        // animação dava para ver o fundo através da janela (piscava). Opaca, menos nos cantos arredondados.
        UINT rad = IsZoomed(h) ? 0 : (UINT)(13.f * dpi / 96);
        for (UINT y = 0; y < hh; y++) {
            bool edgeY = y < rad || y + rad >= hh;
            for (UINT x = 0; x < w; x++) {
                if (edgeY && (x < rad || x + rad >= w)) continue;
                px[((size_t)y * w + x) * 4 + 3] = 255;
            }
        }
    }
    int cap = (flags & 3) ? CaptionPx(h, vis) : 0;
    if (cap && bar.a == 0) {  // cor da barra: perto do topo, no meio (acima do texto do título)
        size_t i = ((size_t)std::min<int>(2, cap - 1) * w + w / 2) * 4;
        if (i + 2 < px.size()) bar = D2D1::ColorF(px[i + 2] / 255.f, px[i + 1] / 255.f, px[i] / 255.f, 1);
    } else if (cap && bar.a > 0) {  // pinta a barra da foto com a cor certa (os botões e o título vão por cima)
        BYTE r = (BYTE)(bar.r * 255), g = (BYTE)(bar.g * 255), b = (BYTE)(bar.b * 255);
        float rad = 12.f * dpi / 96;
        for (int y = 0; y < cap && y < (int)hh; y++)
            for (UINT x = 0; x < w; x++) {
                size_t i = ((size_t)y * w + x) * 4;
                if (px[i + 3] == 0) continue;  // fora do canto arredondado
                float a = px[i + 3] / 255.f;
                px[i] = (BYTE)(b * a); px[i + 1] = (BYTE)(g * a); px[i + 2] = (BYTE)(r * a);
            }
        (void)rad;
    }
    if (keep) *keep = px;
    l.surf = ui::CompSurface((int)w, (int)hh, [&](ID2D1DeviceContext* dc) {
        if (ID2D1Bitmap* bm = ui::Bitmap(dc, w, hh, px)) { dc->DrawBitmap(bm, D2D1::RectF(0, 0, (float)w, (float)hh)); bm->Release(); }
        DrawChrome(dc, h, w, hh, px, flags, vis);
    });
    if (!l.surf || FAILED(dev->CreateVisual(&l.v)) || FAILED(dev->CreateScaleTransform(&l.sc)) || FAILED(dev->CreateEffectGroup(&l.fx))) { Free(l); return false; }
    l.v->SetContent(l.surf);
    l.sc->SetCenterX(0.f); l.sc->SetCenterY(0.f);
    l.v->SetTransform(l.sc);
    l.v->SetEffect(l.fx);
    return true;
}

// Movimento da camada do instante t0 (s desde o começo) até o fim, e a opacidade: entra em `fadeIn` s
// (0 = já opaca), fica até a janela de verdade aparecer e some de uma vez
void Animate(Layer& l, const Anim& an, float t0, UINT w, UINT hh, float fadeIn) {
    float vx = (float)GetSystemMetrics(SM_XVIRTUALSCREEN), vy = (float)GetSystemMetrics(SM_YVIRTUALSCREEN);
    float rest = std::max(0.02f, MOVE_S - t0);
    const int K = 28;
    std::vector<float> ox(K), oy(K), sx(K), sy(K);
    for (int k = 0; k < K; k++) {
        float t = t0 + rest * k / (K - 1);
        RECT r = At(an, EaseInOut(std::min(1.f, t / MOVE_S)));
        ox[k] = r.left - vx; oy[k] = r.top - vy; sx[k] = (float)(r.right - r.left) / w; sy[k] = (float)(r.bottom - r.top) / hh;
    }
    if (auto* c = ui::CompCurve(ox, rest)) { l.v->SetOffsetX(c); c->Release(); }
    if (auto* c = ui::CompCurve(oy, rest)) { l.v->SetOffsetY(c); c->Release(); }
    if (auto* c = ui::CompCurve(sx, rest)) { l.sc->SetScaleX(c); c->Release(); }
    if (auto* c = ui::CompCurve(sy, rest)) { l.sc->SetScaleY(c); c->Release(); }
    if (fadeIn <= 0) { l.fx->SetOpacity(1.f); return; }  // fica até a janela avisar que já apareceu (Fade)
    const int F = 24;
    std::vector<float> op(F);
    for (int k = 0; k < F; k++) { float x = (float)k / (F - 1); op[k] = x * x * (3 - 2 * x); }  // entra suave
    if (auto* c = ui::CompCurve(op, fadeIn)) { l.fx->SetOpacity(c); c->Release(); }
}

// Maximizar/restaurar (flags 4): a foto não estica mais (crescia com o texto gigante e torto). Fica no tamanho
// real, presa no canto de cima à esquerda, e o recorte cresce ou encolhe como uma janela sendo redimensionada.
// O que passa da foto é pintado com a cor de fundo da janela (a mais comum na borda direita e na de baixo):
// repetir a última coluna/linha fazia faixas esticadas (a barra roxa do Discord, áreas pretas).
bool Grow(Anim& an) {
    if (an.px.empty() || !an.wa || !an.ha || an.px.size() < (size_t)an.wa * an.ha * 4) return false;
    UINT w = an.wa, hh = an.ha;
    UINT W = std::max<UINT>(w, (UINT)(an.to.right - an.to.left)), H = std::max<UINT>(hh, (UINT)(an.to.bottom - an.to.top));
    UINT r = (UINT)std::ceil(an.rad) + 1;
    UINT cx = w > 2 * r ? w - 1 - r : w - 1, cy = hh > 2 * r ? hh - 1 - r : hh - 1;  // fora do canto arredondado
    BYTE fill[4] = { 0, 0, 0, 255 };
    {   // cor mais comum (agrupada de 8 em 8) nas bordas direita e de baixo, por dentro dos cantos
        std::map<UINT, int> count;
        auto add = [&](UINT x, UINT y) {
            const BYTE* p = &an.px[((size_t)y * w + x) * 4];
            count[((UINT)(p[0] >> 3) << 10) | ((UINT)(p[1] >> 3) << 5) | (p[2] >> 3)]++;
        };
        for (UINT y = r; y + r < hh; y += 2) add(cx, y);
        for (UINT x = r; x + r < w; x += 2) add(x, cy);
        UINT best = 0; int bn = -1;
        for (auto& [k, n] : count) if (n > bn) { bn = n; best = k; }
        fill[0] = (BYTE)(((best >> 10) & 31) * 255 / 31); fill[1] = (BYTE)(((best >> 5) & 31) * 255 / 31); fill[2] = (BYTE)((best & 31) * 255 / 31);
    }
    std::vector<BYTE> big((size_t)W * H * 4);
    for (UINT y = 0; y < H; y++) {
        BYTE* dst = &big[(size_t)y * W * 4];
        UINT copy = 0;
        if (y <= cy) { copy = cx + 1; memcpy(dst, &an.px[(size_t)y * w * 4], (size_t)copy * 4); }
        for (UINT x = copy; x < W; x++) memcpy(dst + (size_t)x * 4, fill, 4);
    }
    IDCompositionSurface* s = ui::CompSurface((int)W, (int)H, [&](ID2D1DeviceContext* dc) {
        if (ID2D1Bitmap* bm = ui::Bitmap(dc, W, H, big)) { dc->DrawBitmap(bm, D2D1::RectF(0, 0, (float)W, (float)H)); bm->Release(); }
    });
    if (!s) return false;
    an.a.v->SetContent(s);
    if (an.a.surf) an.a.surf->Release();
    an.a.surf = s;
    an.px.clear(); an.px.shrink_to_fit();
    return true;
}

float EaseOutZoom(float t) { return 1 - std::pow(1 - t, 3.f); }  // sai rápido e assenta devagar

// A camada `l` (sem esticar, presa no canto de cima à esquerda) com o recorte indo do retângulo de antes ao novo
bool AnimateClip(Layer& l, Anim& an, float dur) {
    if (!l.clip && FAILED(ui::Comp()->CreateRectangleClip(&l.clip))) return false;
    float vx = (float)GetSystemMetrics(SM_XVIRTUALSCREEN), vy = (float)GetSystemMetrics(SM_YVIRTUALSCREEN);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromRect(&an.to, MONITOR_DEFAULTTONEAREST), &mi);
    auto full = [&](const RECT& r) { return r.left <= mi.rcWork.left && r.top <= mi.rcWork.top && r.right >= mi.rcWork.right && r.bottom >= mi.rcWork.bottom; };
    float r0 = full(an.from) ? 0.f : an.rad, r1 = full(an.to) ? 0.f : an.rad;
    const int K = 30;
    std::vector<float> ox(K), oy(K), cw(K), ch(K), rr(K);
    for (int k = 0; k < K; k++) {
        float e = EaseOutZoom((float)k / (K - 1));
        RECT r = At(an, e);
        ox[k] = r.left - vx; oy[k] = r.top - vy; cw[k] = (float)(r.right - r.left); ch[k] = (float)(r.bottom - r.top);
        rr[k] = r0 + (r1 - r0) * e;
    }
    if (auto* c = ui::CompCurve(ox, dur)) { l.v->SetOffsetX(c); c->Release(); }
    if (auto* c = ui::CompCurve(oy, dur)) { l.v->SetOffsetY(c); c->Release(); }
    l.sc->SetScaleX(1.f); l.sc->SetScaleY(1.f);
    l.clip->SetLeft(0.f); l.clip->SetTop(0.f);
    if (auto* c = ui::CompCurve(cw, dur)) { l.clip->SetRight(c); c->Release(); }
    if (auto* c = ui::CompCurve(ch, dur)) { l.clip->SetBottom(c); c->Release(); }
    if (auto* c = ui::CompCurve(rr, dur)) {
        l.clip->SetTopLeftRadiusX(c); l.clip->SetTopLeftRadiusY(c); l.clip->SetTopRightRadiusX(c); l.clip->SetTopRightRadiusY(c);
        l.clip->SetBottomLeftRadiusX(c); l.clip->SetBottomLeftRadiusY(c); l.clip->SetBottomRightRadiusX(c); l.clip->SetBottomRightRadiusY(c);
        c->Release();
    }
    l.v->SetClip(l.clip);
    l.fx->SetOpacity(1.f);
    return true;
}

}  // namespace

bool ampliar::Start(HWND h, RECT from, RECT to, int flags, COLORREF bar) {
    if (anim::reduceMotion) return false;  // Reduzir Movimento: muda de tamanho na hora
    if (!IsWindow(h) || from.right <= from.left || to.right <= to.left || !Ready()) return false;
    IDCompositionDevice* dev = ui::Comp();
    double t0 = anim::Now();
    Anim an;
    an.h = h; an.from = from; an.to = to; an.flags = flags; an.start = t0;
    // a cor da barra vem da DLL (lida da foto, às vezes vinha preta): as duas fotos são pintadas com ela
    an.bar = D2D1::ColorF(GetRValue(bar) / 255.f, GetGValue(bar) / 255.f, GetBValue(bar) / 255.f, 1);
    UINT w = 0, hh = 0;
    if (!MakeLayer(an.a, h, from, flags, w, hh, an.bar, (flags & 4) ? &an.px : nullptr)) { Log(L"ampliar: sem foto da janela"); return false; }
    { UINT dpi = GetDpiForWindow(h); an.rad = 13.f * (dpi ? dpi : 96) / 96; }
    // parada no lugar de agora até a foto nova chegar (a janela muda de tamanho escondida)
    float vx = (float)GetSystemMetrics(SM_XVIRTUALSCREEN), vy = (float)GetSystemMetrics(SM_YVIRTUALSCREEN);
    an.a.v->SetOffsetX(from.left - vx); an.a.v->SetOffsetY(from.top - vy);
    an.wa = w; an.ha = hh;
    g_root->AddVisual(an.a.v, TRUE, nullptr);
    // logo abaixo do Dock (como no Mac, janela nenhuma passa por cima dele, nem a foto deslizando de volta)
    HWND under = dock::Fundo() && IsWindowVisible(dock::Fundo()) ? dock::Fundo() : HWND_TOPMOST;
    SetWindowPos(g_wnd, under, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    dev->Commit();
    DwmFlush();  // a foto já está na tela quando a DLL esconder a janela
    an.end = GetTickCount() + 1600;  // segurança; a foto nova marca o fim de verdade
    g_anims.push_back(an);
    SetTimer(g_wnd, T_CLEAN, 100, nullptr);
    Log(L"ampliar: animando %ldx%ld -> %ldx%ld (foto em %.1f ms)", from.right - from.left, from.bottom - from.top,
        to.right - to.left, to.bottom - to.top, anim::Now() - t0);
    return true;
}

void ampliar::Shown(HWND h) {
    bool any = false;
    for (auto& an : g_anims) if (an.h == h && !an.cut && (an.b.v || an.showAt)) any = true;
    if (!any) return;
    // Chrome/Electron não desenham escondidos: ao reaparecer mostram por uns quadros a imagem velha (no
    // tamanho antigo) até pintar de novo. A foto (igual à janela pronta) fica por cima mais 150 ms.
    for (auto& an : g_anims) if (an.h == h && (an.b.v || an.showAt) && !an.fadeAt && !an.pollStart) {
        if (an.flags & 4) { an.pollStart = GetTickCount(); an.end = an.pollStart + 2500; continue; }  // confere a janela (CheckReady)
        an.fadeAt = GetTickCount() + (an.b.clip || an.a.clip ? 60 : 150);
        an.retakeAt = 0;
    }
    // SetTimer de propósito: T_FADE só confere se a janela de verdade já se desenhou (uma foto pequena por vez,
    // CheckReady); o sumir da foto em si é animação do compositor (Fade), que o DWM faz no ritmo da tela
    SetTimer(g_wnd, T_FADE, 15, nullptr);
}

// Fechar como no Mac: a foto da janela (tirada agora, ela ainda na tela) encolhe uns 5% e some (~0,18 s)
bool ampliar::Fechar(HWND h, LPARAM bar) {
    if (anim::reduceMotion) return false;  // Reduzir Movimento: fecha na hora (medido no Mac)
    if (shell::Setting(L"animar_janelas", L"1") != L"1" || !IsWindow(h) || !IsWindowVisible(h) || IsIconic(h) || !Ready()) return false;
    RECT vis;
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &vis, sizeof(vis)))) GetWindowRect(h, &vis);
    int W = vis.right - vis.left, H = vis.bottom - vis.top;
    if (W < 120 || H < 80) return false;
    double t0 = anim::Now();
    Anim an;
    an.h = h; an.from = vis; an.to = vis; an.flags = 0; an.start = t0;
    if (bar & 0x1000000) {  // a nossa barra (outra janela, fora da foto): desenhada na foto com a cor dela
        an.flags = (int)((bar >> 25) & 3);
        COLORREF c = (COLORREF)(bar & 0xFFFFFF);
        an.bar = D2D1::ColorF(GetRValue(c) / 255.f, GetGValue(c) / 255.f, GetBValue(c) / 255.f, 1);
    }
    UINT w = 0, hh = 0;
    if (!MakeLayer(an.a, h, vis, an.flags, w, hh, an.bar)) return false;
    // Tahoe (guia, seção 44): some com fade e encolhe para 98 % numa cubic-bezier(0.25, 1, 0.5, 1) de 0,21 s
    const bool th = liquid::On();
    const float D = th ? 0.21f : 0.18f;
    const int K = 16;
    float vx = (float)GetSystemMetrics(SM_XVIRTUALSCREEN), vy = (float)GetSystemMetrics(SM_YVIRTUALSCREEN);
    std::vector<float> ox(K), oy(K), sx(K), sy(K), op(K);
    for (int k = 0; k < K; k++) {
        float t = (float)k / (K - 1), e = t * t;  // acelera saindo
        float s = 1 - 0.05f * e;
        if (th) { float q = anim::Bezier(t, 0.25f, 1, 0.5f, 1); s = 1 - 0.02f * q; op[k] = 1 - q; }
        ox[k] = vis.left + W * (1 - s) / 2 - vx; oy[k] = vis.top + H * (1 - s) / 2 - vy;
        sx[k] = W * s / w; sy[k] = H * s / hh;
        if (!th) op[k] = 1 - t * t * (3 - 2 * t);
    }
    if (auto* c = ui::CompCurve(ox, D)) { an.a.v->SetOffsetX(c); c->Release(); }
    if (auto* c = ui::CompCurve(oy, D)) { an.a.v->SetOffsetY(c); c->Release(); }
    if (auto* c = ui::CompCurve(sx, D)) { an.a.sc->SetScaleX(c); c->Release(); }
    if (auto* c = ui::CompCurve(sy, D)) { an.a.sc->SetScaleY(c); c->Release(); }
    if (auto* c = ui::CompCurve(op, D)) { an.a.fx->SetOpacity(c); c->Release(); }
    g_root->AddVisual(an.a.v, TRUE, nullptr);
    HWND under = dock::Fundo() && IsWindowVisible(dock::Fundo()) ? dock::Fundo() : HWND_TOPMOST;
    SetWindowPos(g_wnd, under, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    ui::Comp()->Commit();
    DwmFlush();  // a foto já está na tela quando a janela sumir
    an.cut = true;  // já está sumindo (o Fade não mexe)
    an.end = GetTickCount() + (DWORD)(D * 1000) + 60;
    g_anims.push_back(an);
    SetTimer(g_wnd, T_CLEAN, 100, nullptr);
    Log(L"fechar: %dx%d (foto em %.1f ms)", W, H, anim::Now() - t0);
    return true;
}

void ampliar::SecondPhoto(HWND h) {
    for (auto& an : g_anims) {
        if (an.h != h || an.b.v) continue;
        UINT w = 0, hh = 0;
        if (an.flags & 4) {  // nossos apps (maximizar/restaurar de verdade): o retângulo final é o de agora
            RECT r;
            if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) GetWindowRect(h, &r);
            if (IsZoomed(h)) {  // maximizada: só a área de trabalho aparece (o resto da janela passa da tela)
                MONITORINFO mi{ sizeof(mi) };
                if (GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) IntersectRect(&r, &r, &mi.rcWork);
            }
            an.to = r;
        }
        an.start = anim::Now();  // o movimento começa agora
        if (an.flags & 4) {
            // Maximizar de verdade (nossos apps, Chrome/Electron): anima só a foto de antes, que é sempre
            // válida. Foto da janela escondida não serve: o Chrome não desenha escondido (saía com buracos e
            // com o conteúdo velho, e piscava no fim). A janela reaparece por baixo da foto e a foto só some
            // quando a janela de verdade já está desenhada (CheckReady).
            Log(L"ampliar: animando a foto de antes até %ldx%ld", an.to.right - an.to.left, an.to.bottom - an.to.top);
            float dur = ZOOM_S;
            if (Grow(an) && AnimateClip(an.a, an, dur)) {}
            else { dur = MOVE_S; Animate(an.a, an, 0, an.wa, an.ha, 0); }
            ui::Comp()->Commit();
            g_showFor = h;
            RECT x;
            // restaurar (a janela nova cabe na foto o tempo todo): a janela já pode aparecer por baixo e ir
            // pintando durante o movimento; maximizar: só no fim (senão ela aparecia em volta da foto)
            bool inside = IntersectRect(&x, &an.from, &an.to) && EqualRect(&x, &an.to);
            an.showAt = GetTickCount() + (inside ? 0 : (DWORD)(dur * 1000));
            an.end = an.showAt + 2500;
            an.minHold = GetTickCount() + (DWORD)(dur * 1000);  // a foto fica pelo menos até o fim do movimento
            // maximizar: a janela já vem por baixo um pouco antes do fim (o app começa a pintar enquanto a foto assenta)
            SetTimer(g_wnd, T_SHOW, inside ? 1 : (UINT)(dur * 1000) - 30, nullptr);
            return;
        }
        // Sem esticar e sem misturar as duas fotos (o "fantasma" no Ableton): crescendo, a foto nova (já no
        // layout novo) aparece pelo recorte que cresce; encolhendo, a foto de antes é recortada até o tamanho novo
        bool grow = (LONGLONG)(an.to.right - an.to.left) * (an.to.bottom - an.to.top) >
                    (LONGLONG)(an.from.right - an.from.left) * (an.from.bottom - an.from.top);
        bool clipped = false;
        if (grow) {
            if (MakeLayer(an.b, h, an.to, an.flags, w, hh, an.bar) && AnimateClip(an.b, an, ZOOM_S)) {
                g_root->AddVisual(an.b.v, TRUE, an.a.v);
                g_root->RemoveVisual(an.a.v);  // só a nova (o Fade não traz a de antes de volta)
                clipped = true;
            } else Free(an.b);
        } else clipped = AnimateClip(an.a, an, ZOOM_S);
        if (clipped) {
            Log(L"ampliar: recorte %ldx%ld", an.to.right - an.to.left, an.to.bottom - an.to.top);
            ui::Comp()->Commit();
            an.showAt = GetTickCount() + (DWORD)(ZOOM_S * 1000);
            an.end = an.showAt + 1500;
            g_showFor = h;
            SetTimer(g_wnd, T_SHOW, (UINT)(ZOOM_S * 1000), nullptr);
            return;
        }
        if (MakeLayer(an.b, h, an.to, an.flags, w, hh, an.bar)) {
            Log(L"ampliar: foto nova %ldx%ld", an.to.right - an.to.left, an.to.bottom - an.to.top);
            // as duas fotos andam juntas e a nova aparece por cima (no Mac é assim): sozinha, a foto nova
            // começava esticada (janela pequena ampliada para a tela inteira, texto gigante e torto)
            Animate(an.a, an, 0, an.wa, an.ha, 0);
            Animate(an.b, an, 0, w, hh, MOVE_S * 0.55f);
            g_root->AddVisual(an.b.v, TRUE, an.a.v);  // por cima da foto antiga
        } else {  // sem a foto nova: anima a de antes (no fim o layout muda)
            Log(L"ampliar: sem a foto nova; animando a de antes");
            Animate(an.a, an, 0, an.wa, an.ha, 0);
        }
        ui::Comp()->Commit();
        an.showAt = GetTickCount() + (DWORD)(MOVE_S * 1000);
        an.end = an.showAt + 1500;  // segurança: o Fade marca o fim de verdade
        if (an.b.v) { an.retakeAt = GetTickCount() + 80; an.retakes = 0; SetTimer(g_wnd, T_RETAKE, 20, nullptr); }
        g_showFor = h;
        SetTimer(g_wnd, T_SHOW, (UINT)(MOVE_S * 1000) + 10, nullptr);
        return;
    }
}
