// Desenho comum do shell: Direct2D numa DIB de 32 bits (como o menu de contexto), acrílico,
// fontes e os ícones próprios (pasta azul do Arquivos, lixeira).
#include "shell.h"
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include "../comum/docicone.h"
#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <dwmapi.h>
#include <atomic>
#include <thread>
#include <algorithm>
#include <cmath>

ID2D1Factory* ui::d2d;
IDWriteFactory* ui::dw;
std::wstring ui::family = L"Segoe UI";
std::wstring ui::iconFamily = L"Segoe Fluent Icons";

static bool HasFamily(const wchar_t* name) {
    IDWriteFontCollection* fc = nullptr;
    if (FAILED(ui::dw->GetSystemFontCollection(&fc))) return false;
    UINT32 i; BOOL ok = FALSE;
    fc->FindFamilyName(name, &i, &ok);
    fc->Release();
    return ok;
}

bool ui::Init() {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&dw))) return false;
    for (auto f : { L"SF Pro Text", L"Inter", L"Segoe UI Variable Text", L"Segoe UI" }) if (HasFamily(f)) { family = f; break; }
    if (!HasFamily(L"Segoe Fluent Icons")) iconFamily = L"Segoe MDL2 Assets";
    Log(L"fonte: %ls, ícones: %ls", family.c_str(), iconFamily.c_str());
    return true;
}

bool ui::Dark() {  // ajuste "tema": auto (segue o Windows), claro ou escuro
    std::wstring t = shell::Setting(L"tema", L"auto");
    if (t == L"claro") return false;
    if (t == L"escuro") return true;
    DWORD v = 1, cb = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &cb);
    return v == 0;
}

float ui::Scale(HWND h) { return GetDpiForWindow(h) / 96.f; }

ID2D1RenderTarget* ui::Begin(Surface& s, int w, int h, float scale) {
    w = std::max(w, 1); h = std::max(h, 1);
    if (s.w != w || s.h != h || !s.mem) {
        if (s.rt) { s.rt->Release(); s.rt = nullptr; }
        if (s.dib) DeleteObject(s.dib);
        if (!s.mem) s.mem = CreateCompatibleDC(nullptr);
        BITMAPINFO bi{};
        bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
        s.dib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &s.bits, nullptr, 0);
        SelectObject(s.mem, s.dib);
        s.w = w; s.h = h;
    }
    if (!s.rt) {
        auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        if (FAILED(d2d->CreateDCRenderTarget(&props, &s.rt))) return nullptr;
        s.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    s.scale = scale;
    RECT rc{ 0, 0, w, h };
    s.rt->BindDC(s.mem, &rc);
    s.rt->SetDpi(96 * scale, 96 * scale);
    s.rt->BeginDraw();
    s.rt->Clear(D2D1::ColorF(0, 0, 0, 0));
    return s.rt;
}

void ui::End(Surface& s, bool layered, POINT pos) {
    if (FAILED(s.rt->EndDraw())) { s.rt->Release(); s.rt = nullptr; return; }
    if (layered) {
        SIZE sz{ s.w, s.h };
        POINT src{ 0, 0 };
        BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        UpdateLayeredWindow(s.hwnd, nullptr, &pos, &sz, s.mem, &src, 0, &bf, ULW_ALPHA);
    } else {
        HDC dc = GetDC(s.hwnd);
        BitBlt(dc, 0, 0, s.w, s.h, s.mem, 0, 0, SRCCOPY);
        ReleaseDC(s.hwnd, dc);
    }
}

void ui::Free(Surface& s) {
    if (s.rt) s.rt->Release();
    if (s.dib) DeleteObject(s.dib);
    if (s.mem) DeleteDC(s.mem);
    s = Surface{};
}

// ---- GPU: um dispositivo para o processo todo; cada janela tem sua swap chain na composição.
namespace {
ID3D11Device* g_d3d;
IDXGIDevice* g_dxgi;
ID2D1Factory1* g_f1;
ID2D1Device* g_d2dDev;
ID2D1DeviceContext* g_dc;
IDCompositionDevice* g_dcomp;
}

ID2D1DeviceContext* ui::Gpu() {
    if (g_dc) return g_dc;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &g_d3d, nullptr, nullptr);
    if (FAILED(hr)) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &g_d3d, nullptr, nullptr);
    if (FAILED(hr)) { Log(L"erro: D3D11 0x%08X", (unsigned)hr); return nullptr; }
    g_d3d->QueryInterface(IID_PPV_ARGS(&g_dxgi));
    // a captura de janelas (fotogpu.cpp) escreve quadros neste dispositivo de outras threads
    if (ID3D11Multithread* mt = nullptr; SUCCEEDED(g_d3d->QueryInterface(IID_PPV_ARGS(&mt)))) { mt->SetMultithreadProtected(TRUE); mt->Release(); }
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr, (void**)&g_f1);
    g_f1->CreateDevice(g_dxgi, &g_d2dDev);
    g_d2dDev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g_dc);
    g_dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    DCompositionCreateDevice(g_dxgi, IID_PPV_ARGS(&g_dcomp));
    return g_dc;
}

static void BindBack(ui::GpuSurface& s) {
    IDXGISurface* surf = nullptr;
    if (FAILED(s.swap->GetBuffer(0, IID_PPV_ARGS(&surf)))) return;
    auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                         D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    g_dc->CreateBitmapFromDxgiSurface(surf, &props, &s.back);
    surf->Release();
}

ID2D1DeviceContext* ui::GpuBegin(GpuSurface& s, int w, int h, float scale) {
    if (!Gpu()) return nullptr;
    w = std::max(w, 1); h = std::max(h, 1);
    if (!s.swap) {
        IDXGIFactory2* fac = nullptr;
        CreateDXGIFactory2(0, IID_PPV_ARGS(&fac));
        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width = w; d.Height = h;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.Scaling = DXGI_SCALING_STRETCH;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        d.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        HRESULT hr = fac->CreateSwapChainForComposition(g_d3d, &d, nullptr, &s.swap);
        fac->Release();
        if (FAILED(hr)) { Log(L"erro: swap chain 0x%08X", (unsigned)hr); return nullptr; }
        g_dcomp->CreateTargetForHwnd(s.hwnd, TRUE, &s.target);
        g_dcomp->CreateVisual(&s.visual);
        s.visual->SetContent(s.swap);
        s.target->SetRoot(s.visual);
        g_dcomp->Commit();
        s.w = w; s.h = h;
        BindBack(s);
    } else if (s.w != w || s.h != h) {
        g_dc->SetTarget(nullptr);
        if (s.back) { s.back->Release(); s.back = nullptr; }
        s.swap->ResizeBuffers(2, w, h, DXGI_FORMAT_UNKNOWN, 0);
        s.w = w; s.h = h;
        BindBack(s);
    }
    g_dc->SetTarget(s.back);
    g_dc->SetDpi(96 * scale, 96 * scale);
    g_dc->BeginDraw();
    g_dc->SetTransform(D2D1::Matrix3x2F::Identity());
    g_dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    return g_dc;
}

void ui::GpuEnd(GpuSurface& s) {
    HRESULT hr = g_dc->EndDraw();
    if (FAILED(hr)) Log(L"erro: EndDraw 0x%08X", (unsigned)hr);
    s.swap->Present(0, 0);  // o ritmo vem do FrameClock (DwmFlush), não do Present
    g_dc->SetTarget(nullptr);
}

IDCompositionDevice* ui::Comp() { return Gpu() ? g_dcomp : nullptr; }
ID3D11Device* ui::D3D() { return Gpu() ? g_d3d : nullptr; }

IDCompositionSurface* ui::CompSurface(int w, int h, const std::function<void(ID2D1DeviceContext*)>& draw) {
    if (!Comp() || w < 1 || h < 1) return nullptr;
    IDCompositionSurface* cs = nullptr;
    if (FAILED(g_dcomp->CreateSurface(w, h, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &cs))) return nullptr;
    IDXGISurface* ds = nullptr; POINT off{};
    if (FAILED(cs->BeginDraw(nullptr, IID_PPV_ARGS(&ds), &off))) { cs->Release(); return nullptr; }
    auto props = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                         D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ID2D1Bitmap1* bm = nullptr;
    g_dc->CreateBitmapFromDxgiSurface(ds, &props, &bm);
    ds->Release();
    bool ok = false;
    if (bm) {
        g_dc->SetTarget(bm);
        g_dc->SetDpi(96, 96);
        g_dc->BeginDraw();
        g_dc->SetTransform(D2D1::Matrix3x2F::Translation((float)off.x, (float)off.y));
        g_dc->PushAxisAlignedClip(D2D1::RectF(0, 0, (float)w, (float)h), D2D1_ANTIALIAS_MODE_ALIASED);
        g_dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        draw(g_dc);
        g_dc->PopAxisAlignedClip();
        g_dc->SetTransform(D2D1::Matrix3x2F::Identity());
        ok = SUCCEEDED(g_dc->EndDraw());
        g_dc->SetTarget(nullptr);
        bm->Release();
    }
    cs->EndDraw();
    if (!ok) { cs->Release(); return nullptr; }
    return cs;
}

IDCompositionAnimation* ui::CompCurve(const std::vector<float>& v, float dur) {
    IDCompositionAnimation* an = nullptr;
    if (!Comp() || v.size() < 2 || FAILED(g_dcomp->CreateAnimation(&an))) return nullptr;
    int K = (int)v.size();
    float dt = dur / (K - 1);
    auto slope = [&](int k) {
        if (k == 0) return (v[1] - v[0]) / dt;
        if (k == K - 1) return (v[K - 1] - v[K - 2]) / dt;
        return (v[k + 1] - v[k - 1]) / (2 * dt);
    };
    for (int k = 0; k < K - 1; k++) {
        float p0 = v[k], p1 = v[k + 1], m0 = slope(k), m1 = slope(k + 1), d = (p1 - p0) / dt;
        an->AddCubic(k * dt, p0, m0, (3 * d - 2 * m0 - m1) / dt, (m0 + m1 - 2 * d) / (dt * dt));
    }
    an->End(dur, v[K - 1]);
    return an;
}

void ui::GpuScale(GpuSurface& s, float scale, float cx, float cy) {
    if (!s.visual || !g_dcomp) return;
    D2D1_MATRIX_3X2_F m = D2D1::Matrix3x2F::Scale(scale, scale, D2D1::Point2F(cx, cy));
    s.visual->SetTransform(m);
    g_dcomp->Commit();
}

void ui::GpuMotion(GpuSurface& s, float scale, float cx, float cy, float opacity) {
    if (!s.visual || !g_dcomp) return;
    s.visual->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale, D2D1::Point2F(cx, cy)));
    IDCompositionEffectGroup* e = nullptr;
    if (SUCCEEDED(g_dcomp->CreateEffectGroup(&e))) { e->SetOpacity(std::clamp(opacity, 0.f, 1.f)); s.visual->SetEffect(e); e->Release(); }
    g_dcomp->Commit();
}

void ui::GpuOffset(GpuSurface& s, float x, float y) {
    if (!s.visual) return;
    s.visual->SetOffsetX(x);
    s.visual->SetOffsetY(y);
    g_dcomp->Commit();
}

// Deslize feito pelo compositor (DWM), não por quadros nossos: liso mesmo com a thread ocupada.
// Curva ease-out cúbica: p(u) = a + d(3u - 3u² + u³), u = t/T.
void ui::GpuSlide(GpuSurface& s, float fromX, float fromY, float toX, float toY, float ms) {
    if (!s.visual) return;
    float T = std::max(ms, 1.f) / 1000.f;
    auto curve = [&](float a, float b) -> IDCompositionAnimation* {
        IDCompositionAnimation* an = nullptr;
        if (FAILED(g_dcomp->CreateAnimation(&an))) return nullptr;
        float d = b - a;
        an->AddCubic(0, a, 3 * d / T, -3 * d / (T * T), d / (T * T * T));
        an->End(T, b);
        return an;
    };
    IDCompositionAnimation *ax = curve(fromX, toX), *ay = curve(fromY, toY);
    if (ax) { s.visual->SetOffsetX(ax); ax->Release(); } else s.visual->SetOffsetX(toX);
    if (ay) { s.visual->SetOffsetY(ay); ay->Release(); } else s.visual->SetOffsetY(toY);
    g_dcomp->Commit();
}

void ui::GpuFree(GpuSurface& s) {
    if (s.back) s.back->Release();
    if (s.visual) s.visual->Release();
    if (s.target) s.target->Release();
    if (s.swap) s.swap->Release();
    s = GpuSurface{ s.hwnd };
}

// Relógio de quadros: uma thread que espera a composição da tela (DwmFlush) e avisa a janela.
namespace {
// Parado, a thread dorme num evento (antes acordava a cada 8 ms para sempre: ~125 vezes por segundo por
// relógio, mesmo sem nada animando). Animando, roda com prioridade alta para não perder quadro quando o
// sistema está ocupado.
// pending/acks: a janela que chama ui::FrameAck recebe o próximo quadro só depois de tratar o anterior (com o
// desenho mais lento que a tela, as mensagens se acumulavam na fila e o que a pessoa digitava esperava atrás delas)
struct Clock { HWND hwnd; UINT msg; std::atomic<bool> on; HANDLE wake; std::atomic<bool> pending{ false }, acks{ false }; DWORD postedAt = 0; };
std::vector<Clock*> g_clocks;
}

// ---- Estilo dos ícones do macOS 26 (Aparência > Estilo de ícones e widgets)
D2D1_COLOR_F ui::Accent() {
    static const std::pair<const wchar_t*, UINT32> acc[] = { { L"azul", 0x007AFF }, { L"roxo", 0x953D96 }, { L"rosa", 0xF74F9E }, { L"vermelho", 0xE0383E },
        { L"laranja", 0xF7821B }, { L"amarelo", 0xFCB827 }, { L"verde", 0x62BA46 }, { L"grafite", 0x8C8C8C }, { L"lavanda", 0x98A8D9 } };  // lavanda: a 10ª cor do macOS 27
    std::wstring d = shell::Setting(L"destaque", L"azul");
    UINT32 c = 0x007AFF;
    for (auto& [n, v] : acc) if (d == n) c = v;
    return D2D1::ColorF(((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f, 1);
}

std::wstring ui::IconStyleKey() {
    std::wstring st = IconStyle();
    return st == L"padrao" ? st : st + L"/" + shell::Setting(L"destaque", L"azul") + (Dark() ? L"/e" : L"/c");
}

std::wstring ui::IconStyle() {
    if (!liquid::On()) return L"padrao";
    std::wstring st = shell::Setting(L"estilo_icones", L"padrao");
    // macOS 27: Escuro com "Auto" (estilo_escuro_auto=1) só vale no modo escuro
    if (st == L"escuro" && shell::Setting(L"estilo_escuro_auto", L"0") == L"1" && !Dark()) return L"padrao";
    return st;
}

// Ícones "Transparente" e "Tingido" do macOS 26: o ícone não ganha só um filtro de cor, vira uma peça de Liquid Glass.
// O corpo (o próprio formato do ícone) fica translúcido — o vidro do Dock aparece através dele —, o desenho vira
// branco gravado no vidro e a borda ganha o brilho especular (luz principal a −45° e de preenchimento a 135°, como o
// CASDFKeyFillHighlightEffect; guia macOS26-Theme-Reference, seção 29.1) e um fio claro em volta. No Tingido o corpo
// leva a cor de destaque. Tudo nos pixels (BGRA pré-multiplicado), então vale no Dock, no Apps, no Spotlight, na Pilha
// de Downloads e na Lixeira.
static void GlassIcon(UINT w, UINT h, std::vector<BYTE>& px, bool tinted, float tr, float tg, float tb, bool dark) {
    const size_t N = (size_t)w * h;
    // 1) cor de fundo do ícone: a média numa faixa a ~12 % da borda do desenho (onde só há fundo)
    double sr = 0, sg = 0, sb = 0; int n = 0;
    auto at = [&](UINT x, UINT y) {
        const BYTE* q = &px[((size_t)y * w + x) * 4];
        if (q[3] < 200) return;
        float a = q[3] / 255.f;
        sb += q[0] / 255.f / a; sg += q[1] / 255.f / a; sr += q[2] / 255.f / a; n++;
    };
    UINT m = std::max(2u, w * 14 / 100);
    for (UINT k = m; k + m < w; k++) { at(k, m); at(k, h - 1 - m); }
    for (UINT k = m; k + m < h; k++) { at(m, k); at(w - 1 - m, k); }
    float br_ = n ? (float)(sr / n) : 1, bg_ = n ? (float)(sg / n) : 1, bb_ = n ? (float)(sb / n) : 1;
    const float bgL = 0.2126f * br_ + 0.7152f * bg_ + 0.0722f * bb_;  // claridade do fundo do ícone
    // 2) distância até a borda do formato (chamfer 3-4, duas passadas): a faixa da borda acende
    std::vector<float> dist(N);
    const float INF = 1e9f;
    for (size_t i = 0; i < N; i++) dist[i] = px[i * 4 + 3] >= 128 ? INF : 0;
    for (UINT y = 0; y < h; y++)
        for (UINT x = 0; x < w; x++) {
            float& d = dist[(size_t)y * w + x];
            if (d == 0) continue;
            if (x > 0) d = std::min(d, dist[(size_t)y * w + x - 1] + 1);
            if (y > 0) d = std::min(d, dist[(size_t)(y - 1) * w + x] + 1);
            if (x > 0 && y > 0) d = std::min(d, dist[(size_t)(y - 1) * w + x - 1] + 1.4142f);
            if (x + 1 < w && y > 0) d = std::min(d, dist[(size_t)(y - 1) * w + x + 1] + 1.4142f);
            if (x == 0 || y == 0 || x + 1 == w) d = std::min(d, 1.f);
        }
    for (int y = (int)h - 1; y >= 0; y--)
        for (int x = (int)w - 1; x >= 0; x--) {
            float& d = dist[(size_t)y * w + x];
            if (d == 0) continue;
            if (x + 1 < (int)w) d = std::min(d, dist[(size_t)y * w + x + 1] + 1);
            if (y + 1 < (int)h) d = std::min(d, dist[(size_t)(y + 1) * w + x] + 1);
            if (x + 1 < (int)w && y + 1 < (int)h) d = std::min(d, dist[(size_t)(y + 1) * w + x + 1] + 1.4142f);
            if (x > 0 && y + 1 < (int)h) d = std::min(d, dist[(size_t)(y + 1) * w + x - 1] + 1.4142f);
            if (y + 1 == (int)h || x == 0) d = std::min(d, 1.f);
        }
    const float s = w / 128.f;            // escala (os desenhos vão de 32 a 256 px)
    const float cx = w / 2.f, cy = h / 2.f;
    // corpo do vidro: claro bem translúcido (Transparente) ou a cor de destaque (Tingido)
    // (referência do usuário, macOS 26: o corpo é um vidro fosco claro onde a cor de trás aparece, mais claro no alto)
    float bodyA = tinted ? (dark ? 0.30f : 0.36f) : (dark ? 0.10f : 0.16f);  // Tingido: bem mais transparente (o usuário)  // a cor de trás passa (o desenho branco se destaca)
    float bR = tinted ? tr : 1, bG = tinted ? tg : 1, bB = tinted ? tb : 1;
    // o desenho: o quanto cada pixel foge da cor de fundo do ícone (calculado antes, para a sombra do relevo)
    std::vector<float> fm(N, 0.f), lm(N, 0.f);
    for (size_t i = 0; i < N; i++) {
        BYTE* p = &px[i * 4];
        float a = p[3] / 255.f;
        if (a <= 0.004f) continue;
        float b = p[0] / 255.f / a, g = p[1] / 255.f / a, r = p[2] / 255.f / a;
        lm[i] = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        float dc = std::sqrt((r - br_) * (r - br_) + (g - bg_) * (g - bg_) + (b - bb_) * (b - bb_));
        fm[i] = std::clamp((dc - 0.10f) / 0.30f, 0.f, 1.f);
        // Tingido (o usuário: "o branco, ao invés de ser branco, é transparente"): o que é claro no ícone original
        // vira branco de verdade, mesmo sendo o fundo do ícone (Chrome, Finder...); o resto fica o vidro colorido
        // (ícone de fundo claro: o fundo vira o vidro colorido e o desenho fica branco, senão sumiria tudo no branco)
        if (tinted && bgL < 0.62f) fm[i] = std::max(fm[i], std::clamp((lm[i] - 0.62f) / 0.22f, 0.f, 1.f));
    }
    // Transparente no escuro (print do Dock real do macOS 26, 16/11/2026): não é vidro claro, é uma peça de vidro
    // ESCURA (o fundo do ícone some num grafite translúcido) com o desenho em cinza-claro monocromático, mais claro
    // no alto e mais escuro embaixo, a claridade do original dando os tons (o verde do Spotify vira cinza médio, o
    // branco do Discord vira quase branco); a borda é só um fio fino e discreto, sem brilho largo nem sombra.
    // Tingido no escuro: o mesmo filtro (17/11/2026, o usuário), com a cor de destaque no corpo e no desenho
    if (dark) {
        for (size_t i = 0; i < N; i++) {
            BYTE* p = &px[i * 4];
            float a = p[3] / 255.f;
            if (a <= 0.004f) continue;
            if (dist[i] == 0) {  // serrilhado fora do formato: um fio cinza fraco
                float k = a > 0.25f ? a * 0.35f : 0.f;
                p[0] = p[1] = p[2] = (BYTE)std::lround(k * 0.8f * 255); p[3] = (BYTE)std::lround(k * 255);
                continue;
            }
            UINT x = (UINT)(i % w), y = (UINT)(i / w);
            float yy = (float)y / h, l = lm[i], f = fm[i], d = dist[i];
            // corpo: grafite translúcido, um véu claro bem leve no alto
            // (medido sobre preto: a peça do Mac fica em 16/255, quase o fundo do Dock; a nossa estava em 30)
            // Transparente: vidro de verdade, quase sem corpo (com 50 % o papel de parede colorido ficava escurecido e o
            // ícone parecia tingido, 17/11/2026); o desenho claro e o fio da borda fazem a forma
            float oR = 0.06f, oG = 0.06f, oB = 0.065f, oa = 0.24f;
            if (tinted) { oR = tr * 0.30f; oG = tg * 0.30f; oB = tb * 0.30f; oa = 0.62f; }
            float top = 0.015f * std::clamp(1 - yy / 0.45f, 0.f, 1.f);
            oR += (1 - oR) * top; oG += (1 - oG) * top; oB += (1 - oB) * top;
            // desenho: cinza pelo tom do original, em degradê vertical (0,93 no alto, ~0,66 embaixo)
            float grad = 0.93f - 0.27f * yy;
            float gv = std::clamp(grad * (0.58f + 0.48f * l), 0.30f, 0.97f);
            float gR = gv, gG = gv, gB = std::min(1.f, gv + 0.01f);
            if (tinted) { gR = gv * (0.62f + 0.38f * tr); gG = gv * (0.62f + 0.38f * tg); gB = gv * (0.62f + 0.38f * tb); }  // desenho claro puxado para o destaque
            oR = oR + (gR - oR) * f; oG = oG + (gG - oG) * f; oB = oB + (gB - oB) * f;
            oa = oa + (0.96f - oa) * f;
            // borda: fio de ~1 px, um pouco mais claro do lado de cima à esquerda
            float nx = x + 0.5f - cx, ny = y + 0.5f - cy, nl = std::sqrt(nx * nx + ny * ny);
            if (nl > 0) { nx /= nl; ny /= nl; }
            float key = std::max(0.f, -(nx + ny) * 0.70711f);
            float rim = d <= 1.2f * s ? 0.22f + 0.22f * key : 0.f;
            rim += std::exp(-std::max(0.f, d - 1) / (1.0f * s)) * 0.10f * key;
            rim = std::clamp(rim, 0.f, 0.5f);
            oR += (0.85f - oR) * rim; oG += (0.85f - oG) * rim; oB += (0.87f - oB) * rim;
            oa += (1 - oa) * rim * 0.6f;
            oa = std::clamp(oa, 0.f, 1.f) * a;
            p[0] = (BYTE)std::lround(std::clamp(oB, 0.f, 1.f) * oa * 255);
            p[1] = (BYTE)std::lround(std::clamp(oG, 0.f, 1.f) * oa * 255);
            p[2] = (BYTE)std::lround(std::clamp(oR, 0.f, 1.f) * oa * 255);
            p[3] = (BYTE)std::lround(oa * 255);
        }
        return;
    }
    const int sh = std::max(1, (int)std::lround(2.f * s));  // sombra do desenho, 2 px (a 128) para baixo
    for (size_t i = 0; i < N; i++) {
        BYTE* p = &px[i * 4];
        float a = p[3] / 255.f;
        if (a <= 0.004f) continue;
        if (dist[i] == 0) {  // fora do formato: a sombra do desenho some (o vidro faz a dele); o serrilhado vira o fio claro
            float k = a > 0.25f ? a * 0.6f : 0.f;
            p[0] = p[1] = p[2] = p[3] = (BYTE)std::lround(k * 255);
            continue;
        }
        float l = lm[i], f = fm[i];
        UINT x = (UINT)(i % w), y = (UINT)(i / w);
        float shadow = y >= (UINT)sh ? fm[i - (size_t)sh * w] * (1 - f) : 0.f;  // logo abaixo do desenho
        float d = dist[i];
        // a borda: um fio fino e o brilho que entra alguns px no vidro, mais forte do lado da luz
        float nx = x + 0.5f - cx, ny = y + 0.5f - cy, nl = std::sqrt(nx * nx + ny * ny);
        if (nl > 0) { nx /= nl; ny /= nl; }
        float key = std::max(0.f, -(nx + ny) * 0.70711f), fill = std::max(0.f, (nx + ny) * 0.70711f) * 0.5f;
        float edge = std::exp(-std::max(0.f, d - 1) / (2.2f * s));
        float rim = std::clamp((d <= 1.3f * s ? 0.70f : 0.f) + edge * (0.30f + 0.70f * std::pow(key + fill, 1.5f)), 0.f, 1.f);
        // vidro: corpo + bem mais claro no alto (o volume do vidro), sumindo até ~60 % da altura
        float yy = (float)y / h, top = 0.16f * std::clamp(1 - yy / 0.55f, 0.f, 1.f);
        float oa = bodyA + top;
        float oR = bR, oG = bG, oB = bB;
        // a sombra do relevo: um pouco mais escuro logo abaixo do desenho
        oR *= 1 - 0.55f * shadow; oG *= 1 - 0.55f * shadow; oB *= 1 - 0.55f * shadow; oa += 0.16f * shadow;
        // o desenho gravado em branco (com o relevo pela claridade do original)
        float gv = std::clamp(0.92f + 0.16f * (l - 0.5f), 0.f, 1.f);
        float ga = tinted ? 1.f : (dark ? 0.92f : 0.96f);
        oR = oR + (gv - oR) * f; oG = oG + (gv - oG) * f; oB = oB + (gv - oB) * f;
        oa = oa + (ga - oa) * f;
        // o brilho da borda por cima (branco)
        oR = oR + (1 - oR) * rim; oG = oG + (1 - oG) * rim; oB = oB + (1 - oB) * rim;
        oa = oa + (1 - oa) * rim * 0.9f;
        oa = std::clamp(oa, 0.f, 1.f) * a;  // o formato (cantos) continua o do ícone
        p[0] = (BYTE)std::lround(std::clamp(oB, 0.f, 1.f) * oa * 255);
        p[1] = (BYTE)std::lround(std::clamp(oG, 0.f, 1.f) * oa * 255);
        p[2] = (BYTE)std::lround(std::clamp(oR, 0.f, 1.f) * oa * 255);
        p[3] = (BYTE)std::lround(oa * 255);
    }
}

// Escuro: o fundo claro do ícone vira grafite e o desenho colorido fica (um pouco mais vivo);
// Transparente e Tingido: o ícone vira vidro (GlassIcon).
void ui::StyleIcon(UINT w, UINT h, std::vector<BYTE>& px, const std::wstring& style) {
    if (style == L"padrao" || style.empty() || px.size() < (size_t)w * h * 4) return;
    float tr = 0.04f, tg = 0.52f, tb = 1.0f;  // azul do Mac
    {
        static const std::pair<const wchar_t*, UINT32> acc[] = { { L"roxo", 0x953D96 }, { L"rosa", 0xF74F9E }, { L"vermelho", 0xE0383E },
            { L"laranja", 0xF7821B }, { L"amarelo", 0xFCB827 }, { L"verde", 0x62BA46 }, { L"grafite", 0x8C8C8C }, { L"lavanda", 0x98A8D9 } };  // lavanda: a 10ª cor do macOS 27
        std::wstring d = shell::Setting(L"destaque", L"azul");
        for (auto& [n, c] : acc) if (d == n) { tr = ((c >> 16) & 255) / 255.f; tg = ((c >> 8) & 255) / 255.f; tb = (c & 255) / 255.f; }
    }
    bool dark = ui::Dark();
    if (style == L"transparente" || style == L"tingido") { GlassIcon(w, h, px, style == L"tingido", tr, tg, tb, dark); return; }
    // Escuro: só ícone de fundo claro tem o fundo trocado (o "Live" do Ableton já é escuro, com letra branca).
    // O fundo é o que aparece perto da borda do desenho: média da claridade numa faixa a ~12% da borda.
    bool lightBg = false;
    if (style == L"escuro") {
        double sum = 0; int n = 0;
        auto at = [&](UINT x, UINT y) {
            const BYTE* q = &px[((size_t)y * w + x) * 4];
            if (q[3] < 200) return;
            sum += (0.2126 * q[2] + 0.7152 * q[1] + 0.0722 * q[0]) / q[3]; n++;
        };
        UINT m = std::max(2u, w * 12 / 100);
        for (UINT k = m; k + m < w; k++) { at(k, m); at(k, h - 1 - m); }
        for (UINT k = m; k + m < h; k++) { at(m, k); at(w - 1 - m, k); }
        lightBg = n > 0 && sum / n > 0.6;
    }
    for (size_t i = 0; i < (size_t)w * h; i++) {
        BYTE* p = &px[i * 4];
        float a = p[3] / 255.f;
        if (a <= 0) continue;
        float b = p[0] / 255.f / a, g = p[1] / 255.f / a, r = p[2] / 255.f / a;  // sem o pré-multiplicado
        float l = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        float mx = std::max({ r, g, b }), mn = std::min({ r, g, b }), sat = mx > 0 ? (mx - mn) / mx : 0;
        float nr = r, ng = g, nb = b, na = a;
        if (style == L"escuro") {
            if (!lightBg) continue;  // fundo já escuro ou colorido: fica como está
            float bg = std::clamp((l - 0.62f) / 0.3f, 0.f, 1.f) * std::clamp(1 - sat / 0.3f, 0.f, 1.f);  // fundo claro e pouco colorido
            float d = 0.13f + 0.10f * (1 - l);
            nr = r + (d - r) * bg; ng = g + (d - g) * bg; nb = b + (d - b) * bg;
        } else if (style == L"transparente") {
            float v = (dark ? 0.72f : 0.86f) + 0.70f * (l - 0.5f);  // o desenho aparece pela claridade (bem marcado)
            v = std::clamp(v, 0.f, 1.f);
            nr = v; ng = v; nb = std::min(1.f, v + 0.02f);
            na = a * (dark ? 0.74f : 0.80f);
        } else if (style == L"tingido") {
            float v = std::clamp(0.25f + 0.85f * l, 0.f, 1.f);
            float k = std::clamp((v - 0.75f) / 0.25f, 0.f, 1.f);  // partes bem claras puxam para o branco
            nr = tr * v + (1 - tr * v) * k * 0.8f; ng = tg * v + (1 - tg * v) * k * 0.8f; nb = tb * v + (1 - tb * v) * k * 0.8f;
            if (dark) { nr *= 0.85f; ng *= 0.85f; nb *= 0.85f; }
        }
        p[0] = (BYTE)std::lround(std::clamp(nb, 0.f, 1.f) * na * 255);
        p[1] = (BYTE)std::lround(std::clamp(ng, 0.f, 1.f) * na * 255);
        p[2] = (BYTE)std::lround(std::clamp(nr, 0.f, 1.f) * na * 255);
        p[3] = (BYTE)std::lround(na * 255);
    }
}

void ui::FrameAck(HWND hwnd, UINT msg) {
    for (auto* c : g_clocks) if (c->hwnd == hwnd && c->msg == msg) { c->acks = true; c->pending = false; return; }
}

void ui::FrameClock(HWND hwnd, UINT msg, bool on) {
    for (auto* c : g_clocks)
        if (c->hwnd == hwnd && c->msg == msg) {
            if (on && !c->on) { c->on = true; SetEvent(c->wake); }
            else if (!on) c->on = false;
            return;
        }
    if (!on) return;
    auto* c = new Clock{ hwnd, msg, true, CreateEventW(nullptr, FALSE, FALSE, nullptr) };
    g_clocks.push_back(c);
    std::thread([c] {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        DWORD since = GetTickCount(); unsigned frames = 0;
        for (;;) {
            if (!c->on) { WaitForSingleObject(c->wake, INFINITE); since = GetTickCount(); frames = 0; continue; }
            DwmFlush();
            if (!c->on) continue;
            // janela destruída não precisa de quadros: desliga sozinho (janela escondida pode precisar: os
            // widgets animam pela janela invisível do relógio deles)
            if (!IsWindow(c->hwnd)) { c->on = false; continue; }
            if (c->acks && c->pending && GetTickCount() - c->postedAt < 250) continue;  // o anterior ainda não foi tratado
            c->pending = true; c->postedAt = GetTickCount();
            PostMessageW(c->hwnd, c->msg, 0, 0);
            if (++frames == 60 * 20) {  // 20 s seguidos animando: avisa no log (algo pode ter ficado ligado)
                wchar_t cls[64] = {}; GetClassNameW(c->hwnd, cls, 64);
                Log(L"relógio de quadros: %ls (msg %u) ligado há %lu s", cls, c->msg, (GetTickCount() - since) / 1000);
            }
        }
    }).detach();
}

void ui::Acrylic(HWND h, DWORD tint) {
    struct ACCENT_POLICY { int state, flags; DWORD gradient; int anim; };
    struct WCA_DATA { int attr; PVOID data; SIZE_T size; };
    using Fn = BOOL(WINAPI*)(HWND, WCA_DATA*);
    static Fn fn = (Fn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute");
    if (!fn) return;
    // Transparência e Efeitos: a mesma regra de tinta do vidro da Composição (backdrop.cpp)
    float a = backdrop::AdjustAlpha((tint >> 24) / 255.f);
    tint = (tint & 0x00FFFFFFu) | ((DWORD)std::lround(std::clamp(a, 0.f, 1.f) * 255) << 24);
    ACCENT_POLICY ap{ 4 /*ACCENT_ENABLE_ACRYLICBLURBEHIND*/, 0, tint, 0 };
    WCA_DATA d{ 19 /*WCA_ACCENT_POLICY*/, &ap, sizeof(ap) };
    fn(h, &d);
}

IDWriteTextFormat* ui::Format(float size, DWRITE_FONT_WEIGHT w, bool icons, DWRITE_TEXT_ALIGNMENT a) {
    IDWriteTextFormat* tf = nullptr;
    dw->CreateTextFormat((icons ? iconFamily : family).c_str(), nullptr, w, DWRITE_FONT_STYLE_NORMAL,
                         DWRITE_FONT_STRETCH_NORMAL, size, L"pt-br", &tf);
    tf->SetTextAlignment(a);
    tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    tf->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return tf;
}

IDWriteTextFormat* ui::FormatFace(const wchar_t* face, float size, DWRITE_FONT_WEIGHT w) {
    IDWriteTextFormat* tf = nullptr;
    if (FAILED(dw->CreateTextFormat(face, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"pt-br", &tf)) || !tf)
        return Format(size, w);
    tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    tf->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return tf;
}

float ui::TextWidth(IDWriteTextFormat* tf, const std::wstring& s) {
    IDWriteTextLayout* tl = nullptr;
    if (FAILED(dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), tf, 10000, 100, &tl))) return 0;
    DWRITE_TEXT_METRICS tm{}; tl->GetMetrics(&tm); tl->Release();
    return tm.widthIncludingTrailingWhitespace;
}

ID2D1Bitmap* ui::Bitmap(ID2D1RenderTarget* rt, UINT w, UINT h, const std::vector<BYTE>& bgra) {
    if (!w || !h || bgra.size() < (size_t)w * h * 4) return nullptr;
    ID2D1Bitmap* b = nullptr;
    rt->CreateBitmap(D2D1::SizeU(w, h), bgra.data(), w * 4,
                     D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &b);
    return b;
}

bool ui::ShellImage(const std::wstring& name, int px, UINT& w, UINT& h, std::vector<BYTE>& bgra, bool iconOnly) {
    IShellItemImageFactory* f = nullptr;
    if (FAILED(SHCreateItemFromParsingName(name.c_str(), nullptr, IID_PPV_ARGS(&f)))) return false;
    HBITMAP hb = nullptr;
    HRESULT hr = f->GetImage(SIZE{ px, px }, SIIGBF_RESIZETOFIT | (iconOnly ? SIIGBF_ICONONLY : 0), &hb);
    f->Release();
    if (FAILED(hr) || !hb) return false;
    BITMAP bm{}; GetObjectW(hb, sizeof(bm), &bm);
    w = bm.bmWidth; h = std::abs(bm.bmHeight);
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), (LONG)w, -(LONG)h, 1, 32, BI_RGB };
    bgra.resize((size_t)w * h * 4);
    HDC screen = GetDC(nullptr);
    GetDIBits(screen, hb, 0, h, bgra.data(), &bi, DIB_RGB_COLORS);
    ReleaseDC(nullptr, screen);
    DeleteObject(hb);
    bool anyAlpha = false, straight = false;
    for (size_t i = 0; i < bgra.size(); i += 4) {
        if (bgra[i + 3]) anyAlpha = true;
        if (bgra[i] > bgra[i + 3] || bgra[i + 1] > bgra[i + 3] || bgra[i + 2] > bgra[i + 3]) straight = true;
    }
    if (!anyAlpha) for (size_t i = 3; i < bgra.size(); i += 4) bgra[i] = 255;
    else if (straight)  // alfa "reto": pré-multiplica (sem isso fica uma borda clara serrilhada)
        for (size_t i = 0; i < bgra.size(); i += 4)
            for (int c = 0; c < 3; c++) bgra[i + c] = (BYTE)(bgra[i + c] * bgra[i + 3] / 255);
    return true;
}

bool ui::FileImage(const std::wstring& path, int px, UINT& w, UINT& h, std::vector<BYTE>& bgra) {
    DWORD at = GetFileAttributesW(path.c_str());
    bool plain = at != INVALID_FILE_ATTRIBUTES && !(at & FILE_ATTRIBUTE_DIRECTORY) && !docicone::IsApp(path);
    if (plain) {
        IShellItemImageFactory* f = nullptr;
        HBITMAP hb = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&f)))) {
            if (FAILED(f->GetImage(SIZE{ px, px }, SIIGBF_RESIZETOFIT | SIIGBF_THUMBNAILONLY, &hb))) hb = nullptr;
            f->Release();
        }
        if (hb) { DeleteObject(hb); return ShellImage(path, px, w, h, bgra, false); }  // tem miniatura: o mesmo caminho de sempre
        if (docicone::Pixels(path, px, w, h, bgra)) return true;
    }
    return ShellImage(path, px, w, h, bgra, false) || ShellImage(path, px, w, h, bgra, true);
}

bool ui::LoadPng(const std::wstring& path, UINT& w, UINT& h, std::vector<BYTE>& bgra) {
    static IWICImagingFactory* wic = nullptr;
    if (!wic && FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    IWICBitmapDecoder* dec = nullptr;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) return false;
    bool ok = false;
    IWICBitmapFrameDecode* fr = nullptr;
    IWICFormatConverter* cv = nullptr;
    if (SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(wic->CreateFormatConverter(&cv)) &&
        SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
        cv->GetSize(&w, &h);
        bgra.resize((size_t)w * h * 4);
        ok = SUCCEEDED(cv->CopyPixels(nullptr, w * 4, (UINT)bgra.size(), bgra.data()));
    }
    if (cv) cv->Release();
    if (fr) fr->Release();
    dec->Release();
    return ok;
}

// Foto inteira pelo WIC (o Quick Look): a miniatura do Windows para em ~1024 px e ficava borrada numa tela de
// 3440. Gira pela orientação do EXIF (foto de celular) e reduz (Fant) até maxSide; nunca amplia.
bool ui::LoadPhoto(const std::wstring& path, UINT maxSide, UINT& w, UINT& h, std::vector<BYTE>& bgra) {
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    IWICBitmapDecoder* dec = nullptr;
    bool ok = false;
    if (SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec))) {
        IWICBitmapFrameDecode* fr = nullptr;
        if (SUCCEEDED(dec->GetFrame(0, &fr))) {
            UINT fw = 0, fh = 0; fr->GetSize(&fw, &fh);
            USHORT orient = 1;  // 1 normal, 3 = 180°, 6 = 90° horário, 8 = 90° anti-horário (2/4/5/7: espelhadas)
            IWICMetadataQueryReader* q = nullptr;
            if (SUCCEEDED(fr->GetMetadataQueryReader(&q))) {
                for (auto key : { L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}", L"System.Photo.Orientation" }) {
                    PROPVARIANT v; PropVariantInit(&v);
                    if (SUCCEEDED(q->GetMetadataByName(key, &v)) && v.vt == VT_UI2) { orient = v.uiVal; PropVariantClear(&v); break; }
                    PropVariantClear(&v);
                }
                q->Release();
            }
            IWICBitmapSource* src = fr; src->AddRef();
            if (fw && fh && maxSide && (fw > maxSide || fh > maxSide)) {
                double k = (double)maxSide / std::max(fw, fh);
                IWICBitmapScaler* sc = nullptr;
                if (SUCCEEDED(wic->CreateBitmapScaler(&sc)) &&
                    SUCCEEDED(sc->Initialize(src, std::max(1u, (UINT)(fw * k)), std::max(1u, (UINT)(fh * k)), WICBitmapInterpolationModeFant))) {
                    src->Release(); src = sc;
                } else if (sc) sc->Release();
            }
            WICBitmapTransformOptions tr = WICBitmapTransformRotate0;
            switch (orient) {
            case 2: tr = WICBitmapTransformFlipHorizontal; break;
            case 3: tr = WICBitmapTransformRotate180; break;
            case 4: tr = WICBitmapTransformFlipVertical; break;
            case 5: tr = (WICBitmapTransformOptions)(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal); break;
            case 6: tr = WICBitmapTransformRotate90; break;
            case 7: tr = (WICBitmapTransformOptions)(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal); break;
            case 8: tr = WICBitmapTransformRotate270; break;
            }
            if (tr != WICBitmapTransformRotate0) {
                IWICBitmapFlipRotator* rot = nullptr;
                if (SUCCEEDED(wic->CreateBitmapFlipRotator(&rot)) && SUCCEEDED(rot->Initialize(src, tr))) { src->Release(); src = rot; }
                else if (rot) rot->Release();
            }
            IWICFormatConverter* cv = nullptr;
            if (SUCCEEDED(wic->CreateFormatConverter(&cv)) &&
                SUCCEEDED(cv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
                cv->GetSize(&w, &h);
                if (w && h && (size_t)w * h <= 80'000'000) {
                    bgra.resize((size_t)w * h * 4);
                    ok = SUCCEEDED(cv->CopyPixels(nullptr, w * 4, (UINT)bgra.size(), bgra.data()));
                }
            }
            if (cv) cv->Release();
            src->Release();
            fr->Release();
        }
        dec->Release();
    }
    wic->Release();
    if (!ok) bgra.clear();
    return ok;
}

// Pasta dos ícones do Sequoia (icones\sequoia, ao lado de bin\): uso pessoal, ver icones\LEIA-ME.txt.
std::wstring ui::PackIcon(const std::wstring& name) {
    std::wstring root = shell::dir.substr(0, shell::dir.find_last_of(L'\\')) + L"\\icones";
    if (liquid::Leopard()) {  // Leopard: os ícones do 10.5 (icones\leopard, tools\icones-leopard.py) vencem até o extra
        std::wstring g = root + L"\\leopard\\" + name + L".png";  // (lá estão as versões Aqua dos ícones do extra)
        if (GetFileAttributesW(g.c_str()) != INVALID_FILE_ATTRIBUTES) return g;
    }
    std::wstring extra = root + L"\\extra\\" + name + L".png";  // escolhidos pelo usuário vencem
    if (GetFileAttributesW(extra.c_str()) != INVALID_FILE_ATTRIBUTES) return extra;
    if (liquid::Golden()) {  // Golden Gate: os ícones do macOS 27 (icones\goldengate, tools\icones-goldengate.py)
        std::wstring g = root + L"\\goldengate\\" + name + L".png";
        if (GetFileAttributesW(g.c_str()) != INVALID_FILE_ATTRIBUTES) return g;
    }
    if (liquid::On()) {  // Tema Tahoe: icones\tahoe antes do pacote do Sequoia
        std::wstring t = root + L"\\tahoe\\" + name + L".png";
        if (GetFileAttributesW(t.c_str()) != INVALID_FILE_ATTRIBUTES) return t;
    }
    std::wstring dir = shell::Setting(L"icones", L"");
    if (dir.empty()) dir = root + L"\\sequoia";
    std::wstring p = dir + L"\\" + name + L".png";
    return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES ? p : L"";
}

// Corta a margem transparente e centraliza num quadrado: todos os ícones ocupam o mesmo espaço,
// como no Mac (cada ícone do Windows vem com uma margem diferente).
void ui::TrimToSquare(UINT& w, UINT& h, std::vector<BYTE>& bgra) {
    int x0 = (int)w, y0 = (int)h, x1 = -1, y1 = -1;
    for (int y = 0; y < (int)h; y++)
        for (int x = 0; x < (int)w; x++)
            if (bgra[((size_t)y * w + x) * 4 + 3] > 24) { x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y); }
    if (x1 < 0) return;
    int cw = x1 - x0 + 1, ch = y1 - y0 + 1, s = std::max(cw, ch);
    if (s >= (int)w - 1 && s >= (int)h - 1) return;  // já sem margem
    std::vector<BYTE> out((size_t)s * s * 4, 0);
    int ox = (s - cw) / 2, oy = (s - ch) / 2;
    for (int y = 0; y < ch; y++)
        memcpy(&out[((size_t)(y + oy) * s + ox) * 4], &bgra[((size_t)(y + y0) * w + x0) * 4], (size_t)cw * 4);
    bgra.swap(out);
    w = h = (UINT)s;
}

// Pasta azul (as mesmas cores medidas do Arquivos), com símbolo opcional.
void ui::DrawFolder(ID2D1RenderTarget* rt, D2D1_RECT_F box, wchar_t glyph) {
    float S = box.right - box.left, x0 = box.left, y0 = box.top;
    auto R = [&](float a, float b, float c, float d) { return D2D1::RectF(x0 + a * S, y0 + b * S, x0 + c * S, y0 + d * S); };
    float rad = 0.06f * S;
    ID2D1SolidColorBrush* br = nullptr;
    rt->CreateSolidColorBrush(D2D1::ColorF(0.06f, 0.49f, 0.78f), &br);
    rt->FillRoundedRectangle(D2D1::RoundedRect(R(0.03f, 0.08f, 0.38f, 0.20f), 0.035f * S, 0.035f * S), br);
    rt->FillRoundedRectangle(D2D1::RoundedRect(R(0.03f, 0.13f, 0.97f, 0.90f), rad, rad), br);
    auto front = R(0.03f, 0.19f, 0.97f, 0.90f);
    ID2D1GradientStopCollection* stops = nullptr;
    D2D1_GRADIENT_STOP gs[2] = { { 0, D2D1::ColorF(0.32f, 0.78f, 0.98f) }, { 1, D2D1::ColorF(0.14f, 0.63f, 0.87f) } };
    rt->CreateGradientStopCollection(gs, 2, &stops);
    ID2D1LinearGradientBrush* lg = nullptr;
    rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, front.top }, { 0, front.bottom }), stops, &lg);
    rt->FillRoundedRectangle(D2D1::RoundedRect(front, rad, rad), lg);
    lg->Release(); stops->Release();
    br->SetColor(D2D1::ColorF(1, 1, 1, 0.28f));
    rt->DrawLine({ front.left + rad, front.top + 0.6f }, { front.right - rad, front.top + 0.6f }, br, 1.0f);
    if (glyph) {
        IDWriteTextFormat* tf = Format(std::round(0.40f * S), DWRITE_FONT_WEIGHT_NORMAL, true, DWRITE_TEXT_ALIGNMENT_CENTER);
        auto er = R(0.03f, 0.25f, 0.97f, 0.86f);
        br->SetColor(D2D1::ColorF(0.05f, 0.42f, 0.72f, 0.9f));
        rt->DrawTextW(&glyph, 1, tf, er, br);
        tf->Release();
    }
    br->Release();
}

// Lixeira: cesto de tela translúcido (vazio) ou com papéis (cheio).
void ui::DrawTrash(ID2D1RenderTarget* rt, D2D1_RECT_F box, bool full) {
    float S = box.right - box.left, cx = (box.left + box.right) / 2;
    float top = box.top + 0.14f * S, bottom = box.bottom - 0.04f * S;
    float wt = 0.62f * S, wb = 0.50f * S;
    ID2D1Factory* f = nullptr; rt->GetFactory(&f);
    ID2D1PathGeometry* g = nullptr; f->CreatePathGeometry(&g);
    ID2D1GeometrySink* sk = nullptr; g->Open(&sk);
    sk->BeginFigure({ cx - wt / 2, top }, D2D1_FIGURE_BEGIN_FILLED);
    sk->AddLine({ cx + wt / 2, top });
    sk->AddLine({ cx + wb / 2, bottom });
    sk->AddLine({ cx - wb / 2, bottom });
    sk->EndFigure(D2D1_FIGURE_END_CLOSED);
    sk->Close(); sk->Release();
    ID2D1SolidColorBrush* br = nullptr;
    rt->CreateSolidColorBrush(D2D1::ColorF(0.86f, 0.88f, 0.90f, 0.55f), &br);
    if (full) {  // papéis amassados aparecendo por cima
        br->SetColor(D2D1::ColorF(0.97f, 0.97f, 0.96f, 1));
        rt->FillEllipse(D2D1::Ellipse({ cx - 0.10f * S, top + 0.02f * S }, 0.15f * S, 0.10f * S), br);
        br->SetColor(D2D1::ColorF(0.90f, 0.92f, 0.95f, 1));
        rt->FillEllipse(D2D1::Ellipse({ cx + 0.10f * S, top + 0.04f * S }, 0.14f * S, 0.09f * S), br);
        br->SetColor(D2D1::ColorF(0.86f, 0.88f, 0.90f, 0.55f));
    }
    rt->FillGeometry(g, br);
    br->SetColor(D2D1::ColorF(0.55f, 0.58f, 0.62f, 0.85f));  // tela: linhas verticais
    for (int k = 1; k < 7; k++) {
        float t = k / 7.f;
        rt->DrawLine({ cx - wt / 2 + wt * t, top + 0.06f * S }, { cx - wb / 2 + wb * t, bottom - 0.02f * S }, br, std::max(0.8f, 0.012f * S));
    }
    rt->DrawGeometry(g, br, std::max(1.f, 0.02f * S));
    br->SetColor(D2D1::ColorF(0.78f, 0.80f, 0.83f, 1));  // aro
    rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - wt / 2 - 0.03f * S, top - 0.035f * S, cx + wt / 2 + 0.03f * S, top + 0.035f * S), 0.03f * S, 0.03f * S), br);
    br->Release(); g->Release(); f->Release();
}

// Reduz uma imagem BGRA pré-multiplicada para caber em maxSide × maxSide, pela média da área coberta
// (sem serrilhado). Os ícones guardados em memória ficam do tamanho em que são desenhados.
void ui::Downscale(UINT& w, UINT& h, std::vector<BYTE>& px, UINT maxSide) {
    if (!w || !h || (w <= maxSide && h <= maxSide) || px.size() < (size_t)w * h * 4) return;
    double k = (double)maxSide / std::max(w, h);
    UINT nw = std::max<UINT>(1, (UINT)std::lround(w * k)), nh = std::max<UINT>(1, (UINT)std::lround(h * k));
    double sx = (double)w / nw, sy = (double)h / nh;
    std::vector<BYTE> out((size_t)nw * nh * 4);
    for (UINT y = 0; y < nh; y++) {
        double y0 = y * sy, y1 = y0 + sy;
        for (UINT x = 0; x < nw; x++) {
            double x0 = x * sx, x1 = x0 + sx, acc[4] = {}, area = 0;
            for (UINT yy = (UINT)y0; yy < h && yy < y1; yy++) {
                double wy = std::min<double>(yy + 1, y1) - std::max<double>(yy, y0);
                for (UINT xx = (UINT)x0; xx < w && xx < x1; xx++) {
                    double wgt = wy * (std::min<double>(xx + 1, x1) - std::max<double>(xx, x0));
                    const BYTE* p = &px[((size_t)yy * w + xx) * 4];
                    for (int c = 0; c < 4; c++) acc[c] += p[c] * wgt;
                    area += wgt;
                }
            }
            BYTE* o = &out[((size_t)y * nw + x) * 4];
            for (int c = 0; c < 4; c++) o[c] = (BYTE)std::clamp(std::lround(acc[c] / area), 0L, 255L);
        }
    }
    px.swap(out);
    w = nw; h = nh;
}
