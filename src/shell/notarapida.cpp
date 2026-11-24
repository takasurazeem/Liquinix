// Canto ativo da Nota Rápida, como no Mac (padrão do macOS: canto inferior direito da tela).
// Encostar o ponteiro no canto faz um quadrado translúcido subir um pouco do canto; com o ponteiro em cima,
// ele sobe mais um pouco; clicar abre a Nota Rápida (Notas.exe --nota-rapida). Afastar o ponteiro esconde.
// A imagem é desenhada uma vez numa janela parada no canto; o deslize é animado pelo próprio compositor
// (DirectComposition), não por quadros do Shell, então fica liso mesmo com o Shell ocupado.
// Desligar: `canto_nota_rapida=0` em %APPDATA%\Shell\ajustes.txt.
#include "shell.h"
#include <shellapi.h>
#include <windowsx.h>
#include <cmath>
#include <algorithm>

namespace {

const float TILE = 84, SHADOW = 18;              // quadrado (pt) e folga da sombra
const float SHOWN = 30, HOVER = 44;               // quanto do quadrado aparece (pt): parado / com o ponteiro em cima
const UINT_PTR T_POLL = 1, T_HIDE = 2;
HWND g_w;
ui::GpuSurface g_s;
bool g_on, g_hover, g_drawnDark, g_drawn;
double g_animAt;
float g_from, g_to, g_vis;    // pt visíveis do quadrado (0 = escondido)
DWORD g_cornerSince, g_awaySince;
RECT g_mon;
float g_sc = 1;

bool Enabled() { return shell::Setting(L"canto_nota_rapida", L"1") != L"0" && cantos::NotaRapida(); }  // o canto pode ter outra ação (Cantos Ativos)

bool FullscreenInFront(const RECT& mon) {  // jogo/vídeo em tela cheia: o canto não atrapalha
    HWND fg = GetForegroundWindow();
    if (!fg) return false;
    wchar_t cls[64] = {}; GetClassNameW(fg, cls, 64);
    if (mesa::IsDeskClass(cls)) return false;
    RECT r; GetWindowRect(fg, &r);
    return r.left <= mon.left && r.top <= mon.top && r.right >= mon.right && r.bottom >= mon.bottom && !(GetWindowLongW(fg, GWL_STYLE) & WS_CAPTION);
}

int WinPx() { return (int)std::lround((TILE + SHADOW * 2) * g_sc); }

// desenha o quadrado uma vez (muda só com o tema ou a escala)
void Draw() {
    bool dark = ui::Dark();
    int px = WinPx();
    auto* rt = ui::GpuBegin(g_s, px, px, g_sc);
    if (!rt) return;
    ID2D1SolidColorBrush* br = nullptr;
    rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &br);
    D2D1_RECT_F r{ SHADOW, SHADOW, SHADOW + TILE, SHADOW + TILE };
    for (int k = 10; k >= 1; k--) {  // sombra suave
        br->SetColor(D2D1::ColorF(0, 0, 0, 0.018f));
        rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left - k, r.top - k + 2, r.right + k, r.bottom + k + 2), 12.f + k, 12.f + k), br);
    }
    br->SetColor(dark ? D2D1::ColorF(0.30f, 0.30f, 0.32f, 0.55f) : D2D1::ColorF(1, 1, 1, 0.55f));  // vidro translúcido
    rt->FillRoundedRectangle(D2D1::RoundedRect(r, 12, 12), br);
    br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.28f) : D2D1::ColorF(1, 1, 1, 0.85f));  // aro claro, como o vidro do Mac
    rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), 11.5f, 11.5f), br, 1);
    br->SetColor(D2D1::ColorF(0, 0, 0, dark ? 0.35f : 0.10f));
    rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left - 0.5f, r.top - 0.5f, r.right + 0.5f, r.bottom + 0.5f), 12.5f, 12.5f), br, 1);
    br->Release();
    ui::GpuEnd(g_s);
    g_drawn = true; g_drawnDark = dark;
}

// a janela cobre o canto (HOVER + sombra); o quadrado entra nela vindo de fora, pela diagonal
int BoxPx() { return (int)std::lround((SHADOW + HOVER) * g_sc); }
float OffsetFor(float vis) { return (HOVER - vis + (vis <= 0 ? SHADOW : 0)) * g_sc; }  // escondido: a sombra também fica fora  // deslocamento do conteúdo (px) para `vis` pt à mostra

// onde o quadrado está agora (a animação corre no compositor; aqui só a mesma conta, para emendar outra)
float CurrentVis() {
    if (!g_animAt) return g_vis;
    float dur = g_to > g_from ? 240.f : 170.f;
    float k = (float)std::min(1.0, (anim::Now() - g_animAt) / dur);
    float e = 1 - (1 - k) * (1 - k) * (1 - k);
    return g_from + (g_to - g_from) * e;
}

void AnimateTo(float vis) {
    if (vis == g_to && g_animAt) return;
    g_from = CurrentVis(); g_to = vis; g_animAt = anim::Now();
    float dur = g_to > g_from ? 240.f : 170.f;
    ui::GpuSlide(g_s, OffsetFor(g_from), OffsetFor(g_from), OffsetFor(g_to), OffsetFor(g_to), dur);
    if (vis == 0) SetTimer(g_w, T_HIDE, (UINT)dur + 30, nullptr);
    else KillTimer(g_w, T_HIDE);
}

void Show(const RECT& mon, HMONITOR hm) {
    g_mon = mon;
    float sc = GetDpiForSystem() / 96.f;
    using F = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
    static F f = (F)GetProcAddress(LoadLibraryW(L"shcore.dll"), "GetDpiForMonitor");
    UINT dx = 96, dy = 96;
    if (f && SUCCEEDED(f(hm, 0, &dx, &dy))) sc = dx / 96.f;
    int box = (int)std::lround((SHADOW + HOVER) * sc);
    SetWindowPos(g_w, HWND_TOPMOST, mon.right - box, mon.bottom - box, box, box, SWP_NOACTIVATE);
    if (!g_drawn || sc != g_sc || g_drawnDark != ui::Dark()) { g_sc = sc; Draw(); }
    g_on = true; g_hover = false; g_awaySince = 0; g_vis = 0; g_from = g_to = 0; g_animAt = 0;
    ui::GpuOffset(g_s, OffsetFor(0), OffsetFor(0));
    ShowWindow(g_w, SW_SHOWNOACTIVATE);
    AnimateTo(SHOWN);
}
void Hide() { if (!g_on) return; g_on = false; g_hover = false; AnimateTo(0); }

void OpenQuickNote() {
    std::wstring exe = shell::dir + L"\\" EXE_NOTES;
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) { Log(L"nota rápida: " EXE_NOTES L" não encontrado"); return; }
    AllowSetForegroundWindow(ASFW_ANY);
    ShellExecuteW(nullptr, L"open", exe.c_str(), L"--nota-rapida", shell::dir.c_str(), SW_SHOWNORMAL);
    Log(L"nota rápida: aberta pelo canto");
}

void Poll() {
    POINT p; if (!GetCursorPos(&p)) return;
    HMONITOR hm = MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(hm, &mi);
    const RECT& m = mi.rcMonitor;
    bool corner = p.x >= m.right - 3 && p.y >= m.bottom - 3;
    bool button = GetAsyncKeyState(VK_LBUTTON) < 0 || GetAsyncKeyState(VK_RBUTTON) < 0;
    DWORD now = GetTickCount();
    if (!g_on) {
        if (corner && !button && Enabled() && !FullscreenInFront(m)) {
            if (!g_cornerSince) g_cornerSince = now;
            if (now - g_cornerSince >= 60) { g_cornerSince = 0; Show(m, hm); }  // um instante no canto (não dispara de passagem)
        } else g_cornerSince = 0;
        return;
    }
    if (!EqualRect(&m, &g_mon)) { Hide(); return; }
    int tile = (int)std::lround(HOVER * g_sc), pad = (int)std::lround(28 * g_sc);
    bool over = p.x >= g_mon.right - tile && p.y >= g_mon.bottom - tile;
    bool near_ = p.x >= g_mon.right - tile - pad && p.y >= g_mon.bottom - tile - pad;
    if (over != g_hover) { g_hover = over; AnimateTo(over ? HOVER : SHOWN); }
    if (near_) g_awaySince = 0;
    else if (!g_awaySince) g_awaySince = now;
    else if (now - g_awaySince > 300) Hide();
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_TIMER:
        if (w == T_POLL) Poll();
        else if (w == T_HIDE) { KillTimer(h, T_HIDE); if (!g_on) { ShowWindow(h, SW_HIDE); g_animAt = 0; g_vis = 0; } }
        return 0;
    case WM_NCHITTEST: {  // só o pedaço do quadrado à mostra recebe o clique
        POINT p{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        int vis = (int)std::lround(CurrentVis() * g_sc);
        return g_on && p.x >= g_mon.right - vis && p.y >= g_mon.bottom - vis ? HTCLIENT : HTTRANSPARENT;
    }
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_SETCURSOR: SetCursor(LoadCursorW(nullptr, IDC_ARROW)); return TRUE;
    case WM_LBUTTONUP:
        if (g_on) { OpenQuickNote(); g_on = false; g_hover = false; AnimateTo(0); }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

namespace notarapida {
void Create(HINSTANCE hi) {
    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = hi; wc.lpszClassName = L"ShellNotaRapida";
    RegisterClassW(&wc);
    g_w = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_POPUP,
                          0, 0, 1, 1, nullptr, nullptr, hi, nullptr);
    if (!g_w) { Log(L"nota rápida: sem janela (%lu)", GetLastError()); return; }
    g_s.hwnd = g_w;
    SetTimer(g_w, T_POLL, 30, nullptr);  // só lê a posição do ponteiro: barato
    Log(L"nota rápida: canto inferior direito %ls", Enabled() ? L"ligado" : L"desligado");
}
}
