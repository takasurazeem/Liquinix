// Seletor de apps (Cmd+Tab do Mac) no Alt+Tab: um ícone por app, na ordem de uso, num painel de vidro
// no meio da tela; o escolhido fica num quadrado escuro com o nome embaixo. Segurando o Alt: Tab avança,
// Shift+Tab volta, setas andam, Q encerra o app escolhido, H oculta, Esc cancela; soltar o Alt troca.
// Um Alt+Tab rápido troca para o app anterior sem nem mostrar o painel (como no Mac).
// Medidas da referência (2x): ícones de 78 pt a 101,5 pt um do outro, quadrado de 103, painel de 141 de
// altura com 13,5 de margem nos lados e 18,5 em cima; nome a 132,5 do topo; cantos de 15.
#include "shell.h"
#include <map>
#include <dwmapi.h>
#include <algorithm>
#include <cmath>

namespace {

// Sequoia: medidas das capturas do Sequoia. Tahoe (Metricas, a cada vez que aparece): guia macOS26-Theme-Reference,
// seção 37 (medido com Acessibilidade no macOS 26): ícones de 128 a cada 134 (6 entre eles), 24 de margem em
// todos os lados, 176 de altura (980 × 176 com 7 apps), centralizado na tela; sem animação.
float ICON = 78, PITCH = 101.5f, TILE = 103, PAD_X = 13.5f, PAD_TOP = 18.5f, PANEL_H = 141, LABEL_CY = 132.5f;
const float RADIUS = 15;
void Metricas() {
    // o usuário achou grande na tela dele (2026-11-16): o guia em ~78% (quadro de 100 em vez de 128, painel de 138)
    if (liquid::On()) { ICON = 100 * 0.805f; PITCH = 105; TILE = 100; PAD_X = 19; PAD_TOP = 19; PANEL_H = 138; LABEL_CY = 19 + 100 + 10; }
    else if (liquid::Leopard()) { ICON = 96; PITCH = 118; TILE = 114; PAD_X = 16; PAD_TOP = 14; PANEL_H = 160; LABEL_CY = 14 + 114 + 15; }  // Leopard: ícones grandes
    else { ICON = 78; PITCH = 101.5f; TILE = 103; PAD_X = 13.5f; PAD_TOP = 18.5f; PANEL_H = 141; LABEL_CY = 132.5f; }
}
const UINT_PTR T_SHOW = 1, T_SHOWNOW = 2;
const UINT WM_SHOWNOW = WM_APP + 31;  // o vidro já está na tela: mostra as janelas
const UINT SHOW_DELAY = 0;    // sem espera: a janela aparece junto com o Tab (sobra só para o temporizador antigo)

struct Entry { std::wstring id, name; ID2D1Bitmap* bmp = nullptr; bool mac = false; };
std::map<std::wstring, ID2D1Bitmap*> g_bmpCache;  // ícone de cada app, guardado entre uma abertura e outra

HWND g_wnd, g_back;
bool g_glass;
ui::GpuSurface g_s;
std::vector<Entry> g_list;
int g_sel = 0;
bool g_active, g_visible;
float g_k = 1;  // encolhe tudo quando não cabe na tela
RECT g_mon{};

float Sc() { return ui::Scale(g_wnd); }
float PanelR() { return liquid::On() ? PANEL_H * 0.4f : RADIUS; }  // Tahoe: cantos de ~40% da altura (captura do macOS 26)

float PanelW() { return PAD_X * 2 + g_list.size() * PITCH + (TILE - PITCH); }

// ordem de uso: a ordem das janelas na tela (a de cima é a mais recente), depois os minimizados
void Collect() {
    g_list.clear();  // os ícones ficam no cache
    apps::Refresh();
    std::vector<App*> order;
    auto add = [&](App* a) { if (a && std::find(order.begin(), order.end(), a) == order.end()) order.push_back(a); };
    for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT)) {
        if (!IsWindowVisible(h) || IsIconic(h)) continue;
        DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
        if (pid == GetCurrentProcessId()) continue;  // Dock, barra de menus, widgets...: não são o "app anterior"
        App* a = apps::OfWindow(h);
        if (a && !a->wins.empty()) add(a);
    }
    for (auto& a : apps::list) if (!a.wins.empty()) add(&a);  // minimizados
    for (auto& a : apps::list) if (a.finder) add(&a);          // o Finder está sempre na lista, como no Mac
    for (App* a : order) {
        apps::EnsureIcon(*a, 128);
        Entry e; e.id = a->id; e.name = a->name; e.mac = a->mac;
        auto f = g_bmpCache.find(a->id);
        if (f != g_bmpCache.end()) e.bmp = f->second;
        else if (!a->icon.empty()) { e.bmp = ui::Bitmap(ui::Gpu(), a->iw, a->ih, a->icon); g_bmpCache[a->id] = e.bmp; }
        g_list.push_back(std::move(e));
    }
}

// Mac OS X Leopard: o seletor de apps do 10.5 — painel preto translúcido de cantos 18 com o fio claro, ícones grandes,
// o escolhido num quadrado cinza claro translúcido e o nome dele em branco, negrito, embaixo. Desenho próprio.
void PaintLeopard() {
    float sc = Sc(), W = PanelW(), H = PANEL_H;
    int wpx = (int)std::lround(W * g_k * sc), hpx = (int)std::lround(H * g_k * sc);
    if (g_back && IsWindowVisible(g_back)) ShowWindow(g_back, SW_HIDE);  // sem o vidro desfocado: o 10.5 não desfocava
    auto* dc = ui::GpuBegin(g_s, wpx, hpx, sc * g_k);
    if (!dc) return;
    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &br);
    const float R = 18;
    br->SetColor(D2D1::ColorF(0.06f, 0.06f, 0.07f, 0.66f));
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, W, H), R, R), br);
    br->SetColor(D2D1::ColorF(1, 1, 1, 0.22f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, W - 0.5f, H - 0.5f), R - 0.5f, R - 0.5f), br, 1);
    for (int i = 0; i < (int)g_list.size(); i++) {
        float x = PAD_X + i * PITCH;
        D2D1_RECT_F tile{ x, PAD_TOP, x + TILE, PAD_TOP + TILE };
        if (i == g_sel) {
            br->SetColor(D2D1::ColorF(1, 1, 1, 0.20f));
            dc->FillRoundedRectangle(D2D1::RoundedRect(tile, 10, 10), br);
            br->SetColor(D2D1::ColorF(1, 1, 1, 0.30f));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tile.left + 0.5f, tile.top + 0.5f, tile.right - 0.5f, tile.bottom - 0.5f), 9.5f, 9.5f), br, 1);
        }
        auto& e = g_list[i];
        if (e.bmp) {
            float s = e.mac ? ICON / 0.8f : ICON;
            float cx = (tile.left + tile.right) / 2, cy = (tile.top + tile.bottom) / 2;
            auto sz = e.bmp->GetSize();
            float k = std::min(s / sz.width, s / sz.height);
            dc->DrawBitmap(e.bmp, D2D1::RectF(cx - sz.width * k / 2, cy - sz.height * k / 2, cx + sz.width * k / 2, cy + sz.height * k / 2), 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        }
    }
    if (g_sel >= 0 && g_sel < (int)g_list.size()) {
        static IDWriteTextFormat* tf = [] { auto* f = ui::FormatFace(L"Lucida Sans Unicode", 13, DWRITE_FONT_WEIGHT_BOLD); if (f) f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); return f; }();
        float cx = PAD_X + g_sel * PITCH + TILE / 2;
        const std::wstring& n = g_list[g_sel].name;
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.97f));
        dc->DrawTextW(n.c_str(), (UINT32)n.size(), tf, D2D1::RectF(cx - 140, LABEL_CY - 10, cx + 140, LABEL_CY + 10), br);
    }
    br->Release();
    ui::GpuEnd(g_s);
}

void Paint() {
    if (!g_visible) return;
    if (liquid::Leopard()) { PaintLeopard(); return; }
    float sc = Sc(), W = PanelW(), H = PANEL_H;
    int wpx = (int)std::lround(W * g_k * sc), hpx = (int)std::lround(H * g_k * sc);
    auto* dc = ui::GpuBegin(g_s, wpx, hpx, sc * g_k);
    if (!dc) return;
    bool dark = ui::Dark();
    // Tahoe: painel de vidro bem mais redondo (Liquid Glass) e o escolhido num quadrado de vidro claro
    const bool tahoe = liquid::On();
    const float R = PanelR();
    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &br);
    if (!g_glass) { br->SetColor(dark ? D2D1::ColorF(0.17f, 0.17f, 0.18f, 0.96f) : D2D1::ColorF(0.93f, 0.93f, 0.93f, 0.96f)); dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, W, H), R, R), br); }
    if (tahoe) {
        if (!backdrop::Liquid()) liquid::Rim(dc, D2D1::RectF(0, 0, W, H), R, dark);  // com o Liquid Glass o vidro já tem o contorno
    } else {
    br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.14f) : D2D1::ColorF(1, 1, 1, 0.35f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, W - 0.5f, H - 0.5f), R - 0.5f, R - 0.5f), br, 1);
    }
    for (int i = 0; i < (int)g_list.size(); i++) {
        float x = PAD_X + i * PITCH;
        D2D1_RECT_F tile{ x, PAD_TOP, x + TILE, PAD_TOP + TILE };
        if (i == g_sel && tahoe) {  // Tahoe (captura do macOS 26): o escolhido num quadrado MAIS ESCURO e translúcido,
            float tr = (tile.right - tile.left) * 0.26f;  // cantos de ~26% e um fio claro fino em volta
            br->SetColor(dark ? D2D1::ColorF(0, 0, 0, 0.34f) : D2D1::ColorF(0, 0, 0, 0.14f));
            dc->FillRoundedRectangle(D2D1::RoundedRect(tile, tr, tr), br);
            br->SetColor(D2D1::ColorF(1, 1, 1, dark ? 0.18f : 0.40f));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tile.left + 0.5f, tile.top + 0.5f, tile.right - 0.5f, tile.bottom - 0.5f), tr, tr), br, 1);
        } else if (i == g_sel) {
            br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.14f) : D2D1::ColorF(0, 0, 0, 0.22f));
            dc->FillRoundedRectangle(D2D1::RoundedRect(tile, 5, 5), br);  // cantos pouco arredondados, como na referência
        }
        auto& e = g_list[i];
        if (e.bmp) {
            // o desenho visível do ícone tem 78 pt: os do Mac têm margem transparente (corpo = 80,5% da imagem)
            float s = e.mac ? ICON / 0.805f : ICON;
            float cx = (tile.left + tile.right) / 2, cy = (tile.top + tile.bottom) / 2;
            auto sz = e.bmp->GetSize();
            float k = std::min(s / sz.width, s / sz.height);
            dc->DrawBitmap(e.bmp, D2D1::RectF(cx - sz.width * k / 2, cy - sz.height * k / 2, cx + sz.width * k / 2, cy + sz.height * k / 2), 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        }
    }
    if (g_sel >= 0 && g_sel < (int)g_list.size()) {  // nome do escolhido, embaixo do quadrado
        IDWriteTextFormat* tf = ui::Format(12, DWRITE_FONT_WEIGHT_NORMAL, false, DWRITE_TEXT_ALIGNMENT_CENTER);
        if (tf) {
            tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            tf->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            float cx = PAD_X + g_sel * PITCH + TILE / 2;
            const std::wstring& n = g_list[g_sel].name;
            br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.80f) : D2D1::ColorF(0, 0, 0, 0.70f));
            dc->DrawTextW(n.c_str(), (UINT32)n.size(), tf, D2D1::RectF(cx - 120, LABEL_CY - 10, cx + 120, LABEL_CY + 10), br);
            tf->Release();
        }
    }
    br->Release();
    ui::GpuEnd(g_s);
    if (g_glass) backdrop::Set(g_back, (float)wpx, (float)hpx, R * g_k * sc, dark ? D2D1::ColorF(0.15f, 0.15f, 0.16f, tahoe ? 0.45f : 0.62f) : D2D1::ColorF(0.96f, 0.96f, 0.95f, tahoe ? 0.30f : 0.42f));
}

void ShowNow() {
    KillTimer(g_wnd, T_SHOWNOW);
    if (!g_visible || IsWindowVisible(g_wnd)) return;
    if (!liquid::Leopard()) SetWindowPos(g_back, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);  // Leopard: sem o vidro
    SetWindowPos(g_wnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void Show() {
    if (g_visible || !g_active) return;
    Metricas();
    MONITORINFO mi{ sizeof(mi) };
    POINT c; GetCursorPos(&c);
    GetMonitorInfoW(MonitorFromPoint(c, MONITOR_DEFAULTTOPRIMARY), &mi);
    g_mon = mi.rcMonitor;
    float sc = Sc();
    float maxW = (g_mon.right - g_mon.left) * 0.94f / sc;
    g_k = std::min(1.f, maxW / PanelW());
    int wpx = (int)std::lround(PanelW() * g_k * sc), hpx = (int)std::lround(PANEL_H * g_k * sc);
    int x = (g_mon.left + g_mon.right - wpx) / 2, y = (g_mon.top + g_mon.bottom - hpx) / 2;
    // desenha antes de aparecer: o primeiro quadro já vem com os ícones
    SetWindowPos(g_wnd, HWND_TOPMOST, x, y, wpx, hpx, SWP_NOACTIVATE);
    g_visible = true;
    Paint();
    SetWindowPos(g_back, HWND_TOPMOST, x, y, wpx, hpx, SWP_NOACTIVATE);
    // Mostra só depois que o vidro redesenhado chegou à tela: a janela do vidro guarda o que tinha da última
    // vez, e aparecendo junto com o desenho novo ela mostrava a janela antiga por um quadro ou dois.
    if (g_glass) { backdrop::AfterCommit(g_wnd, WM_SHOWNOW); SetTimer(g_wnd, T_SHOWNOW, 50, nullptr); }
    else ShowNow();
}

void Hide() {
    KillTimer(g_wnd, T_SHOW);
    KillTimer(g_wnd, T_SHOWNOW);
    ShowWindow(g_wnd, SW_HIDE);
    ShowWindow(g_back, SW_HIDE);
    g_visible = false;
    g_active = false;
    for (auto& e : g_list) e.bmp = nullptr;  // continuam no cache
}

App* SelectedApp() {
    if (g_sel < 0 || g_sel >= (int)g_list.size()) return nullptr;
    return apps::FindById(g_list[g_sel].id);
}

int HitIcon(LPARAM l) {
    float s = Sc() * g_k;
    float x = (short)LOWORD(l) / s, y = (short)HIWORD(l) / s;
    if (y < PAD_TOP || y > PAD_TOP + TILE) return -1;
    int i = (int)std::floor((x - PAD_X) / PITCH);
    return i >= 0 && i < (int)g_list.size() ? i : -1;
}

void Commit() {
    App* a = SelectedApp();
    Hide();
    if (!a) return;
    if (a->wins.empty()) apps::Launch(*a);  // o Finder sem janela: abre uma
    else apps::Activate(*a);
    Log(L"seletor: %ls", a->name.c_str());
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); Paint(); return 0; }
    case WM_TIMER: if (w == T_SHOW) { KillTimer(h, T_SHOW); Show(); } else if (w == T_SHOWNOW) ShowNow(); return 0;  // segurança: o aviso não veio
    case WM_SHOWNOW: ShowNow(); return 0;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;  // o app de baixo continua com o teclado
    case WM_CLOSE: if (g_active) Hide(); return 0;  // nunca destrói (o seletor vive enquanto o Shell vive)
    case WM_MOUSEMOVE: { int i = HitIcon(l); if (i >= 0 && i != g_sel) { g_sel = i; Paint(); } return 0; }
    case WM_LBUTTONUP: { int i = HitIcon(l); if (i >= 0) { g_sel = i; Commit(); } return 0; }
    }
    return DefWindowProcW(h, m, w, l);
}
LRESULT CALLBACK BackProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

void seletor::IconsChanged() {
    for (auto& e : g_list) e.bmp = nullptr;
    for (auto& [k, b] : g_bmpCache) if (b) b->Release();
    g_bmpCache.clear();
}

bool seletor::Create(HINSTANCE hi) {
    WNDCLASSW bc{}; bc.lpfnWndProc = BackProc; bc.hInstance = hi; bc.lpszClassName = L"ShellSeletorFundo";
    RegisterClassW(&bc);
    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = hi; wc.lpszClassName = L"ShellSeletor"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    DWORD ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    g_back = CreateWindowExW(ex | WS_EX_NOREDIRECTIONBITMAP, bc.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
    g_wnd = CreateWindowExW(ex | WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, L"Seletor de Apps", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
    if (!g_wnd || !g_back) return false;
    g_glass = backdrop::Attach(g_back);
    if (!g_glass) {
        DestroyWindow(g_back);
        g_back = CreateWindowExW(ex, bc.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
        DWORD corner = 2; DwmSetWindowAttribute(g_back, 33, &corner, sizeof(corner));
        ui::Acrylic(g_back, ui::Dark() ? 0xC0201E1Eu : 0xC0F6F6F6u);
    } else { DWORD corner = 1; DwmSetWindowAttribute(g_back, 33, &corner, sizeof(corner)); }
    COLORREF none = 0xFFFFFFFE; DwmSetWindowAttribute(g_back, 34, &none, sizeof(none));
    apps::ours.push_back(g_wnd);
    apps::ours.push_back(g_back);
    g_s.hwnd = g_wnd;
    return true;
}

bool seletor::IsActive() { return g_active; }

// Tab/Shift+Tab com o Alt seguro (vem do gancho de teclado do main.cpp)
void seletor::Step(int dir) {
    if (!g_active) {
        double t0 = anim::Now();
        Collect();
        Log(L"seletor: lista em %.1f ms", anim::Now() - t0);
        if (g_list.empty()) return;
        g_active = true;
        g_sel = g_list.size() > 1 ? (dir > 0 ? 1 : (int)g_list.size() - 1) : 0;  // o app anterior
        // aparece junto com o Tab (como o Cmd+Tab do Mac); o toque rápido ainda troca de app normalmente
        double t1 = anim::Now();
        Show();
        Log(L"seletor: janela em %.1f ms", anim::Now() - t1);
        return;
    }
    int n = (int)g_list.size();
    g_sel = ((g_sel + dir) % n + n) % n;
    if (!g_visible) { KillTimer(g_wnd, T_SHOW); Show(); }  // segunda tecla: já mostra
    else Paint();
}

void seletor::Key(UINT vk) {
    if (!g_active) return;
    if (vk == VK_ESCAPE) { Hide(); return; }
    if (vk == VK_LEFT) { Step(-1); return; }
    if (vk == VK_RIGHT) { Step(1); return; }
    App* a = SelectedApp();
    if (!a) return;
    if (vk == 'Q') {  // encerra o escolhido e continua no seletor (como no Mac)
        apps::Quit(*a);
        g_list.erase(g_list.begin() + g_sel);
        if (g_list.empty()) { Hide(); return; }
        g_sel = std::min(g_sel, (int)g_list.size() - 1);
        KillTimer(g_wnd, T_SHOW);
        g_visible = false;  // o painel encolhe: mostra de novo, recentrado
        Show();
    } else if (vk == 'H') { apps::Hide(*a); }
}

void seletor::Release() { if (g_active) Commit(); }
