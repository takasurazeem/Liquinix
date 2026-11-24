// BandejaTeste: protótipo da bandeja (área de notificação) do nosso shell.
// Cria um Shell_TrayWnd invisível acima do do Explorador, recebe os Shell_NotifyIcon dos apps,
// repassa tudo ao Explorador (a barra do Windows continua igual) e mostra os ícones numa faixa
// no estilo da barra de menus do Mac, no canto superior direito. Clique esquerdo/direito vai
// para o app. Fechar (Esc ou botão direito na faixa vazia) desfaz tudo. Ver docs/estudo-bandeja.md.
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <cstdio>
#include <string>
#include <vector>
#include <algorithm>

// Layout fixo de 32 bits que o shell32 manda dentro do WM_COPYDATA (mesmo de apps 64 bits).
#pragma pack(push, 4)
struct NotifyIconData32 {
    DWORD cbSize;
    DWORD hWnd;
    UINT uID;
    UINT uFlags;
    UINT uCallbackMessage;
    DWORD hIcon;
    WCHAR szTip[128];
    DWORD dwState;
    DWORD dwStateMask;
    WCHAR szInfo[256];
    UINT uVersion;  // união com uTimeout
    WCHAR szInfoTitle[64];
    DWORD dwInfoFlags;
    GUID guidItem;
    DWORD hBalloonIcon;
};
struct ShellTrayData {
    DWORD dwSignature;  // 0x34753423
    DWORD dwMessage;    // NIM_*
    NotifyIconData32 nid;
};
#pragma pack(pop)

struct Icon {
    HWND hwnd = nullptr;
    UINT uid = 0;
    GUID guid{};
    UINT callback = 0;
    UINT version = 0;
    HICON icon = nullptr;  // cópia nossa
    std::wstring tip;
    bool hidden = false;
};

static std::vector<Icon> g_icons;
static HWND g_tray, g_notify, g_bar;
static UINT WM_TASKBARCREATED;
static FILE* g_log;
static int g_hover = -1;
const int ICON = 16, CELL = 28, BAR_H = 26;

static void Log(const wchar_t* fmt, ...) {
    if (!g_log) return;
    va_list a; va_start(a, fmt);
    vfwprintf(g_log, fmt, a);
    va_end(a);
    fputwc(L'\n', g_log);
    fflush(g_log);
}

// Shell_TrayWnd do Explorador (qualquer um que não seja o nosso).
static HWND ExplorerTray() {
    HWND h = nullptr;
    while ((h = FindWindowExW(nullptr, h, L"Shell_TrayWnd", nullptr)))
        if (h != g_tray) return h;
    return nullptr;
}

static int Find(const NotifyIconData32& n) {
    for (int i = 0; i < (int)g_icons.size(); i++) {
        const Icon& c = g_icons[i];
        if ((n.uFlags & NIF_GUID) && c.guid == n.guidItem && n.guidItem != GUID{}) return i;
        if (c.hwnd == (HWND)(ULONG_PTR)n.hWnd && c.uid == n.uID) return i;
    }
    return -1;
}

static void Apply(Icon& c, const NotifyIconData32& n) {
    if (n.uFlags & NIF_MESSAGE) c.callback = n.uCallbackMessage;
    if (n.uFlags & NIF_ICON) {
        if (c.icon) DestroyIcon(c.icon);
        c.icon = n.hIcon ? CopyIcon((HICON)(ULONG_PTR)n.hIcon) : nullptr;
    }
    if (n.uFlags & NIF_TIP) c.tip = n.szTip;
    if (n.uFlags & NIF_STATE) {
        if (n.dwStateMask & NIS_HIDDEN) c.hidden = (n.dwState & NIS_HIDDEN) != 0;
    }
    if (n.uFlags & NIF_GUID) c.guid = n.guidItem;
}

static std::vector<int> Visible() {
    std::vector<int> v;
    for (int i = 0; i < (int)g_icons.size(); i++) if (!g_icons[i].hidden && g_icons[i].icon) v.push_back(i);
    return v;
}

static void LayoutBar() {
    if (!g_bar) return;
    int n = (int)Visible().size();
    int w = std::max(1, n) * CELL + 16;
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    SetWindowPos(g_bar, HWND_TOPMOST, wa.right - w - 8, wa.top + 6, w, BAR_H, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(g_bar, nullptr, TRUE);
}

static BOOL OnNotify(DWORD msg, const NotifyIconData32& n) {
    int i = Find(n);
    switch (msg) {
    case NIM_ADD:
        if (i >= 0) { Apply(g_icons[i], n); break; }  // já tinha (reenvio após TaskbarCreated)
        { Icon c; c.hwnd = (HWND)(ULONG_PTR)n.hWnd; c.uid = n.uID; Apply(c, n); g_icons.push_back(c); }
        Log(L"+ %ls (hwnd %08X id %u)", n.szTip, n.hWnd, n.uID);
        break;
    case NIM_MODIFY:
        if (i < 0) return FALSE;
        Apply(g_icons[i], n);
        break;
    case NIM_DELETE:
        if (i < 0) return FALSE;
        Log(L"- %ls", g_icons[i].tip.c_str());
        if (g_icons[i].icon) DestroyIcon(g_icons[i].icon);
        g_icons.erase(g_icons.begin() + i);
        break;
    case NIM_SETVERSION:
        if (i < 0) return FALSE;
        g_icons[i].version = n.uVersion;
        break;
    case NIM_SETFOCUS:
        break;
    default:
        return FALSE;
    }
    LayoutBar();
    return TRUE;
}

// Tudo que chega ao nosso Shell_TrayWnd também vai para o do Explorador.
static LRESULT Forward(UINT m, WPARAM w, LPARAM l) {
    HWND ex = ExplorerTray();
    return ex ? SendMessageW(ex, m, w, l) : 0;
}

static LRESULT CALLBACK TrayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_COPYDATA && l) {
        auto* cd = (COPYDATASTRUCT*)l;
        if (cd->dwData == 1 && cd->cbData >= sizeof(DWORD) * 2 + offsetof(NotifyIconData32, szTip)) {
            auto* td = (ShellTrayData*)cd->lpData;
            NotifyIconData32 n{};
            memcpy(&n, &td->nid, std::min<size_t>(sizeof(n), cd->cbData - sizeof(DWORD) * 2));
            BOOL ours = OnNotify(td->dwMessage, n);
            LRESULT theirs = Forward(m, w, l);  // a barra do Windows continua com os ícones
            return ours || theirs;
        }
        return Forward(m, w, l);  // AppBar (0), GetRect (3)...
    }
    if (m == WM_WINDOWPOSCHANGED) {  // nunca aparece
        auto* wp = (WINDOWPOS*)l;
        if (wp->flags & SWP_SHOWWINDOW) ShowWindow(h, SW_HIDE);
    }
    if (m >= WM_USER || m == WM_COMMAND || m == WM_ACTIVATEAPP) return Forward(m, w, l);
    return DefWindowProcW(h, m, w, l);
}

// Manda um evento de mouse para o app dono do ícone, do jeito que o Explorador manda.
static void SendIcon(const Icon& c, UINT ev, POINT pt) {
    WPARAM w = c.version >= 4 ? MAKEWPARAM(pt.x, pt.y) : c.uid;
    LPARAM l = c.version >= 4 ? MAKELPARAM(ev, c.uid) : ev;
    SendNotifyMessageW(c.hwnd, c.callback, w, l);
}

static int HitBar(int x) {
    auto v = Visible();
    int k = (x - 8) / CELL;
    return (x >= 8 && k >= 0 && k < (int)v.size()) ? v[k] : -1;
}

static LRESULT CALLBACK BarProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        HBRUSH bg = CreateSolidBrush(RGB(40, 40, 42));
        FillRect(dc, &rc, bg);
        DeleteObject(bg);
        auto v = Visible();
        for (int k = 0; k < (int)v.size(); k++) {
            int x = 8 + k * CELL;
            if (v[k] == g_hover) {
                HBRUSH hb = CreateSolidBrush(RGB(70, 70, 74));
                RECT r{ x + 1, 2, x + CELL - 1, BAR_H - 2 };
                FillRect(dc, &r, hb);
                DeleteObject(hb);
            }
            DrawIconEx(dc, x + (CELL - ICON) / 2, (BAR_H - ICON) / 2, g_icons[v[k]].icon, ICON, ICON, 0, nullptr, DI_NORMAL);
        }
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int i = HitBar(GET_X_LPARAM(l));
        if (i != g_hover) {
            g_hover = i;
            InvalidateRect(h, nullptr, FALSE);
            TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
            std::wstring title = i >= 0 ? g_icons[i].tip : L"Bandeja (teste)";
            SetWindowTextW(h, title.c_str());
        }
        if (i >= 0) { POINT pt; GetCursorPos(&pt); SendIcon(g_icons[i], WM_MOUSEMOVE, pt); }
        return 0;
    }
    case WM_MOUSELEAVE: g_hover = -1; InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: {
        int i = HitBar(GET_X_LPARAM(l));
        if (i < 0) { if (m == WM_RBUTTONUP) DestroyWindow(h); return 0; }  // direito no vazio: fecha
        Icon c = g_icons[i];
        if (!IsWindow(c.hwnd)) { g_icons.erase(g_icons.begin() + i); LayoutBar(); return 0; }
        DWORD pid = 0; GetWindowThreadProcessId(c.hwnd, &pid);
        AllowSetForegroundWindow(pid);  // o menu do app precisa poder vir para frente
        POINT pt; GetCursorPos(&pt);
        SendIcon(c, m, pt);
        if (m == WM_LBUTTONUP && c.version >= 3) SendIcon(c, NIN_SELECT, pt);
        if (m == WM_RBUTTONUP && c.version >= 3) SendIcon(c, WM_CONTEXTMENU, pt);
        Log(L"clique %X em %ls (versão %u)", m, c.tip.c_str(), c.version);
        return 0;
    }
    case WM_KEYDOWN: if (w == VK_ESCAPE) DestroyWindow(h); return 0;
    case WM_TIMER: {  // mantém o nosso Shell_TrayWnd acima do do Explorador
        if (FindWindowW(L"Shell_TrayWnd", nullptr) != g_tray)
            SetWindowPos(g_tray, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring logPath = exe; logPath = logPath.substr(0, logPath.find_last_of(L'\\')) + L"\\bandeja.log";
    _wfopen_s(&g_log, logPath.c_str(), L"w, ccs=UTF-8");
    WM_TASKBARCREATED = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW tc{}; tc.lpfnWndProc = TrayProc; tc.hInstance = hi; tc.lpszClassName = L"Shell_TrayWnd";
    RegisterClassW(&tc);
    tc.lpszClassName = L"TrayNotifyWnd";
    RegisterClassW(&tc);
    // invisível, topmost, no mesmo lugar da barra do Windows (quem pergunta a posição dela não se perde)
    RECT tb{ 0, 0, 1, 1 };
    if (HWND ex = ExplorerTray()) GetWindowRect(ex, &tb);
    g_tray = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"Shell_TrayWnd", L"", WS_POPUP,
                             tb.left, tb.top, tb.right - tb.left, tb.bottom - tb.top, nullptr, nullptr, hi, nullptr);
    g_notify = CreateWindowExW(0, L"TrayNotifyWnd", nullptr, WS_CHILD, 0, 0, 1, 1, g_tray, nullptr, hi, nullptr);

    WNDCLASSW bc{}; bc.lpfnWndProc = BarProc; bc.hInstance = hi; bc.lpszClassName = L"BandejaTesteBarra";
    bc.hCursor = LoadCursorW(nullptr, IDC_ARROW); bc.style = CS_DBLCLKS;
    RegisterClassW(&bc);
    g_bar = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, bc.lpszClassName, L"Bandeja (teste)", WS_POPUP,
                            0, 0, 100, BAR_H, nullptr, nullptr, hi, nullptr);
    DWM_WINDOW_CORNER_PREFERENCE cp = DWMWCP_ROUND;
    DwmSetWindowAttribute(g_bar, DWMWA_WINDOW_CORNER_PREFERENCE, &cp, sizeof(cp));
    BOOL dark = TRUE; DwmSetWindowAttribute(g_bar, 20, &dark, sizeof(dark));
    LayoutBar();
    SetTimer(g_bar, 1, 100, nullptr);

    Log(L"Shell_TrayWnd próprio criado; pedindo aos apps que registrem os ícones de novo");
    SendNotifyMessageW(HWND_BROADCAST, WM_TASKBARCREATED, 0, 0);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }

    // desfaz: some com o nosso Shell_TrayWnd e pede aos apps que voltem a falar só com o Explorador
    DestroyWindow(g_tray);
    SendNotifyMessageW(HWND_BROADCAST, WM_TASKBARCREATED, 0, 0);
    Log(L"encerrado (%zu ícones)", g_icons.size());
    if (g_log) fclose(g_log);
    return 0;
}
