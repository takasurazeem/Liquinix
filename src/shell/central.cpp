// Central de Controle no visual do macOS Sequoia: painel de vidro embaixo do ícone da barra de menus,
// com Wi-Fi/Bluetooth/AirDrop, Foco, Mission Control, Espelhamento de Tela, brilho, volume e o que está
// tocando. Medidas tiradas de capturas do Sequoia (painel de 320 pt, blocos com 10 pt de margem).
#include "shell.h"
#include "../menu/context_menu.h"
#include "../comum/simbolos.h"
#include <dwmapi.h>
#include <wlanapi.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <wincodec.h>
#include <shcore.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Storage.Streams.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <algorithm>

#include <DispatcherQueue.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <windows.ui.composition.interop.h>
#include "vidro_wuc.h"
#include "../comum/idioma.h"
#include "../comum/nomes.h"
#include <array>
#include <intrin.h>
#include "../comum/vidro.h"
#include <bluetoothapis.h>
#pragma comment(lib, "bthprops.lib")

#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "dxva2.lib")

namespace wdr = winrt::Windows::Devices::Radios;
namespace wmc = winrt::Windows::Media::Control;

namespace {

const float PW = 320, M = 10, G = 10, COL = (PW - 2 * M - G) / 2;  // pt
const float CONN_H = 140, BLOCK = 64, EDIT = 40, RADIUS = 18, TILE_R = 12;
const UINT WM_CC_FRAME = WM_APP + 70, WM_CC_UPDATE = WM_APP + 71;
const UINT_PTR T_POLL = 1;
const ULONG_PTR KEY_MARK = 0x53484C4C;

HWND g_wnd, g_back;
ui::GpuSurface g_s;
bool g_open, g_glass, g_menu;
double g_openAt;   // ms de anim::Now
DWORD g_closedAt;
IDWriteTextFormat *g_tfBold, *g_tfSmall, *g_tfTile, *g_tfGlyph;

// ---- estado (lido em threads; a janela só desenha)
std::mutex g_mx;
struct State {
    int radio[2] = { -2, -2 };   // Wi-Fi, Bluetooth: 1 ligado, 0 desligado, -1 não existe, -2 carregando
    std::wstring ssid;
    float volume = 0; bool mute = false;
    float bright = -1;           // -1: o monitor não aceita (DDC/CI)
    bool focus = false;
    int airdrop = 0;             // 0 desligado, 1 meus dispositivos, 2 todos
    std::wstring title, artist;  // tocando agora
    bool playing = false, hasMedia = false;
    std::vector<BYTE> art; UINT artW = 0, artH = 0; bool artNew = false;
} g_st;
ID2D1Bitmap* g_art;

wdr::Radio g_radio[2]{ nullptr, nullptr };
std::atomic<bool> g_mediaBusy{ false };

// ---- interação
enum HitId { H_WIFI_BTN, H_WIFI, H_BT_BTN, H_BT, H_AIRDROP, H_FOCUS, H_MISSION, H_STAGE, H_MIRROR, H_DISPLAY, H_BRIGHT, H_SOUND, H_VOLUME, H_OUTPUT,
             H_PLAY, H_NEXT, H_EDIT, H_DARK, H_SHOT, H_REMOVE, H_EXTRA,
             H_FOCUS_TEXT, H_DET_PANEL, H_DET_SWITCH, H_DET_ROW, H_DET_SET, H_NIGHT };  // Tahoe: módulos expandidos
struct Hit { HitId id; D2D1_RECT_F r; int arg = -1; };  // arg: H_REMOVE/H_EXTRA = índice no layout
std::vector<Hit> g_hits;
int g_drag = -1;  // H_BRIGHT ou H_VOLUME sendo arrastado
D2D1_RECT_F g_dragTrack{};
// Tahoe: o botão dos controles Tela e Som é o mesmo do menu de som: pílula branca em repouso; arrastando,
// uma lente de vidro maior que mostra o trilho ampliado e estica com a velocidade
// botão do slider (guia macOS26-Theme-Reference, seção 30, medido a 240 Hz): 20 × 16 em repouso; apertado cresce
// para 25 × 20 (raio 8 → 10) e vira vidro
const float KN_W = 20, KN_H = 16, KN_LENS = 1.25f;
float g_kPress[2], g_kStretch[2], g_kLastX[2] = { -1, -1 };
double g_kFrame;
bool StepKnobs() {  // true enquanto algum botão ainda mexe
    double now = anim::Now();
    float dt = (float)std::clamp(now - g_kFrame, 1.0, 50.0);
    g_kFrame = now;
    bool moving = false;
    for (int i = 0; i < 2; i++) {
        float target = g_drag == (i == 0 ? H_BRIGHT : H_VOLUME) ? 1.f : 0.f;
        g_kPress[i] += (target - g_kPress[i]) * (1 - std::exp(-dt / (target > 0 ? 55.f : 80.f)));
        if (std::fabs(target - g_kPress[i]) < 0.004f) g_kPress[i] = target;
        g_kStretch[i] *= std::exp(-dt / 90.f);
        if (g_kStretch[i] < 0.003f) g_kStretch[i] = 0;
        moving |= g_kPress[i] != target || g_kStretch[i] > 0;
    }
    return moving;
}

float Sc() { return ui::Scale(g_wnd); }

// ---- Tahoe (Tema nos Ajustes): sem painel; cada módulo é uma peça de vidro solta sobre a mesa.
// Medidas das capturas do macOS 26 (2x): coluna de 135 pt, cápsulas e círculos de 60 pt, 12 pt entre eles,
// Tela/Som de 62 pt com trilho fino e ícone nas duas pontas, "Editar Controles" numa pílula embaixo.
// Guia macOS26-Theme-Reference (seção 40, medido no macOS 26 com Acessibilidade): 4 colunas de 64 com 12 entre
// elas (grade de 292), 1×1 = 64, 2×1 = 140 × 64, 2×2 = 140 × 140, Tela/Som 292 × 64, "Editar Controles" 112 × 24.
const float T_W = 308, T_M = 8, T_G = 12, T_CAP = 64, T_BAR = 64, T_EDIT = 24;
const float T_COL = (T_W - 2 * T_M - T_G) / 2;
bool Tahoe() { return liquid::On(); }

// ---- Controles do Tahoe (macOS 26): a Central é uma lista de controles numa grade de 4 colunas (círculo 1×1,
// cápsula 2×1, Tocando Agora 2×2, Tela/Som 4×1), em ordem, cada um no primeiro lugar livre (como no Mac).
// "Editar Controles": um "−" em cada um tira, a galeria (janela ao lado) põe. Ajuste: cc_layout (ids com "|").
struct Ctl { const wchar_t* id; const wchar_t* name; const wchar_t* cat; int w, h; const wchar_t* sym; wchar_t fb; };
const Ctl CTLS[] = {
    { L"wifi", L"Wi‑Fi", L"Conectividade", 2, 1, L"wifi", 0xE701 },
    { L"tocando", L"Tocando Agora", L"Tocando Agora", 2, 2, L"play.fill", 0xE768 },
    { L"bluetooth", L"Bluetooth", L"Conectividade", 2, 1, L"", 0xE702 },
    { L"airdrop", L"AirDrop", L"Conectividade", 2, 1, L"dot.radiowaves.left.and.right", 0xE72D },
    { L"mission", L"Mission Control", L"Mesa e Finder", 1, 1, L"rectangle.3.group", 0xE7C4 },
    { L"organizador", L"Organizador Visual", L"Mesa e Finder", 1, 1, L"squares.leading.rectangle", 0xE7C4 },
    { L"espelhar", L"Espelhar Tela", L"Tela e Brilho", 1, 1, L"rectangle.on.rectangle", 0xEC15 },
    { L"escuro", L"Modo Escuro", L"Tela e Brilho", 1, 1, L"circle.lefthalf.filled", 0xE793 },
    { L"captura", L"Captura de Tela", L"Captura de Tela", 1, 1, L"camera.viewfinder", 0xE722 },
    { L"foco", L"Foco", L"Foco", 2, 1, L"moon.fill", 0xE708 },
    { L"tela", L"Tela", L"Tela e Brilho", 4, 1, L"sun.max", 0xE706 },
    { L"som", L"Som", L"Sons", 4, 1, L"speaker.wave.3.fill", 0xE767 },
    // os que vêm da galeria (círculos)
    { L"calculadora", L"Calculadora", L"Utilitários", 1, 1, L"plus.forwardslash.minus", 0xE8EF },
    { L"bloquear", L"Bloquear Tela", L"Tela Bloqueada", 1, 1, L"lock.fill", 0xE72E },
    { L"mesa", L"Mostrar Mesa", L"Mesa e Finder", 1, 1, L"menubar.dock.rectangle", 0xE7F4 },
    { L"launchpad", L"Apps", L"Mesa e Finder", 1, 1, L"square.grid.3x3.fill", 0xF0E2 },
    { L"spotlight", L"Spotlight", L"Mesa e Finder", 1, 1, L"magnifyingglass", 0xE721 },
    { L"monitor", L"Monitor de Atividade", L"Utilitários", 1, 1, L"waveform.path.ecg", 0xE9D9 },
    { L"terminal", L"Terminal", L"Utilitários", 1, 1, L"terminal", 0xE756 },
    { L"lupa", L"Lupa", L"Acessibilidade", 1, 1, L"plus.magnifyingglass", 0xE71E },
    { L"teclado", L"Teclado na Tela", L"Teclados", 1, 1, L"keyboard", 0xE765 },
    { L"alarme", L"Alarme", L"Relógio", 1, 1, L"alarm", 0xEA8F },
    { L"cronometro", L"Cronômetro", L"Relógio", 1, 1, L"stopwatch", 0xE916 },
    { L"temporizador", L"Temporizador", L"Relógio", 1, 1, L"timer", 0xE823 },
    { L"luznoturna", L"Night Shift", L"Tela e Brilho", 1, 1, L"sun.max", 0xE706 },
    { L"bateria", L"Modo de Baixo Consumo", L"Bateria", 1, 1, L"battery.25", 0xE850 },
};
const int NCTLS = (int)(sizeof(CTLS) / sizeof(CTLS[0]));
const Ctl* FindCtl(const std::wstring& id) { for (auto& c : CTLS) if (id == c.id) return &c; return nullptr; }
const wchar_t* DEFAULT_LAYOUT = L"wifi|tocando|bluetooth|airdrop|organizador|espelhar|escuro|captura|foco|tela|som";
// arrastando um controle (editando): a ordem de prévia (os outros abrem espaço) e onde ele está
std::vector<std::wstring> g_preview;
std::wstring g_dragId;
D2D1_POINT_2F g_dragPt{};   // DIP, nas coordenadas da Central
bool g_dragActive;
std::vector<std::wstring> SavedIds();
std::vector<std::wstring> LayoutIds() { return g_preview.empty() ? SavedIds() : g_preview; }
std::vector<std::wstring> SavedIds() {
    std::wstring s = shell::Setting(L"cc_layout", DEFAULT_LAYOUT);
    std::vector<std::wstring> v;
    size_t a = 0;
    while (a <= s.size()) {
        size_t b = s.find(L'|', a); if (b == std::wstring::npos) b = s.size();
        std::wstring id = s.substr(a, b - a);
        if (FindCtl(id) && std::find(v.begin(), v.end(), id) == v.end()) v.push_back(id);
        a = b + 1;
    }
    return v;
}
void SaveLayout(const std::vector<std::wstring>& v) {
    std::wstring s;
    for (auto& x : v) { if (!s.empty()) s += L'|'; s += x; }
    shell::SetSetting(L"cc_layout", s);
}
bool g_editing;  // "Editar Controles" (Tahoe)
struct Slot { const Ctl* c; D2D1_RECT_F r; int idx; };
const float T_CELL = (T_W - 2 * T_M - 3 * T_G) / 4;  // 61: uma coluna da grade
// posições (DIP) de cada controle, em ordem; *height = altura até o fim do último
std::vector<Slot> LayoutCCFor(const std::vector<std::wstring>& ids, float* height);
std::vector<Slot> LayoutCC(float* height) { return LayoutCCFor(LayoutIds(), height); }
std::vector<Slot> LayoutCCFor(const std::vector<std::wstring>& ids, float* height) {
    std::vector<std::array<bool, 4>> occ;
    std::vector<bool> barRow;
    struct P { const Ctl* c; int row, col, idx; };
    std::vector<P> ps;
    for (int i = 0; i < (int)ids.size(); i++) {
        const Ctl* c = FindCtl(ids[i]);
        for (int row = 0;; row++) {
            bool placed = false;
            for (int col = 0; col + c->w <= 4 && !placed; col++) {
                bool free = true;
                for (int r = row; r < row + c->h && free; r++)
                    for (int k = col; k < col + c->w && free; k++)
                        if (r < (int)occ.size() && occ[r][k]) free = false;
                if (!free) continue;
                while ((int)occ.size() < row + c->h) { occ.push_back({ false, false, false, false }); barRow.push_back(false); }
                for (int r = row; r < row + c->h; r++) { for (int k = col; k < col + c->w; k++) occ[r][k] = true; if (c->w == 4) barRow[r] = true; }
                ps.push_back({ c, row, col, i });
                placed = true;
            }
            if (placed) break;
        }
    }
    std::vector<float> ry(occ.size() + 1);
    float y = T_M;
    for (size_t r = 0; r < occ.size(); r++) { ry[r] = y; y += (barRow[r] ? T_BAR : T_CAP) + T_G; }
    ry[occ.size()] = y;
    std::vector<Slot> out;
    for (auto& p : ps) {
        float x = T_M + p.col * (T_CELL + T_G), w = p.c->w * T_CELL + (p.c->w - 1) * T_G;
        float top = ry[p.row], bot = ry[p.row + p.c->h] - T_G;
        out.push_back({ p.c, D2D1::RectF(x, top, x + w, bot), p.idx });
    }
    if (height) *height = occ.empty() ? T_M : y - T_G;
    return out;
}
float DetailH();
extern int g_detail;
const int EDIT_ROWS = 2;  // editando: duas fileiras de lugares vazios embaixo, para os controles novos
float PanelW() { return Tahoe() ? T_W : PW; }
float PanelH() {
    if (Tahoe()) {
        float h; LayoutCC(&h);
        float base = h + (g_editing ? EDIT_ROWS * (T_CAP + T_G) : 0) + T_G + T_EDIT + T_M;
        return g_detail >= 0 ? std::max(base, T_M + DetailH() + T_M) : base;  // módulo expandido: cresce se precisar
    }
    return M + CONN_H + G + BLOCK + G + BLOCK + G + BLOCK + G + EDIT + M;
}

// Vidro por módulo: um recorte de desfoque (HostBackdropBrush) por peça, num alvo da Composição por cima
// do vidro único do backdrop (que no Tahoe fica com tamanho zero). Mesmo esquema da notificacoes.cpp.
namespace tglass {
namespace wuc = winrt::Windows::UI::Composition;
wuc::Compositor comp{ nullptr };
wuc::Desktop::DesktopWindowTarget target{ nullptr };
wuc::ContainerVisual root{ nullptr };
wuc::CompositionBrush blur{ nullptr };
struct Piece { wuc::ContainerVisual c{ nullptr }; wuc::CompositionRoundedRectangleGeometry geo{ nullptr }; wuc::CompositionColorBrush tint{ nullptr }; };
std::vector<Piece> pool;
size_t used;
bool ok;

bool Init(HWND h) {
    if (ok) return true;
    try {
        comp = backdrop::Compositor();  // o mesmo compositor do backdrop: as peças usam o mesmo fosco do Dock
        if (!comp) return false;
        BOOL on = TRUE;
        DwmSetWindowAttribute(h, 17 /*DWMWA_USE_HOSTBACKDROPBRUSH*/, &on, sizeof(on));
        auto interop = comp.as<ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop>();
        // topmost: o alvo de baixo da janela já é o do backdrop
        winrt::check_hresult(interop->CreateDesktopWindowTarget(h, TRUE,
            reinterpret_cast<ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget**>(winrt::put_abi(target))));
        root = comp.CreateContainerVisual();
        root.RelativeSizeAdjustment({ 1, 1 });
        target.Root(root);
        ok = true;
    } catch (...) { ok = false; }
    return ok;
}
void Begin() { used = 0; }
void Rect(D2D1_RECT_F r, float rad, D2D1_COLOR_F t) {  // px
    if (!ok) return;
    if (used == pool.size()) {
        Piece p;
        p.c = comp.CreateContainerVisual();
        auto b = comp.CreateSpriteVisual(); b.RelativeSizeAdjustment({ 1, 1 }); b.Brush(backdrop::Frost());
        auto tv = comp.CreateSpriteVisual(); tv.RelativeSizeAdjustment({ 1, 1 });
        p.tint = comp.CreateColorBrush(); tv.Brush(p.tint);
        p.c.Children().InsertAtTop(b); p.c.Children().InsertAtTop(tv);
        p.geo = comp.CreateRoundedRectangleGeometry();
        p.c.Clip(comp.CreateGeometricClip(p.geo));
        root.Children().InsertAtTop(p.c);
        pool.push_back(p);
    }
    Piece& p = pool[used++];
    p.c.IsVisible(true);
    p.c.Offset({ r.left, r.top, 0 });
    p.c.Size({ r.right - r.left, r.bottom - r.top });
    p.geo.Size({ r.right - r.left, r.bottom - r.top });
    p.geo.CornerRadius({ rad, rad });
    auto c8 = [](float v) { return (uint8_t)std::lround(std::clamp(v, 0.f, 1.f) * 255); };
    p.tint.Color(winrt::Windows::UI::Color{ c8(t.a), c8(t.r), c8(t.g), c8(t.b) });
}
void End(float opacity, bool visible) {
    if (!ok) return;
    for (size_t i = used; i < pool.size(); i++) pool[i].c.IsVisible(false);
    root.Opacity(opacity);
    root.IsVisible(visible);
}
}  // namespace tglass

// Véu do Tahoe (vídeo do macOS 26): com a Central aberta, a região em volta dela embaça e escurece um pouco,
// forte no canto de cima à direita e sumindo aos poucos para a esquerda e para baixo. Uma janela que não
// recebe cliques, atrás da Central, com o fosco do Shell recortado por um degradê radial (máscara).
namespace veil {
namespace wuc = winrt::Windows::UI::Composition;
HWND w;
wuc::Desktop::DesktopWindowTarget target{ nullptr };
wuc::ContainerVisual root{ nullptr };
bool ok;

const UINT WM_VEIL_CLICK = WM_APP + 72;
HWND owner;
LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN) { PostMessageW(owner, WM_VEIL_CLICK, 0, 0); return 0; }  // clicar no véu fecha a Central
    return DefWindowProcW(h, m, wp, lp);
}

void Create(HINSTANCE hi) {
    WNDCLASSW c{}; c.lpfnWndProc = Proc; c.hInstance = hi; c.lpszClassName = L"ShellCentralVeu";
    RegisterClassW(&c);
    // sem WS_EX_LAYERED: janela em camadas não mostra o conteúdo da Composição
    w = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
                        c.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
    if (!w) return;
    owner = g_wnd;
    DWORD corner = 1; DwmSetWindowAttribute(w, 33, &corner, sizeof(corner));
    COLORREF none = 0xFFFFFFFE; DwmSetWindowAttribute(w, 34, &none, sizeof(none));
    try {
        auto comp = backdrop::Compositor();
        if (!comp) return;
        BOOL on = TRUE;
        DwmSetWindowAttribute(w, 17 /*DWMWA_USE_HOSTBACKDROPBRUSH*/, &on, sizeof(on));
        auto interop = comp.as<ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop>();
        winrt::check_hresult(interop->CreateDesktopWindowTarget(w, FALSE,
            reinterpret_cast<ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget**>(winrt::put_abi(target))));
        root = comp.CreateContainerVisual();
        root.RelativeSizeAdjustment({ 1, 1 });
        auto mask = [&](float a0, float a1, float a2) {  // degradê radial a partir do canto de cima à direita
            auto rg = comp.CreateRadialGradientBrush();
            rg.EllipseCenter({ 1, 0 });
            rg.EllipseRadius({ 1, 1 });
            rg.ColorStops().Append(comp.CreateColorGradientStop(0, { (uint8_t)(a0 * 255), 255, 255, 255 }));
            rg.ColorStops().Append(comp.CreateColorGradientStop(0.5f, { (uint8_t)(a1 * 255), 255, 255, 255 }));
            rg.ColorStops().Append(comp.CreateColorGradientStop(1, { (uint8_t)(a2 * 255), 255, 255, 255 }));
            return rg;
        };
        auto blur = comp.CreateSpriteVisual();
        blur.RelativeSizeAdjustment({ 1, 1 });
        auto mb = comp.CreateMaskBrush();
        mb.Source(backdrop::Frost());
        mb.Mask(mask(1, 0.9f, 0));
        blur.Brush(mb);
        auto dim = comp.CreateSpriteVisual();
        dim.RelativeSizeAdjustment({ 1, 1 });
        auto mb2 = comp.CreateMaskBrush();
        mb2.Source(comp.CreateColorBrush({ 0, 0, 0, 0 }));  // sem escurecer: no Tahoe é só o desfoque (antes um azul escuro em cima)
        mb2.Mask(mask(1, 0.7f, 0));
        dim.Brush(mb2);
        root.Children().InsertAtTop(blur);
        root.Children().InsertAtTop(dim);
        target.Root(root);
        ok = true;
    } catch (winrt::hresult_error const& e) {
        Log(L"central: véu indisponível (0x%08X %ls)", (unsigned)e.code(), e.message().c_str());
    } catch (...) {}
}

void Show(RECT cc, HWND below) {
    if (!ok) return;
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint({ cc.right - 1, cc.top }, MONITOR_DEFAULTTOPRIMARY), &mi);
    int ww = cc.right - cc.left, hh = cc.bottom - cc.top;
    int top = mi.rcMonitor.top + (int)std::lround(menubar::Height() * GetDpiForWindow(w) / 96.0);  // não cobre a barra de menus
    RECT r{ std::max(mi.rcMonitor.left, cc.left - ww * 3 / 2), top, mi.rcMonitor.right, std::min(mi.rcMonitor.bottom, cc.bottom + hh * 2 / 3) };
    SetWindowPos(w, below, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}
void Opacity(float o) { if (ok) try { root.Opacity(o); } catch (...) {} }
void Hide() { if (w) ShowWindow(w, SW_HIDE); }
}  // namespace veil

// ================= leituras do Windows
const wchar_t* PUSH = L"Software\\Microsoft\\Windows\\CurrentVersion\\PushNotifications";
const wchar_t* CDP = L"Software\\Microsoft\\Windows\\CurrentVersion\\CDP";

DWORD RegDw(const wchar_t* key, const wchar_t* name, DWORD def) {
    DWORD v = def, cb = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_DWORD, nullptr, &v, &cb);
    return v;
}

std::wstring Ssid() {
    HANDLE h = nullptr; DWORD ver = 0;
    if (WlanOpenHandle(2, nullptr, &ver, &h) != ERROR_SUCCESS) return L"";
    std::wstring r;
    PWLAN_INTERFACE_INFO_LIST list = nullptr;
    if (WlanEnumInterfaces(h, nullptr, &list) == ERROR_SUCCESS && list) {
        for (DWORD i = 0; i < list->dwNumberOfItems && r.empty(); i++) {
            if (list->InterfaceInfo[i].isState != wlan_interface_state_connected) continue;
            DWORD sz = 0; PWLAN_CONNECTION_ATTRIBUTES ca = nullptr; WLAN_OPCODE_VALUE_TYPE t;
            if (WlanQueryInterface(h, &list->InterfaceInfo[i].InterfaceGuid, wlan_intf_opcode_current_connection, nullptr, &sz, (PVOID*)&ca, &t) == ERROR_SUCCESS && ca) {
                auto& s = ca->wlanAssociationAttributes.dot11Ssid;
                std::string a((const char*)s.ucSSID, s.uSSIDLength);
                int n = MultiByteToWideChar(CP_UTF8, 0, a.c_str(), (int)a.size(), nullptr, 0);
                r.resize(n);
                MultiByteToWideChar(CP_UTF8, 0, a.c_str(), (int)a.size(), r.data(), n);
                WlanFreeMemory(ca);
            }
        }
        WlanFreeMemory(list);
    }
    WlanCloseHandle(h, nullptr);
    return r;
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

// troca a saída de som padrão (a mesma interface que o Windows usa nos ajustes de som)
MIDL_INTERFACE("f8679f50-850a-41cf-9c72-430f290290c8") IPolicyConfig : public IUnknown {
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
const CLSID CLSID_PolicyConfigClient = { 0x870af99c, 0x171d, 0x4f9e, { 0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9 } };

struct Out { std::wstring id, name; bool def; };
std::vector<Out> Outputs() {
    std::vector<Out> r;
    IMMDeviceEnumerator* en = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return r;
    std::wstring def;
    IMMDevice* d = nullptr;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &d))) { LPWSTR id = nullptr; d->GetId(&id); if (id) { def = id; CoTaskMemFree(id); } d->Release(); }
    IMMDeviceCollection* col = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) {
        UINT n = 0; col->GetCount(&n);
        for (UINT i = 0; i < n; i++) {
            IMMDevice* x = nullptr;
            if (FAILED(col->Item(i, &x))) continue;
            LPWSTR id = nullptr; x->GetId(&id);
            IPropertyStore* ps = nullptr;
            std::wstring name;
            if (SUCCEEDED(x->OpenPropertyStore(STGM_READ, &ps))) {
                PROPVARIANT pv; PropVariantInit(&pv);
                if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) name = pv.pwszVal;
                PropVariantClear(&pv); ps->Release();
            }
            if (id) { r.push_back({ id, name, def == id }); CoTaskMemFree(id); }
            x->Release();
        }
        col->Release();
    }
    en->Release();
    return r;
}

template <class F> void WithMonitor(F f) {
    HMONITOR mon = MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    DWORD n = 0;
    if (!GetNumberOfPhysicalMonitorsFromHMONITOR(mon, &n) || !n) return;
    std::vector<PHYSICAL_MONITOR> pm(n);
    if (!GetPhysicalMonitorsFromHMONITOR(mon, n, pm.data())) return;
    f(pm[0].hPhysicalMonitor);
    DestroyPhysicalMonitors(n, pm.data());
}

// brilho (DDC/CI é lento): gravado numa thread que sempre usa o último valor pedido
std::atomic<float> g_brightWant{ -1 };
std::atomic<bool> g_brightBusy{ false };
void BrightWorker() {
    float v;
    while ((v = g_brightWant.exchange(-1)) >= 0)
        WithMonitor([&](HANDLE h) {
            DWORD mn = 0, cur = 0, mx = 100;
            if (GetMonitorBrightness(h, &mn, &cur, &mx)) SetMonitorBrightness(h, mn + (DWORD)std::lround(v * (mx - mn)));
        });
    g_brightBusy = false;
}

void ReadFast() {  // leituras rápidas: ao abrir e a cada atualização
    std::wstring ssid = Ssid();
    float vol = 0; BOOL mute = FALSE;
    if (IAudioEndpointVolume* v = Endpoint()) { v->GetMasterVolumeLevelScalar(&vol); v->GetMute(&mute); v->Release(); }
    bool focus = RegDw(PUSH, L"ToastEnabled", 1) == 0;
    int air = (int)RegDw(CDP, L"NearShareChannelUserAuthzPolicy", 0);
    std::lock_guard<std::mutex> l(g_mx);
    g_st.ssid = ssid; g_st.volume = vol; g_st.mute = mute != FALSE; g_st.focus = focus; g_st.airdrop = air;
}

// Wi-Fi/Bluetooth ligados ou não (Windows.Devices.Radios), numa thread própria
void LoadRadios() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    try {
        wdr::Radio::RequestAccessAsync().get();
        for (auto const& r : wdr::Radio::GetRadiosAsync().get()) {
            int k = r.Kind() == wdr::RadioKind::WiFi ? 0 : r.Kind() == wdr::RadioKind::Bluetooth ? 1 : -1;
            if (k < 0 || g_radio[k]) continue;
            g_radio[k] = r;
            { std::lock_guard<std::mutex> l(g_mx); g_st.radio[k] = r.State() == wdr::RadioState::On ? 1 : 0; }
            r.StateChanged([k](wdr::Radio const& s, auto&&) {
                { std::lock_guard<std::mutex> l(g_mx); g_st.radio[k] = s.State() == wdr::RadioState::On ? 1 : 0; }
                if (g_wnd) PostMessageW(g_wnd, WM_CC_UPDATE, 0, 0);
            });
        }
    } catch (...) {}
    std::lock_guard<std::mutex> l(g_mx);
    for (int k = 0; k < 2; k++) if (!g_radio[k]) g_st.radio[k] = -1;
}

void SetRadio(int k, bool on) {
    { std::lock_guard<std::mutex> l(g_mx); g_st.radio[k] = on ? 1 : 0; }
    std::thread([k, on] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        if (!g_radio[k]) return;
        try { g_radio[k].SetStateAsync(on ? wdr::RadioState::On : wdr::RadioState::Off).get(); } catch (...) {}
        if (g_wnd) PostMessageW(g_wnd, WM_CC_UPDATE, 0, 0);
    }).detach();
}

// O que está tocando (o mesmo que o Windows mostra no controle de mídia), com a capa
void ReadMedia() {
    if (g_mediaBusy.exchange(true)) return;
    std::thread([] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        try {
            auto mgr = wmc::GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
            auto s = mgr.GetCurrentSession();
            if (!s) {
                std::lock_guard<std::mutex> l(g_mx);
                g_st.hasMedia = false; g_st.title.clear(); g_st.artist.clear();
            } else {
                auto p = s.TryGetMediaPropertiesAsync().get();
                bool playing = s.GetPlaybackInfo().PlaybackStatus() == wmc::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
                std::wstring title = p.Title().c_str(), artist = p.Artist().c_str();
                bool changed;
                { std::lock_guard<std::mutex> l(g_mx); changed = title != g_st.title || !g_st.hasMedia; }
                std::vector<BYTE> px; UINT w = 0, h = 0;
                if (changed && p.Thumbnail()) {
                    auto stream = p.Thumbnail().OpenReadAsync().get();
                    IStream* is = nullptr;
                    if (SUCCEEDED(CreateStreamOverRandomAccessStream(winrt::get_unknown(stream), IID_PPV_ARGS(&is)))) {
                        IWICImagingFactory* f = nullptr;
                        if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) {
                            IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* cv = nullptr;
                            if (SUCCEEDED(f->CreateDecoderFromStream(is, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr)) &&
                                SUCCEEDED(f->CreateFormatConverter(&cv)) &&
                                SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
                                cv->GetSize(&w, &h);
                                px.resize((size_t)w * h * 4);
                                if (FAILED(cv->CopyPixels(nullptr, w * 4, (UINT)px.size(), px.data()))) px.clear();
                            }
                            if (cv) cv->Release(); if (fr) fr->Release(); if (dec) dec->Release();
                            f->Release();
                        }
                        is->Release();
                    }
                }
                std::lock_guard<std::mutex> l(g_mx);
                g_st.hasMedia = true; g_st.playing = playing; g_st.title = title; g_st.artist = artist;
                if (changed) { g_st.art = std::move(px); g_st.artW = w; g_st.artH = h; g_st.artNew = true; }
            }
        } catch (...) {}
        g_mediaBusy = false;
        if (g_wnd) PostMessageW(g_wnd, WM_CC_UPDATE, 0, 0);
    }).detach();
}

void MediaCommand(bool next) {
    std::thread([next] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        try {
            auto s = wmc::GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get().GetCurrentSession();
            if (s) { if (next) s.TrySkipNextAsync().get(); else s.TryTogglePlayPauseAsync().get(); }
        } catch (...) {}
        Sleep(250);
        ReadMedia();
    }).detach();
}

void Key(WORD vk, bool up) {
    INPUT in{}; in.type = INPUT_KEYBOARD; in.ki.wVk = vk; in.ki.dwExtraInfo = KEY_MARK; in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
    SendInput(1, &in, sizeof(in));
}

// ================= desenho
struct Pal { D2D1_COLOR_F tint, tile, tileLine, text, text2, off, offGlyph, track, fill, knobIcon; };
Pal Colors() {
    // Leopard: o branco dos menus do 10.5 (sempre claro), texto preto em Lucida, grupos em caixas cinza
    if (liquid::Leopard())
        return { D2D1::ColorF(1, 1, 1, 0.96f), D2D1::ColorF(0, 0, 0, 0.045f), D2D1::ColorF(0, 0, 0, 0.16f), D2D1::ColorF(0, 0, 0, 0.92f),
                 D2D1::ColorF(0, 0, 0, 0.55f), D2D1::ColorF(1, 1, 1, 1), D2D1::ColorF(0.25f, 0.25f, 0.27f, 1), D2D1::ColorF(0, 0, 0, 0.2f),
                 D2D1::ColorF(0.24f, 0.47f, 0.87f, 1), D2D1::ColorF(0, 0, 0, 0.6f) };
    if (ui::Dark())
        return { D2D1::ColorF(0.11f, 0.11f, 0.12f, 0.42f), D2D1::ColorF(1, 1, 1, 0.09f), D2D1::ColorF(1, 1, 1, 0.10f), D2D1::ColorF(1, 1, 1, 0.94f),
                 D2D1::ColorF(1, 1, 1, 0.55f), D2D1::ColorF(1, 1, 1, 0.16f), D2D1::ColorF(1, 1, 1, 0.9f), D2D1::ColorF(1, 1, 1, 0.14f),
                 D2D1::ColorF(1, 1, 1, 0.95f), D2D1::ColorF(0, 0, 0, 0.55f) };
    return { D2D1::ColorF(0.96f, 0.96f, 0.97f, 0.42f), D2D1::ColorF(1, 1, 1, 0.55f), D2D1::ColorF(0, 0, 0, 0.06f), D2D1::ColorF(0, 0, 0, 0.88f),
             D2D1::ColorF(0, 0, 0, 0.50f), D2D1::ColorF(0, 0, 0, 0.10f), D2D1::ColorF(0, 0, 0, 0.75f), D2D1::ColorF(0, 0, 0, 0.09f),
             D2D1::ColorF(1, 1, 1, 1), D2D1::ColorF(0, 0, 0, 0.55f) };
}

void Text(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const std::wstring& s, D2D1_RECT_F r, IDWriteTextFormat* tf, D2D1_COLOR_F c) {
    br->SetColor(c);
    dc->DrawTextW(s.c_str(), (UINT32)s.size(), tf, r, br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void Sym(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const wchar_t* name, wchar_t fallback, D2D1_RECT_F r, D2D1_COLOR_F c) {
    br->SetColor(c);
    if (simbolo::Draw(dc, name, r, br)) return;
    float s = std::max(r.right - r.left, r.bottom - r.top) + 6;
    float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    wchar_t g = fallback;
    dc->DrawTextW(&g, 1, g_tfGlyph, D2D1::RectF(cx - s / 2, cy - s / 2, cx + s / 2, cy + s / 2), br);
}

float PanelR() { return liquid::On() ? 26.f : liquid::Leopard() ? 6.f : RADIUS; }  // Tahoe: painel bem mais redondo; Leopard: o canto do menu

// ---- Mac OS X Leopard: peças do Aqua desenhadas aqui (o 10.5 não tinha Central de Controle; ela vira um menu da
// barra no estilo da época: fundo branco de menu, caixas de grupo cinza, botões redondos em gel e controles
// deslizantes com a trilha azul)
void LeoGrad(ID2D1DeviceContext* dc, D2D1_RECT_F r, float rad, const D2D1_GRADIENT_STOP* st, UINT n, bool ellipse = false) {
    ID2D1GradientStopCollection* gs = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
    if (SUCCEEDED(dc->CreateGradientStopCollection(st, n, &gs)) &&
        SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, r.top }, { 0, r.bottom }), gs, &lb))) {
        if (ellipse) dc->FillEllipse(D2D1::Ellipse({ (r.left + r.right) / 2, (r.top + r.bottom) / 2 }, (r.right - r.left) / 2, (r.bottom - r.top) / 2), lb);
        else dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), lb);
    }
    if (lb) lb->Release();
    if (gs) gs->Release();
}
// gel do Aqua: azul (ligado) ou perolado (desligado), com o brilho forte na metade de cima
void LeoGel(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, D2D1_RECT_F r, float rad, bool blue, bool ellipse = false) {
    D2D1_GRADIENT_STOP st[4] = { { 0, blue ? D2D1::ColorF(0.66f, 0.80f, 0.98f) : D2D1::ColorF(1, 1, 1) },
                                 { 0.5f, blue ? D2D1::ColorF(0.38f, 0.60f, 0.93f) : D2D1::ColorF(0.94f, 0.94f, 0.94f) },
                                 { 0.5f, blue ? D2D1::ColorF(0.22f, 0.45f, 0.86f) : D2D1::ColorF(0.86f, 0.86f, 0.86f) },
                                 { 1, blue ? D2D1::ColorF(0.56f, 0.80f, 1.0f) : D2D1::ColorF(0.98f, 0.98f, 0.98f) } };
    br->SetColor(D2D1::ColorF(0, 0, 0, 0.10f));  // a sombrinha embaixo
    if (ellipse) dc->FillEllipse(D2D1::Ellipse({ (r.left + r.right) / 2, (r.top + r.bottom) / 2 + 1 }, (r.right - r.left) / 2, (r.bottom - r.top) / 2), br);
    else dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left, r.top + 1, r.right, r.bottom + 1), rad, rad), br);
    LeoGrad(dc, r, rad, st, 4, ellipse);
    br->SetColor(blue ? D2D1::ColorF(0.12f, 0.27f, 0.58f, 0.9f) : D2D1::ColorF(0, 0, 0, 0.40f));
    D2D1_RECT_F e = D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f);
    if (ellipse) dc->DrawEllipse(D2D1::Ellipse({ (e.left + e.right) / 2, (e.top + e.bottom) / 2 }, (e.right - e.left) / 2, (e.bottom - e.top) / 2), br, 1);
    else dc->DrawRoundedRectangle(D2D1::RoundedRect(e, rad - 0.5f, rad - 0.5f), br, 1);
}
IDWriteTextFormat* LeoFmt(float size, bool bold, bool center = false) {
    auto* f = ui::FormatFace(L"Lucida Sans Unicode", size, bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL);
    if (f) {
        f->SetWordWrapping(center ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        if (center) f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        DWRITE_TRIMMING t{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        IDWriteInlineObject* dots = nullptr; ui::dw->CreateEllipsisTrimmingSign(f, &dots);
        f->SetTrimming(&t, dots);
        if (dots) dots->Release();
    }
    return f;
}

void Tile(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const Pal& p, D2D1_RECT_F r) {
    if (liquid::On()) {  // Tahoe: módulos em cápsulas de vidro (meia altura até 24 pt)
        float h = r.bottom - r.top, w = r.right - r.left, rad = std::min(h / 2, 24.f);
        // macOS 27 (captura 2x do 512 Pixels): mesma grade do Tahoe, cantos maiores nos botões (~27) e no Tocando
        // Agora (~28), menores nas faixas de Tela e Som (~18)
        if (liquid::Golden()) rad = w > 200 ? 18.f : h > 100 ? 28.f : std::min(h / 2, 27.f);
        liquid::Tile(dc, r, rad, ui::Dark());
        return;
    }
    if (liquid::Leopard()) {  // caixa de grupo do 10.5: cinza bem claro afundado, fio mais escuro em cima
        br->SetColor(p.tile);
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, 5, 5), br);
        br->SetColor(D2D1::ColorF(0, 0, 0, 0.13f));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), 4.5f, 4.5f), br, 1);
        br->SetColor(D2D1::ColorF(0, 0, 0, 0.07f));
        dc->DrawLine({ r.left + 5, r.top + 1.5f }, { r.right - 5, r.top + 1.5f }, br, 1);
        return;
    }
    br->SetColor(p.tile);
    dc->FillRoundedRectangle(D2D1::RoundedRect(r, TILE_R, TILE_R), br);
    br->SetColor(p.tileLine);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), TILE_R, TILE_R), br, 1);
}

// círculo de liga/desliga (azul quando ligado) com o símbolo branco
void Circle(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const Pal& p, D2D1_POINT_2F c, float r, bool on, const wchar_t* sym, wchar_t fb, D2D1_COLOR_F onColor) {
    if (liquid::Leopard()) {  // botão redondo de gel: azul ligado, perolado desligado
        LeoGel(dc, br, D2D1::RectF(c.x - r, c.y - r, c.x + r, c.y + r), r, on, true);
        float s = r * 0.58f;
        Sym(dc, br, sym, fb, D2D1::RectF(c.x - s, c.y - s, c.x + s, c.y + s), on ? D2D1::ColorF(1, 1, 1, 1) : p.offGlyph);
        return;
    }
    if (liquid::On() && !on) liquid::Tile(dc, D2D1::RectF(c.x - r, c.y - r, c.x + r, c.y + r), r, ui::Dark(), 1.3f);  // Tahoe: círculo de vidro
    else if (liquid::On()) {  // Tahoe ligado: círculo branco com o símbolo na cor do controle
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.97f));
        dc->FillEllipse(D2D1::Ellipse(c, r, r), br);
        float s = r * 0.62f;
        Sym(dc, br, sym, fb, D2D1::RectF(c.x - s, c.y - s, c.x + s, c.y + s), onColor);
        return;
    } else {
        br->SetColor(on ? onColor : p.off);
        dc->FillEllipse(D2D1::Ellipse(c, r, r), br);
        if (liquid::On()) liquid::Rim(dc, D2D1::RectF(c.x - r, c.y - r, c.x + r, c.y + r), r, ui::Dark(), 0.6f);
    }
    float s = r * 0.62f;
    Sym(dc, br, sym, fb, D2D1::RectF(c.x - s, c.y - s, c.x + s, c.y + s), on ? D2D1::ColorF(1, 1, 1, 1) : p.offGlyph);
}

// controle deslizante grosso do Sequoia: trilho translúcido, parte cheia branca e botão redondo na ponta
void Slider(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const Pal& p, D2D1_RECT_F t, float v, const wchar_t* sym, wchar_t fb) {
    float h = t.bottom - t.top, rad = h / 2;
    if (liquid::Leopard()) {
        // controle deslizante do 10.5: símbolo pequeno à esquerda, trilha fina afundada com a parte cheia em gel azul
        // e o botão redondo perolado
        float cy = (t.top + t.bottom) / 2, s = 7;
        Sym(dc, br, sym, fb, D2D1::RectF(t.left, cy - s, t.left + 2 * s, cy + s), p.text2);
        D2D1_RECT_F tr = D2D1::RectF(t.left + 2 * s + 10, cy - 3, t.right - 4, cy + 3);
        D2D1_GRADIENT_STOP g0[2] = { { 0, D2D1::ColorF(0.62f, 0.62f, 0.64f) }, { 1, D2D1::ColorF(0.86f, 0.86f, 0.87f) } };
        LeoGrad(dc, tr, 3, g0, 2);
        float x = tr.left + std::clamp(v, 0.f, 1.f) * (tr.right - tr.left);
        if (x > tr.left + 1) {
            D2D1_GRADIENT_STOP g1[3] = { { 0, D2D1::ColorF(0.30f, 0.52f, 0.90f) }, { 0.5f, D2D1::ColorF(0.42f, 0.66f, 0.96f) }, { 1, D2D1::ColorF(0.62f, 0.82f, 1.0f) } };
            LeoGrad(dc, D2D1::RectF(tr.left, tr.top, x, tr.bottom), 3, g1, 3);
        }
        br->SetColor(D2D1::ColorF(0, 0, 0, 0.35f));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tr.left + 0.5f, tr.top + 0.5f, tr.right - 0.5f, tr.bottom - 0.5f), 2.5f, 2.5f), br, 1);
        LeoGel(dc, br, D2D1::RectF(x - 8, cy - 8, x + 8, cy + 8), 8, false, true);
        return;
    }
    if (liquid::On()) {  // Tahoe: trilho fino (4 pt) com a parte cheia branca, símbolo à esquerda, sem botão grande
        // guia macOS26-Theme-Reference (seções 21 e 40, medido): trilho de 219 no meio do módulo, ícones de 16 nas
        // duas pontas, trilho #E8E8E8 (claro) / #232323 (escuro) e a parte cheia branca
        float cy = (t.top + t.bottom) / 2, s = 8, mid = (t.left + t.right) / 2, tw = std::min(219.f, t.right - t.left - 2 * (2 * s + 10));
        D2D1_RECT_F tr = D2D1::RectF(mid - tw / 2, cy - 2, mid + tw / 2, cy + 2);
        Sym(dc, br, sym, fb, D2D1::RectF(tr.left - 10 - 2 * s, cy - s, tr.left - 10, cy + s), p.text);
        Sym(dc, br, sym, fb, D2D1::RectF(tr.right + 10, cy - s, tr.right + 10 + 2 * s, cy + s), p.text);
        br->SetColor(ui::Dark() ? D2D1::ColorF(0x23 / 255.f, 0x23 / 255.f, 0x23 / 255.f, 1) : D2D1::ColorF(0xE8 / 255.f, 0xE8 / 255.f, 0xE8 / 255.f, 1));
        dc->FillRoundedRectangle(D2D1::RoundedRect(tr, 2, 2), br);
        float x = tr.left + std::clamp(v, 0.f, 1.f) * (tr.right - tr.left);
        br->SetColor(D2D1::ColorF(1, 1, 1, 1));
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tr.left, tr.top, std::max(x, tr.left + 4), tr.bottom), 2, 2), br);
        return;
    }

    br->SetColor(p.track);
    dc->FillRoundedRectangle(D2D1::RoundedRect(t, rad, rad), br);
    float x = t.left + h + v * (t.right - t.left - h);  // a parte cheia sempre cobre ao menos o botão
    br->SetColor(p.fill);
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(t.left, t.top, x, t.bottom), rad, rad), br);
    br->SetColor(D2D1::ColorF(0, 0, 0, 0.18f));
    dc->FillEllipse(D2D1::Ellipse({ x - rad, (t.top + t.bottom) / 2 + 0.5f }, rad + 0.6f, rad + 0.6f), br);
    br->SetColor(D2D1::ColorF(1, 1, 1, 1));
    dc->FillEllipse(D2D1::Ellipse({ x - rad, (t.top + t.bottom) / 2 }, rad, rad), br);
    float s = h * 0.30f;
    Sym(dc, br, sym, fb, D2D1::RectF(t.left + h * 0.5f - s, (t.top + t.bottom) / 2 - s, t.left + h * 0.5f + s, (t.top + t.bottom) / 2 + s), p.knobIcon);
}

void PaintTahoe();

bool g_closing;      // fechando animado (a mola comum, de volta)
double g_closeAt;
anim::Panel Motion() {
    if (Tahoe()) return g_open ? anim::CCIn(anim::Now() - g_openAt) : anim::CCOut(anim::Now() - g_closeAt);
    return g_open ? anim::PanelIn(anim::Now() - g_openAt) : anim::PanelOut(anim::Now() - g_closeAt);
}
// o painel cresce a partir do canto de cima, perto do ícone da barra de menus (como no Mac)
void Pivot(int wpx, float& cx, float& cy) { cx = wpx * 0.80f; cy = 0; }

void Paint() {
    if (!g_open && !g_closing) return;
    if (Tahoe()) { PaintTahoe(); return; }
    tglass::End(1, false);
    float sc = Sc();
    int wpx = (int)std::lround(PW * sc), hpx = (int)std::lround(PanelH() * sc);
    auto* dc = ui::GpuBegin(g_s, wpx, hpx, sc);
    if (!dc) return;
    State st;
    { std::lock_guard<std::mutex> l(g_mx); st = g_st; if (g_st.artNew) g_st.artNew = false; }
    if (st.artNew) {
        if (g_art) { g_art->Release(); g_art = nullptr; }
        if (!st.art.empty()) g_art = ui::Bitmap(dc, st.artW, st.artH, st.art);
    }
    Pal p = Colors();
    anim::Panel mo = Motion();
    float t = mo.fade;
    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(p.tint, &br);
    ID2D1Layer* layer = nullptr;
    dc->CreateLayer(nullptr, &layer);
    dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), t), layer);
    const float R = PanelR();
    const bool leo = liquid::Leopard();
    // Leopard: as fontes em Lucida (trocadas só durante este desenho)
    static IDWriteTextFormat *lB = LeoFmt(13, true), *lS = LeoFmt(11, false), *lT = LeoFmt(10, false, true);
    struct Swap { IDWriteTextFormat **a, *b; ~Swap() { if (a) *a = b; } };
    Swap sw1{ leo && lB ? &g_tfBold : nullptr, g_tfBold }, sw2{ leo && lS ? &g_tfSmall : nullptr, g_tfSmall }, sw3{ leo && lT ? &g_tfTile : nullptr, g_tfTile };
    if (leo) {
        if (lB) g_tfBold = lB;
        if (lS) g_tfSmall = lS;
        if (lT) { lT->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR); lT->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM, 12, 9.5f); g_tfTile = lT; }
        if (g_back && IsWindowVisible(g_back)) ShowWindow(g_back, SW_HIDE);  // sem o vidro desfocado
        // o menu do 10.5: preso embaixo da barra (topo reto), cantos de baixo arredondados, branco quase opaco
        ID2D1Factory* fac = nullptr; dc->GetFactory(&fac);
        ID2D1PathGeometry* pg = nullptr;
        float W = PW, H = PanelH();
        if (fac && SUCCEEDED(fac->CreatePathGeometry(&pg))) {
            ID2D1GeometrySink* sk = nullptr;
            if (SUCCEEDED(pg->Open(&sk))) {
                sk->BeginFigure({ 0, 0 }, D2D1_FIGURE_BEGIN_FILLED);
                sk->AddLine({ W, 0 }); sk->AddLine({ W, H - R });
                sk->AddArc(D2D1::ArcSegment({ W - R, H }, { R, R }, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                sk->AddLine({ R, H });
                sk->AddArc(D2D1::ArcSegment({ 0, H - R }, { R, R }, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                sk->EndFigure(D2D1_FIGURE_END_CLOSED); sk->Close(); sk->Release();
                br->SetColor(p.tint);
                dc->FillGeometry(pg, br);
                br->SetColor(D2D1::ColorF(0, 0, 0, 0.28f));
                dc->DrawGeometry(pg, br, 1);
            }
            pg->Release();
        }
        if (fac) fac->Release();
    }
    else if (!g_glass) { dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, PW, PanelH()), R, R), br); }
    if (leo) {}
    else if (liquid::On()) {
        if (!backdrop::Liquid()) liquid::Rim(dc, D2D1::RectF(0, 0, PW, PanelH()), R, ui::Dark());  // com o Liquid Glass o vidro já desenha o contorno
    } else {
    // borda do painel: escura por fora, clara por dentro (como o Dock)
    br->SetColor(ui::Dark() ? D2D1::ColorF(0, 0, 0, 0.45f) : D2D1::ColorF(0, 0, 0, 0.12f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, PW - 0.5f, PanelH() - 0.5f), R, R), br, 1);
    br->SetColor(ui::Dark() ? D2D1::ColorF(1, 1, 1, 0.14f) : D2D1::ColorF(1, 1, 1, 0.5f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(1.5f, 1.5f, PW - 1.5f, PanelH() - 1.5f), R - 1, R - 1), br, 1);
    }
    g_hits.clear();
    const D2D1_COLOR_F blue = D2D1::ColorF(0.04f, 0.52f, 1, 1);

    // conectividade
    D2D1_RECT_F conn = D2D1::RectF(M, M, M + COL, M + CONN_H);
    Tile(dc, br, p, conn);
    struct Row { HitId btn, text; const wchar_t* sym; wchar_t fb; std::wstring title, sub; bool on; };
    std::wstring wifiSub = st.radio[0] == 0 ? Tr(L"Desligado") : !st.ssid.empty() ? st.ssid : st.radio[0] == -1 ? Tr(L"Ethernet") : Tr(L"Não conectado");
    std::wstring btSub = st.radio[1] == 1 ? Tr(L"Ativado") : st.radio[1] == 0 ? Tr(L"Desativado") : st.radio[1] == -1 ? Tr(L"Indisponível") : L"…";
    std::wstring airSub = st.airdrop == 2 ? Tr(L"Todos") : st.airdrop == 1 ? Tr(L"Só Meus Dispositivos") : Tr(L"Desativado");
    Row rows[3] = {
        { H_WIFI_BTN, H_WIFI, L"wifi", 0xE701, L"Wi‑Fi", wifiSub, st.radio[0] == 1 },
        { H_BT_BTN, H_BT, L"", 0xE702, L"Bluetooth", btSub, st.radio[1] == 1 },
        { H_AIRDROP, H_AIRDROP, L"dot.radiowaves.left.and.right", 0xE72D, L"AirDrop", airSub, st.airdrop > 0 },
    };
    for (int i = 0; i < 3; i++) {
        float cy = conn.top + 26 + i * 44;
        D2D1_POINT_2F c{ conn.left + 26, cy };
        Circle(dc, br, p, c, 15, rows[i].on, rows[i].sym, rows[i].fb, blue);
        Text(dc, br, rows[i].title, D2D1::RectF(c.x + 22, cy - 15, conn.right - 6, cy + 1), g_tfBold, p.text);
        Text(dc, br, rows[i].sub, D2D1::RectF(c.x + 22, cy + 1, conn.right - 6, cy + 15), g_tfSmall, p.text2);
        g_hits.push_back({ rows[i].btn, D2D1::RectF(c.x - 16, cy - 16, c.x + 16, cy + 16) });
        g_hits.push_back({ rows[i].text, D2D1::RectF(c.x + 18, cy - 18, conn.right, cy + 18) });
    }
    // Foco
    float x2 = M + COL + G;
    D2D1_RECT_F focus = D2D1::RectF(x2, M, PW - M, M + 65);
    Tile(dc, br, p, focus);
    Circle(dc, br, p, { focus.left + 26, (focus.top + focus.bottom) / 2 }, 15, st.focus, L"moon.fill", 0xE708, D2D1::ColorF(0.37f, 0.36f, 0.90f, 1));
    Text(dc, br, Tr(L"Foco"), D2D1::RectF(focus.left + 48, focus.top, focus.right - 6, focus.bottom), g_tfBold, p.text);
    g_hits.push_back({ H_FOCUS, focus });
    // Organizador Visual (Stage Manager) e Espelhamento de Tela, como no Mac
    float sw = (COL - G) / 2;
    D2D1_RECT_F mc = D2D1::RectF(x2, M + 75, x2 + sw, M + CONN_H), mir = D2D1::RectF(x2 + sw + G, M + 75, PW - M, M + CONN_H);
    for (auto [r, sym, fb, label, id] : { std::tuple{ mc, L"squares.leading.rectangle", (wchar_t)0xE7C4, Tr(L"Organizador\nVisual"), H_STAGE },
                                         std::tuple{ mir, L"rectangle.on.rectangle", (wchar_t)0xEC15, Tr(L"Espelhar\nTela"), H_MIRROR } }) {
        Tile(dc, br, p, r);
        if (id == H_STAGE && palco::On()) {  // ligado: o módulo fica aceso (azul), como os outros ligados
            if (leo) LeoGel(dc, br, r, 5, true);
            else {
            br->SetColor(D2D1::ColorF(0.04f, 0.47f, 1.f, 0.85f));
            dc->FillRoundedRectangle(D2D1::RoundedRect(r, 12, 12), br);
            }
        }
        float cx = (r.left + r.right) / 2;
        Sym(dc, br, sym, fb, D2D1::RectF(cx - 10, r.top + 12, cx + 10, r.top + 28), id == H_STAGE && palco::On() ? D2D1::ColorF(1, 1, 1, 1) : p.text);
        Text(dc, br, label, D2D1::RectF(r.left + 2, r.top + 30, r.right - 2, r.bottom - 4), g_tfTile, p.text);
        g_hits.push_back({ id, r });
    }
    // Tela (brilho)
    float y = M + CONN_H + G;
    D2D1_RECT_F disp = D2D1::RectF(M, y, PW - M, y + BLOCK);
    Tile(dc, br, p, disp);
    Text(dc, br, Tr(L"Tela"), D2D1::RectF(disp.left + 12, disp.top + 6, disp.right - 12, disp.top + 26), g_tfBold, p.text);
    g_hits.push_back({ H_DISPLAY, D2D1::RectF(disp.left, disp.top, disp.right, disp.top + 28) });
    D2D1_RECT_F bt = D2D1::RectF(disp.left + 12, disp.top + 32, disp.right - 12, disp.top + 54);
    Slider(dc, br, p, bt, st.bright >= 0 ? st.bright : 1, L"sun.max", 0xE706);
    if (st.bright >= 0) g_hits.push_back({ H_BRIGHT, bt });
    // Som (volume) + saída de áudio
    y += BLOCK + G;
    D2D1_RECT_F snd = D2D1::RectF(M, y, PW - M, y + BLOCK);
    Tile(dc, br, p, snd);
    Text(dc, br, Tr(L"Som"), D2D1::RectF(snd.left + 12, snd.top + 6, snd.right - 12, snd.top + 26), g_tfBold, p.text);
    g_hits.push_back({ H_SOUND, D2D1::RectF(snd.left, snd.top, snd.right - 50, snd.top + 28) });
    D2D1_RECT_F vt = D2D1::RectF(snd.left + 12, snd.top + 32, snd.right - 50, snd.top + 54);
    Slider(dc, br, p, vt, st.mute ? 0 : st.volume, st.mute || st.volume == 0 ? L"speaker.fill" : L"speaker.wave.2.fill", 0xE767);
    g_hits.push_back({ H_VOLUME, vt });
    D2D1_POINT_2F oc{ snd.right - 26, (vt.top + vt.bottom) / 2 };
    Circle(dc, br, p, oc, 14, false, L"airplay.audio", 0xE7F5, blue);
    g_hits.push_back({ H_OUTPUT, D2D1::RectF(oc.x - 16, oc.y - 16, oc.x + 16, oc.y + 16) });
    // Tocando agora
    y += BLOCK + G;
    D2D1_RECT_F med = D2D1::RectF(M, y, PW - M, y + BLOCK);
    Tile(dc, br, p, med);
    D2D1_RECT_F art = D2D1::RectF(med.left + 10, med.top + 10, med.left + 54, med.top + 54);
    if (g_art && st.hasMedia) {
        ID2D1Factory* f = nullptr; dc->GetFactory(&f);
        ID2D1RoundedRectangleGeometry* rg = nullptr; f->CreateRoundedRectangleGeometry(D2D1::RoundedRect(art, 7, 7), &rg);
        ID2D1Layer* l2 = nullptr; dc->CreateLayer(nullptr, &l2);
        dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), rg), l2);
        auto s = g_art->GetSize();
        float k = std::max(44 / s.width, 44 / s.height);
        float cx = (art.left + art.right) / 2, cy = (art.top + art.bottom) / 2;
        dc->DrawBitmap(g_art, D2D1::RectF(cx - s.width * k / 2, cy - s.height * k / 2, cx + s.width * k / 2, cy + s.height * k / 2), 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        dc->PopLayer();
        l2->Release(); rg->Release(); f->Release();
    } else {
        br->SetColor(p.off);
        dc->FillRoundedRectangle(D2D1::RoundedRect(art, 7, 7), br);
        Sym(dc, br, L"music.note", 0xEC4F, D2D1::RectF(art.left + 13, art.top + 12, art.right - 13, art.bottom - 12), D2D1::ColorF(1.0f, 0.24f, 0.33f, 1));
    }
    std::wstring title = st.hasMedia && !st.title.empty() ? st.title : Tr(L"Nada tocando");
    Text(dc, br, title, D2D1::RectF(art.right + 10, st.hasMedia ? med.top + 14 : med.top, med.right - 70, st.hasMedia ? med.top + 32 : med.bottom), g_tfBold, p.text);
    if (st.hasMedia) Text(dc, br, st.artist, D2D1::RectF(art.right + 10, med.top + 32, med.right - 70, med.top + 48), g_tfSmall, p.text2);
    D2D1_COLOR_F ctl = st.hasMedia ? p.text : p.text2;
    float cy = (med.top + med.bottom) / 2;
    Sym(dc, br, st.playing ? L"pause.fill" : L"play.fill", 0xE768, D2D1::RectF(med.right - 58, cy - 8, med.right - 44, cy + 8), ctl);
    Sym(dc, br, L"forward.fill", 0xE893, D2D1::RectF(med.right - 32, cy - 7, med.right - 12, cy + 7), ctl);
    g_hits.push_back({ H_PLAY, D2D1::RectF(med.right - 64, med.top, med.right - 38, med.bottom) });
    g_hits.push_back({ H_NEXT, D2D1::RectF(med.right - 38, med.top, med.right, med.bottom) });
    // Editar controles
    y += BLOCK + G;
    D2D1_RECT_F ed = D2D1::RectF(M, y, M + EDIT, y + EDIT);
    Tile(dc, br, p, ed);
    Sym(dc, br, L"slider.horizontal.3", 0xE713, D2D1::RectF(ed.left + 11, ed.top + 12, ed.right - 11, ed.bottom - 12), p.text);
    g_hits.push_back({ H_EDIT, ed });

    dc->PopLayer();
    layer->Release();
    br->Release();
    ui::GpuEnd(g_s);
    float pvx, pvy; Pivot(wpx, pvx, pvy);
    ui::GpuScale(g_s, mo.scale, pvx, pvy);
    if (g_glass && !leo) backdrop::Fade(g_back, t, mo.scale, pvx, pvy);
}

void Place() {
    float sc = Sc();
    int wpx = (int)std::lround(PanelW() * sc), hpx = (int)std::lround(PanelH() * sc);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
    int x = mi.rcMonitor.right - wpx - (int)std::lround(8 * sc), y = mi.rcMonitor.top + menubar::Height() + (int)std::lround(6 * sc);
    // Tahoe (guia, seção 40): os módulos começam 12 abaixo da barra e terminam a 14 da borda direita
    if (Tahoe()) { x = mi.rcMonitor.right - wpx - (int)std::lround((14 - T_M) * sc); y = mi.rcMonitor.top + menubar::Height() + (int)std::lround((12 - T_M) * sc); }
    if (liquid::Leopard()) y = mi.rcMonitor.top + menubar::Height();  // Leopard: o menu sai colado na barra
    SetWindowPos(g_wnd, HWND_TOPMOST, x, y, wpx, hpx, SWP_NOACTIVATE);
    SetWindowPos(g_back, g_wnd, x, y, wpx, hpx, SWP_NOACTIVATE | (liquid::Leopard() ? 0 : SWP_SHOWWINDOW));
    // o desfoque por baixo da Central (só o desfoque, embaixo da barra de menus). Desligado por padrão desde
    // 16/11/2026: em cima de alguns fundos (Discord) ele embaçava o vidro dos módulos e deixava um brilho em
    // volta deles; o usuário confirmou que sumiu sem ele. central_veu=1 liga de novo.
    if (Tahoe() && shell::Setting(L"central_veu", L"0") == L"1") { veil::Opacity(0); veil::Show({ x, y, x + wpx, y + hpx }, g_back); }
    else veil::Hide();
    if (g_glass) {
        if (Tahoe()) backdrop::Set(g_back, 0, 0, 0, Colors().tint);  // sem painel: o vidro é por módulo (tglass)
        else backdrop::Set(g_back, (float)wpx, (float)hpx, PanelR() * sc, Colors().tint);
    }
}


// ---- Tahoe: módulo expandido no lugar (guia macOS26-Theme-Reference, seções 47, 53 e 55, medido no macOS 26).
// Clicar no nome de Wi-Fi, Bluetooth, Foco, Tela ou Som faz o bloco crescer até virar o painel inteiro (~0,05 s para
// ocupar a altura, ~0,25 s para assentar). Padrão de todos: título de 16 à esquerda, switch de 54 × 24 à direita,
// linhas de 32 (280 de largura, 14 de margem), link de ajustes de 22 no fim. Clicar fora do painel volta.
enum Det { D_NONE = -1, D_WIFI, D_BT, D_FOCO, D_TELA, D_SOM };
int g_detail = D_NONE;
double g_detailAt;
D2D1_RECT_F g_detailFrom{};
std::vector<std::wstring> g_nets;                 // Wi-Fi: outras redes, da mais forte para a mais fraca (g_mx)
std::vector<std::pair<std::wstring, bool>> g_bts;  // Bluetooth: (nome, conectado) (g_mx)
std::vector<Out> g_detOuts;                       // Som: as saídas
bool g_detLoading;
const wchar_t* FOCO_MODOS[4] = { L"Não Perturbe", L"Trabalho", L"Sono", L"Reduzir Interrupções" };
const float DET_TOP = 44, DET_ROW = 32, DET_SEP = 11, DET_LINK = 22, DET_HEAD = 16;

float DetailH() {
    std::lock_guard<std::mutex> l(g_mx);
    float h = DET_TOP;
    switch (g_detail) {
    case D_WIFI: h += (g_st.ssid.empty() ? 0 : DET_HEAD + DET_ROW) + 22 + std::min<size_t>(std::max<size_t>(g_nets.size(), 1), 6) * DET_ROW; break;
    case D_BT: h += DET_HEAD + std::min<size_t>(std::max<size_t>(g_bts.size(), 1), 6) * DET_ROW; break;
    case D_FOCO: h += 4 * DET_ROW; break;
    case D_TELA: h += 30 + 12 + 69; break;
    case D_SOM: h += 30 + DET_HEAD + std::max<size_t>(g_detOuts.size(), 1) * DET_ROW; break;
    default: return 0;
    }
    return h + DET_SEP + DET_LINK + 10;
}

void LoadDetail(int kind) {
    g_detLoading = true;
    std::thread([kind] {
        if (kind == D_WIFI) {
            std::vector<std::pair<std::wstring, ULONG>> v;
            HANDLE h = nullptr; DWORD ver = 0;
            if (WlanOpenHandle(2, nullptr, &ver, &h) == ERROR_SUCCESS) {
                PWLAN_INTERFACE_INFO_LIST list = nullptr;
                if (WlanEnumInterfaces(h, nullptr, &list) == ERROR_SUCCESS && list) {
                    for (DWORD i = 0; i < list->dwNumberOfItems; i++) {
                        PWLAN_AVAILABLE_NETWORK_LIST nl = nullptr;
                        if (WlanGetAvailableNetworkList(h, &list->InterfaceInfo[i].InterfaceGuid, 0, nullptr, &nl) != ERROR_SUCCESS || !nl) continue;
                        for (DWORD k = 0; k < nl->dwNumberOfItems; k++) {
                            auto& n = nl->Network[k];
                            if (!n.dot11Ssid.uSSIDLength) continue;
                            wchar_t name[64] = {};
                            MultiByteToWideChar(CP_UTF8, 0, (const char*)n.dot11Ssid.ucSSID, (int)n.dot11Ssid.uSSIDLength, name, 63);
                            bool dup = false;
                            for (auto& e : v) if (e.first == name) { e.second = std::max(e.second, n.wlanSignalQuality); dup = true; }
                            if (!dup) v.push_back({ name, n.wlanSignalQuality });
                        }
                        WlanFreeMemory(nl);
                    }
                    WlanFreeMemory(list);
                }
                WlanCloseHandle(h, nullptr);
            }
            std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
            std::lock_guard<std::mutex> l(g_mx);
            g_nets.clear();
            for (auto& e : v) if (e.first != g_st.ssid) g_nets.push_back(e.first);
        } else if (kind == D_BT) {
            std::vector<std::pair<std::wstring, bool>> r;
            BLUETOOTH_DEVICE_SEARCH_PARAMS sp{ sizeof(sp) };
            sp.fReturnAuthenticated = TRUE; sp.fReturnRemembered = TRUE; sp.fReturnConnected = TRUE;
            BLUETOOTH_DEVICE_INFO di{ sizeof(di) };
            if (HBLUETOOTH_DEVICE_FIND f = BluetoothFindFirstDevice(&sp, &di)) {
                do {
                    if (di.fRemembered || di.fAuthenticated) r.push_back({ di.szName, di.fConnected != FALSE });
                    di.dwSize = sizeof(di);
                } while (BluetoothFindNextDevice(f, &di));
                BluetoothFindDeviceClose(f);
            }
            std::stable_sort(r.begin(), r.end(), [](auto& a, auto& b) { return a.second > b.second; });
            std::lock_guard<std::mutex> l(g_mx);
            g_bts = r;
        }
        g_detLoading = false;
        if (g_wnd) PostMessageW(g_wnd, WM_CC_UPDATE, 1, 0);
    }).detach();
}

void Place();
void OpenDetail(int kind, D2D1_RECT_F from) {
    g_detail = kind; g_detailAt = anim::Now(); g_detailFrom = from;
    if (kind == D_SOM) g_detOuts = Outputs();
    if (kind == D_WIFI || kind == D_BT) LoadDetail(kind);
    Place();
    ui::FrameClock(g_wnd, WM_CC_FRAME, true);
}
void CloseDetail() {
    if (g_detail == D_NONE) return;
    g_detail = D_NONE;
    Place();
}
// o modo de Foco ligado (foco_modo) ou -1
int FocoModo(bool focus) { return focus ? std::clamp(_wtoi(shell::Setting(L"foco_modo", L"0").c_str()), 0, 3) : -1; }

// Tahoe: o brilho especular dos cantos fica apagado em repouso e acende no bloco sob o ponteiro, com a luz vindo do
// lado do ponteiro (guia, seções 3.2 e 29.1); apertar faz o vidro crescer um pouco (escala ~1,06, mola ágil)
D2D1_POINT_2F g_ccHover{ -1, -1 };
vidro::LuzSegue g_ccLuz;   // a luz do brilho, atrás do ponteiro com a mola de 0,3 s
double g_ccLuzAt;
D2D1_POINT_2F g_ccPress{ -1, -1 };
double g_ccPressAt, g_ccReleaseAt;
float PressGrow() {  // 0..1: quanto o bloco apertado cresceu
    double now = anim::Now();
    if (anim::reduceMotion) return 0;
    if (g_ccPress.x >= 0) return anim::Spring((float)((now - g_ccPressAt) / 1000.0), 0.5f, 0.85f);
    float t = (float)((now - g_ccReleaseAt) / 1000.0);
    return t < 0.6f ? 1 - anim::Spring(t, 0.5f, 0.85f) : 0.f;
}

// ---- Tahoe: desenho
void PaintTahoe() {
    float sc = Sc();
    const float W = T_W, H = PanelH();
    int wpx = (int)std::lround(W * sc), hpx = (int)std::lround(H * sc);
    auto* dc = ui::GpuBegin(g_s, wpx, hpx, sc);
    if (!dc) return;
    State st;
    { std::lock_guard<std::mutex> l(g_mx); st = g_st; if (g_st.artNew) g_st.artNew = false; }
    if (st.artNew) {
        if (g_art) { g_art->Release(); g_art = nullptr; }
        if (!st.art.empty()) g_art = ui::Bitmap(dc, st.artW, st.artH, st.art);
    }
    bool dark = ui::Dark();
    anim::Panel mo = Motion();
    float t = mo.fade;
    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &br);
    ID2D1Layer* layer = nullptr;
    dc->CreateLayer(nullptr, &layer);
    dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), t), layer);
    g_hits.clear();
    tglass::Begin();
    // texto e símbolos brancos sobre o vidro (como nas capturas); a tinta escurece um pouco o fundo
    const D2D1_COLOR_F white = D2D1::ColorF(1, 1, 1, 0.96f), white2 = D2D1::ColorF(1, 1, 1, 0.72f);
    // vidro "claro" dos módulos (capturas do macOS 26): no claro, branco leitoso leve sobre o fundo saturado;
    // no escuro, grafite; o texto branco continua legível por causa do fosco forte
    // vidro "clear" da Apple (Central do macOS 26): a cor do fundo passa forte; só um pouco de tinta para o texto
    // medido na tela (guia, seções 52 e 60): saída = 0,69 × fundo + 0,09 (escuro) e 0,67 × fundo + 0,09 (claro):
    // 31 % de um cinza 0,29 por cima do fundo desfocado, igual nos dois modos
    const D2D1_COLOR_F tint = dark ? D2D1::ColorF(0.29f, 0.29f, 0.30f, 0.31f) : D2D1::ColorF(0.27f, 0.27f, 0.28f, 0.33f);

    const D2D1_COLOR_F blue = D2D1::ColorF(0.04f, 0.52f, 1, 1);
    Pal p = Colors();
    const bool real = backdrop::Real();  // vidro real (refração): as peças vão para o backdrop, cada uma com a lente dela
    std::vector<backdrop::Piece> pieces;
    static D2D1_RECT_F pressedR{};
    const float grow = PressGrow();
    auto piece = [&](D2D1_RECT_F r, float rad) {  // uma peça de vidro: vidro (Composição) e contorno (D2D)
        bool hov = g_ccHover.x >= r.left && g_ccHover.x < r.right && g_ccHover.y >= r.top && g_ccHover.y < r.bottom;
        bool prs = (g_ccPress.x >= r.left && g_ccPress.x < r.right && g_ccPress.y >= r.top && g_ccPress.y < r.bottom);
        if (prs) pressedR = r;
        if (grow > 0.001f && pressedR.right > pressedR.left && r.left == pressedR.left && r.top == pressedR.top) {
            float gx = (r.right - r.left) * 0.03f * grow, gy = (r.bottom - r.top) * 0.03f * grow;  // escala 1,06
            r.left -= gx; r.right += gx; r.top -= gy; r.bottom += gy; rad += std::min(gx, gy);
        }
        D2D1_RECT_F pr = D2D1::RectF(r.left * sc, r.top * sc, r.right * sc, r.bottom * sc);
        if (real) pieces.push_back({ pr, rad * sc });
        else tglass::Rect(pr, rad * sc, tint);
        // brilho suave no alto e o contorno especular do Liquid Glass
        br->SetColor(D2D1::ColorF(1, 1, 1, dark ? 0.03f : 0.06f));
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), br);
        float lpx = g_ccLuz.x >= 0 ? g_ccLuz.x : g_ccHover.x, lpy = g_ccLuz.y >= 0 ? g_ccLuz.y : g_ccHover.y;
        float lx = hov ? std::clamp((lpx - r.left) / std::max(1.f, r.right - r.left), 0.f, 1.f) : -1, ly = hov ? std::clamp((lpy - r.top) / std::max(1.f, r.bottom - r.top), 0.f, 1.f) : -1;
        float spec = hov ? (prs || grow > 0.5f ? 1.3f : 1.f) : 0.f;  // apertado: +30 %
        liquid::Rim(dc, r, rad, dark, 1.0f, spec, lx, ly);  // fio branco em volta toda; os cantos só acendem com o ponteiro
    };
    auto circleBtn = [&](D2D1_POINT_2F c, float r, bool on, const wchar_t* sym, wchar_t fb, D2D1_COLOR_F onColor) {
        if (on) {  // ligado: círculo branco com o símbolo colorido
            br->SetColor(D2D1::ColorF(1, 1, 1, 0.98f));
            dc->FillEllipse(D2D1::Ellipse(c, r, r), br);
        }
        float s = r * 0.56f;
        Sym(dc, br, sym, fb, D2D1::RectF(c.x - s, c.y - s, c.x + s, c.y + s), on ? onColor : white);
    };
    // os controles pela grade (cc_layout); editando, cada um ganha o "−" e aparecem lugares vazios embaixo
    std::wstring wifiSub = st.radio[0] == 0 ? Tr(L"Desligado") : !st.ssid.empty() ? st.ssid : st.radio[0] == -1 ? Tr(L"Ethernet") : Tr(L"Não conectado");
    std::wstring btSub = st.radio[1] == 1 ? Tr(L"Ativado") : st.radio[1] == 0 ? Tr(L"Desativado") : st.radio[1] == -1 ? Tr(L"Indisponível") : L"…";
    std::wstring airSub = st.airdrop == 2 ? Tr(L"Todos") : st.airdrop == 1 ? Tr(L"Só Contatos") : Tr(L"Desativado");
    auto capsule = [&](D2D1_RECT_F r, HitId btn, HitId text, const wchar_t* sym, wchar_t fb, const std::wstring& title, const std::wstring& sub, bool on, D2D1_COLOR_F onColor) {
        piece(r, (r.bottom - r.top) / 2);
        D2D1_POINT_2F c{ r.left + T_CAP / 2, (r.top + r.bottom) / 2 };
        circleBtn(c, 21, on, sym, fb, onColor);
        if (sub.empty()) Text(dc, br, title, D2D1::RectF(c.x + 28, r.top, r.right - 10, r.bottom), g_tfBold, white);
        else {
            Text(dc, br, title, D2D1::RectF(c.x + 28, c.y - 16, r.right - 10, c.y + 1), g_tfBold, white);
            // vibrancy (guia, seção 1.2): o texto secundário escuro é "plus-lighter" — clareia o que está atrás em vez de
            // cobrir com um cinza fixo, então pega a cor do fundo
            if (dark) {
                dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_ADD);
                Text(dc, br, sub, D2D1::RectF(c.x + 28, c.y + 1, r.right - 10, c.y + 16), g_tfSmall, D2D1::ColorF(1, 1, 1, 0.55f));
                dc->SetPrimitiveBlend(D2D1_PRIMITIVE_BLEND_SOURCE_OVER);
            } else Text(dc, br, sub, D2D1::RectF(c.x + 28, c.y + 1, r.right - 10, c.y + 16), g_tfSmall, white2);
        }
        g_hits.push_back({ btn, D2D1::RectF(c.x - 22, c.y - 22, c.x + 22, c.y + 22) });
        if (text != btn) g_hits.push_back({ text, D2D1::RectF(c.x + 24, r.top, r.right, r.bottom) });
    };
    auto circle = [&](D2D1_RECT_F r, HitId id, const wchar_t* sym, wchar_t fb, bool on, D2D1_COLOR_F onColor, int arg = -1) {
        float cr = std::min(r.right - r.left, r.bottom - r.top) / 2;
        D2D1_POINT_2F c{ (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
        piece(D2D1::RectF(c.x - cr, c.y - cr, c.x + cr, c.y + cr), cr);
        circleBtn(c, on ? cr - 1 : cr, on, sym, fb, onColor);
        g_hits.push_back({ id, D2D1::RectF(c.x - cr, c.y - cr, c.x + cr, c.y + cr), arg });
    };
    auto nowPlaying = [&](D2D1_RECT_F med) {
        piece(med, 30);
        D2D1_RECT_F art = D2D1::RectF(med.left + 16, med.top + 14, med.left + 62, med.top + 60);
        if (g_art && st.hasMedia) {
            ID2D1Factory* f = nullptr; dc->GetFactory(&f);
            ID2D1RoundedRectangleGeometry* rg = nullptr; f->CreateRoundedRectangleGeometry(D2D1::RoundedRect(art, 9, 9), &rg);
            ID2D1Layer* l2 = nullptr; dc->CreateLayer(nullptr, &l2);
            dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), rg), l2);
            auto sz = g_art->GetSize();
            float k = std::max(46 / sz.width, 46 / sz.height), acx = (art.left + art.right) / 2, acy = (art.top + art.bottom) / 2;
            dc->DrawBitmap(g_art, D2D1::RectF(acx - sz.width * k / 2, acy - sz.height * k / 2, acx + sz.width * k / 2, acy + sz.height * k / 2), 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            dc->PopLayer(); l2->Release(); rg->Release(); f->Release();
        } else {
            br->SetColor(D2D1::ColorF(1, 1, 1, 0.16f));
            dc->FillRoundedRectangle(D2D1::RoundedRect(art, 9, 9), br);
        }
        std::wstring title = st.hasMedia && !st.title.empty() ? st.title : Tr(L"Nada Tocando");
        Text(dc, br, title, D2D1::RectF(med.left + 16, med.top + 66, med.right - 12, med.top + 84), g_tfBold, white);
        if (st.hasMedia) Text(dc, br, st.artist, D2D1::RectF(med.left + 16, med.top + 84, med.right - 12, med.top + 100), g_tfSmall, white2);
        float my = med.bottom - 22, mcx = (med.left + med.right) / 2;
        D2D1_COLOR_F ctl = st.hasMedia ? white : white2;
        Sym(dc, br, L"backward.fill", 0xE892, D2D1::RectF(mcx - 46, my - 7, mcx - 26, my + 7), ctl);
        Sym(dc, br, st.playing ? L"pause.fill" : L"play.fill", 0xE768, D2D1::RectF(mcx - 8, my - 9, mcx + 8, my + 9), ctl);
        Sym(dc, br, L"forward.fill", 0xE893, D2D1::RectF(mcx + 26, my - 7, mcx + 46, my + 7), ctl);
        g_hits.push_back({ H_PLAY, D2D1::RectF(mcx - 18, my - 18, mcx + 18, my + 18) });
        g_hits.push_back({ H_NEXT, D2D1::RectF(mcx + 20, my - 18, mcx + 54, my + 18) });
    };
    // Tela e Som: blocos largos com trilho fino e ícone nas duas pontas
    auto bar = [&](D2D1_RECT_F r, const wchar_t* label, HitId head, HitId track, float v, const wchar_t* sl, wchar_t fl, const wchar_t* sr, wchar_t fr, bool output) {
        piece(r, 26);
        Text(dc, br, label, D2D1::RectF(r.left + 16, r.top + 8, r.right - 16, r.top + 28), g_tfBold, white);
        g_hits.push_back({ head, D2D1::RectF(r.left, r.top, r.right - (output ? 50 : 0), r.top + 28) });
        float cy = r.top + 43, right = r.right - (output ? 48 : 16);
        Sym(dc, br, sl, fl, D2D1::RectF(r.left + 14, cy - 6.5f, r.left + 27, cy + 6.5f), white);
        Sym(dc, br, sr, fr, D2D1::RectF(right - 16, cy - 8, right, cy + 8), white);
        D2D1_RECT_F tr = D2D1::RectF(r.left + 36, cy - 2.5f, right - 26, cy + 2.5f);
        float kx = tr.left + KN_W / 2 + std::clamp(v, 0.f, 1.f) * (tr.right - tr.left - KN_W);
        auto drawTrack = [&] {
            // trilho #E8E8E8 / #232323 e a parte cheia branca (guia, seção 21, lido do ControlCenter.car)
            br->SetColor(dark ? D2D1::ColorF(0x23 / 255.f, 0x23 / 255.f, 0x23 / 255.f, 1) : D2D1::ColorF(0xE8 / 255.f, 0xE8 / 255.f, 0xE8 / 255.f, 1));
            dc->FillRoundedRectangle(D2D1::RoundedRect(tr, 2.5f, 2.5f), br);
            br->SetColor(D2D1::ColorF(1, 1, 1, 1));  // o que já está cheio, branco
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tr.left, tr.top, std::max(kx, tr.left + 5), tr.bottom), 2.5f, 2.5f), br);
        };
        drawTrack();
        // macOS 27 (captura Finder-Control-Center): Tela e Som sem o botão; ele só aparece arrastando (vira lente)
        if (!liquid::Golden() || g_kPress[track == H_BRIGHT ? 0 : 1] > 0.001f) {   // o botão: pílula branca; arrastando vira lente (o mesmo do menu de som)
            int ki = track == H_BRIGHT ? 0 : 1;
            float pr = std::clamp(g_kPress[ki], 0.f, 1.f), grow = 1 + (KN_LENS - 1) * pr;
            float kw = KN_W * grow * (1 + g_kStretch[ki]), kh = KN_H * grow * (1 - g_kStretch[ki] * 0.35f), krad = kh / 2;
            D2D1_RECT_F k = D2D1::RectF(kx - kw / 2, cy - kh / 2, kx + kw / 2, cy + kh / 2);
            D2D1_ROUNDED_RECT krr = D2D1::RoundedRect(k, krad, krad);
            for (int sdw = 3; sdw >= 1; sdw--) {  // sombra curta
                float g = sdw * 1.1f;
                br->SetColor(D2D1::ColorF(0, 0, 0, (0.12f + 0.06f * pr) / (sdw + 1)));
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(k.left - g, k.top - g + 1.5f, k.right + g, k.bottom + g + 1.5f), krad + g, krad + g), br);
            }
            if (pr < 0.999f) {
                br->SetColor(D2D1::ColorF(1, 1, 1, 1 - pr));
                dc->FillRoundedRectangle(krr, br);
            }
            if (pr > 0.001f) {
                ID2D1Factory* f = nullptr; dc->GetFactory(&f);
                ID2D1RoundedRectangleGeometry* geo = nullptr;
                if (f) { f->CreateRoundedRectangleGeometry(krr, &geo); f->Release(); }
                if (geo) {
                    dc->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), geo, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), pr), nullptr);
                    br->SetColor(D2D1::ColorF(1, 1, 1, dark ? 0.10f : 0.28f));  // corpo do vidro, quase transparente
                    dc->FillRoundedRectangle(krr, br);
                    D2D1_MATRIX_3X2_F old; dc->GetTransform(&old);
                    dc->SetTransform(D2D1::Matrix3x2F::Scale(1.18f, 1.9f, { kx, cy }) * old);  // refração: o trilho ampliado
                    drawTrack();
                    dc->SetTransform(old);
                    vidro::Borda(dc, k, krad, 1, dark, 1.2f, true);
                    dc->PopLayer();
                    geo->Release();
                }
            }
        }
        // a área de arrastar (DragTo desconta meia pílula nas pontas)
        g_hits.push_back({ track, D2D1::RectF(tr.left - 2, cy - 14, tr.right + 2, cy + 14) });
        if (output) {
            D2D1_POINT_2F oc{ r.right - 26, cy };
            br->SetColor(D2D1::ColorF(1, 1, 1, 0.20f));
            dc->FillEllipse(D2D1::Ellipse(oc, 14, 14), br);
            Sym(dc, br, L"airplay.audio", 0xE7F5, D2D1::RectF(oc.x - 8, oc.y - 8, oc.x + 8, oc.y + 8), white);
            g_hits.push_back({ H_OUTPUT, D2D1::RectF(oc.x - 16, oc.y - 16, oc.x + 16, oc.y + 16) });
        }
    };

    if (g_detail != D_NONE) {
        // o painel: o bloco de origem cresce até a largura toda e a altura do conteúdo (mola ~0,25 s, sem quique)
        float e = anim::reduceMotion ? 1.f : std::min(1.f, anim::Spring((float)((anim::Now() - g_detailAt) / 1000.0), 0.22f, 0.92f));
        D2D1_RECT_F full = D2D1::RectF(T_M, T_M, W - T_M, T_M + DetailH());
        auto lerp = [&](float a, float b) { return a + (b - a) * e; };
        D2D1_RECT_F pr = D2D1::RectF(lerp(g_detailFrom.left, full.left), lerp(g_detailFrom.top, full.top), lerp(g_detailFrom.right, full.right), lerp(g_detailFrom.bottom, full.bottom));
        g_hits.push_back({ H_DET_PANEL, pr });
        piece(pr, 26);
        float ca = std::clamp((e - 0.35f) / 0.65f, 0.f, 1.f);  // o conteúdo entra com fade depois que o bloco cresceu
        dc->PushLayer(D2D1::LayerParameters1(pr, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), ca), nullptr);
        const float L = full.left + 14, R = full.right - 14;
        static IDWriteTextFormat* tfT = nullptr;
        if (!tfT) tfT = ui::Format(16, DWRITE_FONT_WEIGHT_BOLD);
        const wchar_t* titles[5] = { L"Wi‑Fi", L"Bluetooth", L"Foco", L"Tela", L"Som" };
        Text(dc, br, Tr(titles[g_detail]), D2D1::RectF(L, full.top + 10, R - 60, full.top + 34), tfT ? tfT : g_tfBold, white);
        auto sw = [&](bool on) {  // switch de 54 × 24 à direita do título
            D2D1_RECT_F s = D2D1::RectF(R - 54, full.top + 10, R, full.top + 34);
            br->SetColor(on ? blue : D2D1::ColorF(1, 1, 1, 0.22f));
            dc->FillRoundedRectangle(D2D1::RoundedRect(s, 12, 12), br);
            float kx = on ? s.right - 20 : s.left + 20;
            br->SetColor(D2D1::ColorF(1, 1, 1, 1));
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(kx - 18, s.top + 2, kx + 18, s.bottom - 2), 10, 10), br);
            g_hits.push_back({ H_DET_SWITCH, s });
        };
        float y = full.top + DET_TOP;
        auto head = [&](const std::wstring& s) { Text(dc, br, s, D2D1::RectF(L, y, R, y + DET_HEAD), g_tfSmall, white2); y += DET_HEAD; };
        auto row = [&](const std::wstring& name, const wchar_t* sym, wchar_t fb, bool on, D2D1_COLOR_F onColor, int arg, bool lock = false) {
            float cy = y + DET_ROW / 2;
            D2D1_POINT_2F c{ L + 13, cy };
            br->SetColor(on ? onColor : D2D1::ColorF(1, 1, 1, 0.18f));
            dc->FillEllipse(D2D1::Ellipse(c, 13, 13), br);
            Sym(dc, br, sym, fb, D2D1::RectF(c.x - 7, cy - 7, c.x + 7, cy + 7), white);
            Text(dc, br, name, D2D1::RectF(L + 34, y, R - (lock ? 22 : 0), y + DET_ROW), g_tfTile, white);
            if (lock) Sym(dc, br, L"lock.fill", 0xE72E, D2D1::RectF(R - 14, cy - 6, R - 2, cy + 6), white2);
            g_hits.push_back({ H_DET_ROW, D2D1::RectF(full.left + 4, y, full.right - 4, y + DET_ROW), arg });
            y += DET_ROW;
        };
        auto tail = [&](const std::wstring& link) {
            br->SetColor(D2D1::ColorF(1, 1, 1, 0.14f));
            dc->FillRectangle(D2D1::RectF(L, y + 5, R, y + 5 + 1 / sc), br);
            y += DET_SEP;
            Text(dc, br, link, D2D1::RectF(L, y, R, y + DET_LINK), g_tfTile, white);
            g_hits.push_back({ H_DET_SET, D2D1::RectF(full.left + 4, y, full.right - 4, y + DET_LINK) });
        };
        std::vector<std::wstring> nets; std::vector<std::pair<std::wstring, bool>> bts;
        { std::lock_guard<std::mutex> l(g_mx); nets = g_nets; bts = g_bts; }
        // a faixa de volume / brilho (o mesmo trilho dos blocos: #E8E8E8 / #232323 e a parte cheia branca)
        auto slider = [&](HitId track, float v, const wchar_t* sl, wchar_t fl, const wchar_t* sr, wchar_t fr) {
            float cy = y + 12;
            Sym(dc, br, sl, fl, D2D1::RectF(L, cy - 7, L + 14, cy + 7), white);
            Sym(dc, br, sr, fr, D2D1::RectF(R - 16, cy - 8, R, cy + 8), white);
            D2D1_RECT_F tr = D2D1::RectF(L + 24, cy - 2.5f, R - 26, cy + 2.5f);
            br->SetColor(dark ? D2D1::ColorF(0x23 / 255.f, 0x23 / 255.f, 0x23 / 255.f, 1) : D2D1::ColorF(0xE8 / 255.f, 0xE8 / 255.f, 0xE8 / 255.f, 1));
            dc->FillRoundedRectangle(D2D1::RoundedRect(tr, 2.5f, 2.5f), br);
            float kx = tr.left + KN_W / 2 + std::clamp(v, 0.f, 1.f) * (tr.right - tr.left - KN_W);
            br->SetColor(D2D1::ColorF(1, 1, 1, 1));
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tr.left, tr.top, std::max(kx, tr.left + 5), tr.bottom), 2.5f, 2.5f), br);
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(kx - KN_W / 2, cy - KN_H / 2, kx + KN_W / 2, cy + KN_H / 2), KN_H / 2, KN_H / 2), br);
            g_hits.push_back({ track, D2D1::RectF(tr.left - 2, cy - 14, tr.right + 2, cy + 14) });
            y += 30;
        };
        switch (g_detail) {
        case D_WIFI:
            sw(st.radio[0] == 1);
            if (!st.ssid.empty()) { head(Tr(L"Rede Conhecida")); row(st.ssid, L"wifi", 0xE701, true, blue, -1, true); }
            Text(dc, br, Tr(L"Outras Redes"), D2D1::RectF(L, y, R, y + 22), g_tfBold, white2); y += 22;
            if (nets.empty()) { Text(dc, br, g_detLoading ? Tr(L"Procurando…") : Tr(L"Nenhuma rede encontrada"), D2D1::RectF(L, y, R, y + DET_ROW), g_tfTile, white2); y += DET_ROW; }
            for (size_t i = 0; i < nets.size() && i < 6; i++) row(nets[i], L"wifi", 0xE701, false, blue, (int)i, true);
            tail(Tr(L"Ajustes de Wi‑Fi…"));
            break;
        case D_BT:
            sw(st.radio[1] == 1);
            head(Tr(L"Dispositivos"));
            if (bts.empty()) { Text(dc, br, g_detLoading ? Tr(L"Procurando…") : Tr(L"Nenhum dispositivo"), D2D1::RectF(L, y, R, y + DET_ROW), g_tfTile, white2); y += DET_ROW; }
            for (size_t i = 0; i < bts.size() && i < 6; i++) row(bts[i].first, L"headphones", 0xE702, bts[i].second, blue, (int)i);
            tail(Tr(L"Ajustes de Bluetooth…"));
            break;
        case D_FOCO: {
            int cur = FocoModo(st.focus);
            const wchar_t* syms[4] = { L"moon.fill", L"briefcase.fill", L"bed.double.fill", L"person.crop.circle.fill" };
            for (int i = 0; i < 4; i++) row(Tr(FOCO_MODOS[i]), syms[i], 0xE708, cur == i, D2D1::ColorF(0.37f, 0.36f, 0.90f, 1), i);
            tail(Tr(L"Ajustes de Foco…"));
            break;
        }
        case D_TELA: {
            slider(H_BRIGHT, st.bright >= 0 ? st.bright : 1, L"sun.min", 0xE706, L"sun.max", 0xE706);
            y += 12;
            // dois cartões de 133 × 69: Modo Escuro e Night Shift
            float cw = (R - L - 14) / 2;
            D2D1_RECT_F c1 = D2D1::RectF(L, y, L + cw, y + 69), c2 = D2D1::RectF(R - cw, y, R, y + 69);
            auto card = [&](D2D1_RECT_F c, const wchar_t* sym, wchar_t fb, const std::wstring& name, bool on, HitId id) {
                br->SetColor(D2D1::ColorF(1, 1, 1, on ? 0.30f : 0.12f));
                dc->FillRoundedRectangle(D2D1::RoundedRect(c, 14, 14), br);
                Sym(dc, br, sym, fb, D2D1::RectF(c.left + 12, c.top + 10, c.left + 32, c.top + 30), white);
                Text(dc, br, name, D2D1::RectF(c.left + 12, c.bottom - 28, c.right - 8, c.bottom - 8), g_tfTile, white);
                g_hits.push_back({ id, c });
            };
            card(c1, L"circle.lefthalf.filled", 0xE793, Tr(L"Modo Escuro"), dark, H_DARK);
            card(c2, L"sun.max", 0xE706, L"Night Shift", false, H_NIGHT);
            y += 69;
            tail(Tr(L"Ajustes de Tela…"));
            break;
        }
        case D_SOM:
            slider(H_VOLUME, st.mute ? 0 : st.volume, L"speaker.fill", 0xE74F, L"speaker.wave.3.fill", 0xE767);
            head(Tr(L"Saída"));
            for (size_t i = 0; i < g_detOuts.size(); i++) row(g_detOuts[i].name, L"speaker.wave.2.fill", 0xE767, g_detOuts[i].def, blue, (int)i);
            tail(Tr(L"Ajustes de Som…"));
            break;
        }
        dc->PopLayer();
    } else {
    float bodyH = 0;
    auto slots = LayoutCC(&bodyH);
    if (g_dragActive)  // o arrastado é desenhado por último (por cima), seguindo o ponteiro
        std::stable_sort(slots.begin(), slots.end(), [](const Slot& a, const Slot& b) { return (g_dragId != a.c->id) > (g_dragId != b.c->id); });
    for (auto& sl : slots) {
        std::wstring id = sl.c->id;
        D2D1_RECT_F r = sl.r;
        if (g_dragActive && id == g_dragId) {
            float hw = (r.right - r.left) / 2, hh = (r.bottom - r.top) / 2;
            r = D2D1::RectF(g_dragPt.x - hw, g_dragPt.y - hh, g_dragPt.x + hw, g_dragPt.y + hh);
        }
        if (id == L"wifi") capsule(r, H_WIFI_BTN, H_WIFI, sl.c->sym, sl.c->fb, L"Wi‑Fi", wifiSub, st.radio[0] == 1, blue);
        else if (id == L"bluetooth") capsule(r, H_BT_BTN, H_BT, sl.c->sym, sl.c->fb, L"Bluetooth", btSub, st.radio[1] == 1, blue);
        else if (id == L"airdrop") capsule(r, H_AIRDROP, H_AIRDROP, sl.c->sym, sl.c->fb, L"AirDrop", airSub, st.airdrop > 0, blue);
        else if (id == L"foco") capsule(r, H_FOCUS, H_FOCUS_TEXT, sl.c->sym, sl.c->fb, Tr(L"Foco"), L"", st.focus, D2D1::ColorF(0.37f, 0.36f, 0.90f, 1));
        else if (id == L"tocando") nowPlaying(r);
        else if (id == L"tela") bar(r, Tr(L"Tela"), H_DISPLAY, H_BRIGHT, st.bright >= 0 ? st.bright : 1, L"sun.min", 0xE706, L"sun.max", 0xE706, false);
        else if (id == L"som") bar(r, Tr(L"Som"), H_SOUND, H_VOLUME, st.mute ? 0 : st.volume, L"speaker.fill", 0xE74F, L"speaker.wave.3.fill", 0xE767, true);
        else if (id == L"mission") circle(r, H_MISSION, sl.c->sym, sl.c->fb, false, blue);
        else if (id == L"organizador") circle(r, H_STAGE, sl.c->sym, sl.c->fb, palco::On(), blue);  // aceso quando ligado
        else if (id == L"espelhar") circle(r, H_MIRROR, sl.c->sym, sl.c->fb, false, blue);
        else if (id == L"escuro") circle(r, H_DARK, sl.c->sym, sl.c->fb, dark, D2D1::ColorF(0.1f, 0.1f, 0.12f, 1));
        else if (id == L"captura") circle(r, H_SHOT, sl.c->sym, sl.c->fb, false, blue);
        else circle(r, H_EXTRA, sl.c->sym, sl.c->fb, false, blue, sl.idx);  // da galeria
    }
    float yEnd = bodyH;
    if (g_editing) {
        // lugares vazios (círculos apagados) onde os controles novos entram
        for (int rr = 0; rr < EDIT_ROWS; rr++)
            for (int k = 0; k < 4; k++) {
                float cx = T_M + k * (T_CELL + T_G) + T_CELL / 2, cyy = yEnd + T_G + rr * (T_CAP + T_G) + T_CAP / 2, cr = T_CAP / 2 - 2;
                br->SetColor(D2D1::ColorF(1, 1, 1, dark ? 0.06f : 0.14f));
                dc->FillEllipse(D2D1::Ellipse({ cx, cyy }, cr, cr), br);
                br->SetColor(D2D1::ColorF(1, 1, 1, dark ? 0.14f : 0.30f));
                dc->DrawEllipse(D2D1::Ellipse({ cx, cyy }, cr - 0.5f, cr - 0.5f), br, 1);
            }
        yEnd += EDIT_ROWS * (T_CAP + T_G);
        // o "−" de cada controle (por cima de tudo, no canto de cima à esquerda)
        for (auto& sl : slots) {
            if (g_dragActive && g_dragId == sl.c->id) continue;
            float rr = sl.c->w == 1 && sl.c->h == 1 ? (sl.r.right - sl.r.left) / 2 : std::min(26.f, (sl.r.bottom - sl.r.top) / 2);
            D2D1_POINT_2F mc{ sl.r.left + rr * 0.29f + 2, sl.r.top + rr * 0.29f + 2 };
            br->SetColor(D2D1::ColorF(0.42f, 0.42f, 0.45f, 0.96f));
            dc->FillEllipse(D2D1::Ellipse(mc, 10, 10), br);
            br->SetColor(D2D1::ColorF(1, 1, 1, 0.9f));
            dc->DrawEllipse(D2D1::Ellipse(mc, 10, 10), br, 1);
            dc->FillRectangle(D2D1::RectF(mc.x - 5, mc.y - 1, mc.x + 5, mc.y + 1), br);
            g_hits.push_back({ H_REMOVE, D2D1::RectF(mc.x - 13, mc.y - 13, mc.x + 13, mc.y + 13), sl.idx });
        }
    }
    // Editar Controles (pílula); editando: Concluído
    float y7 = yEnd + T_G;
    std::wstring edLabel = g_editing ? Tr(L"Concluído") : Tr(L"Editar Controles");
    float ew = 112;  // pílula de 112 × 24 (guia, seção 40)
    D2D1_RECT_F ed = D2D1::RectF(W / 2 - ew / 2, y7, W / 2 + ew / 2, y7 + T_EDIT);
    piece(ed, T_EDIT / 2);
    g_tfTile->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    Text(dc, br, edLabel, ed, g_tfTile, white);
    g_tfTile->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    g_hits.push_back({ H_EDIT, ed });
    }  // fim do else (módulo expandido)
    (void)p;

    dc->PopLayer();
    layer->Release();
    br->Release();
    ui::GpuEnd(g_s);
    tglass::End(t, !real);
    veil::Opacity(t);
    float cx, cy; Pivot(wpx, cx, cy);
    ui::GpuScale(g_s, mo.scale, cx, cy);
    if (g_glass) {
        if (real) backdrop::SetPieces(g_back, (float)wpx, (float)hpx, pieces, tint, 4.5f);  // vidro "clear": o fundo quase nítido (a lente fica na borda)
        else backdrop::Set(g_back, 0, 0, 0, Colors().tint);
        backdrop::Fade(g_back, t, mo.scale, cx, cy);
    }
}

// ---- Galeria de controles (Editar Controles do macOS 26): janela de vidro ao lado da Central, com a busca e as
// categorias à esquerda e os controles de cada uma à direita. Clicar num controle põe na Central (ou tira, se já
// está); "OK" termina a edição.
void Close();
void Place();
void StartEdit();
void EndEdit();
void ToggleCtl(const std::wstring& id);
void GalleryDrag(const std::wstring& id, POINT screen);
void GalleryDrop(POINT screen);
namespace gal {
HWND w, back;
int pressItem = -1;       // controle pressionado (índice em CTLS)
D2D1_POINT_2F pressPt{};
bool dragging;
ui::GpuSurface s;
bool glass, open;
std::wstring cat, query;  // categoria escolhida ("" = todos) e o texto da busca
float scroll, contentH;
struct GHit { int kind; int v; D2D1_RECT_F r; };  // kind 0 categoria (v: índice, -1 todos), 1 controle (v: índice em CTLS), 2 OK
std::vector<GHit> hits;
std::vector<std::wstring> cats;
IDWriteTextFormat *tfLabel, *tfSide, *tfHead, *tfFoot, *tfBtn;
const float GW = 600, GH = 520, SW = 184, FOOT = 48;

float Sc() { return ui::Scale(w); }
bool Match(const Ctl& c) {
    if (!cat.empty() && cat != c.cat) return false;
    if (query.empty()) return true;
    std::wstring n = Tr(c.name), q = query;
    for (auto& ch : n) ch = towlower(ch);
    for (auto& ch : q) ch = towlower(ch);
    return n.find(q) != std::wstring::npos;
}

void Paint() {
    if (!open) return;
    float sc = Sc();
    int wpx = (int)std::lround(GW * sc), hpx = (int)std::lround(GH * sc);
    auto* dc = ui::GpuBegin(s, wpx, hpx, sc);
    if (!dc) return;
    bool dark = ui::Dark();
    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &br);
    D2D1_COLOR_F text = dark ? D2D1::ColorF(1, 1, 1, 0.94f) : D2D1::ColorF(0, 0, 0, 0.86f);
    D2D1_COLOR_F text2 = dark ? D2D1::ColorF(1, 1, 1, 0.55f) : D2D1::ColorF(0, 0, 0, 0.5f);
    if (!glass) { br->SetColor(dark ? D2D1::ColorF(0.16f, 0.16f, 0.17f, 0.96f) : D2D1::ColorF(0.96f, 0.96f, 0.97f, 0.97f)); dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, GW, GH), 24, 24), br); }
    // barra lateral: um tom mais escuro, com a mesma curva
    br->SetColor(dark ? D2D1::ColorF(0, 0, 0, 0.14f) : D2D1::ColorF(1, 1, 1, 0.30f));
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(8, 8, SW, GH - 8), 16, 16), br);
    hits.clear();
    // busca
    D2D1_RECT_F sr = D2D1::RectF(18, 18, SW - 10, 46);
    br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.10f) : D2D1::ColorF(1, 1, 1, 0.75f));
    dc->FillRoundedRectangle(D2D1::RoundedRect(sr, 14, 14), br);
    Sym(dc, br, L"magnifyingglass", 0xE721, D2D1::RectF(sr.left + 9, sr.top + 8, sr.left + 21, sr.top + 20), text2);
    Text(dc, br, query.empty() ? Tr(L"Buscar Controles") : query, D2D1::RectF(sr.left + 28, sr.top, sr.right - 8, sr.bottom), tfSide, query.empty() ? text2 : text);
    // categorias
    float y = 58;
    for (int i = -1; i < (int)cats.size(); i++) {
        D2D1_RECT_F r = D2D1::RectF(16, y, SW - 8, y + 28);
        bool sel = i < 0 ? cat.empty() : cat == cats[i];
        if (sel) { br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.14f) : D2D1::ColorF(0, 0, 0, 0.08f)); dc->FillRoundedRectangle(D2D1::RoundedRect(r, 9, 9), br); }
        Text(dc, br, i < 0 ? Tr(L"Todos os Controles") : Tr(cats[i].c_str()), D2D1::RectF(r.left + 12, r.top, r.right - 6, r.bottom), tfSide, text);
        hits.push_back({ 0, i, r });
        y += 29;
    }
    // controles, por categoria
    auto ids = LayoutIds();
    D2D1_RECT_F area = D2D1::RectF(SW + 8, 8, GW - 8, GH - FOOT);
    dc->PushAxisAlignedClip(area, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    float x0 = SW + 22, cw = (GW - SW - 44) / 4;
    y = 20 - scroll;
    float top0 = y;
    for (auto& cname : cats) {
        std::vector<int> list;
        for (int i = 0; i < NCTLS; i++) if (CTLS[i].cat == cname && Match(CTLS[i])) list.push_back(i);
        if (list.empty()) continue;
        Text(dc, br, Tr(cname.c_str()), D2D1::RectF(x0, y, GW - 20, y + 22), tfHead, text);
        y += 28;
        for (int k = 0; k < (int)list.size(); k++) {
            if (k && k % 4 == 0) y += 100;
            const Ctl& c = CTLS[list[k]];
            float cx = x0 + (k % 4) * cw + cw / 2, cy = y + 30;
            bool in = std::find(ids.begin(), ids.end(), std::wstring(c.id)) != ids.end();
            D2D1_RECT_F shape = c.w >= 2 ? D2D1::RectF(cx - 46, cy - 22, cx + 46, cy + 22) : D2D1::RectF(cx - 26, cy - 26, cx + 26, cy + 26);
            float rad = (shape.bottom - shape.top) / 2;
            liquid::Tile(dc, shape, rad, dark, 1.3f);
            Sym(dc, br, c.sym, c.fb, c.w >= 2 ? D2D1::RectF(shape.left + 12, cy - 9, shape.left + 30, cy + 9) : D2D1::RectF(cx - 11, cy - 11, cx + 11, cy + 11), text);
            if (c.w >= 2) Text(dc, br, Tr(c.name), D2D1::RectF(shape.left + 34, shape.top, shape.right - 6, shape.bottom), tfSide, text);
            Text(dc, br, Tr(c.name), D2D1::RectF(cx - cw / 2 + 2, cy + 30, cx + cw / 2 - 2, cy + 62), tfLabel, text2);
            if (in) {  // já está na Central: um visto azul
                D2D1_POINT_2F m{ shape.right - 4, shape.top + 4 };
                br->SetColor(D2D1::ColorF(0.04f, 0.52f, 1, 1));
                dc->FillEllipse(D2D1::Ellipse(m, 8, 8), br);
                br->SetColor(D2D1::ColorF(1, 1, 1, 1));
                dc->DrawLine({ m.x - 3.5f, m.y }, { m.x - 1, m.y + 2.5f }, br, 1.6f);
                dc->DrawLine({ m.x - 1, m.y + 2.5f }, { m.x + 3.5f, m.y - 2.5f }, br, 1.6f);
            }
            hits.push_back({ 1, list[k], D2D1::RectF(cx - cw / 2, y, cx + cw / 2, y + 96) });
        }
        y += 108;
    }
    contentH = y + scroll - top0;
    dc->PopAxisAlignedClip();
    // rodapé
    br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.10f) : D2D1::ColorF(0, 0, 0, 0.08f));
    dc->FillRectangle(D2D1::RectF(SW + 8, GH - FOOT, GW - 8, GH - FOOT + 1), br);
    Text(dc, br, Tr(L"Clique ou arraste um controle para a Central de Controle."), D2D1::RectF(SW + 22, GH - FOOT, GW - 90, GH - 4), tfFoot, text2);
    D2D1_RECT_F ok = D2D1::RectF(GW - 78, GH - FOOT + 10, GW - 18, GH - 12);
    br->SetColor(D2D1::ColorF(0.04f, 0.52f, 1, 1));
    dc->FillRoundedRectangle(D2D1::RoundedRect(ok, 13, 13), br);
    Text(dc, br, Tr(L"OK"), ok, tfBtn, D2D1::ColorF(1, 1, 1, 1));
    hits.push_back({ 2, 0, ok });
    liquid::Rim(dc, D2D1::RectF(0, 0, GW, GH), 24, dark);
    br->Release();
    ui::GpuEnd(s);
    if (glass) backdrop::Set(back, (float)wpx, (float)hpx, 24 * sc, dark ? D2D1::ColorF(0.13f, 0.13f, 0.15f, 0.86f) : D2D1::ColorF(0.96f, 0.96f, 0.98f, 0.86f));
}

void Show() {
    if (!w) return;
    cats.clear();
    for (auto& c : CTLS) if (std::find(cats.begin(), cats.end(), std::wstring(c.cat)) == cats.end()) cats.push_back(c.cat);
    std::sort(cats.begin(), cats.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(Tr(a.c_str()), Tr(b.c_str())) < 0; });
    cat.clear(); query.clear(); scroll = 0;
    open = true;
    float sc = Sc();
    int wpx = (int)std::lround(GW * sc), hpx = (int)std::lround(GH * sc);
    RECT cr; GetWindowRect(g_wnd, &cr);
    MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(MonitorFromWindow(g_wnd, MONITOR_DEFAULTTOPRIMARY), &mi);
    int x = std::max((int)mi.rcWork.left + 8, (int)cr.left - wpx - (int)std::lround(18 * sc)), y = cr.top;
    SetWindowPos(w, HWND_TOPMOST, x, y, wpx, hpx, SWP_NOACTIVATE);
    SetWindowPos(back, w, x, y, wpx, hpx, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    Paint();
    if (glass) backdrop::Fade(back, 1, 1, wpx / 2.f, hpx / 2.f);
    ShowWindow(w, SW_SHOW);
    SetForegroundWindow(w);
    SetFocus(w);
}
void Hide() {
    if (!open) return;
    open = false;
    if (w) ShowWindow(w, SW_HIDE);
    if (back) ShowWindow(back, SW_HIDE);
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); Paint(); return 0; }
    case WM_LBUTTONDOWN: {
        float sc = Sc();
        D2D1_POINT_2F pt{ (short)LOWORD(lp) / sc, (short)HIWORD(lp) / sc };
        for (auto& hh : hits) {
            if (pt.x < hh.r.left || pt.x >= hh.r.right || pt.y < hh.r.top || pt.y >= hh.r.bottom) continue;
            if (hh.kind == 1 && pt.y >= GH - FOOT) continue;  // por baixo do rodapé
            if (hh.kind == 0) { cat = hh.v < 0 ? L"" : cats[hh.v]; scroll = 0; Paint(); }
            else if (hh.kind == 1) { pressItem = hh.v; pressPt = pt; dragging = false; SetCapture(h); }  // clique ou arrastar
            else EndEdit();
            return 0;
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (pressItem < 0 || !(wp & MK_LBUTTON)) return 0;
        float sc = Sc();
        D2D1_POINT_2F pt{ (short)LOWORD(lp) / sc, (short)HIWORD(lp) / sc };
        if (!dragging && std::hypot(pt.x - pressPt.x, pt.y - pressPt.y) < 6) return 0;
        dragging = true;
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        POINT sp{ (short)LOWORD(lp), (short)HIWORD(lp) };
        ClientToScreen(h, &sp);
        GalleryDrag(CTLS[pressItem].id, sp);
        return 0;
    }
    case WM_LBUTTONUP: {
        if (pressItem < 0) return 0;
        int it = pressItem; pressItem = -1;
        if (GetCapture() == h) ReleaseCapture();
        if (dragging) {
            dragging = false;
            POINT sp{ (short)LOWORD(lp), (short)HIWORD(lp) };
            ClientToScreen(h, &sp);
            GalleryDrop(sp);
        } else ToggleCtl(CTLS[it].id);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        float view = GH - FOOT - 16;
        scroll = std::clamp(scroll - GET_WHEEL_DELTA_WPARAM(wp) / 120.f * 60, 0.f, std::max(0.f, contentH - view));
        Paint();
        return 0;
    }
    case WM_CHAR:
        if (wp == 8) { if (!query.empty()) query.pop_back(); }
        else if (wp >= 32) query += (wchar_t)wp;
        else return 0;
        scroll = 0;
        Paint();
        return 0;
    case WM_KEYDOWN: if (wp == VK_ESCAPE) EndEdit(); return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && open) {
            HWND to = (HWND)lp;
            if (to != g_wnd && to != back) { Log(L"galeria: perdeu o foco para %p", to); EndEdit(); Close(); }  // clicou fora da Central e da galeria
        }
        return 0;
    case WM_MOUSEACTIVATE: return MA_ACTIVATE;
    }
    return DefWindowProcW(h, m, wp, lp);
}
LRESULT CALLBACK BackProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, wp, lp);
}
void Create(HINSTANCE hi) {
    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = hi; wc.lpszClassName = L"ShellCentralGaleria"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    WNDCLASSW bc{}; bc.lpfnWndProc = BackProc; bc.hInstance = hi; bc.lpszClassName = L"ShellCentralGaleriaFundo";
    RegisterClassW(&bc);
    w = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, L"Controles", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
    back = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, bc.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
    if (!w || !back) return;
    glass = backdrop::Attach(back);
    DWORD corner = 1; DwmSetWindowAttribute(back, 33, &corner, sizeof(corner));
    COLORREF none = 0xFFFFFFFE; DwmSetWindowAttribute(back, 34, &none, sizeof(none));
    apps::ours.push_back(w); apps::ours.push_back(back);
    s.hwnd = w;
    tfLabel = ui::Format(10.5f, DWRITE_FONT_WEIGHT_NORMAL, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    tfSide = ui::Format(12.5f, DWRITE_FONT_WEIGHT_NORMAL);
    tfHead = ui::Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    tfFoot = ui::Format(11.5f, DWRITE_FONT_WEIGHT_NORMAL);
    tfBtn = ui::Format(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    for (auto* tf : { tfSide, tfHead, tfFoot, tfBtn }) if (tf) { tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); tf->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); }
    if (tfLabel) tfLabel->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
}
}  // namespace gal

// ---- arrastar (editando): dentro da Central muda a ordem; da galeria, põe no lugar onde cair
int g_pressIdx = -1;          // controle pressionado na Central (índice no layout)
D2D1_POINT_2F g_downPt{};
// prévia com `id` no lugar mais perto de `pt` (DIP da Central): testa cada posição e fica com a que deixa o
// controle mais perto do ponteiro (os outros se rearrumam como no Mac)
void DragPreview(const std::wstring& id, D2D1_POINT_2F pt) {
    float hBefore = PanelH();
    auto base = SavedIds();
    base.erase(std::remove(base.begin(), base.end(), id), base.end());
    std::vector<std::wstring> best;
    float bd = 1e18f;
    for (size_t k = 0; k <= base.size(); k++) {
        auto v = base;
        v.insert(v.begin() + k, id);
        for (auto& sl : LayoutCCFor(v, nullptr)) {
            if (id != sl.c->id) continue;
            float cx = (sl.r.left + sl.r.right) / 2, cy = (sl.r.top + sl.r.bottom) / 2;
            float d = (cx - pt.x) * (cx - pt.x) + (cy - pt.y) * (cy - pt.y);
            if (d < bd) { bd = d; best = v; }
        }
    }
    g_preview = best;
    g_dragId = id;
    g_dragPt = pt;
    g_dragActive = true;
    if (std::fabs(PanelH() - hBefore) > 0.5f) Place();  // cresceu ou encolheu
    Paint();
}
void DragCancel() {
    if (!g_dragActive && g_preview.empty()) return;
    g_dragActive = false;
    g_preview.clear();
    g_dragId.clear();
    Place();
    Paint();
}
void DragCommit() {
    if (!g_dragActive) return;
    auto v = g_preview;
    g_dragActive = false;
    g_preview.clear();
    g_dragId.clear();
    SaveLayout(v);
    Place();
    Paint();
    gal::Paint();
}
// galeria: o ponteiro (tela) arrastando o controle `id`; dentro da Central mostra onde vai cair
void GalleryDrag(const std::wstring& id, POINT screen) {
    RECT r; GetWindowRect(g_wnd, &r);
    if (!g_open || !PtInRect(&r, screen)) { DragCancel(); return; }
    float sc = Sc();
    DragPreview(id, { (screen.x - r.left) / sc, (screen.y - r.top) / sc });
}
void GalleryDrop(POINT screen) {
    RECT r; GetWindowRect(g_wnd, &r);
    if (g_dragActive && PtInRect(&r, screen)) DragCommit(); else DragCancel();
}

void StartEdit() {
    if (!Tahoe()) return;
    g_editing = true;
    Place();
    Paint();
    gal::Show();
}
void EndEdit() {
    if (!g_editing) return;
    Log(L"central: fim da edição (foco em %p, central %p, galeria %p)", GetForegroundWindow(), g_wnd, gal::w);
    g_editing = false;
    gal::Hide();
    if (g_open) { Place(); Paint(); SetForegroundWindow(g_wnd); }
}
void ToggleCtl(const std::wstring& id) {
    auto v = LayoutIds();
    auto it = std::find(v.begin(), v.end(), id);
    if (it != v.end()) v.erase(it); else v.push_back(id);
    SaveLayout(v);
    Place();
    Paint();
    gal::Paint();
}
void RunExtra(const std::wstring& id) {  // controle da galeria clicado (fora da edição)
    Close();
    auto open = [](const std::wstring& t, const wchar_t* args = nullptr) { ShellExecuteW(nullptr, L"open", t.c_str(), args, nullptr, SW_SHOWNORMAL); };
    if (id == L"calculadora") open(shell::dir + L"\\" EXE_CALCULATOR);
    else if (id == L"terminal") open(shell::dir + L"\\" EXE_TERMINAL);
    else if (id == L"monitor") shell::OpenMonitor();
    else if (id == L"bloquear") bloqueio::Lock();
    else if (id == L"mesa") mesa::WinKey('D');
    else if (id == L"launchpad") launchpad::Toggle();
    else if (id == L"spotlight") spotlight::Toggle();
    else if (id == L"lupa") open(L"magnify.exe");
    else if (id == L"teclado") open(L"osk.exe");
    else if (id == L"alarme" || id == L"cronometro" || id == L"temporizador") open(L"ms-clock:");
    else if (id == L"luznoturna") open(L"ms-settings:nightlight");
    else if (id == L"bateria") open(L"ms-settings:batterysaver");
}


void HideNow() {  // fim do fechar animado
    g_closing = false;
    ShowWindow(g_wnd, SW_HIDE);
    ShowWindow(g_back, SW_HIDE);
    veil::Hide();
}

void Close() {
    if (!g_open) return;
    if (g_editing) { Log(L"central: fechando durante a edição (foco em %p)", GetForegroundWindow()); g_editing = false; gal::Hide(); }
    g_open = false;
    g_detail = D_NONE;  // da próxima vez abre na grade
    menubar::SoundChanged(false);  // a cápsula do ícone da barra sai
    g_closedAt = GetTickCount();
    g_drag = -1;
    KillTimer(g_wnd, T_POLL);
    if (shell::Setting(L"animar_paineis", L"1") != L"1") { HideNow(); return; }
    g_closing = true;  // some na mola comum, de volta; o relógio de quadros esconde no fim
    g_closeAt = anim::Now();
    ui::FrameClock(g_wnd, WM_CC_FRAME, true);
}

void SetVolume(float v) {
    if (IAudioEndpointVolume* e = Endpoint()) {
        e->SetMasterVolumeLevelScalar(std::clamp(v, 0.f, 1.f), nullptr);
        e->SetMute(v <= 0.001f, nullptr);
        e->Release();
    }
    std::lock_guard<std::mutex> l(g_mx);
    g_st.volume = v; g_st.mute = v <= 0.001f;
}

void DragTo(float x) {
    float h = Tahoe() ? KN_W : g_dragTrack.bottom - g_dragTrack.top;  // Tahoe: a pílula nunca sai do trilho
    if (Tahoe()) {  // esticão da lente: proporcional à velocidade
        int ki = g_drag == H_BRIGHT ? 0 : 1;
        if (g_kLastX[ki] >= 0) g_kStretch[ki] = std::min(0.32f, std::max(g_kStretch[ki], std::fabs(x - g_kLastX[ki]) * 0.018f));
        g_kLastX[ki] = x;
        ui::FrameClock(g_wnd, WM_CC_FRAME, true);
    }

    float v = std::clamp((x - g_dragTrack.left - h / 2) / std::max(1.f, g_dragTrack.right - g_dragTrack.left - h), 0.f, 1.f);
    if (g_drag == H_VOLUME) SetVolume(v);
    else {
        { std::lock_guard<std::mutex> l(g_mx); g_st.bright = v; }
        g_brightWant = v;
        if (!g_brightBusy.exchange(true)) std::thread(BrightWorker).detach();
    }
    Paint();
}

void OpenSettings(const wchar_t* page) { Close(); shell::OpenSettings(page); }

void Click(D2D1_POINT_2F p) {
    const Hit* hit = nullptr;
    for (auto& h : g_hits) if (p.x >= h.r.left && p.x < h.r.right && p.y >= h.r.top && p.y < h.r.bottom) hit = &h;
    if (!hit) { if (g_detail != D_NONE) { CloseDetail(); Paint(); } return; }  // fora do painel expandido: volta
    if (g_detail != D_NONE && Tahoe()) {
        State st0; { std::lock_guard<std::mutex> l(g_mx); st0 = g_st; }
        switch (hit->id) {
        case H_DET_SWITCH:
            if (g_detail == D_WIFI && st0.radio[0] >= 0) SetRadio(0, st0.radio[0] != 1);
            if (g_detail == D_BT && st0.radio[1] >= 0) SetRadio(1, st0.radio[1] != 1);
            Paint(); return;
        case H_DET_SET: {
            const wchar_t* pg[5] = { L"wifi", L"bluetooth", L"notificacoes", L"telas", L"som" };
            OpenSettings(pg[g_detail]); return;
        }
        case H_DET_ROW:
            if (g_detail == D_WIFI) { if (hit->arg >= 0) { Close(); ShellExecuteW(nullptr, L"open", L"ms-availablenetworks:", nullptr, nullptr, SW_SHOWNORMAL); } else OpenSettings(L"wifi"); return; }
            if (g_detail == D_BT) { OpenSettings(L"bluetooth"); return; }
            if (g_detail == D_FOCO) {  // um modo: liga o Foco (notificações silenciadas) nele; o mesmo de novo desliga
                int cur = FocoModo(st0.focus);
                bool on = cur != hit->arg;
                DWORD v = on ? 0 : 1;
                RegSetKeyValueW(HKEY_CURRENT_USER, PUSH, L"ToastEnabled", REG_DWORD, &v, sizeof(v));
                if (on) shell::SetSetting(L"foco_modo", std::to_wstring(hit->arg));
                { std::lock_guard<std::mutex> l(g_mx); g_st.focus = on; }
                Paint(); return;
            }
            if (g_detail == D_SOM && hit->arg >= 0 && hit->arg < (int)g_detOuts.size()) {
                IPolicyConfig* pc = nullptr;
                if (SUCCEEDED(CoCreateInstance(CLSID_PolicyConfigClient, nullptr, CLSCTX_ALL, __uuidof(IPolicyConfig), (void**)&pc))) {
                    for (ERole r : { eConsole, eMultimedia, eCommunications }) pc->SetDefaultEndpoint(g_detOuts[hit->arg].id.c_str(), r);
                    pc->Release();
                }
                ReadFast();
                g_detOuts = Outputs();
                Paint(); return;
            }
            return;
        case H_NIGHT: Close(); ShellExecuteW(nullptr, L"open", L"ms-settings:nightlight", nullptr, nullptr, SW_SHOWNORMAL); return;
        case H_DET_PANEL: return;
        default: break;  // brilho, volume, Modo Escuro: o mesmo dos blocos
        }
    }
    if (g_editing) {  // editando: o "−", o Concluído, e pressionar um controle para arrastar
        if (hit->id != H_REMOVE && hit->id != H_EDIT) {
            float bodyH = 0;
            for (auto& sl : LayoutCC(&bodyH))
                if (p.x >= sl.r.left && p.x < sl.r.right && p.y >= sl.r.top && p.y < sl.r.bottom) { g_pressIdx = sl.idx; g_downPt = p; SetCapture(g_wnd); }
            return;
        }
        if (hit->id == H_REMOVE) {
            auto v = LayoutIds();
            if (hit->arg >= 0 && hit->arg < (int)v.size()) ToggleCtl(v[hit->arg]);
        } else if (hit->id == H_EDIT) EndEdit();
        return;
    }
    if (hit->id == H_EXTRA) {
        auto v = LayoutIds();
        if (hit->arg >= 0 && hit->arg < (int)v.size()) RunExtra(v[hit->arg]);
        return;
    }
    if (hit->id == H_EDIT && Tahoe()) { StartEdit(); return; }
    State st;
    { std::lock_guard<std::mutex> l(g_mx); st = g_st; }
    switch (hit->id) {
    case H_WIFI_BTN: if (st.radio[0] >= 0) SetRadio(0, st.radio[0] != 1); break;
    case H_BT_BTN: if (st.radio[1] >= 0) SetRadio(1, st.radio[1] != 1); break;
    case H_WIFI: if (Tahoe()) { OpenDetail(D_WIFI, hit->r); Paint(); return; } OpenSettings(L"wifi"); return;
    case H_BT: if (Tahoe()) { OpenDetail(D_BT, hit->r); Paint(); return; } OpenSettings(L"bluetooth"); return;
    case H_FOCUS_TEXT: OpenDetail(D_FOCO, hit->r); Paint(); return;
    case H_AIRDROP: Close(); ShellExecuteW(nullptr, L"open", L"ms-settings:crossdevice", nullptr, nullptr, SW_SHOWNORMAL); return;
    case H_FOCUS: {  // Foco = notificações silenciadas
        DWORD v = st.focus ? 1 : 0;
        RegSetKeyValueW(HKEY_CURRENT_USER, PUSH, L"ToastEnabled", REG_DWORD, &v, sizeof(v));
        std::lock_guard<std::mutex> l(g_mx);
        g_st.focus = !st.focus;
        break;
    }
    case H_MISSION: Close(); missioncontrol::Toggle(); return;
    case H_STAGE: palco::Toggle(); Paint(); return;
    case H_MIRROR:  // Win+K: Transmitir (o Espelhamento de Tela do Windows)
        Close();
        Key(VK_LWIN, false); Key('K', false); Key('K', true); Key(VK_LWIN, true);
        return;
    case H_DISPLAY: if (Tahoe()) { OpenDetail(D_TELA, hit->r); Paint(); return; } OpenSettings(L"telas"); return;
    case H_SOUND: if (Tahoe()) { OpenDetail(D_SOM, hit->r); Paint(); return; } OpenSettings(L"som"); return;
    case H_BRIGHT: case H_VOLUME:
        g_drag = hit->id; g_dragTrack = hit->r;
        g_kLastX[0] = g_kLastX[1] = -1; g_kFrame = anim::Now();
        ui::FrameClock(g_wnd, WM_CC_FRAME, true);  // a pílula vira lente
        SetCapture(g_wnd);
        DragTo(p.x);
        return;
    case H_OUTPUT: {
        auto outs = Outputs();
        std::vector<ctxmenu::Item> items;
        ctxmenu::Item head; head.label = Tr(L"Saída"); head.disabled = true; items.push_back(head);
        for (size_t i = 0; i < outs.size(); i++) { ctxmenu::Item it; it.label = outs[i].name; it.id = (int)i + 1; it.checked = outs[i].def; items.push_back(it); }
        ctxmenu::Item sep; sep.separator = true; items.push_back(sep);
        ctxmenu::Item more; more.label = Tr(L"Ajustes de Som…"); more.id = 1000; items.push_back(more);
        RECT wr; GetWindowRect(g_wnd, &wr);
        float sc = Sc();
        POINT pt{ wr.left + (LONG)(hit->r.left * sc), wr.top + (LONG)(hit->r.bottom * sc) };
        g_menu = true;
        int id = ctxmenu::ShowItems(g_wnd, pt, items);
        g_menu = false;
        if (id == 1000) { OpenSettings(L"som"); return; }
        if (id > 0 && id <= (int)outs.size()) {
            IPolicyConfig* pc = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_PolicyConfigClient, nullptr, CLSCTX_ALL, __uuidof(IPolicyConfig), (void**)&pc))) {
                for (ERole r : { eConsole, eMultimedia, eCommunications }) pc->SetDefaultEndpoint(outs[id - 1].id.c_str(), r);
                pc->Release();
            }
            ReadFast();
        }
        SetForegroundWindow(g_wnd);
        break;
    }
    case H_PLAY: MediaCommand(false); break;
    case H_NEXT: MediaCommand(true); break;
    case H_EDIT: OpenSettings(L"central"); return;
    case H_DARK: {  // Modo Escuro: o tema do Windows (como o botão de Aparência), e o Shell segue o Windows
        DWORD light = ui::Dark() ? 1 : 0;
        HKEY k;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", 0, KEY_SET_VALUE, &k) == ERROR_SUCCESS) {
            RegSetValueExW(k, L"AppsUseLightTheme", 0, REG_DWORD, (BYTE*)&light, sizeof(light));
            RegSetValueExW(k, L"SystemUsesLightTheme", 0, REG_DWORD, (BYTE*)&light, sizeof(light));
            RegCloseKey(k);
        }
        shell::SetSetting(L"tema", L"auto");
        std::thread([] { SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"ImmersiveColorSet", SMTO_ABORTIFHUNG, 200, nullptr); }).detach();
        break;
    }
    case H_SHOT: {  // Captura de Tela (a barra do Cmd+Shift+5)
        Close();
        static UINT msg = RegisterWindowMessageW(L"SequoiaCaptura");
        if (HWND w = FindWindowW(L"SequoiaCapturaMsg", nullptr)) {
            DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(w, msg, 5, 0);
        } else ShellExecuteW(nullptr, L"open", (shell::dir + L"\\" EXE_SCREENSHOT).c_str(), L"/5", nullptr, SW_SHOWNORMAL);
        return;
    }
    default: break;
    }
    Paint();
}

D2D1_POINT_2F Pt(LPARAM l) { float sc = Sc(); return { (short)LOWORD(l) / sc, (short)HIWORD(l) / sc }; }

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); Paint(); return 0; }
    case WM_CC_FRAME: {
        bool knobs = StepKnobs();
        {   // a luz do brilho anda atrás do ponteiro
            double now = anim::Now();
            bool luz = g_ccLuz.Passo(g_ccHover.x, g_ccHover.y, (float)((now - g_ccLuzAt) / 1000.0));
            g_ccLuzAt = now;
            knobs |= luz;
        }
        Paint();
        if (g_closing && Motion().done) { HideNow(); ui::FrameClock(h, WM_CC_FRAME, false); }
        else if (g_open && Motion().done && !knobs && !(g_detail >= 0 && anim::Now() - g_detailAt < 450) && g_ccPress.x < 0 && PressGrow() < 0.001f)
            ui::FrameClock(h, WM_CC_FRAME, false);
    }
        return 0;
    case WM_CC_UPDATE: if (w == 1 && g_detail >= 0) Place(); Paint(); return 0;  // w 1: a lista do módulo expandido chegou
    case veil::WM_VEIL_CLICK: Close(); return 0;
    case WM_TIMER:
        if (w == T_POLL && g_open && g_drag < 0) { ReadFast(); ReadMedia(); Paint(); }
        return 0;
    case WM_MOUSELEAVE: g_ccHover = { -1, -1 }; if (Tahoe()) Paint(); return 0;
    case WM_LBUTTONDOWN:
        if (Tahoe() && !g_editing) { g_ccPress = Pt(l); g_ccPressAt = anim::Now(); ui::FrameClock(h, WM_CC_FRAME, true); }
        Click(Pt(l)); return 0;
    case WM_MOUSEMOVE:
        if (Tahoe()) {  // o brilho segue o ponteiro
            D2D1_POINT_2F pt = Pt(l);
            if (g_ccHover.x < 0) { TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 }; TrackMouseEvent(&tme); }
            g_ccHover = pt;
            if (g_drag < 0) { g_ccLuzAt = anim::Now(); ui::FrameClock(h, WM_CC_FRAME, true); }  // a luz persegue o ponteiro
        }
        if (g_drag >= 0 && (w & MK_LBUTTON)) DragTo(Pt(l).x);
        else if (g_editing && g_pressIdx >= 0 && (w & MK_LBUTTON)) {  // arrastando um controle da Central
            D2D1_POINT_2F pt = Pt(l);
            if (!g_dragActive && std::hypot(pt.x - g_downPt.x, pt.y - g_downPt.y) < 6) return 0;
            auto v = SavedIds();
            if (g_pressIdx < (int)v.size()) DragPreview(v[g_pressIdx], pt);
        }
        return 0;
    case WM_LBUTTONUP:
        if (g_ccPress.x >= 0) { g_ccPress = { -1, -1 }; g_ccReleaseAt = anim::Now(); ui::FrameClock(h, WM_CC_FRAME, true); }
        if (g_pressIdx >= 0) {
            g_pressIdx = -1;
            if (GetCapture() == h) ReleaseCapture();
            if (g_dragActive) DragCommit();
            return 0;
        }
        if (g_drag >= 0) {
            if (g_drag == H_VOLUME) shell::Sound(L"Pop");  // como no Mac: o estalo no volume novo ao soltar
            g_drag = -1; ReleaseCapture(); g_kFrame = anim::Now();
            ui::FrameClock(h, WM_CC_FRAME, true);  // a lente volta a ser pílula
            Paint();
        }
        return 0;
    case WM_MOUSEWHEEL: {  // rolar em cima do painel muda o volume
        State st; { std::lock_guard<std::mutex> lk(g_mx); st = g_st; }
        SetVolume(st.volume + GET_WHEEL_DELTA_WPARAM(w) / 120.f * 0.04f);
        Paint();
        return 0;
    }
    case WM_KEYDOWN: if (w == VK_ESCAPE) { if (g_editing) EndEdit(); else if (g_detail >= 0) { CloseDetail(); Paint(); } else Close(); } return 0;
    case WM_ACTIVATE: if (LOWORD(w) == WA_INACTIVE && !g_menu && !(g_editing && ((HWND)l == gal::w || (HWND)l == gal::back))) Close(); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK BackProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

bool central::Create(HINSTANCE hi) {
    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = hi; wc.lpszClassName = L"ShellCentral";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    WNDCLASSW bc{}; bc.lpfnWndProc = BackProc; bc.hInstance = hi; bc.lpszClassName = L"ShellCentralFundo";
    RegisterClassW(&bc);
    g_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, L"Central de Controle", WS_POPUP,
                            0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
    // vidro com a curva do painel (o mesmo do Dock); sem a Composição, o acrílico com canto do DWM
    g_back = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, bc.lpszClassName, L"", WS_POPUP,
                             0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
    if (!g_wnd || !g_back) return false;
    g_glass = backdrop::Attach(g_back);
    if (g_glass) tglass::Init(g_back);  // vidro por módulo do Tahoe (por cima do vidro do backdrop)
    if (g_glass) veil::Create(hi);      // véu embaçado atrás da Central (Tahoe)


    if (!g_glass) {
        DestroyWindow(g_back);
        g_back = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, bc.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
        DWORD corner = 2; DwmSetWindowAttribute(g_back, 33, &corner, sizeof(corner));
        ui::Acrylic(g_back, ui::Dark() ? 0x40201E1Eu : 0x40F6F6F6u);
    } else {
        DWORD corner = 1; DwmSetWindowAttribute(g_back, 33, &corner, sizeof(corner));
    }
    COLORREF none = 0xFFFFFFFE;
    DwmSetWindowAttribute(g_back, 34, &none, sizeof(none));
    apps::ours.push_back(g_wnd);
    apps::ours.push_back(g_back);
    g_s.hwnd = g_wnd;
    g_tfBold = ui::Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    g_tfSmall = ui::Format(11, DWRITE_FONT_WEIGHT_NORMAL);
    g_tfTile = ui::Format(11, DWRITE_FONT_WEIGHT_MEDIUM, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfGlyph = ui::Format(14, DWRITE_FONT_WEIGHT_NORMAL, true, DWRITE_TEXT_ALIGNMENT_CENTER);
    for (auto* tf : { g_tfBold, g_tfSmall, g_tfGlyph }) if (tf) { tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); tf->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); }
    if (g_tfTile) g_tfTile->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    DWRITE_TRIMMING tr{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
    for (auto* tf : { g_tfBold, g_tfSmall }) if (tf) {  // texto que não cabe termina em "…" (como no Mac), não cortado no meio
        IDWriteInlineObject* dots = nullptr;
        if (ui::dw) ui::dw->CreateEllipsisTrimmingSign(tf, &dots);
        tf->SetTrimming(&tr, dots);
        if (dots) dots->Release();
    }
    std::thread(LoadRadios).detach();
    gal::Create(hi);  // a galeria do Editar Controles (Tahoe)
    return true;
}

bool central::IsOpen() { return g_open; }

void central::Toggle() {
    if (g_open) { Close(); return; }
    if (GetTickCount() - g_closedAt < 300) return;  // o clique no ícone acabou de fechar (perdeu o foco): não reabre
    ReadFast();
    ReadMedia();
    // brilho: lido numa thread (DDC/CI demora); o painel abre já e o controle aparece quando chegar
    std::thread([] {
        float b = -1;
        WithMonitor([&](HANDLE h) { DWORD mn = 0, cur = 0, mx = 100; if (GetMonitorBrightness(h, &mn, &cur, &mx) && mx > mn) b = (float)(cur - mn) / (mx - mn); });
        { std::lock_guard<std::mutex> l(g_mx); g_st.bright = b; }
        if (g_wnd) PostMessageW(g_wnd, WM_CC_UPDATE, 0, 0);
    }).detach();
    g_open = true;
    menubar::SoundChanged(false);  // o ícone da barra ganha a cápsula do Tahoe
    g_closing = false;
    g_openAt = anim::Now();
    Place();
    Paint();
    ShowWindow(g_wnd, SW_SHOW);
    SetForegroundWindow(g_wnd);
    SetFocus(g_wnd);
    SetTimer(g_wnd, T_POLL, 1000, nullptr);
    ui::FrameClock(g_wnd, WM_CC_FRAME, true);
}
