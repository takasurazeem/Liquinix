// Barra de título do Mac nas janelas dos outros programas (ver src\janelas\macjanela.cpp).
// O gancho é posto só na thread de cada janela elegível de um programa permitido: jogos, launchers
// e anti-cheat nunca recebem a DLL. Programas de 32 bits passam pelo MacJanela32Host.exe.
// Desligar: janelas_mac=0 em ajustes.txt. Excluir mais programas: %APPDATA%\Shell\janelas-excluidas.txt (os jogos
// de %APPDATA%\Shell\jogos.txt, a lista do Modo Jogo, também ficam de fora).
// Quem entra (20/11/2026, regra dura: nada de DLL em jogo ou anti-cheat; detalhes em src\comum\jogos.h):
//   - janela com a barra de título padrão do Windows, de programa sem NENHUMA marca de jogo (nome, pasta, arquivos de
//     motor/loja/anti-cheat ao lado do .exe, classe de motor de jogo);
//   - janela sem a barra do Windows (barra própria) só de app Chromium/Electron reconhecido pelos arquivos;
//   - nada novo enquanto um app roda em tela cheia exclusiva (Direct3D) ou em modo apresentação/jogo (o Windows
//     avisa); programa com janela sem borda cobrindo o monitor (jogo em "tela cheia sem borda") fica fora de vez.
// A decisão é guardada por processo; a DLL confere de novo ao carregar e recusa entrar em jogo.
#include "shell.h"
#include "../comum/jogos.h"
#include <shellapi.h>
#include <shlobj.h>
#include <map>
#include <set>

namespace {

HMODULE g_dll;
HOOKPROC g_proc;
HWINEVENTHOOK g_ev;
std::map<DWORD, HHOOK> g_hooks;      // thread -> gancho (64 bits)
std::set<DWORD> g_hooks32;           // threads entregues ao host de 32 bits
struct Permissao { bool ok = false, chromium = false; ULONGLONG nasc = 0; std::wstring exe; };
std::map<DWORD, Permissao> g_procOk; // processo -> permitido (chromium: Chromium/Electron, pode a barra própria)
std::vector<std::wstring> g_extra;   // exclusões do usuário
const UINT HOST_HOOK = WM_APP + 1, HOST_QUIT = WM_APP + 2;

// Jogos, launchers, anti-cheat e partes do próprio Windows: nunca recebem a DLL
const wchar_t* EXCLUDED_EXE[] = {
    L"explorer.exe", L"dwm.exe", L"shell.exe", L"arquivos.exe", L"ajustes.exe", L"monitoratividade.exe", L"terminal.exe", L"calculadora.exe", L"notas.exe",
    L"wacosshell.exe", L"wacosfinder.exe", L"wacossettings.exe", L"wacosactivitymonitor.exe", L"wacosterminal.exe", L"wacoscalculator.exe", L"wacosnotes.exe", L"wacoswindow32host.exe", L"searchhost.exe", L"startmenuexperiencehost.exe",
    L"applicationframehost.exe", L"textinputhost.exe", L"lockapp.exe", L"shellexperiencehost.exe", L"systemsettings.exe", L"taskmgr.exe",
    L"steam.exe", L"steamwebhelper.exe", L"gameoverlayui.exe", L"battle.net.exe", L"agent.exe", L"eadesktop.exe", L"eabackgroundservice.exe",
    L"origin.exe", L"epicgameslauncher.exe", L"epicwebhelper.exe", L"riotclientservices.exe", L"riotclientux.exe", L"riotclientuxrender.exe",
    L"leagueclient.exe", L"leagueclientux.exe", L"leagueclientuxrender.exe", L"league of legends.exe", L"valorant.exe", L"valorant-win64-shipping.exe",
    L"vgc.exe", L"vgtray.exe", L"faceit.exe", L"faceitclient.exe", L"easyanticheat.exe", L"easyanticheat_eos.exe", L"beservice.exe",
    L"beservice_x64.exe", L"ubisoftconnect.exe", L"upc.exe", L"galaxyclient.exe", L"xboxpcapp.exe", L"gamebar.exe", L"robloxplayerbeta.exe",
    L"minecraftlauncher.exe", L"javaw.exe", L"rockstarlauncher.exe", L"socialclubhelper.exe", L"overwolf.exe", L"medal.exe",
};
const wchar_t* EXCLUDED_PATH[] = {
    L"\\steamapps\\", L"\\riot games\\", L"\\epic games\\", L"\\ea games\\", L"\\electronic arts\\", L"\\battle.net\\", L"\\blizzard",
    L"\\ubisoft", L"\\gog galaxy\\", L"\\xboxgames\\",L"\\rockstar games\\", L"\\easyanticheat", L"\\battleye",
};

std::wstring Lower(std::wstring s) { for (auto& c : s) c = towlower(c); return s; }

void LoadList(const std::wstring& f, bool criar) {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, f.c_str(), L"r, ccs=UTF-8") || !fp) {
        // cria o arquivo com a explicação, para o usuário saber onde pôr os jogos dele
        if (criar && !_wfopen_s(&fp, f.c_str(), L"w, ccs=UTF-8") && fp) {
            fwprintf(fp, L"# Um programa por linha (nome do .exe ou parte do caminho). Estes nunca recebem a barra de título do Mac.\n");
            fclose(fp);
        }
        return;
    }
    wchar_t line[512];
    while (fgetws(line, 512, fp)) {
        std::wstring s = line;
        while (!s.empty() && iswspace(s.back())) s.pop_back();
        if (!s.empty() && s[0] != L'#') g_extra.push_back(Lower(s));
    }
    fclose(fp);
}

void LoadExtra() {
    g_extra.clear();
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) return;
    std::wstring base = std::wstring(p) + L"\\Shell\\";
    CoTaskMemFree(p);
    LoadList(base + L"janelas-excluidas.txt", true);
    LoadList(base + L"jogos.txt", false);  // a lista do Modo Jogo (jogo.cpp): jogo nunca recebe a DLL
}

// Um app está em tela cheia exclusiva (Direct3D) ou em modo apresentação/jogo: nenhum gancho novo agora.
bool TelaCheiaExclusiva() {
    QUERY_USER_NOTIFICATION_STATE q{};
    return SUCCEEDED(SHQueryUserNotificationState(&q)) && (q == QUNS_RUNNING_D3D_FULL_SCREEN || q == QUNS_BUSY || q == QUNS_PRESENTATION_MODE);
}

bool CobreMonitor(HWND h) {
    MONITORINFO mi{ sizeof(mi) };
    RECT r;
    if (!GetWindowRect(h, &r) || !GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi)) return false;
    return r.left <= mi.rcMonitor.left && r.top <= mi.rcMonitor.top && r.right >= mi.rcMonitor.right && r.bottom >= mi.rcMonitor.bottom;
}

ULONGLONG Criacao(HANDLE ph) {
    FILETIME c{}, e{}, k{}, u{};
    if (!GetProcessTimes(ph, &c, &e, &k, &u)) return 0;
    return ((ULONGLONG)c.dwHighDateTime << 32) | c.dwLowDateTime;
}

// Pode pôr o gancho na janela h (processo pid)? O resultado do programa fica guardado (pelo pid + hora de criação,
// para um jogo aberto depois com o mesmo número não herdar a permissão de outro programa).
Permissao Allowed(HWND h, DWORD pid, bool* is32) {
    HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!ph) return {};  // provavelmente elevado: não dá para pôr o gancho
    BOOL wow = FALSE;
    IsWow64Process(ph, &wow);
    *is32 = wow != FALSE;
    ULONGLONG nasc = Criacao(ph);
    auto it = g_procOk.find(pid);
    if (it != g_procOk.end() && it->second.nasc == nasc) {
        CloseHandle(ph);
        Permissao r = it->second;
        // janela de motor de jogo aparecendo depois (launcher que vira o jogo): o programa sai de vez
        wchar_t cls[64] = {}; GetClassNameW(h, cls, 64);
        if (r.ok && jogos::ClasseDeJogo(cls)) { it->second.ok = false; Log(L"janelas: %ls fora (janela de jogo)", it->second.exe.c_str()); return {}; }
        return r;
    }
    wchar_t path[MAX_PATH] = {}; DWORD n = MAX_PATH;
    QueryFullProcessImageNameW(ph, 0, path, &n);
    // apps da Loja: só os de confiança total (Claude, WhatsApp...); os em contêiner (UWP) não carregam a DLL
    bool container = false;
    HANDLE tok = nullptr;
    if (OpenProcessToken(ph, TOKEN_QUERY, &tok)) {
        DWORD v = 0, got = 0;
        if (GetTokenInformation(tok, TokenIsAppContainer, &v, sizeof(v), &got)) container = v != 0;
        CloseHandle(tok);
    }
    CloseHandle(ph);
    std::wstring full = Lower(path), exe = full.substr(full.find_last_of(L'\\') + 1);
    Permissao r;
    r.nasc = nasc;
    r.exe = exe;
    r.ok = !full.empty() && !container;
    const wchar_t* por = L"";
    // Xbox/jogos da Loja continuam fora (anti-cheat)
    if (full.find(L"\\windowsapps\\") != std::wstring::npos && (full.find(L"microsoft.gaming") != std::wstring::npos || full.find(L"xbox") != std::wstring::npos)) { r.ok = false; por = L"Xbox"; }
    for (auto e : EXCLUDED_EXE) if (exe == e) r.ok = false;
    // os apps do WacOS: os que usam esta barra carregam a DLL eles mesmos (src\comum\barra_mac.h), com ou sem o
    // Shell; os outros desenham a própria. O gancho aqui punha uma segunda cópia por cima
    // (também as cópias renomeadas na hora de instalar, "inst-...-wacosfinder.exe")
    if (exe.find(L"wacos") != std::wstring::npos) r.ok = false;
    for (auto e : EXCLUDED_PATH) if (full.find(e) != std::wstring::npos) r.ok = false;
    for (auto& e : g_extra) if (exe == e || full.find(e) != std::wstring::npos) { r.ok = false; por = L"lista do usuário"; }
    // jogo, launcher ou anti-cheat (nome, pasta, arquivos ao lado do .exe): nunca
    if (r.ok && jogos::ProgramaDeJogo(path)) { r.ok = false; por = L"marca de jogo"; }
    wchar_t cls[64] = {}; GetClassNameW(h, cls, 64);
    if (r.ok && jogos::ClasseDeJogo(cls)) { r.ok = false; por = L"janela de motor de jogo"; }
    r.chromium = r.ok && jogos::ProgramaChromium(path);
    // janela sem borda cobrindo o monitor de um programa que não é navegador: jogo em "tela cheia sem borda"
    LONG st = GetWindowLongW(h, GWL_STYLE);
    if (r.ok && !r.chromium && IsWindowVisible(h) && (st & WS_CAPTION) != WS_CAPTION && CobreMonitor(h)) { r.ok = false; por = L"tela cheia sem borda"; }
    g_procOk[pid] = r;
    if (r.ok) Log(L"janelas: barra do Mac em %ls%ls", exe.c_str(), r.chromium ? L" (Chromium/Electron)" : L"");
    else if (*por) Log(L"janelas: %ls fora (%ls)", exe.c_str(), por);
    return r;
}

// 1: barra de título do Windows; 2: barra própria (só vale para Chromium/Electron); 0: não
int Candidate(HWND h) {
    if (!IsWindow(h) || GetAncestor(h, GA_ROOT) != h || !IsWindowVisible(h)) return 0;
    LONG st = GetWindowLongW(h, GWL_STYLE), ex = GetWindowLongW(h, GWL_EXSTYLE);
    if ((st & WS_CHILD) || (ex & WS_EX_TOOLWINDOW)) return 0;
    RECT wr; GetWindowRect(h, &wr);
    POINT c{ 0, 0 }; ClientToScreen(h, &c);
    if ((st & WS_CAPTION) == WS_CAPTION && c.y - wr.top >= 18) return 1;  // barra de título do Windows (a DLL confere de novo, com o DPI do programa)
    // barra própria (Chrome, Discord, Electron): a DLL anima o maximizar e acerta cantos e borda (LeveProc).
    // Só a janela do Chromium (classe Chrome_WidgetWin_1); o programa ainda precisa ser Chromium/Electron (Allowed)
    wchar_t cls[32] = {}; GetClassNameW(h, cls, 32);
    if (wcscmp(cls, L"Chrome_WidgetWin_1")) return 0;
    if ((st & WS_THICKFRAME) && (st & WS_MAXIMIZEBOX) && !(ex & WS_EX_NOACTIVATE) && wr.right - wr.left >= 300 && wr.bottom - wr.top >= 200) return 2;
    return 0;
}

void Consider(HWND h) {
    int tipo = g_proc ? Candidate(h) : 0;
    if (!tipo) return;
    DWORD pid = 0, tid = GetWindowThreadProcessId(h, &pid);
    if (!tid || pid == GetCurrentProcessId()) return;
    if (g_hooks.count(tid) || g_hooks32.count(tid)) { PostMessageW(h, WM_NULL, 0, 0); return; }
    if (TelaCheiaExclusiva()) return;  // jogo/apresentação em tela cheia: nada novo agora (volta na próxima janela)
    bool is32 = false;
    Permissao pm = Allowed(h, pid, &is32);
    if (!pm.ok || (tipo == 2 && !pm.chromium)) return;
    if (is32) {
        HWND host = FindWindowExW(HWND_MESSAGE, nullptr, L"MacJanela32Host", nullptr);
        if (!host) return;
        PostMessageW(host, HOST_HOOK, tid, (LPARAM)h);
        g_hooks32.insert(tid);
        return;
    }
    HHOOK hk = SetWindowsHookExW(WH_GETMESSAGE, g_proc, g_dll, tid);
    // Erro 126: o Windows guarda no máximo 32 DLLs de gancho diferentes, e uma só sai da lista quando
    // todos os programas que a carregaram fecham. NUNCA cair para o MacJanela.dll sem cópia: um programa
    // que já tem uma versão antiga carregada com esse mesmo caminho recebe o endereço do gancho da versão
    // nova e trava (o Ableton fechou assim).
    if (!hk) { Log(L"janelas: gancho falhou na thread %lu (erro %lu)", tid, GetLastError()); return; }
    g_hooks[tid] = hk;
    PostMessageW(h, WM_NULL, 0, 0);  // a thread processa uma mensagem e a DLL entra
}

void CALLBACK OnEvent(HWINEVENTHOOK, DWORD, HWND h, LONG obj, LONG, DWORD, DWORD) {
    if (obj == OBJID_WINDOW && h) Consider(h);
}

// Janela de app criada (ainda escondida): o gancho entra já, para a DLL estar lá quando ela aparecer (antes só
// no "mostrar": a primeira janela de um app recém-aberto aparecia sem a animação de abrir)
HWINEVENTHOOK g_evCreate;
void CALLBACK OnCreate(HWINEVENTHOOK, DWORD, HWND h, LONG obj, LONG, DWORD, DWORD) {
    if (obj != OBJID_WINDOW || !h || !g_proc || GetAncestor(h, GA_ROOT) != h) return;
    LONG st = GetWindowLongW(h, GWL_STYLE), ex = GetWindowLongW(h, GWL_EXSTYLE);
    if ((st & WS_CHILD) || (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) || !((st & WS_CAPTION) == WS_CAPTION || (st & WS_THICKFRAME))) return;
    DWORD pid = 0, tid = GetWindowThreadProcessId(h, &pid);
    if (!tid || pid == GetCurrentProcessId() || g_hooks.count(tid) || g_hooks32.count(tid)) return;
    if (TelaCheiaExclusiva()) return;
    // sem a barra do Windows: só a janela do Chromium/Electron
    wchar_t cls[32] = {}; GetClassNameW(h, cls, 32);
    bool propria = (st & WS_CAPTION) != WS_CAPTION;
    if (propria && wcscmp(cls, L"Chrome_WidgetWin_1")) return;
    bool is32 = false;
    Permissao pm = Allowed(h, pid, &is32);
    if (!pm.ok || (propria && !pm.chromium)) return;
    if (is32) {
        // 32 bits: pelo hospedeiro, já na criação (só no "mostrar" a primeira janela aparecia antes de a DLL
        // entrar e abria sem a animação). Esperando o hospedeiro pôr o gancho: a janela ainda está escondida
        HWND host = FindWindowExW(HWND_MESSAGE, nullptr, L"MacJanela32Host", nullptr);
        if (!host) return;
        DWORD_PTR r = 0;
        SendMessageTimeoutW(host, HOST_HOOK, tid, (LPARAM)h, SMTO_ABORTIFHUNG, 200, &r);
        g_hooks32.insert(tid);
        return;
    }
    HHOOK hk = SetWindowsHookExW(WH_GETMESSAGE, g_proc, g_dll, tid);
    if (!hk) return;
    g_hooks[tid] = hk;
    PostMessageW(h, WM_NULL, 0, 0);  // a thread processa uma mensagem e a DLL entra antes de mostrar
}

// Cópia com nome próprio em bin\obj\mj: os apps que já têm uma versão antiga presa carregam esta como outro
// módulo. O nome vem da data do arquivo: reabrir o Shell reaproveita a mesma cópia (cada nome novo ocupa um
// dos 32 lugares de DLL de gancho do Windows até os apps fecharem); só uma compilação nova gera outro.
// Vale para a DLL de 64 bits e para a de 32 (o hospedeiro de 32 bits recebe o caminho pronto).
std::wstring OwnCopy(const std::wstring& file, const wchar_t* prefix) {
    std::wstring copies = shell::dir + L"\\obj\\mj";
    CreateDirectoryW((shell::dir + L"\\obj").c_str(), nullptr);
    CreateDirectoryW(copies.c_str(), nullptr);
    std::wstring src = shell::dir + L"\\" + file, name;
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(src.c_str(), GetFileExInfoStandard, &fa)) return L"";
    wchar_t b[64]; swprintf_s(b, L"%s-%08lx%08lx.dll", prefix, fa.ftLastWriteTime.dwHighDateTime, fa.ftLastWriteTime.dwLowDateTime);
    name = b;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((copies + L"\\" + prefix + L"-*.dll").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do if (_wcsicmp(fd.cFileName, name.c_str())) DeleteFileW((copies + L"\\" + fd.cFileName).c_str()); while (FindNextFileW(f, &fd));  // as em uso ficam
        FindClose(f);
    }
    std::wstring copy = copies + L"\\" + name;
    if (GetFileAttributesW(copy.c_str()) == INVALID_FILE_ATTRIBUTES && !CopyFileW(src.c_str(), copy.c_str(), TRUE)) return L"";
    return copy;
}

}  // namespace

void janelas::Start() {
    if (g_dll) return;
    // só as barras, sem o Shell (WacOSTitleBars.exe, src\titulos): o Shell assume. Fechar devolve as janelas
    // ao Windows e o gancho daqui entra logo depois (esperando ele sair: dois ganchos na mesma thread não)
    if (HWND only = FindWindowW(L"WacOSBarrasDeTitulo", nullptr)) {
        DWORD pid = 0; GetWindowThreadProcessId(only, &pid);
        if (pid != GetCurrentProcessId()) {
            HANDLE ph = OpenProcess(SYNCHRONIZE, FALSE, pid);
            PostMessageW(only, WM_CLOSE, 0, 0);
            if (ph) { WaitForSingleObject(ph, 3000); CloseHandle(ph); }
            Log(L"janelas: WacOSTitleBars fechado (o Shell assume as barras)");
        }
    }
    if (shell::Setting(L"janelas_mac", L"1") != L"1") return;
    std::wstring copy = OwnCopy(DLL_WINDOW, L"WacOSWindow");
    // sem cópia própria, nada de gancho: o mesmo caminho com outra versão trava os programas (ver Consider)
    if (copy.empty()) {
        Log(L"janelas: não deu para copiar " DLL_WINDOW);
        return;
    }
    g_dll = LoadLibraryW(copy.c_str());
    g_proc = g_dll ? (HOOKPROC)GetProcAddress(g_dll, "GetMsgProc") : nullptr;
    if (!g_proc) { Log(L"janelas: " DLL_WINDOW L" não encontrada"); return; }
    LoadExtra();
    // programas de 32 bits: a DLL de 32 bits fica no rundll32.exe de 32 bits do Windows (entrada WacOSHost, ver
    // macjanela.cpp), que recebe os pedidos de gancho pela janela "MacJanela32Host"
    wchar_t wow[MAX_PATH]{};
    std::wstring copy32;
    if (GetSystemWow64DirectoryW(wow, MAX_PATH) && !FindWindowExW(HWND_MESSAGE, nullptr, L"MacJanela32Host", nullptr) &&
        !(copy32 = OwnCopy(DLL_WINDOW32, L"WacOSWindow32")).empty()) {
        STARTUPINFOW si{ sizeof(si) }; PROCESS_INFORMATION pi{};
        std::wstring cmd = L"\"" + std::wstring(wow) + L"\\rundll32.exe\" \"" + copy32 + L"\",WacOSHost";
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
            WaitForInputIdle(pi.hProcess, 2000);
            CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
        }
    }
    g_ev = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, OnEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    g_evCreate = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_CREATE, nullptr, OnCreate, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    EnumWindows([](HWND h, LPARAM) -> BOOL { Consider(h); return TRUE; }, 0);
}

void janelas::Foreground(HWND h) { Consider(h); }

void janelas::Stop() {
    if (!g_dll) return;
    if (g_ev) { UnhookWinEvent(g_ev); g_ev = nullptr; }
    if (g_evCreate) { UnhookWinEvent(g_evCreate); g_evCreate = nullptr; }
    // devolve o visual do Windows a todas as janelas transformadas (a DLL desfaz na thread de cada uma)
    UINT undo = RegisterWindowMessageW(L"MacJanelaDesfazer");
    EnumWindows([](HWND h, LPARAM undo) -> BOOL {
        if (GetPropW(h, L"MacJanela") || GetPropW(h, L"MacJanelaLeve")) SendMessageTimeoutW(h, (UINT)undo, 0, 0, SMTO_ABORTIFHUNG, 300, nullptr);
        return TRUE;
    }, undo);
    for (auto& [t, hk] : g_hooks) UnhookWindowsHookEx(hk);
    g_hooks.clear();
    g_hooks32.clear();
    if (HWND host = FindWindowExW(HWND_MESSAGE, nullptr, L"MacJanela32Host", nullptr)) PostMessageW(host, HOST_QUIT, 0, 0);
    g_proc = nullptr;
    g_dll = nullptr;  // a DLL fica carregada (presa) no nosso processo também
}
