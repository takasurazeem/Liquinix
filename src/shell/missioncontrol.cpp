// Mission Control: as janelas da mesa atual espalhadas sem se sobrepor (miniaturas vivas do DWM,
// perto de onde já estavam), a barra de Mesas no topo e o Dock por cima, como no macOS Sequoia.
// As Mesas são as áreas de trabalho virtuais do Windows (trocar e criar com Win+Ctrl+setas/D).
// Abrir e fechar são feitos pelo compositor do Windows (DirectComposition): o papel de parede e o
// escurecer são camadas paradas, e as janelas viram fotos (foto.cpp) que a mola leva sozinha, na placa de
// vídeo, sem quadros do Shell. Quando assentam, as fotos dão lugar às miniaturas vivas do DWM.
// Ajuste: mc_motor=composicao (padrão) ou antigo (quadro a quadro, como antes).
#include "shell.h"
#include <thread>
#include <mutex>
#include <dwmapi.h>
#include <dcomp.h>
#include <shobjidl.h>
#include <algorithm>
#include "../comum/idioma.h"

namespace {

const UINT WM_MC_FRAME = WM_APP + 60;
const float ANIM_MS = 300;
// mola suave do Mac (sem quique) para as janelas e a barra; o relógio é o de alta precisão (anim.cpp)
const float RESPONSE = 0.40f, DAMPING = 0.92f;
float g_durMs = 450;
const ULONG_PTR KEY_MARK = 0x53484C4C;  // teclas injetadas pelo shell (o gancho deixa passar)

struct Win {
    HWND h;
    HTHUMBNAIL th = nullptr;
    HTHUMBNAIL mini = nullptr;  // miniatura dentro da Mesa atual, na barra aberta
    RECT frame{};        // onde a janela está na tela (px, sem a borda invisível)
    RECT src{};          // parte da janela mostrada (sem a borda invisível)
    D2D1_RECT_F from{}, to{};
    std::wstring title;
    // compositor: a foto da janela
    IDCompositionSurface* snap = nullptr;
    IDCompositionVisual* sv = nullptr;
    IDCompositionScaleTransform* ss = nullptr;
    UINT sw = 0, sh = 0;
};

HWND g_wnd;
ui::GpuSurface g_s;
bool g_open, g_closing;
double g_animAt;         // início da animação atual (abrir, barra crescer, fechar), em ms de anim::Now
double g_openAt;         // quando abriu (o fundo escurece junto)
std::vector<Win> g_wins;
int g_hover = -1;
HWND g_pick;             // janela escolhida: vai para a frente depois de fechar
ID2D1Bitmap* g_wall;
// papel de parede das miniaturas das Mesas (na faixa de cima): sempre carregado, também no modo compositor
// (lá o fundo é outra camada e g_wall fica vazio: as miniaturas saíam sem fundo). Lido numa thread.
ID2D1Bitmap* g_deskWall;
std::mutex g_deskMx;
std::vector<BYTE> g_deskPx; UINT g_deskW, g_deskH; bool g_deskReady; int g_deskGen;
RECT g_mon{};
float g_bar = 0, g_barFrom = 0, g_barTo = 0;  // 0 = barra fina (só nomes), 1 = aberta (miniaturas)
IDWriteTextFormat *g_tfDesk, *g_tfTitle, *g_tfPlus;

// Arrastar uma janela até uma Mesa da faixa de cima leva a janela para lá (como no Mac). A janela segue o
// ponteiro e encolhe chegando perto da faixa; solta na Mesa, ela voa até a miniatura e some dali.
int g_drag = -1;            // janela pressionada
bool g_dragging;            // já mexeu (senão é um clique)
D2D1_POINT_2F g_dragStart{}, g_dragGrab{};  // onde apertou; ponto da janela sob o ponteiro (0..1)
D2D1_RECT_F g_dragBase{};   // tamanho da janela quando começou a arrastar (px)
int g_dropDesk = -1;        // Mesa sob o ponteiro, arrastando
struct Fly { HWND h; HTHUMBNAIL th; RECT src; D2D1_RECT_F from, to; double t0; };
std::vector<Fly> g_fly;     // janelas indo para outra Mesa
const double FLY_MS = 380;

struct Desk { std::wstring name; };
std::vector<Desk> g_desks;
int g_cur = 0;
std::wstring g_appId, g_appName;  // Exposé do app: só as janelas deste app (vazio = Mission Control)
bool g_appNext;  // ToggleApp pediu: a abertura que vem é o Exposé do app
// a faixa de Mesas (no Exposé do app ela não existe; no Leopard também não: o Exposé do 10.5 só espalha as janelas)
bool Bar() { return g_appId.empty() && !liquid::Leopard(); }
int g_hoverDesk = -1;     // Mesa sob o mouse na barra aberta (mostra o X)
bool g_hoverX = false;    // mouse em cima do X
HWND g_xWnd, g_xFor, g_xPending;  // X de fechar a janela sob o mouse (mais abaixo)
bool g_xHot;

// ---- compositor
const UINT_PTR T_COMP = 7;
IDCompositionDevice* g_cd;
IDCompositionTarget* g_ct;         // por baixo da superfície da barra (que é a de cima)
IDCompositionVisual *g_croot, *g_cblack, *g_cwall, *g_cwins;
IDCompositionEffectGroup *g_wallFx, *g_barFx;
IDCompositionScaleTransform* g_blackSc;
IDCompositionSurface* g_wallSurf;
std::wstring g_wallKey;
bool g_comp;                       // esta abertura usa o compositor
bool g_compAnim;                   // as fotos estão andando (miniaturas vivas escondidas)
std::vector<HWND> g_liveHs;        // a ordem das capturas abertas
foto::Live* g_live;                // capturas abertas enquanto o Mission Control está na tela (fechar sem espera)

float Ease(float t) { t = std::clamp(t, 0.f, 1.f); return 1 - (1 - t) * (1 - t) * (1 - t); }
// Tahoe (guia macOS26-Theme-Reference, seções 38 e 44, medido no WindowServer): entrar = 0,17 s numa mola de
// resposta 0,36 e amortecimento 0,66 (quase linear de tão rápida); sair = 0,28 s, ease-in-out de ~0,24 s
float Curve(float s) {
    if (anim::reduceMotion) return 1;  // Reduzir Movimento: as janelas vão direto para o lugar (o fundo ainda faz fade)
    if (liquid::On()) return g_closing ? anim::EaseInOut(std::min(1.f, s / 0.24f)) : anim::Spring(s, 0.36f, 0.66f);
    return anim::Spring(s, RESPONSE, DAMPING);
}
float Progress() { return std::min(1.f, Curve((float)((anim::Now() - g_animAt) / 1000.0))); }
float W() { return (float)(g_mon.right - g_mon.left); }
float H() { return (float)(g_mon.bottom - g_mon.top); }
float Sc() { return ui::Scale(g_wnd); }

D2D1_RECT_F Lerp(const D2D1_RECT_F& a, const D2D1_RECT_F& b, float t) {
    return { a.left + (b.left - a.left) * t, a.top + (b.top - a.top) * t, a.right + (b.right - a.right) * t, a.bottom + (b.bottom - a.bottom) * t };
}
D2D1_RECT_F Current(const Win& w) { return Lerp(w.from, w.to, Progress()); }
float BarNow() { return g_barFrom + (g_barTo - g_barFrom) * Progress(); }
D2D1_RECT_F FrameF(const Win& w) {
    return { (float)(w.frame.left - g_mon.left), (float)(w.frame.top - g_mon.top), (float)(w.frame.right - g_mon.left), (float)(w.frame.bottom - g_mon.top) };
}

// ---- Mesas (áreas de trabalho virtuais): lidas do registro do Explorer
void LoadDesks() {
    g_desks.clear();
    g_cur = 0;
    const wchar_t* key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops";
    BYTE ids[16 * 32]; DWORD cb = sizeof(ids);
    BYTE cur[16]; DWORD cc = sizeof(cur);
    bool hasCur = RegGetValueW(HKEY_CURRENT_USER, key, L"CurrentVirtualDesktop", RRF_RT_REG_BINARY, nullptr, cur, &cc) == ERROR_SUCCESS && cc == 16;
    if (RegGetValueW(HKEY_CURRENT_USER, key, L"VirtualDesktopIDs", RRF_RT_REG_BINARY, nullptr, ids, &cb) == ERROR_SUCCESS && cb >= 16) {
        for (DWORD i = 0; i + 16 <= cb; i += 16) {
            GUID g; memcpy(&g, ids + i, 16);
            wchar_t gs[64]; StringFromGUID2(g, gs, 64);
            std::wstring sub = std::wstring(key) + L"\\Desktops\\" + gs;
            wchar_t name[128] = {}; DWORD nb = sizeof(name);
            std::wstring n = RegGetValueW(HKEY_CURRENT_USER, sub.c_str(), L"Name", RRF_RT_REG_SZ, nullptr, name, &nb) == ERROR_SUCCESS && name[0]
                                 ? name : Tr(L"Mesa ") + std::to_wstring(i / 16 + 1);
            if (hasCur && !memcmp(cur, ids + i, 16)) g_cur = (int)g_desks.size();
            g_desks.push_back({ n });
        }
    }
    if (g_desks.empty()) g_desks.push_back({ Tr(L"Mesa 1") });
}

void Key(WORD vk, bool up) {
    INPUT in{}; in.type = INPUT_KEYBOARD; in.ki.wVk = vk; in.ki.dwExtraInfo = KEY_MARK;
    in.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (vk == VK_LEFT || vk == VK_RIGHT ? KEYEVENTF_EXTENDEDKEY : 0);
    SendInput(1, &in, sizeof(in));
}
void WinCtrl(WORD vk) {  // Win+Ctrl+tecla: o Windows troca ou cria a área de trabalho virtual
    Key(VK_LWIN, false); Key(VK_LCONTROL, false); Key(vk, false); Key(vk, true); Key(VK_LCONTROL, true); Key(VK_LWIN, true);
}

// ---- Janelas da mesa atual
bool Wanted(HWND h, IVirtualDesktopManager* vdm) {
    if (!IsWindowVisible(h) || IsIconic(h) || apps::IsShellWindow(h)) return false;
    LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    if ((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW)) return false;
    bool lone = GetWindow(h, GW_OWNER) && !(ex & WS_EX_APPWINDOW) && apps::LoneOwned(h);
    if (GetWindow(h, GW_OWNER) && !(ex & WS_EX_APPWINDOW) && !lone) return false;
    DWORD cloaked = 0;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return false;
    if (GetWindowTextLengthW(h) == 0 && !lone) return false;
    wchar_t cls[64]; GetClassNameW(h, cls, 64);
    if (mesa::IsDeskClass(cls) || !wcscmp(cls, L"Shell_TrayWnd")) return false;
    BOOL on = TRUE;
    if (vdm && SUCCEEDED(vdm->IsWindowOnCurrentVirtualDesktop(h, &on)) && !on) return false;
    RECT r;
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) GetWindowRect(h, &r);
    if (r.right - r.left < 60 || r.bottom - r.top < 40) return false;
    RECT x;
    return IntersectRect(&x, &r, &g_mon) != FALSE;
}

void Collect() {
    for (auto& w : g_wins) { if (w.th) DwmUnregisterThumbnail(w.th); if (w.mini) DwmUnregisterThumbnail(w.mini); }
    g_wins.clear();
    IVirtualDesktopManager* vdm = nullptr;
    if (!mesa::Standalone())  // sem o Explorador não há mesas virtuais (a interface é dele)
        CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&vdm));
    std::vector<HWND> hs;
    for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT))
        if (Wanted(h, vdm) && (Bar() || apps::IdOfWindow(h) == g_appId)) hs.push_back(h);
    if (vdm) vdm->Release();
    // registra de baixo para cima: a miniatura registrada por último fica por cima
    for (auto it = hs.rbegin(); it != hs.rend(); ++it) {
        Win w; w.h = *it;
        RECT wr; GetWindowRect(w.h, &wr);
        if (FAILED(DwmGetWindowAttribute(w.h, DWMWA_EXTENDED_FRAME_BOUNDS, &w.frame, sizeof(w.frame)))) w.frame = wr;
        w.src = { w.frame.left - wr.left, w.frame.top - wr.top, w.frame.right - wr.left, w.frame.bottom - wr.top };
        wchar_t t[256]; GetWindowTextW(w.h, t, 256); w.title = t;
        if (FAILED(DwmRegisterThumbnail(g_wnd, w.h, &w.th))) continue;
        DwmRegisterThumbnail(g_wnd, w.h, &w.mini);
        wchar_t cls[64]; GetClassNameW(w.h, cls, 64);
        Log(L"mission control: janela \"%ls\" (%ls) %dx%d ex=%08X", w.title.c_str(), cls, w.frame.right - w.frame.left, w.frame.bottom - w.frame.top,
            (unsigned)GetWindowLongW(w.h, GWL_EXSTYLE));
        g_wins.push_back(w);
    }
}

// Topo da área das janelas: abaixo da barra de Mesas (fina ou aberta)
float BarHeightPx(float bar) { return Bar() ? (54 + (190 - 54) * bar) * Sc() : 64 * Sc(); }  // Exposé do app: só o nome em cima

// Arrumação como a do Mac: as janelas em linhas, na ordem em que estão na tela (de cima para baixo,
// da esquerda para a direita). Testa de 1 a n linhas e fica com a que deixa as janelas maiores.
void Layout(float bar) {
    size_t n = g_wins.size();
    if (!n) return;
    float top = BarHeightPx(bar) + 14 * Sc();
    // o Dock embaixo tira espaço de baixo; à esquerda ou à direita, daquele lado (Ajustes > Posição na tela)
    int dpos = dock::Posicao();
    float dockB = dpos == 0 ? (float)dock::ReservedHeight() : 0.f, dockL = dpos == 1 ? (float)dock::ReservedHeight() : 0.f, dockR = dpos == 2 ? (float)dock::ReservedHeight() : 0.f;
    float bottom = H() - dockB - 18 * Sc();
    D2D1_RECT_F area = { dockL + 28 * Sc(), top, W() - dockR - 28 * Sc(), bottom };
    if (liquid::On()) {  // Tahoe (guia, seção 38, medido): 20 dos lados, de y 80 até ~104 da borda de baixo
        area.left = dockL + 20 * Sc(); area.right = W() - dockR - 20 * Sc();
        area.top = std::max(top, 80 * Sc());
        area.bottom = H() - (dpos == 0 ? 104 * Sc() : 24 * Sc());
    }
    float aw = area.right - area.left, ah = area.bottom - area.top, gap = 34 * Sc();
    std::vector<size_t> order(n);
    for (size_t i = 0; i < n; i++) order[i] = i;
    auto cy = [&](size_t i) { return (g_wins[i].frame.top + g_wins[i].frame.bottom) / 2.f; };
    auto cx = [&](size_t i) { return (g_wins[i].frame.left + g_wins[i].frame.right) / 2.f; };
    std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) { return cy(x) < cy(y); });
    auto ww = [&](size_t i) { return (float)(g_wins[i].frame.right - g_wins[i].frame.left); };
    auto wh = [&](size_t i) { return (float)(g_wins[i].frame.bottom - g_wins[i].frame.top); };
    int bestRows = 1; float bestScore = -1;
    for (int rows = 1; rows <= (int)n; rows++) {
        int cols = (int)((n + rows - 1) / rows);
        float cw = (aw - gap * (cols - 1)) / cols, ch = (ah - gap * (rows - 1)) / rows;
        if (cw <= 0 || ch <= 0) continue;
        float score = 0;
        for (size_t i = 0; i < n; i++) { float k = std::min({ cw / ww(i), ch / wh(i), 0.86f }); score += ww(i) * wh(i) * k * k; }
        if (score > bestScore * 1.02f) { bestScore = score; bestRows = rows; }
    }
    int rows = bestRows, cols = (int)((n + rows - 1) / rows);
    float cw = (aw - gap * (cols - 1)) / cols, ch = (ah - gap * (rows - 1)) / rows;
    for (int r = 0; r < rows; r++) {
        size_t a = (size_t)r * cols, b = std::min(n, a + cols);
        if (a >= n) break;
        std::vector<size_t> row(order.begin() + a, order.begin() + b);
        std::stable_sort(row.begin(), row.end(), [&](size_t x, size_t y) { return cx(x) < cx(y); });
        // largura real da linha (janelas encolhidas) para centralizar a linha incompleta
        std::vector<float> ks(row.size());
        float total = 0;
        for (size_t j = 0; j < row.size(); j++) { ks[j] = std::min({ cw / ww(row[j]), ch / wh(row[j]), 0.86f }); total += ww(row[j]) * ks[j]; }
        total += gap * (row.size() - 1);
        float x = area.left + (aw - total) / 2;
        float yc = area.top + r * (ch + gap) + ch / 2;
        for (size_t j = 0; j < row.size(); j++) {
            float w2 = ww(row[j]) * ks[j], h2 = wh(row[j]) * ks[j];
            g_wins[row[j]].to = { x, yc - h2 / 2, x + w2, yc + h2 / 2 };
            x += w2 + gap;
        }
    }
    // trava: tudo dentro da área (nada cortado na borda nem por baixo do Dock)
    D2D1_RECT_F bb = g_wins[0].to;
    for (auto& w : g_wins) { bb.left = std::min(bb.left, w.to.left); bb.top = std::min(bb.top, w.to.top); bb.right = std::max(bb.right, w.to.right); bb.bottom = std::max(bb.bottom, w.to.bottom); }
    float k = std::min({ 1.f, aw / std::max(1.f, bb.right - bb.left), ah / std::max(1.f, bb.bottom - bb.top) });
    if (k < 0.999f || bb.left < area.left - 0.5f || bb.top < area.top - 0.5f || bb.right > area.right + 0.5f || bb.bottom > area.bottom + 0.5f) {
        Log(L"mission control: arrumação passou da área (%.0f,%.0f,%.0f,%.0f em %.0f,%.0f,%.0f,%.0f); encolhendo", bb.left, bb.top, bb.right, bb.bottom, area.left, area.top, area.right, area.bottom);
        float bcx = (bb.left + bb.right) / 2, bcy = (bb.top + bb.bottom) / 2, acx = (area.left + area.right) / 2, acy = (area.top + area.bottom) / 2;
        for (auto& w : g_wins) {
            auto f = [&](float v, float c0, float c1) { return c1 + (v - c0) * k; };
            w.to = { f(w.to.left, bcx, acx), f(w.to.top, bcy, acy), f(w.to.right, bcx, acx), f(w.to.bottom, bcy, acy) };
        }
    }
}

void CompStop();
void Retarget(float bar) {  // nova posição a partir de onde estão agora (barra cresceu/encolheu)
    for (auto& w : g_wins) w.from = Current(w);
    CompStop();
    g_barFrom = BarNow();
    g_barTo = bar;
    Layout(bar);
    g_animAt = anim::Now();
    ui::FrameClock(g_wnd, WM_MC_FRAME, true);
}

D2D1_RECT_F DeskThumbRect(int i, float sc);

void UpdateFly() {
    double now = anim::Now();
    for (auto& f : g_fly) {
        float u = (float)std::min(1.0, (now - f.t0) / FLY_MS), e = 1 - (1 - u) * (1 - u) * (1 - u);
        D2D1_RECT_F c = Lerp(f.from, f.to, e);
        DWM_THUMBNAIL_PROPERTIES p{};
        p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
        p.rcDestination = { (LONG)std::lround(c.left), (LONG)std::lround(c.top), (LONG)std::lround(c.right), (LONG)std::lround(c.bottom) };
        p.rcSource = f.src;
        p.fVisible = TRUE;
        p.opacity = (BYTE)std::lround(255 * (1 - 0.6f * u));
        DwmUpdateThumbnailProperties(f.th, &p);
    }
    for (size_t i = 0; i < g_fly.size();)
        if (now - g_fly[i].t0 > FLY_MS + 40) { DwmUnregisterThumbnail(g_fly[i].th); g_fly.erase(g_fly.begin() + i); }
        else i++;
}

void UpdateThumbs() {
    UpdateFly();
    float bar = BarNow(), a = std::clamp(bar * 2 - 1, 0.f, 1.f), sc = Sc();
    D2D1_RECT_F dr = DeskThumbRect(g_cur, sc);
    for (auto& w : g_wins) {
        if (!w.mini) continue;
        DWM_THUMBNAIL_PROPERTIES p{};
        p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
        float kx = (dr.right - dr.left) * sc / W(), ky = (dr.bottom - dr.top) * sc / H();
        D2D1_RECT_F f = FrameF(w);
        p.rcDestination = { (LONG)std::lround(dr.left * sc + f.left * kx), (LONG)std::lround(dr.top * sc + f.top * ky),
                            (LONG)std::lround(dr.left * sc + f.right * kx), (LONG)std::lround(dr.top * sc + f.bottom * ky) };
        p.rcSource = w.src;
        p.fVisible = a > 0.01f && !g_closing;
        p.opacity = (BYTE)std::lround(255 * a);
        p.fSourceClientAreaOnly = FALSE;
        DwmUpdateThumbnailProperties(w.mini, &p);
    }
    for (auto& w : g_wins) {
        D2D1_RECT_F c = Current(w);
        DWM_THUMBNAIL_PROPERTIES p{};
        p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
        p.rcDestination = { (LONG)std::lround(c.left), (LONG)std::lround(c.top), (LONG)std::lround(c.right), (LONG)std::lround(c.bottom) };
        p.rcSource = w.src;
        p.fVisible = !g_compAnim;
        p.opacity = 255;
        p.fSourceClientAreaOnly = FALSE;
        DwmUpdateThumbnailProperties(w.th, &p);
    }
}

// ---- Barra de Mesas
D2D1_RECT_F DeskLabelRect(int i, float sc) {  // barra fina: nomes centralizados
    float w = 0;
    for (auto& d : g_desks) w += ui::TextWidth(g_tfDesk, d.name) + 28;
    float x = W() / sc / 2 - w / 2;
    for (int k = 0; k < i; k++) x += ui::TextWidth(g_tfDesk, g_desks[k].name) + 28;
    return { x, 10, x + ui::TextWidth(g_tfDesk, g_desks[i].name) + 24, 38 };
}
D2D1_RECT_F DeskThumbRect(int i, float sc) {  // barra aberta: miniaturas das mesas
    float tw = 180, th = tw * H() / W(), gap = 24;
    float total = g_desks.size() * tw + (g_desks.size() - 1) * gap;
    float x = W() / sc / 2 - total / 2 + i * (tw + gap);
    return { x, 26, x + tw, 26 + th };
}
D2D1_RECT_F PlusRect(float sc) { return { W() / sc - 58, 8, W() / sc - 18, 48 }; }
D2D1_POINT_2F CloseCenter(int i, float sc) { D2D1_RECT_F r = DeskThumbRect(i, sc); return { r.left + 3, r.top + 3 }; }
bool OverClose(int i, D2D1_POINT_2F d, float sc) {
    D2D1_POINT_2F c = CloseCenter(i, sc);
    return (d.x - c.x) * (d.x - c.x) + (d.y - c.y) * (d.y - c.y) <= 13 * 13;
}
int HitDesk(D2D1_POINT_2F d, float sc) {  // barra aberta: miniatura (com o nome embaixo) ou o X dela
    for (int i = 0; i < (int)g_desks.size(); i++) {
        D2D1_RECT_F r = DeskThumbRect(i, sc);
        if ((d.x >= r.left - 12 && d.x < r.right && d.y >= r.top - 12 && d.y < r.bottom + 30) || OverClose(i, d, sc)) return i;
    }
    return -1;
}

void PlaceX();

void Paint() {
    if (!g_wnd || !g_open) return;
    float sc = Sc();
    auto* dc = ui::GpuBegin(g_s, (int)W(), (int)H(), sc);
    if (!dc) return;
    float Wd = W() / sc, Hd = H() / sc;
    // fundo: o papel de parede, escurecendo um pouco (no fechar, clareia de volta)
    float dim = g_closing ? 1 - Progress() : anim::EaseOut((float)((anim::Now() - g_openAt) / ANIM_MS));
    if (g_comp) dim = 1;
    if (g_comp) {
    } else if (g_wall) {
        auto s = g_wall->GetSize();
        float k = std::max(Wd / s.width, Hd / s.height);
        dc->DrawBitmap(g_wall, D2D1::RectF((Wd - s.width * k) / 2, (Hd - s.height * k) / 2, (Wd + s.width * k) / 2, (Hd + s.height * k) / 2), 1,
                       D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    } else dc->Clear(D2D1::ColorF(0.08f, 0.12f, 0.28f, 1));
    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, (liquid::Leopard() ? 0.45f : 0.22f) * dim), &br);  // Leopard: escurece mais (cinza, sem desfoque)
    if (!g_comp) dc->FillRectangle(D2D1::RectF(0, 0, Wd, Hd), br);

    if (!Bar()) {  // Exposé do app: o nome do app no alto, no lugar da faixa de Mesas
        D2D1_LAYER_PARAMETERS lpa = D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), dim);
        ID2D1Layer* la = nullptr; dc->CreateLayer(nullptr, &la);
        dc->PushLayer(lpa, la);
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.92f));
        if (!g_appId.empty()) dc->DrawTextW(g_appName.c_str(), (UINT32)g_appName.size(), g_tfDesk, D2D1::RectF(0, 18, Wd, 50), br);
        dc->PopLayer(); la->Release();
        goto windows;
    }
    {
    // barra de Mesas
    float bar = BarNow();
    float bh = BarHeightPx(bar) / sc;
    br->SetColor(D2D1::ColorF(0, 0, 0, (0.10f + 0.12f * bar) * dim));
    dc->FillRectangle(D2D1::RectF(0, 0, Wd, bh), br);
    D2D1_LAYER_PARAMETERS lp = D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), dim);
    ID2D1Layer* layer = nullptr;
    dc->CreateLayer(nullptr, &layer);
    dc->PushLayer(lp, layer);
    if (bar < 0.5f) {
        float a = 1 - bar * 2;
        for (int i = 0; i < (int)g_desks.size(); i++) {
            D2D1_RECT_F r = DeskLabelRect(i, sc);
            if (i == g_cur) { br->SetColor(D2D1::ColorF(1, 1, 1, 0.16f * a)); dc->FillRoundedRectangle(D2D1::RoundedRect(r, 7, 7), br); }
            br->SetColor(D2D1::ColorF(1, 1, 1, 0.92f * a));
            dc->DrawTextW(g_desks[i].name.c_str(), (UINT32)g_desks[i].name.size(), g_tfDesk, r, br);
        }
    } else {
        float a = bar * 2 - 1;
        for (int i = 0; i < (int)g_desks.size(); i++) {
            D2D1_RECT_F r = DeskThumbRect(i, sc);
            if (!g_deskWall) {  // a thread já leu o arquivo: vira bitmap aqui (contexto da GPU desta thread)
                std::lock_guard<std::mutex> lk(g_deskMx);
                if (g_deskReady && !g_deskPx.empty()) { g_deskWall = ui::Bitmap(ui::Gpu(), g_deskW, g_deskH, g_deskPx); g_deskPx.clear(); g_deskPx.shrink_to_fit(); }
            }
            ID2D1Bitmap* deskBmp = g_deskWall ? g_deskWall : g_wall;
            if (!deskBmp) {  // ainda carregando: caixa escura no lugar (nunca só o nome solto)
                br->SetColor(D2D1::ColorF(0.18f, 0.18f, 0.20f, 0.85f * a));
                dc->FillRoundedRectangle(D2D1::RoundedRect(r, 6, 6), br);
            }
            if (deskBmp) {
                ID2D1RoundedRectangleGeometry* rg = nullptr;
                ID2D1Factory* f = nullptr; dc->GetFactory(&f);
                f->CreateRoundedRectangleGeometry(D2D1::RoundedRect(r, 6, 6), &rg);
                ID2D1Layer* l2 = nullptr; dc->CreateLayer(nullptr, &l2);
                dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), rg, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), a), l2);
                dc->DrawBitmap(deskBmp, r, 1, D2D1_INTERPOLATION_MODE_LINEAR);
                dc->PopLayer();
                l2->Release(); rg->Release(); f->Release();
            }
            if (i == g_cur) {
                br->SetColor(D2D1::ColorF(0.04f, 0.52f, 1, a));
                dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left - 2.5f, r.top - 2.5f, r.right + 2.5f, r.bottom + 2.5f), 8, 8), br, 3);
            }
            br->SetColor(D2D1::ColorF(1, 1, 1, 0.92f * a));
            dc->DrawTextW(g_desks[i].name.c_str(), (UINT32)g_desks[i].name.size(), g_tfDesk, D2D1::RectF(r.left - 20, r.bottom + 6, r.right + 20, r.bottom + 30), br);
        }
        // X da Mesa sob o mouse (só com mais de uma): círculo escuro com borda clara
        if (g_hoverDesk >= 0 && g_hoverDesk < (int)g_desks.size() && g_desks.size() > 1 && !g_closing) {
            D2D1_POINT_2F c = CloseCenter(g_hoverDesk, sc);
            br->SetColor(g_hoverX ? D2D1::ColorF(0.30f, 0.30f, 0.31f, a) : D2D1::ColorF(0.17f, 0.17f, 0.18f, 0.95f * a));
            dc->FillEllipse(D2D1::Ellipse(c, 11, 11), br);
            br->SetColor(D2D1::ColorF(1, 1, 1, 0.35f * a));
            dc->DrawEllipse(D2D1::Ellipse(c, 10.5f, 10.5f), br, 1);
            br->SetColor(D2D1::ColorF(1, 1, 1, 0.95f * a));
            float k = 3.6f;
            dc->DrawLine({ c.x - k, c.y - k }, { c.x + k, c.y + k }, br, 1.8f);
            dc->DrawLine({ c.x - k, c.y + k }, { c.x + k, c.y - k }, br, 1.8f);
        }
    }
    D2D1_RECT_F pr = PlusRect(sc);  // "+": nova mesa, numa caixa translúcida na barra aberta
    float px = (pr.left + pr.right) / 2, py = (pr.top + pr.bottom) / 2 + bar * 50;
    if (bar > 0.01f) {
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.14f * bar));
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(px - 22, py - 22, px + 22, py + 22), 8, 8), br);
    }
    br->SetColor(D2D1::ColorF(1, 1, 1, 0.85f));
    dc->DrawLine({ px - 9, py }, { px + 9, py }, br, 2);
    dc->DrawLine({ px, py - 9 }, { px, py + 9 }, br, 2);
    dc->PopLayer();
    layer->Release();
    }
windows:

    // janela sob o mouse: contorno azul e o título embaixo
    if (g_hover >= 0 && g_hover < (int)g_wins.size() && !g_closing && !g_compAnim) {
        D2D1_RECT_F c = Current(g_wins[g_hover]);
        D2D1_RECT_F d = { c.left / sc, c.top / sc, c.right / sc, c.bottom / sc };
        if (liquid::Leopard()) {  // Leopard: a janela clareia um pouco e o título vem numa pílula escura no meio dela
            br->SetColor(D2D1::ColorF(1, 1, 1, 0.10f));
            dc->FillRectangle(d, br);
            std::wstring t2 = g_wins[g_hover].title;
            float tw = std::min(ui::TextWidth(g_tfTitle, t2) + 30, d.right - d.left + 60), cx = (d.left + d.right) / 2, cy = (d.top + d.bottom) / 2;
            D2D1_RECT_F tr = { cx - tw / 2, cy - 13, cx + tw / 2, cy + 13 };
            br->SetColor(D2D1::ColorF(0, 0, 0, 0.70f));
            dc->FillRoundedRectangle(D2D1::RoundedRect(tr, 13, 13), br);
            br->SetColor(D2D1::ColorF(1, 1, 1, 0.95f));
            dc->DrawTextW(t2.c_str(), (UINT32)t2.size(), g_tfTitle, D2D1::RectF(tr.left + 15, tr.top, tr.right - 15, tr.bottom), br);
            goto hoverDone;
        }
        br->SetColor(D2D1::ColorF(0.04f, 0.52f, 1, 1));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(d.left - 3, d.top - 3, d.right + 3, d.bottom + 3), 10, 10), br, 3.5f);
        std::wstring t2 = g_wins[g_hover].title;
        float tw = std::min(ui::TextWidth(g_tfTitle, t2) + 24, d.right - d.left + 80);
        float cx = (d.left + d.right) / 2;
        D2D1_RECT_F tr = { cx - tw / 2, d.bottom + 10, cx + tw / 2, d.bottom + 36 };
        if (tr.bottom > Hd - (dock::Posicao() == 0 ? dock::ReservedHeight() : 0) / sc) { tr.top = d.bottom - 34; tr.bottom = d.bottom - 8; }
        br->SetColor(D2D1::ColorF(0.12f, 0.12f, 0.13f, 0.82f));
        dc->FillRoundedRectangle(D2D1::RoundedRect(tr, 8, 8), br);
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.95f));
        dc->DrawTextW(t2.c_str(), (UINT32)t2.size(), g_tfTitle, D2D1::RectF(tr.left + 12, tr.top, tr.right - 12, tr.bottom), br);
    }
hoverDone:
    br->Release();
    ui::GpuEnd(g_s);
    UpdateThumbs();
    PlaceX();
}

int HitWindow(D2D1_POINT_2F px) {
    for (int i = (int)g_wins.size() - 1; i >= 0; i--) {  // a de cima primeiro
        D2D1_RECT_F c = Current(g_wins[i]);
        if (px.x >= c.left && px.x < c.right && px.y >= c.top && px.y < c.bottom) return i;
    }
    return -1;
}

// ---- compositor
// papel de parede das miniaturas das Mesas: lido numa thread, guardado entre aberturas (só relê se mudou)
std::wstring g_deskKey;
void LoadDeskWall(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    std::wstring key = path;
    if (!path.empty() && GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) key += L"|" + std::to_wstring(fa.ftLastWriteTime.dwLowDateTime);
    if (key == g_deskKey && (g_deskWall || g_deskReady)) return;  // já pronto (ou chegando)
    g_deskKey = key;
    if (g_deskWall) { g_deskWall->Release(); g_deskWall = nullptr; }
    std::lock_guard<std::mutex> lk(g_deskMx);
    g_deskReady = false; g_deskPx.clear();
    int gen = ++g_deskGen;
    std::wstring wp = path;
    std::thread([wp, gen] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);  // WIC
        UINT ww = 0, wh = 0; std::vector<BYTE> px;
        bool ok = !wp.empty() && ui::LoadPng(wp, ww, wh, px);
        CoUninitialize();
        std::lock_guard<std::mutex> lk2(g_deskMx);
        if (gen != g_deskGen) return;
        if (ok) { g_deskPx = std::move(px); g_deskW = ww; g_deskH = wh; }
        g_deskReady = true;
    }).detach();
}

bool CompInit() {
    if (g_ct) return true;
    g_cd = ui::Comp();
    if (!g_cd || FAILED(g_cd->CreateTargetForHwnd(g_wnd, FALSE, &g_ct))) { Log(L"mission control: sem compositor"); return false; }
    g_cd->CreateVisual(&g_croot); g_cd->CreateVisual(&g_cblack); g_cd->CreateVisual(&g_cwall); g_cd->CreateVisual(&g_cwins);
    g_cd->CreateEffectGroup(&g_wallFx); g_cd->CreateEffectGroup(&g_barFx);
    g_cd->CreateScaleTransform(&g_blackSc);
    if (!g_croot || !g_cblack || !g_cwall || !g_cwins || !g_wallFx || !g_barFx || !g_blackSc) return false;
    if (IDCompositionSurface* b = ui::CompSurface(1, 1, [](ID2D1DeviceContext* dc) { dc->Clear(D2D1::ColorF(0, 0, 0, 1)); })) {
        g_cblack->SetContent(b); b->Release();
    }
    g_cblack->SetTransform(g_blackSc);
    g_cwall->SetEffect(g_wallFx);
    g_croot->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
    g_croot->AddVisual(g_cblack, FALSE, nullptr);
    g_croot->AddVisual(g_cwall, TRUE, g_cblack);
    g_croot->AddVisual(g_cwins, TRUE, g_cwall);
    g_ct->SetRoot(g_croot);
    return true;
}

// papel de parede (cobrindo a tela) numa superfície parada: desenhado uma vez por papel e tamanho
void CompWall(const std::wstring& path) {
    g_blackSc->SetScaleX(W()); g_blackSc->SetScaleY(H());
    std::wstring key = path + L"|" + std::to_wstring((int)W()) + L"x" + std::to_wstring((int)H());
    if (g_wallSurf && key == g_wallKey) return;
    if (g_wallSurf) { g_wallSurf->Release(); g_wallSurf = nullptr; }
    g_wallSurf = ui::CompSurface((int)W(), (int)H(), [&](ID2D1DeviceContext* dc) {
        ID2D1Bitmap* wall = nullptr;
        UINT ww = 0, wh = 0; std::vector<BYTE> px;
        if (!path.empty() && ui::LoadPng(path, ww, wh, px)) wall = ui::Bitmap(dc, ww, wh, px);
        if (wall) {
            auto s = wall->GetSize();
            float k = std::max(W() / s.width, H() / s.height);
            dc->DrawBitmap(wall, D2D1::RectF((W() - s.width * k) / 2, (H() - s.height * k) / 2, (W() + s.width * k) / 2, (H() + s.height * k) / 2), 1,
                           D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            wall->Release();
        } else dc->Clear(D2D1::ColorF(0.08f, 0.12f, 0.28f, 1));
    });
    g_wallKey = key;
    g_cwall->SetContent(g_wallSurf);
}

void FreeSnap(Win& w) {
    if (w.sv) { w.sv->Release(); w.sv = nullptr; }
    if (w.ss) { w.ss->Release(); w.ss = nullptr; }
    if (w.snap) { w.snap->Release(); w.snap = nullptr; }
}

bool AttachSnap(Win& w, IDCompositionSurface* surf, UINT sw, UINT sh);
bool SnapWin(Win& w) {
    std::vector<BYTE> px; UINT sw = 0, sh = 0;
    UINT dpi = GetDpiForWindow(w.h); if (!dpi) dpi = 96;
    float corner = IsZoomed(w.h) ? 0.f : 8.f * dpi / 96;
    if (!foto::Window(w.h, w.src, corner, px, sw, sh)) return false;
    IDCompositionSurface* surf = ui::CompSurface((int)sw, (int)sh, [&](ID2D1DeviceContext* dc) {
        if (ID2D1Bitmap* b = ui::Bitmap(dc, sw, sh, px)) { dc->DrawBitmap(b, D2D1::RectF(0, 0, (float)sw, (float)sh)); b->Release(); }
    });
    return AttachSnap(w, surf, sw, sh);
}

// a foto (superfície pronta) entra no visual da janela
bool AttachSnap(Win& w, IDCompositionSurface* surf, UINT sw, UINT sh) {
    if (!surf) return false;
    if (w.snap) w.snap->Release();
    w.snap = surf; w.sw = sw; w.sh = sh;
    if (!w.sv && FAILED(g_cd->CreateVisual(&w.sv))) return false;
    if (!w.ss && FAILED(g_cd->CreateScaleTransform(&w.ss))) return false;
    w.sv->SetContent(w.snap);
    w.ss->SetCenterX(0.f); w.ss->SetCenterY(0.f);
    w.sv->SetTransform(w.ss);
    return true;
}

bool SnapAll() {
    double t0 = anim::Now();
    // todas de uma vez pela placa de vídeo; a que falhar, pela CPU
    std::vector<HWND> hs; std::vector<RECT> srcs;
    for (auto& w : g_wins) { hs.push_back(w.h); srcs.push_back(w.src); }
    if (!g_live) { g_live = foto::Start(hs, srcs); g_liveHs = hs; }
    auto got = foto::Grab(g_live);
    std::vector<foto::Gpu> gpu(g_wins.size());  // na ordem de agora (a escolhida pode ter ido para cima)
    for (size_t k = 0; k < got.size() && k < g_liveHs.size(); k++) {
        auto it = std::find(hs.begin(), hs.end(), g_liveHs[k]);
        if (it != hs.end()) gpu[it - hs.begin()] = got[k];
        else if (got[k].surf) got[k].surf->Release();
    }
    int byGpu = 0;
    for (size_t i = 0; i < g_wins.size(); i++) {
        Win& w = g_wins[i];
        if (i < gpu.size() && gpu[i].surf) {
            if (AttachSnap(w, gpu[i].surf, gpu[i].w, gpu[i].h)) { byGpu++; continue; }
        }
        if (!SnapWin(w)) {
            for (size_t k = i + 1; k < gpu.size(); k++) if (gpu[k].surf) gpu[k].surf->Release();
            Log(L"mission control: sem foto de \"%ls\"; quadro a quadro", w.title.c_str());
            return false;
        }
    }
    Log(L"mission control: %zu fotos (%d pela GPU) em %.1f ms", g_wins.size(), byGpu, anim::Now() - t0);
    return true;
}

// as fotos andam de `from` a `to` pela mola (o mesmo caminho do Current()); fundo e barra junto
void CompAnimate() {
    const int K = 32;
    float D = g_durMs / 1000.f;
    std::vector<float> p(K);
    for (int k = 0; k < K; k++) p[k] = std::min(1.f, Curve(D * k / (K - 1)));
    p[K - 1] = 1;
    g_cwins->RemoveAllVisuals();
    IDCompositionVisual* prev = nullptr;  // cada uma acima da anterior (g_wins vai de baixo para cima)
    for (auto& w : g_wins) {
        if (!w.sv || !w.sw || !w.sh) continue;
        std::vector<float> ox(K), oy(K), sx(K), sy(K);
        for (int k = 0; k < K; k++) {
            D2D1_RECT_F r = Lerp(w.from, w.to, p[k]);
            ox[k] = r.left; oy[k] = r.top; sx[k] = (r.right - r.left) / w.sw; sy[k] = (r.bottom - r.top) / w.sh;
        }
        if (auto* a = ui::CompCurve(ox, D)) { w.sv->SetOffsetX(a); a->Release(); }
        if (auto* a = ui::CompCurve(oy, D)) { w.sv->SetOffsetY(a); a->Release(); }
        if (auto* a = ui::CompCurve(sx, D)) { w.ss->SetScaleX(a); a->Release(); }
        if (auto* a = ui::CompCurve(sy, D)) { w.ss->SetScaleY(a); a->Release(); }
        g_cwins->AddVisual(w.sv, TRUE, prev);
        prev = w.sv;
    }
    // escurecer do fundo e a barra: abrindo, em 300 ms (ease-out); fechando, junto com a mola
    float Dd = g_closing ? D : ANIM_MS / 1000.f;
    std::vector<float> wall(K), bar(K);
    for (int k = 0; k < K; k++) {
        float dim = g_closing ? 1 - p[k] : anim::EaseOut((float)k / (K - 1));
        wall[k] = 1 - 0.22f * dim; bar[k] = dim;
    }
    if (auto* a = ui::CompCurve(wall, Dd)) { g_wallFx->SetOpacity(a); a->Release(); }
    if (auto* a = ui::CompCurve(bar, Dd)) { g_barFx->SetOpacity(a); a->Release(); }
    g_compAnim = true;
    g_cd->Commit();
    SetTimer(g_wnd, T_COMP, (UINT)g_durMs + 20, nullptr);
}

// a barra mexeu no meio da abertura: as miniaturas vivas (quadro a quadro) seguem de onde as fotos estão
void CompStop() {
    if (!g_compAnim) return;
    KillTimer(g_wnd, T_COMP);
    g_compAnim = false;
    g_wallFx->SetOpacity(0.78f); g_barFx->SetOpacity(1.f);
    g_cwins->RemoveAllVisuals();
    g_cd->Commit();
}

// as fotos assentaram: as miniaturas vivas entram no mesmo lugar e as fotos saem
void CompSettle() {
    KillTimer(g_wnd, T_COMP);
    if (!g_compAnim) return;
    g_compAnim = false;
    g_animAt = anim::Now() - g_durMs - 100;  // Progress() = 1
    g_wallFx->SetOpacity(0.78f); g_barFx->SetOpacity(1.f);
    Paint();          // miniaturas vivas visíveis (e o contorno, se o mouse estiver em cima)
    DwmFlush();
    g_cwins->RemoveAllVisuals();
    g_cd->Commit();
}

void Close(HWND pick) {
    if (!g_open || g_closing) return;
    g_closing = true;
    g_pick = pick;
    g_hover = -1;
    for (auto& w : g_wins) { w.from = Current(w); w.to = FrameF(w); }
    // a escolhida volta por cima das outras
    if (pick)
        for (size_t i = 0; i < g_wins.size(); i++)
            if (g_wins[i].h == pick && i + 1 < g_wins.size()) {
                Win w = g_wins[i];
                DwmUnregisterThumbnail(w.th);
                DwmRegisterThumbnail(g_wnd, w.h, &w.th);
                g_wins.erase(g_wins.begin() + i);
                g_wins.push_back(w);
                break;
            }
    g_barFrom = BarNow(); g_barTo = 0;
    g_animAt = anim::Now();
    if (liquid::On()) g_durMs = 260;  // Tahoe: sair em ~0,28 s
    if (g_comp) {
        bool ok = g_compAnim || SnapAll();  // abrindo ainda: as fotos de agora servem
        if (ok) {
            ui::FrameClock(g_wnd, WM_MC_FRAME, false);
            CompAnimate();
            Paint();      // esconde as miniaturas vivas (as fotos já estão no lugar delas)
            return;
        }
        g_comp = false; g_compAnim = false;   // sem foto: o jeito antigo
        g_cwins->RemoveAllVisuals(); g_wallFx->SetOpacity(0.78f); g_barFx->SetOpacity(1.f); g_cd->Commit();
        if (!g_wall) {  // o fundo agora é desenhado pelo Paint
            wchar_t path[MAX_PATH] = {}; SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, path, 0);
            UINT ww = 0, wh = 0; std::vector<BYTE> px;
            if (path[0] && ui::LoadPng(path, ww, wh, px)) g_wall = ui::Bitmap(ui::Gpu(), ww, wh, px);
        }
    }
    ui::FrameClock(g_wnd, WM_MC_FRAME, true);
}

void Finish() {
    ui::FrameClock(g_wnd, WM_MC_FRAME, false);
    KillTimer(g_wnd, T_COMP);
    g_compAnim = false;
    if (g_live) { foto::Stop(g_live); g_live = nullptr; g_liveHs.clear(); }
    for (auto& w : g_wins) { if (w.th) DwmUnregisterThumbnail(w.th); if (w.mini) DwmUnregisterThumbnail(w.mini); FreeSnap(w); }
    g_wins.clear();
    for (auto& f : g_fly) DwmUnregisterThumbnail(f.th);
    g_fly.clear();
    g_drag = -1; g_dragging = false; g_dropDesk = -1;
    g_open = g_closing = false;
    ShowWindow(g_wnd, SW_HIDE);
    if (g_xWnd) ShowWindow(g_xWnd, SW_HIDE);
    g_xFor = nullptr; g_xHot = false;
    if (g_cd && g_cwins) { g_cwins->RemoveAllVisuals(); g_wallFx->SetOpacity(1.f); g_barFx->SetOpacity(1.f); g_cd->Commit(); }
    if (g_pick) {
        keybd_event(VK_MENU, 0, 0, KEY_MARK);  // permite trazer a janela para a frente
        keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, KEY_MARK);
        SetForegroundWindow(g_pick);
        g_pick = nullptr;
    }
    if (g_wall) { g_wall->Release(); g_wall = nullptr; }
}

D2D1_POINT_2F Px(LPARAM l) { return { (float)(short)LOWORD(l), (float)(short)HIWORD(l) }; }

// ---- X para fechar a janela sob o mouse (como no Mac: no canto de cima à esquerda da miniatura).
// É uma janela própria por cima de tudo: as miniaturas do DWM ficam acima do desenho do Mission Control.
const UINT_PTR T_XCHECK = 8;
const float X_D = 36;  // DIPs

void DrawX() {
    if (!g_xWnd) return;
    float sc = Sc();
    int n = (int)std::ceil(X_D * sc);
    BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), n, -n, 1, 32, BI_RGB };
    void* bits = nullptr;
    HDC mem = CreateCompatibleDC(nullptr);
    HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, dib);
    static ID2D1Factory* f = nullptr;
    if (!f) D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &f);
    ID2D1DCRenderTarget* rt = nullptr;
    auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    if (f && SUCCEEDED(f->CreateDCRenderTarget(&props, &rt))) {
        RECT rc{ 0, 0, n, n };
        rt->BindDC(mem, &rc);
        rt->BeginDraw();
        rt->Clear(D2D1::ColorF(0, 0, 0, 0));
        ID2D1SolidColorBrush* br = nullptr;
        rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0), &br);
        if (br) {
            // cinza da Apple (systemGray5/4 no claro) com o X cinza-escuro, um pouco mais escuro com o mouse em cima
            float c = n / 2.f, r = c - 2.f * sc, k = 5.2f * sc;
            br->SetColor(D2D1::ColorF(0, 0, 0, 0.18f));  // sombra leve
            rt->FillEllipse(D2D1::Ellipse({ c, c + 1.f * sc }, r + 0.5f * sc, r + 0.5f * sc), br);
            br->SetColor(g_xHot ? D2D1::ColorF(0.82f, 0.82f, 0.84f, 1) : D2D1::ColorF(0.90f, 0.90f, 0.92f, 0.96f));
            rt->FillEllipse(D2D1::Ellipse({ c, c }, r, r), br);
            br->SetColor(D2D1::ColorF(0, 0, 0, 0.10f));
            rt->DrawEllipse(D2D1::Ellipse({ c, c }, r - 0.5f, r - 0.5f), br, 1);
            br->SetColor(D2D1::ColorF(0.24f, 0.24f, 0.26f, 1));
            ID2D1StrokeStyle* round = nullptr;
            f->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND), nullptr, 0, &round);
            rt->DrawLine({ c - k, c - k }, { c + k, c + k }, br, 2.2f * sc, round);
            rt->DrawLine({ c - k, c + k }, { c + k, c - k }, br, 2.2f * sc, round);
            if (round) round->Release();
            br->Release();
        }
        rt->EndDraw();
        rt->Release();
    }
    POINT src{ 0, 0 }; SIZE sz{ n, n }; BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_xWnd, nullptr, nullptr, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, old); DeleteObject(dib); DeleteDC(mem);
}

void PlaceX() {
    if (!g_xWnd) return;
    bool show = g_open && !g_closing && !g_compAnim && g_hover >= 0 && g_hover < (int)g_wins.size();
    if (!show) { if (IsWindowVisible(g_xWnd)) ShowWindow(g_xWnd, SW_HIDE); g_xFor = nullptr; g_xHot = false; return; }
    D2D1_RECT_F c = Current(g_wins[g_hover]);
    float sc = Sc();
    int n = (int)std::ceil(X_D * sc);
    int x = g_mon.left + (int)std::lround(c.left + 6 * sc) - n / 2, y = g_mon.top + (int)std::lround(c.top + 6 * sc) - n / 2;
    bool fresh = g_xFor != g_wins[g_hover].h || !IsWindowVisible(g_xWnd);
    g_xFor = g_wins[g_hover].h;
    if (fresh) DrawX();
    SetWindowPos(g_xWnd, HWND_TOPMOST, x, y, n, n, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

// Clicou no X: pede para a janela fechar e ela sai do Mission Control (as outras se rearrumam). Se ela não
// fechar (pergunta se quer salvar), o Mission Control fecha com ela na frente para responder.
void CloseHovered() {
    HWND t = g_xFor;
    if (!t || g_closing) return;
    Log(L"mission control: fechando janela pelo X");
    PostMessageW(t, WM_CLOSE, 0, 0);
    for (size_t i = 0; i < g_wins.size(); i++)
        if (g_wins[i].h == t) {
            Win& w = g_wins[i];
            if (w.th) DwmUnregisterThumbnail(w.th);
            if (w.mini) DwmUnregisterThumbnail(w.mini);
            FreeSnap(w);
            g_wins.erase(g_wins.begin() + i);
            break;
        }
    g_hover = -1; g_xFor = nullptr; g_xHot = false;
    ShowWindow(g_xWnd, SW_HIDE);
    if (!g_wins.empty()) Retarget(g_barTo); else Paint();
    g_xPending = t;
    SetTimer(g_wnd, T_XCHECK, 500, nullptr);
}

LRESULT CALLBACK XProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;  // o Mission Control continua ativo (perder o foco o fecharia)
    case WM_MOUSEMOVE:
        if (!g_xHot) { g_xHot = true; DrawX(); TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t); }
        return 0;
    case WM_MOUSELEAVE: g_xHot = false; DrawX(); return 0;
    case WM_LBUTTONDOWN: return 0;
    case WM_LBUTTONUP: CloseHovered(); return 0;
    case WM_SETCURSOR: SetCursor(LoadCursorW(nullptr, IDC_ARROW)); return TRUE;
    }
    return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); Paint(); return 0; }
    case WM_TIMER:
        if (w == T_XCHECK) {  // a janela fechada pelo X ainda está aberta: ela está perguntando algo
            KillTimer(h, T_XCHECK);
            if (g_xPending && IsWindow(g_xPending) && IsWindowVisible(g_xPending) && g_open && !g_closing) Close(g_xPending);
            g_xPending = nullptr;
            return 0;
        }
        if (w == T_COMP) {
            KillTimer(h, T_COMP);
            if (g_closing) { if (g_compAnim) Finish(); }
            else CompSettle();
        }
        return 0;
    case WM_MC_FRAME: {
        bool done = anim::Now() - g_animAt >= g_durMs + 20 && g_fly.empty();
        if (g_closing && done) { Paint(); Finish(); return 0; }
        Paint();
        if (done) ui::FrameClock(h, WM_MC_FRAME, false);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        if (g_closing || g_compAnim) return 0;
        D2D1_POINT_2F p = Px(l);
        g_drag = HitWindow(p); g_dragging = false;
        if (g_drag >= 0) {
            g_dragStart = p;
            D2D1_RECT_F c = Current(g_wins[g_drag]);
            g_dragBase = c;
            g_dragGrab = { (p.x - c.left) / std::max(1.f, c.right - c.left), (p.y - c.top) / std::max(1.f, c.bottom - c.top) };
            SetCapture(h);
        }
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (g_closing) return 0;
        D2D1_POINT_2F p = Px(l);
        if (g_drag >= 0 && g_drag < (int)g_wins.size() && (GetKeyState(VK_LBUTTON) & 0x8000)) {
            if (!g_dragging && std::hypot(p.x - g_dragStart.x, p.y - g_dragStart.y) > 6) {
                g_dragging = true;
                Win& w = g_wins[g_drag];  // por cima das outras (a miniatura registrada por último fica em cima)
                if (w.th) { DwmUnregisterThumbnail(w.th); w.th = nullptr; DwmRegisterThumbnail(g_wnd, w.h, &w.th); }
                g_hover = -1;
            }
            if (g_dragging) {
                float sc = Sc();
                bool overBar = Bar() && p.y < BarHeightPx(g_barTo) + 4;
                if (overBar && g_barTo == 0) Retarget(1);  // a faixa abre para receber a janela
                // encolhe chegando perto da faixa (como no Mac), até uns 35%
                float bar = BarHeightPx(1);
                float k = 1 - 0.65f * std::clamp((bar + 220 * sc - p.y) / (220 * sc), 0.f, 1.f);
                float bw = (g_dragBase.right - g_dragBase.left) * k, bh = (g_dragBase.bottom - g_dragBase.top) * k;
                D2D1_RECT_F r{ p.x - g_dragGrab.x * bw, p.y - g_dragGrab.y * bh, p.x - g_dragGrab.x * bw + bw, p.y - g_dragGrab.y * bh + bh };
                Win& w = g_wins[g_drag];
                w.from = w.to = r;
                D2D1_POINT_2F d{ p.x / sc, p.y / sc };
                int dd = Bar() && g_barTo == 1 ? HitDesk(d, sc) : -1;
                if (dd != g_dropDesk) { g_dropDesk = dd; g_hoverDesk = dd; g_hoverX = false; }
                if (!g_open) return 0;
                ui::FrameClock(h, WM_MC_FRAME, true);
                Paint();
                return 0;
            }
        }
        int hv = g_compAnim ? -1 : HitWindow(p);
        if (hv != g_hover) { g_hover = hv; Paint(); }
        {
            float sc = Sc();
            D2D1_POINT_2F d{ p.x / sc, p.y / sc };
            int hd = Bar() && g_barTo == 1 ? HitDesk(d, sc) : -1;
            bool hx = hd >= 0 && OverClose(hd, d, sc);
            if (hd != g_hoverDesk || hx != g_hoverX) { g_hoverDesk = hd; g_hoverX = hx; Paint(); }
        }
        bool overBar = Bar() && p.y < BarHeightPx(g_barTo) + 4;
        if (overBar && g_barTo == 0) Retarget(1);
        else if (Bar() && !overBar && g_barTo == 1 && p.y > BarHeightPx(1) + 30) Retarget(0);
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_closing) return 0;
        D2D1_POINT_2F p = Px(l);
        float sc = Sc();
        if (GetCapture() == h) ReleaseCapture();
        if (g_dragging && g_drag >= 0 && g_drag < (int)g_wins.size()) {
            int dw = g_drag, desk = g_dropDesk;
            g_drag = -1; g_dragging = false; g_dropDesk = -1;
            Win& w = g_wins[dw];
            D2D1_RECT_F now = w.to;
            if (desk >= 0 && desk != g_cur && mesas::MoveWindow(w.h, desk)) {  // solta na outra Mesa: voa até a miniatura dela
                Log(L"mission control: \"%ls\" levada para a mesa %d", w.title.c_str(), desk + 1);
                D2D1_RECT_F dr = DeskThumbRect(desk, sc);
                float cx = (dr.left + dr.right) / 2 * sc, cy = (dr.top + dr.bottom) / 2 * sc;
                float tw = (dr.right - dr.left) * sc * 0.45f, th = tw * (now.bottom - now.top) / std::max(1.f, now.right - now.left);
                g_fly.push_back({ w.h, w.th, w.src, now, D2D1_RECT_F{ cx - tw / 2, cy - th / 2, cx + tw / 2, cy + th / 2 }, anim::Now() });
                if (w.mini) DwmUnregisterThumbnail(w.mini);
                FreeSnap(w);
                g_wins.erase(g_wins.begin() + dw);
                g_hover = -1;
                if (!g_wins.empty()) Retarget(g_barTo); else { g_animAt = anim::Now(); ui::FrameClock(h, WM_MC_FRAME, true); }
                return 0;
            }
            // solta em outro lugar: volta para o lugar dela na arrumação
            Retarget(g_barTo);
            if (dw < (int)g_wins.size()) g_wins[dw].from = now;
            return 0;
        }
        g_drag = -1;
        D2D1_POINT_2F d{ p.x / sc, p.y / sc };
        if (!Bar()) { int hit = HitWindow(p); Close(hit >= 0 ? g_wins[hit].h : nullptr); return 0; }  // Exposé do app: só janelas
        D2D1_RECT_F pr = PlusRect(sc);
        if (d.x >= pr.left - 6 && d.x < pr.right + 6 && d.y >= pr.top && d.y < pr.bottom + BarNow() * 50) {  // nova mesa
            if (mesas::Create()) { LoadDesks(); Paint(); }  // como no Mac: cria e continua aqui
            else { Close(nullptr); WinCtrl('D'); }
            return 0;
        }
        if (BarNow() >= 0.5f && g_desks.size() > 1)  // X: fecha a Mesa; as janelas dela vão para a vizinha
            for (int i = 0; i < (int)g_desks.size(); i++)
                if (OverClose(i, d, sc) && i == g_hoverDesk) {
                    bool cur = i == g_cur;
                    if (mesas::Remove(i)) {
                        Log(L"mission control: mesa %d fechada", i + 1);
                        if (cur) { Close(nullptr); return 0; }
                        LoadDesks(); g_hoverDesk = -1; g_hoverX = false; Paint();
                    } else {  // sem a interface interna: vai até a mesa e fecha com Win+Ctrl+F4
                        int delta = i - g_cur;
                        Close(nullptr);
                        for (int k = 0; k < std::abs(delta); k++) WinCtrl(delta > 0 ? VK_RIGHT : VK_LEFT);
                        WinCtrl(VK_F4);
                    }
                    return 0;
                }
        if (p.y < BarHeightPx(BarNow())) {  // trocar de mesa
            for (int i = 0; i < (int)g_desks.size(); i++) {
                D2D1_RECT_F r = BarNow() < 0.5f ? DeskLabelRect(i, sc) : DeskThumbRect(i, sc);
                r.bottom += BarNow() >= 0.5f ? 30 : 0;
                if (d.x >= r.left && d.x < r.right && d.y >= r.top && d.y < r.bottom) {
                    int delta = i - g_cur;
                    Close(nullptr);
                    if (delta && !mesas::Switch(i))  // direto para a mesa (sem passar pelas do meio)
                        for (int k = 0; k < std::abs(delta); k++) WinCtrl(delta > 0 ? VK_RIGHT : VK_LEFT);
                    return 0;
                }
            }
            return 0;
        }
        int hit = HitWindow(p);
        Close(hit >= 0 ? g_wins[hit].h : nullptr);  // clicar no vazio volta para a mesa
        return 0;
    }
    case WM_RBUTTONUP: Close(nullptr); return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) Close(nullptr);
        if (w == VK_RETURN && g_hover >= 0) Close(g_wins[g_hover].h);
        return 0;
    case WM_ACTIVATE: if (LOWORD(w) == WA_INACTIVE && g_open && !g_closing) Close(nullptr); return 0;
    case WM_MOUSEACTIVATE: return MA_ACTIVATE;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

// Antes da primeira abertura (o Shell já pronto e parado): o fundo do compositor (o papel de parede no
// tamanho da tela) e o das miniaturas das Mesas. Antes, a primeira abertura lia e desenhava tudo na hora.
void missioncontrol::Warm() {
    if (g_open || !g_wnd) return;
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
    g_mon = mi.rcMonitor;
    wchar_t path[MAX_PATH] = {};
    SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, path, 0);
    double t0 = anim::Now();
    LoadDeskWall(path);
    if (shell::Setting(L"mc_motor", L"composicao") == L"composicao" && CompInit()) CompWall(path);
    Log(L"mission control: preparado antes de abrir em %.1f ms", anim::Now() - t0);
}

bool missioncontrol::Create(HINSTANCE hi) {
    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = hi; wc.lpszClassName = L"ShellMissionControl";
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    g_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP | WS_EX_TRANSPARENT, wc.lpszClassName, L"Mission Control", WS_POPUP,
                            0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
    if (!g_wnd) return false;
    // WS_EX_TRANSPARENT (sem WS_EX_LAYERED o clique continua chegando): apps feitos com Chromium (Chrome,
    // Discord...) param de desenhar quando se acham cobertos; janelas "transparentes" não contam como
    // cobertura para eles, então as miniaturas continuam vivas.
    apps::ours.push_back(g_wnd);
    g_s.hwnd = g_wnd;
    WNDCLASSW xc{}; xc.lpfnWndProc = XProc; xc.hInstance = hi; xc.lpszClassName = L"ShellMissionControlFechar"; xc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&xc);
    g_xWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, xc.lpszClassName, L"", WS_POPUP,
                             0, 0, 1, 1, g_wnd, nullptr, hi, nullptr);
    g_tfDesk = ui::Format(15, DWRITE_FONT_WEIGHT_MEDIUM, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfTitle = ui::Format(13, DWRITE_FONT_WEIGHT_MEDIUM, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    for (auto* tf : { g_tfDesk, g_tfTitle }) if (tf) tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (g_tfTitle) {  // títulos longos são cortados (uma linha só)
        DWRITE_TRIMMING tr{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        g_tfTitle->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        g_tfTitle->SetTrimming(&tr, nullptr);
    }
    return true;
}

bool missioncontrol::IsOpen() { return g_open && !g_closing; }

void missioncontrol::ToggleApp(const std::wstring& appId, const std::wstring& appName) {
    if (g_open && !g_closing) {
        bool same = g_appId == appId;
        Close(nullptr);
        if (same) return;
        Finish();
    }
    g_appId = appId; g_appName = appName;
    g_appNext = true;  // esta abertura é do app; a próxima (Win+Tab) volta a ser o Mission Control
    Toggle();
}

void missioncontrol::Toggle() {
    if (g_open) {
        if (!g_closing) { Close(nullptr); return; }
        Finish();  // Tab de novo enquanto fecha (Win seguro): abre outra vez na hora
    }
    if (!g_appNext) g_appId.clear();
    g_appNext = false;
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
    g_mon = mi.rcMonitor;
    // papel de parede (o Mission Control mostra a mesa vazia por trás das janelas)
    wchar_t path[MAX_PATH] = {};
    SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, path, 0);
    if (g_wall) { g_wall->Release(); g_wall = nullptr; }
    LoadDeskWall(path);
    g_comp = shell::Setting(L"mc_motor", L"composicao") == L"composicao" && CompInit();
    if (!g_comp) {
        UINT ww = 0, wh = 0; std::vector<BYTE> px;
        if (path[0] && ui::LoadPng(path, ww, wh, px)) g_wall = ui::Bitmap(ui::Gpu(), ww, wh, px);
    }
    LoadDesks();
    SetWindowPos(g_wnd, HWND_TOPMOST, g_mon.left, g_mon.top, (int)W(), (int)H(), SWP_NOACTIVATE);
    Collect();
    for (auto& w : g_wins) w.from = FrameF(w);
    g_barFrom = g_barTo = 0;
    Layout(0);
    g_open = true; g_closing = false; g_hover = -1; g_pick = nullptr; g_hoverDesk = -1; g_hoverX = false;
    g_animAt = g_openAt = anim::Now();
    g_durMs = liquid::On() ? 300.f : std::min(550.f, anim::SpringDuration(RESPONSE, DAMPING) * 1000);
    g_compAnim = false;
    if (g_comp) {
        double t0 = anim::Now();
        CompWall(path);
        g_comp = SnapAll();
        if (!g_comp) {
            for (auto& w : g_wins) FreeSnap(w);
            g_wallFx->SetOpacity(1.f); g_cd->Commit();
            UINT ww = 0, wh = 0; std::vector<BYTE> px;
            if (path[0] && ui::LoadPng(path, ww, wh, px)) g_wall = ui::Bitmap(ui::Gpu(), ww, wh, px);
        }
        Log(L"mission control: preparo do compositor %.1f ms", anim::Now() - t0);
    }
    Paint();  // primeiro quadro pronto antes de aparecer: as miniaturas cobrem as janelas no mesmo lugar
    if (g_comp) {
        if (g_s.visual) g_s.visual->SetEffect(g_barFx);  // a barra aparece pela opacidade do compositor
        g_animAt = g_openAt = anim::Now();
        CompAnimate();
        Paint();  // as fotos no lugar das miniaturas vivas
    }
    ShowWindow(g_wnd, SW_SHOW);
    SetForegroundWindow(g_wnd);
    SetFocus(g_wnd);
    dock::Raise();
    if (!g_comp) ui::FrameClock(g_wnd, WM_MC_FRAME, true);
    Log(L"mission control: %zu janelas, %zu mesas", g_wins.size(), g_desks.size());
}
