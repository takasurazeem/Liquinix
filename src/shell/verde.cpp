// Menu do botão verde (macOS Sequoia): parar o ponteiro sobre o botão verde de uma janela abre um menu para
// encaixar a janela (metades, quartos, centralizar, preencher). Quem percebe o ponteiro parado é a
// WacOSWindow.dll (janelas com a nossa barra) ou o próprio app nosso; eles mandam "ShellMenuVerde" com a
// janela e o ponto da tela. Os encaixes respeitam a área livre (sem passar da barra de menus nem do Dock).
#include "shell.h"
#include "../menu/context_menu.h"
#include <dwmapi.h>
#include <algorithm>

namespace {

HWND g_owner;

LRESULT CALLBACK OwnerProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    LRESULT r = 0;
    if (ctxmenu::ForwardOwnerMsg(m, w, l, &r)) return r;
    return DefWindowProcW(h, m, w, l);
}

// o retângulo visível (sem a borda invisível de redimensionar; com a nossa barra, a região da janela)
RECT Visible(HWND h, RECT& wr) {
    GetWindowRect(h, &wr);
    RECT vis = wr, box;
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &vis, sizeof(vis)))) vis = wr;
    if (GetPropW(h, L"MacJanela") && GetWindowRgnBox(h, &box) != ERROR && box.right > box.left)
        vis = { wr.left + box.left, wr.top + box.top, wr.left + box.right, wr.top + box.bottom };
    return vis;
}

void Place(HWND h, RECT want) {
    if (IsZoomed(h)) ShowWindow(h, SW_RESTORE);
    RECT wr, vis = Visible(h, wr);
    // a borda invisível volta em volta do retângulo pedido
    RECT r{ want.left - (vis.left - wr.left), want.top - (vis.top - wr.top), want.right + (wr.right - vis.right), want.bottom + (wr.bottom - vis.bottom) };
    SetWindowPos(h, nullptr, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOZORDER | SWP_NOACTIVATE);
    SetForegroundWindow(h);
}


// ================= Tahoe: o menu de tiling do macOS 26 (guia macOS26-Theme-Reference, seção 46, medido)
// Janela de vidro de 264 × 218, 13 de margem dos lados: "Mover e Redimensionar" (24) → 4 ícones de 54 × 44 →
// separador (11) → "Preencher e Organizar" (24) → 4 ícones → separador → "Tela Cheia ›" (24, com submenu).
// Com ⌥ as paletas trocam (quartos; Centralizar, Direita e Esquerda...). Aparece sem animação e some na hora.
namespace tiling {
const float W = 264, H = 218, M = 13, CELL_W = 54, CELL_H = 44, R = 12;
const float Y_H1 = 6, Y_P1 = 32, Y_S1 = 80, Y_H2 = 91, Y_P2 = 117, Y_S2 = 165, Y_FS = 182;
const UINT_PTR T_ALT = 1;
HWND g_wnd, g_back, g_target;
bool g_glass, g_alt, g_sub;
int g_hover = -1;  // 0..3 paleta 1, 4..7 paleta 2, 8 Tela Cheia
ui::GpuSurface g_s;

// área ocupada de cada ícone: retângulos em frações da tela (x0, y0, x1, y1); até 4 por ícone (Organizar)
struct Ico { const wchar_t* name; int id; float r[4][4]; int n; };
const Ico MOVE[4] = { { L"Esquerda", 10, { { 0, 0, .5f, 1 } }, 1 }, { L"Direita", 11, { { .5f, 0, 1, 1 } }, 1 },
                      { L"Superior", 12, { { 0, 0, 1, .5f } }, 1 }, { L"Inferior", 13, { { 0, .5f, 1, 1 } }, 1 } };
const Ico MOVE_ALT[4] = { { L"Superior Esquerda", 20, { { 0, 0, .5f, .5f } }, 1 }, { L"Superior Direita", 21, { { .5f, 0, 1, .5f } }, 1 },
                          { L"Inferior Esquerda", 22, { { 0, .5f, .5f, 1 } }, 1 }, { L"Inferior Direita", 23, { { .5f, .5f, 1, 1 } }, 1 } };
const Ico FILL[4] = { { L"Preencher", 1, { { 0, 0, 1, 1 } }, 1 },
                      { L"Esquerda e Direita", 30, { { 0, 0, .5f, 1 }, { .5f, 0, 1, 1 } }, 2 },
                      { L"Esquerda e Quartos", 31, { { 0, 0, .5f, 1 }, { .5f, 0, 1, .5f }, { .5f, .5f, 1, 1 } }, 3 },
                      { L"Quartos", 32, { { 0, 0, .5f, .5f }, { .5f, 0, 1, .5f }, { 0, .5f, .5f, 1 }, { .5f, .5f, 1, 1 } }, 4 } };
const Ico FILL_ALT[4] = { { L"Centralizar", 2, { { .2f, .15f, .8f, .85f } }, 1 },
                          { L"Direita e Esquerda", 33, { { .5f, 0, 1, 1 }, { 0, 0, .5f, 1 } }, 2 },
                          { L"Direita e Quartos", 34, { { .5f, 0, 1, 1 }, { 0, 0, .5f, .5f }, { 0, .5f, .5f, 1 } }, 3 },
                          { L"Quartos", 32, { { 0, 0, .5f, .5f }, { .5f, 0, 1, .5f }, { 0, .5f, .5f, 1 }, { .5f, .5f, 1, 1 } }, 4 } };

const Ico& IcoAt(int k) { return k < 4 ? (g_alt ? MOVE_ALT : MOVE)[k] : (g_alt ? FILL_ALT : FILL)[k - 4]; }
D2D1_RECT_F CellRect(int k) {
    float gap = (W - 2 * M - 4 * CELL_W) / 3, y = k < 4 ? Y_P1 : Y_P2;
    float x = M + (k % 4) * (CELL_W + gap);
    return D2D1::RectF(x, y, x + CELL_W, y + CELL_H);
}
D2D1_RECT_F FsRect() { return D2D1::RectF(5, Y_FS, W - 5, Y_FS + 24); }

void Paint() {
    if (!g_wnd || !IsWindowVisible(g_wnd)) return;
    float sc = ui::Scale(g_wnd);
    int wpx = (int)std::lround(W * sc), hpx = (int)std::lround(H * sc);
    auto* dc = ui::GpuBegin(g_s, wpx, hpx, sc);
    if (!dc) return;
    bool dark = ui::Dark();
    D2D1_COLOR_F text = dark ? D2D1::ColorF(1, 1, 1, 0.92f) : D2D1::ColorF(0, 0, 0, 0.88f);
    D2D1_COLOR_F text2 = dark ? D2D1::ColorF(1, 1, 1, 0.55f) : D2D1::ColorF(0, 0, 0, 0.50f);
    D2D1_COLOR_F accent = D2D1::ColorF(0.0f, 0.39f, 0.88f, 1);  // selectedContentBackground
    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(text, &br);
    if (!br) { ui::GpuEnd(g_s); return; }
    D2D1_RECT_F all = D2D1::RectF(0, 0, W, H);
    if (!g_glass) { br->SetColor(dark ? D2D1::ColorF(0.17f, 0.17f, 0.18f, 0.95f) : D2D1::ColorF(0.96f, 0.96f, 0.965f, 0.95f)); dc->FillRoundedRectangle(D2D1::RoundedRect(all, R, R), br); }
    liquid::Rim(dc, all, R, dark);
    IDWriteTextFormat* tf = ui::Format(13, DWRITE_FONT_WEIGHT_NORMAL);
    IDWriteTextFormat* th = ui::Format(13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    for (auto* f : { tf, th }) if (f) { f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); }
    auto txt = [&](const std::wstring& s, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c) {
        if (!f) return;
        br->SetColor(c);
        dc->DrawTextW(s.c_str(), (UINT32)s.size(), f, r, br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };
    // cabeçalhos: com o ponteiro num ícone, o nome dele aparece no lugar do título da seção (como no Mac)
    std::wstring h1 = g_hover >= 0 && g_hover < 4 ? Tr(IcoAt(g_hover).name) : Tr(L"Mover e Redimensionar");
    std::wstring h2 = g_hover >= 4 && g_hover < 8 ? Tr(IcoAt(g_hover).name) : Tr(L"Preencher e Organizar");
    txt(h1, th, D2D1::RectF(M + 1, Y_H1, W - M, Y_H1 + 24), text2);
    txt(h2, th, D2D1::RectF(M + 1, Y_H2, W - M, Y_H2 + 24), text2);
    for (int k = 0; k < 8; k++) {
        D2D1_RECT_F c = CellRect(k);
        bool hot = g_hover == k;
        if (hot) { br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.12f) : D2D1::ColorF(0, 0, 0, 0.07f)); dc->FillRoundedRectangle(D2D1::RoundedRect(c, 9, 9), br); }
        // a tela: retângulo arredondado de 34 × 24 no meio da célula, com a área ocupada cheia
        float cx = (c.left + c.right) / 2, cy = (c.top + c.bottom) / 2, sw = 34, sh = 24;
        D2D1_RECT_F s = D2D1::RectF(cx - sw / 2, cy - sh / 2, cx + sw / 2, cy + sh / 2);
        const Ico& ic = IcoAt(k);
        for (int i = 0; i < ic.n; i++) {
            const float* q = ic.r[i];
            D2D1_RECT_F a = D2D1::RectF(s.left + 2.5f + (sw - 5) * q[0], s.top + 2.5f + (sh - 5) * q[1], s.left + 2.5f + (sw - 5) * q[2], s.top + 2.5f + (sh - 5) * q[3]);
            a.left += 0.75f; a.top += 0.75f; a.right -= 0.75f; a.bottom -= 0.75f;
            br->SetColor(hot ? accent : (i == 0 ? text : text2));
            dc->FillRoundedRectangle(D2D1::RoundedRect(a, 2, 2), br);
        }
        br->SetColor(hot ? accent : text2);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(s, 5, 5), br, 1.2f);
    }
    auto sep = [&](float y) {
        br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.10f) : D2D1::ColorF(0, 0, 0, 0.10f));
        dc->FillRectangle(D2D1::RectF(M, y + 5, W - M, y + 5 + 1 / sc), br);
    };
    sep(Y_S1); sep(Y_S2);
    D2D1_RECT_F fs = FsRect();
    bool fhot = g_hover == 8 || g_sub;
    if (fhot) { br->SetColor(accent); dc->FillRoundedRectangle(D2D1::RoundedRect(fs, 8, 8), br); }
    txt(Tr(L"Tela Cheia"), tf, D2D1::RectF(M + 1, fs.top, W - M, fs.bottom), fhot ? D2D1::ColorF(1, 1, 1, 1) : text);
    {   // seta ›
        float x = W - M - 6, y = (fs.top + fs.bottom) / 2;
        br->SetColor(fhot ? D2D1::ColorF(1, 1, 1, 1) : text);
        dc->DrawLine({ x - 2, y - 4 }, { x + 2, y }, br, 1.5f);
        dc->DrawLine({ x + 2, y }, { x - 2, y + 4 }, br, 1.5f);
    }
    for (auto* f : { tf, th }) if (f) f->Release();
    br->Release();
    ui::GpuEnd(g_s);
    if (g_glass) {
        D2D1_COLOR_F tint = dark ? D2D1::ColorF(0.165f, 0.165f, 0.172f, 0.85f) : D2D1::ColorF(0.96f, 0.96f, 0.965f, 0.77f);  // vidro dos menus (seção 52)
        // é um menu: vidro fosco que não segue o controle de transparência (no mínimo ele sumia e o texto de trás aparecia)
        if (liquid::On()) {
            tint = dark ? D2D1::ColorF(0.17f, 0.165f, 0.165f, 0.90f) : D2D1::ColorF(0.95f, 0.945f, 0.94f, 0.86f);
            backdrop::SetPieces(g_back, (float)wpx, (float)hpx, { { D2D1::RectF(0, 0, (float)wpx, (float)hpx), R * sc } }, tint, 24.f);
        } else
        backdrop::Set(g_back, (float)wpx, (float)hpx, R * sc, tint);
        backdrop::Fade(g_back, 1, 1, 0, 0);
    }
}

void Hide() {
    if (!g_wnd) return;
    KillTimer(g_wnd, T_ALT);
    ShowWindow(g_wnd, SW_HIDE);
    ShowWindow(g_back, SW_HIDE);
    g_hover = -1; g_sub = false;
}

int HitAt(D2D1_POINT_2F p) {
    for (int k = 0; k < 8; k++) { auto c = CellRect(k); if (p.x >= c.left && p.x < c.right && p.y >= c.top && p.y < c.bottom) return k; }
    auto f = FsRect();
    if (p.x >= f.left && p.x < f.right && p.y >= f.top && p.y < f.bottom) return 8;
    return -1;
}

void Do(int id);
void FullScreenSub();

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); Paint(); return 0; }
    case WM_TIMER:
        if (w == T_ALT) {  // ⌥ apertado troca as paletas (como no Mac)
            bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
            if (alt != g_alt) { g_alt = alt; Paint(); }
        }
        return 0;
    case WM_MOUSEMOVE: {
        float sc = ui::Scale(h);
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 }; TrackMouseEvent(&tme);
        int hv = HitAt({ (short)LOWORD(l) / sc, (short)HIWORD(l) / sc });
        if (hv != g_hover) { g_hover = hv; Paint(); if (hv == 8) FullScreenSub(); }
        return 0;
    }
    case WM_MOUSELEAVE: if (g_hover != -1 && !g_sub) { g_hover = -1; Paint(); } return 0;
    case WM_LBUTTONUP: {
        float sc = ui::Scale(h);
        int k = HitAt({ (short)LOWORD(l) / sc, (short)HIWORD(l) / sc });
        if (k == 8) { FullScreenSub(); return 0; }
        if (k >= 0) { int id = IcoAt(k).id; Hide(); Do(id); }
        return 0;
    }
    case WM_KEYDOWN: if (w == VK_ESCAPE) Hide(); return 0;
    case WM_SYSKEYDOWN: case WM_SYSKEYUP: {
        bool alt = m == WM_SYSKEYDOWN;
        if (w == VK_MENU && alt != g_alt) { g_alt = alt; Paint(); }
        return 0;
    }
    case WM_ACTIVATE: if (LOWORD(w) == WA_INACTIVE && !g_sub) Hide(); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK BackProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

bool Make() {
    if (g_wnd) return true;
    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = shell::inst; wc.lpszClassName = L"ShellMenuVerdeTahoe"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    WNDCLASSW bc{}; bc.lpfnWndProc = BackProc; bc.hInstance = shell::inst; bc.lpszClassName = L"ShellMenuVerdeFundo";
    RegisterClassW(&bc);
    g_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, shell::inst, nullptr);
    g_back = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_NOREDIRECTIONBITMAP, bc.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, shell::inst, nullptr);
    if (!g_wnd || !g_back) return false;
    g_glass = backdrop::Attach(g_back);
    DWORD c1 = 1; COLORREF none = 0xFFFFFFFE;
    for (HWND x : { g_wnd, g_back }) { DwmSetWindowAttribute(x, 33, &c1, sizeof(c1)); DwmSetWindowAttribute(x, 34, &none, sizeof(none)); }
    g_s.hwnd = g_wnd;
    apps::ours.push_back(g_wnd);
    apps::ours.push_back(g_back);
    return true;
}

void Show(HWND target, POINT pt) {
    if (!Make()) return;
    g_target = target; g_hover = -1; g_sub = false;
    g_alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    float sc = ui::Scale(g_wnd);
    int wpx = (int)std::lround(W * sc), hpx = (int)std::lround(H * sc);
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST), &mi);
    // ~15 abaixo do topo do botão verde e ~33 à esquerda dele (o ponto é o ponteiro, no meio do botão de 14)
    int x = pt.x - (int)std::lround(40 * sc), y = pt.y + (int)std::lround(8 * sc);
    x = std::clamp(x, (int)mi.rcWork.left + 4, (int)mi.rcWork.right - wpx - 4);
    y = std::clamp(y, (int)mi.rcWork.top + 4, (int)mi.rcWork.bottom - hpx - 4);
    SetWindowPos(g_back, HWND_TOPMOST, x, y, wpx, hpx, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    SetWindowPos(g_wnd, HWND_TOPMOST, x, y, wpx, hpx, SWP_SHOWWINDOW);
    SetForegroundWindow(g_wnd);
    SetTimer(g_wnd, T_ALT, 50, nullptr);
    Paint();
}
}  // namespace tiling

// as outras janelas na ordem da tela (de cima para baixo), para "Organizar" (Esquerda e Direita, Quartos...)
std::vector<HWND> Outras(HWND target, size_t n) {
    std::vector<HWND> v;
    for (HWND h = GetTopWindow(nullptr); h && v.size() < n; h = GetWindow(h, GW_HWNDNEXT)) {
        if (h == target || !IsWindowVisible(h) || IsIconic(h) || GetWindow(h, GW_OWNER)) continue;
        LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
        if (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) continue;
        if (!(GetWindowLongW(h, GWL_STYLE) & WS_CAPTION) || GetWindowTextLengthW(h) == 0) continue;
        DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
        if (pid == GetCurrentProcessId()) continue;
        BOOL cloaked = FALSE; DwmGetWindowAttribute(h, 14 /*DWMWA_CLOAKED*/, &cloaked, sizeof(cloaked));
        if (cloaked) continue;
        v.push_back(h);
    }
    return v;
}

}  // namespace

void Executar(HWND target, int id);

void tiling::Do(int id) { Executar(g_target, id); }
void tiling::FullScreenSub() {
    if (g_sub || !g_wnd) return;
    g_sub = true;
    Paint();
    float sc = ui::Scale(g_wnd);
    RECT wr; GetWindowRect(g_wnd, &wr);
    POINT p{ wr.right - (LONG)std::lround(6 * sc), wr.top + (LONG)std::lround((Y_FS - 5) * sc) };
    auto MI = [](const std::wstring& lb, int id) { ctxmenu::Item i; i.label = lb; i.id = id; return i; };
    int id = ctxmenu::ShowItems(g_wnd, p, { MI(Tr(L"Tela Inteira"), 40), MI(Tr(L"Esquerda da Tela"), 41), MI(Tr(L"Direita da Tela"), 42) });
    g_sub = false;
    HWND t = g_target;
    Hide();
    if (id) Executar(t, id);
}

void verde::Show(HWND target, POINT pt) {
    if (!IsWindow(target)) return;
    if (liquid::On()) { tiling::Show(target, pt); return; }
    if (!g_owner) {
        WNDCLASSW wc{}; wc.lpfnWndProc = OwnerProc; wc.hInstance = shell::inst; wc.lpszClassName = L"ShellMenuVerde";
        RegisterClassW(&wc);
        g_owner = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, shell::inst, nullptr);
        apps::ours.push_back(g_owner);
    }
    auto MI = [](const std::wstring& l, int id) { ctxmenu::Item i; i.label = l; i.id = id; return i; };
    ctxmenu::Item sep; sep.separator = true;
    std::vector<ctxmenu::Item> m = {
        MI(Tr(L"Preencher"), 1), MI(Tr(L"Centralizar"), 2), sep,
        MI(Tr(L"Esquerda"), 10), MI(Tr(L"Direita"), 11), MI(Tr(L"Parte de Cima"), 12), MI(Tr(L"Parte de Baixo"), 13), sep,
        MI(Tr(L"Superior Esquerdo"), 20), MI(Tr(L"Superior Direito"), 21), MI(Tr(L"Inferior Esquerdo"), 22), MI(Tr(L"Inferior Direito"), 23),
    };
    ctxmenu::PlainNext();
    SetForegroundWindow(g_owner);
    int id = ctxmenu::ShowItems(g_owner, pt, m);
    if (id) Executar(target, id);
}

void Executar(HWND target, int id) {
    if (!id || !IsWindow(target)) return;
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(target, MONITOR_DEFAULTTONEAREST), &mi);
    RECT a = mi.rcWork;
    LONG cx = (a.left + a.right) / 2, cy = (a.top + a.bottom) / 2;
    const LONG G = 0;  // sem vão (o Windows também encaixa colado)
    Log(L"menu verde: opção %d", id);
    switch (id) {
    case 1:
        if (GetPropW(target, L"MacJanela")) {  // a nossa barra: o "ampliar" animado da DLL
            static UINT zoom = RegisterWindowMessageW(L"MacJanelaAmpliar");
            if (!GetPropW(target, L"MacJanelaAmpliada")) PostMessageW(target, zoom, 0, 0);
            SetForegroundWindow(target);
        } else Place(target, a);
        break;
    case 2: {
        RECT wr, v = Visible(target, wr);
        LONG w = std::min(v.right - v.left, a.right - a.left), hh = std::min(v.bottom - v.top, a.bottom - a.top);
        Place(target, { cx - w / 2, cy - hh / 2, cx - w / 2 + w, cy - hh / 2 + hh });
        break;
    }
    case 10: Place(target, { a.left, a.top, cx - G, a.bottom }); break;
    case 11: Place(target, { cx + G, a.top, a.right, a.bottom }); break;
    case 12: Place(target, { a.left, a.top, a.right, cy - G }); break;
    case 13: Place(target, { a.left, cy + G, a.right, a.bottom }); break;
    case 20: Place(target, { a.left, a.top, cx, cy }); break;
    case 21: Place(target, { cx, a.top, a.right, cy }); break;
    case 22: Place(target, { a.left, cy, cx, a.bottom }); break;
    case 23: Place(target, { cx, cy, a.right, a.bottom }); break;
    // Tahoe: Organizar (a janela e as próximas da tela) e Tela Cheia
    case 30: case 33: {  // Esquerda e Direita / Direita e Esquerda
        auto o = Outras(target, 1);
        bool left = id == 30;
        Place(target, left ? RECT{ a.left, a.top, cx, a.bottom } : RECT{ cx, a.top, a.right, a.bottom });
        if (!o.empty()) Place(o[0], left ? RECT{ cx, a.top, a.right, a.bottom } : RECT{ a.left, a.top, cx, a.bottom });
        SetForegroundWindow(target);
        break;
    }
    case 31: case 34: {  // Esquerda e Quartos / Direita e Quartos
        auto o = Outras(target, 2);
        bool left = id == 31;
        Place(target, left ? RECT{ a.left, a.top, cx, a.bottom } : RECT{ cx, a.top, a.right, a.bottom });
        LONG x0 = left ? cx : a.left, x1 = left ? a.right : cx;
        if (o.size() > 0) Place(o[0], { x0, a.top, x1, cy });
        if (o.size() > 1) Place(o[1], { x0, cy, x1, a.bottom });
        SetForegroundWindow(target);
        break;
    }
    case 32: {  // Quartos
        auto o = Outras(target, 3);
        Place(target, { a.left, a.top, cx, cy });
        const RECT q[3] = { { cx, a.top, a.right, cy }, { a.left, cy, cx, a.bottom }, { cx, cy, a.right, a.bottom } };
        for (size_t i = 0; i < o.size(); i++) Place(o[i], q[i]);
        SetForegroundWindow(target);
        break;
    }
    case 40: ShowWindow(target, SW_MAXIMIZE); SetForegroundWindow(target); break;  // Tela Inteira
    case 41: Place(target, { mi.rcMonitor.left, a.top, cx, a.bottom }); break;      // Esquerda da Tela
    case 42: Place(target, { cx, a.top, mi.rcMonitor.right, a.bottom }); break;     // Direita da Tela
    }
}
