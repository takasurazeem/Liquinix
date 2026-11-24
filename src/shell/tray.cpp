// Bandeja: Shell_TrayWnd próprio acima do do Explorador (protótipo em src/tools/bandeja_teste.cpp,
// estudo em docs/estudo-bandeja.md). Recebe Shell_NotifyIcon dos apps e repassa tudo ao Explorador.
#include "shell.h"
#include <shellapi.h>
#include <shlwapi.h>
#pragma comment(lib, "shlwapi.lib")
#include <algorithm>

#pragma pack(push, 4)
struct NotifyIconData32 {  // layout fixo de 32 bits do WM_COPYDATA
    DWORD cbSize, hWnd;
    UINT uID, uFlags, uCallbackMessage;
    DWORD hIcon;
    WCHAR szTip[128];
    DWORD dwState, dwStateMask;
    WCHAR szInfo[256];
    UINT uVersion;
    WCHAR szInfoTitle[64];
    DWORD dwInfoFlags;
    GUID guidItem;
    DWORD hBalloonIcon;
};
struct ShellTrayData { DWORD dwSignature, dwMessage; NotifyIconData32 nid; };
#pragma pack(pop)

std::vector<TrayIcon> tray::icons;
static HWND g_tray, g_notify;
static std::function<void()> g_changed;

static HWND ExplorerTray() {
    HWND h = nullptr;
    while ((h = FindWindowExW(nullptr, h, L"Shell_TrayWnd", nullptr))) if (h != g_tray) return h;
    return nullptr;
}

static int Find(const NotifyIconData32& n) {
    for (int i = 0; i < (int)tray::icons.size(); i++) {
        auto& c = tray::icons[i];
        if ((n.uFlags & NIF_GUID) && n.guidItem != GUID{} && c.guid == n.guidItem) return i;
        if (c.hwnd == (HWND)(ULONG_PTR)n.hWnd && c.uid == n.uID) return i;
    }
    return -1;
}

static void Apply(TrayIcon& c, const NotifyIconData32& n) {
    if (n.uFlags & NIF_MESSAGE) c.callback = n.uCallbackMessage;
    if (n.uFlags & NIF_ICON) { if (c.icon) DestroyIcon(c.icon); c.icon = n.hIcon ? CopyIcon((HICON)(ULONG_PTR)n.hIcon) : nullptr; }
    if (n.uFlags & NIF_TIP) c.tip = n.szTip;
    if ((n.uFlags & NIF_STATE) && (n.dwStateMask & NIS_HIDDEN)) c.hidden = (n.dwState & NIS_HIDDEN) != 0;
    if (n.uFlags & NIF_GUID) c.guid = n.guidItem;
}

static BOOL OnNotify(DWORD msg, const NotifyIconData32& n) {
    int i = Find(n);
    auto& v = tray::icons;
    switch (msg) {
    case NIM_ADD:
        if (i >= 0) { Apply(v[i], n); break; }
        { TrayIcon c; c.hwnd = (HWND)(ULONG_PTR)n.hWnd; c.uid = n.uID; Apply(c, n); v.push_back(c); }
        break;
    case NIM_MODIFY: if (i < 0) return FALSE; Apply(v[i], n); break;
    case NIM_DELETE:
        if (i < 0) return FALSE;
        if (v[i].icon) DestroyIcon(v[i].icon);
        v.erase(v.begin() + i);
        break;
    case NIM_SETVERSION: if (i < 0) return FALSE; v[i].version = n.uVersion; break;
    case NIM_SETFOCUS: break;
    default: return FALSE;
    }
    if (g_changed) g_changed();
    return TRUE;
}

// Repassa à barra do Windows (escondida). Com limite de tempo: um Explorador travado (ou abrindo) não pode
// segurar o Shell; antes era SendMessageW sem limite e o Shell inteiro travava esperando por ele.
static LRESULT Forward(UINT m, WPARAM w, LPARAM l) {
    HWND ex = ExplorerTray();
    if (!ex) return 0;
    DWORD_PTR r = 0;
    return SendMessageTimeoutW(ex, m, w, l, SMTO_ABORTIFHUNG | SMTO_NORMAL, 300, &r) ? (LRESULT)r : 0;
}

static LRESULT CALLBACK TrayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_COPYDATA && l) {
        auto* cd = (COPYDATASTRUCT*)l;
        if (cd->dwData == 1 && cd->cbData >= 8 + offsetof(NotifyIconData32, szTip)) {
            auto* td = (ShellTrayData*)cd->lpData;
            NotifyIconData32 n{};
            memcpy(&n, &td->nid, std::min<size_t>(sizeof(n), cd->cbData - 8));
            BOOL ours = OnNotify(td->dwMessage, n);
            LRESULT theirs = Forward(m, w, l);  // a barra do Windows (escondida) continua em dia
            return ours || theirs;
        }
        return Forward(m, w, l);
    }
    if (m == WM_WINDOWPOSCHANGED && (((WINDOWPOS*)l)->flags & SWP_SHOWWINDOW)) ShowWindow(h, SW_HIDE);
    // mensagens registradas (>= 0xC000, como o aviso "TaskbarCreated" que o próprio Shell espalha) não são
    // do protocolo da bandeja: não vão para o Explorador
    if ((m >= WM_USER && m < 0xC000) || m == WM_COMMAND || m == WM_ACTIVATEAPP) return Forward(m, w, l);
    return DefWindowProcW(h, m, w, l);
}

// ---- modo nativo: a Shell_TrayWnd é a barra do Windows (a MacBarra). O explorer entrega o WM_COPYDATA dos
// apps à barra (ITrayUI::HandleCopyData) na thread dela; aqui ele passa para a thread do Shell por uma janela
// só de mensagens (SendMessage entre threads: a lista de ícones só é mexida na thread do Shell)
static HWND g_nativa;
const UINT WM_NATIVA_ICONE = WM_APP + 1;

static LRESULT CALLBACK NativaProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NATIVA_ICONE && l) return OnNotify((DWORD)w, *(const NotifyIconData32*)l);
    return DefWindowProcW(h, m, w, l);
}

bool tray::DoExplorer(COPYDATASTRUCT* cd, LRESULT* res) {
    if (!cd || cd->dwData != 1 || !cd->lpData || cd->cbData < 8 + offsetof(NotifyIconData32, szTip) || !g_nativa) return false;
    auto* td = (ShellTrayData*)cd->lpData;
    NotifyIconData32 n{};
    memcpy(&n, &td->nid, std::min<size_t>(sizeof(n), cd->cbData - 8));
    DWORD_PTR r = 0;
    if (!SendMessageTimeoutW(g_nativa, WM_NATIVA_ICONE, td->dwMessage, (LPARAM)&n, SMTO_NORMAL, 1000, &r)) r = FALSE;
    if (res) *res = (LRESULT)r;
    return true;
}

// Outra Shell_TrayWnd de um WacOSShell (o Shell antigo, num reinício, ainda fechando): o aviso "TaskbarCreated"
// espera ela sumir. Antes ele ia na hora, os apps mandavam os ícones para a bandeja do Shell antigo, que logo fechava
// levando os ícones junto (a bandeja ficava vazia depois de reiniciar o Shell)
static bool g_avisoPendente;
static DWORD g_avisoDesde;
static bool OutroShellComBandeja() {
    HWND h = nullptr;
    while ((h = FindWindowExW(nullptr, h, L"Shell_TrayWnd", nullptr))) {
        if (h == g_tray) continue;
        DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
        if (!pid || pid == GetCurrentProcessId()) continue;
        if (HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            wchar_t p[MAX_PATH] = {}; DWORD n = MAX_PATH;
            bool shell = QueryFullProcessImageNameW(ph, 0, p, &n) && StrStrIW(p, L"WacOSShell") != nullptr;
            CloseHandle(ph);
            if (shell) return true;
        }
    }
    return false;
}
static void AvisarApps() {
    g_avisoPendente = false;
    SendNotifyMessageW(HWND_BROADCAST, RegisterWindowMessageW(L"TaskbarCreated"), 0, 0);  // os apps mandam os ícones de novo
}

void tray::Start(std::function<void()> changed) {
    g_changed = changed;
    if (shell::nativo) {
        WNDCLASSW nc{}; nc.lpfnWndProc = NativaProc; nc.hInstance = shell::inst; nc.lpszClassName = L"ShellBandejaNativa";
        RegisterClassW(&nc);
        g_nativa = CreateWindowExW(0, nc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, shell::inst, nullptr);
        SendNotifyMessageW(HWND_BROADCAST, RegisterWindowMessageW(L"TaskbarCreated"), 0, 0);  // os apps mandam os ícones de novo
        Log(L"bandeja (nativo): ícones pela barra do Windows");
        return;
    }
    WNDCLASSW tc{}; tc.lpfnWndProc = TrayProc; tc.hInstance = shell::inst; tc.lpszClassName = L"Shell_TrayWnd";
    RegisterClassW(&tc);
    tc.lpszClassName = L"TrayNotifyWnd";
    RegisterClassW(&tc);
    RECT tb{ 0, 0, 1, 1 };
    if (HWND ex = ExplorerTray()) GetWindowRect(ex, &tb);
    g_tray = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"Shell_TrayWnd", L"", WS_POPUP,
                             tb.left, tb.top, tb.right - tb.left, tb.bottom - tb.top, nullptr, nullptr, shell::inst, nullptr);
    g_notify = CreateWindowExW(0, L"TrayNotifyWnd", nullptr, WS_CHILD, 0, 0, 1, 1, g_tray, nullptr, shell::inst, nullptr);
    apps::ours.push_back(g_tray);
    if (OutroShellComBandeja()) { g_avisoPendente = true; g_avisoDesde = GetTickCount(); Log(L"bandeja: esperando o Shell antigo fechar para chamar os ícones"); }
    else AvisarApps();
    Log(L"bandeja: Shell_TrayWnd próprio criado");
}

// "TaskbarCreated" ao sair devolve os ícones à barra do Windows. No reinício (o Shell novo já abre e avisa) ele não
// vai: o Lively Wallpaper lê cada aviso como "o Explorer caiu" e reinicia o papel de parede; dois avisos em
// segundos (sair + abrir) davam "Explorer restarted multiple times in the last 30s" e o "fatal error" (17/11/2026)
static bool g_semAviso;
void tray::SemAvisoAoSair() { g_semAviso = true; }
void tray::Stop() {
    if (g_nativa) { DestroyWindow(g_nativa); g_nativa = nullptr; }
    if (!g_tray) return;
    DestroyWindow(g_tray);
    g_tray = nullptr;
    if (!g_semAviso) SendNotifyMessageW(HWND_BROADCAST, RegisterWindowMessageW(L"TaskbarCreated"), 0, 0);
}

void tray::Keepalive() {
    if (g_avisoPendente && (!OutroShellComBandeja() || GetTickCount() - g_avisoDesde > 15000)) {  // no máximo 15 s
        Log(L"bandeja: Shell antigo fechou, chamando os ícones");
        if (g_tray) SetWindowPos(g_tray, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        AvisarApps();
    }
    if (g_tray && FindWindowW(L"Shell_TrayWnd", nullptr) != g_tray)
        SetWindowPos(g_tray, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

static void SendIcon(const TrayIcon& c, UINT ev, POINT pt) {
    WPARAM w = c.version >= 4 ? MAKEWPARAM(pt.x, pt.y) : c.uid;
    LPARAM l = c.version >= 4 ? MAKELPARAM(ev, c.uid) : ev;
    SendNotifyMessageW(c.hwnd, c.callback, w, l);
}

void tray::Click(int i, UINT button, POINT pt) {
    if (i < 0 || i >= (int)icons.size()) return;
    TrayIcon c = icons[i];
    if (!IsWindow(c.hwnd)) { icons.erase(icons.begin() + i); if (g_changed) g_changed(); return; }
    DWORD pid = 0; GetWindowThreadProcessId(c.hwnd, &pid);
    AllowSetForegroundWindow(pid);
    bool left = button == WM_LBUTTONUP;
    SendIcon(c, left ? WM_LBUTTONDOWN : WM_RBUTTONDOWN, pt);
    SendIcon(c, button, pt);
    if (c.version >= 3) SendIcon(c, left ? NIN_SELECT : WM_CONTEXTMENU, pt);
}
