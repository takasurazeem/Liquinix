// Central de Notificações do macOS Sequoia: clique na data/hora da barra de menus e a coluna desliza da
// direita com as notificações do Windows (UserNotificationListener) e os widgets embaixo.
//  - Cartões de vidro (desfoque do papel de parede, Windows.UI.Composition) com cantos de 17 pt, ícone do app
//    à esquerda, título em negrito, até 3 linhas de texto e a hora à direita ("6 min atrás", "9:01", "Ontem, 14:25").
//  - Notificações do mesmo app ficam empilhadas (cartões espiando embaixo); clique abre o grupo com o nome do
//    app grande, "Mostrar menos" e o ✕. Passar o mouse mostra o ✕ no canto e o botão "Opções".
//  - "Limpar" embaixo da lista, "N notificações a mais" quando ela é longa, widgets e "Editar Widgets…".
// Medidas das capturas do Sequoia (cartão de 344 pt, 55 pt de altura mínima, linhas de 16 pt, pilha de 7 pt).
// Duas janelas, como as outras do Shell: o fundo (vidro de cada cartão, recortado pela composição) e o
// conteúdo por cima (Direct2D na GPU). O Windows não avisa quando chega notificação para app sem pacote
// (NotificationChanged dá "Elemento não encontrado"), então uma thread relê a lista a cada 2 s com o painel aberto.
#include "shell.h"
#include "vidro_wuc.h"
#include "../menu/context_menu.h"
#include "../comum/simbolos.h"
#include <dwmapi.h>
#include <wincodec.h>
#include <shcore.h>
#include <shlobj.h>
#include <ctime>
#include <DispatcherQueue.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <winrt/Windows.UI.Notifications.h>
#include <winrt/Windows.UI.Notifications.Management.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Storage.Streams.h>
#include <windows.ui.composition.interop.h>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include "../comum/idioma.h"

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "CoreMessaging.lib")

namespace wuc = winrt::Windows::UI::Composition;
namespace wun = winrt::Windows::UI::Notifications;
namespace abi = ABI::Windows::UI::Composition::Desktop;

namespace {

// ---- medidas (pt)
const float CARD_W = 344, PAD_L = 22, MARGIN_R = 16, TOP_SEQ = 8;
// Tahoe (guia macOS26-Theme-Reference, seção 65, medido): o banner fica 16 abaixo da barra de menus e a 16 da
// borda direita, 344 de largura, cantos ~22; alturas de 57 (título + 1 linha), 73 e 105 (12 em cima, 13 embaixo,
// título e linhas de 16)
#define TOP (liquid::On() ? 16.f : TOP_SEQ)
const float COL_W = PAD_L + CARD_W + MARGIN_R;
const float CARD_R_SEQ = 17, ICON = 34, ICON_X = 13, TEXT_X = ICON_X + ICON + 10, PAD_V = 11, PAD_R = 14;
const float TITLE_H = 17, LINE_H = 16, MIN_H = 55, MAX_LINES = 3;
const float GAP = 8, GROUP_GAP = 10, PEEK = 7, PEEK_INSET = 10;
const float HEADER_H = 26, PILL_H = 24, CIRCLE = 24;
const float WIDGET_S = 164, WIDGET_GAP = 16, WIDGET_R_SEQ = 20;
// Tahoe (Tema nos Ajustes): cartões e widgets bem mais redondos, com o contorno especular do Liquid Glass
inline float CardR() { return liquid::Golden() ? 18.f : liquid::On() ? 22.f : CARD_R_SEQ; }  // macOS 27: ~18 (captura 2x)
inline float WidgetR() { return liquid::On() ? 26.f : WIDGET_R_SEQ; }
const int MAX_GROUPS = 6;  // grupos mostrados antes de "N notificações a mais"

const UINT WM_NC_FRAME = WM_APP + 90, WM_NC_DATA = WM_APP + 91, WM_NC_NEW = WM_APP + 92, WM_NC_BFRAME = WM_APP + 93;
const UINT_PTR T_TICK = 1, T_FOCO = 2, T_ANIM = 3;  // T_FOCO: confere quem ficou na frente depois de perder o foco "para ninguém"

HWND g_wnd, g_back;
bool IsOurBanner(HWND h) { wchar_t c[64] = {}; GetClassNameW(h, c, 64); return wcsstr(c, L"Notifica") != nullptr || wcsstr(c, L"Banner") != nullptr; }
ui::GpuSurface g_s;
bool g_open, g_closing, g_menu;
double g_slideAt;      // ms (anim::Now) do começo da animação de entrar/sair
float g_slide = 1;     // 0 = no lugar, 1 = fora da tela à direita
DWORD g_closedAt;
float g_scroll, g_scrollTarget, g_contentH;
double g_lastFrame;
DWORD g_clickAt = 0;  // último clique dentro da Central: o foco que some logo depois não fecha (abrir a pilha)
bool g_snap;           // próximo desenho sem animar os cartões (acabou de abrir)

IDWriteTextFormat *g_tfTitle, *g_tfBody, *g_tfTime, *g_tfPill, *g_tfHeader, *g_tfEmpty;

// ================= dados (thread de leitura)
struct Note {
    UINT id = 0;
    std::wstring aumid, app, title, body;
    long long time = 0;  // FILETIME (100 ns, UTC)
};
struct LogoPx { UINT w = 0, h = 0; std::vector<BYTE> px; };

std::mutex g_mx;
std::condition_variable g_cv;
bool g_wake;
std::vector<Note> g_data;           // protegido por g_mx
std::map<std::wstring, LogoPx> g_logos;  // logo do AppInfo por AUMID (fallback do ícone)
std::vector<UINT> g_toRemove;
int g_access = -1;                  // -1 lendo, 1 permitido, 0 negado
bool Dark() { return ui::Dark(); }

bool DecodeStream(const winrt::Windows::Storage::Streams::IRandomAccessStream& stream, LogoPx& out) {
    IStream* is = nullptr;
    if (FAILED(CreateStreamOverRandomAccessStream(winrt::get_unknown(stream), IID_PPV_ARGS(&is)))) return false;
    IWICImagingFactory* f = nullptr;
    bool ok = false;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) {
        IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* cv = nullptr;
        if (SUCCEEDED(f->CreateDecoderFromStream(is, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr)) &&
            SUCCEEDED(f->CreateFormatConverter(&cv)) &&
            SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
            cv->GetSize(&out.w, &out.h);
            out.px.resize((size_t)out.w * out.h * 4);
            ok = SUCCEEDED(cv->CopyPixels(nullptr, out.w * 4, (UINT)out.px.size(), out.px.data()));
        }
        if (cv) cv->Release(); if (fr) fr->Release(); if (dec) dec->Release();
        f->Release();
    }
    is->Release();
    return ok;
}

void Worker() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    wun::Management::UserNotificationListener l{ nullptr };
    try { l = wun::Management::UserNotificationListener::Current(); } catch (...) {}
    std::wstring lastSig;
    std::set<UINT> seen;  // ids já vistos: o que aparecer depois vira banner
    bool first = true;
    // notificações já lidas, por id: ler o texto (XML) de todas a cada segundo era o grosso do gasto do Shell
    // parado; agora só as novas são lidas
    std::map<UINT, Note> parsed;
    for (;;) {
        std::vector<UINT> rm;
        { std::lock_guard<std::mutex> k(g_mx); rm.swap(g_toRemove); }
        for (UINT id : rm) try { if (l) l.RemoveNotification(id); } catch (...) {}
        int access = 0;
        std::vector<Note> notes;
        std::map<std::wstring, LogoPx> logos;
        if (l) {
            try {
                auto st = l.GetAccessStatus();
                if (st == wun::Management::UserNotificationListenerAccessStatus::Unspecified) st = l.RequestAccessAsync().get();
                access = st == wun::Management::UserNotificationListenerAccessStatus::Allowed;
            } catch (...) {}
        }
        if (access) {
            try {
                auto list = l.GetNotificationsAsync(wun::NotificationKinds::Toast).get();
                std::map<UINT, Note> still;
                for (auto const& n : list) {
                    UINT nid = n.Id();
                    if (auto pit = parsed.find(nid); pit != parsed.end()) {  // já lida: só reaproveita
                        notes.push_back(pit->second);
                        still[nid] = std::move(pit->second);
                        continue;
                    }
                    Note x;
                    x.id = nid;
                    try { x.time = winrt::clock::to_file_time(n.CreationTime()).value; } catch (...) {}
                    try {
                        auto ai = n.AppInfo();
                        x.aumid = ai.AppUserModelId().c_str();
                        x.app = ai.DisplayInfo().DisplayName().c_str();
                    } catch (...) {}
                    if (x.aumid.empty()) x.aumid = L"?";
                    if (x.app.empty()) x.app = x.aumid;
                    try {
                        auto b = n.Notification().Visual().GetBinding(wun::KnownNotificationBindings::ToastGeneric());
                        if (b) {
                            int i = 0;
                            for (auto const& e : b.GetTextElements()) {
                                std::wstring t = e.Text().c_str();
                                if (t.empty()) continue;
                                if (i++ == 0) x.title = t;
                                else { if (!x.body.empty()) x.body += L"\n"; x.body += t; }
                            }
                        }
                    } catch (...) {}
                    if (x.title.empty()) x.title = x.app;
                    if (x.body.size() > 600) x.body.resize(600);
                    bool known;
                    { std::lock_guard<std::mutex> k(g_mx); known = g_logos.count(x.aumid) > 0; }
                    if (!known && !logos.count(x.aumid)) {
                        LogoPx lp;
                        try {
                            auto ref = n.AppInfo().DisplayInfo().GetLogo(winrt::Windows::Foundation::Size(64, 64));
                            if (ref) DecodeStream(ref.OpenReadAsync().get(), lp);
                        } catch (...) {}
                        logos[x.aumid] = std::move(lp);
                    }
                    still[x.id] = x;
                    notes.push_back(std::move(x));
                }
                parsed.swap(still);
            } catch (...) {}
        }
        if (access) {
            const Note* fresh = nullptr;
            for (auto& n : notes) if (!first && !seen.count(n.id) && (!fresh || n.time > fresh->time)) fresh = &n;
            UINT freshId = fresh ? fresh->id : 0;
            seen.clear();
            for (auto& n : notes) seen.insert(n.id);
            first = false;
            if (freshId) {
                { std::lock_guard<std::mutex> k(g_mx); g_data = notes; g_access = access; for (auto& [a, p] : logos) g_logos[a] = p; }
                if (g_wnd) PostMessageW(g_wnd, WM_NC_NEW, 0, (LPARAM)freshId);
            }
        }
        std::wstring sig = std::to_wstring(access) + L":";
        for (auto& n : notes) sig += std::to_wstring(n.id) + L",";
        bool changed = sig != lastSig || !logos.empty();
        lastSig = sig;
        if (changed) {
            { std::lock_guard<std::mutex> k(g_mx); g_data = std::move(notes); g_access = access; for (auto& [a, p] : logos) g_logos[a] = std::move(p); }
            if (g_wnd) PostMessageW(g_wnd, WM_NC_DATA, 0, 0);
        }
        std::unique_lock<std::mutex> k(g_mx);
        g_cv.wait_for(k, std::chrono::milliseconds(1000), [] { return g_wake; });
        g_wake = false;
    }
}

void Wake() { { std::lock_guard<std::mutex> k(g_mx); g_wake = true; } g_cv.notify_all(); }

// ================= estado da tela
struct Group { std::wstring aumid, app; std::vector<Note> notes; };  // notas da mais nova para a mais velha
std::vector<Group> g_groups;
std::set<std::wstring> g_expanded;
bool g_showAll;
int g_accessUi = -1;
std::map<std::wstring, ID2D1Bitmap*> g_icons;  // por AUMID (nullptr = sem ícone)
std::map<UINT, std::pair<IDWriteTextLayout*, int>> g_bodies;  // texto já quebrado em linhas, por id

std::vector<notificacoes::Widget> g_widgets;  // widgets.cpp pode trocar (SetWidgets)
std::function<void()> g_editWidgets;
bool g_customWidgets;

void Rebuild() {
    std::vector<Note> data;
    { std::lock_guard<std::mutex> k(g_mx); data = g_data; g_accessUi = g_access; }
    std::sort(data.begin(), data.end(), [](const Note& a, const Note& b) { return a.time > b.time; });
    g_groups.clear();
    for (auto& n : data) {
        auto it = std::find_if(g_groups.begin(), g_groups.end(), [&](const Group& g) { return g.aumid == n.aumid; });
        if (it == g_groups.end()) { g_groups.push_back({ n.aumid, n.app, {} }); it = g_groups.end() - 1; }
        it->notes.push_back(n);
    }
    // grupo expandido que ficou com uma só notificação volta a ser um cartão comum
    for (auto it = g_expanded.begin(); it != g_expanded.end();) {
        auto g = std::find_if(g_groups.begin(), g_groups.end(), [&](const Group& x) { return x.aumid == *it; });
        if (g == g_groups.end() || g->notes.size() < 2) it = g_expanded.erase(it); else ++it;
    }
    std::set<UINT> live;
    for (auto& n : data) live.insert(n.id);
    for (auto it = g_bodies.begin(); it != g_bodies.end();) {
        if (!live.count(it->first)) { if (it->second.first) it->second.first->Release(); it = g_bodies.erase(it); } else ++it;
    }
}

void RemoveIds(const std::vector<UINT>& ids) {
    {
        std::lock_guard<std::mutex> k(g_mx);
        for (UINT id : ids) {
            g_toRemove.push_back(id);
            g_data.erase(std::remove_if(g_data.begin(), g_data.end(), [&](const Note& n) { return n.id == id; }), g_data.end());
        }
    }
    Wake();
    Rebuild();
}

// ================= vidro: um recorte arredondado de desfoque por cartão (Windows.UI.Composition)
struct Glass {
    wuc::ContainerVisual outer{ nullptr }, body{ nullptr };
    wuc::CompositionRoundedRectangleGeometry geo{ nullptr };
    wuc::CompositionColorBrush tint{ nullptr };
    wuc::InsetClip cut{ nullptr };
    wuc::SpriteVisual blur{ nullptr };
};
winrt::Windows::System::DispatcherQueueController g_dq{ nullptr };
wuc::Compositor g_comp{ nullptr };
wuc::CompositionBrush g_blur{ nullptr }, g_frost{ nullptr };  // fundo puro (Sequoia) e o fosco do Tahoe
struct GlassHost {  // uma janela de fundo: o painel e o banner têm cada um a sua
    wuc::Desktop::DesktopWindowTarget target{ nullptr };
    wuc::ContainerVisual root{ nullptr };
    std::vector<Glass> pool;
    size_t used = 0;
};
GlassHost g_hostPanel, g_hostBanner, *g_host = &g_hostPanel;
bool g_hasGlass;

bool GlassInit(HWND h, GlassHost& host) {
    try {
        if (!g_comp) {
            if (!winrt::Windows::System::DispatcherQueue::GetForCurrentThread()) {
                DispatcherQueueOptions o{ sizeof(o), DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE };
                winrt::check_hresult(CreateDispatcherQueueController(o,
                    reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(g_dq))));
            }
            g_comp = wuc::Compositor();
            g_blur = g_comp.CreateHostBackdropBrush();
        }
        BOOL on = TRUE;
        DwmSetWindowAttribute(h, 17 /*DWMWA_USE_HOSTBACKDROPBRUSH*/, &on, sizeof(on));
        auto interop = g_comp.as<abi::ICompositorDesktopInterop>();
        winrt::check_hresult(interop->CreateDesktopWindowTarget(h, FALSE,
            reinterpret_cast<abi::IDesktopWindowTarget**>(winrt::put_abi(host.target))));
        host.root = g_comp.CreateContainerVisual();
        host.root.RelativeSizeAdjustment({ 1, 1 });
        host.target.Root(host.root);
        return true;
    } catch (...) {
        return false;
    }
}

// um retângulo de vidro (px); cutTop: só aparece abaixo dessa linha (camadas da pilha)
void GlassRect(D2D1_RECT_F r, float radius, D2D1_COLOR_F tint, float alpha, float cutTop = -1e9f) {
    if (!g_hasGlass) return;
    if (!g_host->root) return;
    if (g_host->used == g_host->pool.size()) {
        Glass g;
        g.outer = g_comp.CreateContainerVisual();
        g.outer.RelativeSizeAdjustment({ 1, 1 });
        g.cut = g_comp.CreateInsetClip();
        g.outer.Clip(g.cut);
        g.body = g_comp.CreateContainerVisual();
        auto blur = g_comp.CreateSpriteVisual();
        blur.RelativeSizeAdjustment({ 1, 1 });
        blur.Brush(g_blur);
        g.blur = blur;
        auto tv = g_comp.CreateSpriteVisual();
        tv.RelativeSizeAdjustment({ 1, 1 });
        g.tint = g_comp.CreateColorBrush();
        tv.Brush(g.tint);
        g.body.Children().InsertAtTop(blur);
        g.body.Children().InsertAtTop(tv);
        g.geo = g_comp.CreateRoundedRectangleGeometry();
        g.body.Clip(g_comp.CreateGeometricClip(g.geo));
        g.outer.Children().InsertAtTop(g.body);
        g_host->root.Children().InsertAtTop(g.outer);
        g_host->pool.push_back(g);
    }
    Glass& g = g_host->pool[g_host->used++];
    g.outer.IsVisible(true);
    if (liquid::On() && !g_frost) g_frost = backdrop::FrostFor(g_comp);  // Tahoe: o mesmo fosco + vibrância do Dock
    if (g.blur) g.blur.Brush(liquid::On() && g_frost ? g_frost : g_blur);
    g.outer.Opacity(std::clamp(alpha, 0.f, 1.f));
    g.cut.TopInset(std::max(0.f, cutTop));
    g.body.Offset({ r.left, r.top, 0 });
    g.body.Size({ r.right - r.left, r.bottom - r.top });
    g.geo.Size({ r.right - r.left, r.bottom - r.top });
    g.geo.CornerRadius({ radius, radius });
    auto c8 = [](float v) { return (uint8_t)std::lround(std::clamp(v, 0.f, 1.f) * 255); };
    g.tint.Color(winrt::Windows::UI::Color{ c8(tint.a), c8(tint.r), c8(tint.g), c8(tint.b) });
}

void GlassEnd() {
    if (!g_hasGlass) return;
    for (size_t i = g_host->used; i < g_host->pool.size(); i++) g_host->pool[i].outer.IsVisible(false);
}

// ================= cores
struct Pal { D2D1_COLOR_F tint, tintPill, text, text2, line, close, closeGlyph, chip, header; };
Pal Colors() {
    // Leopard: cartão claro do Aqua (o usuário preferiu claro ao "Smoke" escuro do Growl): branco perolado, texto preto
    if (liquid::Leopard())
        return { D2D1::ColorF(0.97f, 0.97f, 0.97f, 0.95f), D2D1::ColorF(0.95f, 0.95f, 0.95f, 0.95f), D2D1::ColorF(0, 0, 0, 0.90f),
                 D2D1::ColorF(0, 0, 0, 0.60f), D2D1::ColorF(0, 0, 0, 0.25f), D2D1::ColorF(0.93f, 0.93f, 0.93f, 1),
                 D2D1::ColorF(0, 0, 0, 0.70f), D2D1::ColorF(0, 0, 0, 0.08f), D2D1::ColorF(1, 1, 1, 0.97f) };
    if (liquid::On()) {  // Tahoe: vidro de verdade, o fundo desfocado aparece (antes a tinta deixava o cartão quase opaco)
        Pal p = Dark()
            ? Pal{ D2D1::ColorF(0.10f, 0.10f, 0.11f, 0.40f), D2D1::ColorF(0.16f, 0.16f, 0.17f, 0.36f), D2D1::ColorF(1, 1, 1, 0.95f),
                   D2D1::ColorF(1, 1, 1, 0.62f), D2D1::ColorF(1, 1, 1, 0.10f), D2D1::ColorF(0.24f, 0.24f, 0.25f, 0.98f),
                   D2D1::ColorF(1, 1, 1, 0.85f), D2D1::ColorF(1, 1, 1, 0.14f), D2D1::ColorF(1, 1, 1, 0.97f) }
            : Pal{ D2D1::ColorF(1, 1, 1, 0.50f), D2D1::ColorF(1, 1, 1, 0.42f), D2D1::ColorF(0, 0, 0, 0.88f),
                   D2D1::ColorF(0, 0, 0, 0.55f), D2D1::ColorF(1, 1, 1, 0.45f), D2D1::ColorF(0.98f, 0.98f, 0.98f, 0.98f),
                   D2D1::ColorF(0, 0, 0, 0.62f), D2D1::ColorF(0, 0, 0, 0.07f), D2D1::ColorF(1, 1, 1, 0.97f) };
        return p;
    }
    if (Dark())
        return { D2D1::ColorF(0.13f, 0.13f, 0.14f, 0.68f), D2D1::ColorF(0.20f, 0.20f, 0.21f, 0.62f), D2D1::ColorF(1, 1, 1, 0.92f),
                 D2D1::ColorF(1, 1, 1, 0.55f), D2D1::ColorF(1, 1, 1, 0.10f), D2D1::ColorF(0.24f, 0.24f, 0.25f, 0.98f),
                 D2D1::ColorF(1, 1, 1, 0.85f), D2D1::ColorF(1, 1, 1, 0.12f), D2D1::ColorF(1, 1, 1, 0.95f) };
    return { D2D1::ColorF(0.97f, 0.97f, 0.98f, 0.72f), D2D1::ColorF(0.97f, 0.97f, 0.98f, 0.62f), D2D1::ColorF(0, 0, 0, 0.86f),
             D2D1::ColorF(0, 0, 0, 0.50f), D2D1::ColorF(1, 1, 1, 0.45f), D2D1::ColorF(0.98f, 0.98f, 0.98f, 0.98f),
             D2D1::ColorF(0, 0, 0, 0.62f), D2D1::ColorF(0, 0, 0, 0.07f), D2D1::ColorF(1, 1, 1, 0.97f) };
}

// ================= textos
std::wstring Hm(const SYSTEMTIME& t) { wchar_t b[16]; swprintf_s(b, L"%d:%02d", t.wHour, t.wMinute); return b; }

std::wstring When(long long ft) {
    if (!ft) return L"";
    FILETIME now; GetSystemTimeAsFileTime(&now);
    long long n = ((long long)now.dwHighDateTime << 32) | now.dwLowDateTime;
    long long mins = (n - ft) / 600000000LL;
    if (mins < 1) return Tr(L"agora");
    if (mins < 60) return std::to_wstring(mins) + Tr(L" min atrás");
    FILETIME f{ (DWORD)ft, (DWORD)(ft >> 32) }, lf, ln;
    SYSTEMTIME t, tn;
    FileTimeToLocalFileTime(&f, &lf); FileTimeToSystemTime(&lf, &t);
    FileTimeToLocalFileTime(&now, &ln); FileTimeToSystemTime(&ln, &tn);
    auto day = [](const SYSTEMTIME& s) { FILETIME x; SYSTEMTIME d = s; d.wHour = d.wMinute = d.wSecond = d.wMilliseconds = 0; SystemTimeToFileTime(&d, &x);
                                         return (((long long)x.dwHighDateTime << 32) | x.dwLowDateTime) / 864000000000LL; };
    long long days = day(tn) - day(t);
    if (days <= 0) return Hm(t);
    if (days == 1) return Tr(L"Ontem, ") + Hm(t);
    if (days < 7) return std::to_wstring(days) + Tr(L" dias atrás");
    wchar_t b[16]; swprintf_s(b, L"%02d/%02d", t.wDay, t.wMonth); return b;
}

IDWriteTextFormat* Fmt(float size, DWRITE_FONT_WEIGHT w, const wchar_t* family = nullptr, bool wrap = false) {
    IDWriteTextFormat* tf = nullptr;
    if (family) ui::dw->CreateTextFormat(family, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"pt-br", &tf);
    if (!tf) tf = ui::Format(size, w);
    if (!tf) return nullptr;
    tf->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    IDWriteInlineObject* ell = nullptr;
    ui::dw->CreateEllipsisTrimmingSign(tf, &ell);
    DWRITE_TRIMMING tr{ wrap ? DWRITE_TRIMMING_GRANULARITY_WORD : DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
    tf->SetTrimming(&tr, ell);
    if (ell) ell->Release();
    return tf;
}

void Text(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const std::wstring& s, IDWriteTextFormat* tf, D2D1_RECT_F r, D2D1_COLOR_F c,
          DWRITE_TEXT_ALIGNMENT al = DWRITE_TEXT_ALIGNMENT_LEADING, bool shadow = false) {
    if (!tf || s.empty()) return;
    tf->SetTextAlignment(al);
    tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (shadow) {  // texto branco sobre o papel de parede (nome do app no grupo aberto)
        for (auto [dx, dy, a] : { std::tuple{ 0.f, 1.f, 0.22f }, std::tuple{ 0.f, 0.f, 0.10f } }) {
            br->SetColor(D2D1::ColorF(0, 0, 0, a * c.a));
            dc->DrawTextW(s.c_str(), (UINT32)s.size(), tf, D2D1::RectF(r.left + dx, r.top + dy, r.right + dx, r.bottom + dy), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
    br->SetColor(c);
    dc->DrawTextW(s.c_str(), (UINT32)s.size(), tf, r, br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float TextW(IDWriteTextFormat* tf, const std::wstring& s) { return tf ? ui::TextWidth(tf, s) : 0; }

// corpo da notificação quebrado em até 3 linhas
std::pair<IDWriteTextLayout*, int>& Body(const Note& n) {
    auto it = g_bodies.find(n.id);
    if (it != g_bodies.end()) return it->second;
    auto& e = g_bodies[n.id];
    e = { nullptr, 0 };
    if (n.body.empty() || !g_tfBody) return e;
    float w = CARD_W - TEXT_X - PAD_R;
    IDWriteTextLayout* lay = nullptr;
    if (FAILED(ui::dw->CreateTextLayout(n.body.c_str(), (UINT32)n.body.size(), g_tfBody, w, 10000, &lay))) return e;
    DWRITE_TEXT_METRICS m{};
    lay->GetMetrics(&m);
    int lines = std::clamp((int)m.lineCount, 1, (int)MAX_LINES);
    lay->SetMaxHeight(lines * LINE_H);
    e = { lay, lines };
    return e;
}

float CardH(const Note& n) {
    auto& b = Body(n);
    if (liquid::On()) return std::max(57.f, 12.f + 16.f + (float)b.second * 16.f + 13.f);
    return std::max(MIN_H, PAD_V + TITLE_H + b.second * LINE_H + PAD_V);
}

// ================= layout com animação: cada peça tem uma chave e anda com mola até o lugar novo
enum Kind { K_CARD, K_UNDER, K_HEADER, K_PILL, K_CIRCLE, K_WIDGET, K_LABEL, K_EMPTY };
enum Act { A_NONE, A_CARD, A_STACK, A_SHOW_LESS, A_CLEAR_GROUP, A_CLEAR_ALL, A_MORE, A_EDIT, A_SETTINGS, A_PERMISSION, A_WIDGET };
struct El {
    std::wstring key;
    Kind kind;
    D2D1_RECT_F r;
    Act act = A_NONE;
    int group = -1, note = -1, layer = 0, widget = -1;
    std::wstring text;
    D2D1_RECT_F from{};  // de onde aparece (nova); vazio = no próprio lugar
    bool hasFrom = false;
    float cutTop = -1e9f;  // camada da pilha: só abaixo desta linha
};
struct Spring1 { float x = 0, v = 0; };
struct Anim { El el; Spring1 l, t, r, b, a; bool live = true; std::wstring anchor;
    bool emerge = false; float fromTop = 0;  // cartão que sai de baixo do de cima ao abrir o grupo
};
std::map<std::wstring, Anim> g_anim;
std::vector<std::wstring> g_order;  // ordem de desenho do último layout

std::vector<El> Layout() {
    std::vector<El> out;
    float y = TOP;
    if (g_accessUi == 0) {
        El e{ L"empty", K_EMPTY, D2D1::RectF(0, y, CARD_W, y + 118) };
        out.push_back(e);
        y += 118 + GROUP_GAP;
        El p{ L"perm", K_PILL, {}, A_PERMISSION };
        p.text = Tr(L"Permitir Acesso às Notificações…");
        float w = TextW(g_tfPill, p.text) + 28;
        p.r = D2D1::RectF((CARD_W - w) / 2, y - 48, (CARD_W + w) / 2, y - 48 + PILL_H);
        out.push_back(p);
    } else if (g_accessUi == 1 && g_groups.empty()) {
        El e{ L"none", K_LABEL, D2D1::RectF(0, y, CARD_W, y + 30) };
        e.text = Tr(L"Sem Notificações");
        out.push_back(e);
        y += 30 + GROUP_GAP;
    }
    if (liquid::Golden() && !g_groups.empty()) {
        // macOS 27 (captura Notification-Center): título "Central de Notificações" em negrito de 17 e, à direita, o × num
        // círculo cinza que limpa tudo (no lugar do "Limpar" de baixo)
        El h{ L"h:central", K_HEADER, D2D1::RectF(0, y, CARD_W, y + 30) };
        h.text = Tr(L"Central de Notificações"); h.layer = 7;
        out.push_back(h);
        El x{ L"hx:central", K_CIRCLE, D2D1::RectF(CARD_W - 20, y + 5, CARD_W, y + 25), A_CLEAR_ALL };
        out.push_back(x);
        y += 30 + 6;
    }
    int shown = 0, hiddenNotes = 0;
    for (int gi = 0; gi < (int)g_groups.size(); gi++) {
        auto& g = g_groups[gi];
        if (!g_showAll && shown >= MAX_GROUPS) { hiddenNotes += (int)g.notes.size(); continue; }
        shown++;
        bool expanded = g.notes.size() > 1 && g_expanded.count(g.aumid);
        std::wstring topKey = L"n:" + std::to_wstring(g.notes[0].id);
        if (expanded) {
            El h{ L"h:" + g.aumid, K_HEADER, D2D1::RectF(0, y, CARD_W, y + HEADER_H) };
            h.text = g.app; h.group = gi;
            out.push_back(h);
            El x{ L"hx:" + g.aumid, K_CIRCLE, D2D1::RectF(CARD_W - CIRCLE, y + (HEADER_H - CIRCLE) / 2, CARD_W, y + (HEADER_H + CIRCLE) / 2), A_CLEAR_GROUP };
            x.group = gi;
            out.push_back(x);
            El p{ L"hp:" + g.aumid, K_PILL, {}, A_SHOW_LESS };
            p.text = Tr(L"Mostrar menos"); p.group = gi;
            float w = TextW(g_tfPill, p.text) + 26;
            p.r = D2D1::RectF(x.r.left - 8 - w, y + (HEADER_H - PILL_H) / 2, x.r.left - 8, y + (HEADER_H + PILL_H) / 2);
            out.push_back(p);
            y += HEADER_H + GAP;
            D2D1_RECT_F first{};
            std::vector<El> cards;
            // Pilha aberta com centenas de notificações (o Chrome guarda todas): cada quadro da animação refazia o
            // layout e a mola de todas e a Central travava ~3 s; o clique seguinte caía no Chrome e ela fechava
            // (19/11/2026). Mostra as mais novas; o resto continua no grupo (Limpar limpa tudo).
            const int MAX_OPEN = 20;
            int nOpen = std::min((int)g.notes.size(), MAX_OPEN);
            for (int ni = 0; ni < nOpen; ni++) {
                float h2 = CardH(g.notes[ni]);
                El c{ L"n:" + std::to_wstring(g.notes[ni].id), K_CARD, D2D1::RectF(0, y, CARD_W, y + h2), A_CARD };
                c.group = gi; c.note = ni;
                if (ni == 0) first = c.r;
                else { c.from = D2D1::RectF(first.left, first.top, first.right, first.top + h2); c.hasFrom = true; }
                cards.push_back(c);
                y += h2 + GAP;
            }
            // as de baixo são desenhadas antes: ao abrir saem de trás do cartão de cima (como no Mac), sem texto
            // de uma por cima do da outra
            out.insert(out.end(), cards.rbegin(), cards.rend());
            if ((int)g.notes.size() > nOpen) {
                El more{ L"hm:" + g.aumid, K_LABEL, D2D1::RectF(0, y, CARD_W, y + 22) };
                int rest = (int)g.notes.size() - nOpen;
                more.text = std::to_wstring(rest) + (rest == 1 ? Tr(L" notificação mais antiga") : Tr(L" notificações mais antigas"));
                out.push_back(more);
                y += 22 + GAP;
            }
            y += GROUP_GAP - GAP;
        } else {
            float h2 = CardH(g.notes[0]);
            D2D1_RECT_F top = D2D1::RectF(0, y, CARD_W, y + h2);
            int layers = std::min(2, (int)g.notes.size() - 1);
            for (int k = layers; k >= 1; k--) {  // as de baixo primeiro (ficam atrás)
                El u{ L"u:" + g.aumid + L":" + std::to_wstring(k), K_UNDER,
                      D2D1::RectF(PEEK_INSET * k, top.bottom + PEEK * k - h2, CARD_W - PEEK_INSET * k, top.bottom + PEEK * k), A_STACK };
                u.group = gi; u.layer = k;
                u.cutTop = top.bottom + PEEK * (k - 1) - 0.5f;
                u.from = D2D1::RectF(u.r.left, top.top, u.r.right, top.bottom); u.hasFrom = true;
                out.push_back(u);
            }
            El c{ topKey, K_CARD, top, layers ? A_STACK : A_CARD };
            c.group = gi; c.note = 0;
            out.push_back(c);
            y += h2 + PEEK * layers + GROUP_GAP;
        }
    }
    if (hiddenNotes > 0 || (g_groups.size() > 1 && !liquid::Golden())) {
        El p{ L"p:list", K_PILL };
        if (hiddenNotes > 0) { p.act = A_MORE; p.text = std::to_wstring(hiddenNotes) + (hiddenNotes == 1 ? Tr(L" notificação a mais") : Tr(L" notificações a mais")); }
        else { p.act = A_CLEAR_ALL; p.text = Tr(L"Limpar"); }
        float w = TextW(g_tfPill, p.text) + 26;
        p.r = D2D1::RectF((CARD_W - w) / 2, y, (CARD_W + w) / 2, y + PILL_H);
        out.push_back(p);
        y += PILL_H + GROUP_GAP;
    }
    // widgets: em linhas de 344 pt (pequeno 164 × 164, médio 344 × 164)
    y += 6;
    float x = 0, rowH = 0;
    for (int i = 0; i < (int)g_widgets.size(); i++) {
        auto& w = g_widgets[i];
        if (x > 0 && x + w.w > CARD_W + 0.5f) { y += rowH + WIDGET_GAP; x = 0; rowH = 0; }
        El e{ L"w:" + std::to_wstring(i), K_WIDGET, D2D1::RectF(x, y, x + w.w, y + w.h), A_WIDGET };
        e.widget = i;
        out.push_back(e);
        x += w.w + WIDGET_GAP;
        rowH = std::max(rowH, w.h);
    }
    if (!g_widgets.empty()) y += rowH + 14;
    El ed{ L"b:edit", K_PILL, {}, A_EDIT };
    ed.text = Tr(L"Editar Widgets…");
    float ew = TextW(g_tfPill, ed.text) + 26;
    float bx = (CARD_W - (ew + 8 + CIRCLE)) / 2;
    ed.r = D2D1::RectF(bx, y, bx + ew, y + PILL_H);
    out.push_back(ed);
    El gear{ L"b:gear", K_CIRCLE, D2D1::RectF(bx + ew + 8, y, bx + ew + 8 + CIRCLE, y + CIRCLE), A_SETTINGS };
    out.push_back(gear);
    y += PILL_H + 16;
    g_contentH = y;
    return out;
}

void Sync(const std::vector<El>& els) {
    for (auto& [k, a] : g_anim) a.live = false;
    std::map<int, std::wstring> topOfGroup;
    for (auto& e : els) if (e.kind == K_CARD && e.note == 0) topOfGroup[e.group] = e.key;
    g_order.clear();
    for (auto& e : els) {
        g_order.push_back(e.key);
        auto it = g_anim.find(e.key);
        if (it == g_anim.end()) {
            Anim a; a.el = e;
            D2D1_RECT_F f = e.hasFrom && !g_snap ? e.from : e.r;
            a.l.x = f.left; a.t.x = f.top; a.r.x = f.right; a.b.x = f.bottom;
            a.a.x = g_snap ? 1.f : 0.f;
            if (e.hasFrom && !g_snap && e.kind == K_CARD) { a.emerge = true; a.fromTop = e.from.top; }
            it = g_anim.emplace(e.key, a).first;
        } else it->second.el = e;
        it->second.live = true;
        if (e.group >= 0 && topOfGroup.count(e.group)) it->second.anchor = topOfGroup[e.group];
    }
    // as que saíram: ficam no fim da ordem e somem (as de um grupo que fechou sobem para o cartão de cima)
    for (auto& [k, a] : g_anim) {
        if (a.live) continue;
        if (std::find(g_order.begin(), g_order.end(), k) == g_order.end()) g_order.insert(g_order.begin(), k);
        if (!a.anchor.empty()) {
            auto t = std::find_if(els.begin(), els.end(), [&](const El& e) { return e.key == a.anchor; });
            if (t != els.end()) { float h = a.el.r.bottom - a.el.r.top; a.el.r = D2D1::RectF(t->r.left, t->r.top, t->r.right, t->r.top + h); }
        }
    }
    if (g_snap) for (auto& [k, a] : g_anim) { a.l.x = a.el.r.left; a.t.x = a.el.r.top; a.r.x = a.el.r.right; a.b.x = a.el.r.bottom; a.l.v = a.t.v = a.r.v = a.b.v = 0; }
    g_snap = false;
}

bool Step(Spring1& s, float target, float dt, float response = 0.38f) {
    float w = 2 * 3.14159265f / response;
    float acc = -w * w * (s.x - target) - 2 * w * s.v;
    s.v += acc * dt;
    s.x += s.v * dt;
    if (std::fabs(s.x - target) < 0.05f && std::fabs(s.v) < 0.5f) { s.x = target; s.v = 0; return false; }
    return true;
}

bool Settled() {
    for (auto& [k, a] : g_anim) {
        if (std::fabs(a.l.x - a.el.r.left) > 0.05f || std::fabs(a.t.x - a.el.r.top) > 0.05f ||
            std::fabs(a.r.x - a.el.r.right) > 0.05f || std::fabs(a.b.x - a.el.r.bottom) > 0.05f) return false;
        if (std::fabs(a.a.x - (a.live ? 1.f : 0.f)) > 0.01f) return false;
    }
    return std::fabs(g_scroll - g_scrollTarget) < 0.3f;
}

bool Advance(float dt) {
    bool busy = false;
    for (auto it = g_anim.begin(); it != g_anim.end();) {
        Anim& a = it->second;
        busy |= Step(a.l, a.el.r.left, dt); busy |= Step(a.t, a.el.r.top, dt);
        busy |= Step(a.r, a.el.r.right, dt); busy |= Step(a.b, a.el.r.bottom, dt);
        float ta = a.live ? 1.f : 0.f;
        a.a.x += (ta - a.a.x) * std::min(1.f, dt * (a.live ? 14.f : 18.f));
        if (std::fabs(a.a.x - ta) < 0.01f) a.a.x = ta; else busy = true;
        if (!a.live && a.a.x <= 0.f) {
            g_order.erase(std::remove(g_order.begin(), g_order.end(), it->first), g_order.end());
            it = g_anim.erase(it);
        } else ++it;
    }
    float before = g_scroll;
    g_scroll += (g_scrollTarget - g_scroll) * std::min(1.f, dt * 16.f);
    if (std::fabs(g_scroll - g_scrollTarget) < 0.3f) g_scroll = g_scrollTarget; else busy = true;
    busy |= before != g_scroll;
    return busy;
}

// ================= desenho
struct Hit { D2D1_RECT_F r; Act act; int group, note, widget; std::wstring key; };
std::vector<Hit> g_hits;
std::wstring g_hover;       // chave do cartão sob o mouse
std::wstring g_hoverPart;   // "x" (✕ do cartão), "opt" (Opções) ou ""

float Sc() { return ui::Scale(g_wnd); }

ID2D1Bitmap* IconFor(ID2D1DeviceContext* dc, const Group& g) {
    auto it = g_icons.find(g.aumid);
    if (it != g_icons.end()) return it->second;
    UINT w = 0, h = 0; std::vector<BYTE> px;
    std::wstring exe = g.aumid.find(L'!') == std::wstring::npos ? L"\\" + g.aumid + L".exe" : L"";
    std::wstring png = apps::PackIconByName(g.app, exe);
    bool ok = !png.empty() && ui::LoadPng(png, w, h, px);
    if (!ok) {  // app sem ícone do Sequoia: o do Windows (pela lista de apps), senão o logo da notificação
        for (auto& a : apps::list)
            if (!_wcsicmp(a.name.c_str(), g.app.c_str()) || !_wcsicmp(a.id.c_str(), (L"aumid:" + g.aumid).c_str())) {
                apps::EnsureIcon(a, 64);
                if (!a.icon.empty()) { w = a.iw; h = a.ih; px = a.icon; ok = true; }
                break;
            }
    }
    if (!ok && g.aumid.find(L'!') != std::wstring::npos) {
        ok = ui::ShellImage(L"shell:AppsFolder\\" + g.aumid, 64, w, h, px, true);
        if (ok) ui::TrimToSquare(w, h, px);
    }
    if (!ok) {
        std::lock_guard<std::mutex> k(g_mx);
        auto lg = g_logos.find(g.aumid);
        if (lg != g_logos.end() && !lg->second.px.empty()) { w = lg->second.w; h = lg->second.h; px = lg->second.px; ok = true; }
        else if (lg == g_logos.end()) return nullptr;  // logo ainda não lido: tenta de novo depois
    }
    ID2D1Bitmap* bmp = ok ? ui::Bitmap(dc, w, h, px) : nullptr;
    g_icons[g.aumid] = bmp;
    return bmp;
}

void DrawIcon(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const Group& g, D2D1_RECT_F r) {
    if (ID2D1Bitmap* b = IconFor(dc, g)) {
        dc->DrawBitmap(b, r, 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        return;
    }
    br->SetColor(D2D1::ColorF(0.55f, 0.55f, 0.58f, 1));
    float s = r.right - r.left;
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + s * 0.08f, r.top + s * 0.08f, r.right - s * 0.08f, r.bottom - s * 0.08f), s * 0.2f, s * 0.2f), br);
    br->SetColor(D2D1::ColorF(1, 1, 1, 1));
    simbolo::Draw(dc, L"bell.badge.fill", D2D1::RectF(r.left + s * 0.28f, r.top + s * 0.26f, r.right - s * 0.28f, r.bottom - s * 0.28f), br);
}

void DrawX(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, D2D1_POINT_2F c, float arm, float stroke, D2D1_COLOR_F col) {
    br->SetColor(col);
    ID2D1StrokeStyle* st = nullptr;
    ID2D1Factory* f = nullptr; dc->GetFactory(&f);
    f->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0, &st);
    dc->DrawLine({ c.x - arm, c.y - arm }, { c.x + arm, c.y + arm }, br, stroke, st);
    dc->DrawLine({ c.x - arm, c.y + arm }, { c.x + arm, c.y - arm }, br, stroke, st);
    if (st) st->Release();
    f->Release();
}

void Border(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const Pal& p, D2D1_RECT_F r, float rad) {
    if (liquid::On()) { liquid::Rim(dc, r, rad, ui::Dark(), 0.9f); return; }
    br->SetColor(p.line);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), rad - 0.5f, rad - 0.5f), br, 1);
}

// Leopard: o cartão claro do Aqua (degradê branco -> cinza bem claro, fio cinza, luz no alto e sombra leve)
void LeoCard(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, D2D1_RECT_F r, float rad, float a) {
    br->SetColor(D2D1::ColorF(0, 0, 0, 0.18f * a));
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left, r.top + 1.5f, r.right, r.bottom + 2), rad, rad), br);
    D2D1_GRADIENT_STOP st[2] = { { 0, D2D1::ColorF(1, 1, 1, 0.97f * a) }, { 1, D2D1::ColorF(0.89f, 0.89f, 0.90f, 0.97f * a) } };
    ID2D1GradientStopCollection* gs = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
    if (SUCCEEDED(dc->CreateGradientStopCollection(st, 2, &gs)) &&
        SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, r.top }, { 0, r.bottom }), gs, &lb)))
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), lb);
    if (lb) lb->Release();
    if (gs) gs->Release();
    br->SetColor(D2D1::ColorF(0, 0, 0, 0.30f * a));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), rad - 0.5f, rad - 0.5f), br, 1);
    br->SetColor(D2D1::ColorF(1, 1, 1, 0.9f * a));
    dc->DrawLine({ r.left + rad, r.top + 1.5f }, { r.right - rad, r.top + 1.5f }, br, 1);
}

void DrawCard(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const Pal& p, const Group& g, const Note& n, D2D1_RECT_F r, const std::wstring& key) {
    if (liquid::Leopard()) {
        // Leopard: cartão claro do Aqua de cantos 8, o ícone à esquerda, o título
        // em negrito preto e o texto embaixo; sem a hora
        LeoCard(dc, br, r, 8, 1);
        float cy0 = (r.top + r.bottom) / 2;
        DrawIcon(dc, br, g, D2D1::RectF(r.left + ICON_X, cy0 - ICON / 2, r.left + ICON_X + ICON, cy0 + ICON / 2));
        float tx0 = r.left + TEXT_X, top0 = r.top + PAD_V;
        Text(dc, br, n.title, g_tfTitle, D2D1::RectF(tx0, top0, r.right - PAD_R, top0 + TITLE_H), D2D1::ColorF(0, 0, 0, 0.92f));
        auto& b0 = Body(n);
        if (b0.first) {
            br->SetColor(D2D1::ColorF(0, 0, 0, 0.78f));
            dc->DrawTextLayout({ tx0, top0 + TITLE_H }, b0.first, br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        return;
    }
    Border(dc, br, p, r, CardR());
    float cy = (r.top + r.bottom) / 2;
    DrawIcon(dc, br, g, D2D1::RectF(r.left + ICON_X, cy - ICON / 2, r.left + ICON_X + ICON, cy + ICON / 2));
    bool hover = g_hover == key;
    float right = r.right - PAD_R;
    float tx = r.left + TEXT_X;
    float rowTop = r.top + PAD_V;
    float timeLeft;
    if (hover) {  // "Opções ⌄" no lugar da hora
        std::wstring o = Tr(L"Opções");
        float w = TextW(g_tfPill, o) + 30;
        D2D1_RECT_F b = D2D1::RectF(right - w + 4, rowTop - 1, right + 4, rowTop + 19);
        br->SetColor(g_hoverPart == L"opt" ? D2D1::ColorF(p.chip.r, p.chip.g, p.chip.b, p.chip.a * 2) : p.chip);
        dc->FillRoundedRectangle(D2D1::RoundedRect(b, 10, 10), br);
        Text(dc, br, o, g_tfPill, D2D1::RectF(b.left + 10, b.top, b.right - 18, b.bottom), p.text);
        br->SetColor(p.text);
        simbolo::Draw(dc, L"chevron.down", D2D1::RectF(b.right - 16, b.top + 7, b.right - 9, b.bottom - 7), br);
        g_hits.push_back({ b, A_NONE, -2, 0, -1, L"opt" });
        timeLeft = b.left;
    } else {
        std::wstring t = When(n.time);
        float w = TextW(g_tfTime, t);
        Text(dc, br, t, g_tfTime, D2D1::RectF(right - w - 2, rowTop, right, rowTop + TITLE_H), p.text2, DWRITE_TEXT_ALIGNMENT_TRAILING);
        timeLeft = right - w - 2;
    }
    Text(dc, br, n.title, g_tfTitle, D2D1::RectF(tx, rowTop, timeLeft - 6, rowTop + TITLE_H), p.text);
    auto& b = Body(n);
    if (b.first) {
        br->SetColor(p.text);
        dc->DrawTextLayout({ tx, rowTop + TITLE_H }, b.first, br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}

void DrawCircleButton(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const Pal& p, D2D1_RECT_F r, bool gear, bool hover) {
    float rad = (r.right - r.left) / 2;
    D2D1_POINT_2F c{ (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
    if (hover) { br->SetColor(p.chip); dc->FillEllipse(D2D1::Ellipse(c, rad, rad), br); }
    br->SetColor(p.line);
    dc->DrawEllipse(D2D1::Ellipse(c, rad - 0.5f, rad - 0.5f), br, 1);
    if (gear) { br->SetColor(p.text); simbolo::Draw(dc, L"gearshape.fill", D2D1::RectF(c.x - 7, c.y - 7, c.x + 7, c.y + 7), br); }
    else DrawX(dc, br, c, 3.6f, 1.6f, p.text);
}

// widgets que vêm junto: Calendário e Relógio (até o widgets.cpp registrar os dele)
void PaintCalendar(ID2D1DeviceContext* dc, float w, float h, bool dark) {
    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &br);
    SYSTEMTIME t; GetLocalTime(&t);
    static const wchar_t* days[] = { L"DOMINGO", L"SEGUNDA", L"TERÇA", L"QUARTA", L"QUINTA", L"SEXTA", L"SÁBADO" };
    Text(dc, br, Tr(days[t.wDayOfWeek]), g_tfPill, D2D1::RectF(16, 14, w - 12, 30), D2D1::ColorF(1.0f, 0.23f, 0.19f, 1));
    IDWriteTextFormat* big = Fmt(46, DWRITE_FONT_WEIGHT_LIGHT, L"SF Pro Display");
    Text(dc, br, std::to_wstring(t.wDay), big, D2D1::RectF(14, 28, w - 12, 82), dark ? D2D1::ColorF(1, 1, 1, 0.95f) : D2D1::ColorF(0, 0, 0, 0.88f));
    if (big) big->Release();
    Text(dc, br, Tr(L"Nenhum evento hoje"), g_tfTime, D2D1::RectF(16, h - 36, w - 12, h - 18), dark ? D2D1::ColorF(1, 1, 1, 0.55f) : D2D1::ColorF(0, 0, 0, 0.5f));
    br->Release();
}

void PaintClock(ID2D1DeviceContext* dc, float w, float h, bool dark) {
    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &br);
    D2D1_POINT_2F c{ w / 2, h / 2 };
    float R = std::min(w, h) / 2 - 12;
    br->SetColor(dark ? D2D1::ColorF(0.07f, 0.07f, 0.08f, 1) : D2D1::ColorF(1, 1, 1, 1));
    dc->FillEllipse(D2D1::Ellipse(c, R, R), br);
    D2D1_COLOR_F ink = dark ? D2D1::ColorF(1, 1, 1, 0.95f) : D2D1::ColorF(0, 0, 0, 0.9f);
    IDWriteTextFormat* num = Fmt(R * 0.22f, DWRITE_FONT_WEIGHT_MEDIUM, L"SF Pro Rounded");
    for (int i = 1; i <= 12; i++) {
        float a = i * 3.14159265f / 6;
        float x = c.x + std::sin(a) * R * 0.78f, y = c.y - std::cos(a) * R * 0.78f;
        Text(dc, br, std::to_wstring(i), num, D2D1::RectF(x - 12, y - 10, x + 12, y + 10), ink, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    if (num) num->Release();
    SYSTEMTIME t; GetLocalTime(&t);
    float sec = t.wSecond + t.wMilliseconds / 1000.f, mn = t.wMinute + sec / 60, hr = (t.wHour % 12) + mn / 60;
    ID2D1StrokeStyle* st = nullptr;
    ID2D1Factory* f = nullptr; dc->GetFactory(&f);
    f->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0, &st);
    auto hand = [&](float ang, float len, float width, D2D1_COLOR_F col, float tail) {
        br->SetColor(col);
        D2D1_POINT_2F d{ std::sin(ang), -std::cos(ang) };
        dc->DrawLine({ c.x - d.x * tail, c.y - d.y * tail }, { c.x + d.x * len, c.y + d.y * len }, br, width, st);
    };
    hand(hr * 3.14159265f / 6, R * 0.48f, 3.2f, ink, 0);
    hand(mn * 3.14159265f / 30, R * 0.72f, 2.4f, ink, 0);
    hand((float)t.wSecond * 3.14159265f / 30, R * 0.80f, 1.0f, D2D1::ColorF(1.0f, 0.58f, 0.0f, 1), R * 0.15f);
    br->SetColor(D2D1::ColorF(1.0f, 0.58f, 0.0f, 1));
    dc->FillEllipse(D2D1::Ellipse(c, 2.6f, 2.6f), br);
    if (st) st->Release();
    f->Release();
    br->Release();
}

// ---- Mac OS X Leopard: os widgets do Dashboard do 10.5 (o iCal em folha de calendário e o relógio redondo)
void LeoGrad(ID2D1DeviceContext* dc, D2D1_RECT_F r, float rad, std::initializer_list<D2D1_GRADIENT_STOP> st, bool ellipse = false) {
    ID2D1GradientStopCollection* gs = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
    if (SUCCEEDED(dc->CreateGradientStopCollection(st.begin(), (UINT)st.size(), &gs)) &&
        SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, r.top }, { 0, r.bottom }), gs, &lb))) {
        if (ellipse) dc->FillEllipse(D2D1::Ellipse({ (r.left + r.right) / 2, (r.top + r.bottom) / 2 }, (r.right - r.left) / 2, (r.bottom - r.top) / 2), lb);
        else dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), lb);
    }
    if (lb) lb->Release();
    if (gs) gs->Release();
}
void PaintCalendarLeo(ID2D1DeviceContext* dc, float w, float h, bool) {
    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &br);
    SYSTEMTIME t; GetLocalTime(&t);
    D2D1_RECT_F pg = D2D1::RectF(14, 12, w - 14, h - 16);
    br->SetColor(D2D1::ColorF(0, 0, 0, 0.35f));  // a sombra do widget
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(pg.left, pg.top + 3, pg.right, pg.bottom + 4), 9, 9), br);
    LeoGrad(dc, pg, 9, { { 0, D2D1::ColorF(1, 1, 1) }, { 1, D2D1::ColorF(0.88f, 0.88f, 0.89f) } });
    float band = (pg.bottom - pg.top) * 0.30f;
    ID2D1Layer* l = nullptr; dc->CreateLayer(nullptr, &l);
    ID2D1Factory* f = nullptr; dc->GetFactory(&f);
    ID2D1RoundedRectangleGeometry* rg = nullptr; f->CreateRoundedRectangleGeometry(D2D1::RoundedRect(pg, 9, 9), &rg);
    dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), rg), l);
    LeoGrad(dc, D2D1::RectF(pg.left, pg.top, pg.right, pg.top + band), 0, { { 0, D2D1::ColorF(0.93f, 0.36f, 0.33f) }, { 0.5f, D2D1::ColorF(0.82f, 0.13f, 0.12f) }, { 1, D2D1::ColorF(0.70f, 0.06f, 0.06f) } });
    dc->PopLayer();
    rg->Release(); f->Release(); l->Release();
    static const wchar_t* meses[] = { L"JAN", L"FEV", L"MAR", L"ABR", L"MAI", L"JUN", L"JUL", L"AGO", L"SET", L"OUT", L"NOV", L"DEZ" };
    IDWriteTextFormat* tm = Fmt(band * 0.55f, DWRITE_FONT_WEIGHT_BOLD, L"Lucida Sans Unicode");
    Text(dc, br, Tr(meses[(t.wMonth + 11) % 12]), tm, D2D1::RectF(pg.left, pg.top, pg.right, pg.top + band), D2D1::ColorF(1, 1, 1, 1), DWRITE_TEXT_ALIGNMENT_CENTER);
    if (tm) tm->Release();
    IDWriteTextFormat* big = Fmt((pg.bottom - pg.top - band) * 0.66f, DWRITE_FONT_WEIGHT_BOLD, L"Lucida Sans Unicode");
    Text(dc, br, std::to_wstring(t.wDay), big, D2D1::RectF(pg.left, pg.top + band, pg.right, pg.bottom), D2D1::ColorF(0.1f, 0.1f, 0.1f, 1), DWRITE_TEXT_ALIGNMENT_CENTER);
    if (big) big->Release();
    br->SetColor(D2D1::ColorF(0, 0, 0, 0.35f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(pg.left + 0.5f, pg.top + 0.5f, pg.right - 0.5f, pg.bottom - 0.5f), 8.5f, 8.5f), br, 1);
    br->Release();
}
void PaintClockLeo(ID2D1DeviceContext* dc, float w, float h, bool) {
    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &br);
    D2D1_POINT_2F c{ w / 2, h / 2 - 2 };
    float R = std::min(w, h) / 2 - 12;
    br->SetColor(D2D1::ColorF(0, 0, 0, 0.35f));
    dc->FillEllipse(D2D1::Ellipse({ c.x, c.y + 3 }, R + 1, R + 1), br);
    // o aro cromado e o mostrador branco do relógio do Dashboard
    LeoGrad(dc, D2D1::RectF(c.x - R, c.y - R, c.x + R, c.y + R), 0, { { 0, D2D1::ColorF(0.97f, 0.97f, 0.98f) }, { 0.5f, D2D1::ColorF(0.62f, 0.63f, 0.66f) }, { 1, D2D1::ColorF(0.92f, 0.92f, 0.94f) } }, true);
    float r2 = R * 0.88f;
    LeoGrad(dc, D2D1::RectF(c.x - r2, c.y - r2, c.x + r2, c.y + r2), 0, { { 0, D2D1::ColorF(0.86f, 0.86f, 0.87f) }, { 1, D2D1::ColorF(1, 1, 1) } }, true);
    D2D1_COLOR_F ink = D2D1::ColorF(0.08f, 0.08f, 0.08f, 1);
    IDWriteTextFormat* num = Fmt(r2 * 0.24f, DWRITE_FONT_WEIGHT_BOLD, L"Lucida Sans Unicode");
    for (int i = 1; i <= 12; i++) {
        float a = i * 3.14159265f / 6;
        float x = c.x + std::sin(a) * r2 * 0.76f, y = c.y - std::cos(a) * r2 * 0.76f;
        Text(dc, br, std::to_wstring(i), num, D2D1::RectF(x - 14, y - 10, x + 14, y + 10), ink, DWRITE_TEXT_ALIGNMENT_CENTER);
    }
    if (num) num->Release();
    SYSTEMTIME t; GetLocalTime(&t);
    float mn = t.wMinute + t.wSecond / 60.f, hr = (t.wHour % 12) + mn / 60;
    ID2D1StrokeStyle* st = nullptr;
    ID2D1Factory* f = nullptr; dc->GetFactory(&f);
    f->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0, &st);
    auto hand = [&](float ang, float len, float width, D2D1_COLOR_F col, float tail) {
        br->SetColor(col);
        D2D1_POINT_2F d{ std::sin(ang), -std::cos(ang) };
        dc->DrawLine({ c.x - d.x * tail, c.y - d.y * tail }, { c.x + d.x * len, c.y + d.y * len }, br, width, st);
    };
    hand(hr * 3.14159265f / 6, r2 * 0.50f, 3.6f, ink, r2 * 0.08f);
    hand(mn * 3.14159265f / 30, r2 * 0.74f, 2.6f, ink, r2 * 0.08f);
    hand((float)t.wSecond * 3.14159265f / 30, r2 * 0.80f, 1.2f, D2D1::ColorF(0.85f, 0.10f, 0.08f, 1), r2 * 0.18f);
    br->SetColor(D2D1::ColorF(0.85f, 0.10f, 0.08f, 1));
    dc->FillEllipse(D2D1::Ellipse(c, 2.8f, 2.8f), br);
    if (st) st->Release();
    f->Release();
    br->Release();
}

// Os widgets da Central (embaixo das notificações): cada um some com central_<id>=0 no ajustes.txt, pelo
// "Editar Widgets…" da Central ou pelo botão direito em cima dele ("Remover Widget")
struct CentralWidget { const wchar_t* id; const wchar_t* name; };
const CentralWidget CENTRAL_W[] = { { L"calendario", L"Calendário" }, { L"relogio", L"Relógio" } };
std::vector<int> g_widgetKind;  // g_widgets[i] é CENTRAL_W[g_widgetKind[i]] (só com os padrões)

bool CentralOn(int k) { return shell::Setting((std::wstring(L"central_") + CENTRAL_W[k].id).c_str(), L"1") != L"0"; }

void DefaultWidgets() {
    g_widgets.clear();
    g_widgetKind.clear();
    if (CentralOn(0)) {
        g_widgets.push_back({ WIDGET_S, WIDGET_S, [](ID2D1DeviceContext* dc, float w, float h, bool d) { (liquid::Leopard() ? PaintCalendarLeo : PaintCalendar)(dc, w, h, d); },
                              [] { ShellExecuteW(nullptr, L"open", L"outlookcal:", nullptr, nullptr, SW_SHOWNORMAL); } });
        g_widgetKind.push_back(0);
    }
    if (CentralOn(1)) {
        g_widgets.push_back({ WIDGET_S, WIDGET_S, [](ID2D1DeviceContext* dc, float w, float h, bool d) { (liquid::Leopard() ? PaintClockLeo : PaintClock)(dc, w, h, d); },
                              [] { ShellExecuteW(nullptr, L"open", L"ms-clock:", nullptr, nullptr, SW_SHOWNORMAL); } });
        g_widgetKind.push_back(1);
    }
}

D2D1_RECT_F Cur(const Anim& a) { return D2D1::RectF(a.l.x, a.t.x, a.r.x, a.b.x); }
D2D1_RECT_F Off(D2D1_RECT_F r, float dx, float dy) { return D2D1::RectF(r.left + dx, r.top + dy, r.right + dx, r.bottom + dy); }

void Paint() {
    if (!g_open && !g_closing) return;
    float sc = Sc();
    RECT wr; GetClientRect(g_wnd, &wr);
    int wpx = wr.right, hpx = wr.bottom;
    float viewH = hpx / sc;
    auto els = Layout();
    g_scrollTarget = std::clamp(g_scrollTarget, 0.f, std::max(0.f, g_contentH - viewH));
    g_scroll = std::clamp(g_scroll, 0.f, std::max(0.f, g_contentH - viewH));
    Sync(els);
    auto* dc = ui::GpuBegin(g_s, wpx, hpx, sc);
    if (!dc) return;
    Pal p = Colors();
    bool dark = Dark();
    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(p.text, &br);
    float ox = PAD_L + g_slide * COL_W, oy = -g_scroll;  // deslocamento (pt) do conteúdo na janela
    g_host = &g_hostPanel;
    g_host->used = 0;
    g_hits.clear();
    ID2D1Layer* layer = nullptr;
    dc->CreateLayer(nullptr, &layer);
    // Leopard: nada de vidro; tudo em cartões claros do Aqua, widgets soltos como no Dashboard
    const bool leo = liquid::Leopard();
    auto smoke = [&](D2D1_RECT_F r, float rad, float a) { LeoCard(dc, br, r, rad, std::min(1.f, a + 0.2f)); };  // o cartão claro
    for (auto& key : g_order) {
        auto it = g_anim.find(key);
        if (it == g_anim.end()) continue;
        Anim& a = it->second;
        const El& e = a.el;
        D2D1_RECT_F r = Off(Cur(a), ox, oy);
        if (r.bottom < -20 || r.top > viewH + 20) continue;
        float alpha = a.a.x;
        if (a.emerge && a.live) {  // saindo de baixo do de cima: só aparece conforme desce (texto não se mistura)
            float span = e.r.top - a.fromTop;
            float k = span > 1 ? std::clamp((a.t.x - a.fromTop) / span, 0.f, 1.f) : 1.f;
            if (k >= 0.999f && a.a.x >= 0.999f) a.emerge = false;
            alpha *= k * k;
        }
        if (alpha <= 0.003f) continue;
        D2D1_RECT_F px = D2D1::RectF(r.left * sc, r.top * sc, r.right * sc, r.bottom * sc);
        bool fade = alpha < 0.999f;
        bool clip = e.kind == K_UNDER;
        float cut = (e.cutTop + oy);
        if (fade) dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), alpha), layer);
        if (clip) dc->PushAxisAlignedClip(D2D1::RectF(-1000, cut, 10000, 10000), D2D1_ANTIALIAS_MODE_ALIASED);
        const Group* g = e.group >= 0 && e.group < (int)g_groups.size() ? &g_groups[e.group] : nullptr;
        switch (e.kind) {
        case K_CARD:
            if (!g || e.note < 0 || e.note >= (int)g->notes.size()) break;
            if (!leo) GlassRect(px, CardR() * sc, p.tint, alpha);
            if (!g_hasGlass && !leo) { br->SetColor(dark ? D2D1::ColorF(0.17f, 0.17f, 0.18f, 0.96f) : D2D1::ColorF(0.95f, 0.95f, 0.96f, 0.96f)); dc->FillRoundedRectangle(D2D1::RoundedRect(r, CardR(), CardR()), br); }
            DrawCard(dc, br, p, *g, g->notes[e.note], r, e.key);
            g_hits.push_back({ r, e.act, e.group, e.note, -1, e.key });
            break;
        case K_UNDER: {
            D2D1_COLOR_F t = p.tint; t.a *= e.layer == 1 ? 0.85f : 0.7f;
            if (leo) { smoke(r, 8, e.layer == 1 ? 0.62f : 0.48f); g_hits.push_back({ D2D1::RectF(r.left, cut, r.right, r.bottom), e.act, e.group, 0, -1, L"n:" + std::to_wstring(g ? g->notes[0].id : 0) }); break; }
            GlassRect(px, CardR() * sc, t, alpha * (e.layer == 1 ? 0.9f : 0.75f), std::max(0.f, cut * sc));
            if (!g_hasGlass) { br->SetColor(dark ? D2D1::ColorF(0.2f, 0.2f, 0.21f, 0.9f) : D2D1::ColorF(0.9f, 0.9f, 0.91f, 0.9f)); dc->FillRoundedRectangle(D2D1::RoundedRect(r, CardR(), CardR()), br); }
            Border(dc, br, p, r, CardR());
            g_hits.push_back({ D2D1::RectF(r.left, cut, r.right, r.bottom), e.act, e.group, 0, -1, L"n:" + std::to_wstring(g ? g->notes[0].id : 0) });
            break;
        }
        case K_HEADER: {
            static IDWriteTextFormat* tfBig = Fmt(17, DWRITE_FONT_WEIGHT_BOLD, L"SF Pro Display");
            IDWriteTextFormat* tf = e.layer == 7 && tfBig ? tfBig : g_tfHeader;
            Text(dc, br, e.text, tf, D2D1::RectF(r.left + 2, r.top, r.right - 150, r.bottom), D2D1::ColorF(1, 1, 1, 0.97f), DWRITE_TEXT_ALIGNMENT_LEADING, true);
            break;
        }
        case K_PILL: {
            float rad = (r.bottom - r.top) / 2;
            bool hv = g_hover == e.key;
            if (leo) {
                smoke(r, rad, hv ? 0.85f : 0.70f);
                Text(dc, br, e.text, g_tfPill, r, D2D1::ColorF(0, 0, 0, 0.85f), DWRITE_TEXT_ALIGNMENT_CENTER);
                g_hits.push_back({ r, e.act, e.group, -1, -1, e.key });
                break;
            }
            GlassRect(px, rad * sc, p.tintPill, alpha);
            if (hv) { br->SetColor(p.chip); dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), br); }
            if (!g_hasGlass) { br->SetColor(dark ? D2D1::ColorF(0.2f, 0.2f, 0.21f, 0.9f) : D2D1::ColorF(0.92f, 0.92f, 0.93f, 0.9f)); dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), br); }
            Border(dc, br, p, r, rad);
            Text(dc, br, e.text, g_tfPill, r, p.text, DWRITE_TEXT_ALIGNMENT_CENTER);
            g_hits.push_back({ r, e.act, e.group, -1, -1, e.key });
            break;
        }
        case K_CIRCLE: {
            float rad = (r.right - r.left) / 2;
            if (leo) smoke(r, rad, 0.70f);
            else GlassRect(px, rad * sc, p.tintPill, alpha);
            if (!g_hasGlass && !leo) { br->SetColor(dark ? D2D1::ColorF(0.2f, 0.2f, 0.21f, 0.9f) : D2D1::ColorF(0.92f, 0.92f, 0.93f, 0.9f)); dc->FillEllipse(D2D1::Ellipse({ (r.left + r.right) / 2, (r.top + r.bottom) / 2 }, rad, rad), br); }
            DrawCircleButton(dc, br, p, r, e.act == A_SETTINGS, g_hover == e.key);
            g_hits.push_back({ r, e.act, e.group, -1, -1, e.key });
            break;
        }
        case K_WIDGET: {
            if (e.widget < 0 || e.widget >= (int)g_widgets.size()) break;
            if (!leo) {  // Leopard: o widget do Dashboard desenha a própria forma, sem placa
            GlassRect(px, WidgetR() * sc, p.tint, alpha);
            if (!g_hasGlass) { br->SetColor(dark ? D2D1::ColorF(0.17f, 0.17f, 0.18f, 0.96f) : D2D1::ColorF(0.95f, 0.95f, 0.96f, 0.96f)); dc->FillRoundedRectangle(D2D1::RoundedRect(r, WidgetR(), WidgetR()), br); }
            Border(dc, br, p, r, WidgetR());
            }
            D2D1_MATRIX_3X2_F old; dc->GetTransform(&old);
            dc->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            dc->SetTransform(D2D1::Matrix3x2F::Translation(r.left, r.top) * old);
            if (g_widgets[e.widget].paint) g_widgets[e.widget].paint(dc, r.right - r.left, r.bottom - r.top, dark);
            dc->SetTransform(old);
            dc->PopAxisAlignedClip();
            g_hits.push_back({ r, e.act, -1, -1, e.widget, e.key });
            break;
        }
        case K_LABEL:
            Text(dc, br, e.text, g_tfEmpty, r, D2D1::ColorF(1, 1, 1, 0.92f), DWRITE_TEXT_ALIGNMENT_CENTER, true);
            break;
        case K_EMPTY: {
            if (leo) smoke(r, 8, 0.75f);
            else GlassRect(px, CardR() * sc, p.tint, alpha);
            if (!g_hasGlass && !leo) { br->SetColor(dark ? D2D1::ColorF(0.17f, 0.17f, 0.18f, 0.96f) : D2D1::ColorF(0.95f, 0.95f, 0.96f, 0.96f)); dc->FillRoundedRectangle(D2D1::RoundedRect(r, CardR(), CardR()), br); }
            if (!leo) Border(dc, br, p, r, CardR());
            br->SetColor(p.text2);
            float cx = (r.left + r.right) / 2;
            simbolo::Draw(dc, L"bell.badge.fill", D2D1::RectF(cx - 11, r.top + 14, cx + 11, r.top + 36), br);
            Text(dc, br, Tr(L"Sem Notificações"), g_tfTitle, D2D1::RectF(r.left, r.top + 40, r.right, r.top + 58), p.text, DWRITE_TEXT_ALIGNMENT_CENTER);
            Text(dc, br, Tr(L"Permita que o Shell leia as notificações do Windows."), g_tfTime, D2D1::RectF(r.left, r.top + 58, r.right, r.top + 74), p.text2, DWRITE_TEXT_ALIGNMENT_CENTER);
            break;
        }
        }
        if (clip) dc->PopAxisAlignedClip();
        if (fade) dc->PopLayer();
    }
    // ✕ do cartão sob o mouse, no canto de cima à esquerda (por cima de tudo)
    if (!g_hover.empty() && g_hover.rfind(L"n:", 0) == 0) {
        auto it = g_anim.find(g_hover);
        if (it != g_anim.end() && it->second.live && it->second.a.x > 0.5f) {
            D2D1_RECT_F r = Off(Cur(it->second), ox, oy);
            D2D1_POINT_2F c{ r.left + 4, r.top + 4 };
            float rad = 9.5f;
            br->SetColor(D2D1::ColorF(0, 0, 0, dark ? 0.35f : 0.12f));
            dc->FillEllipse(D2D1::Ellipse({ c.x, c.y + 0.5f }, rad + 0.8f, rad + 0.8f), br);
            br->SetColor(p.close);
            dc->FillEllipse(D2D1::Ellipse(c, rad, rad), br);
            DrawX(dc, br, c, 3.2f, 1.5f, g_hoverPart == L"x" ? p.text : p.closeGlyph);
            const Hit* card = nullptr;
            for (auto& h : g_hits) if (h.key == g_hover && h.note >= 0) card = &h;
            if (card) g_hits.push_back({ D2D1::RectF(c.x - rad - 2, c.y - rad - 2, c.x + rad + 2, c.y + rad + 2), A_NONE, card->group, card->note, -1, L"x" });
        }
    }
    layer->Release();
    br->Release();
    ui::GpuEnd(g_s);
    GlassEnd();
}

// ================= abrir e fechar
void Place() {
    float sc = Sc();
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
    int top = mi.rcMonitor.top + menubar::Height();
    int w = (int)std::lround(COL_W * sc), h = std::max(100, (int)mi.rcWork.bottom - top);
    int x = mi.rcMonitor.right - w;
    SetWindowPos(g_wnd, HWND_TOPMOST, x, top, w, h, SWP_NOACTIVATE);
    SetWindowPos(g_back, g_wnd, x, top, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

// Os quadros vêm do relógio de quadros (DwmFlush numa thread). Às vezes ele não entregava nenhum: o clique abria a
// pilha (o log dizia "abriu a pilha") e a tela não mudava, nem o fechar escondia a janela (19/11/2026). Um temporizador
// de reserva desenha o quadro quando o relógio ficou mais de 50 ms sem mandar nada.
double g_frameMsgAt = 0;
// O relógio do passo (g_lastFrame) só recomeça se a animação estava parada. Antes cada Animate() o zerava: com os
// dados chegando a cada segundo (WM_NC_DATA chama Animate) e o clique, o passo de cada quadro ficava em 1 ms e as
// molas quase não andavam: a pilha "abria" (o log dizia) mas os cartões ficavam parados no lugar (19/11/2026)
bool g_running = false;
void Animate() {
    if (!g_running) { g_lastFrame = anim::Now(); g_running = true; }
    ui::FrameClock(g_wnd, WM_NC_FRAME, true);
    SetTimer(g_wnd, T_ANIM, 33, nullptr);
}

void Hide() {
    g_open = g_closing = false;
    g_hover.clear(); g_hoverPart.clear();
    KillTimer(g_wnd, T_TICK);
    ui::FrameClock(g_wnd, WM_NC_FRAME, false);
    KillTimer(g_wnd, T_ANIM);
    g_running = false;
    ShowWindow(g_wnd, SW_HIDE);
    ShowWindow(g_back, SW_HIDE);
    for (auto& gl : g_hostPanel.pool) gl.outer.IsVisible(false);
}

void Close(const wchar_t* why = nullptr) {
    if (!g_open) return;
    if (why) Log(L"notificações: fechou (%ls)", why);  // cada caminho diz por quê (a Central sumia sem rastro)
    g_open = false;
    menubar::SoundChanged(false);  // a cápsula do relógio da barra sai
    g_closing = true;
    g_closedAt = GetTickCount();
    g_slideAt = anim::Now();
    g_hover.clear(); g_hoverPart.clear();
    Animate();
}

void Frame() {
    double now = anim::Now();
    float dt = (float)std::clamp((now - g_lastFrame) / 1000.0, 0.0, 0.034);
    g_lastFrame = now;
    float t = (float)((now - g_slideAt) / 1000.0);
    bool slideBusy;
    // Tahoe (guia, seção 53, gravado): abre deslizando da direita em ~0,30 s (em 35 ms já mostra quase tudo, sem
    // passar do ponto: out-expo) e fecha em ~0,20 s. Reduzir Movimento: aparece/some na hora (seção 62).
    const bool th = liquid::On();
    if (g_closing) {  // sai depressa, acelerando (como o Mac)
        float D = anim::reduceMotion ? 0.001f : th ? 0.20f : 0.30f;
        float k = std::min(1.f, t / D);
        g_slide = anim::Bezier(k, 0.4f, 0.0f, 0.9f, 0.6f);
        slideBusy = k < 1;
        if (!slideBusy) { Hide(); return; }
    } else if (anim::reduceMotion) {
        g_slide = 0; slideBusy = false;
    } else if (th) {
        float k = std::min(1.f, t / 0.30f);
        g_slide = 1 - anim::Bezier(k, 0.16f, 1.f, 0.3f, 1.f);
        slideBusy = k < 1;
    } else {
        g_slide = 1 - anim::Spring(t, 0.48f, 1.0f);
        slideBusy = t < 0.9f;
        if (!slideBusy) g_slide = 0;
    }
    bool busy = Advance(dt);
    Paint();
    // O layout novo (Sync, dentro do Paint) só entra agora: a pilha que acabou de abrir tem alvos novos que o Advance
    // ainda não via. Antes o relógio desligava aqui ("nada a animar") e os cartões ficavam parados no lugar antigo:
    // a pilha não abria (19/11/2026). Só para se, depois do desenho, tudo já estiver no alvo.
    if (!busy && !slideBusy) busy = !Settled();
    if (!busy && !slideBusy) { ui::FrameClock(g_wnd, WM_NC_FRAME, false); KillTimer(g_wnd, T_ANIM); g_running = false; }
}

void OpenApp(const Note& n) {
    if (n.aumid == L"WacOS.Atualizacao") { shell::OpenSettings(L"atualizacao"); return; }  // atualizacao.cpp
    for (auto& a : apps::list)
        if (!a.wins.empty() && (!_wcsicmp(a.id.c_str(), (L"aumid:" + n.aumid).c_str()) || !_wcsicmp(a.name.c_str(), n.app.c_str()))) { apps::Activate(a); return; }
    for (auto& a : apps::list)
        if (!_wcsicmp(a.name.c_str(), n.app.c_str())) { apps::Launch(a); return; }
    if (!apps::ActivatePackaged(L"shell:AppsFolder\\" + n.aumid))
        ShellExecuteW(nullptr, L"open", (L"shell:AppsFolder\\" + n.aumid).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void ClearGroup(int gi) {
    if (gi < 0 || gi >= (int)g_groups.size()) return;
    std::vector<UINT> ids;
    for (auto& n : g_groups[gi].notes) ids.push_back(n.id);
    g_expanded.erase(g_groups[gi].aumid);
    RemoveIds(ids);
}

void Options(int gi, int ni, D2D1_RECT_F r) {
    if (gi < 0 || gi >= (int)g_groups.size() || ni < 0 || ni >= (int)g_groups[gi].notes.size()) return;
    Note n = g_groups[gi].notes[ni];
    bool stack = g_groups[gi].notes.size() > 1 && !g_expanded.count(g_groups[gi].aumid);
    std::vector<ctxmenu::Item> items;
    ctxmenu::Item it;
    it.label = Tr(L"Abrir ") + n.app; it.id = 1; items.push_back(it);
    it.label = stack ? Tr(L"Limpar Tudo") : Tr(L"Limpar"); it.id = 2; items.push_back(it);
    if (!stack && g_groups[gi].notes.size() > 1) { it.label = Tr(L"Limpar Tudo de ") + n.app; it.id = 3; items.push_back(it); }
    ctxmenu::Item sep; sep.separator = true; items.push_back(sep);
    it.label = Tr(L"Ajustes de Notificações…"); it.id = 4; items.push_back(it);
    RECT wr; GetWindowRect(g_wnd, &wr);
    float sc = Sc();
    POINT pt{ wr.left + (LONG)(r.left * sc), wr.top + (LONG)((r.bottom + 4) * sc) };
    g_menu = true;
    int id = ctxmenu::ShowItems(g_wnd, pt, items);
    g_menu = false;
    if (id == 1) { RemoveIds({ n.id }); Close(); OpenApp(n); return; }
    if (id == 2) { if (stack) ClearGroup(gi); else RemoveIds({ n.id }); }
    if (id == 3) ClearGroup(gi);
    if (id == 4) { Close(); shell::OpenSettings(L"notificacoes"); return; }
    SetForegroundWindow(g_wnd);
    Animate();
}

// "Editar Widgets…" da Central (e o botão direito num widget dela): ligar/desligar o Calendário e o Relógio daqui;
// os widgets da mesa têm o menu deles ("Widgets da Mesa…")
void WidgetsMenu(D2D1_RECT_F r, int widget) {
    std::vector<ctxmenu::Item> items;
    for (int k = 0; k < (int)std::size(CENTRAL_W); k++) {
        ctxmenu::Item it; it.label = Tr(CENTRAL_W[k].name); it.id = 10 + k; it.checked = CentralOn(k); items.push_back(it);
    }
    ctxmenu::Item sep; sep.separator = true; items.push_back(sep);
    if (widget >= 0 && widget < (int)g_widgetKind.size()) { ctxmenu::Item rm; rm.label = Tr(L"Remover Widget"); rm.id = 1; items.push_back(rm); }
    ctxmenu::Item desk; desk.label = Tr(L"Widgets da Mesa…"); desk.id = 2; items.push_back(desk);
    RECT wr; GetWindowRect(g_wnd, &wr);
    float sc = Sc();
    POINT pt{ wr.left + (LONG)(r.left * sc), wr.top + (LONG)((r.bottom + 4) * sc) };
    g_menu = true;
    int id = ctxmenu::ShowItems(g_wnd, pt, items);
    g_menu = false;
    int off = -1;
    if (id == 1) off = g_widgetKind[widget];
    else if (id >= 10 && id < 10 + (int)std::size(CENTRAL_W)) {
        int k = id - 10;
        if (CentralOn(k)) off = k;
        else shell::SetSetting((std::wstring(L"central_") + CENTRAL_W[k].id).c_str(), L"1");
    } else if (id == 2) { Close(); POINT c; GetCursorPos(&c); widgets::EditMenu(c); return; }
    if (off >= 0) shell::SetSetting((std::wstring(L"central_") + CENTRAL_W[off].id).c_str(), L"0");
    if (id > 0 && !g_customWidgets) DefaultWidgets();
    SetForegroundWindow(g_wnd);
    Animate();
}

const Hit* HitAt(D2D1_POINT_2F p) {
    const Hit* hit = nullptr;
    for (auto& h : g_hits) if (p.x >= h.r.left && p.x < h.r.right && p.y >= h.r.top && p.y < h.r.bottom) hit = &h;  // o último desenhado vence
    return hit;
}

DWORD g_expandedAt = 0;  // quando uma pilha foi aberta

void Click(D2D1_POINT_2F p) {
    g_clickAt = GetTickCount();
    const Hit* h = HitAt(p);
    if (!h) { Close(L"clique no vazio"); return; }  // clique no vazio da coluna: fecha, como clicar fora
    Hit hit = *h;
    if (hit.key == L"x") {  // ✕ do cartão: numa pilha fechada limpa o grupo todo
        if (hit.group >= 0 && hit.group < (int)g_groups.size()) {
            auto& g = g_groups[hit.group];
            if (g.notes.size() > 1 && !g_expanded.count(g.aumid)) ClearGroup(hit.group);
            else if (hit.note >= 0 && hit.note < (int)g.notes.size()) RemoveIds({ g.notes[hit.note].id });
        }
        g_hover.clear();
        Animate();
        return;
    }
    if (hit.key == L"opt") {
        const Hit* card = nullptr;
        for (auto& c : g_hits) if (c.key == g_hover && c.note >= 0) card = &c;
        if (card) Options(card->group, card->note, hit.r);
        return;
    }
    switch (hit.act) {
    case A_STACK:
        if (hit.group >= 0 && hit.group < (int)g_groups.size()) { g_expanded.insert(g_groups[hit.group].aumid); g_expandedAt = GetTickCount(); Log(L"notificações: abriu a pilha de %ls (%zu notificações, aumid %ls)", g_groups[hit.group].app.c_str(), g_groups[hit.group].notes.size(), g_groups[hit.group].aumid.c_str()); }
        break;
    case A_CARD:
        // segundo clique de um clique duplo na pilha: só abre o grupo (não abre o app nem fecha a central)
        if (GetTickCount() - g_expandedAt < 500) break;
        if (hit.group >= 0 && hit.group < (int)g_groups.size() && hit.note >= 0 && hit.note < (int)g_groups[hit.group].notes.size()) {
            Note n = g_groups[hit.group].notes[hit.note];
            RemoveIds({ n.id });
            Close(L"clique no cartão: abre o app");
            OpenApp(n);
            return;
        }
        break;
    case A_SHOW_LESS: if (hit.group >= 0 && hit.group < (int)g_groups.size()) g_expanded.erase(g_groups[hit.group].aumid); break;
    case A_CLEAR_GROUP: ClearGroup(hit.group); break;
    case A_CLEAR_ALL: {
        std::vector<UINT> ids;
        for (auto& g : g_groups) for (auto& n : g.notes) ids.push_back(n.id);
        g_expanded.clear();
        g_showAll = false;
        RemoveIds(ids);
        break;
    }
    case A_MORE: g_showAll = true; break;
    case A_EDIT:
        if (g_editWidgets) { Close(); g_editWidgets(); return; }
        WidgetsMenu(hit.r, -1);  // Calendário e Relógio da Central; "Widgets da Mesa…" leva ao menu da mesa
        return;
    case A_SETTINGS: Close(); shell::OpenSettings(L"notificacoes"); return;
    case A_PERMISSION: Close(); ShellExecuteW(nullptr, L"open", L"ms-settings:privacy-notifications", nullptr, nullptr, SW_SHOWNORMAL); return;
    case A_WIDGET:
        if (hit.widget >= 0 && hit.widget < (int)g_widgets.size() && g_widgets[hit.widget].click) { auto f = g_widgets[hit.widget].click; Close(); f(); return; }
        break;
    default: break;
    }
    Animate();
}

void Hover(D2D1_POINT_2F p) {
    const Hit* h = HitAt(p);
    std::wstring hv, part;
    if (h) {
        if (h->key == L"x" || h->key == L"opt") { hv = g_hover; part = h->key; }
        else hv = h->key;
    }
    if (hv != g_hover || part != g_hoverPart) { g_hover = hv; g_hoverPart = part; Paint(); }
}

D2D1_POINT_2F Pt(LPARAM l) { float sc = Sc(); return { (short)LOWORD(l) / sc, (short)HIWORD(l) / sc }; }

void BannerShow(UINT id);

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); Paint(); return 0; }
    case WM_NC_FRAME: g_frameMsgAt = anim::Now(); Frame(); return 0;
    case WM_NC_NEW: BannerShow((UINT)l); return 0;
    case WM_NC_DATA:
        dock::BadgesChanged();
        Rebuild();
        if (g_open) Animate();
        return 0;
    case WM_TIMER:
        if (w == T_TICK && g_open) Paint();  // horas relativas e o relógio
        if (w == T_ANIM) {
            if (anim::Now() - g_frameMsgAt > 50) {
                static int avisos = 0;
                if (avisos++ < 5) Log(L"notificações: relógio de quadros parado; desenhando pelo temporizador");
                Frame();
            }
            return 0;
        }
        if (w == T_FOCO) {
            KillTimer(h, T_FOCO);
            if (!g_open || g_menu) return 0;
            HWND f = GetForegroundWindow(); DWORD pid = 0;
            if (f) GetWindowThreadProcessId(f, &pid);
            if (f && pid != GetCurrentProcessId() && GetTickCount() - g_clickAt >= 800) {
                wchar_t fc[64] = {}; GetClassNameW(f, fc, 64);
                Log(L"notificações: fechou (outro programa na frente: %ls)", fc); Close();
            }
            else SetForegroundWindow(g_wnd);  // ninguém (ou o próprio Shell) na frente: continua aberta
        }
        return 0;
    case WM_LBUTTONDOWN: {
        D2D1_POINT_2F p = Pt(l);
        const Hit* hh = HitAt(p);
        Log(L"notificações: clique em %.0f,%.0f (%ls, ação %d, aberta %d, frente %p)", p.x, p.y, hh ? hh->key.c_str() : L"nada", hh ? (int)hh->act : -1, (int)g_open, GetForegroundWindow());
        if (g_open) Click(p);
        return 0;
    }
    case WM_RBUTTONUP: {  // botão direito num widget da Central: ligar/desligar e "Remover Widget"
        if (!g_open || g_customWidgets) return 0;
        const Hit* hh = HitAt(Pt(l));
        if (hh && hh->act == A_WIDGET) { Hit copy = *hh; WidgetsMenu(copy.r, copy.widget); }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!g_open) return 0;
        TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t);
        Hover(Pt(l));
        return 0;
    }
    case WM_MOUSELEAVE: if (!g_hover.empty() && !g_menu) { g_hover.clear(); g_hoverPart.clear(); Paint(); } return 0;
    case WM_MOUSEWHEEL:
        g_scrollTarget -= GET_WHEEL_DELTA_WPARAM(w) / 120.f * 60.f;
        Animate();
        return 0;
    case WM_KEYDOWN: if (w == VK_ESCAPE) Close(L"Esc"); return 0;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE && !g_menu) {
            // a ativação foi para outra janela do próprio Shell (o vidro de trás, o banner, a barra): não fecha. Antes um
            // clique que caía no vidro (g_back) ou um menu da barra tirava o foco e a Central sumia da tela
            HWND to = (HWND)l; DWORD pid = 0;
            if (to) GetWindowThreadProcessId(to, &pid);
            if (to && pid == GetCurrentProcessId() && (to == g_back || GetAncestor(to, GA_ROOTOWNER) == g_wnd || IsOurBanner(to))) {
                SetForegroundWindow(g_wnd);
                return 0;
            }
            {   // onde estava o ponteiro e que janela havia embaixo dele (o clique que "não chegava" na Central)
                wchar_t c0[64] = {}, c1[64] = {}; if (to) GetClassNameW(to, c0, 64);
                POINT cp{}; GetCursorPos(&cp); HWND under = WindowFromPoint(cp); if (under) GetClassNameW(GetAncestor(under, GA_ROOT), c1, 64);
                RECT wr{}; GetWindowRect(g_wnd, &wr);
                Log(L"notificações: perdeu a ativação para %ls (%u ms depois do clique); ponteiro %ld,%ld sobre %ls; Central %ld,%ld-%ld,%ld visível %d",
                    to ? c0 : L"ninguém", GetTickCount() - g_clickAt, cp.x, cp.y, c1, wr.left, wr.top, wr.right, wr.bottom, (int)IsWindowVisible(g_wnd));
            }
            if (!to) { SetTimer(h, T_FOCO, 150, nullptr); return 0; }  // "para ninguém" (ao abrir uma pilha): confere já já
            if (GetTickCount() - g_clickAt < 800) { SetTimer(h, T_FOCO, 150, nullptr); return 0; }  // logo depois de um clique aqui dentro: não foi clique fora
            wchar_t cls[64] = {}; if (to) GetClassNameW(to, cls, 64);
            Log(L"notificações: fechou (perdeu o foco para %ls)", cls);
            Close();
        }
        return 0;
    case WM_DPICHANGED: if (g_open) { Place(); Paint(); } return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ================= banner: a notificação que chega com o painel fechado (o do Mac no lugar do toast do Windows)
HWND g_bwnd, g_bback;
ui::GpuSurface g_bs;
Note g_bnote;
bool g_bshow, g_bout, g_bhover, g_bhoverX;
double g_bAt;
float g_bslide = 1;
const UINT_PTR T_BANNER = 2;
// some sozinho depois de 5 s (parado enquanto o mouse estiver em cima); Ajustes > Notificações > "Tempo do banner na
// tela" muda (notif_duracao, em segundos, 2 a 30)
UINT BannerMs() { std::wstring v = shell::Setting(L"notif_duracao", L"5"); return (UINT)(std::clamp(_wtof(v.c_str()), 2.0, 30.0) * 1000); }

bool BannersOn() { return shell::Setting(L"banners_mac", L"1") != L"0"; }
// outra notificação chegando com o banner na tela: a nova cai no lugar numa mola e a anterior fica
// espiando embaixo (pilha de banners do Mac)
bool g_bstack;
double g_bswapAt;
const float STACK_PEEK = 8;
float SwapDy() {
    if (g_bswapAt <= 0) return 0;
    float t = (float)((anim::Now() - g_bswapAt) / 1000.0);
    return -14.f * (1 - anim::Spring(t, 0.34f, 0.78f));
}
float BannerH() { return TOP + CardH(g_bnote) + 14 + (g_bstack ? STACK_PEEK : 0); }
D2D1_RECT_F BannerCard() { float ox = PAD_L + g_bslide * COL_W, dy = SwapDy(); return D2D1::RectF(ox, TOP + dy, ox + CARD_W, TOP + dy + CardH(g_bnote)); }

void BannerPlace() {
    float sc = ui::Scale(g_bwnd);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
    int top = mi.rcMonitor.top + menubar::Height();
    int w = (int)std::lround(COL_W * sc), h = (int)std::lround(BannerH() * sc);
    int x = mi.rcMonitor.right - w;
    SetWindowPos(g_bwnd, HWND_TOPMOST, x, top, w, h, SWP_NOACTIVATE);
    SetWindowPos(g_bback, g_bwnd, x, top, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void BannerPaint() {
    if (!g_bshow) return;
    float sc = ui::Scale(g_bwnd);
    RECT wr; GetClientRect(g_bwnd, &wr);
    auto* dc = ui::GpuBegin(g_bs, wr.right, wr.bottom, sc);
    if (!dc) return;
    Pal p = Colors();
    bool dark = Dark();
    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(p.text, &br);
    D2D1_RECT_F r = BannerCard();
    g_host = &g_hostBanner;
    g_host->used = 0;
    if (g_bstack) {  // o banner anterior, espiando embaixo do novo
        float base = TOP + CardH(g_bnote);
        D2D1_RECT_F pr = D2D1::RectF(r.left + 9, base - 24, r.right - 9, base + STACK_PEEK);
        GlassRect(D2D1::RectF(pr.left * sc, pr.top * sc, pr.right * sc, pr.bottom * sc), CardR() * sc, p.tint, 0.8f, r.bottom * sc);
        if (!g_hasGlass) {
            br->SetColor(dark ? D2D1::ColorF(0.15f, 0.15f, 0.16f, 0.90f) : D2D1::ColorF(0.90f, 0.90f, 0.91f, 0.90f));
            dc->FillRoundedRectangle(D2D1::RoundedRect(pr, CardR(), CardR()), br);
        }
    }
    if (!liquid::Leopard()) {  // Leopard: o balão do Growl é desenhado pelo DrawCard (sem o vidro)
    GlassRect(D2D1::RectF(r.left * sc, r.top * sc, r.right * sc, r.bottom * sc), CardR() * sc, p.tint, 1);
    if (!g_hasGlass) { br->SetColor(dark ? D2D1::ColorF(0.17f, 0.17f, 0.18f, 0.97f) : D2D1::ColorF(0.95f, 0.95f, 0.96f, 0.97f)); dc->FillRoundedRectangle(D2D1::RoundedRect(r, CardR(), CardR()), br); }
    }
    Group g{ g_bnote.aumid, g_bnote.app, {} };
    std::wstring saved = g_hover;
    g_hover.clear();
    DrawCard(dc, br, p, g, g_bnote, r, L"banner");
    g_hover = saved;
    if (g_bhover) {  // ✕ no canto, como no painel
        D2D1_POINT_2F c{ r.left + 4, r.top + 4 };
        br->SetColor(D2D1::ColorF(0, 0, 0, dark ? 0.35f : 0.12f));
        dc->FillEllipse(D2D1::Ellipse({ c.x, c.y + 0.5f }, 10.3f, 10.3f), br);
        br->SetColor(p.close);
        dc->FillEllipse(D2D1::Ellipse(c, 9.5f, 9.5f), br);
        DrawX(dc, br, c, 3.2f, 1.5f, g_bhoverX ? p.text : p.closeGlyph);
    }
    br->Release();
    ui::GpuEnd(g_bs);
    GlassEnd();
    g_host = &g_hostPanel;
}

void BannerHide() {
    bool was = g_bshow;
    g_bshow = g_bout = false;
    if (was) menubar::SoundChanged(false);  // a pílula do relógio sai
    g_bstack = false; g_bswapAt = 0;
    KillTimer(g_bwnd, T_BANNER);
    ui::FrameClock(g_bwnd, WM_NC_BFRAME, false);
    ShowWindow(g_bwnd, SW_HIDE);
    ShowWindow(g_bback, SW_HIDE);
    for (auto& gl : g_hostBanner.pool) gl.outer.IsVisible(false);
}

void BannerOut() {
    if (!g_bshow || g_bout) return;
    g_bout = true;
    g_bAt = anim::Now();
    ui::FrameClock(g_bwnd, WM_NC_BFRAME, true);
}

void BannerShowNote(const Note& n);
void BannerShow(UINT id) {
    if (g_open || g_closing || !BannersOn() || !g_bwnd) return;
    Note n;
    {
        std::lock_guard<std::mutex> k(g_mx);
        auto it = std::find_if(g_data.begin(), g_data.end(), [&](const Note& x) { return x.id == id; });
        if (it == g_data.end()) return;
        n = *it;
    }
    BannerShowNote(n);
}

// ---- avisos dos apps do Shell (Lembretes e Calendário): o Shell lê os arquivos deles a cada 30 s e mostra o
// banner quando a hora chega, mesmo com o app fechado. Já avisados: %APPDATA%\Shell\avisos-feitos.txt.
const UINT AVISO_ID = 0xF0000000;
std::set<std::wstring> g_avisados;
bool g_avisadosLidos;
std::wstring ShellData(const wchar_t* name) {
    PWSTR p = nullptr; std::wstring d;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) d = std::wstring(p) + L"\\Shell\\" + name;
    CoTaskMemFree(p);
    return d;
}
std::vector<std::vector<std::wstring>> ReadTsv(const std::wstring& path) {
    std::vector<std::vector<std::wstring>> out;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r, ccs=UTF-8") || !f) return out;
    wchar_t line[8192];
    while (fgetws(line, 8192, f)) {
        std::wstring s = line; while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
        std::vector<std::wstring> v; size_t a = 0, b;
        while ((b = s.find(L'\t', a)) != std::wstring::npos) { v.push_back(s.substr(a, b - a)); a = b + 1; }
        v.push_back(s.substr(a));
        for (auto& x : v) { std::wstring o; for (size_t i = 0; i < x.size(); i++) { if (x[i] == L'\\' && i + 1 < x.size()) { wchar_t c = x[++i]; o += c == L'n' ? L' ' : c == L't' ? L' ' : c; } else o += x[i]; } x = o; }
        out.push_back(v);
    }
    fclose(f);
    return out;
}
void ChecarAvisos() {
    if (!BannersOn()) return;
    std::wstring done = ShellData(L"avisos-feitos.txt");
    if (!g_avisadosLidos) { g_avisadosLidos = true; for (auto& v : ReadTsv(done)) if (!v.empty()) g_avisados.insert(v[0]); }
    long long now = (long long)time(nullptr) / 60;
    std::vector<std::pair<std::wstring, Note>> due;
    static UINT seq;
    auto add = [&](const std::wstring& key, const wchar_t* app, const std::wstring& exe, const std::wstring& title, const std::wstring& body) {
        if (g_avisados.count(key)) return;
        Note n; n.id = AVISO_ID + (++seq & 0xFFFFFF); n.aumid = shell::dir + L"\\" + exe; n.app = Tr(app); n.title = title; n.body = body;
        FILETIME ft; GetSystemTimeAsFileTime(&ft); n.time = ((long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
        due.push_back({ key, n });
    };
    for (auto& v : ReadTsv(ShellData(L"lembretes.txt")))  // R id lista feito data título notas sinal
        if (v.size() >= 8 && v[0] == L"R" && v[3] != L"1") {
            long long m = _wtoi64(v[4].c_str());
            if (m && m <= now && m > now - 24 * 60) add(L"R" + v[1] + L"@" + v[4], L"Lembretes", EXE_REMINDERS, v[5], v[6]);
        }
    for (auto& v : ReadTsv(ShellData(L"calendario.txt")))  // E id início fim diaInteiro título cor notas
        if (v.size() >= 6 && v[0] == L"E" && v[4] != L"1") {
            long long s = _wtoi64(v[2].c_str());
            if (s && s <= now && s > now - 10) {
                SYSTEMTIME st{}; time_t t = (time_t)(s * 60); tm lt{}; localtime_s(&lt, &t);
                wchar_t b[32]; swprintf_s(b, L"%d:%02d", lt.tm_hour, lt.tm_min);
                add(L"E" + v[1] + L"@" + v[2], L"Calendário", EXE_CALENDAR, v[5], std::wstring(Tr(L"Agora")) + L" · " + b);
                (void)st;
            }
        }
    {   // alarmes do Relógio: A id hora minuto ligado nome dias (bits: 0 = domingo; 0 = uma vez só)
        time_t tt = time(nullptr); tm lt{}; localtime_s(&lt, &tt);
        std::vector<std::wstring> once;  // alarmes de uma vez só que tocaram: desligam (como no Mac)
        for (auto& v : ReadTsv(ShellData(L"relogio.txt")))
            if (v.size() >= 7 && v[0] == L"A" && v[4] == L"1") {
                int hh = _wtoi(v[2].c_str()), mm = _wtoi(v[3].c_str()), days = _wtoi(v[6].c_str());
                if (days && !(days & (1 << lt.tm_wday))) continue;
                int diff = (lt.tm_hour * 60 + lt.tm_min) - (hh * 60 + mm);
                if (diff < 0 || diff > 2) continue;
                wchar_t key[64]; swprintf_s(key, L"A%ls@%04d%02d%02d%02d%02d", v[1].c_str(), lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday, hh, mm);
                wchar_t hm[16]; swprintf_s(hm, L"%d:%02d", hh, mm);
                if (!g_avisados.count(key)) { add(key, L"Relógio", EXE_CLOCK, v[5].empty() ? std::wstring(Tr(L"Alarme")) : v[5], hm); if (!days) once.push_back(v[1]); }
            }
        if (!once.empty()) {  // desliga no arquivo (troca o "1" de ligado por "0")
            std::wstring path = ShellData(L"relogio.txt");
            FILE* f = nullptr; std::vector<std::wstring> lines;
            if (!_wfopen_s(&f, path.c_str(), L"r, ccs=UTF-8") && f) { wchar_t ln[4096]; while (fgetws(ln, 4096, f)) { std::wstring s = ln; while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back(); lines.push_back(s); } fclose(f); }
            for (auto& s : lines)
                for (auto& id : once)
                    if (s.rfind(L"A\t" + id + L"\t", 0) == 0) {
                        size_t p = 0; for (int k = 0; k < 4 && p != std::wstring::npos; k++) p = s.find(L'\t', p + 1);
                        if (p != std::wstring::npos && p + 1 < s.size() && s[p + 1] == L'1') s[p + 1] = L'0';
                    }
            if (!_wfopen_s(&f, path.c_str(), L"w, ccs=UTF-8") && f) { for (auto& s : lines) fwprintf(f, L"%ls\n", s.c_str()); fclose(f); }
        }
    }
    if (due.empty()) return;
    FILE* f = nullptr;
    _wfopen_s(&f, done.c_str(), L"a, ccs=UTF-8");
    for (auto& [k, n] : due) { g_avisados.insert(k); if (f) fwprintf(f, L"%ls\n", k.c_str()); }
    if (f) fclose(f);
    for (auto& [k, n] : due) { BannerShowNote(n); shell::Sound(L"notificacao"); Log(L"aviso: %ls — %ls", n.app.c_str(), n.title.c_str()); }
}

void BannerShowNote(const Note& n) {
    // jogo ou vídeo em tela cheia no monitor do banner: sem banner (ela fica na Central), senão o jogo sai da tela cheia
    if (shell::NaoIncomodar(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY))) { Log(L"notificação: banner calado (app em tela cheia)"); return; }
    // Ilha Dinâmica ligada: a notificação aparece nela, no lugar do banner do canto
    if (!g_open && !g_closing) {
        Group gr{ n.aumid, n.app, {} };
        Note nn = n;
        if (ilha::Notificacao(n.app, n.title, n.body,
                              [gr](ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, D2D1_RECT_F r) { DrawIcon(dc, br, gr, r); },
                              [nn] { OpenApp(nn); })) { Log(L"notificação na ilha: %ls", n.app.c_str()); return; }
    }
    if (g_open || g_closing || !BannersOn() || !g_bwnd) return;
    bool wasIn = g_bshow && !g_bout;
    g_bnote = n;
    g_bshow = true;
    g_bout = false;
    if (!wasIn) { g_bAt = anim::Now(); g_bslide = 1; g_bhover = g_bhoverX = false; g_bstack = false; g_bswapAt = 0; }
    else { g_bstack = true; g_bswapAt = anim::Now(); }  // já tinha um: a nova cai por cima, a velha espia embaixo
    BannerPlace();
    BannerPaint();
    ShowWindow(g_bwnd, SW_SHOWNOACTIVATE);
    SetTimer(g_bwnd, T_BANNER, BannerMs(), nullptr);
    menubar::SoundChanged(false);  // Tahoe: o relógio ganha a pílula de destaque enquanto o banner aparece
    ui::FrameClock(g_bwnd, WM_NC_BFRAME, true);
}

void BannerFrame() {
    float t = (float)((anim::Now() - g_bAt) / 1000.0);
    if (g_bout) {
        float k = std::min(1.f, t / 0.30f);
        g_bslide = anim::Bezier(k, 0.4f, 0.0f, 0.9f, 0.6f);
        if (k >= 1) { BannerHide(); return; }
    } else {
        // Tahoe (guia, seção 65): mola de resposta 0,24 s e amortecimento 1,0 (sem passar do ponto): 90 % em
        // 0,18 s, assenta em ~0,30 s
        bool th = liquid::On();
        g_bslide = anim::reduceMotion ? 0.f : 1 - (th ? anim::Spring(t, 0.24f, 1.0f) : anim::Spring(t, 0.42f, 0.82f));  // entra da direita e assenta
        bool swapDone = g_bswapAt <= 0 || anim::Now() - g_bswapAt > 650;
        if (t > (th ? 0.45f : 0.9f) && swapDone) { g_bslide = 0; g_bswapAt = 0; ui::FrameClock(g_bwnd, WM_NC_BFRAME, false); }
    }
    BannerPaint();
}

LRESULT CALLBACK BannerProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); BannerPaint(); return 0; }
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_NC_BFRAME: BannerFrame(); return 0;
    case WM_TIMER:
        if (w == T_BANNER) { if (g_bhover) SetTimer(h, T_BANNER, 1500, nullptr); else { KillTimer(h, T_BANNER); BannerOut(); } }
        if (w == 7) ChecarAvisos();  // Lembretes e Calendário
        return 0;
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: {
        if (!g_bshow || g_bout) return 0;
        float sc = ui::Scale(h);
        D2D1_POINT_2F p{ (short)LOWORD(l) / sc, (short)HIWORD(l) / sc };
        D2D1_RECT_F r = BannerCard();
        float dx = p.x - (r.left + 4), dy = p.y - (r.top + 4);
        bool onX = dx * dx + dy * dy <= 12 * 12;
        bool onCard = p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom;
        if (m == WM_LBUTTONDOWN) {
            if (onX && g_bhover) { BannerOut(); return 0; }  // fica na Central, como no Mac
            if (onCard) {
                Note n = g_bnote;
                BannerOut();
                if (n.id >= AVISO_ID) { ShellExecuteW(nullptr, L"open", n.aumid.c_str(), nullptr, nullptr, SW_SHOWNORMAL); return 0; }  // aviso do Lembretes/Calendário: abre o app
                RemoveIds({ n.id }); OpenApp(n);
            }
            return 0;
        }
        TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t);
        bool hv = onCard || (onX && g_bhover);
        if (hv != g_bhover || onX != g_bhoverX) { g_bhover = hv; g_bhoverX = onX; BannerPaint(); }
        return 0;
    }
    case WM_MOUSELEAVE: if (g_bhover) { g_bhover = g_bhoverX = false; BannerPaint(); } return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ================= o toast do Windows: a janela "New notification" do ShellExperienceHost vai para fora da tela
// (nada fica gravado; ao sair, ela volta para o lugar). A notificação continua chegando para a Central.
HWND g_toast;
RECT g_toastRect;
bool g_toastMoved;

bool IsToast(HWND h) {
    if (h == g_toast) return true;
    wchar_t c[64] = {};
    if (!GetClassNameW(h, c, 64) || wcscmp(c, L"Windows.UI.Core.CoreWindow")) return false;
    wchar_t t[128] = {};
    GetWindowTextW(h, t, 128);
    _wcslwr_s(t);
    if (!wcsstr(t, L"notification") && !wcsstr(t, L"notifica")) return false;
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!ph) return false;
    wchar_t path[MAX_PATH] = {}; DWORD n = MAX_PATH;
    QueryFullProcessImageNameW(ph, 0, path, &n);
    CloseHandle(ph);
    _wcslwr_s(path);
    if (!wcsstr(path, L"shellexperiencehost.exe")) return false;
    g_toast = h;
    return true;
}

void CALLBACK OnToast(HWINEVENTHOOK, DWORD, HWND h, LONG obj, LONG, DWORD, DWORD) {
    if (obj != OBJID_WINDOW || !h || !IsToast(h) || !BannersOn()) return;
    RECT r; GetWindowRect(h, &r);
    if (r.left <= -30000) return;
    g_toastRect = r;
    g_toastMoved = true;
    SetWindowPos(h, nullptr, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

LRESULT CALLBACK BackProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

bool notificacoes::Create(HINSTANCE hi) {
    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = hi; wc.lpszClassName = L"ShellNotificacoes";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    WNDCLASSW bc{}; bc.lpfnWndProc = BackProc; bc.hInstance = hi; bc.lpszClassName = L"ShellNotificacoesFundo";
    RegisterClassW(&bc);
    g_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, L"Central de Notificações", WS_POPUP,
                            0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
    g_back = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, bc.lpszClassName, L"", WS_POPUP,
                             0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
    if (!g_wnd || !g_back) return false;
    g_hasGlass = GlassInit(g_back, g_hostPanel);
    DWORD corner = 1; DwmSetWindowAttribute(g_back, 33, &corner, sizeof(corner));
    COLORREF none = 0xFFFFFFFE;
    DwmSetWindowAttribute(g_back, 34, &none, sizeof(none));
    apps::ours.push_back(g_wnd);
    apps::ours.push_back(g_back);
    g_s.hwnd = g_wnd;
    g_tfTitle = Fmt(13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_tfBody = Fmt(13, DWRITE_FONT_WEIGHT_NORMAL, nullptr, true);
    if (g_tfBody) g_tfBody->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, LINE_H, 12.5f);
    g_tfTime = Fmt(12, DWRITE_FONT_WEIGHT_NORMAL);
    g_tfPill = Fmt(12, DWRITE_FONT_WEIGHT_MEDIUM);
    g_tfHeader = Fmt(21, DWRITE_FONT_WEIGHT_BOLD, L"SF Pro Display");
    g_tfEmpty = Fmt(15, DWRITE_FONT_WEIGHT_SEMI_BOLD, L"SF Pro Display");
    if (!g_customWidgets) DefaultWidgets();
    // banner do Mac (notificação chegando com o painel fechado)
    WNDCLASSW bw{}; bw.lpfnWndProc = BannerProc; bw.hInstance = hi; bw.lpszClassName = L"ShellBanner"; bw.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&bw);
    g_bwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, bw.lpszClassName, L"Notificação", WS_POPUP,
                             0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
    g_bback = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, bc.lpszClassName, L"", WS_POPUP,
                              0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
    if (g_bwnd && g_bback) {
        if (g_hasGlass) GlassInit(g_bback, g_hostBanner);
        DwmSetWindowAttribute(g_bback, 33, &corner, sizeof(corner));
        DwmSetWindowAttribute(g_bback, 34, &none, sizeof(none));
        apps::ours.push_back(g_bwnd);
        apps::ours.push_back(g_bback);
        g_bs.hwnd = g_bwnd;
        SetTimer(g_bwnd, 7, 30000, nullptr);  // avisos do Lembretes e do Calendário
    }
    // o toast do Windows sai da tela assim que aparece (o banner do Mac fica no lugar)
    SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, OnToast, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    jogo::GanchoPosicao(OnToast, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);  // sai no modo jogo (jogo.cpp)
    std::thread(Worker).detach();
    return true;
}

bool notificacoes::IsOpen() { return g_open; }
bool notificacoes::BannerVisible() { return g_bshow && !g_bout; }

void notificacoes::Toggle() {
    if (g_open) { Close(L"clique no relógio / canto ativo"); return; }
    if (GetTickCount() - g_closedAt < 300) return;  // o clique no relógio acabou de fechar (perdeu o foco): não reabre
    BannerHide();
    g_open = true;
    menubar::SoundChanged(false);  // o ícone da barra ganha a cápsula do Tahoe
    g_closing = false;
    g_showAll = false;
    g_expanded.clear();
    g_scroll = g_scrollTarget = 0;
    g_anim.clear();
    g_order.clear();
    g_snap = true;
    g_slideAt = anim::Now();
    g_slide = 1;
    Rebuild();
    Wake();  // relê já (e a cada 2 s enquanto aberto)
    Place();
    Paint();
    ShowWindow(g_wnd, SW_SHOWNOACTIVATE);
    SetForegroundWindow(g_wnd);
    SetFocus(g_wnd);
    SetTimer(g_wnd, T_TICK, 1000, nullptr);
    Animate();
}

void notificacoes::SetWidgets(std::vector<Widget> list, std::function<void()> onEdit) {
    g_customWidgets = true;
    g_widgets = std::move(list);
    g_editWidgets = std::move(onEdit);
    if (g_open) Paint();
}

void notificacoes::Stop() {
    if (g_toastMoved && g_toast && IsWindow(g_toast))  // o toast do Windows volta para onde estava
        SetWindowPos(g_toast, nullptr, g_toastRect.left, g_toastRect.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    g_toastMoved = false;
}

std::vector<notificacoes::Brief> notificacoes::Recentes() {
    std::vector<Brief> r;
    std::lock_guard<std::mutex> k(g_mx);
    for (auto& n : g_data) r.push_back({ n.aumid, n.app, n.time });
    return r;
}
