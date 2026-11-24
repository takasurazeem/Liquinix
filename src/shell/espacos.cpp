// Spaces do Mac OS X Leopard (tema Leopard; Win+F8, como o F8 do Mac): a tela recua e mostra todas as áreas de
// trabalho lado a lado numa grade, sobre o cinza-escuro do 10.5. A área atual aparece como está (foto da tela tirada
// antes de abrir); as outras com o papel de parede e as janelas de cada uma (moldura de metal com o ícone e o nome).
// Clicar numa área vai para ela (a grade amplia até a escolhida); a casa "+" cria outra área. Esc ou Win+F8 fecha.
// As áreas são as do Windows (mesas.cpp: as mesmas do Mission Control).
#include "shell.h"
#include <shobjidl.h>
#include <dwmapi.h>
#include <cmath>
#include <algorithm>
#include "../comum/idioma.h"
#include "../comum/leopard.h"

namespace {

const UINT WM_SP_FRAME = WM_APP + 182;
struct WinBox { RECT r; std::wstring title; ID2D1Bitmap* icon = nullptr; };
struct Space { GUID id; std::vector<WinBox> wins; };

HWND g_wnd;
ui::GpuSurface g_s;
bool g_open, g_closing;
double g_at;
int g_cur, g_pick = -1, g_hover = -1;
std::vector<Space> g_sp;
RECT g_mon;
float g_W, g_H;
ID2D1Bitmap *g_shot, *g_wall;
UINT g_shotW, g_shotH; std::vector<BYTE> g_shotPx;

float Sc() { return ui::Scale(g_wnd); }

void Release() {
    if (g_shot) { g_shot->Release(); g_shot = nullptr; }
    for (auto& s : g_sp) for (auto& w : s.wins) if (w.icon) w.icon->Release();
    g_sp.clear();
}

// as áreas do Windows (registro do Explorador) e a atual
void LoadSpaces() {
    const wchar_t* key = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops";
    BYTE ids[16 * 32]; DWORD cb = sizeof(ids);
    BYTE cur[16]; DWORD cc = sizeof(cur);
    bool hasCur = RegGetValueW(HKEY_CURRENT_USER, key, L"CurrentVirtualDesktop", RRF_RT_REG_BINARY, nullptr, cur, &cc) == ERROR_SUCCESS && cc == 16;
    g_cur = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, key, L"VirtualDesktopIDs", RRF_RT_REG_BINARY, nullptr, ids, &cb) == ERROR_SUCCESS && cb >= 16)
        for (DWORD i = 0; i + 16 <= cb; i += 16) {
            Space s; memcpy(&s.id, ids + i, 16);
            if (hasCur && !memcmp(cur, ids + i, 16)) g_cur = (int)g_sp.size();
            g_sp.push_back(s);
        }
    if (g_sp.empty()) { g_sp.push_back({}); g_cur = 0; }
}

ID2D1Bitmap* IconBitmap(HWND h) {
    HICON ic = nullptr;
    DWORD_PTR res = 0;
    if (SendMessageTimeoutW(h, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 50, &res) && res) ic = (HICON)res;
    if (!ic) ic = (HICON)GetClassLongPtrW(h, GCLP_HICON);
    if (!ic) return nullptr;
    const int N = 48;
    BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), N, -N, 1, 32, BI_RGB };
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bm = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bm) { DeleteDC(dc); return nullptr; }
    HGDIOBJ old = SelectObject(dc, bm);
    DrawIconEx(dc, 0, 0, ic, N, N, 0, nullptr, DI_NORMAL);
    SelectObject(dc, old);
    std::vector<BYTE> px((BYTE*)bits, (BYTE*)bits + N * N * 4);
    DeleteObject(bm); DeleteDC(dc);
    bool anyA = false;
    for (int i = 3; i < N * N * 4; i += 4) if (px[i]) { anyA = true; break; }
    if (!anyA) for (int i = 3; i < N * N * 4; i += 4) px[i] = 255;
    for (int i = 0; i < N * N * 4; i += 4) { BYTE a = px[i + 3]; px[i] = px[i] * a / 255; px[i + 1] = px[i + 1] * a / 255; px[i + 2] = px[i + 2] * a / 255; }
    return ui::Bitmap(ui::Gpu(), N, N, px);
}

// as janelas de cada área (as das outras áreas ficam "escondidas" pelo Windows, mas a posição continua valendo)
void LoadWindows() {
    IVirtualDesktopManager* vdm = nullptr;
    CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&vdm));
    if (!vdm) return;
    std::vector<HWND> hs;
    for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT)) {
        if (!IsWindowVisible(h) || IsIconic(h) || apps::IsShellWindow(h)) continue;
        LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
        if ((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW)) continue;
        if (GetWindow(h, GW_OWNER) && !(ex & WS_EX_APPWINDOW)) continue;
        if (GetWindowTextLengthW(h) == 0) continue;
        DWORD cloaked = 0; DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (cloaked & 1) continue;  // escondida pelo próprio app (não é de outra área)
        wchar_t cls[64]; GetClassNameW(h, cls, 64);
        if (mesa::IsDeskClass(cls) || !wcscmp(cls, L"Shell_TrayWnd")) continue;
        hs.push_back(h);
    }
    for (auto it = hs.rbegin(); it != hs.rend(); ++it) {  // de baixo para cima: a de cima é desenhada por último
        GUID d{};
        if (FAILED(vdm->GetWindowDesktopId(*it, &d))) continue;
        for (auto& s : g_sp) {
            if (memcmp(&s.id, &d, sizeof(GUID))) continue;
            WinBox b;
            if (FAILED(DwmGetWindowAttribute(*it, DWMWA_EXTENDED_FRAME_BOUNDS, &b.r, sizeof(b.r)))) GetWindowRect(*it, &b.r);
            if (b.r.right - b.r.left < 60 || b.r.bottom - b.r.top < 40) break;
            wchar_t t[256]; GetWindowTextW(*it, t, 256); b.title = t;
            b.icon = IconBitmap(*it);
            s.wins.push_back(b);
            break;
        }
    }
    vdm->Release();
}

void Shot() {  // a área atual como está agora
    int w = g_mon.right - g_mon.left, h = g_mon.bottom - g_mon.top;
    BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB };
    void* bits = nullptr;
    HDC sdc = GetDC(nullptr), mdc = CreateCompatibleDC(sdc);
    HBITMAP bm = CreateDIBSection(sdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bm) {
        HGDIOBJ old = SelectObject(mdc, bm);
        BitBlt(mdc, 0, 0, w, h, sdc, g_mon.left, g_mon.top, SRCCOPY | CAPTUREBLT);
        SelectObject(mdc, old);
        g_shotPx.assign((BYTE*)bits, (BYTE*)bits + (size_t)w * h * 4);
        for (size_t i = 3; i < g_shotPx.size(); i += 4) g_shotPx[i] = 255;
        g_shotW = w; g_shotH = h;
        UINT dw = w, dh = h;
        ui::Downscale(dw, dh, g_shotPx, 1600);
        g_shotW = dw; g_shotH = dh;
        DeleteObject(bm);
    }
    DeleteDC(mdc); ReleaseDC(nullptr, sdc);
}

void Wallpaper() {
    if (g_wall) return;
    wchar_t p[MAX_PATH] = {};
    SystemParametersInfoW(SPI_GETDESKWALLPAPER, MAX_PATH, p, 0);
    UINT w = 0, h = 0; std::vector<BYTE> px;
    if (p[0] && ui::LoadPhoto(p, 800, w, h, px)) g_wall = ui::Bitmap(ui::Gpu(), w, h, px);
}

// a grade: colunas e linhas para n casas (mais a casa "+")
D2D1_RECT_F Tile(int i, int n) {
    int cols = (int)std::ceil(std::sqrt((double)n)), rows = (n + cols - 1) / cols;
    float gap = 26, ar = g_W / g_H;
    float aw = g_W * 0.86f, ah = g_H * 0.80f;
    float tw = (aw - (cols - 1) * gap) / cols, th = tw / ar;
    if (th * rows + (rows - 1) * gap > ah) { th = (ah - (rows - 1) * gap) / rows; tw = th * ar; }
    float x0 = (g_W - (cols * tw + (cols - 1) * gap)) / 2, y0 = (g_H - (rows * th + (rows - 1) * gap)) / 2;
    int c = i % cols, r = i / cols;
    return D2D1::RectF(x0 + c * (tw + gap), y0 + r * (th + gap), x0 + c * (tw + gap) + tw, y0 + r * (th + gap) + th);
}
int Count() { return (int)g_sp.size() + (g_sp.size() < 16 ? 1 : 0); }  // a última casa é o "+"

float Prog() {
    float k = (float)std::clamp((anim::Now() - g_at) / 320.0, 0.0, 1.0);
    k = anim::Ease(k);
    return g_closing ? 1 - k : k;
}
D2D1_RECT_F Lerp(D2D1_RECT_F a, D2D1_RECT_F b, float t) {
    return { a.left + (b.left - a.left) * t, a.top + (b.top - a.top) * t, a.right + (b.right - a.right) * t, a.bottom + (b.bottom - a.bottom) * t };
}

void DrawBitmapCover(ID2D1DeviceContext* dc, ID2D1Bitmap* b, D2D1_RECT_F r) {
    auto s = b->GetSize();
    float k = std::max((r.right - r.left) / s.width, (r.bottom - r.top) / s.height);
    float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    dc->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    dc->DrawBitmap(b, D2D1::RectF(cx - s.width * k / 2, cy - s.height * k / 2, cx + s.width * k / 2, cy + s.height * k / 2), 1, D2D1_INTERPOLATION_MODE_LINEAR);
    dc->PopAxisAlignedClip();
}

void DrawSpace(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, int i, D2D1_RECT_F r) {
    if (i == g_cur && g_shot) { dc->DrawBitmap(g_shot, r, 1, D2D1_INTERPOLATION_MODE_LINEAR); return; }
    if (g_wall) DrawBitmapCover(dc, g_wall, r);
    else { br->SetColor(D2D1::ColorF(0.20f, 0.28f, 0.45f)); dc->FillRectangle(r, br); }
    float kx = (r.right - r.left) / (g_mon.right - g_mon.left), ky = (r.bottom - r.top) / (g_mon.bottom - g_mon.top);
    static IDWriteTextFormat* ft = [] { auto* f = ui::FormatFace(L"Lucida Sans Unicode", 9, DWRITE_FONT_WEIGHT_NORMAL); if (f) { f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); } return f; }();
    dc->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for (auto& w : g_sp[i].wins) {  // a janela em miniatura: corpo claro, a barra de metal com o nome e o ícone no meio
        D2D1_RECT_F q = D2D1::RectF(r.left + (w.r.left - g_mon.left) * kx, r.top + (w.r.top - g_mon.top) * ky,
                                    r.left + (w.r.right - g_mon.left) * kx, r.top + (w.r.bottom - g_mon.top) * ky);
        br->SetColor(D2D1::ColorF(0, 0, 0, 0.30f));
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(q.left + 1, q.top + 2, q.right + 1, q.bottom + 3), 3, 3), br);
        br->SetColor(D2D1::ColorF(0.93f, 0.93f, 0.93f, 0.97f));
        dc->FillRoundedRectangle(D2D1::RoundedRect(q, 3, 3), br);
        float bh = std::min(14.f, (q.bottom - q.top) * 0.2f);
        leopard::Metal(dc, D2D1::RectF(q.left, q.top, q.right, q.top + bh), true, false, true, 3);
        if (ft && q.right - q.left > 40) {
            br->SetColor(D2D1::ColorF(0, 0, 0, 0.8f));
            dc->DrawTextW(w.title.c_str(), (UINT32)w.title.size(), ft, D2D1::RectF(q.left + 4, q.top, q.right - 4, q.top + bh), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        if (w.icon) {
            float s = std::min(40.f, std::min(q.right - q.left, q.bottom - q.top - bh) * 0.5f);
            float cx = (q.left + q.right) / 2, cy = (q.top + bh + q.bottom) / 2;
            dc->DrawBitmap(w.icon, D2D1::RectF(cx - s / 2, cy - s / 2, cx + s / 2, cy + s / 2), 1, D2D1_INTERPOLATION_MODE_LINEAR);
        }
        br->SetColor(D2D1::ColorF(0, 0, 0, 0.35f));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(q, 3, 3), br, 0.8f);
    }
    dc->PopAxisAlignedClip();
}

void Paint() {
    if (!g_open) return;
    float sc = Sc();
    RECT rc; GetClientRect(g_wnd, &rc);
    auto* dc = ui::GpuBegin(g_s, rc.right, rc.bottom, sc);
    if (!dc) return;
    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &br);
    float k = Prog();
    D2D1_RECT_F full = D2D1::RectF(0, 0, g_W, g_H);
    br->SetColor(D2D1::ColorF(0.24f, 0.24f, 0.25f, std::min(1.f, k * 1.5f)));  // o cinza do Spaces do 10.5
    dc->FillRectangle(full, br);
    int n = Count();
    int focus = g_pick >= 0 ? g_pick : g_cur;  // a casa que cresce para a tela inteira (abrindo: a atual; escolhendo: a escolhida)
    ID2D1Layer* layer = nullptr; dc->CreateLayer(nullptr, &layer);
    static IDWriteTextFormat* fnum = [] { auto* f = ui::FormatFace(L"Lucida Sans Unicode", 13, DWRITE_FONT_WEIGHT_BOLD); if (f) { f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); } return f; }();
    for (int pass = 0; pass < 2; pass++)  // a casa em foco por último (por cima das outras enquanto cresce)
        for (int i = 0; i < n; i++) {
            bool f = i == focus;
            if ((pass == 0) == f) continue;
            D2D1_RECT_F t = Tile(i, n);
            D2D1_RECT_F r = f ? Lerp(full, t, k) : t;
            float a = f ? 1.f : k;
            dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), a), nullptr);
            if (i >= (int)g_sp.size()) {  // a casa "+": criar outra área
                br->SetColor(D2D1::ColorF(1, 1, 1, g_hover == i ? 0.16f : 0.08f));
                dc->FillRoundedRectangle(D2D1::RoundedRect(r, 6, 6), br);
                br->SetColor(D2D1::ColorF(1, 1, 1, 0.35f));
                ID2D1StrokeStyle* dash = nullptr; ID2D1Factory* fac = nullptr; dc->GetFactory(&fac);
                if (fac) { fac->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_LINE_JOIN_MITER, 10, D2D1_DASH_STYLE_DASH), nullptr, 0, &dash); fac->Release(); }
                dc->DrawRoundedRectangle(D2D1::RoundedRect(r, 6, 6), br, 2, dash);
                if (dash) dash->Release();
                float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2, s = std::min(r.right - r.left, r.bottom - r.top) * 0.12f;
                br->SetColor(D2D1::ColorF(1, 1, 1, 0.7f));
                dc->DrawLine({ cx - s, cy }, { cx + s, cy }, br, 4); dc->DrawLine({ cx, cy - s }, { cx, cy + s }, br, 4);
            } else {
                br->SetColor(D2D1::ColorF(0, 0, 0, 0.45f * k));
                dc->FillRectangle(D2D1::RectF(r.left + 2, r.top + 4, r.right + 2, r.bottom + 6), br);
                DrawSpace(dc, br, i, r);
                if (k > 0.05f) {
                    // contorno: claro na área sob o mouse, mais forte na atual (como o destaque do 10.5)
                    br->SetColor(D2D1::ColorF(1, 1, 1, (i == g_cur ? 0.85f : g_hover == i ? 0.6f : 0.18f) * k));
                    dc->DrawRectangle(D2D1::RectF(r.left - 1.5f, r.top - 1.5f, r.right + 1.5f, r.bottom + 1.5f), br, i == g_cur || g_hover == i ? 3.f : 1.f);
                    if (fnum) {  // o número da área num selinho embaixo
                        D2D1_RECT_F nb = D2D1::RectF((r.left + r.right) / 2 - 13, r.bottom - 30, (r.left + r.right) / 2 + 13, r.bottom - 8);
                        br->SetColor(D2D1::ColorF(0, 0, 0, 0.55f * k));
                        dc->FillRoundedRectangle(D2D1::RoundedRect(nb, 11, 11), br);
                        br->SetColor(D2D1::ColorF(1, 1, 1, 0.95f * k));
                        std::wstring s = std::to_wstring(i + 1);
                        dc->DrawTextW(s.c_str(), (UINT32)s.size(), fnum, nb, br);
                    }
                }
            }
            dc->PopLayer();
        }
    layer->Release();
    br->Release();
    ui::GpuEnd(g_s);
}

void HideNow() {
    ui::FrameClock(g_wnd, WM_SP_FRAME, false);
    ShowWindow(g_wnd, SW_HIDE);
    bool sw = g_pick >= 0 && g_pick != g_cur && g_pick < (int)g_sp.size();
    int to = g_pick;
    g_open = g_closing = false;
    Release();
    if (sw) mesas::Switch(to);
}
void Close(int pick) {
    if (!g_open || g_closing) return;
    g_pick = pick;
    g_closing = true; g_at = anim::Now();
    ui::FrameClock(g_wnd, WM_SP_FRAME, true);
}
int Hit(LPARAM l) {
    float sc = Sc();
    D2D1_POINT_2F p{ (short)LOWORD(l) / sc, (short)HIWORD(l) / sc };
    int n = Count();
    for (int i = 0; i < n; i++) { D2D1_RECT_F r = Tile(i, n); if (p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom) return i; }
    return -1;
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_SP_FRAME:
        ui::FrameAck(h, WM_SP_FRAME);
        Paint();
        if (anim::Now() - g_at > 340) { if (g_closing) HideNow(); else ui::FrameClock(h, WM_SP_FRAME, false); }
        return 0;
    case WM_MOUSEMOVE: { int i = Hit(l); if (i != g_hover && !g_closing) { g_hover = i; Paint(); } return 0; }
    case WM_LBUTTONUP: {
        int i = Hit(l);
        if (i < 0) { Close(g_cur); return 0; }
        if (i >= (int)g_sp.size()) {  // "+": cria a área e mostra a grade de novo
            if (mesas::Create()) { Release(); LoadSpaces(); Shot(); LoadWindows(); if (!g_shotPx.empty()) g_shot = ui::Bitmap(ui::Gpu(), g_shotW, g_shotH, g_shotPx); Paint(); }
            return 0;
        }
        Close(i);
        return 0;
    }
    case WM_KEYDOWN:
        if (w == VK_ESCAPE || w == VK_F8) Close(g_cur);
        else if (w >= '1' && w <= '9' && (int)(w - '1') < (int)g_sp.size()) Close((int)(w - '1'));
        return 0;
    case WM_ACTIVATE: if (LOWORD(w) == WA_INACTIVE && g_open && !g_closing) Close(g_cur); return 0;
    case WM_CLOSE: Close(g_cur); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

bool espacos::Create(HINSTANCE hi) {
    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = hi; wc.lpszClassName = L"ShellSpaces"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    g_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, L"Spaces", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
    if (!g_wnd) return false;
    apps::ours.push_back(g_wnd);
    g_s.hwnd = g_wnd;
    return true;
}

bool espacos::IsOpen() { return g_open && !g_closing; }

void espacos::Toggle() {
    if (!g_wnd && !espacos::Create(shell::inst)) return;
    if (g_open) { if (!g_closing) Close(g_cur); return; }
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
    g_mon = mi.rcMonitor;
    Release();
    LoadSpaces();
    Shot();
    LoadWindows();
    Wallpaper();
    if (!g_shotPx.empty()) g_shot = ui::Bitmap(ui::Gpu(), g_shotW, g_shotH, g_shotPx);
    SetWindowPos(g_wnd, HWND_TOPMOST, g_mon.left, g_mon.top, g_mon.right - g_mon.left, g_mon.bottom - g_mon.top, SWP_NOACTIVATE);
    float sc = Sc();
    g_W = (g_mon.right - g_mon.left) / sc; g_H = (g_mon.bottom - g_mon.top) / sc;
    g_open = true; g_closing = false; g_pick = -1; g_hover = -1; g_at = anim::Now();
    Paint();
    ShowWindow(g_wnd, SW_SHOW);
    HWND fg = GetForegroundWindow();
    DWORD ft = fg ? GetWindowThreadProcessId(fg, nullptr) : 0, me = GetCurrentThreadId();
    bool att = ft && ft != me && AttachThreadInput(me, ft, TRUE);
    SetForegroundWindow(g_wnd); SetFocus(g_wnd);
    if (att) AttachThreadInput(me, ft, FALSE);
    ui::FrameClock(g_wnd, WM_SP_FRAME, true);
    Log(L"spaces: %zu áreas, atual %d", g_sp.size(), g_cur + 1);
}
