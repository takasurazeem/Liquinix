// Shell.exe: barra de menus + Dock + bandeja no estilo do macOS Sequoia, por cima do Explorador.
// Enquanto roda, esconde a barra de tarefas do Windows e ajusta a área de trabalho; ao sair
// (menu do logo → Sair do Shell, Ctrl+Alt+Shift+F12, ou erro), devolve tudo como estava.
#include "shell.h"
#include "../comum/simbolos.h"
#include "../menu/context_menu.h"
#include "../plataforma/plataforma.h"
#include <shlobj.h>
#include <share.h>
#include <cstdarg>
#include <map>
#include <thread>
#include <atomic>
#include <mmsystem.h>
#pragma comment(lib, "winmm.lib")

HINSTANCE shell::inst;
std::wstring shell::dir;
bool shell::nativo;

DWORD shell::SemProprio() { return nativo ? 0 : WINEVENT_SKIPOWNPROCESS; }

// Janela do próprio Shell. Fora do modo nativo: qualquer janela deste processo. No nativo o processo é o
// explorer.exe: as janelas dele (pastas, Executar, avisos de copiar/apagar) são de app; nossas são só as
// criadas com o módulo do Shell (a MacBarra.dll).
bool shell::Nossa(HWND h) {
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return false;
    if (!nativo) return true;
    return (HINSTANCE)GetWindowLongPtrW(h, GWLP_HINSTANCE) == inst || apps::IsShellWindow(h);
}

static FILE* g_log;
static HWND g_msg;
static UINT WM_SHELLHOOK;
static RECT g_origWork;
static std::vector<HWND> g_hiddenBars;
static std::map<std::wstring, std::wstring> g_settings;
static bool g_quitting;
static bool g_crashed, g_sessionEnding;  // Restore() sem abrir o Explorador (o vigia ou o logoff cuidam)

// ---- tecla Windows: sozinha abre o Launchpad; com outra tecla (Win+E, Win+D, Win+L...) segue para o
// Windows normalmente. O gancho roda numa thread própria para nunca atrasar o teclado.
const ULONG_PTR KEY_MARK = 0x53484C4C;  // teclas que nós mesmos injetamos
const UINT WM_APP_LAUNCHPAD = WM_APP + 30, WM_APP_SETTINGS = WM_APP + 31, WM_APP_MISSION = WM_APP + 32, WM_APP_SNAP = WM_APP + 33, WM_APP_SPOTLIGHT = WM_APP + 34, WM_APP_MONITOR = WM_APP + 35, WM_APP_SWITCH = WM_APP + 36, WM_APP_SWITCH_KEY = WM_APP + 37, WM_APP_SWITCH_END = WM_APP + 38;
static std::atomic<bool> g_altTab;  // seletor de apps aberto (Alt seguro desde o primeiro Tab)
static HHOOK g_kbHook;
static bool g_winDown, g_winCombo, g_winEaten, g_spaceHeld, g_tabHeld;  // eaten: a combinação foi nossa (Win+I)
static DWORD g_winVk, g_winAt;  // At: quando a tecla Win desceu (ms do gancho)

static void Inject(DWORD vk, bool up) {
    INPUT in{ INPUT_KEYBOARD };
    in.ki.wVk = (WORD)vk;
    in.ki.dwFlags = (up ? KEYEVENTF_KEYUP : 0) | (vk == VK_LWIN || vk == VK_RWIN ? KEYEVENTF_EXTENDEDKEY : 0);
    in.ki.dwExtraInfo = KEY_MARK;
    SendInput(1, &in, sizeof(in));
}

const UINT WM_APP_LOCK = WM_APP + 39, WM_APP_WINKEY = WM_APP + 40, WM_APP_CAPTURA = WM_APP + 41;
static bool g_capturaExe;   // LiquinixScreenshot.exe existe: Win+Shift+3/4/5/S são dela
// Win+Q / Win+H / Win+W = Cmd+Q / Cmd+H / Cmd+W do Mac (encerrar o app, ocultar, fechar a janela).
// Ajustes > Teclado > "Atalhos do Mac para apps" (atalhos_mac); desligado, os atalhos voltam ao Windows
const UINT WM_APP_MACKEY = WM_APP + 43;
const UINT WM_APP_EXPOSE = WM_APP + 44;  // Ctrl+Win+↓: Exposé do app da frente (Ctrl+↓ no Mac)
static volatile bool g_macKeys = true;
static DWORD g_macKeyVk;    // a tecla do atalho: soltar também é nosso (o app não vê uma letra solta)
static DWORD g_capKey;      // tecla do atalho da Captura de Tela ainda segurada
static bool g_fullHook = true;  // false: o gancho só existe para a tela de bloqueio (tecla Windows do Launchpad desligada)
const UINT WM_APP_VOLUME = WM_APP + 42;
static bool g_volumeKeys = true;  // ajuste volume_mac: teclas de volume com o aviso do Mac (0 = o Windows trata)

static LRESULT CALLBACK KeyHook(int code, WPARAM w, LPARAM l) {
    if (code == HC_ACTION && w == WM_KEYDOWN && ((KBDLLHOOKSTRUCT*)l)->vkCode == VK_CAPITAL) ilha::CapsLock();  // Ilha Dinâmica: aviso do Caps Lock
    if (code == HC_ACTION && g_volumeKeys && !bloqueio::Active()) {
        // teclas de volume (Fn + F-teclas): o Shell muda o volume em 16 passos e mostra o aviso do Mac
        // (o painel do Windows não aparece). Só essas três teclas: os atalhos Ctrl+Alt+Shift+F11/F12 seguem livres.
        auto* k = (KBDLLHOOKSTRUCT*)l;
        if (k->dwExtraInfo != KEY_MARK && (k->vkCode == VK_VOLUME_UP || k->vkCode == VK_VOLUME_DOWN || k->vkCode == VK_VOLUME_MUTE)) {
            if (w == WM_KEYDOWN || w == WM_SYSKEYDOWN) {
                bool fine = GetAsyncKeyState(VK_SHIFT) < 0 && GetAsyncKeyState(VK_MENU) < 0;
                PostMessageW(g_msg, WM_APP_VOLUME, k->vkCode, fine);
            }
            return 1;
        }
    }
    if (code == HC_ACTION && bloqueio::Active()) {
        // tela travada: nada de Iniciar, Alt+Tab, Ctrl+Esc, Alt+Esc, Alt+F4, Ctrl+Shift+Esc (Ctrl+Alt+Del não dá para barrar)
        auto* k = (KBDLLHOOKSTRUCT*)l;
        bool alt = (k->flags & LLKHF_ALTDOWN) != 0, ctrl = GetAsyncKeyState(VK_CONTROL) < 0;
        DWORD vk = k->vkCode;
        if (vk == VK_LWIN || vk == VK_RWIN || vk == VK_APPS) { g_winDown = false; return 1; }
        if ((alt && (vk == VK_TAB || vk == VK_ESCAPE || vk == VK_F4 || vk == VK_SPACE)) || (ctrl && vk == VK_ESCAPE)) return 1;
        g_altTab = false;
        return CallNextHookEx(g_kbHook, code, w, l);
    }
    if (!g_fullHook) return CallNextHookEx(g_kbHook, code, w, l);
    if (code == HC_ACTION) {
        auto* k = (KBDLLHOOKSTRUCT*)l;
        if (k->dwExtraInfo != KEY_MARK) {
            bool down = w == WM_KEYDOWN || w == WM_SYSKEYDOWN;
            bool win = k->vkCode == VK_LWIN || k->vkCode == VK_RWIN;
            if (win) {
                auto mods = [] { return GetAsyncKeyState(VK_CONTROL) < 0 || GetAsyncKeyState(VK_SHIFT) < 0 || GetAsyncKeyState(VK_MENU) < 0; };
                if (shell::nativo) {
                    // modo nativo (dentro do explorer): a tecla segue para o Windows; o atalho reinjetado daqui não
                    // valia (Win+R não abria). Sozinha, ao soltar, abre o Launchpad
                    if (down) {
                        if (!g_winDown) { g_winDown = true; g_winCombo = mods(); g_winEaten = false; g_spaceHeld = false; g_tabHeld = false; g_winVk = k->vkCode; g_winAt = k->time; }
                    } else if (g_winDown) {
                        g_winDown = false;
                        if (!g_winCombo && k->time - g_winAt <= 1000) PostMessageW(g_msg, WM_APP_LAUNCHPAD, 0, 0);
                    }
                    return CallNextHookEx(g_kbHook, code, w, l);
                }
                if (down) {  // segura: ainda não sabemos se é sozinha ou combinação
                    if (!g_winDown) {
                        g_winDown = true; g_winCombo = false; g_winEaten = false; g_spaceHeld = false; g_tabHeld = false; g_winVk = k->vkCode; g_winAt = k->time;
                        if (mods()) { g_winCombo = true; Inject(g_winVk, false); }  // Ctrl/Shift/Alt já seguros: é atalho do Windows
                    }
                    return 1;
                }
                if (g_winDown) {
                    g_winDown = false;
                    if (g_winCombo) {  // fim da combinação: as nossas (Win+I, Win+Tab) não mandam nada ao Windows
                        if (!g_winEaten) Inject(g_winVk, true);
                    } else if (k->time - g_winAt > 1000) {
                        // segurada muito tempo (ex.: Win + clique/arraste do mouse): não abre nada
                    } else PostMessageW(g_msg, WM_APP_LAUNCHPAD, 0, 0);  // sozinha: Launchpad (e o Iniciar não abre)
                    return 1;
                }
            } else if (k->vkCode == VK_TAB && down && !g_winDown && GetAsyncKeyState(VK_MENU) < 0 && GetAsyncKeyState(VK_CONTROL) >= 0) {
                // Alt+Tab: o seletor de apps do Mac no lugar do Alternar Tarefas do Windows. A tecla "muda"
                // (0xE8) evita que o app da frente veja um Alt sozinho ao soltar e abra o menu dele
                if (!g_altTab) { Inject(0xE8, false); Inject(0xE8, true); }
                g_altTab = true;
                PostMessageW(g_msg, WM_APP_SWITCH, GetAsyncKeyState(VK_SHIFT) < 0 ? (WPARAM)-1 : 1, 0);
                return 1;
            } else if (g_altTab && down && (k->vkCode == 'Q' || k->vkCode == 'H' || k->vkCode == VK_ESCAPE || k->vkCode == VK_LEFT || k->vkCode == VK_RIGHT)) {
                if (k->vkCode == VK_ESCAPE) g_altTab = false;
                PostMessageW(g_msg, WM_APP_SWITCH_KEY, k->vkCode, 0);
                return 1;
            } else if (g_altTab && !down && (k->vkCode == VK_LMENU || k->vkCode == VK_RMENU || k->vkCode == VK_MENU)) {
                g_altTab = false;
                PostMessageW(g_msg, WM_APP_SWITCH_END, 0, 0);  // o Alt solto segue para o Windows normalmente
            } else if (k->vkCode == VK_ESCAPE && down && GetAsyncKeyState(VK_CONTROL) < 0 && GetAsyncKeyState(VK_SHIFT) < 0 && GetAsyncKeyState(VK_MENU) >= 0) {
                // Ctrl+Shift+Esc: o nosso Monitor de Atividade no lugar do Gerenciador de Tarefas
                PostMessageW(g_msg, WM_APP_MONITOR, 0, 0);
                return 1;
            } else if (k->vkCode == VK_ESCAPE && down && GetAsyncKeyState(VK_CONTROL) < 0 && GetAsyncKeyState(VK_MENU) < 0 && GetAsyncKeyState(VK_SHIFT) >= 0) {
                // Ctrl+Alt+Esc: Forçar Encerrar Aplicativos (Command+Option+Esc do Mac)
                PostMessageW(g_msg, WM_APP_MONITOR, 1, 0);
                return 1;
            } else if (k->vkCode == VK_SPACE && g_winDown && (!g_winCombo || g_winEaten)) {
                // Win+Espaço: o Spotlight (Cmd+Espaço no Mac; no Windows trocaria o idioma do teclado).
                // Segurando o Win, cada Espaço abre/fecha de novo; repetição de tecla segurada não conta e
                // nenhum Espaço chega ao app de baixo
                if (down && !g_spaceHeld) PostMessageW(g_msg, WM_APP_SPOTLIGHT, 0, 0);
                g_spaceHeld = down;
                g_winCombo = g_winEaten = true;
                return 1;
            } else if (k->vkCode == VK_TAB && g_winDown && (!g_winCombo || g_winEaten)) {
                // Win+Tab: o Mission Control (no lugar da Visão de Tarefas). Segurando o Win, cada Tab abre/fecha
                // de novo (igual ao Espaço); repetição de tecla segurada não conta
                if (down && !g_tabHeld) PostMessageW(g_msg, WM_APP_MISSION, 0, 0);
                g_tabHeld = down;
                g_winCombo = g_winEaten = true;
                return 1;
            } else if (g_winDown && down && (k->vkCode == VK_F12 || k->vkCode == VK_F8) && liquid::Leopard()) {
                // Leopard: Win+F12 = Dashboard (o F12 do Mac), Win+F8 = Spaces (o F8 do Mac). Só com o Win: o F12 sozinho
                // continua dos apps (ferramentas do navegador)
                static UINT openPanel = RegisterWindowMessageW(L"ShellAbrirPainel");
                PostMessageW(g_msg, openPanel, k->vkCode == VK_F12 ? 16 : 17, 0);
                g_winCombo = g_winEaten = true;
                return 1;
            } else if (g_winDown && down && !g_winCombo && (k->vkCode == 'I' || k->vkCode == VK_TAB)) {
                // Win+I: o nosso Ajustes do Sistema; Win+Tab: o Mission Control (no lugar da Visão de Tarefas)
                g_winCombo = g_winEaten = true;
                PostMessageW(g_msg, k->vkCode == VK_TAB ? WM_APP_MISSION : WM_APP_SETTINGS, 0, 0);
                return 1;
            } else if (!shell::nativo && g_winDown && down && (k->vkCode == VK_LEFT || k->vkCode == VK_RIGHT) && GetAsyncKeyState(VK_CONTROL) >= 0 &&
                       GetAsyncKeyState(VK_SHIFT) >= 0 && GetAsyncKeyState(VK_MENU) >= 0) {
                // Win+←/→: metade da tela (o encaixe do Windows fica desligado enquanto o Shell roda: encaixe.cpp)
                if (!g_winCombo) g_winCombo = g_winEaten = true;
                PostMessageW(g_msg, WM_APP_SNAP, k->vkCode, 0);
                return 1;
            } else if (g_winDown && down && !g_winCombo && (k->vkCode == VK_UP || k->vkCode == VK_DOWN) &&
                       GetPropW(GetForegroundWindow(), L"MacJanela")) {
                // Win+↑/↓ em janela com a barra do Mac: sem o botão de maximizar o Windows ignora; fazemos nós
                g_winCombo = g_winEaten = true;
                PostMessageW(g_msg, WM_APP_SNAP, k->vkCode, 0);
                return 1;
            } else if (!shell::nativo && g_winDown && down && k->vkCode == 'Q' && GetAsyncKeyState(VK_CONTROL) < 0 && GetAsyncKeyState(VK_MENU) >= 0) {
                // Ctrl+Win+Q: o atalho do Mac (Control+Command+Q) para a nossa tela de bloqueio
                if (!g_winCombo) g_winCombo = g_winEaten = true;
                PostMessageW(g_msg, WM_APP_LOCK, 0, 0);
                return 1;
            } else if (!shell::nativo && g_winDown && down && !g_winCombo && k->vkCode == 'L') {
                // Win+L: a nossa tela de bloqueio (o Win foi engolido pelo gancho, o Windows não vê o atalho);
                // com `bloqueio=0`, bloqueio::Lock() chama o bloqueio do Windows
                g_winCombo = g_winEaten = true;
                PostMessageW(g_msg, WM_APP_LOCK, 0, 0);
                return 1;
            } else if (g_winDown && (k->vkCode == VK_UP || k->vkCode == VK_DOWN) && GetAsyncKeyState(VK_CONTROL) < 0 &&
                       GetAsyncKeyState(VK_MENU) >= 0 && GetAsyncKeyState(VK_SHIFT) >= 0) {
                // Ctrl+Win+↑ / ↓ = Ctrl+↑ / ↓ do Mac: Mission Control / Exposé do app da frente (Ctrl+Win+←/→
                // já trocam de Mesa, como Ctrl+←/→ no Mac). A repetição da tecla segurada não abre de novo
                if (down && g_macKeyVk != k->vkCode) PostMessageW(g_msg, k->vkCode == VK_UP ? WM_APP_MISSION : WM_APP_EXPOSE, 0, 0);
                g_macKeyVk = down ? k->vkCode : 0;
                if (!g_winCombo) g_winCombo = g_winEaten = true;
                return 1;
            } else if (g_macKeyVk && k->vkCode == g_macKeyVk && !down) {
                g_macKeyVk = 0;
                return 1;
            } else if (g_macKeys && g_winDown && down && (!g_winCombo || g_winEaten) && (k->vkCode == 'Q' || k->vkCode == 'H' || k->vkCode == 'W') &&
                       GetAsyncKeyState(VK_CONTROL) >= 0 && GetAsyncKeyState(VK_SHIFT) >= 0 && GetAsyncKeyState(VK_MENU) >= 0) {
                // Win+Q encerra o app da frente, Win+H oculta, Win+W fecha a janela (como Cmd no Mac). Segurando o
                // Win, cada toque vale de novo; a repetição da tecla segurada não conta
                if (g_macKeyVk != k->vkCode) PostMessageW(g_msg, WM_APP_MACKEY, k->vkCode, 0);
                g_macKeyVk = k->vkCode;
                g_winCombo = g_winEaten = true;
                return 1;
            } else if (g_capKey && k->vkCode == g_capKey && !down) {
                g_capKey = 0;  // fim do atalho da Captura de Tela: a tecla solta também é nossa
                return 1;
            } else if (g_winDown && down && GetAsyncKeyState(VK_SHIFT) < 0 && GetAsyncKeyState(VK_MENU) >= 0 &&
                       (k->vkCode == '3' || k->vkCode == '4' || k->vkCode == '5' || k->vkCode == 'S') && g_capturaExe) {
                // Win+Shift+3/4/5 (Cmd+Shift+3/4/5 do Mac) e Win+Shift+S: a nossa Captura de Tela
                // (LiquinixScreenshot.exe) no lugar da Ferramenta de Captura; com Ctrl, só área de transferência.
                // A tecla segurada (repetição) não abre de novo; a tecla muda (0xE8) faz o Windows não ver
                // um Win sozinho ao soltar (o Win e o Shift já foram devolvidos a ele: combinação)
                if (g_capKey != k->vkCode) {
                    g_capKey = k->vkCode;
                    Inject(0xE8, false); Inject(0xE8, true);
                    int kind = k->vkCode == 'S' ? 4 : (int)(k->vkCode - '0');
                    PostMessageW(g_msg, WM_APP_CAPTURA, kind, GetAsyncKeyState(VK_CONTROL) < 0 ? 1 : 0);
                }
                return 1;
            } else if (g_winDown && down && k->vkCode == 'S' && GetAsyncKeyState(VK_SHIFT) < 0 && GetAsyncKeyState(VK_CONTROL) >= 0 && mesa::Standalone()) {
                // Win+Shift+S sem a nossa Captura de Tela: a Ferramenta de Captura do Windows (quem atende é
                // o Explorador quando ele é o shell; sem ele, fazemos nós)
                PostMessageW(g_msg, WM_APP_WINKEY, 'S', 0);
                return 1;
            } else if (g_winDown && down && !g_winCombo && !shell::nativo && k->vkCode == 'E' && GetAsyncKeyState(VK_SHIFT) >= 0 && GetAsyncKeyState(VK_CONTROL) >= 0) {
                // Win+E com o Explorador rodando (explorer=fundo): ele abria a página "Início" dele, que não é pasta de
                // disco e o Finder não trocava (17/11/2026). O Shell atende sempre: Finder (ou o Explorador, se
                // "Abrir pastas no Finder" estiver desligado — mesa::WinKey decide)
                g_winCombo = g_winEaten = true;
                PostMessageW(g_msg, WM_APP_WINKEY, 'E', 0);
                return 1;
            } else if (g_winDown && down && !g_winCombo && !shell::nativo && k->vkCode == 'D') {
                // Win+D é sempre nosso: com o Explorador de fundo, o "mostrar a área de trabalho" dele punha a mesa
                // DELE na frente e a nossa voltava por cima em seguida (piscava, os widgets sumiam e voltavam)
                g_winCombo = g_winEaten = true;
                PostMessageW(g_msg, WM_APP_WINKEY, 'D', 0);
                return 1;
            } else if (g_winDown && down && !g_winCombo && mesa::Standalone() && (k->vkCode == 'E' || k->vkCode == 'R' || k->vkCode == 'D')) {
                // sem o Explorador ninguém trata Win+E/R/D: fazemos nós (Finder, Executar, mostrar a mesa)
                g_winCombo = g_winEaten = true;
                PostMessageW(g_msg, WM_APP_WINKEY, k->vkCode, 0);
                return 1;
            } else if (g_winDown && down && !g_winCombo && shell::nativo) {
                g_winCombo = true;  // nativo: a tecla Windows já foi para o Windows; esta segue também
            } else if (g_winDown && down && !g_winCombo) {
                // combinação: devolve a tecla Windows e esta tecla, na ordem, para o Windows tratar
                g_winCombo = true;
                Inject(g_winVk, false);
                Inject(k->vkCode, false);
                return 1;
            }
        }
    }
    return CallNextHookEx(g_kbHook, code, w, l);
}

static void KeyThread() {
    g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyHook, GetModuleHandleW(nullptr), 0);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {}
    if (g_kbHook) UnhookWindowsHookEx(g_kbHook);
}

void Log(const wchar_t* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fwprintf(g_log, L"%02d:%02d:%02d ", st.wHour, st.wMinute, st.wSecond);
    va_list a; va_start(a, fmt); vfwprintf(g_log, fmt, a); va_end(a);
    fputwc(L'\n', g_log); fflush(g_log);
}

std::wstring shell::Setting(const wchar_t* key, const wchar_t* def) {
    auto it = g_settings.find(key);
    return it != g_settings.end() ? it->second : def;
}

static std::wstring SettingsPath() {
    std::wstring f = plataforma::PastaAjustes();
    CreateDirectoryW(f.c_str(), nullptr);
    return f + L"\\ajustes.txt";
}

void shell::SetSetting(const wchar_t* key, const std::wstring& value) {
    g_settings[key] = value;
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, SettingsPath().c_str(), L"w, ccs=UTF-8") || !fp) return;
    for (auto& [k, v] : g_settings) fwprintf(fp, L"%ls=%ls\n", k.c_str(), v.c_str());
    fclose(fp);
}

// Calculadora (Calculadora.exe) no lugar da Calculadora do Windows: Launchpad, Spotlight e Dock passam
// o alvo (AUMID, atalho ou .exe) por aqui. Sem o Calculadora.exe, segue a do Windows (devolve false).
bool shell::OpenCalculator(const std::wstring& target) {
    std::wstring t = target;
    for (auto& c : t) c = towlower(c);
    auto ends = [&](const wchar_t* s) { size_t n = wcslen(s); return t.size() >= n && t.compare(t.size() - n, n, s) == 0; };
    bool calc = t.find(L"microsoft.windowscalculator") != std::wstring::npos || t == L"calc.exe" || t == L"calc" ||
                ends(L"\\calc.exe") || ends(L"calculatorapp.exe") || ends(L"\\calculadora.exe") || ends(L"\\liquinixcalculator.exe");
    if (!calc) return false;
    std::wstring exe = shell::dir + L"\\" EXE_CALCULATOR;
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return true;
}

// Monitor de Atividade (MonitorAtividade.exe); sem ele, o Gerenciador de Tarefas do Windows
void shell::OpenMonitor() {
    std::wstring exe = shell::dir + L"\\" EXE_MONITOR;
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) exe = L"taskmgr.exe";
    ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// Forçar Encerrar Aplicativos (LiquinixForceQuit.exe); sem ele, o Monitor de Atividade
void shell::OpenForceQuit(bool memory) {
    std::wstring exe = shell::dir + L"\\" EXE_FORCEQUIT;
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) { shell::OpenMonitor(); return; }
    AllowSetForegroundWindow(ASFW_ANY);  // aberta pela pressão de memória, sem clique: precisa vir para a frente
    ShellExecuteW(nullptr, L"open", exe.c_str(), memory ? L"--memoria" : nullptr, nullptr, SW_SHOWNORMAL);
}

void shell::Sound(const wchar_t* name) {
    std::wstring root = shell::dir.substr(0, shell::dir.find_last_of(L'\\'));  // a pasta do projeto (bin fica dentro dela)
    PlaySoundW((root + L"\\sons\\mac\\" + name + L".wav").c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT);
}

void shell::OpenSettings(const std::wstring& page) {
    std::wstring args = page.empty() ? L"" : (page.rfind(L"ms-settings:", 0) == 0 ? page : L"--pagina=" + page);
    ShellExecuteW(nullptr, L"open", (shell::dir + L"\\" EXE_SETTINGS).c_str(), args.c_str(), nullptr, SW_SHOWNORMAL);
}

static void LoadSettings() {
    std::wstring pasta = plataforma::PastaAjustes();
    if (pasta.empty()) return;
    std::wstring f = pasta + L"\\ajustes.txt";
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, f.c_str(), L"r, ccs=UTF-8") || !fp) return;
    wchar_t line[512];
    while (fgetws(line, 512, fp)) {
        std::wstring s = line;
        while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ')) s.pop_back();
        size_t eq = s.find(L'=');
        if (eq != std::wstring::npos && s[0] != L'#') g_settings[s.substr(0, eq)] = s.substr(eq + 1);
    }
    fclose(fp);
    backdrop::Reload();  // Transparência e Efeitos (Liquid Glass, reduzir transparência, tinta)
    anim::reduceMotion = shell::Setting(L"reduzir_movimento", L"0") == L"1";
}

// ---- barra de tarefas do Windows: escondida enquanto o shell roda
static void HideTaskbars() {
    if (shell::nativo) return;  // nativo: a barra do Windows é o próprio Shell (a MacBarra a mantém escondida)
    EnumWindows([](HWND h, LPARAM) -> BOOL {
        wchar_t c[64]; GetClassNameW(h, c, 64);
        if ((!wcscmp(c, L"Shell_TrayWnd") || !wcscmp(c, L"Shell_SecondaryTrayWnd")) && !apps::IsShellWindow(h)) {
            // sem esperar o Explorador: travado ou ainda subindo, ele segurava o Shell aqui (o vigia reiniciava o
            // Shell em ciclo, 09/11/2026)
            if (IsWindowVisible(h)) ShowWindowAsync(h, SW_HIDE);
            if (std::find(g_hiddenBars.begin(), g_hiddenBars.end(), h) == g_hiddenBars.end()) g_hiddenBars.push_back(h);
        }
        return TRUE;
    }, 0);
}

static void ShowTaskbars() {
    for (HWND h : g_hiddenBars) if (IsWindow(h)) ShowWindowAsync(h, SW_SHOWNOACTIVATE);
    g_hiddenBars.clear();
}

// Barra do Windows nos outros monitores: só esconder não segurava (o Explorador a mostrava de novo e ela
// reservava espaço). Enquanto o Shell roda, desliga "Mostrar a barra de tarefas em todos os monitores"
// (HKCU ...\Explorer\Advanced\MMTaskbarEnabled = 0, a mesma chave da Configuração do Windows). O valor de antes
// fica em ajustes.txt (barra_windows_monitores_antes: 0, 1 ou "x" = não existia) e volta ao sair.
static const wchar_t* MM_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";

static void TellExplorerTray() {  // o Explorador relê o ajuste da barra (como a Configuração faz)
    std::thread([] {
        EnumWindows([](HWND h, LPARAM) -> BOOL {
            wchar_t c[64] = {}; GetClassNameW(h, c, 64);
            if (!wcscmp(c, L"Shell_TrayWnd") && !apps::IsShellWindow(h))
                SendMessageTimeoutW(h, WM_SETTINGCHANGE, 0, (LPARAM)L"TraySettings", SMTO_ABORTIFHUNG, 1000, nullptr);
            return TRUE;
        }, 0);
    }).detach();
}

static void SecondaryTaskbarsOff() {
    if (shell::nativo) return;
    DWORD v = 0, cb = sizeof(v);
    bool had = RegGetValueW(HKEY_CURRENT_USER, MM_KEY, L"MMTaskbarEnabled", RRF_RT_REG_DWORD, nullptr, &v, &cb) == ERROR_SUCCESS;
    if (had && v == 0) return;  // já desligado (pelo usuário ou por nós antes)
    if (shell::Setting(L"barra_windows_monitores_antes", L"").empty())
        shell::SetSetting(L"barra_windows_monitores_antes", had ? std::to_wstring(v) : L"x");
    DWORD zero = 0;
    if (RegSetKeyValueW(HKEY_CURRENT_USER, MM_KEY, L"MMTaskbarEnabled", REG_DWORD, &zero, sizeof(zero)) == ERROR_SUCCESS) {
        Log(L"registro: HKCU\\%ls\\MMTaskbarEnabled = 0 (antes: %ls) - barra do Windows só no monitor principal", MM_KEY, had ? std::to_wstring(v).c_str() : L"não existia");
        TellExplorerTray();
    }
}

static void SecondaryTaskbarsRestore() {
    std::wstring before = shell::Setting(L"barra_windows_monitores_antes", L"");
    if (before.empty()) return;
    if (before == L"x") RegDeleteKeyValueW(HKEY_CURRENT_USER, MM_KEY, L"MMTaskbarEnabled");
    else { DWORD v = (DWORD)_wtoi(before.c_str()); RegSetKeyValueW(HKEY_CURRENT_USER, MM_KEY, L"MMTaskbarEnabled", REG_DWORD, &v, sizeof(v)); }
    shell::SetSetting(L"barra_windows_monitores_antes", L"");
    Log(L"registro: MMTaskbarEnabled devolvido (%ls)", before == L"x" ? L"apagado, não existia" : before.c_str());
    TellExplorerTray();
}

// Área de trabalho de cada monitor: a barra de menus em cima (todos têm a sua) e o Dock embaixo (só no
// monitor dele). O SPI_SETWORKAREA vale para o monitor que contém o retângulo.
RECT shell::WorkAreaFor(HMONITOR hm) {
    MONITORINFO mi{ sizeof(mi) };
    if (!GetMonitorInfoW(hm, &mi)) return {};
    RECT want = mi.rcMonitor;
    want.top += (mi.dwFlags & MONITORINFOF_PRIMARY) ? menubar::Height() : menubar::HeightOn(hm);
    if (hm == dock::MonitorHandle()) {  // o Dock na borda escolhida (Ajustes > Posição na tela)
        int r = dock::ReservedHeight();
        if (dock::Posicao() == 1) want.left += r;
        else if (dock::Posicao() == 2) want.right -= r;
        else want.bottom -= r;
    }
    return want;
}

void shell::ApplyWorkArea() {
    if (g_quitting) return;
    struct Ctx { HMONITOR dock; bool changed; } ctx{ dock::MonitorHandle(), false };
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hm, HDC, LPRECT, LPARAM p) -> BOOL {
        auto* c = (Ctx*)p;
        MONITORINFO mi{ sizeof(mi) };
        if (!GetMonitorInfoW(hm, &mi)) return TRUE;
        RECT want = shell::WorkAreaFor(hm), cur = mi.rcWork;
        (void)c;
        if (EqualRect(&want, &cur)) return TRUE;
        static int times;
        Log(L"área de trabalho estava %d,%d-%d,%d; agora %d,%d-%d,%d (vez %d)", cur.left, cur.top, cur.right, cur.bottom,
            want.left, want.top, want.right, want.bottom, ++times);
        // Sem SPIF_SENDCHANGE: o aviso geral fazia o Explorador recalcular e desfazer (briga a cada 2 s).
        SystemParametersInfoW(SPI_SETWORKAREA, 0, &want, 0);
        c->changed = true;
        return TRUE;
    }, (LPARAM)&ctx);
    if (!ctx.changed) return;
    // Avisa só as janelas dos apps: o DefWindowProc reencaixa as maximizadas na área nova.
    EnumWindows([](HWND h, LPARAM) -> BOOL {
        if (!IsWindowVisible(h) || apps::IsShellWindow(h)) return TRUE;
        wchar_t c[64]; GetClassNameW(h, c, 64);
        if (!wcscmp(c, L"Shell_TrayWnd") || !wcscmp(c, L"Shell_SecondaryTrayWnd") || mesa::IsDeskClass(c)) return TRUE;
        if (IsZoomed(h)) SendMessageTimeoutW(h, WM_SETTINGCHANGE, SPI_SETWORKAREA, 0, SMTO_ABORTIFHUNG, 200, nullptr);
        return TRUE;
    }, 0);
}

static void Restore() {
    if (g_quitting) return;
    g_quitting = true;
    tray::Stop();
    menus::Stop();
    dialogs::Stop();
    janelas::Stop();
    encaixe::Stop();
    memoria::Stop();
    barras::Stop();
    sombras::Stop();
    alertas::Stop();
    progresso::Stop();  // as janelas de progresso do Windows voltam para a tela
    if (!shell::nativo) {  // no nativo a tela de bloqueio continua do Windows (não foi trocada)
        bloqueio::Stop();  // Win+L volta para o Windows (política apagada)
        mesa::Stop(!g_crashed && !g_sessionEnding);  // sem Explorador: ele volta (ao cair, quem abre é o vigia)
    } else mesa::Stop(true);  // nativo: a nossa mesa some e a do Explorador (que ficou atrás) reaparece
    revelar::Stop();  // janelas que estavam nas bordas voltam
    ponteiro::Stop();  // o cursor volta ao tamanho do esquema
    palco::Stop();    // Organizador Visual: as janelas escondidas por ele voltam
    genio::Stop();    // a animação de minimizar do Windows volta como estava
    notificacoes::Stop();  // o banner de notificação do Windows volta
    ShowTaskbars();
    SecondaryTaskbarsRestore();
    // os outros monitores voltam inteiros (a barra do Windows, se voltar lá, reserva o espaço dela)
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hm, HDC, LPRECT, LPARAM) -> BOOL {
        MONITORINFO mi{ sizeof(mi) };
        if (GetMonitorInfoW(hm, &mi) && !(mi.dwFlags & MONITORINFOF_PRIMARY)) SystemParametersInfoW(SPI_SETWORKAREA, 0, &mi.rcMonitor, 0);
        return TRUE;
    }, 0);
    SystemParametersInfoW(SPI_SETWORKAREA, 0, &g_origWork, SPIF_SENDCHANGE);
    Log(L"saindo: barra do Windows e área de trabalho devolvidas");
}

void shell::Quit(const wchar_t* why) {
    Log(L"pedido para sair: %ls", why);
    pastas::Stop();
    limites::Stop();
    bloqueio::Fallback();  // saiu com a tela travada (ex.: Ctrl+Alt+Shift+F12): o Windows bloqueia
    Restore();
    PostQuitMessage(0);
}

static LONG WINAPI OnCrash(EXCEPTION_POINTERS* ep) {
    HMODULE mod = nullptr;
    void* at = ep->ExceptionRecord->ExceptionAddress;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)at, &mod);
    wchar_t name[MAX_PATH] = L"?"; if (mod) GetModuleFileNameW(mod, name, MAX_PATH);
    Log(L"ERRO 0x%08X em %ls+0x%llX", ep->ExceptionRecord->ExceptionCode, name, (unsigned long long)((BYTE*)at - (BYTE*)mod));
    bloqueio::Fallback();  // caiu com a tela travada: o Windows bloqueia de verdade
    g_crashed = true;
    Restore();  // nunca deixar o usuário sem barra de tarefas
    return EXCEPTION_CONTINUE_SEARCH;
}

// Finder reserva: um Arquivos.exe já inicializado e escondido. Abrir uma pasta, o Win+E ou a janela
// de Abrir/Salvar entregam o pedido a ele (aparece na hora); quando é usado, preparamos outro.
// Captura de Tela (LiquinixScreenshot.exe) sempre aberta e escondida: a mira do Win+Shift+4 aparece na hora
static void EnsureScreenshot() {
    static DWORD lastSpawn;
    if (!g_capturaExe || FindWindowW(L"SequoiaCapturaMsg", nullptr)) return;
    if (lastSpawn && GetTickCount() - lastSpawn < 10000) return;
    lastSpawn = GetTickCount();
    std::wstring cmd = L"\"" + shell::dir + L"\\" EXE_SCREENSHOT L"\" /residente";
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        Log(L"Captura de Tela residente aberta");
    }
}

// pedido do atalho: 3 tela inteira, 4 região/janela, 5 barra de ferramentas; clip = Ctrl seguro
static void Screenshot(int kind, bool clip) {
    static UINT msg = RegisterWindowMessageW(L"SequoiaCaptura");
    if (HWND w = FindWindowW(L"SequoiaCapturaMsg", nullptr)) {
        DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
        AllowSetForegroundWindow(pid);
        PostMessageW(w, msg, kind, clip ? 1 : 0);
        return;
    }
    std::wstring args = L"/" + std::to_wstring(kind) + (clip ? L" /c" : L"");
    ShellExecuteW(nullptr, L"open", (shell::dir + L"\\" EXE_SCREENSHOT).c_str(), args.c_str(), nullptr, SW_SHOWNORMAL);
    Log(L"Captura de Tela aberta agora (%ls)", args.c_str());
}

void shell::AbrirFinder(const std::wstring& args) {
    if (shell::nativo) {
        HMODULE m = GetModuleHandleW(L"MacFinder.dll");
        auto pedir = m ? (BOOL(WINAPI*)(const wchar_t*))GetProcAddress(m, "FinderPedir") : nullptr;
        if (pedir && pedir(args.c_str())) { Log(L"Finder (nativo): %ls", args.c_str()); return; }
        Log(L"Finder nativo indisponível, abrindo o exe: %ls", args.c_str());
    }
    ShellExecuteW(nullptr, L"open", (shell::dir + L"\\" EXE_FINDER).c_str(), args.c_str(), nullptr, SW_SHOWNORMAL);
}

static void EnsureReserve() {
    if (shell::nativo) return;  // no nativo a reserva é a janela do Finder dentro do explorer (MacFinder.dll)
    static DWORD lastSpawn;
    if (FindWindowW(L"ArquivosJanela", L"​ArquivosReserva")) return;
    if (lastSpawn && GetTickCount() - lastSpawn < 4000) return;  // um por vez
    lastSpawn = GetTickCount();
    std::wstring cmd = L"\"" + shell::dir + L"\\" EXE_FINDER L"\" --reserva";
    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        Log(L"Finder reserva preparado");
    }
}

// Pré-carga: lê os .exe dos outros apps do projeto para o cache de arquivos do Windows (memória de
// espera, que o sistema devolve quando precisa). Assim o primeiro clique abre como se já tivesse sido
// aberto antes, sem deixar nenhum processo rodando. Prioridade de fundo, depois que o Shell está pronto.
// os painéis que montam coisa cara na primeira abertura: prepara com o Shell já parado, na thread
// principal (o compositor e a GPU são dela)
static void CALLBACK WarmPanels(HWND, UINT, UINT_PTR id, DWORD) {
    KillTimer(nullptr, id);
    missioncontrol::Warm();
    launchpad::Warm();
}

static void PreloadApps() {
    SetTimer(nullptr, 0, 4000, WarmPanels);
    std::thread([] {
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        Sleep(3000);
        static const wchar_t* files[] = { EXE_SETTINGS, EXE_NOTES, EXE_CALCULATOR, EXE_TERMINAL, EXE_MONITOR, DLL_WINDOW32 };
        std::vector<char> buf(1 << 20);
        size_t total = 0;
        for (auto* f : files) {
            HANDLE h = CreateFileW((shell::dir + L"\\" + f).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                                   OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (h == INVALID_HANDLE_VALUE) continue;
            DWORD got = 0;
            while (ReadFile(h, buf.data(), (DWORD)buf.size(), &got, nullptr) && got) total += got;
            CloseHandle(h);
        }
        Log(L"pré-carga: %zu KB dos apps no cache", total / 1024);
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
    }).detach();
}

// App da frente cobrindo o monitor inteiro (vídeo, jogo, F11): Dock e barra de menus somem, como no Mac.
static bool IsFullscreen(HWND fg) {
    if (!fg || !IsWindowVisible(fg) || IsIconic(fg) || apps::IsShellWindow(fg)) return false;
    wchar_t cls[64] = {}; GetClassNameW(fg, cls, 64);
    if (mesa::IsDeskClass(cls) || !wcscmp(cls, L"Shell_TrayWnd")) return false;
    if (IsZoomed(fg) && (GetWindowLongW(fg, GWL_STYLE) & WS_CAPTION) == WS_CAPTION) return false;  // só maximizada
    MONITORINFO mi{ sizeof(mi) };
    if (!GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    RECT r; if (!GetWindowRect(fg, &r)) return false;
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
}

// Jogo/vídeo em tela cheia no monitor `mon` (ou o Windows em modo "não incomodar": Direct3D exclusivo,
// apresentação): os banners de notificação não aparecem, só vão para a Central. Uma janela "sempre no topo" por cima
// do jogo tirava ele da tela cheia (17/11/2026, Fortnite caindo a cada notificação).
bool shell::NaoIncomodar(HMONITOR mon) {
    QUERY_USER_NOTIFICATION_STATE q{};
    if (SUCCEEDED(SHQueryUserNotificationState(&q)) && (q == QUNS_RUNNING_D3D_FULL_SCREEN || q == QUNS_PRESENTATION_MODE || q == QUNS_BUSY))
        return true;
    HWND fg = GetForegroundWindow();
    if (!IsFullscreen(fg)) return false;
    return !mon || MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST) == mon;
}

// Com mais de um monitor, só some o que está no monitor do app em tela cheia (a barra daquele monitor; o Dock
// se ele estiver lá). Tela travada: tudo some.
static void CheckFullscreen() {
    HWND fg = GetForegroundWindow();
    bool locked = bloqueio::Active();
    bool fs = !locked && IsFullscreen(fg);
    HMONITOR fm = fs ? MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST) : nullptr;
    {   // registro: quem fez o Dock e a barra de menus sumirem (uma vez por janela)
        static HWND visto;
        if (fs && fg != visto) {
            wchar_t c[80] = {}, t[80] = {}; GetClassNameW(fg, c, 80); GetWindowTextW(fg, t, 80);
            DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
            Log(L"tela cheia: janela %p classe '%ls' título '%ls' pid %lu estilo %08lX ex %08lX", fg, c, t, pid, GetWindowLongW(fg, GWL_STYLE), GetWindowLongW(fg, GWL_EXSTYLE));
        }
        visto = fs ? fg : nullptr;
    }
    dock::SetFullscreen(locked || (fs && fm == dock::MonitorHandle()));
    menubar::SetFullscreen(locked || fs, fm);
    ilha::SetFullscreen(locked);  // a ilha confere sozinha a tela cheia do monitor principal (jogo, vídeo, app)
}

static void CALLBACK OnWinEvent(HWINEVENTHOOK, DWORD ev, HWND h, LONG obj, LONG, DWORD, DWORD) {
    if (ev == EVENT_OBJECT_LOCATIONCHANGE && (obj != OBJID_WINDOW || h != GetForegroundWindow())) return;
    if (ev == EVENT_SYSTEM_FOREGROUND) { revelar::Foreground(h); jogo::Foreground(h); }
    CheckFullscreen();
}

static void Changed() {
    if (apps::Refresh()) dock::Update();
    menubar::Update();
}

// Aparência "Automático" (Ajustes do Sistema): claro das 7h às 19h, escuro no resto do dia.
static void AutoAppearance() {
    if (shell::Setting(L"aparencia_auto", L"0") != L"1") return;
    SYSTEMTIME st; GetLocalTime(&st);
    DWORD want = st.wHour >= 7 && st.wHour < 19 ? 1 : 0, cur = 1, cb = sizeof(cur);
    const wchar_t* key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize";
    RegGetValueW(HKEY_CURRENT_USER, key, L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &cur, &cb);
    if (cur == want) return;
    RegSetKeyValueW(HKEY_CURRENT_USER, key, L"AppsUseLightTheme", REG_DWORD, &want, sizeof(want));
    RegSetKeyValueW(HKEY_CURRENT_USER, key, L"SystemUsesLightTheme", REG_DWORD, &want, sizeof(want));
    Log(L"aparência automática: %ls", want ? L"clara" : L"escura");
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"ImmersiveColorSet", SMTO_ABORTIFHUNG, 200, nullptr);
}

static LRESULT CALLBACK MsgProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    // Outro programa (o Explorador, com a barra do Windows escondida ou em ocultar automaticamente) devolveu a área de
    // trabalho para a tela inteira: reaplica a nossa NA HORA. Antes esperava o tique de 1 s, e nesse meio-tempo as
    // janelas maximizadas e os ícones da mesa subiam para baixo da barra de menus e voltavam (issue #1: "a área
    // pula sem parar"). O nosso próprio SPI_SETWORKAREA vai sem aviso (sem SPIF_SENDCHANGE), então não volta aqui.
    if (m == WM_SETTINGCHANGE && w == SPI_SETWORKAREA && !g_quitting) {
        static DWORD win = 0; static int n = 0;
        DWORD now = GetTickCount();
        if (now - win > 1000) { win = now; n = 0; }
        if (++n <= 8) shell::ApplyWorkArea();  // no máximo 8 por segundo (se alguém brigar sem parar, não gira em laço)
    }
    static UINT reload = RegisterWindowMessageW(L"ShellAjustesMudaram");
    static UINT openCC = RegisterWindowMessageW(L"ShellAbrirCentral");  // abre/fecha a Central de Controle de fora
    static UINT restart = RegisterWindowMessageW(L"ShellReiniciar");    // reiniciar: sai sem abrir o Explorador e abre o Shell de novo
    if (m == restart && restart) {
        // o Shell novo abre já (no mesmo caminho: depois de compilar, é a versão nova) e espera este sair
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir = exe; dir = dir.substr(0, dir.find_last_of(L'\\'));
        std::wstring cmd = std::wstring(L"\"") + exe + L"\" --depois-de " + std::to_wstring(GetCurrentProcessId());
        STARTUPINFOW si{ sizeof(si) }; PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
        else Log(L"reiniciar: não abriu o Shell novo (%lu)", GetLastError());
        mesa::NoExplorerOnExit();
        tray::SemAvisoAoSair();
        shell::Quit(L"reiniciar");
        return 0;
    }
    if (m == openCC && openCC) { central::Toggle(); return 0; }
    static UINT openNC = RegisterWindowMessageW(L"ShellAbrirNotificacoes");  // abre/fecha a Central de Notificações de fora
    if (m == openNC && openNC) { notificacoes::Toggle(); return 0; }
    static UINT openSP = RegisterWindowMessageW(L"ShellAbrirSpotlight");  // abre/fecha o Spotlight de fora (como o Win+Espaço)
    if (m == openSP && openSP) { PostMessageW(h, WM_APP_SPOTLIGHT, 0, 0); return 0; }
    // abre um painel de fora (fotos do README, testes): 1 Launchpad/Apps, 2 Mission Control, 3..6 Spotlight no modo 1..4,
    // 7 Spotlight, 8 Sobre Este Mac
    static UINT openPanel = RegisterWindowMessageW(L"ShellAbrirPainel");
    if (m == openPanel && openPanel) {
        // pedido de fora: o Windows não deixa o Shell vir para a frente sozinho (o Spotlight e o Launchpad fechavam na
        // hora, sem foco). Entra na fila de entrada da janela da frente enquanto abre, como os alertas fazem.
        HWND fg = GetForegroundWindow();
        DWORD ft = fg ? GetWindowThreadProcessId(fg, nullptr) : 0, me = GetCurrentThreadId();
        bool att = ft && ft != me && AttachThreadInput(me, ft, TRUE);
        if (w == 1) launchpad::Toggle();
        else if (w == 2) missioncontrol::Toggle();
        else if (w >= 3 && w <= 6) spotlight::OpenMode((int)w - 2);
        else if (w == 7) spotlight::Toggle();
        else if (w == 8) sobre::Show();  // Sobre Este Mac
        else if (w == 9) central::Toggle();        // Central de Controle (testes)
        else if (w == 10) notificacoes::Toggle();  // Central de Notificações (testes)
        else if (w == 12) dock::OpenAppsStack();   // Leopard: a pilha Aplicativos (testes)
        else if (w == 13) seletor::Step(1);        // o seletor de apps sem segurar o Alt (testes; 14 fecha)
        else if (w == 14) seletor::Key(VK_ESCAPE);
        else if (w == 15) dock::OpenDownloadsStack();  // a pilha Downloads (testes)
        else if (w == 16) dashboard::Toggle();         // Leopard: Dashboard (Win+F12)
        else if (w == 17) espacos::Toggle();           // Leopard: Spaces (Win+F8)
        else if (w == 18) tempo::Enter();              // Leopard: a viagem no tempo do Time Machine
        else if (w == 11) {  // Quick Look de uma foto do macOS 27 (testes)
            wchar_t p[MAX_PATH] = {}; GetEnvironmentVariableW(L"USERPROFILE", p, MAX_PATH);
            quicklook::Toggle(std::wstring(p) + L"\\Pictures\\Wallpapers macOS\\macOS 27\\macos-27-golden-4096x2160-27635.jpg");
        }
        if (att) AttachThreadInput(me, ft, FALSE);
        return 0;
    }
    if (atualizacao::Handle(m)) return 0;
    // Ajustes > Aparência > "Reiniciar o Liquid Glass": refaz o vidro real do zero (se ele desistiu ou travou)
    static UINT redoGlass = RegisterWindowMessageW(L"ShellReiniciarVidro");
    if (m == redoGlass && redoGlass) { backdrop::Restart(); return 1; }
    static UINT capVis = RegisterWindowMessageW(L"ShellCapturaVisivel");  // a Captura de Tela vai tirar a foto
    if (m == capVis && capVis) { backdrop::CaptureVisible(w != 0); return 1; }
    if (m == WM_COPYDATA && l && ((COPYDATASTRUCT*)l)->dwData == 0x4C504150 /*'PAPL'*/) {  // Ajustes: papel de parede escolhido
        auto* cd = (COPYDATASTRUCT*)l;
        if (!cd->lpData || cd->cbData < sizeof(wchar_t) || cd->cbData > 4096) return FALSE;
        std::wstring p((const wchar_t*)cd->lpData, cd->cbData / sizeof(wchar_t));
        while (!p.empty() && !p.back()) p.pop_back();
        mesa::WallpaperNow(p);
        return TRUE;
    }
    if (m == WM_COPYDATA && l && ((COPYDATASTRUCT*)l)->dwData == 0x514C4F4B /*'QLOK'*/) {  // Finder: Quick Look (vazio = fechar)
        auto* cd = (COPYDATASTRUCT*)l;
        if (cd->cbData > 8192) return FALSE;
        std::wstring p = cd->lpData ? std::wstring((const wchar_t*)cd->lpData, cd->cbData / sizeof(wchar_t)) : L"";
        while (!p.empty() && !p.back()) p.pop_back();
        // responde já e abre logo depois: o vídeo leva mais que o tempo de espera do Finder (0,8 s), e ele achava
        // que tinha falhado e abria também o painel antigo dele
        auto* copy = new std::wstring(p);
        PostMessageW(h, WM_APP + 77, 0, (LPARAM)copy);
        return TRUE;
    }
    if (m == WM_APP + 77 && l) {  // o Quick Look pedido pelo Finder (acima)
        std::unique_ptr<std::wstring> p((std::wstring*)l);
        if (p->empty()) quicklook::Close();
        else quicklook::Toggle(*p, false);  // o Finder fica com o foco
        return 0;
    }
    if (m == WM_COPYDATA && l && ((COPYDATASTRUCT*)l)->dwData == 0x414D504C /*'AMPL'*/) {  // MacJanela.dll: ampliar animado
        struct Msg { unsigned long long hwnd; RECT from, to; int flags; COLORREF cap; };
        auto* cd = (COPYDATASTRUCT*)l;
        if (cd->cbData != sizeof(Msg) || !cd->lpData) return FALSE;
        auto* mm = (Msg*)cd->lpData;
        // nativo: janelas de barra própria (flags 8: Chrome, Edge, Electron...) têm a barra do Mac por fora (barras.cpp).
        // A animação usa uma foto da janela de verdade, com a barra e os botões do Windows: por meio segundo as
        // bolinhas sumiam e apareciam os botões antigos (vídeo do usuário de 12/11/2026). Sem animação: muda na hora.
        if (shell::nativo && (mm->flags & 8)) return FALSE;
        return ampliar::Start((HWND)(UINT_PTR)mm->hwnd, mm->from, mm->to, mm->flags, mm->cap) ? TRUE : FALSE;
    }
    static UINT ampFoto = RegisterWindowMessageW(L"ShellAmpliarFoto");  // MacJanela.dll: a janela já mudou de tamanho
    if (m == ampFoto && ampFoto) { ampliar::SecondPhoto((HWND)w); return 0; }
    static UINT ampPronto = RegisterWindowMessageW(L"ShellAmpliarPronto");  // a janela já reapareceu
    if (m == ampPronto && ampPronto) { ampliar::Shown((HWND)w); return 0; }
    static UINT menuVerde = RegisterWindowMessageW(L"ShellMenuVerde");  // ponteiro parado no botão verde (DLL ou app nosso)
    if (m == menuVerde && menuVerde) { verde::Show((HWND)w, POINT{ (short)LOWORD(l), (short)HIWORD(l) }); return 0; }
    static UINT jAbrir = RegisterWindowMessageW(L"ShellJanelaAbrir");    // LiquinixWindow.dll: janela nova, escondida e pintada
    if (m == jAbrir && jAbrir) { abrir::Open((HWND)w, (HWND)l); return 0; }
    static UINT jFechar = RegisterWindowMessageW(L"ShellJanelaFechar");  // LiquinixWindow.dll: a janela vai sumir agora
    if (m == jFechar && jFechar) return ampliar::Fechar((HWND)w, l) ? 1 : 0;
    if (m == WM_COPYDATA && l) {  // o Ajustes do Sistema manda "chave=valor": o Shell grava no arquivo que ele mesmo lê
        // (o Ajustes pode estar num contêiner que desvia o %APPDATA% dele para outra cópia do ajustes.txt)
        auto* cd = (COPYDATASTRUCT*)l;
        if (cd->dwData == 0x534A5354 /*'SJST'*/ && cd->lpData && cd->cbData >= sizeof(wchar_t)) {
            std::wstring kv((const wchar_t*)cd->lpData, cd->cbData / sizeof(wchar_t));
            while (!kv.empty() && !kv.back()) kv.pop_back();
            size_t eq = kv.find(L'=');
            if (eq != std::wstring::npos && eq > 0 && eq < 128 && kv.find(L'\n') == std::wstring::npos) {
                shell::SetSetting(kv.substr(0, eq).c_str(), kv.substr(eq + 1));
                PostMessageW(h, reload, 0, 0);
            }
            return TRUE;
        }
    }
    if (m == reload && reload) {  // o Ajustes do Sistema gravou ajustes.txt
        g_macKeys = shell::Setting(L"atalhos_mac", L"1") == L"1";
        static int lastLiquid = -1;
        if (lastLiquid < 0) lastLiquid = liquid::Leopard() ? 3 : liquid::Golden() ? 2 : liquid::On();
        static int lastTheme = -1;  // 0 Sequoia, 1 Tahoe, 2 Golden Gate, 3 Leopard
        auto themeNow = [] { return liquid::Leopard() ? 3 : liquid::Golden() ? 2 : (int)liquid::On(); };
        if (lastTheme < 0) lastTheme = themeNow();
        LoadSettings();
        ilha::Reload();
        static int lastLang = -1;
        if (lastLang < 0) lastLang = idioma::Current();
        if (int lang = idioma::FromCode(shell::Setting(L"idioma", L"en")); lang != lastLang) {  // Ajustes > Geral > Idioma
            lastLang = lang;
            idioma::Set(lang);
            apps::Relabel();
            dock::IconsChanged();
            Log(L"idioma: %ls", idioma::Code());
        }
        if ((liquid::Leopard() ? 3 : liquid::Golden() ? 2 : (int)liquid::On()) != lastLiquid) {  // tema Sequoia/Tahoe/Golden Gate/Leopard: outro pacote de ícones
            lastLiquid = liquid::Leopard() ? 3 : liquid::Golden() ? 2 : liquid::On();
            for (auto& a : apps::list) { a.icon.clear(); a.iw = a.ih = 0; a.mac = false; }
            dock::IconsChanged();
            seletor::IconsChanged();
            Log(L"tema mudou: ícones do Dock e do seletor recarregados");
        }
        if (themeNow() != lastTheme) {
            // tema trocado: as janelas com a barra do Mac (LiquinixWindow.dll) redesenham já (barra metálica e botões de gel
            // do Leopard, ou de volta) em vez de esperar a próxima ativação
            lastTheme = themeNow();
            EnumWindows([](HWND w, LPARAM msg) -> BOOL {
                if (GetPropW(w, L"MacJanela")) PostMessageW(w, (UINT)msg, 0, 0);
                return TRUE;
            }, (LPARAM)reload);
            Log(L"tema: %d (janelas avisadas)", lastTheme);
            ponteiro::TemaLeopard(lastTheme == 3);  // Leopard: cursor do Mac OS X; saindo, o cursor de antes
        }
        dock::ReloadSettings();
        dock::ApplyTheme();
        menubar::ApplyTheme();
        menubar::Update();
        shell::ApplyWorkArea();
        mesa::SettingsChanged();
        widgets::SettingsChanged();  // Personalizar a Mesa: widgets ligados/desligados
        jogo::Reload();  // modo_jogo e jogos.txt
        palco::Sync();
        return 0;
    }
    if (m == WM_SHELLHOOK && WM_SHELLHOOK) {
        switch (w & 0x7FFF) {
        case HSHELL_WINDOWCREATED: case HSHELL_WINDOWDESTROYED: case HSHELL_WINDOWACTIVATED: case HSHELL_REDRAW:
            Changed();
            if ((w & 0x7FFF) == HSHELL_REDRAW && (w & 0x8000)) {  // HSHELL_FLASH: app pedindo atenção
                apps::Refresh();
                if (App* a = apps::OfWindow((HWND)l)) { a->attentionAt = GetTickCount(); dock::Update(); }
            }
            break;
        case HSHELL_GETMINRECT: {  // para onde a janela encolhe ao minimizar: o Dock, antes da Lixeira
            auto* shi = (SHELLHOOKINFO*)l;
            RECT r{};
            if (shi && genio::OnMinRect(shi->hwnd, &r)) { shi->rc = r; return TRUE; }  // o gênio já começou, com a janela ainda na tela
            r = dock::MinimizeTarget();
            if (shi && r.right > r.left) { shi->rc = r; SetTimer(g_msg, 7, 450, nullptr); return TRUE; }  // e o Dock se atualiza logo depois
            break;
        }
        }
        return 0;
    }
    switch (m) {
    case WM_TIMER:
        if (w == 7) { KillTimer(g_msg, 7); Changed(); return 0; }
        if (w == 1) {
            static int ticks;
            if (ticks++ % 60 == 0) AutoAppearance();
            Changed(); CheckFullscreen(); HideTaskbars(); shell::ApplyWorkArea(); if (shell::Setting(L"finder_reserva", L"1") == L"1") EnsureReserve(); EnsureScreenshot();
        }
        if (w == 2) tray::Keepalive();
        if (w == 9) {  // extensões do menu de contexto carregadas cedo: o 1º botão direito na mesa/Spotlight/Launchpad sai rápido
            KillTimer(g_msg, 9);
            wchar_t* p = nullptr; std::wstring desk;
            if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &p))) { desk = p; CoTaskMemFree(p); }
            std::wstring file;
            WIN32_FIND_DATAW fd; HANDLE fh = desk.empty() ? INVALID_HANDLE_VALUE : FindFirstFileW((desk + L"\\*").c_str(), &fd);
            if (fh != INVALID_HANDLE_VALUE) {
                do if (!(fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN))) { file = desk + L"\\" + fd.cFileName; break; }
                while (FindNextFileW(fh, &fd));
                FindClose(fh);
            }
            if (!desk.empty()) ctxmenu::WarmupAsync(desk, file);
        }
        return 0;
    case WM_HOTKEY: shell::Quit(L"atalho Ctrl+Alt+Shift+F12"); return 0;
    case WM_APP_SETTINGS: shell::OpenSettings(L""); return 0;
    case WM_APP_SNAP: {
        HWND fg = GetForegroundWindow();
        if (!fg) return 0;
        if (w == VK_LEFT || w == VK_RIGHT) {
            HWND root = GetAncestor(fg, GA_ROOT);
            if (root && !apps::IsShellWindow(root) && (GetWindowLongW(root, GWL_STYLE) & WS_THICKFRAME)) encaixe::Half(root, w == VK_RIGHT);
            return 0;
        }
        // o "ampliar" do Mac (MacJanela.dll): ocupa a área de trabalho e mantém cantos e sombra
        static UINT zoom = RegisterWindowMessageW(L"MacJanelaAmpliar");
        bool ampliada = GetPropW(fg, L"MacJanelaAmpliada") != nullptr;
        if (w == VK_UP) { if (!ampliada) PostMessageW(fg, zoom, 0, 0); }
        else if (ampliada) PostMessageW(fg, zoom, 0, 0);
        else ShowWindowAsync(fg, SW_MINIMIZE);
        return 0;
    }
    case WM_APP_MISSION:
        if (launchpad::IsOpen()) launchpad::Toggle();
        keybd_event(VK_MENU, 0, 0, KEY_MARK);  // permite ao Mission Control vir para a frente
        keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, KEY_MARK);
        missioncontrol::Toggle();
        return 0;
    case WM_APP_EXPOSE: {
        HWND fg = GetForegroundWindow();
        HWND root = fg ? GetAncestor(fg, GA_ROOTOWNER) : nullptr;
        apps::Refresh();
        App* a = root && !apps::IsShellWindow(root) ? apps::OfWindow(root) : nullptr;
        if (!a) { Log(L"Exposé do app: a janela da frente não é de um app do Dock"); return 0; }
        if (launchpad::IsOpen()) launchpad::Toggle();
        keybd_event(VK_MENU, 0, 0, KEY_MARK);  // permite ao Exposé vir para a frente
        keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, KEY_MARK);
        missioncontrol::ToggleApp(a->id, a->name);
        return 0;
    }
    case WM_APP_MACKEY: {
        HWND fg = GetForegroundWindow();
        HWND root = fg ? GetAncestor(fg, GA_ROOT) : nullptr;
        if (!root || apps::IsShellWindow(root) || apps::IsShellWindow(GetAncestor(root, GA_ROOTOWNER))) return 0;  // Dock, mesa, painéis
        if (w == 'W') {  // fecha só a janela da frente (o app continua aberto)
            Log(L"atalho Win+W: fechando a janela %p", root);
            PostMessageW(root, WM_SYSCOMMAND, SC_CLOSE, 0);
            return 0;
        }
        apps::Refresh();
        App* a = apps::OfWindow(GetAncestor(root, GA_ROOTOWNER));
        if (!a) a = apps::OfWindow(root);
        if (!a) { Log(L"atalho Win+%lc: janela da frente sem app no Dock", (wchar_t)w); return 0; }
        if (w == 'Q') {
            if (a->finder) return 0;  // como no Mac: o Finder não se encerra
            Log(L"atalho Win+Q: encerrando %ls", a->name.c_str());
            apps::Quit(*a);
        } else {
            Log(L"atalho Win+H: ocultando %ls", a->name.c_str());
            apps::Hide(*a);
        }
        dock::Update();
        return 0;
    }
    case WM_APP_LOCK:
        keybd_event(VK_MENU, 0, 0, KEY_MARK);  // permite à tela de bloqueio vir para a frente e receber o teclado
        keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, KEY_MARK);
        bloqueio::Lock();
        return 0;
    case WM_APP_MONITOR:
        if (w == 1) shell::OpenForceQuit(false); else shell::OpenMonitor();
        return 0;
    case WM_APP_SWITCH: seletor::Step((int)(INT_PTR)w); return 0;
    case WM_APP_SWITCH_KEY: seletor::Key((UINT)w); return 0;
    case WM_APP_SWITCH_END: seletor::Release(); return 0;
    case WM_APP_SPOTLIGHT:
        if (launchpad::IsOpen()) launchpad::Toggle();
        keybd_event(VK_MENU, 0, 0, KEY_MARK);  // permite ao Spotlight vir para a frente e receber o teclado
        keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, KEY_MARK);
        spotlight::Toggle();
        return 0;
    case WM_APP_LAUNCHPAD:
        keybd_event(VK_MENU, 0, 0, KEY_MARK);  // permite ao Launchpad vir para a frente
        keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, KEY_MARK);
        launchpad::Toggle();
        return 0;
    case WM_APP_WINKEY: mesa::WinKey((UINT)w); return 0;
    case WM_APP_CAPTURA: Screenshot((int)w, l != 0); return 0;
    case WM_APP_VOLUME: som::Key((UINT)w, l != 0); return 0;
    case WM_SYSCOMMAND:
        if ((w & 0xFFF0) == SC_TASKLIST) {  // Ctrl+Esc com o Shell como Taskman (sem Explorador): Launchpad
            keybd_event(VK_MENU, 0, 0, KEY_MARK); keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, KEY_MARK);
            launchpad::Toggle();
            return 0;
        }
        break;
    case WM_QUERYENDSESSION: return TRUE;
    case WM_ENDSESSION: if (w) { Log(L"fim da sessão do Windows"); g_sessionEnding = true; Restore(); } return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int AjudanteRodar(const wchar_t* cmd);  // ajudante.cpp: --sair, --aplicar-cursor... (para os scripts do instalador)

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int) {
    if (int r = AjudanteRodar(GetCommandLineW()); r >= 0) return r;
    if (migrar::TrocarPeloNovo()) return 0;  // nome velho (SequoiaShell.exe): o LiquinixShell.exe ao lado assume
    if (const wchar_t* a = wcsstr(GetCommandLineW(), L"--depois-de ")) {  // reinício: espera o Shell antigo sair (até 20 s)
        if (HANDLE old = OpenProcess(SYNCHRONIZE, FALSE, wcstoul(a + 12, nullptr, 10))) { WaitForSingleObject(old, 20000); CloseHandle(old); }
    }
    if (bloqueio::Watchdog(GetCommandLineW())) return 0;  // vigia da tela de bloqueio (outro processo)
    if (mesa::Watchdog(GetCommandLineW())) return 0;      // vigia da mesa: Shell caiu sem Explorador → abre o Explorador
    if (wcsstr(GetCommandLineW(), L"--bloqueio-png ")) {  // imagem de teste da tela de bloqueio
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        OleInitialize(nullptr);
        LoadSettings();  // o tema (Tahoe/Sequoia) e o idioma valem também na imagem de teste
        if (ui::Init()) bloqueio::RenderPng(GetCommandLineW());
        return 0;
    }
    if (wcsstr(GetCommandLineW(), L"--senha-bloqueio")) {  // define a senha própria da tela de bloqueio
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        return bloqueio::SetPasswordUi(hi);
    }
    if (wcsstr(GetCommandLineW(), L"--testar-bloqueio")) {  // a tela de bloqueio por ~3,5 s, sem conferir senha
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        OleInitialize(nullptr);
        return ui::Init() ? bloqueio::Preview(hi) : 1;
    }
    if (wcsstr(GetCommandLineW(), L"--testar-boot")) { SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); return boot::Preview(hi); }
    HANDLE single = CreateMutexW(nullptr, TRUE, L"ShellProprioUmaInstancia");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        // reinício: o Shell antigo ainda não largou (preso no relatório de erro do Windows, por exemplo).
        // Antes o novo desistia e ficava sem Shell nenhum; agora espera ele sair (até 60 s)
        DWORD w = wcsstr(GetCommandLineW(), L"--depois-de ") && single ? WaitForSingleObject(single, 60000) : WAIT_TIMEOUT;
        if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED) return 0;
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    shell::inst = hi;
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    shell::dir = exe; shell::dir = shell::dir.substr(0, shell::dir.find_last_of(L'\\'));
    // o log da sessão anterior fica em shell-anterior.log (o vigia escreve lá o motivo de um reinício)
    MoveFileExW((shell::dir + L"\\shell.log").c_str(), (shell::dir + L"\\shell-anterior.log").c_str(), MOVEFILE_REPLACE_EXISTING);
    g_log = _wfsopen((shell::dir + L"\\shell.log").c_str(), L"w, ccs=UTF-8", _SH_DENYNO);
    Log(L"Shell iniciando");
    OleInitialize(nullptr);
    ctxmenu::Init(hi, g_log);
    if (!ui::Init()) { Log(L"erro: Direct2D/DirectWrite"); return 1; }
    LoadSettings();
    migrar::Registro();  // caminhos com o nome velho (Sequoia) no registro do Windows (depois dos ajustes: grava um)
    // Voltando depois de "Desligar o Liquinix" (Ajustes > Geral): o desligar mandou as pastas e o Win+E de volta para o
    // Explorador (finder_padrao=0). Abrir o Liquinix de novo devolve ao Finder; antes o 0 ficava e o Win+E abria o
    // Explorador até apagar os ajustes (issue #1). Quem desligar "Abrir pastas no Finder" à mão continua com o 0.
    if (shell::Setting(L"finder_volta", L"") == L"1") {
        shell::SetSetting(L"finder_volta", L"");
        std::wstring finder = shell::dir + L"\\" EXE_FINDER;
        if (GetFileAttributesW(finder.c_str()) != INVALID_FILE_ATTRIBUTES) {
            shell::SetSetting(L"finder_padrao", L"1");
            ShellExecuteW(nullptr, L"open", finder.c_str(), L"--quieto --padrao on", nullptr, SW_HIDE);
            Log(L"finder: de volta depois do Desligar (finder_padrao=1, --padrao on)");
        }
    }
    boot::Show(hi);  // tela de inicialização do Mac (só logo depois do login)
    SetUnhandledExceptionFilter(OnCrash);
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &g_origWork, 0);

    simbolo::Dir() = shell::dir.substr(0, shell::dir.find_last_of(L'\\')) + L"\\icones\\simbolos";  // SF Symbols da barra de menus
    apps::Load(shell::dir + L"\\" EXE_FINDER);
    apps::Refresh();

    WNDCLASSW mc{}; mc.lpfnWndProc = MsgProc; mc.hInstance = hi; mc.lpszClassName = L"ShellMensagens";
    RegisterClassW(&mc);
    g_msg = CreateWindowExW(WS_EX_TOOLWINDOW, mc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hi, nullptr);
    apps::ours.push_back(g_msg);
    atualizacao::Start(g_msg);  // aviso de versão nova no GitHub (90 s depois e a cada 6 h)
    boot::Progress(0.2f);

    if (!menubar::Create(hi) || !dock::Create(hi) || !launchpad::Create(hi) || !missioncontrol::Create(hi) || !central::Create(hi) || !spotlight::Create(hi) || !alertas::Create(hi) || !sobre::Create(hi) || !seletor::Create(hi) || !som::Create(hi)) { Log(L"erro criando as janelas"); Restore(); return 1; }
    instalados::Start();  // programa instalado ou removido: o Spotlight e o Launchpad releem os apps
    indice::Start();  // índice de arquivos do Spotlight (thread de fundo)
    revelar::Start();  // clique no vazio da área de trabalho mostra a mesa
    ponteiro::Start();  // sacudir o mouse: o ponteiro cresce para achar
    ponteiro::TemaLeopard(liquid::Leopard());  // tema Leopard: o cursor do Mac OS X (ou devolve o de antes)
    genio::Create(hi);  // efeito gênio ao minimizar
    palco::Sync();  // Organizador Visual (Stage Manager), se ligado
    widgets::Create(hi);  // widgets na mesa (abaixo de todas as janelas)
    bloqueio::Create(hi);  // tela de bloqueio do Sequoia (Win+L)
    notificacoes::Create(hi);  // Central de Notificações (clique no relógio da barra de menus)
    ilha::Create(hi);  // Ilha Dinâmica (ajuste ilha=1)
    notarapida::Create(hi);  // canto inferior direito: Nota Rápida
    progresso::Create(hi);  // copiar/mover/apagar: a janela de progresso do Finder no lugar da do Windows
    cantos::Start();         // Cantos Ativos (Mission Control, mesa, Launchpad... por canto)
    jogo::Start();           // Modo Jogo: jogo na frente desliga o que mais pesa (vidro real, ganchos do mouse...)
    boot::Progress(0.7f);
    tray::Start([] { menubar::Update(); });
    mesa::Start(hi, g_msg);  // ajuste explorer=0: a mesa é nossa e o explorer.exe sai
    menus::Start();
    dialogs::Start();
    janelas::Start();
    encaixe::Start();  // arrastar até a borda: metade, quarto, preencher (com a prévia)
    memoria::Start();  // pressão de memória, soneca de apps (App Nap), memória descartável
    pastas::Start();  // janelas de pasta do Explorador viram o Finder
    limites::Start();  // janelas não cruzam a barra de menus nem o Dock
    sombras::Start();
    HideTaskbars();
    SecondaryTaskbarsOff();  // barra do Windows nos outros monitores: desligada enquanto o Shell roda
    shell::ApplyWorkArea();

    WM_SHELLHOOK = RegisterWindowMessageW(L"SHELLHOOK");
    RegisterShellHookWindow(g_msg);
    RegisterHotKey(g_msg, 1, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, VK_F12);  // era Q: fácil de apertar sem querer
    g_fullHook = shell::Setting(L"tecla_windows_launchpad", L"1") == L"1";
    g_macKeys = shell::Setting(L"atalhos_mac", L"1") == L"1";
    g_capturaExe = GetFileAttributesW((shell::dir + L"\\" EXE_SCREENSHOT).c_str()) != INVALID_FILE_ATTRIBUTES;
    EnsureScreenshot();
    g_volumeKeys = shell::Setting(L"volume_mac", L"1") == L"1";
    if (g_fullHook || g_volumeKeys || shell::Setting(L"bloqueio", L"1") != L"0") std::thread(KeyThread).detach();  // sem o Launchpad: só barra teclas com a tela travada
    SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
    jogo::GanchoPosicao(OnWinEvent, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);  // sai no modo jogo (jogo.cpp)
    SetTimer(g_msg, 1, 1000, nullptr);
    SetTimer(g_msg, 2, 200, nullptr);
    SetTimer(g_msg, 9, 3000, nullptr);  // pré-carregar o menu de contexto (depois que o Shell já apareceu)
    Log(L"pronto: %zu apps no Dock", apps::list.size());
    boot::Done();
    mesa::RunStartupApps();  // só quando o Shell é o shell do Windows (o Explorador não abre no login)
    PreloadApps();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    Restore();
    DeregisterShellHookWindow(g_msg);
    // Sai daqui direto (TerminateProcess), com tudo já devolvido ao Windows. Voltar do wWinMain roda o
    // encerramento do C++ (fecha todos os arquivos, inclusive o log; OleUninitialize desmonta o COM) com as
    // threads de fundo (vidro real, índice, notificações) ainda rodando: elas escreviam no log fechado e o
    // Shell caía ao reiniciar (0xC0000005), e o Windows segurava o processo morto no relatório de erro.
    if (g_log) fflush(g_log);
    ReleaseMutex(single);
    TerminateProcess(GetCurrentProcess(), 0);
    return 0;
}

// ---- modo nativo: o Shell como a barra de tarefas do Windows (barra-nativa\MacBarra.dll, dentro do explorer.exe).
// A MacBarra chama numa thread própria quando o explorer cria a barra (ITrayUI::OnCreate) e esta função só
// volta quando a barra é destruída. Mesmas peças do LiquinixShell.exe, menos as que são de processo:
// mutex de instância única, DPI do processo, tela de inicialização, filtro de falhas, TerminateProcess,
// mesa própria (a mesa é do explorer), tela de bloqueio própria (é o AuthUX), ganchos com DLL nos outros
// programas (janelas.cpp) e esconder a barra do Windows (ela somos nós).
int shell::RodarNativo(HINSTANCE hi, const std::wstring& dirBin, const std::wstring& logPath) {
    nativo = true;
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    shell::inst = hi;
    shell::dir = dirBin;
    g_log = _wfsopen(logPath.c_str(), L"w, ccs=UTF-8", _SH_DENYNO);
    Log(L"Shell nativo iniciando dentro do explorer (pid %lu), pasta %ls", GetCurrentProcessId(), dirBin.c_str());
    OleInitialize(nullptr);
    ctxmenu::Init(hi, g_log);
    if (!ui::Init()) { Log(L"erro: Direct2D/DirectWrite"); return 1; }
    LoadSettings();
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &g_origWork, 0);

    simbolo::Dir() = shell::dir.substr(0, shell::dir.find_last_of(L'\\')) + L"\\icones\\simbolos";
    apps::Load(shell::dir + L"\\" EXE_FINDER);
    apps::Refresh();

    WNDCLASSW mc{}; mc.lpfnWndProc = MsgProc; mc.hInstance = hi; mc.lpszClassName = L"ShellMensagens";
    RegisterClassW(&mc);
    g_msg = CreateWindowExW(WS_EX_TOOLWINDOW, mc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hi, nullptr);
    apps::ours.push_back(g_msg);
    atualizacao::Start(g_msg);  // aviso de versão nova no GitHub (90 s depois e a cada 6 h)

    if (!menubar::Create(hi) || !dock::Create(hi) || !launchpad::Create(hi) || !missioncontrol::Create(hi) || !central::Create(hi) || !spotlight::Create(hi) || !alertas::Create(hi) || !sobre::Create(hi) || !seletor::Create(hi) || !som::Create(hi)) { Log(L"erro criando as janelas"); Restore(); return 1; }
    instalados::Start();
    indice::Start();
    revelar::Start();
    ponteiro::Start();
    ponteiro::TemaLeopard(liquid::Leopard());
    genio::Create(hi);
    widgets::Create(hi);
    notificacoes::Create(hi);
    ilha::Create(hi);
    notarapida::Create(hi);
    progresso::Create(hi);
    cantos::Start();
    tray::Start([] { menubar::Update(); });  // nativo: sem janela; os ícones chegam por tray::DoExplorer
    menus::Start();
    dialogs::Start();
    pastas::Start();
    if (GetFileAttributesW(L"C:\\Users\\Public\\MacMesa-desligada.txt") == INVALID_FILE_ATTRIBUTES)
        mesa::Start(hi, g_msg);  // nativo: a mesa do projeto por cima da do Explorador (mesmo processo)
    else Log(L"MacMesa-desligada.txt: a mesa fica com o Windows");
    limites::Start();
    sombras::Start();
    // Cantos do Mac (maiores que os 8 px do Windows), sombra e animações nas janelas com a barra padrão: só a
    // LiquinixWindow.dll DENTRO dos programas consegue (autorizado pelo usuário em 12/11/2026, "pode injetar").
    // Ela cuida das janelas de barra padrão; as de barra própria (Chrome, Bloco de Notas...) continuam com barras.cpp.
    // C:\Users\Public\MacInjecao-desligada.txt volta a não carregar nada nos outros programas.
    if (GetFileAttributesW(L"C:\\Users\\Public\\MacInjecao-desligada.txt") == INVALID_FILE_ATTRIBUTES) janelas::Start();
    else Log(L"MacInjecao-desligada.txt: nada é carregado nos outros programas (cantos de 8 px)");
    barras::Start();  // nativo: barra de título do Mac por fora (sem nada dentro dos outros programas)
    shell::ApplyWorkArea();

    WM_SHELLHOOK = RegisterWindowMessageW(L"SHELLHOOK");
    RegisterShellHookWindow(g_msg);
    g_fullHook = shell::Setting(L"tecla_windows_launchpad", L"1") == L"1";
    g_macKeys = shell::Setting(L"atalhos_mac", L"1") == L"1";
    g_capturaExe = GetFileAttributesW((shell::dir + L"\\" EXE_SCREENSHOT).c_str()) != INVALID_FILE_ATTRIBUTES;
    EnsureScreenshot();
    g_volumeKeys = shell::Setting(L"volume_mac", L"1") == L"1";
    if (g_fullHook || g_volumeKeys) std::thread(KeyThread).detach();
    SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnWinEvent, 0, 0, WINEVENT_OUTOFCONTEXT);
    jogo::GanchoPosicao(OnWinEvent, WINEVENT_OUTOFCONTEXT);  // sai no modo jogo (jogo.cpp)
    SetTimer(g_msg, 1, 1000, nullptr);
    SetTimer(g_msg, 9, 3000, nullptr);
    Log(L"pronto (nativo): %zu apps no Dock", apps::list.size());
    PreloadApps();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    Restore();
    DeregisterShellHookWindow(g_msg);
    Log(L"Shell nativo encerrado");
    if (g_log) fflush(g_log);
    return 0;
}
