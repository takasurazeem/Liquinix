// Encaixe nas bordas, como no macOS Sequoia: arrastar uma janela até a borda esquerda/direita da tela = metade;
// até um canto = um quarto; até a barra de menus = preencher. Enquanto arrasta aparece a prévia do Mac (um
// retângulo arredondado translúcido atrás da janela) onde ela vai ficar; ao soltar ela vai para lá. Arrastar uma
// janela encaixada a devolve ao tamanho de antes.
// É o Shell quem faz, para todas as janelas (antes cada app com a WacOSWindow.dll fazia o seu, e o encaixe do
// próprio Windows brigava com ele: a janela não ficava na metade e a prévia não aparecia). O encaixe do Windows
// fica desligado só enquanto o Shell roda (SPI_SETWINARRANGING sem gravar no registro: ao sair, ou ao reiniciar o
// PC, volta como estava). O interruptor do Windows ("Ajustar janelas", HKCU\Control Panel\Desktop\
// WindowArrangementActive) liga e desliga o nosso; Ajustes > Mesa e Dock mexe nele.
// Janela com a nossa barra (WacOSWindow.dll): o Shell manda "MacJanelaEncaixar" e a DLL encaixa (ela sabe as
// bordas invisíveis e guarda o tamanho de antes); as outras o Shell move direto.
#include "shell.h"
#include <dwmapi.h>
#include <algorithm>
#include <cmath>
#include <map>

namespace {

HWINEVENTHOOK g_ev;
HWND g_prev;               // a prévia
UINT_PTR g_timer;
HWND g_drag;               // janela sendo arrastada (0 = nenhuma)
RECT g_start{};            // retângulo dela no começo do arrasto
int g_kind;                // alvo mostrado agora: 0 nada, 1 retângulo, 2 preencher
RECT g_target{};           // retângulo visível do alvo
BYTE g_alpha;              // opacidade atual da prévia (aparece suave)
bool g_turnedOff;          // o Shell desligou o encaixe do Windows (devolve ao sair)
BOOL g_winBefore = TRUE;
std::map<HWND, RECT> g_before;  // janelas sem a DLL encaixadas por nós: o retângulo de antes
std::map<HWND, RECT> g_tiled;   // ...e o retângulo em que ficaram

bool Enabled() {
    wchar_t v[8] = L"1"; DWORD n = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"WindowArrangementActive", RRF_RT_REG_SZ, nullptr, v, &n);
    return v[0] != L'0';
}

// o encaixe do Windows desligado só nesta sessão (sem gravar): com ele ligado, o Windows também agia na borda
void SilenceWindows() {
    BOOL cur = TRUE;
    SystemParametersInfoW(SPI_GETWINARRANGING, 0, &cur, 0);
    if (!cur) return;
    if (!g_turnedOff) { g_winBefore = cur; g_turnedOff = true; }
    SystemParametersInfoW(SPI_SETWINARRANGING, FALSE, nullptr, 0);
}

RECT Visible(HWND h, RECT& wr) {
    GetWindowRect(h, &wr);
    RECT vis = wr, box;
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &vis, sizeof(vis)))) vis = wr;
    if (GetPropW(h, L"MacJanela") && GetWindowRgnBox(h, &box) != ERROR && box.right > box.left)
        vis = { wr.left + box.left, wr.top + box.top, wr.left + box.right, wr.top + box.bottom };
    return vis;
}

// alvo pelo ponteiro: 0 nada, 1 metade/quarto em *t, 2 preencher (a área de trabalho)
int Target(HWND h, RECT* t) {
    POINT c; GetCursorPos(&c);
    MONITORINFO mi{ sizeof(mi) };
    if (!GetMonitorInfoW(MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST), &mi)) return 0;
    RECT m = mi.rcMonitor, wa = mi.rcWork;
    float sc = ui::Scale(h);
    int e = (int)(4 * sc), q = (int)(60 * sc);
    bool L = c.x <= m.left + e, R = c.x >= m.right - 1 - e, T = c.y <= std::max<LONG>(m.top + e, wa.top);
    LONG mx = (wa.left + wa.right) / 2, my = (wa.top + wa.bottom) / 2;
    if (L || R) {
        *t = L ? RECT{ wa.left, wa.top, mx, wa.bottom } : RECT{ mx, wa.top, wa.right, wa.bottom };
        if (c.y <= wa.top + q) t->bottom = my; else if (c.y >= wa.bottom - q) t->top = my;
        return 1;
    }
    if (T) { *t = wa; return 2; }
    return 0;
}

LRESULT CALLBACK PrevProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(h, m, w, l);
}

// a prévia: retângulo arredondado translúcido com borda clara e um contorno escuro fino (aparece também em
// papel de parede claro), com margem em volta, como no Sequoia
void Draw(RECT t, HWND h) {
    float sc = ui::Scale(h);
    int g = (int)(6 * sc);
    InflateRect(&t, -g, -g);
    int W = t.right - t.left, H = t.bottom - t.top;
    if (W <= 0 || H <= 0) return;
    HDC mem = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader = { sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) { DeleteDC(mem); return; }
    auto* px = (UINT32*)bits;
    float r = 14 * sc, hw = W / 2.f, hh = H / 2.f;
    auto cov = [](float d) { return std::clamp(0.5f - d, 0.f, 1.f); };
    const float fillA = 0.26f, lineA = 0.70f, darkA = 0.22f;
    float lw = std::max(1.f, sc);
    for (int y = 0; y < H; y++) {
        float qy = std::fabs(y + 0.5f - hh) - (hh - r);
        bool edgeY = qy > -2 * lw - 2;
        for (int x = 0; x < W; x++) {
            float qx = std::fabs(x + 0.5f - hw) - (hw - r);
            UINT32 out;
            if (!edgeY && qx <= -2 * lw - 2) {  // miolo: só o preenchimento (rápido)
                BYTE v = (BYTE)std::lround(fillA * 255);
                out = (UINT32)v << 24 | (UINT32)v << 16 | (UINT32)v << 8 | v;
            } else {
                float d = std::hypot(std::max(qx, 0.f), std::max(qy, 0.f)) + std::min(std::max(qx, qy), 0.f) - r;
                float outer = cov(d), inner1 = cov(d + lw), inner2 = cov(d + 2 * lw);
                float dark = outer - inner1, light = inner1 - inner2, fill = inner2;
                float a = dark * darkA + light * lineA + fill * fillA;              // alfa total
                float white = light * lineA + fill * fillA;                         // parte branca (pré-multiplicada)
                BYTE A = (BYTE)std::lround(a * 255), V = (BYTE)std::lround(white * 255);
                out = (UINT32)A << 24 | (UINT32)V << 16 | (UINT32)V << 8 | V;
            }
            px[y * W + x] = out;
        }
    }
    HGDIOBJ old = SelectObject(mem, bmp);
    POINT src{ 0, 0 }, dst{ t.left, t.top };
    SIZE sz{ W, H };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, g_alpha, AC_SRC_ALPHA };
    UpdateLayeredWindow(g_prev, nullptr, &dst, &sz, mem, &src, 0, &bf, ULW_ALPHA);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

void Hide() {
    g_kind = 0;
    g_alpha = 0;
    if (g_prev) ShowWindow(g_prev, SW_HIDE);
}

void Show(int kind, RECT t) {
    if (kind == g_kind && EqualRect(&t, &g_target)) {
        if (g_alpha < 255) {  // aparecendo: ~100 ms
            g_alpha = (BYTE)std::min(255, g_alpha + 50);
            BLENDFUNCTION bf{ AC_SRC_OVER, 0, g_alpha, AC_SRC_ALPHA };
            UpdateLayeredWindow(g_prev, nullptr, nullptr, nullptr, nullptr, nullptr, 0, &bf, ULW_ALPHA);
        }
        return;
    }
    if (!g_kind) g_alpha = 60;
    g_kind = kind; g_target = t;
    Draw(t, g_drag);
    // logo atrás da janela arrastada (acima das outras)
    SetWindowPos(g_prev, g_drag, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void Place(HWND h, RECT want) {
    if (IsZoomed(h)) ShowWindow(h, SW_RESTORE);
    RECT wr, vis = Visible(h, wr);
    RECT r{ want.left - (vis.left - wr.left), want.top - (vis.top - wr.top), want.right + (wr.right - vis.right), want.bottom + (wr.bottom - vis.bottom) };
    SetWindowPos(h, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
}

UINT TileMsg() { static UINT m = RegisterWindowMessageW(L"MacJanelaEncaixar"); return m; }

// manda a janela para o alvo (kind 1: retângulo visível t; 2: preencher)
void Apply(HWND h, int kind, RECT t, RECT start) {
    if (GetPropW(h, L"MacJanela")) {  // a DLL encaixa (bordas invisíveis, tamanho de antes, "ampliar" animado)
        if (kind == 2) PostMessageW(h, TileMsg(), 0, 0);
        else PostMessageW(h, TileMsg(), MAKEWPARAM((WORD)(short)t.right, (WORD)(short)t.bottom), MAKELPARAM((WORD)(short)t.left, (WORD)(short)t.top));
        return;
    }
    if (!g_before.count(h)) g_before[h] = start;
    Place(h, t);
    RECT now; GetWindowRect(h, &now);
    g_tiled[h] = now;
}

void Tick() {
    if (!g_drag || !IsWindow(g_drag)) { Hide(); return; }
    RECT t; int k = Target(g_drag, &t);
    if (k) Show(k, t); else if (g_kind) Hide();
}

void CALLBACK TimerProc(HWND, UINT, UINT_PTR, DWORD) { Tick(); }

bool Candidate(HWND h) {
    if (!h || GetAncestor(h, GA_ROOT) != h || shell::Nossa(h) || apps::IsShellWindow(h)) return false;
    LONG st = GetWindowLongW(h, GWL_STYLE), ex = GetWindowLongW(h, GWL_EXSTYLE);
    if (!(st & WS_THICKFRAME) || (st & WS_CHILD) || (ex & WS_EX_NOACTIVATE)) return false;
    return true;
}

void CALLBACK OnEvent(HWINEVENTHOOK, DWORD ev, HWND h, LONG obj, LONG, DWORD, DWORD) {
    if (obj != OBJID_WINDOW) return;
    if (ev == EVENT_SYSTEM_MOVESIZESTART) {
        if (!Candidate(h) || !Enabled()) return;
        SilenceWindows();  // (o Ajustes pode ter religado)
        // arrastar ou redimensionar? o ponto em que o botão desceu diz (borda = redimensionar)
        POINT c; GetCursorPos(&c);
        DWORD_PTR hit = HTCAPTION;
        SendMessageTimeoutW(h, WM_NCHITTEST, 0, MAKELPARAM((WORD)(short)c.x, (WORD)(short)c.y), SMTO_ABORTIFHUNG, 60, &hit);
        if (hit >= HTLEFT && hit <= HTBOTTOMRIGHT) return;
        g_drag = h;
        GetWindowRect(h, &g_start);
        Hide();
        g_timer = SetTimer(nullptr, 0, 15, TimerProc);
    } else if (ev == EVENT_SYSTEM_MOVESIZEEND) {
        if (h != g_drag) return;
        if (g_timer) { KillTimer(nullptr, g_timer); g_timer = 0; }
        HWND d = g_drag; g_drag = nullptr;
        Hide();
        if (!IsWindow(d)) return;
        RECT t; int k = Target(d, &t);
        RECT now; GetWindowRect(d, &now);
        bool hasDll = GetPropW(d, L"MacJanela") != nullptr;
        if (!hasDll && (now.right - now.left != g_start.right - g_start.left || now.bottom - now.top != g_start.bottom - g_start.top)) return;  // redimensionou
        if (k) {
            if (k == 2 && !hasDll) {
                MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(MonitorFromRect(&t, MONITOR_DEFAULTTONEAREST), &mi);
                t = mi.rcWork;
            }
            Log(L"encaixe: %ls (%ld,%ld %ldx%ld)", k == 2 ? L"preencher" : L"borda", t.left, t.top, t.right - t.left, t.bottom - t.top);
            Apply(d, k, t, g_start);
            return;
        }
        // sem a DLL: arrastou uma janela encaixada por nós para longe da borda -> volta ao tamanho de antes,
        // no mesmo ponto do ponteiro (a DLL faz isso sozinha durante o arrasto)
        auto b = g_before.find(d);
        auto tl = g_tiled.find(d);
        if (!hasDll && b != g_before.end() && tl != g_tiled.end() &&
            g_start.right - g_start.left == tl->second.right - tl->second.left && g_start.bottom - g_start.top == tl->second.bottom - tl->second.top) {
            POINT c; GetCursorPos(&c);
            int bw = b->second.right - b->second.left, bh = b->second.bottom - b->second.top;
            float fx = (c.x - now.left) / (float)std::max<LONG>(now.right - now.left, 1);
            int left = c.x - (int)(fx * bw);
            SetWindowPos(d, nullptr, left, now.top, bw, bh, SWP_NOZORDER | SWP_NOACTIVATE);
            g_before.erase(b); g_tiled.erase(d);
        }
    }
}

}  // namespace

void encaixe::Start() {
    if (!g_prev) {
        WNDCLASSW wc{}; wc.lpfnWndProc = PrevProc; wc.hInstance = shell::inst; wc.lpszClassName = L"ShellEncaixePrevia";
        RegisterClassW(&wc);
        g_prev = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"",
                                 WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, shell::inst, nullptr);
        if (g_prev) apps::ours.push_back(g_prev);
    }
    if (Enabled()) SilenceWindows();
    if (!g_ev) g_ev = SetWinEventHook(EVENT_SYSTEM_MOVESIZESTART, EVENT_SYSTEM_MOVESIZEEND, nullptr, OnEvent, 0, 0, WINEVENT_OUTOFCONTEXT | shell::SemProprio());
    Log(L"encaixe: ligado (encaixe do Windows %ls nesta sessão)", g_turnedOff ? L"desligado" : L"já estava desligado");
}

void encaixe::Stop() {
    if (g_ev) { UnhookWinEvent(g_ev); g_ev = nullptr; }
    if (g_timer) { KillTimer(nullptr, g_timer); g_timer = 0; }
    Hide();
    // o encaixe do Windows volta como estava (sem gravar: o registro nunca mudou)
    if (g_turnedOff) { SystemParametersInfoW(SPI_SETWINARRANGING, g_winBefore, nullptr, 0); g_turnedOff = false; }
}

// Win+← / Win+→ (sem o encaixe do Windows): metade esquerda/direita da tela do monitor da janela
void encaixe::Half(HWND h, bool right) {
    if (!h || !IsWindow(h)) return;
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
    RECT a = mi.rcWork, s; GetWindowRect(h, &s);
    LONG cx = (a.left + a.right) / 2;
    RECT t = right ? RECT{ cx, a.top, a.right, a.bottom } : RECT{ a.left, a.top, cx, a.bottom };
    Apply(h, 1, t, s);
}
