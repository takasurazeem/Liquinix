#pragma once
// Barra de controles de vídeo do macOS 27 (QuickTime Player e Quick Look), medida na captura QuickTime do 512 Pixels
// em pt: 458 × 98, cantos 22; linha de cima a 30,5 (alto-falante a 27, volume de 40,5 a 117,5, ⏪ ⏵ ⏩ no meio,
// AirPlay, legendas e » a 124,5/81/26 da direita); linha de baixo a 73 (tempos a 17 das bordas, linha do tempo de
// 113,5 até 113 da direita). Trilhos de 5, botões em pílula de 20 × 15. A cor vem do vidro fosco de trás (Acrilico)
// com um véu quente por cima. Quem usa: src\quicktime e src\shell\quicklook.cpp.
#include <windows.h>
#include <d2d1.h>
#include <d2d1_1.h>
#include <d2d1effects.h>
#pragma comment(lib, "dxguid.lib")
#include <dwrite.h>
#include <dwmapi.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace barravideo {

enum { VOL = 1, BACK, PLAY, FWD, FULL, SCRUB };
struct Hit { int id; D2D1_RECT_F r; };
const float LARGURA = 458, ALTURA = 98;

// desenha a barra em (0, 0, W, H) do alvo; hits recebe os retângulos clicáveis (em pt)
inline void Desenha(ID2D1RenderTarget* rt2, ID2D1SolidColorBrush* b, float W, float H, float volume, bool playing, double pos, double len,
                    IDWriteTextFormat* tfL, IDWriteTextFormat* tfR, std::vector<Hit>& hits) {
    (void)H;
    ID2D1Factory* fac = nullptr; rt2->GetFactory(&fac);
    b->SetColor(D2D1::ColorF(0.36f, 0.34f, 0.31f, 0.52f));
    rt2->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, W, H), 22, 22), b);
    b->SetColor(D2D1::ColorF(1, 1, 1, 0.20f)); rt2->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, W - 0.5f, H - 0.5f), 21.5f, 21.5f), b, 1.f);
    auto fill = [&](auto build) {
        ID2D1PathGeometry* g = nullptr;
        if (!fac || FAILED(fac->CreatePathGeometry(&g))) return;
        ID2D1GeometrySink* sk = nullptr;
        if (SUCCEEDED(g->Open(&sk))) { build(sk); sk->Close(); sk->Release(); }
        rt2->FillGeometry(g, b); g->Release();
    };
    auto stroke = [&](auto build, float wd) {
        ID2D1PathGeometry* g = nullptr;
        if (!fac || FAILED(fac->CreatePathGeometry(&g))) return;
        ID2D1GeometrySink* sk = nullptr;
        if (SUCCEEDED(g->Open(&sk))) { build(sk); sk->Close(); sk->Release(); }
        D2D1_STROKE_STYLE_PROPERTIES sp{ D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND, 10, D2D1_DASH_STYLE_SOLID, 0 };
        ID2D1StrokeStyle* ss = nullptr; fac->CreateStrokeStyle(sp, nullptr, 0, &ss);
        rt2->DrawGeometry(g, b, wd, ss); if (ss) ss->Release(); g->Release();
    };
    auto tri = [&](D2D1_POINT_2F a, D2D1_POINT_2F c, D2D1_POINT_2F d) { fill([&](ID2D1GeometrySink* sk) { sk->BeginFigure(a, D2D1_FIGURE_BEGIN_FILLED); sk->AddLine(c); sk->AddLine(d); sk->EndFigure(D2D1_FIGURE_END_CLOSED); }); };
    auto knob = [&](float x, float y) {  // pílula branca com sombra curta
        for (int k = 3; k >= 1; k--) { b->SetColor(D2D1::ColorF(0, 0, 0, 0.07f)); rt2->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x - 10 - k * 0.4f, y - 7.5f + k * 0.4f, x + 10 + k * 0.4f, y + 7.5f + k * 0.8f), 7.5f + k * 0.4f, 7.5f + k * 0.4f), b); }
        b->SetColor(D2D1::ColorF(0.96f, 0.96f, 0.95f, 1)); rt2->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x - 10, y - 7.5f, x + 10, y + 7.5f), 7.5f, 7.5f), b);
    };
    auto track = [&](D2D1_RECT_F r, float f) {  // trilho de 5: o que passou branco, o resto translúcido
        b->SetColor(D2D1::ColorF(1, 1, 1, 0.26f)); rt2->FillRoundedRectangle(D2D1::RoundedRect(r, 2.5f, 2.5f), b);
        float kx = r.left + 10 + (r.right - r.left - 20) * f;
        b->SetColor(D2D1::ColorF(1, 1, 1, 0.94f)); rt2->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left, r.top, kx, r.bottom), 2.5f, 2.5f), b);
        knob(kx, (r.top + r.bottom) / 2);
    };
    const D2D1_COLOR_F ic = D2D1::ColorF(1, 1, 1, 0.86f);
    float cy = 30.5f, cx = W / 2;
    // alto-falante (corpo cheio) e as ondas conforme o volume
    b->SetColor(ic);
    fill([&](ID2D1GeometrySink* sk) {
        sk->BeginFigure({ 20.f, cy - 2.6f }, D2D1_FIGURE_BEGIN_FILLED);
        sk->AddLine({ 23.2f, cy - 2.6f }); sk->AddLine({ 27.5f, cy - 6.5f }); sk->AddLine({ 27.5f, cy + 6.5f }); sk->AddLine({ 23.2f, cy + 2.6f }); sk->AddLine({ 20.f, cy + 2.6f });
        sk->EndFigure(D2D1_FIGURE_END_CLOSED);
    });
    for (int k = 0; k < 2; k++) {
        if (volume <= (k ? 0.5f : 0.01f)) break;
        float rr = k ? 6.5f : 3.6f, a = k ? 0.95f : 0.85f;
        stroke([&](ID2D1GeometrySink* sk) {
            sk->BeginFigure({ 28.5f + rr * 0.6f, cy - rr * a }, D2D1_FIGURE_BEGIN_HOLLOW);
            sk->AddArc(D2D1::ArcSegment({ 28.5f + rr * 0.6f, cy + rr * a }, { rr, rr }, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
            sk->EndFigure(D2D1_FIGURE_END_OPEN);
        }, 1.4f);
    }
    D2D1_RECT_F vol = D2D1::RectF(40.5f, cy - 2.5f, 117.5f, cy + 2.5f);
    track(vol, volume);
    hits.push_back({ VOL, D2D1::RectF(vol.left - 6, cy - 12, vol.right + 6, cy + 12) });
    // ⏪ ⏵/⏸ ⏩: triângulos cheios
    b->SetColor(ic);
    float bx = cx - 41, fx = cx + 39;
    tri({ bx - 11, cy }, { bx, cy - 6.5f }, { bx, cy + 6.5f }); tri({ bx, cy }, { bx + 11, cy - 6.5f }, { bx + 11, cy + 6.5f });
    tri({ fx + 11, cy }, { fx, cy - 6.5f }, { fx, cy + 6.5f }); tri({ fx, cy }, { fx - 11, cy - 6.5f }, { fx - 11, cy + 6.5f });
    b->SetColor(D2D1::ColorF(1, 1, 1, 0.94f));
    if (playing) {
        rt2->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 8.5f, cy - 12.5f, cx - 2.5f, cy + 12.5f), 1.5f, 1.5f), b);
        rt2->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx + 2.5f, cy - 12.5f, cx + 8.5f, cy + 12.5f), 1.5f, 1.5f), b);
    } else {
        fill([&](ID2D1GeometrySink* sk) {  // ⏵ de 24 × 27 com as pontas levemente arredondadas
            sk->BeginFigure({ cx - 9.f, cy - 12.f }, D2D1_FIGURE_BEGIN_FILLED);
            sk->AddQuadraticBezier(D2D1::QuadraticBezierSegment({ cx - 9.f, cy - 14.5f }, { cx - 6.8f, cy - 13.2f }));
            sk->AddLine({ cx + 13.f, cy - 1.4f });
            sk->AddQuadraticBezier(D2D1::QuadraticBezierSegment({ cx + 15.f, cy }, { cx + 13.f, cy + 1.4f }));
            sk->AddLine({ cx - 6.8f, cy + 13.2f });
            sk->AddQuadraticBezier(D2D1::QuadraticBezierSegment({ cx - 9.f, cy + 14.5f }, { cx - 9.f, cy + 12.f }));
            sk->EndFigure(D2D1_FIGURE_END_CLOSED);
        });
    }
    hits.push_back({ BACK, D2D1::RectF(bx - 16, cy - 14, bx + 16, cy + 14) });
    hits.push_back({ PLAY, D2D1::RectF(cx - 18, cy - 18, cx + 18, cy + 18) });
    hits.push_back({ FWD, D2D1::RectF(fx - 16, cy - 14, fx + 16, cy + 14) });
    // AirPlay (tela com o triângulo), legendas (balão com linhas) e » (mais / tela cheia)
    b->SetColor(D2D1::ColorF(1, 1, 1, 0.78f));
    float ax = W - 124.5f, sx = W - 81, mx = W - 26;
    stroke([&](ID2D1GeometrySink* sk) {
        sk->BeginFigure({ ax - 4.5f, cy + 5.5f }, D2D1_FIGURE_BEGIN_HOLLOW);
        sk->AddLine({ ax - 9.f, cy + 5.5f }); sk->AddLine({ ax - 9.f, cy - 7.f }); sk->AddLine({ ax + 9.f, cy - 7.f }); sk->AddLine({ ax + 9.f, cy + 5.5f }); sk->AddLine({ ax + 4.5f, cy + 5.5f });
        sk->EndFigure(D2D1_FIGURE_END_OPEN);
    }, 1.3f);
    tri({ ax, cy + 1.f }, { ax + 5.f, cy + 8.f }, { ax - 5.f, cy + 8.f });
    stroke([&](ID2D1GeometrySink* sk) {
        sk->BeginFigure({ sx - 6.f, cy + 5.f }, D2D1_FIGURE_BEGIN_HOLLOW);
        sk->AddLine({ sx - 7.f, cy + 5.f });
        sk->AddArc(D2D1::ArcSegment({ sx - 9.f, cy + 3.f }, { 2, 2 }, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        sk->AddLine({ sx - 9.f, cy - 5.f });
        sk->AddArc(D2D1::ArcSegment({ sx - 7.f, cy - 7.f }, { 2, 2 }, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        sk->AddLine({ sx + 7.f, cy - 7.f });
        sk->AddArc(D2D1::ArcSegment({ sx + 9.f, cy - 5.f }, { 2, 2 }, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        sk->AddLine({ sx + 9.f, cy + 3.f });
        sk->AddArc(D2D1::ArcSegment({ sx + 7.f, cy + 5.f }, { 2, 2 }, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        sk->AddLine({ sx - 2.f, cy + 5.f }); sk->AddLine({ sx - 6.f, cy + 8.5f });
        sk->EndFigure(D2D1_FIGURE_END_CLOSED);
    }, 1.3f);
    rt2->DrawLine({ sx - 5.f, cy - 1.5f }, { sx + 5.f, cy - 1.5f }, b, 1.2f);
    rt2->DrawLine({ sx - 5.f, cy + 1.5f }, { sx + 2.f, cy + 1.5f }, b, 1.2f);
    stroke([&](ID2D1GeometrySink* sk) {
        for (float o : { -3.5f, 2.5f }) {
            sk->BeginFigure({ mx + o - 2.5f, cy - 5.5f }, D2D1_FIGURE_BEGIN_HOLLOW);
            sk->AddLine({ mx + o + 2.5f, cy }); sk->AddLine({ mx + o - 2.5f, cy + 5.5f });
            sk->EndFigure(D2D1_FIGURE_END_OPEN);
        }
    }, 1.4f);
    D2D1_RECT_F rfull = D2D1::RectF(mx - 14, cy - 14, mx + 14, cy + 14);
    hits.push_back({ FULL, rfull });
    // linha de baixo: tempo, linha do tempo e duração, no formato do QuickTime (hh:mm:ss:quadros)
    auto tc = [](double v) { v = std::max(0.0, v); int t2 = (int)v, fr = (int)((v - t2) * 30); wchar_t s2[24]; swprintf_s(s2, L"%02d:%02d:%02d:%02d", t2 / 3600, t2 / 60 % 60, t2 % 60, fr); return std::wstring(s2); };
    float by = 73;
    b->SetColor(D2D1::ColorF(1, 1, 1, 0.72f));
    std::wstring a1 = tc(pos), a2 = tc(len);
    rt2->DrawTextW(a1.c_str(), (UINT32)a1.size(), tfL, D2D1::RectF(17, by - 10, 120, by + 10), b);
    rt2->DrawTextW(a2.c_str(), (UINT32)a2.size(), tfR, D2D1::RectF(W - 120, by - 10, W - 17, by + 10), b);
    D2D1_RECT_F bar = D2D1::RectF(113.5f, by - 2.5f, W - 113, by + 2.5f);
    track(bar, len > 0 ? (float)std::clamp(pos / len, 0.0, 1.0) : 0.f);
    hits.push_back({ SCRUB, D2D1::RectF(bar.left, by - 12, bar.right, by + 12) });
    if (fac) fac->Release();
}

// posição do volume a partir do x clicado (o centro da pílula anda de 10 a 10 das pontas do trilho)
inline float VolumeEm(const D2D1_RECT_F& hit, float x) { return std::clamp((x - hit.left - 16) / (hit.right - hit.left - 32), 0.f, 1.f); }
// posição na linha do tempo (0..1) a partir do x clicado
inline float PosicaoEm(const D2D1_RECT_F& hit, float x) { return std::clamp((x - hit.left - 10) / (hit.right - hit.left - 20), 0.f, 1.f); }

// Vidro fosco de verdade, sem o acrílico (que não aceita recorte e deixava uma faixa sem desfoque em volta): copia o
// que a janela src mostra (PrintWindow, pega o vídeo) no retângulo da tela scr, desfoca (Gaussian) e desenha em dst
// (DIP do alvo) recortado no canto rad. Sem src ou se falhar, não desenha nada (fica só a tinta por cima).
inline bool FundoBorrado(ID2D1RenderTarget* rt, HWND src, RECT scr, float sc, D2D1_RECT_F dst, float rad, float sigma = 18.f) {
    if (!rt || !src || !IsWindowVisible(src)) return false;
    RECT wr; GetWindowRect(src, &wr);
    int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
    if (ww <= 0 || wh <= 0) return false;
    // a cópia da janela fica guardada (uma por quadro serve à barra de título e à de controles)
    static HDC mem; static HBITMAP bmp; static void* bits; static int bw, bh; static HWND last; static DWORD at;
    if (!mem) mem = CreateCompatibleDC(nullptr);
    if (ww != bw || wh != bh) {
        if (bmp) DeleteObject(bmp);
        BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), ww, -wh, 1, 32, BI_RGB };
        bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bmp) { bw = bh = 0; return false; }
        SelectObject(mem, bmp); bw = ww; bh = wh; last = nullptr;
    }
    DWORD now = GetTickCount();
    if (last != src || now - at > 25) {
        if (!PrintWindow(src, mem, 2 /*PW_RENDERFULLCONTENT*/)) return false;
        GdiFlush(); last = src; at = now;
    }
    int x0 = std::clamp((int)(scr.left - wr.left), 0, ww), y0 = std::clamp((int)(scr.top - wr.top), 0, wh);
    int x1 = std::clamp((int)(scr.right - wr.left), 0, ww), y1 = std::clamp((int)(scr.bottom - wr.top), 0, wh);
    if (x1 - x0 < 2 || y1 - y0 < 2) return false;
    ID2D1Bitmap* b = nullptr;
    auto bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96 * sc, 96 * sc);
    if (FAILED(rt->CreateBitmap(D2D1::SizeU(x1 - x0, y1 - y0), (BYTE*)bits + ((size_t)y0 * ww + x0) * 4, ww * 4, bp, &b)) || !b) return false;
    D2D1_POINT_2F at2{ dst.left + (x0 - (scr.left - wr.left)) / sc, dst.top + (y0 - (scr.top - wr.top)) / sc };
    ID2D1Factory* fac = nullptr; rt->GetFactory(&fac);
    ID2D1RoundedRectangleGeometry* rg = nullptr;
    if (fac) fac->CreateRoundedRectangleGeometry(D2D1::RoundedRect(dst, rad, rad), &rg);
    if (rg) rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), rg), nullptr);
    bool ok = false;
    ID2D1DeviceContext* dc = nullptr;
    if (SUCCEEDED(rt->QueryInterface(IID_PPV_ARGS(&dc))) && dc) {
        ID2D1Effect* fx = nullptr;
        if (SUCCEEDED(dc->CreateEffect(CLSID_D2D1GaussianBlur, &fx)) && fx) {
            fx->SetInput(0, b);
            fx->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, sigma);
            fx->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
            dc->DrawImage(fx, at2);
            fx->Release(); ok = true;
        }
        dc->Release();
    }
    if (!ok) {  // sem efeitos: reduz e amplia de volta (borra também)
        ID2D1BitmapRenderTarget* reduz = nullptr;
        auto sz = b->GetSize();
        if (SUCCEEDED(rt->CreateCompatibleRenderTarget(D2D1::SizeF(std::max(1.f, sz.width / 10), std::max(1.f, sz.height / 10)), &reduz)) && reduz) {
            reduz->BeginDraw();
            reduz->DrawBitmap(b, D2D1::RectF(0, 0, std::max(1.f, sz.width / 10), std::max(1.f, sz.height / 10)), 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            reduz->EndDraw();
            ID2D1Bitmap* sb = nullptr; reduz->GetBitmap(&sb);
            if (sb) { rt->DrawBitmap(sb, D2D1::RectF(at2.x, at2.y, at2.x + sz.width, at2.y + sz.height), 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR); sb->Release(); ok = true; }
            reduz->Release();
        }
    }
    if (rg) { rt->PopLayer(); rg->Release(); }
    if (fac) fac->Release();
    b->Release();
    return ok;
}

// O vidro fosco de trás: uma janela com o acrílico do Windows (desfoca o que está atrás). O acrílico ignora recorte
// (SetWindowRgn); quem usa a põe 4,5 pt para dentro da barra, com o canto redondo de 8 do Windows.
inline LRESULT CALLBACK AcrilicoProc(HWND h, UINT m, WPARAM w, LPARAM l) { if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE; return DefWindowProcW(h, m, w, l); }
inline HWND Acrilico(HINSTANCE hi, HWND owner, DWORD tintAABBGGRR = 0x18403C38, DWORD corner = 2 /*DWMWCP_ROUND*/) {
    static bool reg = false;
    if (!reg) {
        WNDCLASSEXW gc{ sizeof(gc) };
        gc.lpfnWndProc = AcrilicoProc; gc.hInstance = hi; gc.lpszClassName = L"LiquinixVidroAcrilico"; gc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        RegisterClassExW(&gc);
        reg = true;
    }
    HWND g = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, L"LiquinixVidroAcrilico", L"", WS_POPUP, 0, 0, 10, 10, owner, nullptr, hi, nullptr);
    if (!g) return nullptr;
    COLORREF none = 0xFFFFFFFE;
    DwmSetWindowAttribute(g, 33, &corner, sizeof(corner)); DwmSetWindowAttribute(g, 34, &none, sizeof(none));
    struct ACCENT_POLICY { int state, flags; DWORD gradient; int anim; };
    struct WCA_DATA { int attr; void* data; SIZE_T size; };
    using Fn = BOOL(WINAPI*)(HWND, WCA_DATA*);
    auto fn = (Fn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute");
    ACCENT_POLICY ap{ 4 /*ACCENT_ENABLE_ACRYLICBLURBEHIND*/, 0, tintAABBGGRR, 0 };
    WCA_DATA d{ 19 /*WCA_ACCENT_POLICY*/, &ap, sizeof(ap) };
    if (!fn || !fn(g, &d)) { DestroyWindow(g); return nullptr; }
    return g;
}

}  // namespace barravideo
