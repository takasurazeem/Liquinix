// Organizador Visual (Stage Manager do Mac). Ligado (organizador=1): só o app da frente fica na tela; os outros
// somem (o mesmo "Ocultar" do Dock: minimizados sem o gênio e fora da lista de minimizadas) e aparecem numa
// faixa à esquerda, com a miniatura da janela mais recente e o ícone do app, os mais recentes em cima.
// Clicar numa miniatura traz aquele app (todas as janelas) e o que estava na frente vai para a faixa.
// A faixa fica logo acima da mesa (abaixo de todas as janelas): uma janela por cima a cobre, como no Mac.
// Desligar devolve tudo como estava (as janelas ocultas pelo Organizador voltam, sem roubar a frente).
#include "shell.h"
#include <dwmapi.h>
#include <algorithm>
#include <cmath>
#include <map>
#include "../comum/idioma.h"

namespace {

const UINT WM_PL_CHECK = WM_APP + 95;
const float STRIP_W = 190, THUMB_W = 138, THUMB_H = 92, ICON = 30, GAP = 26, LEFT = 10;
const int MAX_TILES = 6;
HWND g_strip;
ui::GpuSurface g_s;
HWINEVENTHOOK g_hook;
bool g_on;
std::wstring g_stage;                 // id do app no palco
std::vector<std::wstring> g_recent;   // ids dos apps na faixa (mais recente primeiro)
std::map<HWND, HTHUMBNAIL> g_thumbs;
struct Tile { std::wstring id; HWND win; D2D1_RECT_F thumb, icon; };
std::vector<Tile> g_tiles;
int g_hover = -1;
bool g_raised = false;               // faixa trazida para frente pela borda esquerda
std::map<std::wstring, ID2D1Bitmap*> g_icons;
IDWriteTextFormat* g_tf;

App* FindApp(const std::wstring& id) { for (auto& a : apps::list) if (a.id == id) return &a; return nullptr; }

float Sc() { return ui::Scale(g_strip); }

RECT WorkArea() {
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY), &mi);
    return mi.rcWork;
}

// a janela mais recente do app que o Organizador escondeu (a miniatura dela vai para a faixa)
HWND ShownWin(App& a) {
    for (auto& w : a.wins) if (IsWindow(w.hwnd) && apps::IsHidden(w.hwnd)) return w.hwnd;
    return nullptr;
}

void ClearThumbs() {
    for (auto& [h, t] : g_thumbs) DwmUnregisterThumbnail(t);
    g_thumbs.clear();
}

// logo acima da mesa: abaixo de todas as janelas dos apps
void PlaceStrip() {
    RECT wa = WorkArea();
    float sc = ui::Scale(g_strip);
    int w = (int)std::lround(STRIP_W * sc);
    HWND desk = nullptr;
    for (HWND x = GetWindow(GetDesktopWindow(), GW_CHILD); x; x = GetWindow(x, GW_HWNDNEXT)) if (mesa::IsDeskWindow(x)) desk = x;
    HWND after = desk ? GetWindow(desk, GW_HWNDPREV) : HWND_BOTTOM;
    if (after == g_strip) after = nullptr;
    SetWindowPos(g_strip, after ? after : HWND_BOTTOM, wa.left, wa.top, w, wa.bottom - wa.top, SWP_NOACTIVATE | SWP_SHOWWINDOW | (after ? 0 : SWP_NOZORDER));
}

void Paint() {
    if (!g_strip) return;
    float sc = Sc();
    RECT wr; GetWindowRect(g_strip, &wr);
    int wpx = wr.right - wr.left, hpx = wr.bottom - wr.top;
    // as peças, de cima para baixo no meio da altura
    g_tiles.clear();
    std::vector<std::pair<std::wstring, HWND>> items;
    for (auto& id : g_recent) {
        App* a = FindApp(id);
        if (!a) continue;
        HWND w = ShownWin(*a);
        if (w) items.push_back({ id, w });
        if ((int)items.size() >= MAX_TILES) break;
    }
    float H = hpx / sc, total = items.size() * THUMB_H + (items.empty() ? 0 : (items.size() - 1) * GAP);
    float y = std::max(16.f, (H - total) / 2);
    for (auto& [id, w] : items) {
        Tile t{ id, w };
        t.thumb = D2D1::RectF(LEFT + 22, y, LEFT + 22 + THUMB_W, y + THUMB_H);
        t.icon = D2D1::RectF(LEFT, y + THUMB_H - ICON + 8, LEFT + ICON, y + THUMB_H + 8);
        g_tiles.push_back(t);
        y += THUMB_H + GAP;
    }
    // miniaturas (o DWM desenha por cima do conteúdo da janela)
    std::map<HWND, HTHUMBNAIL> keep;
    for (auto& t : g_tiles) {
        HTHUMBNAIL th = nullptr;
        auto f = g_thumbs.find(t.win);
        if (f != g_thumbs.end()) { th = f->second; g_thumbs.erase(f); }
        else if (FAILED(DwmRegisterThumbnail(g_strip, t.win, &th))) th = nullptr;
        if (!th) continue;
        SIZE src{};
        DwmQueryThumbnailSourceSize(th, &src);
        float bw = t.thumb.right - t.thumb.left, bh = t.thumb.bottom - t.thumb.top;
        float k = src.cx > 0 && src.cy > 0 ? std::min(bw / src.cx, bh / src.cy) : 1;
        float w = src.cx > 0 ? src.cx * k : bw, h = src.cy > 0 ? src.cy * k : bh;
        float x = t.thumb.left + (bw - w) / 2, yy = t.thumb.top + (bh - h) / 2;
        float grow = t.id == (g_hover >= 0 && g_hover < (int)g_tiles.size() ? g_tiles[g_hover].id : L"") ? 1.06f : 1.f;
        float cx = x + w / 2, cy = yy + h / 2;
        w *= grow; h *= grow;
        DWM_THUMBNAIL_PROPERTIES tp{};
        tp.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
        tp.rcDestination = { (LONG)std::lround((cx - w / 2) * sc), (LONG)std::lround((cy - h / 2) * sc), (LONG)std::lround((cx + w / 2) * sc), (LONG)std::lround((cy + h / 2) * sc) };
        tp.fVisible = TRUE; tp.opacity = 255; tp.fSourceClientAreaOnly = FALSE;
        DwmUpdateThumbnailProperties(th, &tp);
        keep[t.win] = th;
    }
    ClearThumbs();
    g_thumbs = keep;
    // ícones dos apps (fora das miniaturas, no canto de baixo à esquerda)
    ID2D1DeviceContext* dc = ui::GpuBegin(g_s, wpx, hpx, sc);
    if (!dc) return;
    for (auto& t : g_tiles) {
        App* a = FindApp(t.id);
        if (!a) continue;
        ID2D1Bitmap*& b = g_icons[t.id];
        if (!b) { apps::EnsureIcon(*a, (int)std::lround(ICON * 2 * sc)); b = ui::Bitmap(dc, a->iw, a->ih, a->icon); }
        if (b) dc->DrawBitmap(b, t.icon, 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    }
    ui::GpuEnd(g_s);
}

void Refresh() {
    if (!g_on || !g_strip) return;
    if (!g_raised) PlaceStrip();
    Paint();
}

// o app da frente mudou: os outros vão para a faixa
void StageChanged() {
    HWND fg = GetForegroundWindow();
    if (!fg || apps::IsShellWindow(fg) || mesa::IsDeskWindow(fg)) return;
    HWND root = GetAncestor(fg, GA_ROOTOWNER);
    App* a = apps::OfWindow(root ? root : fg);
    if (!a || a->wins.empty()) return;
    if (a->id == g_stage) { Refresh(); return; }
    g_stage = a->id;
    g_recent.erase(std::remove(g_recent.begin(), g_recent.end(), a->id), g_recent.end());
    for (auto& o : apps::list) {
        if (&o == a || o.wins.empty()) continue;
        bool shown = false;
        for (auto& w : o.wins) if (!w.minimized && !IsIconic(w.hwnd)) shown = true;
        if (!shown) continue;
        apps::Hide(o);
        g_recent.erase(std::remove(g_recent.begin(), g_recent.end(), o.id), g_recent.end());
        g_recent.insert(g_recent.begin(), o.id);
    }
    if (g_recent.size() > 12) g_recent.resize(12);
    Log(L"organizador: no palco %ls (%zu na faixa)", a->name.c_str(), g_recent.size());
    Refresh();
}

void CALLBACK OnForeground(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
    if (g_strip) { KillTimer(g_strip, 1); SetTimer(g_strip, 1, 180, nullptr); }  // espera a troca assentar
}

int HitTile(LPARAM l) {
    float sc = Sc(), x = (short)LOWORD(l) / sc, y = (short)HIWORD(l) / sc;
    for (int i = 0; i < (int)g_tiles.size(); i++) {
        auto& t = g_tiles[i];
        if ((x >= t.thumb.left && x < t.thumb.right && y >= t.thumb.top && y < t.thumb.bottom) ||
            (x >= t.icon.left && x < t.icon.right && y >= t.icon.top && y < t.icon.bottom)) return i;
    }
    return -1;
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_TIMER:
        if (w == 1) { KillTimer(h, 1); StageChanged(); }
        if (w == 2 && !g_raised) Refresh();  // apps abrindo/fechando, janelas novas
        if (w == 3) {  // janela grande por cima da faixa: o mouse na borda esquerda traz a faixa (como no Mac)
            POINT c; GetCursorPos(&c);
            RECT wa = WorkArea(), sr; GetWindowRect(h, &sr);
            bool edge = c.x <= wa.left + 1 && c.y >= wa.top && c.y < wa.bottom && GetAsyncKeyState(VK_LBUTTON) >= 0;
            if (!g_raised && edge && !g_tiles.empty()) {
                g_raised = true;
                SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            } else if (g_raised && !PtInRect(&sr, c)) {
                g_raised = false;
                SetWindowPos(h, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                PlaceStrip();
            }
        }
        return 0;
    case WM_NCHITTEST: {
        POINT p{ (short)LOWORD(l), (short)HIWORD(l) }; ScreenToClient(h, &p);
        return HitTile(MAKELPARAM(p.x, p.y)) >= 0 ? HTCLIENT : HTTRANSPARENT;
    }
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_MOUSEMOVE: {
        int i = HitTile(l);
        if (i != g_hover) { g_hover = i; Paint(); }
        TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE: if (g_hover >= 0) { g_hover = -1; Paint(); } return 0;
    case WM_LBUTTONUP: {
        int i = HitTile(l);
        if (i < 0) return 0;
        if (App* a = FindApp(g_tiles[i].id)) apps::Activate(*a);  // volta com todas as janelas; a troca da frente esconde o de antes
        return 0;
    }
    case WM_DPICHANGED: case WM_DISPLAYCHANGE: Refresh(); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void TurnOn() {
    if (g_on) return;
    HINSTANCE hi = GetModuleHandleW(nullptr);
    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = hi; wc.lpszClassName = L"ShellOrganizador"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    g_strip = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
    if (!g_strip) return;
    g_s.hwnd = g_strip;
    apps::ours.push_back(g_strip);
    g_on = true;
    g_stage.clear();
    g_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnForeground, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    SetTimer(g_strip, 2, 1500, nullptr);
    SetTimer(g_strip, 3, 100, nullptr);
    g_raised = false;
    Log(L"organizador: ligado");
    StageChanged();
    Refresh();
}

void TurnOff() {
    if (!g_on) return;
    g_on = false;
    if (g_hook) { UnhookWinEvent(g_hook); g_hook = nullptr; }
    ClearThumbs();
    for (auto& [id, b] : g_icons) if (b) b->Release();
    g_icons.clear();
    ui::GpuFree(g_s);
    if (g_strip) { apps::ours.erase(std::remove(apps::ours.begin(), apps::ours.end(), g_strip), apps::ours.end()); DestroyWindow(g_strip); g_strip = nullptr; }
    // as janelas que o Organizador escondeu voltam, atrás da da frente
    HWND fg = GetForegroundWindow();
    for (auto& id : g_recent)
        if (App* a = FindApp(id))
            for (auto& w : a->wins) if (apps::IsHidden(w.hwnd)) ShowWindow(w.hwnd, SW_SHOWNOACTIVATE);
    if (fg) SetForegroundWindow(fg);
    g_recent.clear(); g_tiles.clear(); g_stage.clear();
    Log(L"organizador: desligado");
}

}  // namespace

bool palco::On() { return g_on; }
void palco::Sync() { if (shell::Setting(L"organizador", L"0") == L"1") TurnOn(); else TurnOff(); }
void palco::Toggle() { shell::SetSetting(L"organizador", g_on ? L"0" : L"1"); Sync(); }
void palco::Stop() { TurnOff(); }
