// Vidro desfocado com recorte arredondado de qualquer raio (Windows.UI.Composition).
// O acrílico do SetWindowCompositionAttribute só sai com o canto fixo de 8 px do DWM e ignora a região
// da janela; aqui o desfoque do DWM (HostBackdropBrush) é recortado por uma geometria com antisserrilhado,
// então a curva fica igual à que o Dock desenha por cima.
//
// Liquid Glass (macOS Tahoe), ligado em Ajustes do Sistema > Aparência > Transparência e Efeitos:
// - lente nas bordas: anéis concêntricos do mesmo fundo, cada um ampliado um pouco mais (efeito
//   2D affine transform do D2D sobre o HostBackdropBrush), então perto da borda o que está atrás
//   aparece "puxado" para fora, como a refração da borda do vidro. Os anéis são só recortes
//   (clipes geométricos) uns sobre os outros: nada é redesenhado na CPU e as animações seguem na
//   Composição;
// - brilho especular: contorno com degradê diagonal (claro no canto de cima à esquerda e no de baixo
//   à direita), um halo interno suave e um reflexo no alto;
// - tinta: "Nítido" quase sem tinta (o fundo aparece), "Tingido" mais opaco, como no 26.1.
// Tudo isso só existe quando ligado: com o Liquid Glass desligado a árvore é a do Sequoia.
#include "shell.h"
#include "../menu/context_menu.h"
#include "../comum/vidro.h"
#include <dwmapi.h>
#include <d2d1effects.h>
#include <DispatcherQueue.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Effects.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <windows.ui.composition.interop.h>
#include <windows.graphics.effects.interop.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <mutex>
#include <thread>
#include <atomic>
#include <random>
#include <map>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "CoreMessaging.lib")
#pragma comment(lib, "dxguid.lib")

namespace wuc = winrt::Windows::UI::Composition;
namespace wfn = winrt::Windows::Foundation::Numerics;
namespace abi = ABI::Windows::UI::Composition::Desktop;
namespace abifx = ABI::Windows::Graphics::Effects;
using winrt::Windows::Graphics::Effects::IGraphicsEffect;
using winrt::Windows::Graphics::Effects::IGraphicsEffectSource;

namespace {

// Efeito "2D affine transform" do D2D descrito para a Composição (o que o Win2D faria, sem o Win2D).
// A matriz é animável: "lente.TransformMatrix" muda a cada Set sem recompilar o efeito.
struct AffineFx : winrt::implements<AffineFx, IGraphicsEffect, IGraphicsEffectSource, abifx::IGraphicsEffectD2D1Interop> {
    winrt::hstring name{ L"lente" };
    IGraphicsEffectSource src{ nullptr };
    explicit AffineFx(IGraphicsEffectSource s) : src(std::move(s)) {}
    winrt::hstring Name() { return name; }
    void Name(winrt::hstring const& n) { name = n; }

    HRESULT __stdcall GetEffectId(GUID* id) noexcept override { *id = CLSID_D2D12DAffineTransform; return S_OK; }
    HRESULT __stdcall GetNamedPropertyMapping(LPCWSTR n, UINT* index, abifx::GRAPHICS_EFFECT_PROPERTY_MAPPING* map) noexcept override {
        if (_wcsicmp(n, L"TransformMatrix")) return E_INVALIDARG;
        *index = D2D1_2DAFFINETRANSFORM_PROP_TRANSFORM_MATRIX;
        *map = abifx::GRAPHICS_EFFECT_PROPERTY_MAPPING_DIRECT;
        return S_OK;
    }
    HRESULT __stdcall GetPropertyCount(UINT* c) noexcept override { *c = 4; return S_OK; }
    HRESULT __stdcall GetProperty(UINT i, ABI::Windows::Foundation::IPropertyValue** v) noexcept override {
        using winrt::Windows::Foundation::PropertyValue;
        try {
            winrt::Windows::Foundation::IInspectable p;
            switch (i) {
            case D2D1_2DAFFINETRANSFORM_PROP_INTERPOLATION_MODE: p = PropertyValue::CreateUInt32(D2D1_2DAFFINETRANSFORM_INTERPOLATION_MODE_LINEAR); break;
            case D2D1_2DAFFINETRANSFORM_PROP_BORDER_MODE: p = PropertyValue::CreateUInt32(D2D1_BORDER_MODE_HARD); break;
            case D2D1_2DAFFINETRANSFORM_PROP_TRANSFORM_MATRIX: { float m[6] = { 1, 0, 0, 1, 0, 0 }; p = PropertyValue::CreateSingleArray(m); break; }
            case D2D1_2DAFFINETRANSFORM_PROP_SHARPNESS: p = PropertyValue::CreateSingle(0.f); break;
            default: return E_INVALIDARG;
            }
            *v = p.as<ABI::Windows::Foundation::IPropertyValue>().detach();
            return S_OK;
        } catch (...) { return E_FAIL; }
    }
    HRESULT __stdcall GetSource(UINT i, abifx::IGraphicsEffectSource** s) noexcept override {
        if (i != 0 || !src) return E_INVALIDARG;
        src.as<abifx::IGraphicsEffectSource>().copy_to(s);
        return S_OK;
    }
    HRESULT __stdcall GetSourceCount(UINT* c) noexcept override { *c = 1; return S_OK; }
};

// Desfoque gaussiano do D2D (mesmo esquema da AffineFx): depois da lente, para o vidro todo (centro e
// anéis) ter o MESMO fosco. Antes o centro usava o HostBackdropBrush direto (desfoque pesado do acrílico)
// e os anéis o fundo pela lente (quase nítido): a emenda aparecia como um vidro "remendado".
// Medido nas capturas do macOS 26 (Ars Technica, 2x): atrás do vidro o fundo vira mancha (texto nunca
// fica legível) e a cor dele é reforçada ("vibrância": o azul do papel de parede deixa o vidro azulado).
const float FROST = 16.f;      // desvio-padrão do desfoque (DIP)
const float VIBRANCE = 1.65f;  // saturação do fundo atrás do vidro (1 = como está)
struct BlurFx : winrt::implements<BlurFx, IGraphicsEffect, IGraphicsEffectSource, abifx::IGraphicsEffectD2D1Interop> {
    winrt::hstring name{ L"fosco" };
    IGraphicsEffectSource src{ nullptr };
    explicit BlurFx(IGraphicsEffectSource s) : src(std::move(s)) {}
    winrt::hstring Name() { return name; }
    void Name(winrt::hstring const& n) { name = n; }
    HRESULT __stdcall GetEffectId(GUID* id) noexcept override { *id = CLSID_D2D1GaussianBlur; return S_OK; }
    HRESULT __stdcall GetNamedPropertyMapping(LPCWSTR, UINT*, abifx::GRAPHICS_EFFECT_PROPERTY_MAPPING*) noexcept override { return E_INVALIDARG; }
    HRESULT __stdcall GetPropertyCount(UINT* c) noexcept override { *c = 3; return S_OK; }
    HRESULT __stdcall GetProperty(UINT i, ABI::Windows::Foundation::IPropertyValue** v) noexcept override {
        using winrt::Windows::Foundation::PropertyValue;
        try {
            winrt::Windows::Foundation::IInspectable p;
            switch (i) {
            case D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION: p = PropertyValue::CreateSingle(FROST); break;
            case D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION: p = PropertyValue::CreateUInt32(D2D1_GAUSSIANBLUR_OPTIMIZATION_BALANCED); break;
            case D2D1_GAUSSIANBLUR_PROP_BORDER_MODE: p = PropertyValue::CreateUInt32(D2D1_BORDER_MODE_HARD); break;
            default: return E_INVALIDARG;
            }
            *v = p.as<ABI::Windows::Foundation::IPropertyValue>().detach();
            return S_OK;
        } catch (...) { return E_FAIL; }
    }
    HRESULT __stdcall GetSource(UINT i, abifx::IGraphicsEffectSource** s) noexcept override {
        if (i != 0 || !src) return E_INVALIDARG;
        src.as<abifx::IGraphicsEffectSource>().copy_to(s);
        return S_OK;
    }
    HRESULT __stdcall GetSourceCount(UINT* c) noexcept override { *c = 1; return S_OK; }
};

// Saturação do D2D (vibrância): depois do desfoque.
struct SatFx : winrt::implements<SatFx, IGraphicsEffect, IGraphicsEffectSource, abifx::IGraphicsEffectD2D1Interop> {
    winrt::hstring name{ L"vibra" };
    IGraphicsEffectSource src{ nullptr };
    explicit SatFx(IGraphicsEffectSource s) : src(std::move(s)) {}
    winrt::hstring Name() { return name; }
    void Name(winrt::hstring const& n) { name = n; }
    HRESULT __stdcall GetEffectId(GUID* id) noexcept override { *id = CLSID_D2D1Saturation; return S_OK; }
    HRESULT __stdcall GetNamedPropertyMapping(LPCWSTR, UINT*, abifx::GRAPHICS_EFFECT_PROPERTY_MAPPING*) noexcept override { return E_INVALIDARG; }
    HRESULT __stdcall GetPropertyCount(UINT* c) noexcept override { *c = 1; return S_OK; }
    HRESULT __stdcall GetProperty(UINT i, ABI::Windows::Foundation::IPropertyValue** v) noexcept override {
        if (i != D2D1_SATURATION_PROP_SATURATION) return E_INVALIDARG;
        try {
            *v = winrt::Windows::Foundation::PropertyValue::CreateSingle(VIBRANCE).as<ABI::Windows::Foundation::IPropertyValue>().detach();
            return S_OK;
        } catch (...) { return E_FAIL; }
    }
    HRESULT __stdcall GetSource(UINT i, abifx::IGraphicsEffectSource** s) noexcept override {
        if (i != 0 || !src) return E_INVALIDARG;
        src.as<abifx::IGraphicsEffectSource>().copy_to(s);
        return S_OK;
    }
    HRESULT __stdcall GetSourceCount(UINT* c) noexcept override { *c = 1; return S_OK; }
};

const int LENS = 3;  // anéis da lente (mais = degrau menor, mais trabalho para a GPU)

struct Entry {
    HWND hwnd = nullptr;
    wuc::Desktop::DesktopWindowTarget target{ nullptr };
    wuc::ContainerVisual root{ nullptr };
    wuc::SpriteVisual blur{ nullptr };
    wuc::SpriteVisual tint{ nullptr };
    wuc::CompositionRoundedRectangleGeometry geo{ nullptr };
    wuc::CompositionColorBrush tintBrush{ nullptr };
    // Liquid Glass (criado na primeira vez que liga)
    bool liquidBuilt = false;
    wuc::SpriteVisual lens[LENS]{ nullptr, nullptr, nullptr };
    wuc::CompositionEffectBrush lensBrush[LENS]{ nullptr, nullptr, nullptr };
    wuc::CompositionRoundedRectangleGeometry lensGeo[LENS]{ nullptr, nullptr, nullptr };
    wuc::CompositionBrush baseBrush{ nullptr };        // vidro do Tahoe: fosco + vibrância (MakeFrost)
    wuc::CompositionBrush hostBrush{ nullptr };        // o HostBackdropBrush direto (Sequoia)
    wuc::CompositionRoundedRectangleGeometry baseGeo{ nullptr };
    wuc::CompositionGeometricClip baseClip{ nullptr };
    wuc::SpriteVisual sheen{ nullptr };
    wuc::CompositionLinearGradientBrush sheenBrush{ nullptr };
    wuc::ShapeVisual rim{ nullptr };
    std::vector<std::pair<wuc::CompositionRoundedRectangleGeometry, float>> rimGeo;  // geometria e recuo (px)
    std::wstring rimKey;  // estilo/tema/faixa com que o contorno foi montado
    // último Set (para reaplicar quando os ajustes mudam)
    float w = 0, h = 0, radius = 0;
    D2D1_COLOR_F tintIn{};
    bool hasSet = false;
    // vidro real (Liquid Glass com refração): a tela capturada por baixo, desenhada numa superfície
    wuc::CompositionDrawingSurface surf{ nullptr };
    wuc::CompositionSurfaceBrush surfBrush{ nullptr };
    int surfW = 0, surfH = 0;
    bool real = false;                       // esta peça está usando o vidro real agora
    std::vector<backdrop::Piece> pieces;     // várias peças numa janela (módulos da Central)
    D2D1_COLOR_F pieceTint{};
    float pieceFrost = 0;                    // desfoque mínimo das peças (DIP): blocos com texto (Central)
    RECT lastRect{};
    // conteúdo desenhado junto com o vidro (ContentBegin/ContentEnd)
    wuc::SpriteVisual content{ nullptr };
    wuc::CompositionDrawingSurface contentSurf{ nullptr };
    int contentW = 0, contentH = 0;
    ID2D1DeviceContext* contentDc = nullptr;  // entre ContentBegin e ContentEnd
};

winrt::Windows::System::DispatcherQueueController g_dq{ nullptr };
wuc::Compositor g_comp{ nullptr };
wuc::CompositionEffectFactory g_lensFactory{ nullptr };
bool g_lensFailed = false;
std::map<HWND, Entry> g_entries;
backdrop::Style g_style;

// O Windows recusa transformar o pincel de fundo ("Backdrop brushes cannot be transformed"): a lente
// (ampliar o fundo nas bordas com 2D affine) nunca funcionou e cada anel falhava em silêncio, deixando a
// faixa da borda SEM vidro (o que está atrás aparecia nítido, só com a tinta por cima). O vidro do Tahoe
// agora é uma peça só: desfoque + vibrância no retângulo todo; a borda fica com o contorno especular e o
// halo. Se o desfoque extra também for recusado, cai para só a vibrância e, por fim, para o fundo puro.
std::vector<wuc::CompositionEffectFactory> g_frostFx;
int g_frostOk = -1;  // índice da receita que funciona; -2 = nenhuma (fundo puro)

wuc::CompositionBrush MakeFrost() {
    if (g_frostFx.empty() && g_frostOk == -1) {
        auto src = [] { return wuc::CompositionEffectSourceParameter(L"fundo"); };
        auto add = [&](IGraphicsEffect fx) {
            try { g_frostFx.push_back(g_comp.CreateEffectFactory(fx)); }
            catch (winrt::hresult_error const& e) { Log(L"liquid glass: receita recusada (0x%08X %ls)", (unsigned)e.code(), e.message().c_str()); g_frostFx.push_back(nullptr); }
        };
        add(winrt::make<SatFx>(winrt::make<BlurFx>(src())));
        add(winrt::make<BlurFx>(src()));
        add(winrt::make<SatFx>(src()));
    }
    if (g_frostOk != -2) {
        for (int i = std::max(g_frostOk, 0); i < (int)g_frostFx.size(); i++) {
            if (!g_frostFx[i]) continue;
            try {
                auto b = g_frostFx[i].CreateBrush();
                b.SetSourceParameter(L"fundo", g_comp.CreateHostBackdropBrush());
                if (g_frostOk != i) Log(L"liquid glass: vidro com a receita %d (0 = fosco + vibrância)", i);
                g_frostOk = i;
                return b;
            } catch (winrt::hresult_error const& e) {
                Log(L"liquid glass: receita %d falhou (0x%08X %ls)", i, (unsigned)e.code(), e.message().c_str());
            }
        }
        g_frostOk = -2;
    }
    return g_comp.CreateHostBackdropBrush();
}

winrt::Windows::UI::Color Col(float r, float g, float b, float a) {
    auto c8 = [](float v) { return (uint8_t)std::lround(std::clamp(v, 0.f, 1.f) * 255); };
    return { c8(a), c8(r), c8(g), c8(b) };
}

void AddStop(wuc::CompositionGradientBrush const& b, float off, winrt::Windows::UI::Color c) {
    b.ColorStops().Append(g_comp.CreateColorGradientStop(off, c));
}

void BuildLiquid(Entry& e) {
    if (e.liquidBuilt) return;
    e.liquidBuilt = true;
    auto kids = e.root.Children();
    // o vidro todo (sem anéis de lente: o fundo não pode ser transformado, ver MakeFrost)
    e.baseBrush = MakeFrost();
    e.baseGeo = g_comp.CreateRoundedRectangleGeometry();
    e.baseClip = g_comp.CreateGeometricClip(e.baseGeo);
    // reflexo no alto
    e.sheen = g_comp.CreateSpriteVisual();
    e.sheenBrush = g_comp.CreateLinearGradientBrush();
    e.sheenBrush.StartPoint({ 0, 0 });
    e.sheenBrush.EndPoint({ 0, 1 });
    e.sheen.Brush(e.sheenBrush);
    kids.InsertAbove(e.sheen, e.tint);
    // contorno especular e halo
    e.rim = g_comp.CreateShapeVisual();
    e.rim.RelativeSizeAdjustment({ 1, 1 });
    kids.InsertAtTop(e.rim);
}

void SetLiquidVisible(Entry& e, bool on) {
    if (!e.liquidBuilt) return;
    for (auto& l : e.lens) if (l) l.IsVisible(on);
    e.sheen.IsVisible(on);
    e.rim.IsVisible(on);
    e.blur.Clip(nullptr);  // o vidro cobre a peça inteira (antes: só o miolo, e a borda ficava sem vidro)
    if (e.baseBrush && e.hostBrush) e.blur.Brush(on ? e.baseBrush : e.hostBrush);
}

namespace rg { void Use(Entry& e, bool want); void Redraw(Entry& e); void SetActive(bool on); extern bool active; extern bool semCaptura; }  // vidro real (mais abaixo)

void Apply(Entry& e) {
    if (!e.hasSet) return;
    const auto& st = g_style;
    float w = e.w, hgt = e.h, radius = e.radius;
    bool dark = ui::Dark();
    e.geo.Size({ w, hgt });
    e.geo.CornerRadius({ radius, radius });

    D2D1_COLOR_F t = backdrop::AdjustTint(e.tintIn);
    e.tintBrush.Color(Col(t.r, t.g, t.b, t.a));

    bool liquid = st.liquid && !st.reduce;
    if (!liquid) { SetLiquidVisible(e, false); rg::Use(e, false); return; }
    BuildLiquid(e);
    SetLiquidVisible(e, true);
    // vidro real (captura + refração) quando ligado; senão fica o fosco do MakeFrost
    rg::Use(e, st.real);
    if (!e.pieces.empty()) {  // várias peças: a tinta e o contorno são de cada peça (desenhados por quem pediu)
        e.tintBrush.Color(Col(0, 0, 0, 0));
        e.sheen.IsVisible(false);
        e.rim.IsVisible(false);
        e.geo.CornerRadius({ 0, 0 });
        // Sem o vidro real (captura de janelas indisponível: VM, área remota) e com uma peça só do tamanho da
        // janela (alertas, progresso): o fosco ganha os cantos e a tinta da peça. Sem isto era um retângulo
        // reto e sem tinta (visto na VM em 11/11/2026).
        if (!e.real && e.pieces.size() == 1 && !e.pieces[0].goo) {
            const auto& p = e.pieces[0];
            if (p.r.left <= 1 && p.r.top <= 1 && p.r.right >= e.w - 1 && p.r.bottom >= e.h - 1) {
                e.geo.CornerRadius({ p.radius, p.radius });
                D2D1_COLOR_F pt = backdrop::AdjustTint(e.pieceTint);
                e.tintBrush.Color(Col(pt.r, pt.g, pt.b, pt.a));
            }
        }
        return;
    }

    float px = GetDpiForWindow(e.hwnd) / 96.f;
    if (px <= 0) px = 1;
    // faixa da lente: larga o bastante para se ver, nunca mais que o raio nem que 1/4 do lado menor
    float band = std::min({ std::max(radius, 8 * px) * 0.95f, std::min(w, hgt) * 0.24f, 22 * px });
    float k = std::clamp(st.refraction, 0.f, 1.f);

    // contorno, halo e reflexo: montados só quando o estilo, o tema ou a faixa mudam;
    // no resto (Dock ampliando, painel crescendo) só as geometrias acompanham o tamanho
    bool clear = !st.tinted;
    wchar_t key[160];
    swprintf_s(key, L"%d%d%d%.2f%.2f%.2f", (int)dark, (int)clear, (int)st.real, k, std::round(band * 2) / 2, px);
    if (e.rimKey != key) {
        e.rimKey = key;
        // Tahoe (capturas da Apple): sem reflexo branco no alto; a borda é só um fio de luz fino e fraco
        float sheenA = 0;
        e.sheenBrush.ColorStops().Clear();
        AddStop(e.sheenBrush, 0, Col(1, 1, 1, sheenA));
        AddStop(e.sheenBrush, 1, Col(1, 1, 1, 0));

        auto shapes = e.rim.Shapes();
        shapes.Clear();
        e.rimGeo.clear();
        auto add = [&](float inset, float thick, wuc::CompositionBrush const& brush) {
            auto g = g_comp.CreateRoundedRectangleGeometry();
            auto s = g_comp.CreateSpriteShape(g);
            s.StrokeBrush(brush);
            s.StrokeThickness(thick);
            shapes.Append(s);
            e.rimGeo.push_back({ g, inset });
        };
        // borda escura bem fina por fora
        // vidro real: a borda da Apple é só luz (sem contorno escuro); no fosco, o contorno ajuda a separar
        add(0.5f * px, 1 * px, g_comp.CreateColorBrush(Col(0, 0, 0, st.real ? (dark ? 0.18f : 0.03f) : (dark ? 0.38f : 0.10f))));
        // fio de luz: um pouco mais claro em cima e embaixo, quase some dos lados (borda do Dock do Tahoe)
        auto spec = g_comp.CreateLinearGradientBrush();
        spec.StartPoint({ 0, 0 });
        spec.EndPoint({ 0, 1 });
        float hi = dark ? 0.24f : 0.42f, lo = dark ? 0.06f : 0.12f;
        if (st.real) { hi *= 0.3f; lo *= 0.3f; }  // o vidro real já desenha o brilho especular (SpecFor)
        AddStop(spec, 0.00f, Col(1, 1, 1, hi));
        AddStop(spec, 0.35f, Col(1, 1, 1, lo));
        AddStop(spec, 0.65f, Col(1, 1, 1, lo));
        AddStop(spec, 1.00f, Col(1, 1, 1, hi * 0.75f));
        add(0.5f * px, 1.0f * px, spec);
        // halo interno: a luz que a lente concentra na borda, mais forte em cima, some para o centro
        const int GLOW = 0;  // sem halo interno (a referência não tem)
        float gw = band / GLOW;
        for (int i = 0; i < GLOW; i++) {
            float a = (dark ? 0.03f : 0.08f) * (1 - (float)i / GLOW) * (0.5f + 0.5f * k);  // halo discreto: forte demais virava um aro claro em cartões escuros
            auto gb = g_comp.CreateLinearGradientBrush();
            gb.StartPoint({ 0, 0 });
            gb.EndPoint({ 0, 1 });
            AddStop(gb, 0, Col(1, 1, 1, a));
            AddStop(gb, 0.5f, Col(1, 1, 1, a * 0.35f));
            AddStop(gb, 1, Col(1, 1, 1, a * 0.75f));
            add(2.5f * px + gw * (i + 0.5f), gw, gb);
        }
    }
    e.sheen.Size({ w, hgt * 0.55f });
    for (auto& [g, inset] : e.rimGeo) {
        g.Offset({ inset, inset });
        g.Size({ std::max(0.f, w - 2 * inset), std::max(0.f, hgt - 2 * inset) });
        float r = std::max(0.f, radius - inset);
        g.CornerRadius({ r, r });
    }
}

// ============================================================================================
// Vidro real do Liquid Glass. O pincel de fundo do Windows não pode ser deformado, então a refração
// vem da própria tela: captura da tela principal pela GPU (Windows.Graphics.Capture), com as janelas do
// Shell que ficam por cima (Dock, menus, painéis) escondidas da captura (WDA_EXCLUDEFROMCAPTURE), para o
// vidro não ver a si mesmo. Para cada peça de vidro, o recorte da tela atrás dela passa por:
//   desfoque leve (controlado pela Transparência) -> vibrância (saturação 1,8, como no estudo) ->
//   mapa de deslocamento com perfil de lente convexa na borda (o fundo "dobra" perto da borda, como no Tahoe)
// e por cima: a tinta da peça, um ruído de 2,5% (contra degraus de cor) e o contorno Fresnel (rim).
// A Captura de Tela do projeto avisa (ShellCapturaVisivel) para as peças aparecerem no print.
namespace rg {
namespace wgc = winrt::Windows::Graphics::Capture;
namespace wd3 = winrt::Windows::Graphics::DirectX::Direct3D11;
const UINT WM_RG_FRAME = WM_APP + 140;
const UINT_PTR T_RG = 1;
bool tried, ok, active;
ID3D11Device* d3d;
ID3D11DeviceContext* ctx;
ID2D1Factory1* fac;
ID2D1Device* d2d;
ID2D1DeviceContext* util;  // para criar bitmaps (do mesmo dispositivo das superfícies)
wuc::CompositionGraphicsDevice gfx{ nullptr };
wd3::IDirect3DDevice wdev{ nullptr };
wgc::GraphicsCaptureItem item{ nullptr };
wgc::Direct3D11CaptureFramePool pool{ nullptr };
wgc::GraphicsCaptureSession sess{ nullptr };
std::mutex mx;
wgc::Direct3D11CaptureFrame latest{ nullptr };
std::atomic<bool> posted{ false };
ID3D11Texture2D* tex;      // cópia do último quadro
ID2D1Bitmap1* screen;      // a cópia como bitmap do D2D
UINT texW, texH;
RECT mon{};                // tela principal (px)
HWND msgWnd;
bool showInCapture;        // a Captura de Tela pediu as peças visíveis
std::atomic<bool> capDirty{ false };  // chegou quadro novo ou janela mexeu: montar o fundo de novo
// janela mexeu/mudou de ordem, peça nova: monta e redesenha todas as áreas. Só quadro novo de uma janela:
// só as áreas (e as peças) que ficam na frente dela
std::atomic<bool> allDirty{ true };
std::atomic<bool> frameDirty{ false };  // só chegou quadro novo (conteúdo): monta no máximo a cada 15 ms (30 por segundo ficava lento de ver)
double lastFrameCompose = 0;
std::vector<RECT> lastDirty;  // áreas montadas na última vez (as peças nelas são redesenhadas)
// Montando o fundo agora. Os objetos da captura vivem em outra thread (MTA): usá-los daqui faz o COM
// processar mensagens enquanto espera, e um aviso de quadro novo chamava a montagem de novo no meio dela,
// que esperava pela trava que ela mesma segurava (o Shell travou ao fechar o FL Studio).
bool g_busy;
DWORD lastTry;
ID2D1Bitmap* noise;
std::map<std::wstring, ID2D1Bitmap*> maps;  // mapas de lente por tamanho/raio/força

void Redraw(Entry& e);
void Poll(bool refresh = true);  // refresh: relê a lista de janelas (só quando algo abriu/fechou/mexeu)
// Áreas das peças de vidro (coordenadas da tela principal), copiadas para a thread da captura: um quadro novo
// cuja parte alterada (Windows 11 24H2: regiões sujas) não passa atrás de nenhuma peça não remonta nada.
// Ex.: o app do Claude/Discord animando no meio da janela fazia o vidro remontar ~50 vezes/s à toa.
std::mutex areasMx;
std::vector<RECT> areasSnap;
std::atomic<int> st_frames, st_composes, st_redraws, st_skipped;  // contagem a cada 10 s (diagnóstico de CPU)
std::map<std::wstring, int> st_frameBy;               // quadros por classe de janela atrás das peças
// quadro novo de uma janela que não é o papel de parede (Progman/WorkerW/SequoiaMesa). Só o papel de parede animado
// mudando (Lively: ~58 quadros/s, o tempo todo) remonta o vidro a 30/s, não a 60: o vidro parado sobre um papel de
// parede animado custava uma montagem a cada quadro da tela e roubava a fluidez das animações
std::atomic<bool> st_nonDesk{ true };
std::mutex st_mx;
void Dump();

// esconde (ou mostra) da captura todas as janelas do Shell que ficam por cima; a mesa (papel de parede) não
void ApplyAffinity() {
    DWORD me = GetCurrentProcessId();
    EnumWindows([](HWND h, LPARAM p) -> BOOL {
        DWORD pid = 0, tid = GetWindowThreadProcessId(h, &pid);
        // só as janelas desta thread (a da interface): mexer numa janela de outra thread faz o Windows
        // conversar com ela, e se ela estiver esperando a interface, o Shell trava (aconteceu)
        if (pid != (DWORD)p || tid != GetCurrentThreadId()) return TRUE;
        wchar_t c[64] = {}; GetClassNameW(h, c, 64);
        if (!wcscmp(c, L"SequoiaMesa")) return TRUE;
        DWORD want = 0 /*WDA_NONE*/;  // o vidro real não vê mais a si mesmo: tudo aparece nas gravações
        DWORD cur = 0;
        if (GetWindowDisplayAffinity(h, &cur) && cur != want) {
            if (!SetWindowDisplayAffinity(h, want)) Log(L"vidro real: não deu para esconder %ls da captura (%lu)", c, GetLastError());
        }
        return TRUE;
    }, (LPARAM)me);
}

LRESULT CALLBACK MsgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    static UINT capVis = RegisterWindowMessageW(L"ShellCapturaVisivel");
    static UINT dumpMsg = RegisterWindowMessageW(L"ShellVidroFoto");
    if (m == dumpMsg && dumpMsg) { Dump(); return 1; }  // diagnóstico: grava o fundo montado e cada janela em bin\vidro-*.bmp
    if (m == WM_RG_FRAME) {  // quadro novo, ou janela apareceu/sumiu/mudou de ordem: monta o fundo já
        posted = false;
        // janela mexendo: até 60 por segundo; só conteúdo mudando (vídeo, texto rolando atrás do Dock): também até 60 (30 deixava o vidro atrasado)
        // (frameDirty, ver Poll)
        if (w == 1) frameDirty = true; else capDirty = true;
        static double last = 0;
        bool soPapel = w == 1 && !capDirty && !st_nonDesk.load();  // só o papel de parede animado: 30 por segundo
        double gap = soPapel ? 33.0 : 15.5;
        if (anim::Now() - last >= gap) { last = anim::Now(); st_nonDesk = false; Poll(w != 1); }
        else SetTimer(h, T_RG + 1, soPapel ? 33 : 16, nullptr);  // cedo demais: monta no próximo tique (uma vez só)
        return 0;
    }
    // o que ficou pendente; o temporizador só existe enquanto há algo (antes acordava 60 vezes/s sempre)
    if (m == WM_TIMER && w == T_RG + 1) {
        KillTimer(h, T_RG + 1);
        st_nonDesk = false;
        Poll(false);
        if (frameDirty || capDirty) SetTimer(h, T_RG + 1, 16, nullptr);
        return 0;
    }
    if (m == WM_TIMER && w == T_RG && g_busy) return 0;
    if (m == WM_TIMER && w == T_RG) {  // rede de segurança: janelas que se moveram sem aviso; janelas novas do Shell
        if (semCaptura && active) {
            Log(L"vidro real: nenhuma janela pôde ser capturada (fundo preto); voltando ao vidro fosco até o Shell reiniciar");
            g_style.real = false;
            SetActive(false);
            return 0;
        }
        static int tick = 0;
        if (tick++ % 8 == 0) ApplyAffinity();  // ~2 s (as janelas novas do Shell são raras)
        if (tick % 40 == 0) {  // a cada 10 s: o que o vidro fez (diagnóstico de CPU)
            std::wstring by;
            { std::lock_guard<std::mutex> lk(st_mx); for (auto& [k, v] : st_frameBy) by += k + L"=" + std::to_wstring(v) + L" "; st_frameBy.clear(); }
            Log(L"vidro real: 10 s: %d quadros (atrás das peças: %ls; %d ignorados, mudança longe do vidro), %d montagens, %d redesenhos de peça",
                st_frames.exchange(0), by.c_str(), st_skipped.exchange(0), st_composes.exchange(0), st_redraws.exchange(0));
        }
        Poll(true);  // a lista de janelas, 4 vezes/s: abrir, fechar, mexer e trocar de ordem já avisam na hora (OnWinChange)
        for (auto& [hh, e] : g_entries) {
            if (!e.real || !IsWindowVisible(e.hwnd)) continue;
            RECT r; GetWindowRect(e.hwnd, &r);
            if (memcmp(&r, &e.lastRect, sizeof(r))) Redraw(e);
        }
        return 0;
    }
    if (m == capVis && capVis) return 1;  // a Captura de Tela avisa antes da foto; as peças já aparecem sempre
    return DefWindowProcW(h, m, w, l);
}

// Fundo do vidro real montado pelas janelas que estão atrás das peças, em vez da tela inteira. Cada janela
// vem da captura do Windows por janela (funciona mesmo com ela coberta por outras): o papel de parede (a
// nossa mesa) embaixo e as janelas por cima, na ordem da tela. As janelas do Shell (o próprio vidro) nunca
// entram na montagem, então o vidro não vê a si mesmo e as peças não precisam mais se esconder das
// gravações: o Dock, a barra de menus e os painéis aparecem no OBS, no Discord e no print, com o vidro real.
// Antes era a duplicação da tela inteira, que exigia esconder as peças de toda captura (WDA_EXCLUDEFROMCAPTURE).
const auto FMT = winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized;
struct WinCap {
    HWND h = nullptr;
    wgc::GraphicsCaptureItem item{ nullptr };
    wgc::Direct3D11CaptureFramePool pool{ nullptr };
    wgc::GraphicsCaptureSession session{ nullptr };
    std::mutex m;
    wgc::Direct3D11CaptureFrame frame{ nullptr };  // o último que chegou
    winrt::Windows::Graphics::SizeInt32 size{};
    RECT rect{};                                   // onde estava na última montagem
    std::atomic<bool> relevant{ true };            // está atrás de alguma peça de vidro (senão quadro novo não monta nada)
    std::atomic<bool> fresh{ true };               // chegou quadro desde a última montagem
    void Close() {
        try { if (session) session.Close(); if (pool) pool.Close(); } catch (...) {}
        std::lock_guard<std::mutex> lk(m);
        try { if (frame) frame.Close(); } catch (...) {}
        frame = nullptr;
    }
};
std::vector<std::shared_ptr<WinCap>> caps;  // de baixo para cima
std::map<HWND, DWORD> capFailed;            // janelas que a captura recusa (de administrador): tenta de novo depois
ID2D1Bitmap1* target;                       // tex como alvo de desenho da montagem

bool Cloaked(HWND h) { DWORD c = 0; DwmGetWindowAttribute(h, DWMWA_CLOAKED, &c, sizeof(c)); return c != 0; }
RECT Bounds(HWND h) {
    RECT r{};
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) GetWindowRect(h, &r);
    return r;
}

std::shared_ptr<WinCap> StartCap(HWND h) {
    auto c = std::make_shared<WinCap>();
    c->h = h;
    try {
        auto interop = winrt::get_activation_factory<wgc::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        winrt::check_hresult(interop->CreateForWindow(h, winrt::guid_of<wgc::GraphicsCaptureItem>(), winrt::put_abi(c->item)));
        c->size = c->item.Size();
        c->pool = wgc::Direct3D11CaptureFramePool::CreateFreeThreaded(wdev, FMT, 2, c->size);
        c->session = c->pool.CreateCaptureSession(c->item);
        try { c->session.IsCursorCaptureEnabled(false); } catch (...) {}
        try { c->session.IsBorderRequired(false); } catch (...) {}  // sem a borda amarela de captura
        try { c->session.DirtyRegionMode(wgc::GraphicsCaptureDirtyRegionMode::ReportOnly); } catch (...) {}  // 24H2+: o que mudou em cada quadro
        std::weak_ptr<WinCap> wk = c;  // quadro atrasado depois de fechar não encontra nada
        c->pool.FrameArrived([wk](wgc::Direct3D11CaptureFramePool const& pl, auto&&) {
            auto c = wk.lock();
            if (!c) return;
            auto f = pl.TryGetNextFrame();
            if (!f) return;
            auto cs = f.ContentSize();
            wgc::Direct3D11CaptureFrame old{ nullptr };
            { std::lock_guard<std::mutex> lk(c->m); old = c->frame; c->frame = f; }
            if (old) old.Close();  // devolve o buffer
            if (cs.Width != c->size.Width || cs.Height != c->size.Height) {  // a janela mudou de tamanho
                c->size = cs;
                try { pl.Recreate(wdev, FMT, 2, cs); } catch (...) {}
            }
            // vídeo tocando numa janela longe da barra e do Dock: quadro novo não muda o vidro
            st_frames++;
            if (c->relevant) {  // a parte que mudou passa atrás de alguma peça? (sem a informação: assume que sim)
                bool touches = true;
                try {
                    auto dr = f.DirtyRegions();
                    if (dr && dr.Size() > 0) {
                        RECT wr; GetWindowRect(c->h, &wr);
                        if (cs.Width != wr.right - wr.left || cs.Height != wr.bottom - wr.top) wr = Bounds(c->h);
                        LONG ox = wr.left - mon.left, oy = wr.top - mon.top;
                        std::lock_guard<std::mutex> lk(areasMx);
                        touches = areasSnap.empty();  // ainda sem montagem: não sabe as áreas, monta
                        for (auto r : dr) {
                            RECT d{ ox + r.X, oy + r.Y, ox + r.X + r.Width, oy + r.Y + r.Height }, x;
                            for (auto& a : areasSnap) if (IntersectRect(&x, &d, &a)) { touches = true; break; }
                            if (touches) break;
                        }
                    }
                } catch (...) {}
                if (!touches) { st_skipped++; return; }  // o quadro fica guardado (a próxima montagem já usa o mais novo)
            }
            c->fresh = true;
            if (c->relevant) {
                wchar_t cl[48] = {}; GetClassNameW(c->h, cl, 48);
                if (wcscmp(cl, L"Progman") && wcscmp(cl, L"WorkerW") && wcscmp(cl, L"SequoiaMesa")) st_nonDesk = true;
                std::lock_guard<std::mutex> lk(st_mx); st_frameBy[cl]++;
            }
            if (c->relevant && !posted.exchange(true)) PostMessageW(msgWnd, WM_RG_FRAME, 1, 0);
        });
        c->session.StartCapture();
        return c;
    } catch (...) {
        return nullptr;
    }
}

void StopCaps() {
    for (auto& c : caps) c->Close();
    caps.clear();
}

// Quais janelas estão na tela agora (de cima para baixo, as 10 primeiras, mesmo cobertas por outras), não só
// as que estão atrás das peças: quando uma peça nova aparece (o seletor do Alt+Tab, no meio da tela) ou uma
// janela de trás vem para a frente, o vidro já tem o quadro dela. Capturas novas abrem numa thread (cada
// uma leva ~8 ms conversando com o DWM; na thread da interface atrasava o Alt+Tab).
std::mutex readyMx;
std::vector<std::pair<HWND, std::shared_ptr<WinCap>>> ready;  // abertas pela thread (nullptr = recusada)
std::vector<HWND> pending;

bool capJaFuncionou = false;  // alguma montagem já teve janela com quadro (a captura funciona neste PC)

void RefreshWindows() {
    {   // sem captura nenhuma (todas recusadas, nenhuma aberta) por 3 s: também desiste
        static DWORD semCaps = 0;
        if (!caps.empty() || capFailed.empty()) semCaps = 0;
        else if (!semCaps) semCaps = GetTickCount();
        else if (GetTickCount() - semCaps > 3000 && !capJaFuncionou) semCaptura = true;
    }
    std::vector<std::pair<HWND, std::shared_ptr<WinCap>>> got;
    { std::lock_guard<std::mutex> lk(readyMx); got.swap(ready); }
    for (auto& [h, c] : got) {
        pending.erase(std::remove(pending.begin(), pending.end(), h), pending.end());
        if (!c) {
            if (!capFailed.count(h)) { wchar_t cl[64] = {}; GetClassNameW(h, cl, 64); Log(L"vidro real: captura recusada para %ls", cl); }
            {   // a mesa (o papel de parede) é o fundo de quase tudo: sem ela o vidro fica preto onde não há janela atrás
                wchar_t cl[64] = {}; GetClassNameW(h, cl, 64);
                if (!capJaFuncionou && (!wcscmp(cl, L"SequoiaMesa") || !wcscmp(cl, L"Progman") || !wcscmp(cl, L"WorkerW"))) semCaptura = true;
            }
            capFailed[h] = GetTickCount();
        }
    }
    std::vector<HWND> want;
    bool anyGlass = false;
    for (auto& [hh, e] : g_entries) if (e.real && IsWindowVisible(e.hwnd)) { anyGlass = true; break; }
    if (anyGlass) {
        struct Ctx { std::vector<HWND>* out; DWORD me; HRGN covered; HRGN tmp; HRGN screen; bool afterMesa; } cx{ &want, GetCurrentProcessId(),
            CreateRectRgn(0, 0, 0, 0), CreateRectRgn(0, 0, 0, 0), CreateRectRgnIndirect(&mon), false };
        EnumWindows([](HWND h, LPARAM lp) -> BOOL {
            auto* cx = (Ctx*)lp;
            if (!IsWindowVisible(h) || IsIconic(h) || Cloaked(h)) return TRUE;
            DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
            // papel de parede animado (Lively...): a nossa mesa ficou transparente (só os ícones) e o papel de
            // parede é uma janela dentro da mesa do Windows (Progman), logo abaixo. O vidro pega as duas: a Progman
            // por baixo e a mesa por cima (17/11/2026: com o Lively o vidro ficava preto, sem o papel de parede)
            if (cx->afterMesa) {
                wchar_t c[32] = {}; GetClassNameW(h, c, 32);
                if (!wcscmp(c, L"Progman") || !wcscmp(c, L"WorkerW")) { cx->out->push_back(h); return FALSE; }
                return TRUE;
            }
            bool mesa = false;
            if (pid == cx->me) {  // do Shell só a mesa (papel de parede); o resto é o próprio vidro
                wchar_t c[32] = {}; GetClassNameW(h, c, 32);
                if (wcscmp(c, L"SequoiaMesa")) return TRUE;
                mesa = true;
            }
            if (cx->out->size() >= 10 && !mesa) return TRUE;  // cheio: só falta a mesa, que fica sempre (embaixo de tudo)
            // camadas transparentes por cima de tudo (sombras de janelas, o overlay da NVIDIA): não são conteúdo
            // e a captura devolve elas pretas, o que escurecia o vidro inteiro
            LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
            if ((ex & WS_EX_TRANSPARENT) || ((ex & WS_EX_LAYERED) && (ex & WS_EX_NOACTIVATE))) return TRUE;
            RECT r = Bounds(h), x;
            if (r.right - r.left < 2 || r.bottom - r.top < 2 || !IntersectRect(&x, &r, &mon)) return TRUE;
            // janelas cobertas também: no Alt+Tab a de trás vem para a frente e o vidro já tem o quadro dela
            // (sem isso o vidro mostrava a janela antiga por alguns quadros, até a captura nova abrir)
            cx->out->push_back(h);
            if (mesa && (ex & WS_EX_LAYERED)) { cx->afterMesa = true; return TRUE; }  // mesa transparente: falta o fundo dela
            return !mesa;  // a mesa é a última
        }, (LPARAM)&cx);
        DeleteObject(cx.covered); DeleteObject(cx.tmp); DeleteObject(cx.screen);
        std::reverse(want.begin(), want.end());
    }
    bool changed = false;
    std::vector<std::shared_ptr<WinCap>> next;
    std::vector<HWND> toStart;
    for (HWND h : want) {
        std::shared_ptr<WinCap> c;
        for (auto& o : caps) if (o->h == h) { c = o; break; }
        if (!c) for (auto& [gh, gc] : got) if (gh == h && gc) { c = gc; changed = true; break; }
        if (!c) {
            auto f = capFailed.find(h);
            bool failedRecently = f != capFailed.end() && GetTickCount() - f->second < 5000;
            if (!failedRecently && std::find(pending.begin(), pending.end(), h) == pending.end()) { pending.push_back(h); toStart.push_back(h); }
            continue;
        }
        RECT wr; GetWindowRect(h, &wr);
        if (!EqualRect(&wr, &c->rect)) changed = true;  // mexeu (sem quadro novo: o conteúdo é o mesmo)
        next.push_back(c);
    }
    for (auto& [gh, gc] : got) if (gc && std::find(next.begin(), next.end(), gc) == next.end()) gc->Close();  // não precisa mais
    for (auto& o : caps) if (std::find(next.begin(), next.end(), o) == next.end()) { o->Close(); changed = true; }
    if (!changed) for (size_t i = 0; i < next.size(); i++) if (i >= caps.size() || next[i] != caps[i]) { changed = true; break; }
    caps = next;
    if (changed) { capDirty = true; allDirty = true; }
    if (capFailed.size() > 256) capFailed.clear();
    if (!toStart.empty()) {
        std::thread([toStart] {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
            for (HWND h : toStart) {
                auto c = StartCap(h);
                { std::lock_guard<std::mutex> lk(readyMx); ready.emplace_back(h, c); }
                if (!posted.exchange(true)) PostMessageW(msgWnd, WM_RG_FRAME, 0, 0);
            }
            winrt::uninit_apartment();
        }).detach();
    }
}

// Área do fundo que as peças usam (coordenadas da tela principal): as janelas de vidro visíveis mais uma
// margem para o embaçado e a lente. A montagem desenha só ela, em vez da tela inteira a cada quadro.
// Cada peça tem a sua área (antes era um retângulo só juntando todas: a barra de menus em cima e o Dock
// embaixo davam a tela inteira, montada de novo a cada quadro de qualquer janela).
const LONG COMPOSE_MARGIN = 64;
std::vector<RECT> composed;  // as áreas montadas da última vez
// Peça escondida atrás de um app em tela cheia (YouTube/F11/jogo): a janela da frente cobre o monitor inteiro e
// está acima dela. Os widgets da mesa continuam "visíveis" atrás do vídeo, e cada quadro do vídeo remontava o vidro
// deles (~60 vezes/s, 19/11/2026: o YouTube em tela cheia engasgava com o tema Golden Gate).
bool CoveredByFullscreen(HWND piece) {
    static DWORD at; static HWND lastFg, lastPiece; static bool lastRes;
    HWND fg = GetForegroundWindow();
    if (GetTickCount() - at < 100 && fg == lastFg && piece == lastPiece) return lastRes;
    at = GetTickCount(); lastFg = fg; lastPiece = piece;
    lastRes = false;
    if (!fg || fg == piece || !IsWindowVisible(fg) || IsIconic(fg) || Cloaked(fg)) return false;
    DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
    if (pid == GetCurrentProcessId()) return false;  // janela do próprio Shell (Launchpad, Mission Control): o vidro é dela
    if (GetWindowLongW(piece, GWL_EXSTYLE) & WS_EX_TOPMOST && !(GetWindowLongW(fg, GWL_EXSTYLE) & WS_EX_TOPMOST)) return false;
    MONITORINFO mi{ sizeof(mi) };
    if (!GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    RECT r; GetWindowRect(fg, &r);
    if (r.left > mi.rcMonitor.left || r.top > mi.rcMonitor.top || r.right < mi.rcMonitor.right || r.bottom < mi.rcMonitor.bottom) return false;
    RECT pr; GetWindowRect(piece, &pr);
    RECT x; if (!IntersectRect(&x, &pr, &mi.rcMonitor) || !EqualRect(&x, &pr)) return false;  // a peça sai do monitor do app
    for (HWND w = GetWindow(piece, GW_HWNDPREV); w; w = GetWindow(w, GW_HWNDPREV)) if (w == fg) { lastRes = true; break; }  // o app está acima
    return lastRes;
}
std::vector<RECT> GlassAreas() {
    std::vector<RECT> v;
    RECT all{ 0, 0, (LONG)texW, (LONG)texH };
    for (auto& [hh, e] : g_entries) {
        if (!e.real || !IsWindowVisible(e.hwnd) || CoveredByFullscreen(e.hwnd)) continue;
        RECT r; GetWindowRect(e.hwnd, &r);
        OffsetRect(&r, -mon.left, -mon.top);
        InflateRect(&r, COMPOSE_MARGIN, COMPOSE_MARGIN);
        if (!IntersectRect(&r, &r, &all)) continue;
        bool merged = false;  // sobrepostas viram uma só
        for (auto& o : v) { RECT x; if (IntersectRect(&x, &o, &r)) { UnionRect(&o, &o, &r); merged = true; break; } }
        if (!merged) v.push_back(r);
    }
    return v;
}
bool Inside(const RECT& r, const std::vector<RECT>& areas) {
    for (auto& a : areas) { RECT x; if (IntersectRect(&x, &r, &a) && EqualRect(&x, &r)) return true; }
    return false;
}
// A peça está numa parte do fundo que já foi montada? (senão, monta de novo antes de desenhar)
bool Composed(HWND h) {
    RECT r; GetWindowRect(h, &r);
    OffsetRect(&r, -mon.left, -mon.top);
    RECT all{ 0, 0, (LONG)texW, (LONG)texH };
    IntersectRect(&r, &r, &all);
    return IsRectEmpty(&r) || Inside(r, composed);
}

// Monta o fundo em tex: preto, a mesa e as janelas na ordem (só na área das peças).
void Compose() {
    if (!target) return;
    std::vector<RECT> areas = GlassAreas();
    composed = areas;
    { std::lock_guard<std::mutex> lk(areasMx); areasSnap = areas; }
    for (auto& c : caps) {  // quem está atrás de alguma peça: só o quadro novo dessas monta o fundo de novo
        RECT b = Bounds(c->h), x;
        OffsetRect(&b, -mon.left, -mon.top);
        bool rel = false;
        for (auto& a : areas) if (IntersectRect(&x, &b, &a)) { rel = true; break; }
        c->relevant = rel;
    }
    if (areas.empty()) return;
    if (!allDirty) {  // só quadros novos de janelas longe das peças: nada a montar (nem abrir o desenho)
        bool any = false;
        for (auto& c : caps) {
            if (!c->fresh) continue;
            RECT b = Bounds(c->h), x;
            OffsetRect(&b, -mon.left, -mon.top);
            for (auto& a : areas) if (IntersectRect(&x, &b, &a)) { any = true; break; }
            if (any) break;
        }
        if (!any) { for (auto& c : caps) c->fresh = false; lastDirty.clear(); return; }
    }
    std::vector<std::unique_lock<std::mutex>> locks;  // os quadros não são trocados até o desenho terminar
    for (auto& c : caps) locks.emplace_back(c->m);
    util->SetTarget(target);
    util->BeginDraw();
    util->SetTransform(D2D1::Matrix3x2F::Identity());
    std::vector<winrt::com_ptr<ID2D1Bitmap1>> keep;
    bool all = allDirty.exchange(false);
    lastDirty.clear();
    for (auto& area : areas) {
    if (!all) {  // só quadro novo: esta área só se alguma janela atrás dela mudou
        bool d = false;
        for (auto& c : caps) {
            if (!c->fresh) continue;
            RECT b = Bounds(c->h), x;
            OffsetRect(&b, -mon.left, -mon.top);
            if (IntersectRect(&x, &b, &area)) { d = true; break; }
        }
        if (!d) continue;
    }
    lastDirty.push_back(area);
    util->PushAxisAlignedClip(D2D1::RectF((float)area.left, (float)area.top, (float)area.right, (float)area.bottom), D2D1_ANTIALIAS_MODE_ALIASED);
    util->Clear(D2D1::ColorF(0, 0, 0, 1));
    for (auto& c : caps) {
        if (!c->frame) continue;
        {  // janela fora da área das peças: não precisa desenhar
            RECT b = Bounds(c->h), x;
            OffsetRect(&b, -mon.left, -mon.top);
            if (!IntersectRect(&x, &b, &area)) { GetWindowRect(c->h, &c->rect); continue; }
        }
        try {
            auto access = c->frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
            winrt::com_ptr<IDXGISurface> sf;
            winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(sf.put())));
            winrt::com_ptr<ID2D1Bitmap1> bmp;
            auto bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            winrt::check_hresult(util->CreateBitmapFromDxgiSurface(sf.get(), &bp, bmp.put()));
            auto cs = c->frame.ContentSize();
            RECT wr; GetWindowRect(c->h, &wr);
            RECT at = wr;  // o quadro é a janela inteira, ou só a parte visível (sem a borda invisível)
            if (cs.Width != wr.right - wr.left || cs.Height != wr.bottom - wr.top) at = Bounds(c->h);
            float x = (float)(at.left - mon.left), y = (float)(at.top - mon.top);
            util->DrawBitmap(bmp.get(), D2D1::RectF(x, y, x + cs.Width, y + cs.Height), 1, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
                             D2D1::RectF(0, 0, (float)cs.Width, (float)cs.Height));
            keep.push_back(bmp);
            c->rect = wr;
        } catch (...) {}
    }
    util->PopAxisAlignedClip();
    }
    for (auto& c : caps) c->fresh = false;
    HRESULT hr = util->EndDraw();
    util->SetTarget(nullptr);
    {   // nenhuma janela entregou quadro (captura de janelas recusada: VM, área remota, política): o fundo do vidro
        // ficaria PRETO (Dock, barra de menus e painéis pretos). Depois de 3 s assim, o vidro real desiste e volta o fosco.
        static DWORD vazio = 0;
        // Só desiste se a captura NUNCA funcionou: com ela já funcionando, um fundo vazio é passageiro (a Captura
        // de Tela, um jogo em tela cheia ou o bloqueio cobrindo tudo) e antes desligava o vidro até reiniciar.
        if (!keep.empty()) { vazio = 0; capJaFuncionou = true; }
        else if (!vazio) vazio = GetTickCount();
        else if (GetTickCount() - vazio > 3000 && !capJaFuncionou) semCaptura = true;
    }
    static size_t lastN = (size_t)-1;
    if (caps.size() != lastN || FAILED(hr)) {  // só quando muda o número de janelas (antes logava ~1,5 vez por segundo)
        lastN = caps.size();
        Log(L"vidro real: fundo com %zu janelas (%zu com quadro, desenho 0x%08X)", lastN, keep.size(), (unsigned)hr);
    }
}

// Janela da frente mudou, ou uma janela apareceu, sumiu, mudou de ordem ou de lugar: avisa (um aviso por
// vez) e o fundo é montado já, sem esperar o temporizador de 16 ms.
void CALLBACK OnWinChange(HWINEVENTHOOK, DWORD ev, HWND h, LONG obj, LONG child, DWORD, DWORD) {
    if (!active || !h) return;
    if (ev != EVENT_SYSTEM_FOREGROUND && (obj != OBJID_WINDOW || child != CHILDID_SELF || GetAncestor(h, GA_ROOT) != h)) return;
    if (!posted.exchange(true)) PostMessageW(msgWnd, WM_RG_FRAME, 0, 0);
}

// Diagnóstico (mensagem ShellVidroFoto): grava em bin\vidro-fundo.bmp o fundo montado e em bin\vidro-N.bmp o
// último quadro de cada janela capturada, com a lista em bin\vidro-lista.txt.
void SaveTex(ID3D11Texture2D* src, UINT w, UINT h, const std::wstring& path) {
    D3D11_TEXTURE2D_DESC d{}; src->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    winrt::com_ptr<ID3D11Texture2D> st;
    if (FAILED(d3d->CreateTexture2D(&d, nullptr, st.put()))) return;
    ctx->CopyResource(st.get(), src);
    D3D11_MAPPED_SUBRESOURCE ms{};
    if (FAILED(ctx->Map(st.get(), 0, D3D11_MAP_READ, 0, &ms))) return;
    w = std::min(w, d.Width); h = std::min(h, d.Height);
    BITMAPFILEHEADER fh{}; BITMAPINFOHEADER ih{ sizeof(ih), (LONG)w, -(LONG)h, 1, 32, BI_RGB };
    fh.bfType = 0x4D42; fh.bfOffBits = sizeof(fh) + sizeof(ih); fh.bfSize = fh.bfOffBits + w * h * 4;
    FILE* f = nullptr;
    if (!_wfopen_s(&f, path.c_str(), L"wb") && f) {
        fwrite(&fh, sizeof(fh), 1, f); fwrite(&ih, sizeof(ih), 1, f);
        for (UINT y = 0; y < h; y++) fwrite((BYTE*)ms.pData + (size_t)y * ms.RowPitch, 4, w, f);
        fclose(f);
    }
    ctx->Unmap(st.get(), 0);
}

void Dump() {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe; dir = dir.substr(0, dir.find_last_of(L'\\'));
    if (tex) SaveTex(tex, texW, texH, dir + L"\\vidro-fundo.bmp");
    FILE* lf = nullptr; _wfopen_s(&lf, (dir + L"\\vidro-lista.txt").c_str(), L"w, ccs=UTF-8");
    int i = 0;
    for (auto& c : caps) {
        wchar_t cl[64] = {}; GetClassNameW(c->h, cl, 64);
        RECT wr; GetWindowRect(c->h, &wr);
        std::lock_guard<std::mutex> lk(c->m);
        int fw = 0, fh = 0;
        if (c->frame) {
            try {
                auto cs = c->frame.ContentSize(); fw = cs.Width; fh = cs.Height;
                auto access = c->frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
                winrt::com_ptr<ID3D11Texture2D> t;
                if (SUCCEEDED(access->GetInterface(IID_PPV_ARGS(t.put())))) SaveTex(t.get(), fw, fh, dir + L"\\vidro-" + std::to_wstring(i) + L".bmp");
            } catch (...) {}
        }
        if (lf) fwprintf(lf, L"%d %ls %ld,%ld-%ld,%ld quadro %dx%d\n", i, cl, wr.left, wr.top, wr.right, wr.bottom, fw, fh);
        i++;
    }
    if (lf) fclose(lf);
    Log(L"vidro real: diagnóstico gravado (%d janelas)", i);
}

void Poll(bool refresh) {
    static HWINEVENTHOOK fg = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnWinChange, 0, 0, WINEVENT_OUTOFCONTEXT);
    static HWINEVENTHOOK ch = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_REORDER, nullptr, OnWinChange, 0, 0, WINEVENT_OUTOFCONTEXT | shell::SemProprio());
    static bool mv = (jogo::GanchoPosicao(OnWinChange, WINEVENT_OUTOFCONTEXT | shell::SemProprio()), true);  // sai no modo jogo (jogo.cpp)
    (void)fg; (void)ch; (void)mv;
    if (g_busy) { capDirty = true; return; }  // chamada de dentro dela mesma: o temporizador pega depois
    g_busy = true;
    struct Done { ~Done() { g_busy = false; } } done;
    double t0 = anim::Now();
    if (refresh) RefreshWindows();
    {  // uma peça apareceu ou cresceu para fora da área montada
        for (auto& a : GlassAreas()) if (!Inside(a, composed)) { capDirty = true; allDirty = true; break; }
    }
    bool go = capDirty.exchange(false);
    if (!go && frameDirty && anim::Now() - lastFrameCompose >= 15) go = true;
    if (!go) return;
    frameDirty = false;
    lastFrameCompose = anim::Now();
    st_composes++;
    Compose();
    for (auto& [hh, e] : g_entries) {
        if (!e.real || !IsWindowVisible(e.hwnd)) continue;
        RECT r; GetWindowRect(e.hwnd, &r);
        OffsetRect(&r, -mon.left, -mon.top);
        bool d = false;
        for (auto& a : lastDirty) { RECT x; if (IntersectRect(&x, &r, &a)) { d = true; break; } }
        if (d) Redraw(e);  // o fundo atrás desta peça mudou
    }
    static int slow = 0;
    double ms = anim::Now() - t0;
    if (ms > 12 && slow++ < 20) Log(L"vidro real: montagem lenta, %.1f ms", ms);
}

bool Init() {
    if (ok) return true;
    if (tried && GetTickCount() - lastTry < 10000) return false;  // não insiste a cada janela nova
    tried = true; lastTry = GetTickCount();
    try {
        if (!wgc::GraphicsCaptureSession::IsSupported()) { Log(L"vidro real: captura de janelas indisponível"); return false; }
        if (!d3d) {
            HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, &ctx);
            if (FAILED(hr)) { Log(L"vidro real: D3D11 0x%08X", (unsigned)hr); return false; }
            if (ID3D11Multithread* mt = nullptr; SUCCEEDED(d3d->QueryInterface(IID_PPV_ARGS(&mt)))) { mt->SetMultithreadProtected(TRUE); mt->Release(); }
            winrt::com_ptr<IDXGIDevice> dx; d3d->QueryInterface(IID_PPV_ARGS(dx.put()));
            D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr, (void**)&fac);
            winrt::check_hresult(fac->CreateDevice(dx.get(), &d2d));
            winrt::check_hresult(d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &util));
            auto ci = g_comp.as<ABI::Windows::UI::Composition::ICompositorInterop>();
            winrt::check_hresult(ci->CreateGraphicsDevice(d2d, reinterpret_cast<ABI::Windows::UI::Composition::ICompositionGraphicsDevice**>(winrt::put_abi(gfx))));
            winrt::com_ptr<::IInspectable> insp;
            winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dx.get(), insp.put()));
            wdev = insp.as<wd3::IDirect3DDevice>();
            const UINT N = 128;  // ruído monocromático (dithering do estudo: 2-3%)
            std::vector<UINT32> px(N * N);
            std::mt19937 rnd(7);
            for (auto& p : px) { BYTE v = (BYTE)(rnd() & 255); p = 0xFF000000u | (v << 16) | (v << 8) | v; }
            util->CreateBitmap(D2D1::SizeU(N, N), px.data(), N * 4, D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &noise);
            WNDCLASSW wc{}; wc.lpfnWndProc = MsgProc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"ShellVidroMsg";
            RegisterClassW(&wc);
            // janela comum escondida (não "só de mensagem"): a Captura de Tela acha ela com FindWindow
            msgWnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"ShellVidroMsg", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
        }
        HMONITOR hm = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
        MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(hm, &mi); mon = mi.rcMonitor;
        texW = mon.right - mon.left; texH = mon.bottom - mon.top;
        D3D11_TEXTURE2D_DESC nd{};
        nd.Width = texW; nd.Height = texH; nd.MipLevels = 1; nd.ArraySize = 1; nd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        nd.SampleDesc.Count = 1; nd.Usage = D3D11_USAGE_DEFAULT; nd.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        winrt::check_hresult(d3d->CreateTexture2D(&nd, nullptr, &tex));
        winrt::com_ptr<IDXGISurface> sf; tex->QueryInterface(IID_PPV_ARGS(sf.put()));
        auto bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
        winrt::check_hresult(util->CreateBitmapFromDxgiSurface(sf.get(), &bp, &screen));
        auto tp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        winrt::check_hresult(util->CreateBitmapFromDxgiSurface(sf.get(), &tp, &target));
        ok = true;
        Log(L"vidro real: pronto (montado pelas janelas, %ux%u)", texW, texH);
    } catch (winrt::hresult_error const& e) {
        Log(L"vidro real: indisponível (0x%08X %ls)", (unsigned)e.code(), e.message().c_str());
        ok = false;
    } catch (...) { ok = false; }
    return ok;
}

bool semCaptura = false;  // a captura de janelas não funciona aqui: o vidro real fica desligado (senão tudo fica preto)
void SetActive(bool on) {
    if (on && semCaptura) return;
    if (on && !Init()) return;
    if (!ok || on == active) return;
    active = on;
    try {
        if (on) { SetTimer(msgWnd, T_RG, 250, nullptr); SetTimer(msgWnd, T_RG + 1, 16, nullptr); }
        else { KillTimer(msgWnd, T_RG); KillTimer(msgWnd, T_RG + 1); StopCaps(); }
    } catch (...) {}
    ApplyAffinity();
    Log(L"vidro real: %ls", on ? L"ligado" : L"desligado");
    for (auto& [hh, e] : g_entries) { try { Apply(e); } catch (...) {} }  // as peças já montadas passam para o vidro novo
}

// Mapa de deslocamento de uma peça arredondada (px): dentro da faixa da borda (bezel), cada ponto lê a
// tela um pouco mais para dentro, com intensidade (1 - t)^2 (t = 0 na borda, 1 no fim da faixa): o perfil
// de uma lente convexa; no miolo, nada (o fundo aparece no lugar). R = deslocamento em x, G = em y.
// Quanto o fundo "anda" em cada ponto da borda (0..1), pela refração de verdade (estudo da kube.io sobre o
// Liquid Glass): a borda é um squircle convexo h(t) = (1 - (1 - t)^4)^(1/4) (t = 0 na beirada, 1 no fim da
// faixa); a inclinação dá o ângulo da superfície, a lei de Snell (vidro n = 1,5) dá o ângulo de saída e o
// desvio é tan(entrada - saída). Forte bem na beirada e caindo rápido para dentro, como no Tahoe.
float SnellAt(float t) {
    static float lut[257];
    static bool init = false;
    if (!init) {
        const float n = 1.5f, SLOPE = 2.5f;  // SLOPE: espessura da borda em relação à faixa
        float mx = 0;
        for (int i = 0; i <= 256; i++) {
            float x = std::max(i / 256.f, 1e-4f);
            float u = 1 - x;
            float slope = u * u * u / std::pow(std::max(1e-6f, 1 - u * u * u * u), 0.75f) * SLOPE;
            float th = std::atan(slope);
            float tr = std::asin(std::sin(th) / n);
            lut[i] = std::tan(th - tr);
            mx = std::max(mx, lut[i]);
        }
        for (auto& v : lut) v = mx > 0 ? v / mx : 0;
        init = true;
    }
    float f = std::clamp(t, 0.f, 1.f) * 256;
    int i = std::min(255, (int)f);
    return lut[i] + (lut[i + 1] - lut[i]) * (f - i);
}

// Brilho especular da borda (o "rim" do Liquid Glass): a luz vem de cima à esquerda; onde a normal da borda
// aponta para ela o vidro acende (o canto de cima à esquerda é o mais forte), do lado oposto aparece um reflexo
// mais fraco (a luz que atravessa e volta), e dos lados quase some. Um fio nítido na beirada e um brilho suave
// que entra um pouco no vidro (a curvatura). Branco pré-multiplicado, desenhado por cima da peça.
std::map<std::wstring, ID2D1Bitmap*> specs;
ID2D1Bitmap* SpecFor(int w, int h, float rad, float px, bool dark) {
    wchar_t key[96];
    swprintf_s(key, L"%d|%d|%.1f|%.2f|%d", w, h, rad, px, (int)dark);
    auto it = specs.find(key);
    if (it != specs.end()) return it->second;
    if (specs.size() > 48) { for (auto& [k, b] : specs) if (b) b->Release(); specs.clear(); }
    std::vector<UINT32> out((size_t)w * h);
    float hx = w / 2.f, hy = h / 2.f, r = std::min({ rad, hx, hy });
    // fio fino como o do Dock real do macOS 26 (print do usuário, 16/11/2026): antes o brilho entrava até 12 px no
    // vidro e a borda parecia uma linha grossa; agora no máximo 4 px, e bem mais fraco
    float band = std::clamp(std::min(w, h) * 0.08f, 2 * px, 6 * px);
    // a luz vem de cima (Dock real do macOS 26 sobre preto, 16/11/2026: fio 78 em cima, 91 embaixo, ~13 dos lados;
    // antes vinha da diagonal e acendia o lado esquerdo)
    const float Lx = 0.f, Ly = -1.f;
    float gain = dark ? 0.80f : 0.85f;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float px_ = x + 0.5f - hx, py = y + 0.5f - hy;
            float ax = std::fabs(px_), ay = std::fabs(py);
            float qx = ax - (hx - r), qy = ay - (hy - r);
            float sdf, nx, ny;
            if (qx > 0 && qy > 0) {
                float l = std::sqrt(qx * qx + qy * qy);
                sdf = l - r;
                nx = l > 0 ? qx / l : 0.7071f; ny = l > 0 ? qy / l : 0.7071f;
            } else if (qx > qy) { sdf = qx - r; nx = 1; ny = 0; }
            else { sdf = qy - r; nx = 0; ny = 1; }
            if (px_ < 0) nx = -nx;
            if (py < 0) ny = -ny;
            float cov = std::clamp(0.5f - sdf, 0.f, 1.f);  // borda com antisserrilhado
            if (cov <= 0) continue;
            float d = std::max(0.f, -sdf);
            if (d > band) continue;
            float soft = 1 - d / band; soft *= soft * soft;
            float edge = 0.8f * std::exp(-d / (0.55f * px)) + 0.16f * soft;
            float a = nx * Lx + ny * Ly;
            float ang = 0.04f + 0.85f * std::pow(std::max(a, 0.f), 2.f) + 1.0f * std::pow(std::max(-a, 0.f), 2.f);  // embaixo um pouco mais forte, como no Mac
            float al = std::clamp(cov * edge * ang * gain, 0.f, 1.f);
            BYTE v = (BYTE)std::lround(al * 255);
            out[(size_t)y * w + x] = ((UINT32)v << 24) | ((UINT32)v << 16) | ((UINT32)v << 8) | v;
        }
    }
    ID2D1Bitmap* b = nullptr;
    util->CreateBitmap(D2D1::SizeU(w, h), out.data(), w * 4, D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &b);
    specs[key] = b;
    return b;
}

ID2D1Bitmap* MapFor(int w, int h, float rad, float bezel, float dmax, float scale) {
    wchar_t key[96];
    swprintf_s(key, L"%d|%d|%.1f|%.1f|%.1f", w, h, rad, bezel, dmax);
    auto it = maps.find(key);
    if (it != maps.end()) return it->second;
    if (maps.size() > 64) { for (auto& [k, b] : maps) if (b) b->Release(); maps.clear(); }
    std::vector<UINT32> px((size_t)w * h);
    float hx = w / 2.f, hy = h / 2.f, r = std::min({ rad, hx, hy });
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float px_ = x + 0.5f - hx, py = y + 0.5f - hy;
            float ax = std::fabs(px_), ay = std::fabs(py);
            float qx = ax - (hx - r), qy = ay - (hy - r);
            float sdf, nx, ny;  // distância (negativa dentro) e normal para fora
            if (qx > 0 && qy > 0) {
                float l = std::sqrt(qx * qx + qy * qy);
                sdf = l - r;
                nx = l > 0 ? qx / l : 0.7071f; ny = l > 0 ? qy / l : 0.7071f;
            } else if (qx > qy) { sdf = qx - r; nx = 1; ny = 0; }
            else { sdf = qy - r; nx = 0; ny = 1; }
            if (px_ < 0) nx = -nx;
            if (py < 0) ny = -ny;
            float t = std::clamp(-sdf / bezel, 0.f, 1.f);
            float mag = dmax * SnellAt(t);
            float dx = -nx * mag, dy = -ny * mag;  // lê para dentro
            BYTE R = (BYTE)std::clamp((int)std::lround((0.5f + dx / scale) * 255), 0, 255);
            BYTE G = (BYTE)std::clamp((int)std::lround((0.5f + dy / scale) * 255), 0, 255);
            px[(size_t)y * w + x] = 0xFF000000u | (R << 16) | (G << 8) | 128;
        }
    }
    ID2D1Bitmap* b = nullptr;
    util->CreateBitmap(D2D1::SizeU(w, h), px.data(), w * 4, D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &b);
    maps[key] = b;
    return b;
}

// Desenha uma peça: a tela atrás de [sx, sy, sx + w, sy + h] (px da tela capturada) em (ox, oy) do dc
// Os efeitos do vidro ficam guardados (por contexto de desenho) em vez de criados a cada peça a cada quadro: eram 5
// (10 com a aberração cromática) por peça, ~60 vezes por segundo com uma janela mudando atrás do vidro (17/11/2026)
struct FxCache { ID2D1DeviceContext* dc = nullptr; ID2D1Effect* e[10]{}; } g_fx;
ID2D1Effect* Fx(ID2D1DeviceContext* dc, int i, REFCLSID id) {
    if (g_fx.dc != dc) {
        for (auto& e : g_fx.e) if (e) { e->Release(); e = nullptr; }
        if (g_fx.dc) g_fx.dc->Release();
        g_fx.dc = dc; dc->AddRef();  // segura o contexto: o ponteiro não pode ser reaproveitado por outro enquanto guardado
    }
    if (!g_fx.e[i]) dc->CreateEffect(id, &g_fx.e[i]);
    return g_fx.e[i];
}

void DrawPiece(ID2D1DeviceContext* dc, float ox, float oy, float sx, float sy, float w, float h, float rad, float px, D2D1_COLOR_F tint, bool withTint, float minFrost = 0) {
    const auto& st = g_style;
    float k = std::clamp(st.refraction, 0.f, 1.f);
    // Filtro glassBackground do macOS 26 (guia macOS26-Theme-Reference, seção 29, lido da árvore de camadas):
    // InnerRefractionHeight (a faixa da borda que dobra) = 0,25 × H, com teto de 20 px; InnerRefractionAmount
    // (quanto ela puxa para dentro) = 0,8 × H, com teto de 60 px; H = o lado menor da peça. Só para dentro.
    // Refração 0,6 (padrão dos Ajustes) = exatamente o medido; 0 = 40 % disso; 1 = 140 %.
    float H = std::min(w, h);
    float bezel = std::clamp(H * 0.25f, 4 * px, 20 * px);
    float dmax = std::min(H * 0.8f, 60 * px) * (0.4f + 1.0f * k);
    float scale = 2 * dmax + 2;
    // Transparência (vidro_tinta): 0 = quase nítido, 0,5 = o do Tahoe, 1 = bem fosco
    // macOS 27 Golden Gate: mesmo no mínimo o vidro continua fosco (o controle vai de "quase transparente" a opaco;
    // vídeo do MacVince e capturas do 512 Pixels: o fundo sempre aparece bem desfocado)
    float sigma = std::max(0.6f + 11.f * st.tintK, minFrost) * px;
    float m = 3 * sigma + dmax + 2;
    ID2D1Effect *crop = Fx(dc, 0, CLSID_D2D1Crop), *blur = Fx(dc, 1, CLSID_D2D1GaussianBlur), *sat = Fx(dc, 2, CLSID_D2D1Saturation),
                *disp = Fx(dc, 3, CLSID_D2D1DisplacementMap), *mapT = Fx(dc, 4, CLSID_D2D12DAffineTransform);
    // o mapa só depende da distância até a borda: num painel crescendo (Spotlight digitando), um mapa por degrau de
    // 32 px esticado até o tamanho exato (diferença invisível) em vez de um mapa novo (~5 ms) a cada quadro
    const int STEP = 32;
    int mw = (int)std::ceil(w), mh = (int)std::ceil(h);
    int qw = mw > 2 * STEP ? (mw + STEP - 1) / STEP * STEP : mw, qh = mh > 2 * STEP ? (mh + STEP - 1) / STEP * STEP : mh;
    ID2D1Bitmap* map = MapFor(qw, qh, rad, bezel, dmax, scale);
    if (crop && blur && sat && disp && mapT && map) {
        crop->SetInput(0, screen);
        crop->SetValue(D2D1_CROP_PROP_RECT, D2D1::Vector4F(sx - m, sy - m, sx + w + m, sy + h + m));
        crop->SetValue(D2D1_CROP_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
        blur->SetInputEffect(0, crop);
        blur->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, std::max(0.01f, sigma));
        blur->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_HARD);
        sat->SetInputEffect(0, blur);
        sat->SetValue(D2D1_SATURATION_PROP_SATURATION, 1.8f);  // "saturate(180%)"
        mapT->SetInput(0, map);
        mapT->SetValue(D2D1_2DAFFINETRANSFORM_PROP_TRANSFORM_MATRIX, D2D1::Matrix3x2F::Scale((float)mw / qw, (float)mh / qh) * D2D1::Matrix3x2F::Translation(sx, sy));
        disp->SetInputEffect(0, sat);
        disp->SetInputEffect(1, mapT);
        disp->SetValue(D2D1_DISPLACEMENTMAP_PROP_SCALE, scale);
        disp->SetValue(D2D1_DISPLACEMENTMAP_PROP_X_CHANNEL_SELECT, D2D1_CHANNEL_SELECTOR_R);
        disp->SetValue(D2D1_DISPLACEMENTMAP_PROP_Y_CHANNEL_SELECT, D2D1_CHANNEL_SELECTOR_G);
        // Aberração cromática (o Liquid Glass do macOS 26): na borda da lente cada cor dobra um pouco diferente e
        // aparece uma franja colorida. Três deslocamentos (vermelho mais forte, azul mais fraco), cada um só com o
        // seu canal, somados. No meio da peça o deslocamento é zero e as três cores coincidem (sem franja).
        // vidro_cromatico=0 desliga (um deslocamento só, como antes).
        static int cromatico = -1;
        if (cromatico < 0) cromatico = shell::Setting(L"vidro_cromatico", L"1") != L"0";
        ID2D1Effect *d2 = cromatico ? Fx(dc, 5, CLSID_D2D1DisplacementMap) : nullptr, *d3 = cromatico ? Fx(dc, 6, CLSID_D2D1DisplacementMap) : nullptr,
                    *m1 = cromatico ? Fx(dc, 7, CLSID_D2D1ColorMatrix) : nullptr, *m2 = cromatico ? Fx(dc, 8, CLSID_D2D1ColorMatrix) : nullptr,
                    *m3 = cromatico ? Fx(dc, 9, CLSID_D2D1ColorMatrix) : nullptr;
        if (cromatico && d2 && d3 && m1 && m2 && m3) {
            const float K = 0.10f;  // quanto o vermelho e o azul se afastam do verde
            ID2D1Effect* ds[3] = { disp, d2, d3 };
            float sc3[3] = { scale * (1 + K), scale, scale * (1 - K) };
            ID2D1Effect* ms[3] = { m1, m2, m3 };
            for (int k = 0; k < 3; k++) {
                if (k) {
                    ds[k]->SetInputEffect(0, sat);
                    ds[k]->SetInputEffect(1, mapT);
                    ds[k]->SetValue(D2D1_DISPLACEMENTMAP_PROP_X_CHANNEL_SELECT, D2D1_CHANNEL_SELECTOR_R);
                    ds[k]->SetValue(D2D1_DISPLACEMENTMAP_PROP_Y_CHANNEL_SELECT, D2D1_CHANNEL_SELECTOR_G);
                }
                ds[k]->SetValue(D2D1_DISPLACEMENTMAP_PROP_SCALE, sc3[k]);
                D2D1_MATRIX_5X4_F cm{};  // só o canal k (pré-multiplicado) e 1/3 do alfa: os três somados dão a cor e o alfa originais
                if (k == 0) cm._11 = 1; else if (k == 1) cm._22 = 1; else cm._33 = 1;
                cm._44 = 1.f / 3.f;
                ms[k]->SetInputEffect(0, ds[k]);
                ms[k]->SetValue(D2D1_COLORMATRIX_PROP_COLOR_MATRIX, cm);
                ms[k]->SetValue(D2D1_COLORMATRIX_PROP_ALPHA_MODE, D2D1_COLORMATRIX_ALPHA_MODE_STRAIGHT);  // a matriz direto nos valores pré-multiplicados: somados dão (R, G, B, alfa).
                // (com PREMULTIPLIED o efeito desfaz e refaz o pré-multiplicado e cada canal saía com 1/3: o vidro inteiro ficava
                // escuro e "tingido", 17/11/2026)
                dc->DrawImage(ms[k], D2D1::Point2F(ox, oy), D2D1::RectF(sx, sy, sx + w, sy + h), D2D1_INTERPOLATION_MODE_LINEAR,
                              k == 0 ? D2D1_COMPOSITE_MODE_SOURCE_OVER : D2D1_COMPOSITE_MODE_PLUS);
            }
        } else dc->DrawImage(disp, D2D1::Point2F(ox, oy), D2D1::RectF(sx, sy, sx + w, sy + h), D2D1_INTERPOLATION_MODE_LINEAR, D2D1_COMPOSITE_MODE_SOURCE_OVER);
    }
    if (withTint) {
        ID2D1SolidColorBrush* b = nullptr;
        D2D1_COLOR_F t = backdrop::AdjustTint(tint);
        // peças com texto (Spotlight, Central): a tinta não some com a "Transparência do vidro" no mínimo,
        // senão o texto fica sobre o fundo quase nítido e não dá para ler
        // (17/11/2026, o usuário: o painel de Apps sobre uma página branca ficou branco, nomes ilegíveis): no mínimo 70 %
        // da tinta normal; daí para cima segue o slider
        if (minFrost > 0) t.a = std::max(t.a, tint.a * std::min(1.f, 0.70f + 0.6f * g_style.tintK));
        dc->CreateSolidColorBrush(t, &b);
        if (b) { dc->FillRectangle(D2D1::RectF(ox, oy, ox + w, oy + h), b); b->Release(); }
    }
    {   // véu branco de 5% por dentro (rgba(255,255,255,0.05) do estudo; a referência é mais clara por dentro)
        ID2D1SolidColorBrush* v = nullptr;
        dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.05f * std::min(1.f, 2 * g_style.tintK)), &v);  // some com a transparência no mínimo (no Golden Gate fica)
        if (v) { dc->FillRectangle(D2D1::RectF(ox, oy, ox + w, oy + h), v); v->Release(); }
    }
    if (noise) {  // ruído de 2,5%: some com os degraus de cor do desfoque
        ID2D1BitmapBrush* nb = nullptr;
        dc->CreateBitmapBrush(noise, D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP), D2D1::BrushProperties(0.025f), &nb);
        if (nb) { dc->FillRectangle(D2D1::RectF(ox, oy, ox + w, oy + h), nb); nb->Release(); }
    }
    if (ID2D1Bitmap* sp = withTint ? nullptr : SpecFor((int)std::ceil(w), (int)std::ceil(h), rad, px, ui::Dark()))  // brilho especular da borda (as peças: quem desenha por cima faz o liquid::Rim)
        dc->DrawBitmap(sp, D2D1::RectF(ox, oy, ox + std::ceil(w), oy + std::ceil(h)), 1, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
}

void Redraw(Entry& e) {
    st_redraws++;
    if (!active || !screen || !e.surf) return;
    if (CoveredByFullscreen(e.hwnd)) return;  // atrás do app em tela cheia: ninguém vê; redesenha quando ele sair
    // peça nova ou que se mexeu para fora da área montada: monta o fundo de novo (a montagem redesenha todas)
    if (!g_busy && IsWindowVisible(e.hwnd) && !Composed(e.hwnd)) { capDirty = true; allDirty = true; Poll(); return; }
    RECT r; GetWindowRect(e.hwnd, &r);
    e.lastRect = r;
    float px = GetDpiForWindow(e.hwnd) / 96.f;
    if (px <= 0) px = 1;
    auto si = e.surf.as<ABI::Windows::UI::Composition::ICompositionDrawingSurfaceInterop>();
    ID2D1DeviceContext* dc = nullptr;
    POINT off{};
    if (FAILED(si->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), (void**)&dc, &off)) || !dc) return;
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    float wx = (float)(r.left - mon.left), wy = (float)(r.top - mon.top);
    if (e.pieces.empty()) {
        DrawPiece(dc, (float)off.x, (float)off.y, wx, wy, e.w, e.h, e.radius, px, e.tintIn, false);
    } else {
        ID2D1Factory* f = nullptr; dc->GetFactory(&f);
        for (auto& p : e.pieces) {
            float pw = p.r.right - p.r.left, ph = p.r.bottom - p.r.top;
            ID2D1Geometry* g = nullptr;
            if (p.goo) {  // cápsula + círculos numa peça só de "líquido"
                std::vector<D2D1_ELLIPSE> cs = p.circ;
                for (auto& c : cs) { c.point.x += off.x; c.point.y += off.y; }
                g = vidro::Gosma(f, D2D1::RectF(off.x + p.cap.left, off.y + p.cap.top, off.x + p.cap.right, off.y + p.cap.bottom), p.capR, cs.data(), (int)cs.size(), p.broken);
            } else {
                ID2D1RoundedRectangleGeometry* rg = nullptr;
                f->CreateRoundedRectangleGeometry(D2D1::RoundedRect(D2D1::RectF(off.x + p.r.left, off.y + p.r.top, off.x + p.r.right, off.y + p.r.bottom), p.radius, p.radius), &rg);
                g = rg;
            }
            if (!g) continue;
            dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), g, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE), nullptr);
            DrawPiece(dc, off.x + p.r.left, off.y + p.r.top, wx + p.r.left, wy + p.r.top, pw, ph, p.radius, px, e.pieceTint, true, e.pieceFrost);
            dc->PopLayer();
            g->Release();
        }
        if (f) f->Release();
    }
    dc->Release();
    si->EndDraw();
}

// liga o vidro real numa peça (ou volta ao fosco): a superfície do tamanho da janela vira o fundo
void Use(Entry& e, bool want) {
    want = want && ok && active;
    if (want && e.w >= 1 && e.h >= 1) {
        int W = (int)std::ceil(e.w), H = (int)std::ceil(e.h);
        try {
            if (!e.surf) {
                e.surf = gfx.CreateDrawingSurface({ (float)W, (float)H }, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                                  winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
                e.surfBrush = g_comp.CreateSurfaceBrush(e.surf);
                e.surfBrush.Stretch(wuc::CompositionStretch::None);
                e.surfBrush.HorizontalAlignmentRatio(0);
                e.surfBrush.VerticalAlignmentRatio(0);
                e.surfW = W; e.surfH = H;
            } else if (W > e.surfW || H > e.surfH) {
                // só cresce: recriar a superfície a cada quadro de uma animação de tamanho (o Spotlight crescendo
                // ao digitar) travava a animação; maior que a peça não atrapalha (desenho sem esticar, no canto)
                int nw = std::max(W, e.surfW), nh = std::max(H, e.surfH);
                e.surf.as<ABI::Windows::UI::Composition::ICompositionDrawingSurfaceInterop>()->Resize({ nw, nh });
                e.surfW = nw; e.surfH = nh;
            }
            e.blur.Brush(e.surfBrush);
            e.real = true;
            Redraw(e);
            return;
        } catch (...) {}
    }
    // Saindo do vidro real (Tahoe -> Sequoia com o Shell aberto): o fundo continuava sendo a superfície do
    // vidro real, que para de ser redesenhada (o Dock ficava preto até reiniciar). Volta ao vidro normal.
    if (e.real && e.blur) {
        try { e.blur.Brush(backdrop::Liquid() && e.baseBrush ? e.baseBrush : e.hostBrush); } catch (...) {}
    }
    e.real = false;
}
}  // namespace rg

}  // namespace

const backdrop::Style& backdrop::Current() { return g_style; }
bool backdrop::Liquid() { return g_style.liquid && !g_style.reduce; }

void backdrop::Restart() {
    Log(L"vidro real: reiniciado pelo usuário (estava %ls%ls)", rg::active ? L"ligado" : L"desligado", rg::semCaptura ? L", desistido" : L"");
    rg::semCaptura = false;
    rg::capJaFuncionou = false;
    if (rg::active) rg::SetActive(false);  // fecha todas as capturas; o Reload abre de novo
    Reload();
}

void backdrop::Reload() {
    Style s;
    s.liquid = shell::Setting(L"liquid_glass", L"0") == L"1";
    s.tinted = shell::Setting(L"liquid_estilo", L"nitido") == L"tingido";
    s.golden = s.liquid && shell::Setting(L"golden_gate", L"0") == L"1";
    s.leopard = !s.liquid && shell::Setting(L"leopard", L"0") == L"1";
    s.refraction = (float)_wtof(shell::Setting(L"liquid_refracao", L"0.6").c_str());
    s.reduce = shell::Setting(L"reduzir_transparencia", L"0") == L"1";
    s.tintK = (float)_wtof(shell::Setting(L"vidro_tinta", L"0.5").c_str());
    s.refraction = std::clamp(s.refraction, 0.f, 1.f);
    s.tintK = std::clamp(s.tintK, 0.f, 1.f);
    s.real = s.liquid && !s.reduce && shell::Setting(L"vidro_real", L"1") == L"1" && !rg::semCaptura;  // aparece nas gravações (montado pelas janelas)
    // modo jogo (jogo.cpp): o vidro real remontava o fundo a cada quadro do jogo atrás das peças; fica o vidro comum
    if (jogo::Ativo()) s.real = false;
    g_style = s;
    if (g_comp) rg::SetActive(s.real);
    // menus (barra de menus, Dock, Central): mais opacos que os painéis, para ler bem
    ctxmenu::Glass mg;
    mg.liquid = s.liquid && !s.reduce;
    mg.golden = s.golden;
    mg.leopard = s.leopard;
    mg.alpha = [](float a) {
        const auto& st = g_style;
        if (st.tintK < 0.5f) a *= 0.25f + 1.5f * st.tintK;  // menus: no mínimo 25 % (texto legível); Golden Gate 60 %
        else a += (1 - a) * (st.tintK - 0.5f) * 1.4f;
        if (st.reduce) return std::max(a, 0.97f);
        if (st.liquid) a = st.tinted ? a + (1 - a) * 0.3f : a * 0.72f;
        return std::clamp(a, 0.f, 1.f);
    };
    ctxmenu::SetGlass(mg);
    for (auto& [h, e] : g_entries) {
        try { Apply(e); }
        catch (winrt::hresult_error const& x) { Log(L"vidro: erro ao reaplicar (0x%08X %ls)", (unsigned)x.code(), x.message().c_str()); }
        catch (...) { Log(L"vidro: erro ao reaplicar"); }
    }
    Log(L"vidro: liquid=%d estilo=%ls refração=%.2f reduzir=%d tinta=%.2f real=%d (%d janelas)", (int)s.liquid, s.tinted ? L"tingido" : L"nítido",
        s.refraction, (int)s.reduce, s.tintK, (int)s.real, (int)g_entries.size());
}

float backdrop::AdjustAlpha(float a) {
    const auto& st = g_style;
    // tinta do vidro: 0 = mais transparente, 0,5 = a de cada janela, 1 = bem mais opaco
    // 0 = totalmente transparente (17/11/2026, o usuário: "deixe o slider fazer ele ficar totalmente transparente");
    // antes o mínimo ainda deixava 35 % da tinta
    if (st.tintK < 0.5f) a *= 2 * st.tintK;
    else a += (1 - a) * (st.tintK - 0.5f) * 1.4f;
    // Reduzir transparência: no Tahoe cor sólida, nada do fundo (medido no macOS 26: a ≈ 0; guia, seção 60); no Sequoia
    // como era (94 %)
    if (st.reduce) return shell::Setting(L"liquid_glass", L"0") == L"1" ? 1.f : std::max(a, 0.94f);
    // Tahoe: as tintas de cada peça já são as medidas na tela (guia, seções 52 e 60) para o estilo Nítido com a
    // transparência no meio; Tingido fica 25 % mais fechado, como no 26.1
    if (st.liquid && st.tinted) a = a + (1 - a) * 0.25f;

    return std::clamp(a, 0.f, 1.f);
}

D2D1_COLOR_F backdrop::AdjustTint(D2D1_COLOR_F c) {
    c.a = AdjustAlpha(c.a);
    return c;
}

bool backdrop::Attach(HWND h) {
    try {
        if (!g_comp) {
            // O compositor precisa de uma DispatcherQueue nesta thread (a do laço de mensagens).
            DispatcherQueueOptions o{ sizeof(o), DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE };
            winrt::check_hresult(CreateDispatcherQueueController(o,
                reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(g_dq))));
            g_comp = wuc::Compositor();
        }
        BOOL on = TRUE;
        DwmSetWindowAttribute(h, 17 /*DWMWA_USE_HOSTBACKDROPBRUSH*/, &on, sizeof(on));
        Entry e;
        e.hwnd = h;
        auto interop = g_comp.as<abi::ICompositorDesktopInterop>();
        winrt::check_hresult(interop->CreateDesktopWindowTarget(h, FALSE,
            reinterpret_cast<abi::IDesktopWindowTarget**>(winrt::put_abi(e.target))));
        e.root = g_comp.CreateContainerVisual();
        e.root.RelativeSizeAdjustment({ 1, 1 });
        e.blur = g_comp.CreateSpriteVisual();
        e.blur.RelativeSizeAdjustment({ 1, 1 });
        e.hostBrush = g_comp.CreateHostBackdropBrush();
        e.blur.Brush(e.hostBrush);

        e.tint = g_comp.CreateSpriteVisual();
        e.tint.RelativeSizeAdjustment({ 1, 1 });
        e.tintBrush = g_comp.CreateColorBrush();
        e.tint.Brush(e.tintBrush);
        e.root.Children().InsertAtTop(e.blur);
        e.root.Children().InsertAtTop(e.tint);
        e.geo = g_comp.CreateRoundedRectangleGeometry();
        e.root.Clip(g_comp.CreateGeometricClip(e.geo));
        e.target.Root(e.root);
        g_entries[h] = e;
        if (g_style.real && !rg::active) rg::SetActive(true);  // o ajuste foi lido antes do primeiro vidro existir
        return true;
    } catch (...) {
        return false;
    }
}

namespace { wuc::CompositionGraphicsDevice g_contentGfx{ nullptr }; }

ID2D1DeviceContext* backdrop::ContentBegin(HWND h, int wpx, int hpx, float scale) {
    auto it = g_entries.find(h);
    if (it == g_entries.end() || !g_comp || wpx < 1 || hpx < 1) return nullptr;
    auto& e = it->second;
    try {
        if (!g_contentGfx) {  // do mesmo dispositivo D2D da interface: os ícones já carregados valem aqui
            ID2D1DeviceContext* ui0 = ui::Gpu();
            if (!ui0) return nullptr;
            ID2D1Device* dev = nullptr;
            ui0->GetDevice(&dev);
            if (!dev) return nullptr;
            auto ci = g_comp.as<ABI::Windows::UI::Composition::ICompositorInterop>();
            HRESULT hr = ci->CreateGraphicsDevice(dev, reinterpret_cast<ABI::Windows::UI::Composition::ICompositionGraphicsDevice**>(winrt::put_abi(g_contentGfx)));
            dev->Release();
            winrt::check_hresult(hr);
        }
        if (!e.content) {
            e.content = g_comp.CreateSpriteVisual();
            e.root.Children().InsertAtTop(e.content);
        }
        if (!e.contentSurf) {
            e.contentSurf = g_contentGfx.CreateDrawingSurface({ (float)wpx, (float)hpx }, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                                               winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
            auto b = g_comp.CreateSurfaceBrush(e.contentSurf);
            b.Stretch(wuc::CompositionStretch::None);
            b.HorizontalAlignmentRatio(0);
            b.VerticalAlignmentRatio(0);
            e.content.Brush(b);
            e.contentW = wpx; e.contentH = hpx;
        } else if (wpx > e.contentW || hpx > e.contentH) {  // só cresce (como o vidro real)
            e.contentW = std::max(wpx, e.contentW); e.contentH = std::max(hpx, e.contentH);
            e.contentSurf.as<ABI::Windows::UI::Composition::ICompositionDrawingSurfaceInterop>()->Resize({ e.contentW, e.contentH });
        }
        e.content.Size({ (float)e.contentW, (float)e.contentH });
        auto si = e.contentSurf.as<ABI::Windows::UI::Composition::ICompositionDrawingSurfaceInterop>();
        ID2D1DeviceContext* dc = nullptr;
        POINT off{};
        winrt::check_hresult(si->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), (void**)&dc, &off));
        dc->SetDpi(96 * scale, 96 * scale);
        dc->SetTransform(D2D1::Matrix3x2F::Translation(off.x / scale, off.y / scale));
        dc->Clear(D2D1::ColorF(0, 0, 0, 0));
        dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);  // fundo transparente: sem ClearType
        e.contentDc = dc;  // solto no ContentEnd, depois do EndDraw
        return dc;
    } catch (winrt::hresult_error const& ex) {
        static int n;
        if (n++ < 3) Log(L"vidro: conteúdo junto indisponível (0x%08X %ls)", (unsigned)ex.code(), ex.message().c_str());
    } catch (...) {}
    return nullptr;
}

void backdrop::ContentEnd(HWND h) {
    auto it = g_entries.find(h);
    if (it == g_entries.end() || !it->second.contentSurf) return;
    try { it->second.contentSurf.as<ABI::Windows::UI::Composition::ICompositionDrawingSurfaceInterop>()->EndDraw(); } catch (...) {}
    if (it->second.contentDc) { it->second.contentDc->Release(); it->second.contentDc = nullptr; }
}

void backdrop::Fade(HWND h, float opacity, float scale, float cx, float cy) {
    auto it = g_entries.find(h);
    if (it == g_entries.end()) return;
    auto& e = it->second;
    e.root.Opacity(std::clamp(opacity, 0.f, 1.f));
    e.root.CenterPoint({ cx, cy, 0 });
    e.root.Scale({ scale, scale, 1 });
}

void backdrop::Set(HWND h, float w, float hgt, float radius, D2D1_COLOR_F tint) {
    auto it = g_entries.find(h);
    if (it == g_entries.end()) return;
    auto& e = it->second;
    bool same = e.hasSet && e.w == w && e.h == hgt && e.radius == radius && !memcmp(&e.tintIn, &tint, sizeof(tint));
    if (same) {  // vidro real: a janela pode ter andado sem mudar de tamanho
        // vidro real: redesenha sempre com o fundo atual. Antes só se a janela tivesse andado, e o seletor do
        // Alt+Tab, que reabre no mesmo lugar, aparecia com o fundo da última vez (a janela antiga atrás)
        if (e.real) try { rg::Redraw(e); } catch (...) {}
        return;
    }
    e.pieces.clear();

    bool sizeOnly = e.hasSet && !memcmp(&e.tintIn, &tint, sizeof(tint)) && e.radius == radius;
    e.w = w; e.h = hgt; e.radius = radius; e.tintIn = tint; e.hasSet = true;
    try {
        if (sizeOnly && !(g_style.liquid && !g_style.reduce)) {  // caminho rápido (Dock ampliando, Spotlight animando)
            e.geo.Size({ w, hgt });
            return;
        }
        Apply(e);
    } catch (...) {}
}

#include "vidro_wuc.h"

winrt::Windows::UI::Composition::Compositor backdrop::Compositor() { return g_comp; }

winrt::Windows::UI::Composition::CompositionBrush backdrop::Frost() {
    if (!g_comp) return nullptr;
    return MakeFrost();
}

// O mesmo fosco + vibrância para quem tem o próprio compositor (ex.: os cartões da Central de Notificações).
winrt::Windows::UI::Composition::CompositionBrush backdrop::FrostFor(winrt::Windows::UI::Composition::Compositor const& c) {
    static std::map<void*, wuc::CompositionEffectFactory> fx;
    if (!c) return nullptr;
    try {
        auto it = fx.find(winrt::get_abi(c));
        if (it == fx.end())
            it = fx.emplace(winrt::get_abi(c), c.CreateEffectFactory(winrt::make<SatFx>(winrt::make<BlurFx>(wuc::CompositionEffectSourceParameter(L"fundo"))))).first;
        auto f = it->second;
        auto b = f.CreateBrush();
        b.SetSourceParameter(L"fundo", c.CreateHostBackdropBrush());
        return b;
    } catch (winrt::hresult_error const& e) {
        Log(L"liquid glass: fosco indisponível neste compositor (0x%08X %ls)", (unsigned)e.code(), e.message().c_str());
    } catch (...) {}
    return c.CreateHostBackdropBrush();
}

bool backdrop::Real() { return g_style.real && rg::active; }

void backdrop::Detach(HWND h) {
    auto it = g_entries.find(h);
    if (it == g_entries.end()) return;
    try { it->second.target = nullptr; } catch (...) {}
    g_entries.erase(it);
}

void backdrop::AfterCommit(HWND to, UINT msg) {
    try {
        if (g_comp) {
            g_comp.RequestCommitAsync().Completed([to, msg](auto&&, auto&&) { PostMessageW(to, msg, 0, 0); });
            return;
        }
    } catch (...) {}
    PostMessageW(to, msg, 0, 0);
}

// a Captura de Tela vai tirar a foto: as peças do vidro real aparecem (true) ou voltam a sumir (false)
void backdrop::CaptureVisible(bool show) {
    rg::showInCapture = show;
    rg::ApplyAffinity();
    DwmFlush();
}

void backdrop::SetPieces(HWND h, float w, float hgt, const std::vector<Piece>& pieces, D2D1_COLOR_F tint, float minFrost) {
    auto it = g_entries.find(h);
    if (it == g_entries.end()) return;
    auto& e = it->second;
    bool same = e.hasSet && e.w == w && e.h == hgt && e.pieces.size() == pieces.size() && !memcmp(&e.pieceTint, &tint, sizeof(tint)) &&
                std::equal(pieces.begin(), pieces.end(), e.pieces.begin(), [](const Piece& a, const Piece& b) {
                    return !memcmp(&a.r, &b.r, sizeof(a.r)) && a.radius == b.radius && a.goo == b.goo && !memcmp(&a.cap, &b.cap, sizeof(a.cap)) &&
                           a.capR == b.capR && a.broken == b.broken && a.circ.size() == b.circ.size() &&
                           (a.circ.empty() || !memcmp(a.circ.data(), b.circ.data(), a.circ.size() * sizeof(D2D1_ELLIPSE)));
                });
    if (same) {
        // vidro real: redesenha sempre com o fundo atual. Antes só se a janela tivesse andado, e o seletor do
        // Alt+Tab, que reabre no mesmo lugar, aparecia com o fundo da última vez (a janela antiga atrás)
        if (e.real) try { rg::Redraw(e); } catch (...) {}
        return;
    }
    e.w = w; e.h = hgt; e.radius = 0; e.tintIn = D2D1::ColorF(0, 0, 0, 0); e.hasSet = true;
    e.pieces = pieces;
    e.pieceTint = tint;
    e.pieceFrost = minFrost;
    try { Apply(e); } catch (...) {}
}


// o dispositivo D3D deste arquivo, para a memória descartável (memoria.cpp: Trim na pressão de memória)
ID3D11Device* BackdropD3D() { return rg::d3d; }
