// Cantos Ativos, como no Mac (Ajustes do Sistema > Mesa e Dock > Cantos Ativos): levar o ponteiro até um
// canto da tela faz uma ação (Mission Control, mostrar a mesa, Launchpad, Central de Notificações...).
// Ajustes: canto_se / canto_sd / canto_ie / canto_id (superior/inferior, esquerdo/direito) com
//   nada | mission | janelas | mesa | launchpad | notificacoes | central | spotlight | bloquear | tela | nota
//   (janelas = Janelas do Aplicativo: o Exposé do app da frente)
// O canto inferior direito vem com "nota": a Nota Rápida (notarapida.cpp, que tem o quadradinho dela).
// Dispara uma vez ao chegar (precisa sair do canto para disparar de novo); não dispara com botão do mouse
// apertado (arrastando) nem com um app em tela cheia na frente (jogo, vídeo).
#include "shell.h"
#include <algorithm>

namespace {

const UINT_PTR T_CANTO = 0x4341;
int g_armed = -1;          // canto onde o ponteiro está (já disparou); -1 = fora de todos
DWORD g_arriveAt;          // chegou ao canto (uma pequena espera evita disparos ao passar raspando)
int g_pending = -1;

const wchar_t* KEYS[4] = { L"canto_se", L"canto_sd", L"canto_ie", L"canto_id" };

std::wstring ActionOf(int c) { return shell::Setting(KEYS[c], c == 3 ? L"nota" : L"nada"); }

bool FullscreenInFront(const RECT& mon) {
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    wchar_t cls[64] = {}; GetClassNameW(fg, cls, 64);
    if (mesa::IsDeskClass(cls)) return false;
    RECT r; GetWindowRect(fg, &r);
    return r.left <= mon.left && r.top <= mon.top && r.right >= mon.right && r.bottom >= mon.bottom && !(GetWindowLongW(fg, GWL_STYLE) & WS_CAPTION);
}

void Run(int c) {
    std::wstring a = ActionOf(c);
    Log(L"cantos: %ls -> %ls", KEYS[c], a.c_str());
    if (a == L"mission") missioncontrol::Toggle();
    else if (a == L"janelas") {
        HWND fg = GetForegroundWindow();
        HWND root = fg ? GetAncestor(fg, GA_ROOTOWNER) : nullptr;
        apps::Refresh();
        if (App* ap = root && !apps::IsShellWindow(root) ? apps::OfWindow(root) : nullptr) missioncontrol::ToggleApp(ap->id, ap->name);
    }
    else if (a == L"mesa") mesa::WinKey('D');
    else if (a == L"launchpad") launchpad::Toggle();
    else if (a == L"notificacoes") notificacoes::Toggle();
    else if (a == L"central") central::Toggle();
    else if (a == L"spotlight") spotlight::Toggle();
    else if (a == L"bloquear") bloqueio::Lock();
    else if (a == L"tela") PostMessageW(HWND_BROADCAST, WM_SYSCOMMAND, SC_MONITORPOWER, 2);  // descansar a tela
}

void CALLBACK Tick(HWND, UINT, UINT_PTR, DWORD) {
    POINT p;
    if (!GetCursorPos(&p)) return;
    MONITORINFO mi{ sizeof(mi) };
    if (!GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), &mi)) return;
    RECT m = mi.rcMonitor;
    const int E = 2;  // px de folga
    bool L = p.x <= m.left + E, R = p.x >= m.right - 1 - E, T = p.y <= m.top + E, B = p.y >= m.bottom - 1 - E;
    int c = T && L ? 0 : T && R ? 1 : B && L ? 2 : B && R ? 3 : -1;
    if (c < 0) { g_armed = g_pending = -1; return; }
    if (c == g_armed) return;  // já disparou neste canto
    std::wstring a = ActionOf(c);
    if (a == L"nada" || a == L"nota") { g_armed = c; return; }  // "nota": a Nota Rápida cuida (quadradinho)
    if (GetAsyncKeyState(VK_LBUTTON) < 0 || GetAsyncKeyState(VK_RBUTTON) < 0 || FullscreenInFront(m)) { g_armed = c; return; }
    if (g_pending != c) { g_pending = c; g_arriveAt = GetTickCount(); return; }
    if (GetTickCount() - g_arriveAt < 120) return;  // ~2 leituras no canto: chegou de propósito
    g_armed = c; g_pending = -1;
    Run(c);
}

}  // namespace

static UINT_PTR g_timer;  // sem janela: o Windows dá o número do temporizador (o T_CANTO é ignorado)
void cantos::Start() {
    g_timer = SetTimer(nullptr, T_CANTO, 50, Tick);
}

// modo jogo (jogo.cpp): sem ler o ponteiro 20 vezes por segundo enquanto o jogo roda
void cantos::Pausar(bool p) {
    if (p && g_timer) { KillTimer(nullptr, g_timer); g_timer = 0; }
    else if (!p && !g_timer) g_timer = SetTimer(nullptr, T_CANTO, 50, Tick);
}

bool cantos::NotaRapida() { return ActionOf(3) == L"nota"; }
