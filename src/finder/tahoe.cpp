// Tema macOS Tahoe (Liquid Glass) nos apps fora do Shell: Finder e Ajustes do Sistema.
// Ligado em Ajustes do Sistema > Aparência > Tema (liquid_glass=1 em %APPDATA%\Shell\ajustes.txt);
// desligado, nada daqui é usado e as janelas ficam como no Sequoia.
//
// Janela: o DWM só arredonda 8 px e pinta o acrílico no retângulo inteiro. No Tahoe a janela tem
// cantos de 26 pt; então o acrílico do DWM é desligado e o desfoque vem de um HostBackdropBrush da
// Composição (como o backdrop.cpp do Shell), recortado com antisserrilhado no mesmo raio que o
// conteúdo D2D usa. Nada é gravado no sistema: tudo são atributos da própria janela.
#include "finder.h"
#include <dwmapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <DispatcherQueue.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <windows.ui.composition.interop.h>
#include <windows.graphics.effects.interop.h>
#include <winrt/Windows.Graphics.Effects.h>
#include <d2d1effects.h>
#include <map>

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "CoreMessaging.lib")
#pragma comment(lib, "dxguid.lib")

namespace wuc = winrt::Windows::UI::Composition;
namespace abi = ABI::Windows::UI::Composition::Desktop;

namespace {
// Fosco + vibrância do Liquid Glass (o mesmo do backdrop.cpp do Shell): efeito do D2D descrito para a
// Composição. O fundo não pode ser transformado (lente), mas desfoque e saturação são aceitos.
namespace abifx = ABI::Windows::Graphics::Effects;
using winrt::Windows::Graphics::Effects::IGraphicsEffect;
using winrt::Windows::Graphics::Effects::IGraphicsEffectSource;
struct Fx : winrt::implements<Fx, IGraphicsEffect, IGraphicsEffectSource, abifx::IGraphicsEffectD2D1Interop> {
    winrt::hstring name;
    GUID id;
    std::vector<std::pair<UINT, winrt::Windows::Foundation::IInspectable>> props;
    IGraphicsEffectSource src{ nullptr };
    Fx(const wchar_t* n, GUID g, IGraphicsEffectSource s) : name(n), id(g), src(std::move(s)) {}
    winrt::hstring Name() { return name; }
    void Name(winrt::hstring const& n) { name = n; }
    HRESULT __stdcall GetEffectId(GUID* g) noexcept override { *g = id; return S_OK; }
    HRESULT __stdcall GetNamedPropertyMapping(LPCWSTR, UINT*, abifx::GRAPHICS_EFFECT_PROPERTY_MAPPING*) noexcept override { return E_INVALIDARG; }
    HRESULT __stdcall GetPropertyCount(UINT* c) noexcept override { *c = (UINT)props.size(); return S_OK; }
    HRESULT __stdcall GetProperty(UINT i, ABI::Windows::Foundation::IPropertyValue** v) noexcept override {
        for (auto& [k, val] : props) if (k == i) { *v = val.as<ABI::Windows::Foundation::IPropertyValue>().detach(); return S_OK; }
        return E_INVALIDARG;
    }
    HRESULT __stdcall GetSource(UINT i, abifx::IGraphicsEffectSource** s) noexcept override {
        if (i != 0 || !src) return E_INVALIDARG;
        src.as<abifx::IGraphicsEffectSource>().copy_to(s);
        return S_OK;
    }
    HRESULT __stdcall GetSourceCount(UINT* c) noexcept override { *c = 1; return S_OK; }
};

bool g_on = false, g_reduce = false, g_tint = true, g_golden = false, g_leopard = false;
int g_folderColor = -1;  // Aparência > Cor das pastas (cor_pastas; -1 = automática)
std::wstring g_accent = L"azul";  // Ajustes > Aparência > Cor de destaque (destaque=)
FILETIME g_stamp{};
bool g_read = false;

struct Win {
    wuc::Desktop::DesktopWindowTarget target{ nullptr };
    wuc::ContainerVisual root{ nullptr };
    wuc::CompositionRoundedRectangleGeometry geo{ nullptr };
    wuc::CompositionColorBrush tint{ nullptr };
};
winrt::Windows::System::DispatcherQueueController g_dq{ nullptr };
wuc::Compositor g_comp{ nullptr };
std::map<HWND, Win> g_wins;

std::wstring Path() {
    PWSTR p = nullptr;
    std::wstring f;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) { f = std::wstring(p) + L"\\Shell\\ajustes.txt"; CoTaskMemFree(p); }
    return f;
}

bool ReadOn() {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, Path().c_str(), L"r, ccs=UTF-8") || !fp) return false;
    bool liquid = false, reduce = false, golden = false, leo = false;
    int folder = -1;
    wchar_t line[512];
    while (fgetws(line, 512, fp)) {
        std::wstring s = line;
        while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ')) s.pop_back();
        if (s == L"liquid_glass=1") liquid = true;
        if (s == L"reduzir_transparencia=1") reduce = true;
        if (s == L"golden_gate=1") golden = true;
        if (s == L"leopard=1") leo = true;
        if (s.rfind(L"cor_pastas=", 0) == 0) folder = _wtoi(s.c_str() + 11);
        if (s.rfind(L"destaque=", 0) == 0) g_accent = s.substr(9);
        if (s == L"tingir_janelas=0") g_tint = false;
        if (s == L"tingir_janelas=1") g_tint = true;
    }
    fclose(fp);
    g_reduce = reduce;
    g_golden = liquid && golden;
    g_leopard = !liquid && leo;  // Mac OS X Leopard: só sem o Liquid Glass
    g_folderColor = folder;
    return liquid;
}

void Dwm(HWND h, bool tahoe) {
    int corner = tahoe ? 1 /*DWMWCP_DONOTROUND*/ : 0 /*DWMWCP_DEFAULT*/;
    DwmSetWindowAttribute(h, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner, sizeof(corner));
    if (tahoe) {
        COLORREF none = 0xFFFFFFFE;  // DWMWA_COLOR_NONE: sem a borda retangular do DWM
        DwmSetWindowAttribute(h, 34 /*DWMWA_BORDER_COLOR*/, &none, sizeof(none));
        int bd = 1;  // DWMSBT_NONE: o desfoque vem da Composição, recortado no raio certo
        DwmSetWindowAttribute(h, 38 /*DWMWA_SYSTEMBACKDROP_TYPE*/, &bd, sizeof(bd));
        BOOL on = TRUE;
        DwmSetWindowAttribute(h, 17 /*DWMWA_USE_HOSTBACKDROPBRUSH*/, &on, sizeof(on));
    }
    // sem sombra retangular do DWM no Tahoe (a sombra redonda é a de shadow.cpp)
    DWMNCRENDERINGPOLICY pol = tahoe ? DWMNCRP_DISABLED : DWMNCRP_USEWINDOWSTYLE;
    DwmSetWindowAttribute(h, DWMWA_NCRENDERING_POLICY, &pol, sizeof(pol));
}

// fundo da janela desfocado (16) e com a cor reforçada (1,65), como no Shell; sem o efeito, o fundo puro
wuc::CompositionBrush Frost() {
    using winrt::Windows::Foundation::PropertyValue;
    try {
        auto blur = winrt::make_self<Fx>(L"fosco", CLSID_D2D1GaussianBlur, wuc::CompositionEffectSourceParameter(L"fundo"));
        blur->props = { { D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, PropertyValue::CreateSingle(16.f) },
                        { D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION, PropertyValue::CreateUInt32(D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED) },
                        { D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, PropertyValue::CreateUInt32(D2D1_BORDER_MODE_HARD) } };
        auto sat = winrt::make_self<Fx>(L"vibra", CLSID_D2D1Saturation, blur.as<IGraphicsEffectSource>());
        sat->props = { { D2D1_SATURATION_PROP_SATURATION, PropertyValue::CreateSingle(1.65f) } };
        auto b = g_comp.CreateEffectFactory(sat.as<IGraphicsEffect>()).CreateBrush();
        b.SetSourceParameter(L"fundo", g_comp.CreateHostBackdropBrush());
        return b;
    } catch (winrt::hresult_error const& e) {
        Log(L"tahoe: fosco indisponível (0x%08X %ls)", (unsigned)e.code(), e.message().c_str());
    } catch (...) {}
    return g_comp.CreateHostBackdropBrush();
}

winrt::Windows::UI::Color Col(D2D1_COLOR_F c) {
    auto c8 = [](float v) { return (uint8_t)std::lround(std::clamp(v, 0.f, 1.f) * 255); };
    return { c8(c.a), c8(c.r), c8(c.g), c8(c.b) };
}
}  // namespace

bool tahoe::Reduced() { return g_reduce; }

// Cor de destaque escolhida no Ajustes (as mesmas cores da página Aparência); "multicor" = o azul do sistema.
// No escuro um tom mais claro (como o Mac faz nos ícones da barra lateral e no texto selecionado).
D2D1_COLOR_F tahoe::Accent(bool dark) {
    if (!g_read) Reload();
    static const std::pair<const wchar_t*, UINT32> cols[] = {
        { L"azul", 0x007AFF }, { L"roxo", 0x953D96 }, { L"rosa", 0xF74F9E }, { L"vermelho", 0xE0383E },
        { L"laranja", 0xF7821B }, { L"amarelo", 0xFCB827 }, { L"verde", 0x62BA46 }, { L"grafite", 0x8C8C8C }, { L"lavanda", 0x98A8D9 } };  // lavanda: a 10ª cor do macOS 27
    UINT32 c = 0x007AFF;
    for (auto& [n, v] : cols) if (g_accent == n) c = v;
    float r = ((c >> 16) & 255) / 255.f, g = ((c >> 8) & 255) / 255.f, b = (c & 255) / 255.f;
    if (dark) { r += (1 - r) * 0.22f; g += (1 - g) * 0.22f; b += (1 - b) * 0.22f; }
    return D2D1::ColorF(r, g, b, 1);
}

// Tinta do papel de parede: no macOS 26 toda janela tem um CAChameleonLayer "desktop tint" com opacidade 0,10 (guia
// macOS26-Theme-Reference, seção 29.3): a janela pega 10 % da cor do papel de parede. Só a cor (o quanto ele foge do
// cinza) entra, como na barra lateral dos apps novos (app2d.h): a janela continua clara no claro e escura no escuro.
// Ajustes > Aparência > Transparência > "Permitir tingir janelas com o papel de parede" (tingir_janelas).
D2D1_COLOR_F tahoe::Tint(D2D1_COLOR_F base) {
    static std::wstring lastWp;
    static float dr, dg, db;
    static bool have;
    if (!g_on || !g_tint) return base;
    wchar_t wp[MAX_PATH] = {};
    if (!SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, wp, 0) || !wp[0]) return base;
    if (lastWp != wp) {
        lastWp = wp; have = false;
        IWICImagingFactory* wic = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) {
            IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICBitmapScaler* sc = nullptr; IWICFormatConverter* cv = nullptr;
            if (SUCCEEDED(wic->CreateDecoderFromFilename(wp, nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr)) &&
                SUCCEEDED(wic->CreateBitmapScaler(&sc)) && SUCCEEDED(sc->Initialize(fr, 24, 24, WICBitmapInterpolationModeFant)) &&
                SUCCEEDED(wic->CreateFormatConverter(&cv)) &&
                SUCCEEDED(cv->Initialize(sc, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
                std::vector<BYTE> px(24 * 24 * 4);
                if (SUCCEEDED(cv->CopyPixels(nullptr, 24 * 4, (UINT)px.size(), px.data()))) {
                    double R = 0, G = 0, B = 0;
                    for (size_t i = 0; i < px.size(); i += 4) { B += px[i]; G += px[i + 1]; R += px[i + 2]; }
                    double n = 24 * 24 * 255.0;
                    float r = (float)(R / n), g = (float)(G / n), b = (float)(B / n);
                    float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                    auto lim = [](float v) { return std::clamp(v, -0.35f, 0.35f); };
                    dr = lim(r - lum); dg = lim(g - lum); db = lim(b - lum);
                    have = true;
                }
            }
            if (cv) cv->Release(); if (sc) sc->Release(); if (fr) fr->Release(); if (dec) dec->Release();
            wic->Release();
        }
    }
    if (!have) return base;
    const float k = 0.10f;
    auto cl = [](float v) { return std::clamp(v, 0.f, 1.f); };
    return D2D1::ColorF(cl(base.r + dr * k), cl(base.g + dg * k), cl(base.b + db * k), base.a);
}

bool tahoe::On() {

    if (!g_read) Reload();
    return g_on;
}

bool tahoe::Reload() {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    GetFileAttributesExW(Path().c_str(), GetFileExInfoStandard, &fa);
    if (g_read && !CompareFileTime(&fa.ftLastWriteTime, &g_stamp)) return false;
    g_stamp = fa.ftLastWriteTime;
    std::wstring accWas = g_accent;
    bool was = g_on, goldWas = g_golden, leoWas = g_leopard, first = !g_read;
    int folderWas = g_folderColor;
    g_read = true;
    g_on = ReadOn();
    if (first || was != g_on || goldWas != g_golden || leoWas != g_leopard) Log(L"tema: liquid_glass=%d golden_gate=%d leopard=%d reduzir=%d (%ls)", (int)g_on, (int)g_golden, (int)g_leopard, (int)g_reduce, Path().c_str());
    return !first && (was != g_on || goldWas != g_golden || leoWas != g_leopard || accWas != g_accent || folderWas != g_folderColor);  // a cor de destaque também redesenha
}

void tahoe::Frame(HWND h, bool on) {
    if (!on) {
        auto it = g_wins.find(h);
        if (it != g_wins.end()) it->second.root.IsVisible(false);
        Dwm(h, false);
        return;
    }
    Dwm(h, true);
    if (g_wins.count(h)) { g_wins[h].root.IsVisible(true); return; }
    try {
        if (!g_comp) {
            // o menu (context_menu.cpp) pode já ter criado a fila desta thread (app aberto no tema Sequoia e
            // trocado para o Tahoe depois): uma segunda é recusada pelo Windows e a janela ficava sem vidro
            if (!winrt::Windows::System::DispatcherQueue::GetForCurrentThread()) {
            DispatcherQueueOptions o{ sizeof(o), DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE };
            winrt::check_hresult(CreateDispatcherQueueController(o,
                reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(g_dq))));
            }
            g_comp = wuc::Compositor();
        }
        Win w;
        auto interop = g_comp.as<abi::ICompositorDesktopInterop>();
        // por baixo (isTopmost FALSE): a swap chain do D2D fica por cima, no alvo do DirectComposition
        winrt::check_hresult(interop->CreateDesktopWindowTarget(h, FALSE,
            reinterpret_cast<abi::IDesktopWindowTarget**>(winrt::put_abi(w.target))));
        w.root = g_comp.CreateContainerVisual();
        w.root.RelativeSizeAdjustment({ 1, 1 });
        auto blur = g_comp.CreateSpriteVisual();
        blur.RelativeSizeAdjustment({ 1, 1 });
        blur.Brush(Frost());
        auto tint = g_comp.CreateSpriteVisual();
        tint.RelativeSizeAdjustment({ 1, 1 });
        w.tint = g_comp.CreateColorBrush();
        tint.Brush(w.tint);
        w.root.Children().InsertAtTop(blur);
        w.root.Children().InsertAtTop(tint);
        w.geo = g_comp.CreateRoundedRectangleGeometry();
        w.root.Clip(g_comp.CreateGeometricClip(w.geo));
        w.target.Root(w.root);
        g_wins[h] = w;
        RECT rc; GetClientRect(h, &rc);
        Size(h, (float)rc.right, (float)rc.bottom, IsDarkTheme());
    } catch (winrt::hresult_error const& e) {
        Log(L"tahoe: vidro indisponível (0x%08X)", (unsigned)e.code());
    } catch (...) {}
}

static bool g_darkTheme = false;
bool tahoe::IsDarkTheme() { return g_darkTheme; }

void tahoe::Size(HWND h, float wpx, float hpx, bool dark) {
    g_darkTheme = dark;
    auto it = g_wins.find(h);
    if (it == g_wins.end()) return;
    (void)wpx; (void)hpx;  // o vidro fica só no painel da barra lateral (SetPanel), não na janela toda
    try {
        // Tahoe claro: vidro quase branco, o que está atrás só aparece de leve (sem isso, sobre uma
        // janela escura atrás, a barra lateral saía cinza-médio); escuro: grafite translúcido
        // a "face" do vidro da barra lateral (filtro glassBackground, guia macOS26-Theme-Reference, seção 29): escuro =
        // níveis 0,20→0,60 e 35 % de preto (saída = 0,26 × fundo + 0,13: 74 % de um cinza 0,176); claro = 0,50→0,95 e
        // 35 % de branco (0,29 × fundo + 0,675: 71 % de 0,95). Reduzir transparência: quase opaco.
        float a = g_reduce ? 0.97f : dark ? 0.74f : 0.71f;
        it->second.tint.Color(Col(dark ? D2D1::ColorF(0.176f, 0.176f, 0.185f, a) : D2D1::ColorF(0.95f, 0.95f, 0.955f, a)));
    } catch (...) {}
}

// Recorte do vidro = o painel flutuante da barra lateral (DIPs). Fora dele a janela é só o desenho
// D2D, então os cantos grandes saem com o antisserrilhado do D2D e nada do vidro sobra por fora da curva.
void tahoe::SetPanel(HWND h, D2D1_RECT_F r, float radius) {
    auto it = g_wins.find(h);
    if (it == g_wins.end()) return;
    float k = GetDpiForWindow(h) / 96.f;
    try {
        auto& g = it->second.geo;
        if (r.right <= r.left || r.bottom <= r.top) { g.Size({ 0, 0 }); return; }  // sem painel de vidro (Golden Gate)
        // 1 px para FORA: o conteúdo opaco do D2D cobre a sobra; para dentro, os pixels de borda
        // (meio transparentes no D2D) mostravam o que está atrás da janela como um tracejado escuro
        g.Offset({ r.left * k - 1, r.top * k - 1 });
        g.Size({ (r.right - r.left) * k + 2, (r.bottom - r.top) * k + 2 });
        float rr = radius * k * 0.5f;  // menor que o canto contínuo (D2D): o vidro sempre cobre a curva toda
        g.CornerRadius({ rr, rr });
    } catch (...) {}
}

float tahoe::Radius(HWND h) { return IsZoomed(h) ? 0 : WindowR(); }
// macOS 27 Golden Gate: todas as janelas com o mesmo canto, 16 pt (medido nas capturas do 512 Pixels), bem menos redondo que os 26 pt do Tahoe
float tahoe::WindowR() { if (!g_read) Reload(); return g_golden ? 16.f : 26.f; }
bool tahoe::Golden() { if (!g_read) Reload(); return g_golden; }
bool tahoe::Leopard() { if (!g_read) Reload(); return g_leopard; }
int tahoe::FolderColor() { if (!g_read) Reload(); return g_on ? g_folderColor : -1; }

// Cápsula de vidro desenhada em D2D (botões da barra de ferramentas, campos, painéis): corpo claro
// translúcido, sombra curta e difusa, contorno especular claro em cima e uma linha escura fina embaixo.
void tahoe::Capsule(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, D2D1_RECT_F r, float radius, bool dark, float lift) {
    if (radius < 0) radius = (r.bottom - r.top) / 2;
    // sombra: alguns anéis bem fracos, deslocados para baixo
    for (int i = 3; i >= 1; i--) {
        float k = i * 1.2f * lift;
        br->SetColor(D2D1::ColorF(0, 0, 0, (dark ? 0.06f : 0.035f) * lift));
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left - k * 0.5f, r.top + k * 0.4f, r.right + k * 0.5f, r.bottom + k), radius + k * 0.5f, radius + k * 0.5f), br);
    }
    auto rr = D2D1::RoundedRect(r, radius, radius);
    // escuro: quase a cor da janela, um pouco mais clara e translúcida (referência do Tahoe 26 escuro)
    br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.075f) : D2D1::ColorF(1, 1, 1, 0.94f));
    dc->FillRoundedRectangle(rr, br);
    // contorno especular: degradê de cima (claro) para baixo
    ID2D1GradientStopCollection* stops = nullptr;
    D2D1_GRADIENT_STOP gs[3] = {
        { 0, dark ? D2D1::ColorF(1, 1, 1, 0.20f) : D2D1::ColorF(0, 0, 0, 0.05f) },
        { 0.5f, dark ? D2D1::ColorF(1, 1, 1, 0.05f) : D2D1::ColorF(0, 0, 0, 0.07f) },
        { 1, dark ? D2D1::ColorF(1, 1, 1, 0.10f) : D2D1::ColorF(0, 0, 0, 0.12f) } };

    if (SUCCEEDED(dc->CreateGradientStopCollection(gs, 3, &stops))) {
        ID2D1LinearGradientBrush* lg = nullptr;
        if (SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, r.top }, { 0, r.bottom }), stops, &lg))) {
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.25f, r.top + 0.25f, r.right - 0.25f, r.bottom - 0.25f), radius, radius), lg, 0.6f);
            lg->Release();
        }
        stops->Release();
    }
    if (!dark) {  // brilho branco por dentro, no alto
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.9f));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 1, r.top + 1, r.right - 1, r.bottom - 1), radius - 1, radius - 1), br, 0.6f);
    }
}

// Canto contínuo da Apple ("squircle"): em cada canto, a curva |x/a|^5 + |y/a|^5 = 1 em vez do arco de
// círculo, então a curvatura cresce aos poucos a partir da reta (sem o "degrau" do arco). Para o canto
// ocupar o mesmo espaço visual de um arco de raio r, a = 1,35 r (limitado à metade do lado menor).
ID2D1PathGeometry* tahoe::Squircle(ID2D1Factory* f, D2D1_RECT_F r, float radius) {
    ID2D1PathGeometry* g = nullptr;
    if (FAILED(f->CreatePathGeometry(&g))) return nullptr;
    ID2D1GeometrySink* sk = nullptr;
    if (FAILED(g->Open(&sk))) { g->Release(); return nullptr; }
    // Canto contínuo da Apple: a curva começa antes (1,7 × raio) e é uma superelipse n = 4, o que dá o
    // tamanho visual de um círculo de ~0,93 × raio com a entrada suave. Antes (n = 5 em 1,35 × raio) o canto
    // parecia um círculo de ~11 px numa janela de 26 pt: apertado e com degraus.
    float a = std::min({ radius * 1.7f, (r.right - r.left) / 2, (r.bottom - r.top) / 2 });
    a = std::max(a, 0.f);
    const int N = 48;  // segmentos por canto: sem "dentes" na curva
    // centros dos cantos e sinais (de cima à direita, no sentido horário)
    struct C { float cx, cy, sx, sy; } cs[4] = {
        { r.right - a, r.top + a, 1, -1 }, { r.right - a, r.bottom - a, 1, 1 },
        { r.left + a, r.bottom - a, -1, 1 }, { r.left + a, r.top + a, -1, -1 } };
    sk->BeginFigure({ r.left + a, r.top }, D2D1_FIGURE_BEGIN_FILLED);
    for (int k = 0; k < 4; k++) {
        for (int i = 0; i <= N; i++) {
            // k=0: de (0,-1) a (1,0); k=1: de (1,0) a (0,1); ... ângulo t vai de 0 a 90 graus
            float t = 1.5707963f * i / N;
            // |.|^(2/4): superelipse n = 4. O cosseno de 90° sai -4e-8, e a raiz de um negativo é NaN: a geometria
            // inteira virava NaN e o recorte da janela escondia todo o conteúdo (Monitor, Ajustes)
            float u = std::pow(std::max(0.f, std::sin(t)), 0.5f), v = std::pow(std::max(0.f, std::cos(t)), 0.5f);
            float px, py;
            if (k == 0) { px = u; py = v; } else if (k == 1) { px = v; py = u; } else if (k == 2) { px = u; py = v; } else { px = v; py = u; }
            sk->AddLine({ cs[k].cx + cs[k].sx * a * px, cs[k].cy + cs[k].sy * a * py });
        }
    }
    sk->EndFigure(D2D1_FIGURE_END_CLOSED);
    sk->Close(); sk->Release();
    return g;
}

static D2D1_RECT_F Grow(D2D1_RECT_F r, float d) { return { r.left - d, r.top - d, r.right + d, r.bottom + d }; }

// Painel de vidro flutuante (barra lateral): transparente por dentro (o desfoque da janela aparece),
// com o contorno especular do Liquid Glass e uma borda fina por fora; cantos contínuos.
void tahoe::Panel(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, D2D1_RECT_F r, float radius, bool dark) {
    ID2D1Factory* f = nullptr; dc->GetFactory(&f);
    ID2D1PathGeometry* body = Squircle(f, r, radius);
    ID2D1PathGeometry* inner = Squircle(f, Grow(r, -0.5f), radius - 0.5f);
    ID2D1PathGeometry* outer = Squircle(f, Grow(r, 0.5f), radius + 0.5f);
    f->Release();
    D2D1_GRADIENT_STOP gs[4] = {
        { 0.00f, D2D1::ColorF(1, 1, 1, dark ? 0.34f : 0.95f) },
        { 0.25f, D2D1::ColorF(1, 1, 1, dark ? 0.08f : 0.45f) },
        { 0.75f, D2D1::ColorF(1, 1, 1, dark ? 0.05f : 0.35f) },
        { 1.00f, D2D1::ColorF(1, 1, 1, dark ? 0.22f : 0.80f) } };
    br->SetColor(D2D1::ColorF(1, 1, 1, dark ? 0.03f : 0.10f));  // reflexo suave
    if (body) dc->FillGeometry(body, br);
    ID2D1GradientStopCollection* stops = nullptr;
    if (inner && SUCCEEDED(dc->CreateGradientStopCollection(gs, 4, &stops))) {
        ID2D1LinearGradientBrush* lg = nullptr;
        if (SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ r.left, r.top }, { r.right, r.bottom }), stops, &lg))) {
            dc->DrawGeometry(inner, lg, 1.0f);
            lg->Release();
        }
        stops->Release();
    }
    br->SetColor(D2D1::ColorF(0, 0, 0, dark ? 0.40f : 0.08f));  // borda escura fina por fora: 1 px inteiro
    if (outer) dc->DrawGeometry(outer, br, 1.0f);
    if (body) body->Release();
    if (inner) inner->Release();
    if (outer) outer->Release();
}

// Contorno da janela inteira (o DWM não desenha borda no Tahoe): linha fina no mesmo canto contínuo.
void tahoe::WindowRim(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, D2D1_SIZE_F s, float radius, bool dark) {
    ID2D1Factory* f = nullptr; dc->GetFactory(&f);
    ID2D1PathGeometry* g = Squircle(f, D2D1::RectF(0.5f, 0.5f, s.width - 0.5f, s.height - 0.5f), radius - 0.5f);
    f->Release();
    if (!g) return;
    br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.16f) : D2D1::ColorF(0, 0, 0, 0.16f));
    dc->DrawGeometry(g, br, 1);
    g->Release();
}

// Tudo o que vem depois fica recortado no contorno da janela (canto contínuo; transparente fora dele).
bool tahoe::PushWindowClip(ID2D1DeviceContext* dc, D2D1_SIZE_F s, float radius) {
    if (radius <= 0) return false;
    ID2D1Factory* f = nullptr;
    dc->GetFactory(&f);
    ID2D1PathGeometry* g = Squircle(f, D2D1::RectF(0, 0, s.width, s.height), radius);
    f->Release();
    if (!g) return false;
    dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), g, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE), nullptr);
    g->Release();
    return true;
}

// ---- Borda de rolagem do Tahoe: listas e conteúdo rolam por baixo das barras flutuantes (busca, Voltar/Avançar,
// título). O que passa por baixo delas fica desfocado e some aos poucos, em vez de ser cortado por uma faixa
// opaca. A cena é desenhada fora da tela por um segundo contexto do mesmo dispositivo (o principal pode estar
// com camadas abertas, como o recorte da janela, e não pode trocar de alvo) e depois composta no principal.
namespace {
struct Scene {
    ID2D1Device* dev = nullptr;
    ID2D1DeviceContext* dc = nullptr;
    ID2D1Bitmap1* bmp = nullptr;
    ID2D1Effect* blur = nullptr;
    UINT w = 0, h = 0;
    float dpi = 96;
    void Reset() {
        if (blur) blur->Release();
        if (bmp) bmp->Release();
        if (dc) dc->Release();
        if (dev) dev->Release();
        *this = Scene{};
    }
};
Scene g_scenes[4];
wchar_t g_sceneErr[96] = L"";
}
const wchar_t* tahoe::SceneError() { return g_sceneErr; }

ID2D1DeviceContext* tahoe::SceneBegin(ID2D1DeviceContext* main, int slot, D2D1_SIZE_F dip, float dpi) {
    if (slot < 0 || slot >= 4 || !main) return nullptr;
    Scene& s = g_scenes[slot];
    ID2D1Device* dev = nullptr;
    main->GetDevice(&dev);
    if (!dev) { wcscpy_s(g_sceneErr, L"sem dispositivo"); return nullptr; }
    if (dev != s.dev) {  // dispositivo novo (ou perdido e recriado)
        s.Reset();
        s.dev = dev; dev->AddRef();
        if (HRESULT hr = dev->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &s.dc); FAILED(hr)) { swprintf_s(g_sceneErr, L"contexto 0x%08X", (unsigned)hr); dev->Release(); s.Reset(); return nullptr; }
        if (FAILED(s.dc->CreateEffect(CLSID_D2D1GaussianBlur, &s.blur))) s.blur = nullptr;
    }
    dev->Release();
    UINT w = (UINT)std::ceil(dip.width * dpi / 96), h = (UINT)std::ceil(dip.height * dpi / 96);
    if (w < 1 || h < 1) { wcscpy_s(g_sceneErr, L"tamanho zero"); return nullptr; }
    if (!s.bmp || w != s.w || h != s.h || dpi != s.dpi) {
        if (s.bmp) { s.bmp->Release(); s.bmp = nullptr; }
        auto bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), dpi, dpi);
        if (HRESULT hr = s.dc->CreateBitmap(D2D1::SizeU(w, h), nullptr, 0, &bp, &s.bmp); FAILED(hr)) { swprintf_s(g_sceneErr, L"bitmap 0x%08X", (unsigned)hr); return nullptr; }
        s.w = w; s.h = h; s.dpi = dpi;
    }
    s.dc->SetTarget(s.bmp);
    s.dc->SetDpi(dpi, dpi);
    s.dc->BeginDraw();
    s.dc->SetTransform(D2D1::Matrix3x2F::Identity());
    s.dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    return s.dc;
}

void tahoe::SceneEnd(int slot) {
    if (slot < 0 || slot >= 4 || !g_scenes[slot].dc) return;
    g_scenes[slot].dc->EndDraw();
    g_scenes[slot].dc->SetTarget(nullptr);
}

void tahoe::SceneDraw(ID2D1DeviceContext* dc, int slot, D2D1_RECT_F area, float band, D2D1_COLOR_F fade) {
    if (slot < 0 || slot >= 4) return;
    Scene& s = g_scenes[slot];
    if (!s.bmp) return;
    float edge = area.top + band;
    auto mask = [&](std::initializer_list<D2D1_GRADIENT_STOP> st, float y0, float y1) -> ID2D1LinearGradientBrush* {
        ID2D1GradientStopCollection* sc = nullptr;
        if (FAILED(dc->CreateGradientStopCollection(st.begin(), (UINT32)st.size(), &sc))) return nullptr;
        ID2D1LinearGradientBrush* lg = nullptr;
        dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, y0 }, { 0, y1 }), sc, &lg);
        sc->Release();
        return lg;
    };
    // nítida: some na metade de baixo da faixa
    if (auto* m = mask({ { 0, D2D1::ColorF(0, 0, 0, 0) }, { 1, D2D1::ColorF(0, 0, 0, 1) } }, edge - band * 0.45f, edge)) {
        dc->PushLayer(D2D1::LayerParameters1(area, nullptr, D2D1_ANTIALIAS_MODE_ALIASED, D2D1::IdentityMatrix(), 1, m), nullptr);
        dc->DrawImage(s.bmp);
        dc->PopLayer();
        m->Release();
    } else dc->DrawImage(s.bmp);
    // desfocada na faixa, mais fraca bem no alto (onde ficam o título e os botões)
    if (!s.blur || band <= 0) return;
    s.blur->SetInput(0, s.bmp);
    s.blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, 12.f);
    s.blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
    if (auto* m = mask({ { 0, D2D1::ColorF(0, 0, 0, 0.30f) }, { 0.6f, D2D1::ColorF(0, 0, 0, 0.85f) }, { 1, D2D1::ColorF(0, 0, 0, 0) } }, area.top, edge)) {
        dc->PushLayer(D2D1::LayerParameters1(D2D1::RectF(area.left, area.top, area.right, edge), nullptr, D2D1_ANTIALIAS_MODE_ALIASED, D2D1::IdentityMatrix(), 1, m), nullptr);
        dc->DrawImage(s.blur);
        dc->PopLayer();
        m->Release();
    }
    // e a faixa vai para a cor do fundo (o conteúdo some sob os botões, sem uma linha)
    if (fade.a > 0) {
        D2D1_COLOR_F f0 = fade, f1 = fade; f1.a = 0;
        D2D1_COLOR_F fm = fade; fm.a = fade.a * 0.85f;
        if (auto* g = mask({ { 0, f0 }, { 0.5f, fm }, { 1, f1 } }, area.top, edge)) {
            dc->FillRectangle(D2D1::RectF(area.left, area.top, area.right, edge), g);
            g->Release();
        }
    }
}
