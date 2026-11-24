// Janelas abrindo como no Mac: a janela nova aparece crescendo de ~94% para 100% enquanto surge (~0,18 s).
// A WacOSWindow.dll (dentro do app) esconde a janela (cloak) no WM_SHOWWINDOW, antes de ela aparecer, e
// avisa aqui ("ShellJanelaAbrir") depois de uns quadros, com o app já pintado. O Shell anima uma miniatura
// viva do DWM (o mesmo recurso do gênio) numa camada nossa, logo abaixo do Dock, e no fim manda a DLL
// mostrar a janela de verdade ("MacJanelaAbriu"); a miniatura sai dois quadros depois (sem buraco).
// O fechar (foto encolhendo e sumindo) fica em ampliar::Fechar.
#include "shell.h"
#include <dwmapi.h>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <thread>
#include <vector>

namespace {

const double DUR_SEQUOIA = 180;    // ms (era 220: abrir mais ágil)
// Tahoe (guia macOS26-Theme-Reference, seção 44, WindowServer amostrado a ~250 Hz): a janela nasce com 98,5 % e
// cresce para 100 % numa mola de resposta 0,20 s e amortecimento 0,54 (passa 0,2 % e volta); 90 % em 0,08 s,
// 0,17 s no total. Lido em Open (thread principal) e guardado: o laço roda em outra thread.
bool g_tahoeAbrir;
double Dur() { return g_tahoeAbrir ? 170 : DUR_SEQUOIA; }
const UINT WM_AB_DONE = WM_APP + 1;  // o movimento de uma janela terminou (wParam = hwnd)
const UINT_PTR T_REMOVE = 1, T_SAFE = 2;

struct Item {
    HWND h = nullptr;
    HTHUMBNAIL th = nullptr;
    HTHUMBNAIL bar = nullptr;  // a nossa barra (janela separada por cima, com o Ableton e afins)
    RECT barRect{};
    RECT vis{};        // tela: parte visível da janela
    RECT src{};        // na miniatura: a mesma parte, em coordenadas da janela
    double t0 = 0;
    bool done = false;
    DWORD removeAt = 0;  // já mostrada: a miniatura sai neste instante
};

HWND g_ov;
POINT g_org{};      // canto da camada (tela virtual)
std::vector<Item> g_items;
std::mutex g_mx;
bool g_thread;

float EaseOut(float t) { return 1 - std::pow(1 - t, 3.f); }

void Frame(Item& it, float u) {
    float e = EaseOut(std::clamp(u, 0.f, 1.f));
    float s = 0.94f + 0.06f * e;
    if (g_tahoeAbrir) s = u >= 1 ? 1.f : 0.985f + 0.015f * anim::Spring(std::max(0.f, u) * 0.17f, 0.20f, 0.54f);
    float w = (float)(it.vis.right - it.vis.left), hh = (float)(it.vis.bottom - it.vis.top);
    float cx = (it.vis.left + it.vis.right) / 2.f - g_org.x, cy = (it.vis.top + it.vis.bottom) / 2.f - g_org.y;
    DWM_THUMBNAIL_PROPERTIES p{};
    p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_OPACITY | DWM_TNP_VISIBLE;
    p.rcDestination = { (LONG)std::lround(cx - w * s / 2), (LONG)std::lround(cy - hh * s / 2), (LONG)std::lround(cx + w * s / 2), (LONG)std::lround(cy + hh * s / 2) };
    p.rcSource = it.src;
    p.opacity = (BYTE)std::lround(255 * std::clamp(u / (g_tahoeAbrir ? 0.5f : 0.7f), 0.f, 1.f));  // aparece um pouco antes de terminar de crescer
    p.fVisible = TRUE;
    DwmUpdateThumbnailProperties(it.th, &p);
    if (it.bar) {  // a barra acompanha a mesma escala, em volta do mesmo centro
        auto tx = [&](LONG v, float c0, float c) { return (LONG)std::lround(c + (v - c0) * s); };
        float c0x = (it.vis.left + it.vis.right) / 2.f, c0y = (it.vis.top + it.vis.bottom) / 2.f;
        p.rcDestination = { tx(it.barRect.left, c0x, cx + g_org.x) - g_org.x, tx(it.barRect.top, c0y, cy + g_org.y) - g_org.y,
                            tx(it.barRect.right, c0x, cx + g_org.x) - g_org.x, tx(it.barRect.bottom, c0y, cy + g_org.y) - g_org.y };
        p.rcSource = { 0, 0, it.barRect.right - it.barRect.left, it.barRect.bottom - it.barRect.top };
        DwmUpdateThumbnailProperties(it.bar, &p);
    }
}

// Um quadro por atualização da tela (DwmFlush), fora da thread principal: liso mesmo com o Shell ocupado
void Worker() {
    for (;;) {
        DwmFlush();
        std::lock_guard<std::mutex> k(g_mx);
        bool any = false;
        double now = anim::Now();
        for (auto& it : g_items) {
            if (it.done) continue;
            any = true;
            float u = (float)((now - it.t0) / Dur());
            Frame(it, u);
            if (u >= 1) { it.done = true; PostMessageW(g_ov, WM_AB_DONE, (WPARAM)it.h, 0); }
        }
        if (!any) { g_thread = false; return; }
    }
}

void Show(HWND h) {  // a janela de verdade aparece (a DLL tira o cloak)
    static UINT msg = RegisterWindowMessageW(L"MacJanelaAbriu");
    if (IsWindow(h)) PostMessageW(h, msg, 0, 0);
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_AB_DONE: {
        Show((HWND)w);
        std::lock_guard<std::mutex> k(g_mx);
        for (auto& it : g_items) if (it.h == (HWND)w) it.removeAt = GetTickCount() + 50;  // ~3 quadros: a janela já está na tela
        SetTimer(h, T_REMOVE, 25, nullptr);
        return 0;
    }
    case WM_TIMER:
        if (w == T_REMOVE) {
            std::lock_guard<std::mutex> k(g_mx);
            DWORD now = GetTickCount();
            for (size_t i = 0; i < g_items.size();)
                if (g_items[i].removeAt && (int)(now - g_items[i].removeAt) >= 0) {
                    DwmUnregisterThumbnail(g_items[i].th);
                    if (g_items[i].bar) DwmUnregisterThumbnail(g_items[i].bar);
                    g_items.erase(g_items.begin() + i);
                }
                else i++;
            bool pending = false;
            for (auto& it : g_items) if (it.removeAt) pending = true;
            if (!pending) KillTimer(h, T_REMOVE);
            if (g_items.empty()) ShowWindow(h, SW_HIDE);
        }
        if (w == T_SAFE) {
            // Segurança: miniatura que não terminou (o aviso do fim se perdeu, a janela foi minimizada/fechada no meio)
            // ficava presa na camada por cima de tudo, quase transparente: o contorno e os três botões da janela
            // "fantasma" por cima do Chrome (print do usuário, 19/11/2026). Passou do tempo: sai e a janela aparece.
            std::lock_guard<std::mutex> k(g_mx);
            double now = anim::Now();
            for (size_t i = 0; i < g_items.size();) {
                Item& it = g_items[i];
                bool gone = !IsWindow(it.h) || IsIconic(it.h) || !IsWindowVisible(it.h);
                if (gone || now - it.t0 > Dur() + 700) {
                    Log(L"abrir: miniatura presa removida (%.0f ms%ls)", now - it.t0, gone ? L", janela sumiu" : L"");
                    DwmUnregisterThumbnail(it.th);
                    if (it.bar) DwmUnregisterThumbnail(it.bar);
                    if (!gone) Show(it.h);
                    g_items.erase(g_items.begin() + i);
                } else i++;
            }
            if (g_items.empty()) { KillTimer(h, T_SAFE); ShowWindow(h, SW_HIDE); }
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

bool Ready() {
    if (g_ov) return true;
    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = shell::inst; wc.lpszClassName = L"ShellAbrir";
    RegisterClassW(&wc);
    g_ov = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_NOREDIRECTIONBITMAP,
                           wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, shell::inst, nullptr);
    if (g_ov) apps::ours.push_back(g_ov);
    return g_ov != nullptr;
}

}  // namespace

void abrir::Open(HWND h, HWND bar) {
    bool on = shell::Setting(L"animar_janelas", L"1") == L"1";
    g_tahoeAbrir = liquid::On();
    if (anim::reduceMotion) { Show(h); return; }  // Reduzir Movimento: a janela aparece na hora (medido no Mac)
    RECT wr, vis;
    if (!on || !IsWindow(h) || !Ready() || !GetWindowRect(h, &wr)) { Show(h); return; }
    if (progresso::Owns(h)) { Show(h); return; }  // janela de progresso do Windows que o Shell tirou da tela: nada de animar
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &vis, sizeof(vis)))) vis = wr;
    if (vis.right - vis.left < 120 || vis.bottom - vis.top < 80) { Show(h); return; }
    {
        std::lock_guard<std::mutex> k(g_mx);
        for (auto& it : g_items) if (it.h == h) return;  // já animando
    }
    // a camada cobre a tela virtual inteira, logo abaixo do Dock (como no Mac, nada passa por cima dele)
    int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int cw = GetSystemMetrics(SM_CXVIRTUALSCREEN), ch = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    g_org = { x, y };
    HWND under = dock::Fundo() && IsWindowVisible(dock::Fundo()) ? dock::Fundo() : HWND_TOPMOST;
    SetWindowPos(g_ov, under, x, y, cw, ch, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    Item it;
    it.h = h; it.vis = vis;
    it.src = { vis.left - wr.left, vis.top - wr.top, vis.right - wr.left, vis.bottom - wr.top };
    if (FAILED(DwmRegisterThumbnail(g_ov, h, &it.th))) { Log(L"abrir: sem miniatura"); Show(h); return; }
    if (bar && IsWindow(bar) && GetWindowRect(bar, &it.barRect) && FAILED(DwmRegisterThumbnail(g_ov, bar, &it.bar))) it.bar = nullptr;
    it.t0 = anim::Now();
    Frame(it, 0);
    Log(L"abrir: %ldx%ld", vis.right - vis.left, vis.bottom - vis.top);
    std::lock_guard<std::mutex> k(g_mx);
    g_items.push_back(it);
    if (!g_thread) { g_thread = true; std::thread(Worker).detach(); }
    SetTimer(g_ov, T_SAFE, 300, nullptr);
}
