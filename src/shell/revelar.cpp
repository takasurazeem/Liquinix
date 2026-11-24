// "Clique no papel de parede para mostrar a mesa" (macOS Sonoma/Sequoia): um clique num lugar vazio da
// área de trabalho (inclusive ao lado do Dock) empurra as janelas para as bordas da tela, deixando uma
// faixa de cada uma à mostra. Outro clique no vazio, ou ativar qualquer janela, traz todas de volta.
// Animado pelo compositor (miniaturas do DWM numa camada por cima), com a mola suave do Mac: fluido no
// ritmo do monitor, sem depender de cada app redesenhar. As maximizadas também vão para a borda.
// Com revelar_motor=composicao (padrão), a camada é do compositor do Windows (DirectComposition): a mesa e
// as janelas viram fotos (fotogpu.cpp) e o deslize com a mola o DWM faz sozinho; senão, miniaturas
// movidas quadro a quadro (antigo, também o caminho se alguma foto falhar).
// Gancho de mouse numa thread só dele (não faz nada pesado: só avisa a janela de mensagens);
// a conferência "é vazio ou é um ícone?" é feita pela UI Automation na thread principal.
#include "shell.h"
#include <UIAutomation.h>
#include <dwmapi.h>
#include <dcomp.h>
#include <shobjidl.h>
#include <algorithm>
#include <cmath>
#include <thread>

namespace {

const UINT WM_RV_CLICK = WM_APP + 80, WM_RV_FRAME = WM_APP + 81;
const int PEEK = 40;  // px de cada janela que ficam à mostra na borda

struct Moved {
    HWND h;
    RECT from{}, to{};
    bool minimized = false;  // estava maximizada: foi minimizada
    bool failed = false;     // o Windows recusou mover (janela de administrador)
    bool atEdge = false;     // foi mesmo para a borda (para devolver também se for minimizada lá)
    RECT src{};              // parte visível da janela (sem a borda invisível; nas maximizadas ela fica fora da tela)
};

HWND g_wnd;
HHOOK g_hook;
DWORD g_hookThread;
POINT g_down{};
bool g_downOnDesk;
bool g_revealed;
DWORD g_animAt;
std::vector<Moved> g_moved;
IUIAutomation* g_uia;
DWORD g_restoredAt;

bool IsDesktopWindow(HWND h) {
    HWND root = GetAncestor(h, GA_ROOT);
    wchar_t cls[32] = {}; GetClassNameW(root, cls, 32);
    return mesa::IsDeskClass(cls);
}

// O gancho do mouse só anota o ponto e passa adiante: quem pergunta "o que está embaixo?" é a thread g_worker.
// Antes o próprio gancho chamava WindowFromPoint, que manda WM_NCHITTEST para a janela embaixo do cursor; com o
// app ocupado (OBS gravando, jogo carregando) o mouse do Windows INTEIRO esperava junto: "o mouse trava e parece
// que cai o fps" (Reddit, 20/11/2026). Um gancho de mouse global tem que voltar na hora.
const UINT WM_RV_DOWN = WM_APP + 82, WM_RV_UP = WM_APP + 83;
DWORD g_worker;

LRESULT CALLBACK MouseHook(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION && g_worker && (w == WM_LBUTTONDOWN || w == WM_LBUTTONUP)) {
        auto* m = (MSLLHOOKSTRUCT*)l;
        PostThreadMessageW(g_worker, w == WM_LBUTTONDOWN ? WM_RV_DOWN : WM_RV_UP, (m->flags & LLMHF_INJECTED) ? 1 : 0, MAKELPARAM(m->pt.x, m->pt.y));
    }
    return CallNextHookEx(g_hook, code, w, l);
}

void Worker() {
    MSG msg;
    PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);  // cria a fila antes de o gancho postar
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        POINT pt{ (short)LOWORD(msg.lParam), (short)HIWORD(msg.lParam) };
        if (msg.message == WM_RV_DOWN) {
            if (notificacoes::IsOpen()) {  // diagnóstico: onde o clique cai com a Central aberta (clique que "não chegava")
                HWND at = WindowFromPoint(pt); wchar_t c[64] = {}; if (at) GetClassNameW(GetAncestor(at, GA_ROOT), c, 64);
                Log(L"clique com a Central aberta: %ld,%ld sobre %ls%ls", pt.x, pt.y, c, msg.wParam ? L" (simulado)" : L"");
            }
            g_down = pt;
            g_downOnDesk = IsDesktopWindow(WindowFromPoint(pt));
        } else if (msg.message == WM_RV_UP && g_downOnDesk) {
            g_downOnDesk = false;
            if (std::abs(pt.x - g_down.x) <= 4 && std::abs(pt.y - g_down.y) <= 4)  // clique, não arrasto de seleção
                PostMessageW(g_wnd, WM_RV_CLICK, 0, msg.lParam);
        }
    }
}

// vazio = não há ícone da área de trabalho embaixo do clique
bool EmptySpot(POINT pt) {
    {  // a nossa mesa (sem Explorador) não tem itens de UI Automation: pergunta a ela
        wchar_t c[32] = {}; GetClassNameW(GetAncestor(WindowFromPoint(pt), GA_ROOT), c, 32);
        if (!wcscmp(c, L"SequoiaMesa")) return !mesa::IconAt(pt);
    }
    if (!g_uia) return true;
    IUIAutomationElement* e = nullptr;
    if (FAILED(g_uia->ElementFromPoint(pt, &e)) || !e) return true;
    bool empty = true;
    IUIAutomationTreeWalker* tw = nullptr;
    g_uia->get_ControlViewWalker(&tw);
    IUIAutomationElement* cur = e; cur->AddRef();
    for (int depth = 0; cur && depth < 3; depth++) {  // o próprio item ou o texto/imagem dentro dele
        CONTROLTYPEID t = 0; cur->get_CurrentControlType(&t);
        if (t == UIA_ListItemControlTypeId) { empty = false; break; }
        if (t == UIA_ListControlTypeId || t == UIA_PaneControlTypeId) break;
        IUIAutomationElement* parent = nullptr;
        if (tw) tw->GetParentElement(cur, &parent);
        cur->Release();
        cur = parent;
    }
    if (cur) cur->Release();
    if (tw) tw->Release();
    e->Release();
    return empty;
}

bool Wanted(HWND h, IVirtualDesktopManager* vdm) {
    if (!IsWindowVisible(h) || IsIconic(h) || apps::IsShellWindow(h)) return false;
    LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    if ((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW)) return false;
    if (ex & WS_EX_TOPMOST) return false;
    if (GetWindow(h, GW_OWNER) && !(ex & WS_EX_APPWINDOW)) return false;
    DWORD cloaked = 0; DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked || GetWindowTextLengthW(h) == 0) return false;
    if (IsDesktopWindow(h)) return false;
    wchar_t cls[64]; GetClassNameW(h, cls, 64);
    if (!wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Shell_SecondaryTrayWnd")) return false;
    BOOL on = TRUE;
    if (vdm && SUCCEEDED(vdm->IsWindowOnCurrentVirtualDesktop(h, &on)) && !on) return false;
    return true;
}

float Lerp(float a, float b, float k) { return a + (b - a) * k; }

// ---- Animação pelo compositor, como no Mac: uma camada por cima da tela mostra a imagem viva da mesa e
// das janelas (miniaturas do DWM) e só elas andam, no ritmo do monitor; as janelas de verdade pulam para
// o lugar final de uma vez, escondidas atrás da camada. Antes cada app redesenhava a cada quadro (lento).
enum Phase { P_IDLE, P_OUT, P_BACK, P_SETTLE_OUT, P_SETTLE_BACK };
Phase g_phase = P_IDLE;
double g_t0;
float g_dur = 0.45f;                       // s (mola suave)
const float RESPONSE = 0.42f, DAMPING = 0.95f;
HWND g_ov;                                  // a camada (por cima de tudo, menos o Dock e a barra de menus)
RECT g_ovRect{};
std::vector<HTHUMBNAIL> g_deskThumbs;       // a mesa (papel de parede e ícones)
std::vector<HTHUMBNAIL> g_winThumbs;        // uma por janela de g_moved
float g_fromK = 0;                          // de onde a animação de volta começa (0 = no lugar, 1 = na borda)

RECT At(const Moved& m, float k) {          // posição da janela (tela) para o avanço k (0 lugar, 1 borda)
    RECT r = m.from;
    int dx = (int)std::lround(Lerp(0, (float)(m.to.left - m.from.left), k));
    OffsetRect(&r, dx, 0);
    return r;
}

// ---- compositor: fotos da mesa (paradas) e das janelas (deslizam)
IDCompositionTarget* g_ct;
IDCompositionVisual* g_croot;
bool g_comp;                                // a camada mostrada agora é a do compositor
struct Photo { IDCompositionVisual* v = nullptr; IDCompositionSurface* s = nullptr; };
std::vector<Photo> g_deskP, g_winP;         // g_winP alinhado com g_moved

void FreePhotos() {
    for (auto* list : { &g_deskP, &g_winP })
        for (auto& p : *list) { if (p.v) p.v->Release(); if (p.s) p.s->Release(); }
    g_deskP.clear(); g_winP.clear();
    if (g_croot) { g_croot->RemoveAllVisuals(); ui::Comp()->Commit(); }
}

bool CompInit() {
    if (g_ct) return true;
    IDCompositionDevice* cd = ui::Comp();
    if (!cd || FAILED(cd->CreateTargetForHwnd(g_ov, TRUE, &g_ct)) || FAILED(cd->CreateVisual(&g_croot))) { Log(L"revelar: sem compositor"); return false; }
    g_croot->SetBitmapInterpolationMode(DCOMPOSITION_BITMAP_INTERPOLATION_MODE_LINEAR);
    g_ct->SetRoot(g_croot);
    return true;
}

float PhotoX(const Moved& m, float k) { return Lerp((float)m.from.left, (float)m.to.left, k) + m.src.left - g_ovRect.left; }

// fotos da mesa (de baixo para cima) e das janelas (de baixo para cima), as janelas na posição k
bool CompShow(const std::vector<HWND>& desk, float k) {
    FreePhotos();
    IDCompositionDevice* cd = ui::Comp();
    std::vector<HWND> hs; std::vector<RECT> srcs;
    for (HWND h : desk) { RECT wr; GetWindowRect(h, &wr); hs.push_back(h); srcs.push_back({ 0, 0, wr.right - wr.left, wr.bottom - wr.top }); }
    for (auto& m : g_moved) { hs.push_back(m.h); srcs.push_back(m.src); }
    double t0 = anim::Now();
    auto got = foto::Windows(hs, srcs);
    for (size_t i = 0; i < got.size(); i++)
        if (!got[i].surf) {
            for (auto& g : got) if (g.surf) g.surf->Release();
            Log(L"revelar: sem foto de uma janela; quadro a quadro");
            return false;
        }
    IDCompositionVisual* prev = nullptr;  // cada uma acima da anterior
    auto add = [&](IDCompositionSurface* surf, float x, float y) {
        Photo p; p.s = surf;
        cd->CreateVisual(&p.v);
        p.v->SetContent(surf);
        p.v->SetOffsetX(x); p.v->SetOffsetY(y);
        g_croot->AddVisual(p.v, TRUE, prev);
        prev = p.v;
        return p;
    };
    for (size_t i = 0; i < desk.size(); i++) {
        RECT wr; GetWindowRect(desk[i], &wr);
        g_deskP.push_back(add(got[i].surf, (float)(wr.left - g_ovRect.left), (float)(wr.top - g_ovRect.top)));
    }
    g_winP.assign(g_moved.size(), Photo{});
    for (int i = (int)g_moved.size() - 1; i >= 0; i--) {  // g_moved está de cima para baixo
        const Moved& m = g_moved[i];
        g_winP[i] = add(got[desk.size() + i].surf, PhotoX(m, k), (float)(m.from.top + m.src.top - g_ovRect.top));
    }
    cd->Commit();
    Log(L"revelar: %zu fotos em %.1f ms", hs.size(), anim::Now() - t0);
    return true;
}

// as janelas deslizam de k0 a k1 pela mola, feito pelo compositor
void CompSlide(float k0, float k1) {
    const int K = 24;
    for (size_t i = 0; i < g_moved.size() && i < g_winP.size(); i++) {
        if (!g_winP[i].v) continue;
        std::vector<float> x(K);
        for (int k = 0; k < K; k++) {
            float s = k == K - 1 ? 1.f : std::min(1.f, anim::Spring(g_dur * k / (K - 1), RESPONSE, DAMPING));
            x[k] = PhotoX(g_moved[i], Lerp(k0, k1, s));
        }
        if (auto* a = ui::CompCurve(x, g_dur)) { g_winP[i].v->SetOffsetX(a); a->Release(); }
    }
    ui::Comp()->Commit();
}

void ClearThumbs() {
    for (auto t : g_deskThumbs) DwmUnregisterThumbnail(t);
    for (auto t : g_winThumbs) if (t) DwmUnregisterThumbnail(t);
    g_deskThumbs.clear(); g_winThumbs.clear();
}

void PlaceThumbs(float k) {
    if (g_comp) return;  // o compositor move as fotos
    for (size_t i = 0; i < g_moved.size() && i < g_winThumbs.size(); i++) {
        if (!g_winThumbs[i]) continue;
        // só a parte visível: a borda invisível (8 px; nas maximizadas fica fora da tela) aparecia como uma
        // linha clara acompanhando a janela
        const Moved& m = g_moved[i];
        RECT r = At(m, k);
        RECT d{ r.left + m.src.left, r.top + m.src.top, r.left + m.src.right, r.top + m.src.bottom };
        DWM_THUMBNAIL_PROPERTIES p{};
        p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_RECTSOURCE | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
        p.rcSource = m.src;
        p.rcDestination = { d.left - g_ovRect.left, d.top - g_ovRect.top, d.right - g_ovRect.left, d.bottom - g_ovRect.top };
        p.fVisible = TRUE; p.opacity = 255; p.fSourceClientAreaOnly = FALSE;
        DwmUpdateThumbnailProperties(g_winThumbs[i], &p);
    }
}

// mostra a camada com a mesa e as janelas na posição k
void ShowOverlay(float k) {
    ClearThumbs();
    FreePhotos();
    SetWindowPos(g_ov, HWND_TOPMOST, g_ovRect.left, g_ovRect.top, g_ovRect.right - g_ovRect.left, g_ovRect.bottom - g_ovRect.top, SWP_NOACTIVATE);
    // a mesa: as janelas da área de trabalho, de baixo para cima (a registrada por último fica por cima)
    std::vector<HWND> desk;
    for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT)) {
        wchar_t cls[32] = {}; GetClassNameW(h, cls, 32);
        if (mesa::IsDeskClass(cls) && IsWindowVisible(h)) desk.push_back(h);
    }
    std::reverse(desk.begin(), desk.end());
    g_comp = shell::Setting(L"revelar_motor", L"composicao") == L"composicao" && CompInit() && CompShow(desk, k);
    if (g_comp) {
        ShowWindow(g_ov, SW_SHOWNOACTIVATE);
        dock::Raise();
        if (HWND mb = menubar::Hwnd()) SetWindowPos(mb, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        DwmFlush();
        return;
    }
    for (auto it = desk.begin(); it != desk.end(); ++it) {
        HTHUMBNAIL th = nullptr;
        if (FAILED(DwmRegisterThumbnail(g_ov, *it, &th))) continue;
        RECT wr; GetWindowRect(*it, &wr);
        DWM_THUMBNAIL_PROPERTIES p{};
        p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY;
        p.rcDestination = { wr.left - g_ovRect.left, wr.top - g_ovRect.top, wr.right - g_ovRect.left, wr.bottom - g_ovRect.top };
        p.fVisible = TRUE; p.opacity = 255;
        DwmUpdateThumbnailProperties(th, &p);
        g_deskThumbs.push_back(th);
    }
    // as janelas, de baixo para cima (g_moved está de cima para baixo)
    g_winThumbs.assign(g_moved.size(), nullptr);
    for (int i = (int)g_moved.size() - 1; i >= 0; i--) {
        HTHUMBNAIL th = nullptr;
        if (SUCCEEDED(DwmRegisterThumbnail(g_ov, g_moved[i].h, &th))) g_winThumbs[i] = th;
    }
    PlaceThumbs(k);
    ShowWindow(g_ov, SW_SHOWNOACTIVATE);
    dock::Raise();  // o Dock e a barra de menus continuam por cima, como no Mac
    if (HWND mb = menubar::Hwnd()) SetWindowPos(mb, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    DwmFlush();  // a camada aparece antes das janelas de verdade se mexerem
}

void HideOverlay() {
    ShowWindow(g_ov, SW_HIDE);
    ClearThumbs();
    FreePhotos();
}

// as janelas de verdade: uma por vez e sem esperar (uma travada não segura as outras)
void MoveReal(float k) {
    for (auto& m : g_moved) {
        if (!IsWindow(m.h)) continue;
        RECT r = At(m, k);
        if (IsIconic(m.h)) {
            // minimizada com as janelas na borda (Win+D, ocultar): mover a janela minimizada não muda onde ela volta.
            // Muda o lugar de restaurar, a partir da borda (onde ela foi minimizada), sem tirar do Dock
            WINDOWPLACEMENT wp{ sizeof(wp) };
            if (m.atEdge && k < 0.5f && GetWindowPlacement(m.h, &wp)) {
                m.atEdge = false;
                OffsetRect(&wp.rcNormalPosition, r.left - m.to.left, 0);
                wp.flags = 0; wp.showCmd = SW_SHOWMINNOACTIVE;
                SetWindowPlacement(m.h, &wp);
            }
            continue;
        }
        bool ok = SetWindowPos(m.h, nullptr, r.left, r.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS) != FALSE;
        if (ok) m.atEdge = k > 0.5f;
        if (!ok && !m.failed) {
            m.failed = true;
            wchar_t t[80] = {}; GetWindowTextW(m.h, t, 80);
            Log(L"revelar: o Windows não deixou mover \"%ls\" (%lu)", t, GetLastError());
        }
    }
}

float Progress() {  // 0..1 pela mola, desde g_t0
    float t = (float)((anim::Now() - g_t0) / 1000.0);
    return std::min(1.f, anim::Spring(t, RESPONSE, DAMPING));
}

void Step() {
    float t = (float)((anim::Now() - g_t0) / 1000.0);
    if (g_phase == P_OUT || g_phase == P_BACK) {
        float s = Progress();
        float k = g_phase == P_OUT ? Lerp(g_fromK, 1, s) : Lerp(g_fromK, 0, s);
        PlaceThumbs(k);
        if (t >= g_dur) {
            if (g_phase == P_OUT) MoveReal(1);  // atrás da camada: ninguém vê o pulo
            g_phase = g_phase == P_OUT ? P_SETTLE_OUT : P_SETTLE_BACK;
            g_t0 = anim::Now();
        }
        return;
    }
    if ((g_phase == P_SETTLE_OUT || g_phase == P_SETTLE_BACK) && anim::Now() - g_t0 > 70) {  // as janelas já redesenharam no lugar novo
        bool back = g_phase == P_SETTLE_BACK;
        HideOverlay();
        g_phase = P_IDLE;
        ui::FrameClock(g_wnd, WM_RV_FRAME, false);
        if (back) {
            g_revealed = false;
            if (!g_moved.empty() && IsWindow(g_moved.front().h)) {  // a que estava na frente volta a ter o foco
                keybd_event(VK_MENU, 0, 0, 0); SetForegroundWindow(g_moved.front().h); keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
            }
            g_moved.clear();
        }
        g_restoredAt = GetTickCount();
    }
}

void Reveal() {
    MONITORINFO mi{ sizeof(mi) };
    POINT c; GetCursorPos(&c);
    HMONITOR mon = MonitorFromPoint(c, MONITOR_DEFAULTTOPRIMARY);
    GetMonitorInfoW(mon, &mi);
    g_ovRect = mi.rcMonitor;
    UINT dpi = GetDpiForWindow(menubar::Hwnd() ? menubar::Hwnd() : GetDesktopWindow());
    int peek = MulDiv(PEEK, dpi ? dpi : 96, 96);
    IVirtualDesktopManager* vdm = nullptr;
    if (!mesa::Standalone())  // sem o Explorador não há mesas virtuais (a interface é dele)
        CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&vdm));
    g_moved.clear();
    int mid = (mi.rcWork.left + mi.rcWork.right) / 2;
    std::vector<HWND> all;  // a lista inteira primeiro, de cima para baixo
    for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT))
        if (Wanted(h, vdm) && MonitorFromWindow(h, MONITOR_DEFAULTTONULL) == mon) all.push_back(h);
    if (vdm) vdm->Release();
    for (HWND h : all) {
        Moved m; m.h = h;
        GetWindowRect(h, &m.from);
        RECT vis = m.from; DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &vis, sizeof(vis));  // sem a borda invisível
        if (IsZoomed(h)) { RECT x; IntersectRect(&x, &vis, &mi.rcMonitor); vis = x; }  // maximizada: só o que está na tela
        m.src = { vis.left - m.from.left, vis.top - m.from.top, vis.right - m.from.left, vis.bottom - m.from.top };
        int left = vis.left - m.from.left, w = vis.right - vis.left;
        int cx = (vis.left + vis.right) / 2;
        // para a borda mais perto, deixando uma faixa (as maximizadas também, como no Mac)
        int nx = cx < mid ? mi.rcWork.left - w + peek : mi.rcWork.right - peek;
        m.to = m.from;
        OffsetRect(&m.to, nx - left - m.from.left, 0);
        g_moved.push_back(m);
    }
    if (g_moved.empty()) return;
    g_revealed = true;
    g_fromK = 0;
    ShowOverlay(0);
    g_phase = P_OUT;
    g_t0 = anim::Now();
    if (g_comp) CompSlide(0, 1);
    g_animAt = GetTickCount();
    ui::FrameClock(g_wnd, WM_RV_FRAME, true);
    SetTimer(g_wnd, 9, 500, nullptr);  // trava de segurança (Proc, WM_TIMER)
    Log(L"revelar: %zu janelas para as bordas", g_moved.size());
}

void Restore() {
    if (!g_revealed || g_phase == P_BACK || g_phase == P_SETTLE_BACK) return;
    float k = 1;
    if (g_phase == P_OUT) k = Lerp(g_fromK, 1, Progress());  // ainda saindo: volta de onde está
    else if (g_phase == P_IDLE || g_phase == P_SETTLE_OUT) {
        // as janelas estão na borda: a camada aparece com elas lá e as de verdade voltam escondidas
        if (g_phase == P_IDLE) ShowOverlay(1);
        k = 1;
    }
    MoveReal(0);
    g_fromK = k;
    g_phase = P_BACK;
    g_t0 = anim::Now();
    if (g_comp) CompSlide(k, 0);
    g_animAt = GetTickCount();
    ui::FrameClock(g_wnd, WM_RV_FRAME, true);
    SetTimer(g_wnd, 9, 500, nullptr);  // trava de segurança (Proc, WM_TIMER)
}

LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_RV_CLICK: {
        POINT pt{ (short)LOWORD(l), (short)HIWORD(l) };
        if (missioncontrol::IsOpen()) return 0;
        // em cima do Dock (o vidro dele): o clique passa pelos vãos transparentes entre os ícones e chegava à mesa
        // como "clique ao lado do Dock": as janelas iam para as bordas e voltavam com o app aberto pelo Dock
        // (Reddit, 20/11/2026: "clico embaixo, ele tenta minimizar tudo e tudo volta")
        if (HWND f = dock::Fundo(); f && IsWindowVisible(f)) {
            RECT r; GetWindowRect(f, &r);
            UINT d = GetDpiForWindow(f);
            InflateRect(&r, MulDiv(12, d ? d : 96, 96), MulDiv(12, d ? d : 96, 96));
            if (PtInRect(&r, pt)) return 0;
        }
        // como no macOS 15/26: "Clicar no papel de parede para mostrar a mesa" vem em "Só no Organizador Visual";
        // "Sempre" empurrava as janelas para as bordas a cada clique na mesa (o usuário: "os apps pulam", 17/11/2026)
        if (!g_revealed) {
            std::wstring modo = shell::Setting(L"clique_mostra_mesa", L"palco");
            // ao lado do Dock (a faixa de baixo da tela) o clique sempre mostra a mesa: é o gesto de propósito. No resto
            // da mesa só no modo "Sempre" (o usuário: clicar na mesa não pode empurrar os apps; o canto de baixo, sim)
            MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), &mi);
            UINT dpi = GetDpiForWindow(menubar::Hwnd() ? menubar::Hwnd() : GetDesktopWindow());
            bool besideDock = pt.y >= mi.rcMonitor.bottom - MulDiv(100, dpi ? dpi : 96, 96);
            if (modo == L"0") return 0;
            if (modo == L"palco" && shell::Setting(L"organizador", L"0") != L"1" && !besideDock) return 0;
        }
        if (!EmptySpot(pt)) return 0;  // clicou num ícone: só seleciona, como no Mac
        if (g_revealed) Restore(); else Reveal();
        return 0;
    }
    case WM_RV_FRAME: Step(); return 0;
    case WM_TIMER:
        // trava de segurança: a camada fica por cima da tela inteira e engole os cliques; se os quadros
        // pararem (já aconteceu: relógio desligado), a animação nunca termina e o PC parece travado
        if (w == 9) {
            if (g_phase == P_IDLE) { KillTimer(h, 9); return 0; }
            if (GetTickCount() - g_animAt > 2500) {
                Log(L"revelar: animação parada há 2,5 s (fase %d): terminando à força", (int)g_phase);
                if (g_phase == P_OUT) { MoveReal(1); g_phase = P_SETTLE_OUT; }
                else if (g_phase == P_BACK) g_phase = P_SETTLE_BACK;
                g_t0 = anim::Now() - 1000;
                Step();
                if (g_phase != P_IDLE) { HideOverlay(); g_phase = P_IDLE; ui::FrameClock(g_wnd, WM_RV_FRAME, false); }
                KillTimer(h, 9);
            }
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void HookThread() {
    g_hook = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, GetModuleHandleW(nullptr), 0);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {}
    if (g_hook) UnhookWindowsHookEx(g_hook);
    g_hook = nullptr;
}

}  // namespace

void revelar::Start() {
    if (shell::Setting(L"clique_mostra_mesa", L"palco") == L"0") return;  // "Nunca": nem o gancho do mouse
    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = shell::inst; wc.lpszClassName = L"ShellRevelar";
    RegisterClassW(&wc);
    g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, HWND_MESSAGE, nullptr, shell::inst, nullptr);
    WNDCLASSW oc{}; oc.lpfnWndProc = OverlayProc; oc.hInstance = shell::inst; oc.lpszClassName = L"ShellRevelarCamada";
    RegisterClassW(&oc);
    g_ov = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP | WS_EX_TRANSPARENT, oc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, shell::inst, nullptr);
    apps::ours.push_back(g_ov);
    g_dur = std::min(0.6f, anim::SpringDuration(RESPONSE, DAMPING));
    CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_uia));
    std::thread([] { g_worker = GetCurrentThreadId(); Worker(); }).detach();
    std::thread t([] { g_hookThread = GetCurrentThreadId(); HookThread(); });
    t.detach();
}

// Modo jogo (jogo.cpp): sem o gancho global do mouse enquanto o jogo roda (cada clique do jogo passava pelo Shell);
// o gancho volta numa thread nova quando o jogo fecha
void revelar::Pausar(bool p) {
    if (!g_wnd) return;  // "clique mostra a mesa" desligado: não há gancho
    if (p) { if (DWORD t = g_hookThread) { g_hookThread = 0; PostThreadMessageW(t, WM_QUIT, 0, 0); } }
    else if (!g_hookThread) std::thread([] { g_hookThread = GetCurrentThreadId(); HookThread(); }).detach();
}

// outra janela veio para a frente (clicou na faixa de uma janela, Alt+Tab, Dock): todas voltam
void revelar::Foreground(HWND h) {
    if (!g_revealed || !h || apps::IsShellWindow(h) || IsDesktopWindow(h)) return;
    if (g_phase == P_OUT || g_phase == P_SETTLE_OUT || GetTickCount() - g_animAt < 900) return;  // o foco pulando enquanto as janelas saem não conta
    Restore();
}

bool revelar::Owns(HWND h) {
    if (!g_revealed) return false;
    for (auto& m : g_moved) if (m.h == h && m.minimized) return true;
    return false;
}

void revelar::Stop() {
    if (g_ov) HideOverlay();
    if (g_revealed) {  // devolve as janelas na hora
        for (auto& m : g_moved) {
            if (!IsWindow(m.h)) continue;
            if (m.minimized) ShowWindow(m.h, SW_SHOWMAXIMIZED);
            else SetWindowPos(m.h, nullptr, m.from.left, m.from.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        g_moved.clear();
        g_revealed = false;
    }
    if (g_hookThread) PostThreadMessageW(g_hookThread, WM_QUIT, 0, 0);
    if (g_uia) { g_uia->Release(); g_uia = nullptr; }
}
