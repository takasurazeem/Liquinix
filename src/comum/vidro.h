// Borda do Liquid Glass desenhada em D2D, igual em todos os apps (menus de contexto, cartões da Central de
// Notificações, blocos da Central de Controle, Spotlight). Como a Apple descreve o material nas versões novas:
// "uma borda escurecida e um brilho especular mais forte" dão a profundidade.
// - borda escura bem fina por fora (separa o vidro do fundo);
// - brilho especular: a luz vem de cima à esquerda; o canto de cima à esquerda acende, o de baixo à direita
//   tem o reflexo (mais fraco), e o meio dos lados quase some. Um fio nítido na beirada e um brilho que
//   entra alguns px no vidro, caindo depressa (a curvatura da borda).
// Só pincéis de degradê (nada de bitmap): funciona em qualquer ID2D1RenderTarget, inclusive o de DC.
#pragma once
#include <d2d1_1.h>
#include <algorithm>
#include <cmath>

namespace vidro {

// clara: o vidro "clear" da Apple (Central de Controle, cartões, botões): sem a borda escura, com um fio branco
// em volta toda; senão o "regular" (menus, avisos): borda escurecida por fora e o fio só nos cantos com luz
// spec: força do brilho especular dos cantos (CASDFKeyFillHighlightEffect, guia macOS26-Theme-Reference, seção 29.1:
// luz principal a −45° e de preenchimento a 135°, 0,5 cada, invisível em repouso e acesa no hover/clique); o fio fino
// em volta (o degradê de borda, seção 29.2) fica sempre. lx, ly: a luz seguindo o ponteiro (0..1 dentro da peça; < 0 =
// canto de cima à esquerda, a luz padrão)
inline void Borda(ID2D1RenderTarget* rt, D2D1_RECT_F r, float rad, float px, bool dark, float k = 1, bool clara = false,
                  float spec = 1, float lx = -1, float ly = -1) {
    if (!rt || r.right - r.left < 4 || r.bottom - r.top < 4) return;
    ID2D1SolidColorBrush* line = nullptr;
    if (!clara && SUCCEEDED(rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, (dark ? 0.30f : 0.10f) * k), &line))) {  // borda escurecida
        float in = 0.5f * px;
        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + in, r.top + in, r.right - in, r.bottom - in), std::max(0.f, rad - in), std::max(0.f, rad - in)), line, px);
        line->Release();
    }
    float w = r.right - r.left, h = r.bottom - r.top;
    float span = std::min(w, h) * 0.5f + std::max(w, h) * 0.18f;  // até onde o brilho de cada canto chega
    float gain = (dark ? 0.75f : 0.95f) * k;
    // reflexo dos cantos bem mais fraco e o fio em volta mais por igual: no macOS 26 a borda é um fio fino quase
    // uniforme (prints do usuário, 16/11/2026); antes o canto de cima à esquerda acendia forte
    // dois degradês: do canto de cima à esquerda (luz) e do de baixo à direita (reflexo), sumindo para o meio
    auto corner = [&](float x0, float y0, float x1, float y1, float a) -> ID2D1LinearGradientBrush* {
        D2D1_GRADIENT_STOP st[3] = { { 0, D2D1::ColorF(1, 1, 1, a) }, { 0.45f, D2D1::ColorF(1, 1, 1, a * 0.28f) }, { 1, D2D1::ColorF(1, 1, 1, 0) } };
        ID2D1GradientStopCollection* sc = nullptr;
        if (FAILED(rt->CreateGradientStopCollection(st, 3, &sc))) return nullptr;
        ID2D1LinearGradientBrush* b = nullptr;
        rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ x0, y0 }, { x1, y1 }), sc, &b);
        sc->Release();
        return b;
    };
    float d = span / 1.41421f;
    // de onde a luz vem: o canto de cima à esquerda, ou o lado do ponteiro (a luz de preenchimento fica no oposto)
    float kx = r.left, ky = r.top, ux = 1, uy = 1;
    if (lx >= 0 && ly >= 0) {
        float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
        float vx = (lx - 0.5f) * w, vy = (ly - 0.5f) * h, len = std::sqrt(vx * vx + vy * vy);
        if (len > 0.5f) { ux = -vx / len; uy = -vy / len; } else { ux = uy = 0.70711f; }
        kx = cx - ux * (w / 2); ky = cy - uy * (h / 2);
    } else { ux = uy = 0.70711f; }
    ID2D1LinearGradientBrush* lit = spec > 0.001f ? corner(kx, ky, kx + ux * d, ky + uy * d, 0.42f * gain * spec) : nullptr;
    float bx = 2 * ((r.left + r.right) / 2) - kx, by = 2 * ((r.top + r.bottom) / 2) - ky;
    ID2D1LinearGradientBrush* back = spec > 0.001f ? corner(bx, by, bx - ux * d, by - uy * d, 0.20f * gain * spec) : nullptr;
    // aros de 1 px de fora para dentro: o primeiro é o fio nítido, o segundo um resto bem fraco do brilho.
    // Eram 5 aros (a borda parecia uma linha grossa); o macOS 26 tem só um fio fino (print do usuário, 16/11/2026)
    const int N = 2;
    for (int i = 0; i < N; i++) {
        float in = (1.0f + i) * px;
        float a = i == 0 ? 0.85f : 0.10f;
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(D2D1::RectF(r.left + in, r.top + in, r.right - in, r.bottom - in), std::max(0.f, rad - in), std::max(0.f, rad - in));
        if (lit) { lit->SetOpacity(a); rt->DrawRoundedRectangle(rr, lit, px); }
        if (back) { back->SetOpacity(a); rt->DrawRoundedRectangle(rr, back, px); }
    }
    {   // fio bem fraco em volta toda (o vidro nunca fica sem borda nos lados)
        ID2D1SolidColorBrush* amb = nullptr;
        if (SUCCEEDED(rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, (clara ? (dark ? 0.24f : 0.42f) : (dark ? 0.16f : 0.22f)) * k), &amb))) {
            float in = 1.0f * px;
            rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + in, r.top + in, r.right - in, r.bottom - in), std::max(0.f, rad - in), std::max(0.f, rad - in)), amb, px);
            amb->Release();
        }
    }
    if (lit) lit->Release();
    if (back) back->Release();
}

// A luz do brilho seguindo o ponteiro, suavizada como no Mac (FocusSpecularHighlightNormalizedPosition com mola de
// resposta 0,3 s e amortecimento 1,0; guia macOS26-Theme-Reference, seção 3.2). Mola crítica integrada por quadro.
// Passo(): avança até o alvo; devolve true enquanto ainda anda (quem chama pede outro quadro).
struct LuzSegue {
    float x = -1, y = -1, vx = 0, vy = 0;
    bool Passo(float tx, float ty, float dtSec) {
        if (tx < 0 || ty < 0) { x = y = -1; vx = vy = 0; return false; }
        if (x < 0 || y < 0) { x = tx; y = ty; vx = vy = 0; return false; }  // entrou agora: começa no ponteiro
        const float w = 2 * 3.14159265f / 0.3f;  // resposta 0,3 s
        dtSec = std::clamp(dtSec, 0.f, 0.05f);
        for (int i = 0; i < 4; i++) {  // 4 subpassos: estável mesmo a 30 quadros/s
            float h = dtSec / 4;
            float ax = w * w * (tx - x) - 2 * w * vx, ay = w * w * (ty - y) - 2 * w * vy;
            vx += ax * h; vy += ay * h; x += vx * h; y += vy * h;
        }
        bool moving = std::fabs(tx - x) > 0.3f || std::fabs(ty - y) > 0.3f || std::fabs(vx) > 2 || std::fabs(vy) > 2;
        if (!moving) { x = tx; y = ty; vx = vy = 0; }
        return moving;
    }
};

// "Gosma" do Liquid Glass (macOS 26): a cápsula e os círculos que saem dela como uma peça só de líquido. Cada
// par vizinho (a ponta da cápsula e o 1º círculo, o 1º e o 2º...) ganha um "pescoço" côncavo que engrossa quando
// eles se encostam e afina até romper quando se afastam; tudo unido num contorno só (sem linhas por dentro).
// Os círculos vêm em ordem da esquerda para a direita, todos com o centro na altura do meio da cápsula.
// Devolve a geometria (quem chamou faz Release) ou nullptr.
// o pescoço rompe quando a folga entre os dois passa disto (como no vídeo: logo que eles se afastam um pouco)
inline float GosmaRompe(float raio) { return 0.28f * raio; }
// broken: bit i = o pescoço antes do círculo i já rompeu (quem anima guarda: rompido não religa)
inline ID2D1Geometry* Gosma(ID2D1Factory* f, D2D1_RECT_F cap, float capR, const D2D1_ELLIPSE* cs, int n, unsigned broken = 0) {
    if (!f) return nullptr;
    ID2D1RoundedRectangleGeometry* rr = nullptr;
    if (FAILED(f->CreateRoundedRectangleGeometry(D2D1::RoundedRect(cap, capR, capR), &rr))) return nullptr;
    ID2D1Geometry* acc = rr;
    auto unite = [&](ID2D1Geometry* g) {
        ID2D1PathGeometry* p = nullptr;
        if (FAILED(f->CreatePathGeometry(&p))) { g->Release(); return; }
        ID2D1GeometrySink* s = nullptr;
        if (SUCCEEDED(p->Open(&s))) {
            acc->CombineWithGeometry(g, D2D1_COMBINE_MODE_UNION, nullptr, 0.2f, s);
            s->Close(); s->Release();
            acc->Release();
            acc = p;
        } else p->Release();
        g->Release();
    };
    float cy = (cap.top + cap.bottom) / 2;
    D2D1_ELLIPSE prev = D2D1::Ellipse({ cap.right - capR, cy }, capR, capR);  // a ponta da cápsula
    for (int i = 0; i < n; i++) {
        const D2D1_ELLIPSE& c = cs[i];
        if (c.radiusX < 0.5f || c.radiusY < 0.5f) continue;
        ID2D1EllipseGeometry* eg = nullptr;
        if (SUCCEEDED(f->CreateEllipseGeometry(c, &eg))) unite(eg);
        float m = std::min(prev.radiusY, c.radiusY);
        float gap = (c.point.x - c.radiusX) - (prev.point.x + prev.radiusX);
        float G = GosmaRompe(m);  // mais longe que isso, o pescoço já rompeu
        if (gap < G && c.point.x > prev.point.x && !(broken & (1u << i))) {
            float k = std::clamp(1 - gap / G, 0.f, 4.f);
            float h = m * std::clamp(0.05f + 0.28f * k, 0.f, 0.92f);  // meia altura do pescoço no meio: fina quando vai romper
            float ax = prev.point.x + prev.radiusX * 0.35f, bx = c.point.x - c.radiusX * 0.35f, mx = (ax + bx) / 2;
            float ay = prev.radiusY * 0.937f, by = c.radiusY * 0.937f;
            ID2D1PathGeometry* bp = nullptr;
            if (bx > ax && SUCCEEDED(f->CreatePathGeometry(&bp))) {
                ID2D1GeometrySink* s = nullptr;
                if (SUCCEEDED(bp->Open(&s))) {
                    s->BeginFigure({ ax, cy - ay }, D2D1_FIGURE_BEGIN_FILLED);
                    s->AddQuadraticBezier(D2D1::QuadraticBezierSegment({ mx, cy - h }, { bx, cy - by }));
                    s->AddLine({ bx, cy + by });
                    s->AddQuadraticBezier(D2D1::QuadraticBezierSegment({ mx, cy + h }, { ax, cy + ay }));
                    s->EndFigure(D2D1_FIGURE_END_CLOSED);
                    s->Close(); s->Release();
                    unite(bp);
                } else bp->Release();
            }
        }
        prev = c;
    }
    return acc;
}

}  // namespace vidro
