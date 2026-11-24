// A barra de menus e o Dock são linhas que as janelas não cruzam (como no Mac): ao soltar uma janela depois
// de arrastar/redimensionar, ou quando uma janela nova aparece, o que passou da área de trabalho volta para
// dentro dela. Maximizadas já respeitam a área (o Windows usa a área de trabalho); tela cheia (jogos,
// vídeo, F11) e janelas de ferramenta ficam como estão.
#include "shell.h"
#include <dwmapi.h>
#include <algorithm>
#include <cmath>
#include <vector>
#include <thread>
#include <mutex>

namespace {

HWINEVENTHOOK g_move, g_show;

bool Candidate(HWND h) {
    if (!IsWindow(h) || GetAncestor(h, GA_ROOT) != h || !IsWindowVisible(h) || IsIconic(h) || IsZoomed(h)) return false;
    LONG st = GetWindowLongW(h, GWL_STYLE), ex = GetWindowLongW(h, GWL_EXSTYLE);
    if ((st & WS_CHILD) || (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST))) return false;
    if (!(st & WS_CAPTION) && !(st & WS_THICKFRAME)) return false;  // popups sem moldura (menus, avisos, splash)
    if (shell::Nossa(h)) return false;
    if (GetPropW(h, L"MacJanelaAmpliada")) return false;  // ampliada pelo botão verde: já ocupa a área certa
    return true;
}

// Volta deslizando (ease-out, ~0,18 s), como no Mac, em vez de pular para o lugar. Janela com a nossa barra:
// a DLL anima pela foto no compositor (o mesmo do ampliar: liso na taxa do monitor mesmo com o app pesado).
// As outras: uma thread move a janela a cada quadro da tela (DwmFlush), não por temporizador (~30 quadros).
std::vector<HWND> g_sliding;
std::mutex g_slideMx;

void SlideThread(HWND h, RECT from, RECT to) {
    const double DUR = 180;
    double t0 = anim::Now();
    for (;;) {
        DwmFlush();
        double u = std::min(1.0, (anim::Now() - t0) / DUR);
        double e = 1 - (1 - u) * (1 - u) * (1 - u);  // desacelera no fim
        auto lerp = [e](LONG a, LONG b) { return (int)std::lround(a + (b - a) * e); };
        if (!IsWindow(h) || IsZoomed(h) || IsIconic(h)) break;
        SetWindowPos(h, nullptr, lerp(from.left, to.left), lerp(from.top, to.top), lerp(from.right, to.right) - lerp(from.left, to.left),
                     lerp(from.bottom, to.bottom) - lerp(from.top, to.top), SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        if (u >= 1) break;
    }
    std::lock_guard<std::mutex> k(g_slideMx);
    g_sliding.erase(std::remove(g_sliding.begin(), g_sliding.end(), h), g_sliding.end());
}

// o usuário está arrastando/redimensionando esta janela agora (aí ela pode passar do Dock até soltar)
bool InMoveSize(HWND h) {
    GUITHREADINFO gi{ sizeof(gi) };
    return GetGUIThreadInfo(GetWindowThreadProcessId(h, nullptr), &gi) && (gi.flags & GUI_INMOVESIZE) && gi.hwndMoveSize == h;
}

void Clamp(HWND h) {
    if (!Candidate(h) || InMoveSize(h)) return;
    RECT wr, vis;
    GetWindowRect(h, &wr);
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &vis, sizeof(vis)))) vis = wr;
    // fora da tela de propósito (o alerta do Mac esconde a caixa original em -32000; o Explorador trocado pelo
    // Finder também): não puxa de volta. Antes ela voltava por cima do alerta e as duas brigavam
    if (wr.left <= -30000 || wr.top <= -30000) return;
    RECT box;  // com a nossa barra a parte visível é a região da janela (a moldura do DWM passa uns px)
    if (GetPropW(h, L"MacJanela") && GetWindowRgnBox(h, &box) != ERROR && box.right > box.left)
        vis = { wr.left + box.left, wr.top + box.top, wr.left + box.right, wr.top + box.bottom };
    MONITORINFO mi{ sizeof(mi) };
    HMONITOR hm = MonitorFromRect(&vis, MONITOR_DEFAULTTONEAREST);
    if (!GetMonitorInfoW(hm, &mi)) return;
    RECT wa = shell::WorkAreaFor(hm), mon = mi.rcMonitor;  // a do Shell: a do Windows pode estar trocada por um instante (issue #1)
    if (vis.left <= mon.left && vis.top <= mon.top && vis.right >= mon.right && vis.bottom >= mon.bottom) return;  // tela cheia
    if (EqualRect(&wa, &mon)) return;  // sem barra nem Dock reservando espaço (Dock escondido, monitor sem barra)
    int w = vis.right - vis.left, hh = vis.bottom - vis.top;
    if (w < 120 || hh < 80) return;
    int dy = 0, newH = hh;
    if (vis.top < wa.top) dy = wa.top - vis.top;                                                 // passou da barra de menus
    else if (vis.bottom > wa.bottom) dy = -std::min(vis.bottom - wa.bottom, vis.top - wa.top);  // passou do Dock: sobe o que der
    // ainda maior que a área: encolhe até o Dock. Com a nossa barra (MacJanela) a janela perde o WS_THICKFRAME
    // de verdade, mas continua redimensionável (Ableton); diálogos de tamanho fixo só mudam de lugar
    bool resizable = (GetWindowLongW(h, GWL_STYLE) & WS_THICKFRAME) || GetPropW(h, L"MacJanela");
    if (vis.top + dy + newH > wa.bottom && resizable) newH = wa.bottom - (vis.top + dy);
    // Dock à esquerda ou à direita (Ajustes > Posição na tela): a mesma conta na horizontal, só do lado dele
    int dx = 0, newW = w;
    if (dock::Posicao() != 0 && hm == dock::MonitorHandle()) {
        if (dock::Posicao() == 1 && vis.left < wa.left) dx = wa.left - vis.left;
        else if (dock::Posicao() == 2 && vis.right > wa.right) dx = -std::min((int)(vis.right - wa.right), std::max(0, (int)(vis.left - wa.left)));
        // à direita, mais larga que a área: encolhe até o Dock (à esquerda, empurrar para dentro já basta)
        if (dock::Posicao() == 2 && vis.left + dx + newW > wa.right && resizable) newW = std::max(120, (int)(wa.right - (vis.left + dx)));
    }
    if (!dy && newH == hh && !dx && newW == w) return;
    {
        std::lock_guard<std::mutex> k(g_slideMx);
        if (std::find(g_sliding.begin(), g_sliding.end(), h) != g_sliding.end()) return;  // já deslizando
        g_sliding.push_back(h);
    }
    int bt = vis.top - wr.top, bb = wr.bottom - vis.bottom;  // borda invisível de redimensionar
    int bl = vis.left - wr.left, brr = wr.right - vis.right;
    RECT to{ wr.left + dx, wr.top + dy, wr.left + dx + newW + bl + brr, wr.top + dy + newH + bt + bb };
    static UINT msgMove = RegisterWindowMessageW(L"MacJanelaMover");
    DWORD_PTR ok = 0;
    // a DLL das janelas só sabe deslizar na vertical: na horizontal (Dock do lado) a janela anda pela thread
    if (!dx && newW == w && GetPropW(h, L"MacJanela") &&
        SendMessageTimeoutW(h, msgMove, (WPARAM)(INT_PTR)dy, (LPARAM)(to.bottom - to.top), SMTO_ABORTIFHUNG, 300, &ok) && ok) {
        std::lock_guard<std::mutex> k(g_slideMx);
        g_sliding.erase(std::remove(g_sliding.begin(), g_sliding.end(), h), g_sliding.end());
    } else {
        std::thread(SlideThread, h, wr, to).detach();
    }
    wchar_t cls[64] = {}; GetClassNameW(h, cls, 64);
    Log(L"limites: %ls voltando para a área (dx %d, dy %d, altura %d -> %d, largura %d -> %d)", cls, dx, dy, hh, newH, w, newW);
}

void CALLBACK OnMoveEnd(HWINEVENTHOOK, DWORD, HWND h, LONG obj, LONG child, DWORD, DWORD) {
    if (obj == OBJID_WINDOW && child == CHILDID_SELF) Clamp(h);
}

// Confere de novo um pouco depois: apps como o Ableton aparecem pequenos e só depois crescem para o tamanho
// salvo (passando da barra e do Dock); ao ligar o PC, a área de trabalho ainda podia estar sem a reserva.
struct Recheck { HWND h; DWORD at; };
std::vector<Recheck> g_rechecks;
UINT_PTR g_recheckTimer;

void CALLBACK RecheckTick(HWND, UINT, UINT_PTR, DWORD) {
    DWORD now = GetTickCount();
    std::vector<HWND> due;
    for (size_t i = 0; i < g_rechecks.size();)
        if ((int)(now - g_rechecks[i].at) >= 0) { due.push_back(g_rechecks[i].h); g_rechecks.erase(g_rechecks.begin() + i); }
        else i++;
    if (g_rechecks.empty() && g_recheckTimer) { KillTimer(nullptr, g_recheckTimer); g_recheckTimer = 0; }
    for (HWND h : due) {
        if (h) Clamp(h);
        else EnumWindows([](HWND w, LPARAM) -> BOOL { if (Candidate(w)) Clamp(w); return TRUE; }, 0);  // todas
    }
}

void Later(HWND h, std::initializer_list<DWORD> delays) {
    DWORD now = GetTickCount();
    for (DWORD d : delays) g_rechecks.push_back({ h, now + d });
    if (!g_recheckTimer) g_recheckTimer = SetTimer(nullptr, 0, 150, RecheckTick);
}

void CALLBACK OnShow(HWINEVENTHOOK, DWORD, HWND h, LONG obj, LONG child, DWORD, DWORD) {
    if (obj != OBJID_WINDOW || child != CHILDID_SELF || !Candidate(h)) return;
    if (GetPropW(h, L"ShellLimiteVisto")) return;  // só na primeira vez (depois o usuário manda)
    SetPropW(h, L"ShellLimiteVisto", (HANDLE)1);
    Clamp(h);
    Later(h, { 600, 1800, 4500 });
}

}  // namespace

void limites::Start() {
    if (shell::Setting(L"limitar_janelas", L"1") != L"1") return;
    if (!g_move) g_move = SetWinEventHook(EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZEEND, nullptr, OnMoveEnd, 0, 0, WINEVENT_OUTOFCONTEXT | shell::SemProprio());
    if (!g_show) g_show = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, OnShow, 0, 0, WINEVENT_OUTOFCONTEXT | shell::SemProprio());
    // as que já estavam abertas (Shell reiniciado, ou janela grande de antes passando por baixo do Dock)
    EnumWindows([](HWND h, LPARAM) -> BOOL { if (Candidate(h)) { SetPropW(h, L"ShellLimiteVisto", (HANDLE)1); Clamp(h); } return TRUE; }, 0);
    Later(nullptr, { 2500, 8000, 20000 });  // ao ligar o PC os apps ainda estão abrindo e se ajeitando
}

void limites::Stop() {
    if (g_move) { UnhookWinEvent(g_move); g_move = nullptr; }
    if (g_show) { UnhookWinEvent(g_show); g_show = nullptr; }
    if (g_recheckTimer) { KillTimer(nullptr, g_recheckTimer); g_recheckTimer = 0; }
    g_rechecks.clear();
}
