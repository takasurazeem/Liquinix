// Som do macOS na barra de menus (som.cpp):
//  - menu "Som" do ícone de som: título, controle de volume (trilho azul, botão branco; no Tahoe o botão
//    vira uma lente de vidro maior enquanto é arrastado e estica com a velocidade, como no vídeo da Apple),
//    "Saída" com as saídas de áudio de verdade (Core Audio) e "Ajustes de Som…";
//  - aviso de volume (HUD) das teclas de volume: no Sequoia o quadrado de vidro no centro embaixo com o
//    alto-falante grande e a barra de 16 segmentos; no Tahoe a pílula de vidro logo abaixo do ícone de som
//    com o nome da saída, o controle e os pontinhos de marcação. Some sozinho depois de ~1,5 s.
// Medidas do menu tiradas da captura do Tahoe (2x): 320 pt de largura, título a 20, controle a 46,
// separador a 60, "Saída" a 72, saídas a cada 32 pt com círculo de 26.
#include "shell.h"
#include "../comum/simbolos.h"
#include <dwmapi.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <algorithm>
#include <cmath>
#include "../comum/idioma.h"

namespace {

// ================= áudio (Core Audio)
// troca a saída padrão (a mesma interface que os ajustes de som do Windows usam)
MIDL_INTERFACE("f8679f50-850a-41cf-9c72-430f290290c8") IPolicyConfigSom : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};
const CLSID CLSID_PolicyConfigSom = { 0x870af99c, 0x171d, 0x4f9e, { 0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9 } };
const PROPERTYKEY KEY_FORM = { { 0x1da5d803, 0xd492, 0x4edd, { 0x8c, 0x23, 0xe0, 0xc0, 0xff, 0xee, 0x7f, 0x0e } }, 0 };  // PKEY_AudioEndpoint_FormFactor

struct Out { std::wstring id, name; UINT form = 0; bool def = false; };
struct Audio { float vol = 0; bool mute = false; bool ok = false; std::wstring name; std::vector<Out> outs; } g_au;

std::wstring DevName(IMMDevice* d, UINT* form) {
    std::wstring name;
    IPropertyStore* ps = nullptr;
    if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
        PROPVARIANT pv; PropVariantInit(&pv);
        if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) name = pv.pwszVal;
        PropVariantClear(&pv);
        if (form && SUCCEEDED(ps->GetValue(KEY_FORM, &pv)) && pv.vt == VT_UI4) *form = pv.ulVal;
        PropVariantClear(&pv);
        ps->Release();
    }
    return name;
}

IAudioEndpointVolume* Endpoint() {
    IMMDeviceEnumerator* en = nullptr;
    IAudioEndpointVolume* v = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) {
        IMMDevice* d = nullptr;
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &d))) { d->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&v); d->Release(); }
        en->Release();
    }
    return v;
}

// volume, mudo e nome da saída padrão; com `outs`, a lista das saídas ativas
void Read(bool outs) {
    g_au.ok = false;
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return;
    std::wstring def;
    IMMDevice* d = nullptr;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &d))) {
        LPWSTR id = nullptr; d->GetId(&id);
        if (id) { def = id; CoTaskMemFree(id); }
        g_au.name = DevName(d, nullptr);
        IAudioEndpointVolume* v = nullptr;
        if (SUCCEEDED(d->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, (void**)&v))) {
            BOOL m = FALSE; v->GetMasterVolumeLevelScalar(&g_au.vol); v->GetMute(&m);
            g_au.mute = m != FALSE; g_au.ok = true;
            v->Release();
        }
        d->Release();
    }
    if (outs) {
        g_au.outs.clear();
        IMMDeviceCollection* col = nullptr;
        if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) {
            UINT n = 0; col->GetCount(&n);
            for (UINT i = 0; i < n; i++) {
                IMMDevice* x = nullptr;
                if (FAILED(col->Item(i, &x))) continue;
                LPWSTR id = nullptr; x->GetId(&id);
                Out o; o.name = DevName(x, &o.form);
                if (id) { o.id = id; o.def = def == id; CoTaskMemFree(id); g_au.outs.push_back(o); }
                x->Release();
            }
            col->Release();
        }
    }
    en->Release();
}

void SetVolume(float v) {
    v = std::clamp(v, 0.f, 1.f);
    if (IAudioEndpointVolume* e = Endpoint()) {
        e->SetMasterVolumeLevelScalar(v, nullptr);
        e->SetMute(v <= 0.001f, nullptr);  // no zero, mudo (o ícone vira o alto-falante cortado, como no Mac)
        e->Release();
    }
    g_au.vol = v; g_au.mute = v <= 0.001f;
    menubar::SoundChanged(false);
}

void SetMute(bool m) {
    if (IAudioEndpointVolume* e = Endpoint()) { e->SetMute(m, nullptr); e->Release(); }
    g_au.mute = m;
    menubar::SoundChanged(false);
}

void SetDefault(const std::wstring& id) {
    IPolicyConfigSom* pc = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_PolicyConfigSom, nullptr, CLSCTX_ALL, __uuidof(IPolicyConfigSom), (void**)&pc))) {
        for (ERole r : { eConsole, eMultimedia, eCommunications }) pc->SetDefaultEndpoint(id.c_str(), r);
        pc->Release();
        Log(L"som: saída padrão trocada para %ls", id.c_str());
    }
    Read(true);
    menubar::SoundChanged(true);
}

const wchar_t* OutSymbol(const Out& o) {
    std::wstring n = o.name;
    for (auto& c : n) c = towlower(c);
    if (n.find(L"airpods") != std::wstring::npos) return L"airpods.gen3";
    if (o.form == 3 || o.form == 5 || n.find(L"fone") != std::wstring::npos || n.find(L"headphone") != std::wstring::npos ||
        n.find(L"headset") != std::wstring::npos) return L"headphones";
    if (o.form == 8 || n.find(L"hdmi") != std::wstring::npos || n.find(L"displayport") != std::wstring::npos ||
        n.find(L"monitor") != std::wstring::npos || n.find(L"nvidia") != std::wstring::npos) return L"display";
    return L"speaker.wave.2.fill";
}

const wchar_t* LevelSymbol(float v, bool mute) {
    if (mute || v < 0.001f) return L"speaker.slash.fill";
    return v < 0.34f ? L"speaker.wave.1.fill" : v < 0.67f ? L"speaker.wave.2.fill" : L"speaker.wave.3.fill";
}

// ================= janela de vidro: atrás o vidro do Shell (backdrop), na frente o desenho na GPU
struct GlassWin {
    HWND wnd = nullptr, back = nullptr;
    ui::GpuSurface s;
    bool glass = false;
};

LRESULT CALLBACK BackProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

bool MakeGlass(GlassWin& g, HINSTANCE hi, const wchar_t* cls, WNDPROC proc, bool clickThrough, const wchar_t* title) {
    WNDCLASSW wc{}; wc.lpfnWndProc = proc; wc.hInstance = hi; wc.lpszClassName = cls; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    static bool backReg;
    if (!backReg) { WNDCLASSW bc{}; bc.lpfnWndProc = BackProc; bc.hInstance = hi; bc.lpszClassName = L"ShellSomFundo"; RegisterClassW(&bc); backReg = true; }
    DWORD ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP | (clickThrough ? WS_EX_NOACTIVATE | WS_EX_TRANSPARENT : 0);
    g.wnd = CreateWindowExW(ex, cls, title, WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
    g.back = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_NOREDIRECTIONBITMAP, L"ShellSomFundo", L"", WS_POPUP,
                             0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
    if (!g.wnd || !g.back) return false;
    g.glass = backdrop::Attach(g.back);
    DWORD corner = 1;
    if (!g.glass) {  // sem a Composição: acrílico do Windows com o canto do DWM
        DestroyWindow(g.back);
        g.back = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, L"ShellSomFundo", L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
        corner = 2;
        ui::Acrylic(g.back, ui::Dark() ? 0x60201E1Eu : 0x70F4F4F4u);
    }
    DwmSetWindowAttribute(g.back, 33, &corner, sizeof(corner));
    DWORD c1 = 1; DwmSetWindowAttribute(g.wnd, 33, &c1, sizeof(c1));
    COLORREF none = 0xFFFFFFFE;
    DwmSetWindowAttribute(g.back, 34, &none, sizeof(none));
    DwmSetWindowAttribute(g.wnd, 34, &none, sizeof(none));
    g.s.hwnd = g.wnd;
    apps::ours.push_back(g.wnd);
    apps::ours.push_back(g.back);
    return true;
}

void GlassShape(GlassWin& g, int wpx, int hpx, float rpx, D2D1_COLOR_F tint, float opacity, float scale, float cx, float cy) {
    if (!g.glass) return;
    if (liquid::On() && backdrop::Real()) {
        std::vector<backdrop::Piece> pcs{ { D2D1::RectF(0, 0, (float)wpx, (float)hpx), rpx } };
        backdrop::SetPieces(g.back, (float)wpx, (float)hpx, pcs, tint, 16.f);
    } else backdrop::Set(g.back, (float)wpx, (float)hpx, rpx, tint);
    backdrop::Fade(g.back, opacity, scale, cx, cy);
}

void GlassPlace(GlassWin& g, int x, int y, int w, int h, bool activate) {
    SetWindowPos(g.back, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetWindowPos(g.wnd, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW | (activate ? 0 : SWP_NOACTIVATE));
}

void GlassHide(GlassWin& g) { ShowWindow(g.wnd, SW_HIDE); ShowWindow(g.back, SW_HIDE); }

// ================= desenho comum
const D2D1_COLOR_F BLUE = D2D1::ColorF(0.04f, 0.52f, 1, 1);
IDWriteTextFormat *g_tfTitle, *g_tfHead, *g_tfRow, *g_tfHud;

struct Pal { D2D1_COLOR_F tint, text, text2, sep, hover, track, circle, circleGlyph; };
Pal Colors() {
    bool tahoe = liquid::On();
    if (ui::Dark())
        return { D2D1::ColorF(0.13f, 0.13f, 0.14f, 0.62f), D2D1::ColorF(1, 1, 1, 0.92f), D2D1::ColorF(1, 1, 1, 0.48f), D2D1::ColorF(1, 1, 1, 0.12f),
                 D2D1::ColorF(1, 1, 1, 0.10f), D2D1::ColorF(1, 1, 1, 0.18f), D2D1::ColorF(1, 1, 1, 0.16f), D2D1::ColorF(1, 1, 1, 0.88f) };
    return { tahoe ? D2D1::ColorF(1, 1, 1, 0.62f) : D2D1::ColorF(0.97f, 0.97f, 0.98f, 0.66f), D2D1::ColorF(0, 0, 0, 0.86f), D2D1::ColorF(0, 0, 0, 0.45f),
             D2D1::ColorF(0, 0, 0, 0.10f), D2D1::ColorF(0, 0, 0, 0.07f), D2D1::ColorF(0, 0, 0, 0.10f), D2D1::ColorF(0, 0, 0, 0.09f), D2D1::ColorF(0, 0, 0, 0.62f) };
}

void Sym(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const wchar_t* name, D2D1_RECT_F r, D2D1_COLOR_F c) {
    br->SetColor(c);
    simbolo::Draw(dc, name, r, br);
}

void Text(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const std::wstring& s, D2D1_RECT_F r, IDWriteTextFormat* tf, D2D1_COLOR_F c) {
    if (!tf) return;
    br->SetColor(c);
    dc->DrawTextW(s.c_str(), (UINT32)s.size(), tf, r, br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

ID2D1RoundedRectangleGeometry* RoundGeo(ID2D1DeviceContext* dc, D2D1_ROUNDED_RECT rr) {
    ID2D1Factory* f = nullptr; dc->GetFactory(&f);
    ID2D1RoundedRectangleGeometry* g = nullptr;
    if (f) { f->CreateRoundedRectangleGeometry(rr, &g); f->Release(); }
    return g;
}

// ================= menu "Som"
// MW e TRACK_R mudam ao abrir: Tahoe = 308 (guia macOS26-Theme-Reference, seção 47: os menus da barra usam o
// painel de 308 da Central de Controle, 14 de margem); Sequoia = 320
float MW = 320, TRACK_R = 320 - 38;
const float TRACK_L = 30, SL_Y = 46, TRACK_H = 6;
const float KNOB_W = 22, KNOB_H = 16;   // Tahoe: pílula branca em repouso
const float LENS_K = 1.6f;              // Tahoe: a lente cresce ~60% enquanto arrasta (vídeo)
const float ROWS_Y = 84, ROW_H = 32;
const UINT WM_SOM_FRAME = WM_APP + 90, WM_HUD_FRAME = WM_APP + 91;
const UINT_PTR T_POLL = 1, T_FADE = 2;

GlassWin g_m;
bool g_open;
double g_openAt;
DWORD g_closedAt;
RECT g_icon;            // ícone da barra (tela) que abriu o menu
int g_hover = -1;       // 0..n-1 saídas, 1000 Ajustes de Som
bool g_drag;
// botão do controle (Tahoe): pressão animada (0 pílula branca, 1 lente) e o esticão pela velocidade
float g_press, g_stretch, g_lastKx = -1;
double g_lastFrame;

enum { H_SLIDER = 2000, H_SETTINGS = 1000 };
struct Hit { int id; D2D1_RECT_F r; };
std::vector<Hit> g_hits;

float SepY2() { return ROWS_Y + ROW_H * (float)g_au.outs.size() + 6; }
float MenuH() { return SepY2() + 33; }
float MenuR() { return liquid::On() ? 14.f : 10.f; }
float Sc() { return ui::Scale(g_m.wnd); }

// posição do centro do botão para o valor v (o botão nunca sai do trilho)
float KnobX(float v) { return TRACK_L + KNOB_W / 2 + std::clamp(v, 0.f, 1.f) * (TRACK_R - TRACK_L - KNOB_W); }
float ValueAt(float x) { return std::clamp((x - TRACK_L - KNOB_W / 2) / (TRACK_R - TRACK_L - KNOB_W), 0.f, 1.f); }

void Track(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const Pal& p, float cy, float kx, float th) {
    br->SetColor(p.track);
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(TRACK_L, cy - th / 2, TRACK_R, cy + th / 2), th / 2, th / 2), br);
    br->SetColor(BLUE);
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(TRACK_L, cy - th / 2, std::max(kx, TRACK_L + th), cy + th / 2), th / 2, th / 2), br);
}

void Shadow(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, D2D1_RECT_F k, float rad, float a) {
    for (int i = 3; i >= 1; i--) {
        float g = i * 1.1f;
        br->SetColor(D2D1::ColorF(0, 0, 0, a / (i + 1)));
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(k.left - g, k.top - g + 1.5f, k.right + g, k.bottom + g + 1.5f), rad + g, rad + g), br);
    }
}

void Slider(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const Pal& p, float v) {
    const float cy = SL_Y;
    const bool dark = ui::Dark();
    Sym(dc, br, L"speaker.fill", D2D1::RectF(12, cy - 5.5f, 24, cy + 5.5f), p.text2);
    Sym(dc, br, L"speaker.wave.3.fill", D2D1::RectF(TRACK_R + 8, cy - 6, TRACK_R + 26, cy + 6), p.text2);
    float kx = KnobX(v);
    Track(dc, br, p, cy, kx, TRACK_H);
    g_hits.push_back({ H_SLIDER, D2D1::RectF(TRACK_L - 10, cy - 13, TRACK_R + 10, cy + 13) });

    if (!liquid::On()) {  // Sequoia: botão redondo branco com sombra curta e um fio de borda
        float r = 9;
        D2D1_RECT_F k = D2D1::RectF(kx - r, cy - r, kx + r, cy + r);
        Shadow(dc, br, k, r, 0.16f);
        br->SetColor(g_drag ? D2D1::ColorF(0.93f, 0.93f, 0.94f) : D2D1::ColorF(1, 1, 1));
        dc->FillEllipse(D2D1::Ellipse({ kx, cy }, r, r), br);
        br->SetColor(D2D1::ColorF(0, 0, 0, dark ? 0.25f : 0.12f));
        dc->DrawEllipse(D2D1::Ellipse({ kx, cy }, r - 0.25f, r - 0.25f), br, 0.5f);
        return;
    }

    // Tahoe: pílula branca em repouso; arrastando, uma lente de vidro maior que mostra o trilho ampliado,
    // com borda azulada da luz que ela entorta, e que estica na direção do movimento
    float pr = std::clamp(g_press, 0.f, 1.f);
    float grow = 1 + (LENS_K - 1) * pr;
    float kw = KNOB_W * grow * (1 + g_stretch), kh = KNOB_H * grow * (1 - g_stretch * 0.35f);
    float rad = kh / 2;
    D2D1_RECT_F k = D2D1::RectF(kx - kw / 2, cy - kh / 2, kx + kw / 2, cy + kh / 2);
    D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(k, rad, rad);
    Shadow(dc, br, k, rad, 0.10f + 0.06f * pr);
    if (pr < 0.999f) {  // pílula branca (some conforme a lente aparece)
        br->SetColor(D2D1::ColorF(1, 1, 1, 1 - pr));
        dc->FillRoundedRectangle(rr, br);
        br->SetColor(D2D1::ColorF(0, 0, 0, 0.08f * (1 - pr)));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(k.left + 0.25f, k.top + 0.25f, k.right - 0.25f, k.bottom - 0.25f), rad, rad), br, 0.5f);
    }
    if (pr > 0.001f) {
        ID2D1RoundedRectangleGeometry* geo = RoundGeo(dc, rr);
        if (geo) {
            dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), geo, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), pr), nullptr);
            br->SetColor(D2D1::ColorF(1, 1, 1, dark ? 0.10f : 0.30f));  // o corpo do vidro, quase transparente
            dc->FillRoundedRectangle(rr, br);
            // refração: o trilho embaixo aparece ampliado (mais grosso) dentro da lente
            D2D1_MATRIX_3X2_F old; dc->GetTransform(&old);
            dc->SetTransform(D2D1::Matrix3x2F::Scale(1.18f, 1.9f, { kx, cy }) * old);
            Track(dc, br, p, cy, kx - kw * 0.02f, TRACK_H);  // no vídeo o azul vai até perto do meio da lente
            dc->SetTransform(old);
            // luz por dentro: forte em cima, fraca embaixo
            D2D1_GRADIENT_STOP gs[3] = { { 0, D2D1::ColorF(1, 1, 1, 0.95f) }, { 0.5f, D2D1::ColorF(1, 1, 1, 0.25f) }, { 1, D2D1::ColorF(1, 1, 1, 0.65f) } };
            ID2D1GradientStopCollection* stops = nullptr;
            if (SUCCEEDED(dc->CreateGradientStopCollection(gs, 3, &stops))) {
                ID2D1LinearGradientBrush* lg = nullptr;
                if (SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, k.top }, { 0, k.bottom }), stops, &lg))) {
                    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(k.left + 1.2f, k.top + 1.2f, k.right - 1.2f, k.bottom - 1.2f), rad - 1.2f, rad - 1.2f), lg, 1.4f);
                    lg->Release();
                }
                stops->Release();
            }
            dc->PopLayer();
            geo->Release();
        }
        // borda: o azul do trilho entortado pela lente
        br->SetColor(D2D1::ColorF(0.25f, 0.62f, 1, 0.55f * pr));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(k.left + 0.4f, k.top + 0.4f, k.right - 0.4f, k.bottom - 0.4f), rad - 0.4f, rad - 0.4f), br, 0.9f);
    }
}

void PaintMenu() {
    if (!g_open) return;
    float sc = Sc();
    const float H = MenuH(), R = MenuR();
    int wpx = (int)std::lround(MW * sc), hpx = (int)std::lround(H * sc);
    auto* dc = ui::GpuBegin(g_m.s, wpx, hpx, sc);
    if (!dc) return;
    Pal p = Colors();
    const bool dark = ui::Dark();
    anim::Panel mo = anim::PanelIn(anim::Now() - g_openAt);  // a mola comum dos painéis
    float t = mo.fade;
    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(p.text, &br);
    if (!br) { ui::GpuEnd(g_m.s); return; }
    dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), t), nullptr);
    D2D1_RECT_F all = D2D1::RectF(0, 0, MW, H);
    if (!g_m.glass) { br->SetColor(dark ? D2D1::ColorF(0.17f, 0.17f, 0.18f, 0.80f) : D2D1::ColorF(0.965f, 0.965f, 0.965f, 0.80f)); dc->FillRoundedRectangle(D2D1::RoundedRect(all, R, R), br); }
    if (liquid::On()) liquid::Rim(dc, all, R, dark);
    else {  // borda dos menus do Sequoia: escura por fora, clara por dentro
        br->SetColor(dark ? D2D1::ColorF(0, 0, 0, 0.45f) : D2D1::ColorF(0, 0, 0, 0.12f));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, MW - 0.5f, H - 0.5f), R, R), br, 1);
        br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.14f) : D2D1::ColorF(1, 1, 1, 0.5f));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(1.5f, 1.5f, MW - 1.5f, H - 1.5f), R - 1, R - 1), br, 1);
    }
    g_hits.clear();
    Text(dc, br, Tr(L"Som"), D2D1::RectF(14, 8, MW - 14, 32), g_tfTitle, p.text);
    Slider(dc, br, p, g_au.mute ? 0 : g_au.vol);
    auto sep = [&](float y) { br->SetColor(p.sep); dc->FillRectangle(D2D1::RectF(10, y, MW - 10, y + 1 / Sc()), br); };
    sep(60);
    Text(dc, br, Tr(L"Saída"), D2D1::RectF(14, 62, MW - 14, 82), g_tfHead, p.text2);
    for (size_t i = 0; i < g_au.outs.size(); i++) {
        const Out& o = g_au.outs[i];
        float y = ROWS_Y + ROW_H * i, cy = y + ROW_H / 2;
        D2D1_RECT_F row = D2D1::RectF(5, y + 1, MW - 5, y + ROW_H - 1);
        if (g_hover == (int)i) { br->SetColor(p.hover); dc->FillRoundedRectangle(D2D1::RoundedRect(row, 7, 7), br); }
        float r = 13, cx = 14 + r;
        br->SetColor(o.def ? BLUE : p.circle);
        dc->FillEllipse(D2D1::Ellipse({ cx, cy }, r, r), br);
        const wchar_t* s = OutSymbol(o);
        float gh = wcscmp(s, L"headphones") == 0 ? 7.5f : 6.5f;
        Sym(dc, br, s, D2D1::RectF(cx - 8, cy - gh, cx + 8, cy + gh), o.def ? D2D1::ColorF(1, 1, 1, 1) : p.circleGlyph);
        Text(dc, br, o.name.empty() ? Tr(L"(sem nome)") : o.name, D2D1::RectF(48, y, MW - 14, y + ROW_H), g_tfRow, p.text);
        g_hits.push_back({ (int)i, row });
    }
    float s2 = SepY2();
    sep(s2);
    D2D1_RECT_F set = D2D1::RectF(5, s2 + 5, MW - 5, s2 + 28);
    if (g_hover == H_SETTINGS) { br->SetColor(p.hover); dc->FillRoundedRectangle(D2D1::RoundedRect(set, 6, 6), br); }
    Text(dc, br, Tr(L"Ajustes de Som…"), D2D1::RectF(14, set.top, MW - 14, set.bottom), g_tfRow, p.text);
    g_hits.push_back({ H_SETTINGS, set });
    dc->PopLayer();
    br->Release();
    ui::GpuEnd(g_m.s);
    ui::GpuScale(g_m.s, mo.scale, wpx / 2.f, 0);
    GlassShape(g_m, wpx, hpx, R * sc, p.tint, t, mo.scale, wpx / 2.f, 0);
}

// lente do Tahoe: aproxima a pressão do alvo e solta o esticão; true enquanto ainda mexe
bool StepKnob() {
    double now = anim::Now();
    float dt = (float)std::clamp(now - g_lastFrame, 1.0, 50.0);
    g_lastFrame = now;
    float target = g_drag ? 1.f : 0.f;
    float k = 1 - std::exp(-dt / (g_drag ? 55.f : 80.f));
    g_press += (target - g_press) * k;
    if (std::fabs(target - g_press) < 0.004f) g_press = target;
    g_stretch *= std::exp(-dt / 90.f);
    if (g_stretch < 0.003f) g_stretch = 0;
    return g_press != target || g_stretch > 0;
}

void Animate() { ui::FrameClock(g_m.wnd, WM_SOM_FRAME, true); }

void CloseMenu() {
    if (!g_open) return;
    g_open = false;
    g_closedAt = GetTickCount();
    if (g_drag) { g_drag = false; ReleaseCapture(); }
    g_press = 0; g_stretch = 0; g_hover = -1;
    KillTimer(g_m.wnd, T_POLL);
    ui::FrameClock(g_m.wnd, WM_SOM_FRAME, false);
    GlassHide(g_m);
    menubar::SoundChanged(false);  // o destaque do ícone sai
}

void DragTo(float x) {
    float v = ValueAt(x);
    float kx = KnobX(v);
    if (g_lastKx >= 0 && liquid::On()) {  // esticão: proporcional à velocidade do botão
        float dx = std::fabs(kx - g_lastKx);
        g_stretch = std::min(0.32f, std::max(g_stretch, dx * 0.018f));
    }
    g_lastKx = kx;
    SetVolume(v);
    PaintMenu();
    Animate();
}

int HitAt(D2D1_POINT_2F p) {
    for (auto& h : g_hits) if (p.x >= h.r.left && p.x < h.r.right && p.y >= h.r.top && p.y < h.r.bottom) return h.id;
    return -1;
}

D2D1_POINT_2F Pt(HWND h, LPARAM l) { float sc = ui::Scale(h); return { (short)LOWORD(l) / sc, (short)HIWORD(l) / sc }; }

LRESULT CALLBACK MenuProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); PaintMenu(); return 0; }
    case WM_SOM_FRAME: {
        bool moving = StepKnob();
        PaintMenu();
        if (!moving && anim::PanelIn(anim::Now() - g_openAt).done) ui::FrameClock(h, WM_SOM_FRAME, false);
        return 0;
    }
    case WM_TIMER:  // volume mudado por fora (teclas, outros apps) aparece no controle
        if (w == T_POLL && g_open && !g_drag) {
            float v = g_au.vol; bool mu = g_au.mute; size_t n = g_au.outs.size();
            Read(false);
            if (v != g_au.vol || mu != g_au.mute || n != g_au.outs.size()) PaintMenu();
        }
        return 0;
    case WM_LBUTTONDOWN: {
        D2D1_POINT_2F p = Pt(h, l);
        int id = HitAt(p);
        if (id == H_SLIDER) {
            g_drag = true; g_lastKx = -1; g_lastFrame = anim::Now();
            SetCapture(h);
            DragTo(p.x);
        } else if (id == H_SETTINGS) {
            CloseMenu();
            shell::OpenSettings(L"som");
        } else if (id >= 0 && id < (int)g_au.outs.size()) {
            if (!g_au.outs[id].def) {
                SetDefault(g_au.outs[id].id);
                if (g_hover >= (int)g_au.outs.size()) g_hover = -1;
                // o menu muda de altura se a lista mudou
                RECT wr; GetWindowRect(h, &wr);
                float sc = Sc();
                GlassPlace(g_m, wr.left, wr.top, (int)std::lround(MW * sc), (int)std::lround(MenuH() * sc), false);
                PaintMenu();
            }
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        D2D1_POINT_2F p = Pt(h, l);
        if (g_drag) { DragTo(p.x); return 0; }
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 }; TrackMouseEvent(&tme);
        int id = HitAt(p);
        int hv = (id == H_SETTINGS || (id >= 0 && id < (int)g_au.outs.size())) ? id : -1;
        if (hv != g_hover) { g_hover = hv; PaintMenu(); }
        return 0;
    }
    case WM_MOUSELEAVE: if (g_hover != -1) { g_hover = -1; PaintMenu(); } return 0;
    case WM_LBUTTONUP:
        if (g_drag) {
            g_drag = false; g_lastKx = -1;
            ReleaseCapture();
            shell::Sound(L"Pop");  // como no Mac: o estalo no volume novo ao soltar
            g_lastFrame = anim::Now();
            Animate();
        }
        return 0;
    case WM_CAPTURECHANGED: if (g_drag && (HWND)l != h) { g_drag = false; g_lastKx = -1; Animate(); } return 0;
    case WM_MOUSEWHEEL:
        SetVolume((g_au.mute ? 0 : g_au.vol) + GET_WHEEL_DELTA_WPARAM(w) / 120.f / 32.f);
        PaintMenu();
        return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) CloseMenu();
        else if (w == VK_RIGHT || w == VK_UP) { SetVolume(g_au.vol + 1 / 16.f); PaintMenu(); }
        else if (w == VK_LEFT || w == VK_DOWN) { SetVolume(g_au.vol - 1 / 16.f); PaintMenu(); }
        return 0;
    case WM_ACTIVATE: if (LOWORD(w) == WA_INACTIVE) CloseMenu(); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ================= aviso de volume (HUD)
GlassWin g_h;
bool g_hudOn, g_hudTahoe;
double g_hudShownAt, g_hudFadeAt;   // ms (anim::Now); fadeAt 0 = ainda não está sumindo
int g_hudX, g_hudY;
const float SQ = 200, SQ_R = 18;                 // Sequoia: quadrado de 200 pt
const float PILL_W = 224, PILL_H = 50, PILL_R = 15;  // Tahoe: pílula embaixo do ícone
const double HUD_HOLD = 1500, HUD_FADE_SQ = 400, HUD_FADE_T = 260;

float HudW() { return g_hudTahoe ? PILL_W : SQ; }
float HudH() { return g_hudTahoe ? PILL_H : SQ; }

void PaintHud() {
    if (!g_hudOn) return;
    float sc = ui::Scale(g_h.wnd);
    const float W = HudW(), H = HudH(), R = g_hudTahoe ? PILL_R : SQ_R;
    int wpx = (int)std::lround(W * sc), hpx = (int)std::lround(H * sc);
    double now = anim::Now();
    float fade = 1, scale = 1;
    if (g_hudTahoe) {  // aparece crescendo um pouco a partir do ícone, com a mola do Mac
        float sp = anim::Spring((float)((now - g_hudShownAt) / 1000.0), 0.38f, 0.8f);
        scale = 0.86f + 0.14f * sp;
        fade = std::min(1.f, (float)((now - g_hudShownAt) / 120.0));
    }
    if (g_hudFadeAt > 0) {
        float f = 1 - (float)((now - g_hudFadeAt) / (g_hudTahoe ? HUD_FADE_T : HUD_FADE_SQ));
        fade *= std::clamp(f, 0.f, 1.f);
        if (g_hudTahoe) scale *= 0.96f + 0.04f * std::clamp(f, 0.f, 1.f);
    }
    auto* dc = ui::GpuBegin(g_h.s, wpx, hpx, sc);
    if (!dc) return;
    const bool dark = ui::Dark();
    Pal p = Colors();
    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(p.text, &br);
    if (!br) { ui::GpuEnd(g_h.s); return; }
    float cx = g_hudTahoe ? W * 0.7f : W / 2, cy = g_hudTahoe ? 0 : H / 2;  // centro da escala (Tahoe: perto do ícone)
    dc->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale, { cx, cy }));
    dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), fade), nullptr);
    D2D1_RECT_F all = D2D1::RectF(0, 0, W, H);
    if (!g_h.glass) { br->SetColor(dark ? D2D1::ColorF(0.17f, 0.17f, 0.18f, 0.85f) : D2D1::ColorF(0.94f, 0.94f, 0.95f, 0.85f)); dc->FillRoundedRectangle(D2D1::RoundedRect(all, R, R), br); }
    const float v = g_au.mute ? 0 : g_au.vol;
    if (!g_hudTahoe) {
        if (liquid::On()) liquid::Rim(dc, all, R, dark);
        else { br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.10f) : D2D1::ColorF(1, 1, 1, 0.35f)); dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, W - 0.5f, H - 0.5f), R, R), br, 1); }
        // alto-falante grande: a caixa é a da versão com 3 ondas, alinhada à esquerda (o corpo não pula)
        D2D1_COLOR_F ink = dark ? D2D1::ColorF(1, 1, 1, 0.85f) : D2D1::ColorF(0.05f, 0.04f, 0.12f, 0.78f);
        const wchar_t* s = LevelSymbol(g_au.vol, g_au.mute);
        float gw = !wcscmp(s, L"speaker.wave.3.fill") ? 108.f : !wcscmp(s, L"speaker.wave.2.fill") ? 92.f : !wcscmp(s, L"speaker.wave.1.fill") ? 76.f : !wcscmp(s, L"speaker.slash.fill") ? 92.f : 58.f;
        float left = W / 2 - 54;
        Sym(dc, br, s, D2D1::RectF(left, 46, left + gw, 118), ink);
        // barra de 16 segmentos: trilho escuro, segmentos claros (com 1/64 de passo, o último pela metade)
        const float bx = 19, bw = W - 38, by = 172, bh = 9, gap = 1.2f;
        br->SetColor(dark ? D2D1::ColorF(0, 0, 0, 0.55f) : D2D1::ColorF(0.05f, 0.04f, 0.12f, 0.72f));
        dc->FillRectangle(D2D1::RectF(bx, by, bx + bw, by + bh), br);
        float segW = (bw - gap * 17) / 16, filled = v * 16;
        br->SetColor(D2D1::ColorF(1, 1, 1, dark ? 0.92f : 0.95f));
        for (int i = 0; i < 16; i++) {
            float f = std::clamp(filled - i, 0.f, 1.f);
            if (f <= 0.01f) break;
            float x0 = bx + gap + i * (segW + gap);
            dc->FillRectangle(D2D1::RectF(x0, by + gap, x0 + segW * f, by + bh - gap), br);
        }
    } else {
        liquid::Rim(dc, all, R, dark);
        D2D1_COLOR_F ink = dark ? D2D1::ColorF(1, 1, 1, 0.95f) : D2D1::ColorF(0, 0, 0, 0.82f);
        D2D1_COLOR_F ink2 = dark ? D2D1::ColorF(1, 1, 1, 0.75f) : D2D1::ColorF(0, 0, 0, 0.55f);
        Text(dc, br, g_au.name.empty() ? Tr(L"Saída de Som") : g_au.name, D2D1::RectF(12, 5, W - 12, 21), g_tfHud, ink);
        const float tl = 30, tr = W - 32, ty = 30;
        Sym(dc, br, g_au.mute || v < 0.001f ? L"speaker.slash.fill" : L"speaker.fill", D2D1::RectF(11, ty - 5, 24, ty + 5), ink2);
        Sym(dc, br, L"speaker.wave.3.fill", D2D1::RectF(tr + 6, ty - 5.5f, tr + 22, ty + 5.5f), ink2);
        br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.22f) : D2D1::ColorF(0, 0, 0, 0.14f));
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tl, ty - 2.5f, tr, ty + 2.5f), 2.5f, 2.5f), br);
        br->SetColor(ink);
        if (v > 0.001f) dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tl, ty - 2.5f, tl + std::max(5.f, v * (tr - tl)), ty + 2.5f), 2.5f, 2.5f), br);
        // pontinhos de marcação: um a cada passo da tecla (16 passos, 17 pontos)
        br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.55f) : D2D1::ColorF(0, 0, 0, 0.38f));
        for (int i = 0; i <= 16; i++) dc->FillEllipse(D2D1::Ellipse({ tl + 1.2f + (tr - tl - 2.4f) * i / 16.f, ty + 9.5f }, 1.2f, 1.2f), br);
    }
    dc->PopLayer();
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    br->Release();
    ui::GpuEnd(g_h.s);
    D2D1_COLOR_F tint = g_hudTahoe ? (dark ? D2D1::ColorF(0.12f, 0.12f, 0.14f, 0.45f) : D2D1::ColorF(1, 1, 1, 0.40f))
                                   : (dark ? D2D1::ColorF(0.14f, 0.14f, 0.15f, 0.55f) : D2D1::ColorF(0.93f, 0.93f, 0.95f, 0.45f));
    GlassShape(g_h, wpx, hpx, R * sc, tint, fade, scale, cx * sc, cy * sc);
}

void HideHud() {
    if (!g_hudOn) return;
    g_hudOn = false;
    KillTimer(g_h.wnd, T_FADE);
    ui::FrameClock(g_h.wnd, WM_HUD_FRAME, false);
    GlassHide(g_h);
    menubar::SoundChanged(false);
}

void PlaceHud() {
    float sc = ui::Scale(g_h.wnd);
    RECT icon{};
    bool hasIcon = menubar::SoundIcon(&icon);
    POINT ref = hasIcon ? POINT{ (icon.left + icon.right) / 2, icon.bottom } : POINT{ 0, 0 };
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint(ref, MONITOR_DEFAULTTOPRIMARY), &mi);
    // a escala segue o monitor (a janela pode estar em outro)
    UINT dpi = GetDpiForWindow(menubar::Hwnd());
    if (dpi) sc = dpi / 96.f;
    int w = (int)std::lround(HudW() * sc), h = (int)std::lround(HudH() * sc);
    if (g_hudTahoe) {
        // como na captura do Tahoe: logo abaixo da barra, com o ícone de som perto da ponta direita
        int right = hasIcon ? ref.x + (int)std::lround(56 * sc) : mi.rcMonitor.right - (int)std::lround(10 * sc);
        g_hudX = std::clamp(right - w, (int)mi.rcMonitor.left + (int)(6 * sc), (int)mi.rcMonitor.right - w - (int)(6 * sc));
        g_hudY = mi.rcMonitor.top + menubar::Height() + (int)std::lround(6 * sc);
    } else {  // Sequoia: centro, embaixo (acima do Dock)
        g_hudX = mi.rcMonitor.left + (mi.rcMonitor.right - mi.rcMonitor.left - w) / 2;
        g_hudY = mi.rcMonitor.bottom - (int)std::lround(140 * sc) - h;
    }
    GlassPlace(g_h, g_hudX, g_hudY, w, h, false);
}

void ShowHud() {
    bool tahoe = liquid::On();
    double now = anim::Now();
    bool fresh = !g_hudOn || tahoe != g_hudTahoe || (g_hudFadeAt > 0 && now - g_hudFadeAt > 120);
    g_hudTahoe = tahoe;
    if (fresh) g_hudShownAt = now;
    g_hudFadeAt = 0;
    g_hudOn = true;
    if (fresh) PlaceHud();
    PaintHud();
    SetTimer(g_h.wnd, T_FADE, (UINT)HUD_HOLD, nullptr);
    ui::FrameClock(g_h.wnd, WM_HUD_FRAME, true);
    menubar::SoundChanged(false);
}

LRESULT CALLBACK HudProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); PaintHud(); return 0; }
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_TIMER:
        if (w == T_FADE) { KillTimer(h, T_FADE); g_hudFadeAt = anim::Now(); ui::FrameClock(h, WM_HUD_FRAME, true); }
        return 0;
    case WM_HUD_FRAME: {
        double now = anim::Now();
        if (g_hudFadeAt > 0 && now - g_hudFadeAt >= (g_hudTahoe ? HUD_FADE_T : HUD_FADE_SQ)) { HideHud(); return 0; }
        PaintHud();
        bool appearing = g_hudTahoe && now - g_hudShownAt < 700;
        if (!appearing && g_hudFadeAt == 0) ui::FrameClock(h, WM_HUD_FRAME, false);
        return 0;
    }
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

bool som::Create(HINSTANCE hi) {
    if (!MakeGlass(g_m, hi, L"ShellSomMenu", MenuProc, false, L"Som")) return false;
    if (!MakeGlass(g_h, hi, L"ShellSomAviso", HudProc, true, L"Volume")) return false;
    g_tfTitle = ui::Format(13, DWRITE_FONT_WEIGHT_BOLD);
    g_tfHead = ui::Format(12, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_tfRow = ui::Format(13, DWRITE_FONT_WEIGHT_NORMAL);
    g_tfHud = ui::Format(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    DWRITE_TRIMMING tr{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
    for (auto* tf : { g_tfTitle, g_tfHead, g_tfRow, g_tfHud })
        if (tf) { tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); tf->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); tf->SetTrimming(&tr, nullptr); }
    return true;
}

bool som::IsOpen() { return g_open; }
bool som::HudLit() { return g_hudOn && g_hudTahoe; }

void som::Toggle(RECT icon) {
    if (g_open) { CloseMenu(); return; }
    if (GetTickCount() - g_closedAt < 300) return;  // o clique no ícone acabou de fechar o menu (perdeu o foco)
    HideHud();
    Read(true);
    g_icon = icon;
    MW = liquid::On() ? 308.f : 320.f; TRACK_R = MW - 38;
    g_open = true;
    g_openAt = anim::Now();
    g_hover = -1; g_press = 0; g_stretch = 0;
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint({ icon.left, icon.bottom }, MONITOR_DEFAULTTOPRIMARY), &mi);
    float sc = ui::Scale(menubar::Hwnd());
    int w = (int)std::lround(MW * sc), h = (int)std::lround(MenuH() * sc);
    int x = std::clamp((int)icon.left, (int)mi.rcMonitor.left + (int)(5 * sc), (int)mi.rcMonitor.right - w - (int)(5 * sc));
    int y = icon.bottom + (int)std::lround((liquid::On() ? 3 : 1) * sc);
    GlassPlace(g_m, x, y, w, h, true);
    PaintMenu();
    SetForegroundWindow(g_m.wnd);
    SetFocus(g_m.wnd);
    SetTimer(g_m.wnd, T_POLL, 500, nullptr);
    Animate();
    menubar::SoundChanged(false);  // destaque do ícone
}

void som::Key(UINT vk, bool fine) {
    Read(false);
    if (!g_au.ok) return;
    const float steps = fine ? 64.f : 16.f;  // Shift+Alt: quartos de passo, como Shift+Option no Mac
    if (vk == VK_VOLUME_MUTE) SetMute(!g_au.mute);
    else if (vk == VK_VOLUME_UP) {
        float v = g_au.mute ? std::max(g_au.vol, 0.f) : g_au.vol;
        v = std::min(1.f, (std::floor(v * steps + 0.5f) + 1) / steps);
        if (g_au.mute && g_au.vol > 0.001f) v = g_au.vol;  // estava mudo: volta ao volume de antes
        SetVolume(v);
    } else if (vk == VK_VOLUME_DOWN) {
        float v = std::max(0.f, (std::floor(g_au.vol * steps + 0.5f) - 1) / steps);
        SetVolume(g_au.mute ? 0 : v);
    }
    if (g_open) { PaintMenu(); return; }  // com o menu aberto, o controle dele mostra
    if (ilha::Volume(g_au.mute ? 0 : g_au.vol, g_au.mute)) { HideHud(); return; }  // Ilha Dinâmica ligada: o volume aparece nela
    ShowHud();
}
