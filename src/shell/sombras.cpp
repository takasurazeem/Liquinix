// Sombra do Mac nas janelas que a MacJanela.dll não trata (Chrome, Discord, Claude, VS Code, apps da
// loja...): o Shell põe uma janela de sombra logo abaixo de cada uma e a move junto (eventos do Windows).
// As tratadas pela DLL têm a própria sombra, dentro do programa, que acompanha sem atraso.
#include "shell.h"
#include "../comum/sombra.h"
#include <map>
#include <memory>

namespace {

std::map<HWND, std::unique_ptr<sombra::Sombra>> g_s;
HWINEVENTHOOK g_hooks[4];

bool Eligible(HWND h) {
    if (!IsWindow(h) || GetAncestor(h, GA_ROOT) != h || !IsWindowVisible(h) || IsIconic(h) || IsZoomed(h)) return false;
    if (apps::IsShellWindow(h) || GetPropW(h, L"MacJanela")) return false;  // nossas e as da DLL (têm sombra própria)
    LONG st = GetWindowLongW(h, GWL_STYLE), ex = GetWindowLongW(h, GWL_EXSTYLE);
    if ((ex & WS_EX_TOOLWINDOW) || (ex & WS_EX_LAYERED) || (ex & WS_EX_TRANSPARENT)) return false;
    if (!(st & WS_THICKFRAME) && (st & WS_CAPTION) != WS_CAPTION) return false;  // só janelas "de verdade"
    DWORD cloaked = 0;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked || GetWindowTextLengthW(h) == 0) return false;
    wchar_t cls[64]; GetClassNameW(h, cls, 64);
    if (mesa::IsDeskClass(cls)) return false;
    for (auto c : { L"Shell_TrayWnd", L"ArquivosJanela", L"AjustesJanela", L"MonitorAtividadeJanela", L"TerminalMacJanela", L"CalculadoraJanela", L"NotasJanela", L"NotasRapidaJanela", L"MacJanelaBarra" })
        if (!wcscmp(cls, c)) return false;
    RECT r; GetWindowRect(h, &r);
    return r.right - r.left >= 200 && r.bottom - r.top >= 120;
}

bool Active(HWND h) { HWND f = GetForegroundWindow(); return f && GetAncestor(f, GA_ROOTOWNER) == GetAncestor(h, GA_ROOTOWNER); }

void Handle(HWND h) {
    auto it = g_s.find(h);
    if (!Eligible(h)) {
        if (it != g_s.end()) { it->second->Destroy(); g_s.erase(it); }
        return;
    }
    if (it == g_s.end()) {
        auto s = std::make_unique<sombra::Sombra>();
        if (!s->Create(h, shell::inst, L"ShellSombra")) return;
        it = g_s.emplace(h, std::move(s)).first;
    }
    it->second->tahoe = liquid::On(); it->second->dark = ui::Dark();  // Tahoe: sombra medida no macOS 26
    it->second->Update(Active(h));
}

void UpdateAll() {
    for (auto it = g_s.begin(); it != g_s.end();) {
        if (!Eligible(it->first)) { it->second->Destroy(); it = g_s.erase(it); continue; }
        it->second->tahoe = liquid::On(); it->second->dark = ui::Dark();
        it->second->Update(Active(it->first));
        ++it;
    }
}

void CALLBACK OnEvent(HWINEVENTHOOK, DWORD ev, HWND h, LONG obj, LONG child, DWORD, DWORD) {
    if (obj != OBJID_WINDOW || child != CHILDID_SELF || !h) return;
    if (ev == EVENT_SYSTEM_FOREGROUND) { Handle(h); UpdateAll(); return; }  // ativa/inativa e a ordem mudaram
    if (ev == EVENT_OBJECT_DESTROY) {
        auto it = g_s.find(h);
        if (it != g_s.end()) { it->second->Destroy(); g_s.erase(it); }
        return;
    }
    if (GetAncestor(h, GA_ROOT) != h) return;
    Handle(h);
}

}  // namespace

void sombras::Start() {
    if (shell::Setting(L"sombras_mac", L"1") != L"1") return;
    DWORD f = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    g_hooks[0] = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnEvent, 0, 0, f);
    g_hooks[1] = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, nullptr, OnEvent, 0, 0, f);
    g_hooks[2] = SetWinEventHook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE, nullptr, OnEvent, 0, 0, f);  // destruir, mostrar, esconder
    jogo::GanchoPosicao(OnEvent, f);  // sai no modo jogo (jogo.cpp)
    EnumWindows([](HWND h, LPARAM) -> BOOL { Handle(h); return TRUE; }, 0);
}

void sombras::Stop() {
    for (auto& hk : g_hooks) if (hk) { UnhookWinEvent(hk); hk = nullptr; }
    jogo::SoltarPosicao(OnEvent);
    for (auto& [h, s] : g_s) s->Destroy();
    g_s.clear();
}
