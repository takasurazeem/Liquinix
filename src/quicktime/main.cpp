// QuickTime Player (Mac): o vídeo numa janela escura do tamanho dele e, por cima, a barra de controles flutuante
// do QuickTime (volume, voltar 15 s, tocar/pausar, avançar 15 s, tempo, barra de progresso e tela cheia), que
// aparece com o mouse e some sozinha. O vídeo é do Media Foundation (MFPlay), que toca o que o Windows toca.
// Espaço tocar/pausar; ← → 5 s; ↑ ↓ volume; Ctrl+F ou duplo clique tela cheia; Esc sai; Ctrl+O abrir; soltar arquivo.
// Uma janela por vídeo, como no Mac.
#include "../comum/app2d.h"
#include "../comum/barra_video.h"
#include <mfplay.h>
#include <mfapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <propvarutil.h>
#pragma comment(lib, "mfplay.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "propsys.lib")

using namespace app2d;

namespace {

const UINT WM_MFEVENT = WM_APP + 5;
HWND g_video, g_bar;
IMFPMediaPlayer* g_mp;
std::wstring g_path;
bool g_full = false, g_ended = false, g_ready = false, g_noVideo = false;  // noVideo: só som (sem o codec da imagem)
WINDOWPLACEMENT g_place{ sizeof(g_place) };
float g_volume = 1.0f;
DWORD g_lastMove = 0;
bool g_barShown = false, g_scrubbing = false;
IDWriteTextFormat *g_tfTime, *g_tfIcon, *g_tfIconBig, *g_tfTitle;
struct BarHit { int id; D2D1_RECT_F r; };
std::vector<BarHit> g_hits;
enum { B_VOL = 1, B_BACK, B_PLAY, B_FWD, B_FULL, B_SCRUB };
const float BAR_W = 520, BAR_H = 76;
// macOS 27 (captura QuickTime do 512 Pixels, 2x, medida em pt): 454 × 98, cantos 22, vidro claro e quente sobre o
// vídeo; em cima o volume com o botão redondo, ⏪ ⏯ ⏩ no meio e AirPlay, legendas e "»" à direita; embaixo os dois
// tempos e a linha do tempo com o botão redondo, a 20 do fundo da janela
const float GG_W = 458, GG_H = 98, GG_TOP = 32;
// a barra de controles tem vidro fosco de verdade: uma janela com o acrílico do Windows (desfoca o vídeo) logo
// abaixo da barra em camadas, recortada nos mesmos cantos de 22
HWND g_barBack;
// macOS 27: a barra de título é vidro fosco por cima do vídeo (o vídeo ocupa a janela toda). g_topBack = acrílico
// (desfoca), g_top = camada com a tinta escura, os cantos de 16 da janela, o ícone do documento e o nome. As duas ficam
// logo acima da janela e abaixo dos três botões da LiquinixWindow.dll; o mouse passa direto (arrasta pela janela).
HWND g_topBack, g_top;
IDWriteTextFormat *g_tfTimeL, *g_tfTimeR, *g_tfTop;
bool Unified() { return semaforo::Aqua() && !g_full; }  // macOS 27: barra de título de 32 desenhada pelo app

// ---- MFPlay
class Callback : public IMFPMediaPlayerCallback {
    LONG ref = 1;
public:
    STDMETHODIMP QueryInterface(REFIID riid, void** p) override {
        if (riid == IID_IUnknown || riid == __uuidof(IMFPMediaPlayerCallback)) { *p = this; AddRef(); return S_OK; }
        *p = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref); }
    STDMETHODIMP_(ULONG) Release() override { LONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
    void STDMETHODCALLTYPE OnMediaPlayerEvent(MFP_EVENT_HEADER* e) override {
        if (!e) return;
        if (e->eEventType == MFP_EVENT_TYPE_MEDIAITEM_SET) PostMessageW(wnd, WM_MFEVENT, 1, FAILED(e->hrEvent));
        else if (e->eEventType == MFP_EVENT_TYPE_PLAYBACK_ENDED) PostMessageW(wnd, WM_MFEVENT, 2, 0);
        else if (e->eEventType == MFP_EVENT_TYPE_ERROR) PostMessageW(wnd, WM_MFEVENT, 3, e->hrEvent);
    }
};
Callback* g_cb;

double Pos() {
    if (!g_mp) return 0;
    PROPVARIANT v; PropVariantInit(&v);
    double s = 0;
    if (SUCCEEDED(g_mp->GetPosition(MFP_POSITIONTYPE_100NS, &v)) && v.vt == VT_I8) s = v.hVal.QuadPart / 1e7;
    PropVariantClear(&v);
    return s;
}
double Len() {
    if (!g_mp) return 0;
    PROPVARIANT v; PropVariantInit(&v);
    double s = 0;
    if (SUCCEEDED(g_mp->GetDuration(MFP_POSITIONTYPE_100NS, &v)) && v.vt == VT_UI8) s = v.uhVal.QuadPart / 1e7;
    PropVariantClear(&v);
    return s;
}
void Seek(double s) {
    if (!g_mp) return;
    double len = Len();
    s = std::clamp(s, 0.0, len > 0 ? len - 0.05 : s);
    PROPVARIANT v; PropVariantInit(&v); v.vt = VT_I8; v.hVal.QuadPart = (LONGLONG)(s * 1e7);
    g_mp->SetPosition(MFP_POSITIONTYPE_100NS, &v);
    g_ended = false;
}
bool Playing() { MFP_MEDIAPLAYER_STATE st = MFP_MEDIAPLAYER_STATE_EMPTY; if (g_mp) g_mp->GetState(&st); return st == MFP_MEDIAPLAYER_STATE_PLAYING; }
void Toggle() {
    if (!g_mp) return;
    if (Playing()) g_mp->Pause();
    else { if (g_ended) { Seek(0); } g_mp->Play(); }
}
std::wstring Hms(double s) {
    int t = (int)std::max(0.0, s);
    wchar_t b[24];
    if (t >= 3600) swprintf_s(b, L"%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60); else swprintf_s(b, L"%d:%02d", t / 60, t % 60);
    return b;
}

// ---- a barra flutuante (janela em camadas, alfa por pixel, por cima do vídeo)
void PaintBar() {
    if (!g_bar) return;
    float s = GetDpiForWindow(wnd) / 96.f;
    RECT cr; GetClientRect(wnd, &cr);
    POINT org{ 0, 0 }; ClientToScreen(wnd, &org);
    const bool gg = semaforo::Aqua();
    int w = (int)std::lround(std::min(gg ? GG_W : BAR_W, cr.right / s - 40) * s), h = (int)std::lround((gg ? GG_H : BAR_H) * s);
    if (w < 200) { ShowWindow(g_bar, SW_HIDE); return; }
    int x = org.x + (cr.right - w) / 2, y = org.y + cr.bottom - h - (int)std::lround(24 * s);
    HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
    void* bits = nullptr; HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    ID2D1DCRenderTarget* rt2 = nullptr;
    auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96 * s, 96 * s);
    g_hits.clear();
    if (SUCCEEDED(d2d->CreateDCRenderTarget(&props, &rt2))) {
        RECT br2{ 0, 0, w, h }; rt2->BindDC(mem, &br2);
        rt2->BeginDraw(); rt2->Clear(D2D1::ColorF(0, 0, 0, 0));
        ID2D1SolidColorBrush* b = nullptr; rt2->CreateSolidColorBrush(D2D1::ColorF(0.12f, 0.12f, 0.13f, 0.78f), &b);
        float W = w / s, H = h / s;
        if (gg && b) {
            // macOS 27 (captura QuickTime, medida em pt): 458 × 98, cantos 22, a 20 do fundo; linha de cima a 30,5
            // (alto-falante a 27, volume de 40,5 a 117,5, ⏪ ⏵ ⏩ no meio, AirPlay, legendas e » a 124,5/81/26 da
            // direita); linha de baixo a 73 (tempos de 13 a 17 das bordas, linha do tempo de 113,5 a 344,5). Trilhos
            // de 5, botões em pílula de 20 × 15. A cor vem do vidro (fosco atrás) com um véu quente por cima.
            {
                std::vector<barravideo::Hit> hv;
                int by0 = org.y + cr.bottom - h - (int)std::lround(20 * s);
                barravideo::FundoBorrado(rt2, g_video, RECT{ x, by0, x + w, by0 + h }, s, D2D1::RectF(0, 0, W, H), 22);  // o vídeo desfocado atrás
                barravideo::Desenha(rt2, b, W, H, g_volume, Playing(), Pos(), Len(), g_tfTimeL, g_tfTimeR, hv);
                for (auto& x : hv) g_hits.push_back({ x.id, x.r });
            }
            b->Release(); rt2->EndDraw(); rt2->Release();
            POINT dst{ x, org.y + cr.bottom - h - (int)std::lround(20 * s) }, src{ 0, 0 }; SIZE szz{ w, h };
            BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            UpdateLayeredWindow(g_bar, screen, &dst, &szz, mem, &src, 0, &bf, ULW_ALPHA);
            SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(nullptr, screen);
            if (g_barBack) {  // o vidro fosco logo abaixo. O acrílico ignora recorte (SetWindowRgn): fica 4,5 pt para
                // dentro, com o canto redondo de 8 do Windows, que assim cabe inteiro dentro do canto de 22 da barra
                int in = (int)std::lround(4.5f * s);
                SetWindowPos(g_barBack, g_bar, dst.x + in, dst.y + in, w - 2 * in, h - 2 * in, SWP_NOACTIVATE | (g_barShown ? SWP_SHOWWINDOW : 0));
            }
            return;
        }
        rt2->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, W - 0.5f, H - 0.5f), 14, 14), b);
        b->SetColor(D2D1::ColorF(1, 1, 1, 0.14f)); rt2->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, W - 0.5f, H - 0.5f), 14, 14), b, 1);
        auto txt = [&](const std::wstring& t, IDWriteTextFormat* f, D2D1_RECT_F r, float a) { b->SetColor(D2D1::ColorF(1, 1, 1, a)); rt2->DrawTextW(t.c_str(), (UINT32)t.size(), f, r, b); };
        // linha de cima: volume à esquerda, ⏪ ⏯ ⏩ no meio, tela cheia à direita
        float cy = 24, cx = W / 2;
        wchar_t vg[2] = { g_volume <= 0.01f ? (wchar_t)0xE74F : (wchar_t)0xE767, 0 };
        txt(vg, g_tfIcon, D2D1::RectF(10, cy - 12, 34, cy + 12), 0.85f);
        D2D1_RECT_F vol = D2D1::RectF(38, cy - 2, 108, cy + 2);
        b->SetColor(D2D1::ColorF(1, 1, 1, 0.25f)); rt2->FillRoundedRectangle(D2D1::RoundedRect(vol, 2, 2), b);
        b->SetColor(D2D1::ColorF(1, 1, 1, 0.9f)); rt2->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(vol.left, vol.top, vol.left + (vol.right - vol.left) * g_volume, vol.bottom), 2, 2), b);
        g_hits.push_back({ B_VOL, D2D1::RectF(vol.left - 4, cy - 10, vol.right + 4, cy + 10) });
        wchar_t back[2] = { 0xED3C, 0 }, fwd[2] = { 0xED3D, 0 }, play[2] = { Playing() ? (wchar_t)0xE769 : (wchar_t)0xE768, 0 }, full[2] = { g_full ? (wchar_t)0xE73F : (wchar_t)0xE740, 0 };
        D2D1_RECT_F rb = D2D1::RectF(cx - 70, cy - 14, cx - 38, cy + 14), rp = D2D1::RectF(cx - 18, cy - 16, cx + 18, cy + 16), rf = D2D1::RectF(cx + 38, cy - 14, cx + 70, cy + 14), rfull = D2D1::RectF(W - 40, cy - 12, W - 12, cy + 12);
        txt(back, g_tfIcon, rb, 0.9f); txt(play, g_tfIconBig, rp, 1); txt(fwd, g_tfIcon, rf, 0.9f); txt(full, g_tfIcon, rfull, 0.85f);
        g_hits.push_back({ B_BACK, rb }); g_hits.push_back({ B_PLAY, rp }); g_hits.push_back({ B_FWD, rf }); g_hits.push_back({ B_FULL, rfull });
        // linha de baixo: tempo, barra, tempo que falta
        double pos = Pos(), len = Len();
        float by = 56;
        txt(Hms(pos), g_tfTime, D2D1::RectF(10, by - 9, 60, by + 9), 0.8f);
        txt(L"-" + Hms(len - pos), g_tfTime, D2D1::RectF(W - 64, by - 9, W - 10, by + 9), 0.8f);
        D2D1_RECT_F bar = D2D1::RectF(64, by - 2, W - 68, by + 2);
        b->SetColor(D2D1::ColorF(1, 1, 1, 0.25f)); rt2->FillRoundedRectangle(D2D1::RoundedRect(bar, 2, 2), b);
        float f = len > 0 ? (float)std::clamp(pos / len, 0.0, 1.0) : 0;
        b->SetColor(D2D1::ColorF(1, 1, 1, 0.9f)); rt2->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bar.left, bar.top, bar.left + (bar.right - bar.left) * f, bar.bottom), 2, 2), b);
        g_hits.push_back({ B_SCRUB, D2D1::RectF(bar.left, by - 10, bar.right, by + 10) });
        b->Release(); rt2->EndDraw(); rt2->Release();
    }
    POINT dst{ x, y }, src{ 0, 0 }; SIZE sz{ w, h };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_bar, screen, &dst, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(nullptr, screen);
}
void PaintTop() {
    if (!g_top) return;
    bool on = Unified() && IsWindowVisible(wnd) && !IsIconic(wnd);
    if (!on) { ShowWindow(g_top, SW_HIDE); if (g_topBack) ShowWindow(g_topBack, SW_HIDE); return; }
    float s = GetDpiForWindow(wnd) / 96.f;
    RECT cr; GetClientRect(wnd, &cr);
    POINT org{ 0, 0 }; ClientToScreen(wnd, &org);
    int w = cr.right, h = (int)std::lround(GG_TOP * s);
    if (w <= 0) return;
    HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
    BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
    void* bits = nullptr; HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    ID2D1DCRenderTarget* rt2 = nullptr;
    auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96 * s, 96 * s);
    if (SUCCEEDED(d2d->CreateDCRenderTarget(&props, &rt2))) {
        RECT rb{ 0, 0, w, h }; rt2->BindDC(mem, &rb);
        rt2->BeginDraw(); rt2->Clear(D2D1::ColorF(0, 0, 0, 0));
        ID2D1SolidColorBrush* b = nullptr; rt2->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0), &b);
        if (b) {
            float W = w / s, R = IsZoomed(wnd) ? 0.f : 16.f;
            // tinta cinza-escura (medida: 62,63,64 sobre o preto) com os cantos de cima da janela
            rt2->PushAxisAlignedClip(D2D1::RectF(0, 0, W, GG_TOP), D2D1_ANTIALIAS_MODE_ALIASED);
            barravideo::FundoBorrado(rt2, g_video, RECT{ org.x, org.y, org.x + w, org.y + h }, s, D2D1::RectF(0, 0, W, GG_TOP + R + 2), R);  // o vídeo desfocado
            b->SetColor(D2D1::ColorF(0.235f, 0.24f, 0.25f, 0.62f));
            rt2->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, W, GG_TOP + R + 2), R, R), b);
            rt2->PopAxisAlignedClip();
            b->SetColor(D2D1::ColorF(0, 0, 0, 0.18f));
            rt2->FillRectangle(D2D1::RectF(0, GG_TOP - 0.5f, W, GG_TOP), b);
            // folha com o canto dobrado (10 × 13) a 84 e o nome em negrito de 13 a 101,5
            float cy = GG_TOP / 2;
            b->SetColor(D2D1::ColorF(0.93f, 0.93f, 0.94f, 1));
            rt2->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(84, cy - 6.5f, 94, cy + 6.5f), 1.2f, 1.2f), b);
            b->SetColor(D2D1::ColorF(0.6f, 0.6f, 0.62f, 1));
            rt2->FillRectangle(D2D1::RectF(90.5f, cy - 6.5f, 94, cy - 3.f), b);
            wchar_t ttl[512] = {}; GetWindowTextW(wnd, ttl, 512);
            std::wstring name = ttl;
            size_t dot = name.find_last_of(L'.'); if (dot != std::wstring::npos && dot > 0) name = name.substr(0, dot);
            b->SetColor(D2D1::ColorF(1, 1, 1, 0.92f));
            if (g_tfTop) rt2->DrawTextW(name.c_str(), (UINT32)name.size(), g_tfTop, D2D1::RectF(101.5f, 0, W - 12, GG_TOP), b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            b->Release();
        }
        rt2->EndDraw(); rt2->Release();
    }
    POINT src{ 0, 0 }; SIZE sz{ w, h };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_top, screen, &org, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, old); DeleteObject(bmp); DeleteDC(mem); ReleaseDC(nullptr, screen);
    // logo acima da janela (abaixo de tudo o que já estava por cima dela, como os três botões da DLL)
    HWND above = GetWindow(wnd, GW_HWNDPREV);
    if (g_topBack) {
        int in = IsZoomed(wnd) ? 0 : (int)std::lround(12 * s);  // o acrílico não aceita recorte: fica dentro dos cantos de 16
        if (above == g_top || above == g_topBack) above = GetWindow(above, GW_HWNDPREV);
        SetWindowPos(g_topBack, above ? above : HWND_TOP, org.x + in, org.y, w - 2 * in, h, SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
        HWND a2 = GetWindow(g_topBack, GW_HWNDPREV);
        if (a2 == g_top) a2 = GetWindow(a2, GW_HWNDPREV);
        SetWindowPos(g_top, a2 ? a2 : HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
    } else {
        if (above == g_top) above = GetWindow(above, GW_HWNDPREV);
        SetWindowPos(g_top, above ? above : HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
    }
}
void ShowBar(bool on) {
    if (!g_bar) return;
    if (on && g_ready) {
        PaintBar();
        if (!g_barShown) {
            SetWindowPos(g_bar, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW); g_barShown = true;
            if (g_barBack) SetWindowPos(g_barBack, g_bar, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
    }
    else if (!on && g_barShown) { ShowWindow(g_bar, SW_HIDE); if (g_barBack) ShowWindow(g_barBack, SW_HIDE); g_barShown = false; }
}
void Poke() { g_lastMove = GetTickCount(); ShowBar(true); }

void ScrubTo(float x) {
    for (auto& h : g_hits) if (h.id == B_SCRUB) { double f = semaforo::Aqua() ? barravideo::PosicaoEm(h.r, x) : std::clamp((x - h.r.left) / (h.r.right - h.r.left), 0.f, 1.f); Seek(f * Len()); }
}
void BarAction(int id, float x) {
    switch (id) {
    case B_VOL: for (auto& h : g_hits) if (h.id == B_VOL) {
            g_volume = semaforo::Aqua() ? barravideo::VolumeEm(h.r, x) : std::clamp((x - h.r.left - 4) / (h.r.right - h.r.left - 8), 0.f, 1.f);
            if (g_mp) g_mp->SetVolume(g_volume);
        } break;
    case B_BACK: Seek(Pos() - 15); break;
    case B_FWD: Seek(Pos() + 15); break;
    case B_PLAY: Toggle(); break;
    case B_FULL: PostMessageW(wnd, WM_KEYDOWN, 'F', 1); break;
    case B_SCRUB: g_scrubbing = true; SetCapture(g_bar); ScrubTo(x); break;
    }
    PaintBar();
}
LRESULT CALLBACK BarProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    float s = GetDpiForWindow(h) / 96.f;
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_MOUSEMOVE: g_lastMove = GetTickCount(); if (g_scrubbing) { ScrubTo(GET_X_LPARAM(l) / s); PaintBar(); } return 0;
    case WM_LBUTTONDOWN: { float x = GET_X_LPARAM(l) / s, y = GET_Y_LPARAM(l) / s; for (auto& ht : g_hits) if (In(ht.r, x, y)) { BarAction(ht.id, x); break; } return 0; }
    case WM_LBUTTONUP: if (g_scrubbing) { g_scrubbing = false; ReleaseCapture(); } return 0;
    case WM_LBUTTONDBLCLK: return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---- janela
void FitWindowToVideo() {
    if (!g_mp || g_full) return;
    SIZE vs{}, ar{};
    if (FAILED(g_mp->GetNativeVideoSize(&vs, &ar)) || !vs.cx || !vs.cy) return;
    // a proporção da imagem (pixels não quadrados, vídeo de celular): a altura que ela ocupa de verdade
    if (ar.cx > 0 && ar.cy > 0) vs.cy = std::max<LONG>(1, (LONG)std::lround((double)vs.cx * ar.cy / ar.cx));
    MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST), &mi);
    float maxW = (mi.rcWork.right - mi.rcWork.left) * 0.72f, maxH = (mi.rcWork.bottom - mi.rcWork.top) * 0.78f;
    float k = std::min({ 1.f, maxW / vs.cx, maxH / vs.cy });
    RECT r{ 0, 0, (LONG)(vs.cx * k), (LONG)(vs.cy * k) };
    if (Unified()) {  // macOS 27: a barra de 32 do app no lugar da barra de título (as bordas continuam)
        AdjustWindowRectExForDpi(&r, WS_THICKFRAME, FALSE, 0, GetDpiForWindow(wnd));
        r.top = 0;
    } else
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, GetDpiForWindow(wnd));
    int W = r.right - r.left, H = r.bottom - r.top;
    SetWindowPos(wnd, nullptr, mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - W) / 2, mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - H) / 2, W, H, SWP_NOZORDER);
}

void Open(const std::wstring& path) {
    if (g_mp) { g_mp->Shutdown(); g_mp->Release(); g_mp = nullptr; }
    g_path = path; g_ready = false; g_ended = false;
    std::wstring name = path.substr(path.find_last_of(L"\\/") + 1);
    SetWindowTextW(wnd, name.c_str());
    if (!g_cb) g_cb = new Callback;
    HRESULT hr = MFPCreateMediaPlayer(path.c_str(), TRUE, 0, g_cb, g_video, &g_mp);
    if (FAILED(hr)) { MessageBoxW(wnd, Tr(L"Não foi possível abrir este vídeo."), L"QuickTime Player", MB_OK | MB_ICONERROR); return; }
    g_mp->SetVolume(g_volume);
    g_mp->SetBorderColor(RGB(0, 0, 0));  // a sobra em volta do vídeo (vertical numa janela larga): preta, não lixo da tela
    SHAddToRecentDocs(SHARD_PATHW, path.c_str());
}

void OpenDialog() {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    COMDLG_FILTERSPEC f[] = { { Tr(L"Filmes"), L"*.mp4;*.mov;*.m4v;*.mkv;*.avi;*.wmv;*.webm;*.mp3;*.m4a;*.wav" }, { Tr(L"Todos os arquivos"), L"*.*" } };
    d->SetFileTypes(2, f);
    if (SUCCEEDED(d->Show(wnd))) { IShellItem* it = nullptr; if (SUCCEEDED(d->GetResult(&it))) { PWSTR p = nullptr; if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
        if (g_path.empty()) Open(p); else { wchar_t me[MAX_PATH]; GetModuleFileNameW(nullptr, me, MAX_PATH); ShellExecuteW(nullptr, L"open", me, (L"\"" + std::wstring(p) + L"\"").c_str(), nullptr, SW_SHOWNORMAL); }
        CoTaskMemFree(p); } it->Release(); } }
    d->Release();
}

void SetFull(bool on) {
    if (on == g_full) return;
    LONG st = GetWindowLongW(wnd, GWL_STYLE);
    if (on) {
        GetWindowPlacement(wnd, &g_place);
        MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(MonitorFromWindow(wnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetWindowLongW(wnd, GWL_STYLE, st & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(wnd, GWL_STYLE, st | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(wnd, &g_place);
        SetWindowPos(wnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    }
    g_full = on;
    if (semaforo::Aqua()) {  // tela cheia: sem a barra de título de 32 (nem arrastar por ela)
        BAR = on ? 0.f : GG_TOP;
        RECT cr; GetClientRect(wnd, &cr);
        SendMessageW(wnd, WM_SIZE, 0, MAKELPARAM(cr.right, cr.bottom));
        InvalidateRect(wnd, nullptr, TRUE);
        PaintTop();
    }
    Poke();
}

LRESULT CALLBACK VideoProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        if (g_mp && g_ready && !g_noVideo) g_mp->UpdateVideo();
        else {
            FillRect(dc, &ps.rcPaint, (HBRUSH)GetStockObject(BLACK_BRUSH));
            if (g_noVideo) {  // tocando só o som: o Windows não tem o codec da imagem (HEVC, ProRes...)
                RECT r; GetClientRect(h, &r); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, RGB(200, 200, 200));
                NONCLIENTMETRICSW nm{ sizeof(nm) }; SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(nm), &nm, 0); HFONT fo = CreateFontIndirectW(&nm.lfMessageFont); HGDIOBJ old = SelectObject(dc, fo);
                DrawTextW(dc, Tr(L"Só o som deste vídeo pode ser tocado: o Windows não tem o codec da imagem (por exemplo HEVC). Instale as Extensões de Vídeo HEVC da Microsoft Store."), -1, &r, DT_CENTER | DT_VCENTER | DT_WORDBREAK);
                SelectObject(dc, old); DeleteObject(fo);
            }
        }
        EndPaint(h, &ps); return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_NCHITTEST:
        if (Unified()) {
            POINT p{ GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ScreenToClient(h, &p);
            if (p.y < (int)std::lround(GG_TOP * GetDpiForWindow(h) / 96.f)) return HTTRANSPARENT;
        }
        break;
    case WM_MOUSEMOVE: { static LPARAM last; if (l != last) { last = l; Poke(); } return 0; }
    case WM_LBUTTONDOWN: SetFocus(wnd); return 0;
    case WM_LBUTTONDBLCLK: SetFull(!g_full); return 0;
    case WM_DROPFILES: return SendMessageW(wnd, m, w, l);
    }
    return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_SIZE: if (g_video) {
            int top = 0;  // macOS 27: o vídeo ocupa a janela toda, a barra de título é vidro por cima
            SetWindowPos(g_video, nullptr, 0, top, LOWORD(l), std::max(0, (int)HIWORD(l) - top), SWP_NOZORDER);
            if (g_mp) g_mp->UpdateVideo();
        }
        PaintTop();
        if (g_barShown) PaintBar(); return 0;
    case WM_PAINT: {  // macOS 27: a barra de título escura com o ícone do documento e o nome em negrito depois dos botões
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps);
        RECT cr; GetClientRect(h, &cr);
        float s = GetDpiForWindow(h) / 96.f;
        int top = 0;  // a barra de título agora é a janela de vidro g_top (PaintTop)
        if (top) {
            ID2D1DCRenderTarget* rt2 = nullptr;
            auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96 * s, 96 * s);
            if (SUCCEEDED(d2d->CreateDCRenderTarget(&props, &rt2))) {
                RECT rb{ 0, 0, cr.right, top }; rt2->BindDC(dc, &rb);
                rt2->BeginDraw();
                rt2->Clear(D2D1::ColorF(0.235f, 0.24f, 0.25f));
                ID2D1SolidColorBrush* b = nullptr; rt2->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.9f), &b);
                if (b) {
                    float W = cr.right / s, cy = GG_TOP / 2;
                    // folha com o canto dobrado (10 × 13), a 84
                    b->SetColor(D2D1::ColorF(0.93f, 0.93f, 0.94f, 1));
                    rt2->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(84, cy - 6.5f, 94, cy + 6.5f), 1.2f, 1.2f), b);
                    b->SetColor(D2D1::ColorF(0.6f, 0.6f, 0.62f, 1));
                    rt2->FillRectangle(D2D1::RectF(90.5f, cy - 6.5f, 94, cy - 3.f), b);
                    b->SetColor(D2D1::ColorF(1, 1, 1, 0.92f));
                    wchar_t ttl[512] = {}; GetWindowTextW(h, ttl, 512);
                    std::wstring name = ttl;
                    size_t dot = name.find_last_of(L'.'); if (dot != std::wstring::npos && dot > 0) name = name.substr(0, dot);
                    if (g_tfTop) rt2->DrawTextW(name.c_str(), (UINT32)name.size(), g_tfTop, D2D1::RectF(101.5f, 0, W - 12, GG_TOP), b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    b->Release();
                }
                rt2->EndDraw(); rt2->Release();
            }
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOVE: PaintTop(); if (g_barShown) PaintBar(); return 0;
    case WM_SHOWWINDOW: { LRESULT r = DefWindowProcW(h, m, w, l); PostMessageW(h, WM_APP + 9, 0, 0); return r; }
    case WM_APP + 9: PaintTop(); return 0;
    case WM_ERASEBKGND: { RECT r; GetClientRect(h, &r); FillRect((HDC)w, &r, (HBRUSH)GetStockObject(BLACK_BRUSH)); return 1; }
    case WM_SETTEXT: { LRESULT r = DefWindowProcW(h, m, w, l); PaintTop(); return r; }
    case WM_MFEVENT:
        if (w == 1) {
            if (l) { MessageBoxW(h, Tr(L"Não foi possível abrir este vídeo."), L"QuickTime Player", MB_OK | MB_ICONERROR); return 0; }
            g_ready = true;
            { SIZE vs{}, ar{}; g_noVideo = !g_mp || FAILED(g_mp->GetNativeVideoSize(&vs, &ar)) || !vs.cx; }
            FitWindowToVideo(); if (g_mp) g_mp->Play(); Poke(); InvalidateRect(g_video, nullptr, TRUE);
        } else if (w == 2) { g_ended = true; ShowBar(true); }
        return 0;
    case WM_TIMER:  // a barra some depois de 2,5 s sem mexer o mouse (tocando); o tempo anda
        if (g_top && (IsWindowVisible(g_top) || Unified())) PaintTop();  // macOS 27: o vidro acompanha o vídeo (15 quadros/s)
        if (g_barShown) {
            POINT p; GetCursorPos(&p); RECT br; GetWindowRect(g_bar, &br);
            bool over = PtInRect(&br, p) != FALSE;
            if (Playing() && !over && !g_scrubbing && GetTickCount() - g_lastMove > 2500) ShowBar(false); else PaintBar();
        }
        if (g_full && !g_barShown) { static DWORD t; if (GetTickCount() - g_lastMove > 2500 && GetTickCount() - t > 1000) { t = GetTickCount(); SetCursor(nullptr); } }
        return 0;
    case WM_ACTIVATE: if (LOWORD(w) == WA_INACTIVE) { POINT p; GetCursorPos(&p); RECT br{}; if (g_bar) GetWindowRect(g_bar, &br); if (!PtInRect(&br, p)) ShowBar(false); } else Poke(); return 0;
    case WM_KEYDOWN: {
        bool ctrl = GetKeyState(VK_CONTROL) < 0;
        if (w == VK_SPACE || w == 'K') Toggle();
        else if (w == VK_LEFT) Seek(Pos() - 5);
        else if (w == VK_RIGHT) Seek(Pos() + 5);
        else if (w == 'J') Seek(Pos() - 10);
        else if (w == 'L') Seek(Pos() + 10);
        else if (w == VK_UP) { g_volume = std::min(1.f, g_volume + 0.1f); if (g_mp) g_mp->SetVolume(g_volume); }
        else if (w == VK_DOWN) { g_volume = std::max(0.f, g_volume - 0.1f); if (g_mp) g_mp->SetVolume(g_volume); }
        else if ((ctrl && w == 'F') || (w == 'F' && l == 1)) SetFull(!g_full);
        else if (w == VK_ESCAPE && g_full) SetFull(false);
        else if (ctrl && w == 'O') OpenDialog();
        else if (ctrl && w == 'W') DestroyWindow(h);
        else if (w == VK_HOME) Seek(0);
        Poke();
        return 0;
    }
    case WM_DROPFILES: { wchar_t p[MAX_PATH * 2] = {}; if (DragQueryFileW((HDROP)w, 0, p, MAX_PATH * 2)) Open(p); DragFinish((HDROP)w); return 0; }
    case WM_DESTROY:
        if (g_mp) { g_mp->Shutdown(); g_mp->Release(); g_mp = nullptr; }
        if (g_bar) DestroyWindow(g_bar);
        if (g_barBack) DestroyWindow(g_barBack);
        if (g_top) DestroyWindow(g_top);
        if (g_topBack) DestroyWindow(g_topBack);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) {
    if (!Init()) return 1;
    MFStartup(MF_VERSION);
    g_tfTime = Fmt(11, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfIcon = Fmt(16, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, IconFamily());
    g_tfIconBig = Fmt(24, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, false, IconFamily());
    g_tfTimeL = Fmt(13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING);
    g_tfTimeR = Fmt(13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    g_tfTop = Fmt(13, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING);
    for (auto* f : { g_tfTimeL, g_tfTimeR, g_tfTop }) if (f) { f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); }
    WNDCLASSEXW wc{ sizeof(wc) }; wc.style = CS_DBLCLKS; wc.lpfnWndProc = Proc; wc.hInstance = hi; wc.lpszClassName = L"QuickTimeJanela";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1)); wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassExW(&wc);
    WNDCLASSEXW vc{ sizeof(vc) }; vc.style = CS_DBLCLKS; vc.lpfnWndProc = VideoProc; vc.hInstance = hi; vc.lpszClassName = L"QuickTimeVideo"; vc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&vc);
    WNDCLASSEXW bc{ sizeof(bc) }; bc.style = CS_DBLCLKS; bc.lpfnWndProc = BarProc; bc.hInstance = hi; bc.lpszClassName = L"QuickTimeBarra"; bc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassExW(&bc);
    MONITORINFO mi{ sizeof(mi) }; POINT c; GetCursorPos(&c); GetMonitorInfoW(MonitorFromPoint(c, MONITOR_DEFAULTTOPRIMARY), &mi);
    float s = GetDpiForSystem() / 96.f; int W = (int)(800 * s), H = (int)(480 * s);
    bool mac = barramac::Ligar();  // os três botões do Mac (LiquinixWindow.dll), com ou sem o Shell
    wnd = CreateWindowExW(WS_EX_ACCEPTFILES, wc.lpszClassName, L"QuickTime Player", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                          mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - W) / 2, mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - H) / 2, W, H, nullptr, nullptr, hi, nullptr);
    if (!wnd) return 1;
    BOOL dark = TRUE; DwmSetWindowAttribute(wnd, 20, &dark, sizeof(dark));  // o QuickTime é sempre escuro
    g_video = CreateWindowExW(WS_EX_ACCEPTFILES, vc.lpszClassName, L"", WS_CHILD | WS_VISIBLE, 0, 0, W, H, wnd, nullptr, hi, nullptr);
    g_bar = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, bc.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, wnd, nullptr, hi, nullptr);
    if (mac && semaforo::Aqua()) {
        // a barra de título de 32 do app (a DLL só põe os três botões a 16)
        SetPropW(wnd, L"LiquinixBarraUnificada", (HANDLE)(INT_PTR)std::lround(GG_TOP));
        BAR = GG_TOP;
        SetWindowSubclass(wnd, UnifiedSub, 0x55A1, 0);
        SetWindowPos(wnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        // o vidro fosco da barra de controles (desfoca o vídeo atrás), em barra_video.h
        // o vidro fosco das barras é desenhado aqui (barravideo::FundoBorrado), já recortado nos cantos certos
        g_top = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, bc.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, wnd, nullptr, hi, nullptr);
    }
    SetTimer(wnd, 1, semaforo::Aqua() ? 66 : 250, nullptr);  // macOS 27: o vidro das barras desfoca o vídeo que anda
    ShowWindow(wnd, show);
    int argc = 0; LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc > 1 && argv[1][0]) Open(argv[1]); else PostMessageW(wnd, WM_KEYDOWN, 'O', 0x1D0001);
    if (argv) LocalFree(argv);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_KEYDOWN && msg.hwnd == wnd && msg.wParam == 'O' && msg.lParam == 0x1D0001) { OpenDialog(); continue; }  // sem arquivo: pergunta qual
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    MFShutdown();
    return 0;
}
