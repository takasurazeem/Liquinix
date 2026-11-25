// Menus do Windows no visual do Mac, em todos os programas, sem injetar nada:
//  1. O Windows avisa (WinEvent, fora do processo) quando um menu nativo (#32768) abre.
//  2. Lemos os itens pelo HMENU (MN_GETHMENU), tiramos a janela nativa da tela e mostramos a nossa.
//  3. O mouse é nosso (gancho de mouse só enquanto há menu aberto); ao escolher, selecionamos o
//     mesmo item no menu nativo (MN_SELECTITEM) e mandamos Enter: o programa recebe o comando como
//     sempre recebeu, inclusive menus que esperam a resposta direta (TrackPopupMenu com TPM_RETURNCMD).
//  4. O teclado continua indo para o menu nativo; acompanhamos o destaque pelo evento de foco.
// Programas que desenham o próprio menu (Chrome, Electron, WinUI) não usam #32768 e ficam como estão.
#include "shell.h"
#include "../menu/context_menu.h"
#include <dwmapi.h>
#include <dcomp.h>
#include <shellscalingapi.h>
#include <algorithm>
#include <cmath>
#include <atomic>
#include <mutex>
#include <thread>

namespace {

const UINT MN_SELECT = 0x01E5;  // MN_SELECTITEM (não documentada): destaca o item no menu nativo
const ULONG_PTR KEY_MARK = 0x53484C4C;  // o mesmo de main.cpp: teclas nossas não passam pelo gancho da tecla Windows
const UINT T_SUBMENU = 1, T_SUBCHECK = 2, T_CHOOSECHECK = 4, T_SHOWNOW = 5;
// rolagem pela seta do menu: quadros no ritmo da tela (ui::FrameClock), 0,375 pt/ms (os antigos 6 pt a cada 16 ms)
const UINT WM_MENU_SCROLL = WM_APP + 5;
double g_scrollAt = -1;
const UINT MN_BTNDOWN = 0x01ED, MN_BTNUP = 0x01EF;  // MN_BUTTONDOWN/UP (não documentadas): clique no item pela posição
const UINT MN_OPENHIER = 0x01E3;  // MN_OPENHIERARCHY (não documentada): abre o submenu do item destacado
const UINT WM_MENU_MOVE = WM_APP + 1, WM_MENU_CLICK = WM_APP + 2, WM_MENU_WHEEL = WM_APP + 3, WM_MENU_SHOWNOW = WM_APP + 4, WM_HOOK_ON = WM_USER + 1, WM_HOOK_OFF = WM_USER + 2;

struct Item {
    int pos;                 // posição no menu nativo
    std::wstring label, shortcut;
    bool sep = false, disabled = false, checked = false, radio = false, sub = false;
    wchar_t icon = 0;        // Tahoe: símbolo à esquerda (pelo nome do item)
    D2D1_RECT_F r{};
};

struct Level {
    HWND native = nullptr;
    HMENU menu = nullptr;
    HWND wnd = nullptr;
    HWND back = nullptr;     // só o desfoque, atrás do menu
    ui::GpuSurface s;        // GPU + desfoque nativo do Windows 11: sem destaque cortado ao meio
    bool glass = false;      // o vidro de trás é o do Shell (cantos de 12 lisos, como o menu do Tahoe)
    bool clipped = false;    // o desenho já tem o recorte arredondado
    std::vector<Item> items;
    int hover = -1;
    float w = 0, h = 0;      // DIPs (h = altura de todos os itens)
    float textL = 21;        // onde o texto começa (mais à direita com ícones)
    float view = 0;          // altura visível: menor que h quando o menu não cabe na tela e rola, como no Mac
    float scroll = 0;        // quanto já rolou (DIPs)
    float scale = 1;
};

std::vector<Level*> g_levels;
HWINEVENTHOOK g_menuHook, g_focusHook, g_createHook;
HHOOK g_mouse;
HWND g_host;                 // janela de mensagens (timers)
DWORD g_selfPid;
IDWriteTextFormat *g_tf, *g_tfR, *g_tfIcon;
int g_subWanted = -1;        // item com submenu esperando o atraso para abrir
// Submenu pedido ao nativo: se não abrir em 250 ms, tenta outros caminhos (alguns programas ignoram a tecla)
Level* g_subFrom; int g_subIdx = -1, g_subTry;
// Item escolhido esperando o programa aceitar: guardado pela janela nativa e posição (não pelo nível, que
// pode ser apagado e outro nascer no mesmo endereço); o mouse mexendo para outro item cancela
HWND g_chooseNative; int g_choosePos = -1, g_chooseIdx = -1, g_chooseTry;
float g_scaleFmt;
// O gancho de mouse roda numa thread só dele e não faz nada pesado: diz se o ponto está sobre um
// menu nosso (retângulos copiados aqui), engole os cliques e avisa a janela principal. Trabalho
// pesado dentro do gancho segura o cursor do sistema inteiro.
std::mutex g_rmu;
std::vector<RECT> g_rects;
std::atomic<bool> g_movePending;
DWORD g_hookTid;
DWORD g_lastMouse;           // última vez que o mouse mexeu (o foco do menu nativo só manda no teclado)

Level* LevelOf(HWND native) {
    for (auto* l : g_levels) if (l->native == native) return l;
    return nullptr;
}

std::wstring Clean(const wchar_t* s, std::wstring* shortcut) {
    std::wstring out;
    for (const wchar_t* p = s; *p; p++) {
        if (*p == L'\t') { if (shortcut) *shortcut = p + 1; break; }
        if (*p == L'&') { if (p[1] == L'&') { out += L'&'; p++; } continue; }
        out += *p;
    }
    return out;
}

void ReadItems(Level& l) {
    l.items.clear();
    int n = GetMenuItemCount(l.menu);
    for (int k = 0; k < n; k++) {
        wchar_t buf[512] = {};
        MENUITEMINFOW mi{ sizeof(mi) };
        mi.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_SUBMENU | MIIM_STRING;
        mi.dwTypeData = buf; mi.cch = 511;
        if (!GetMenuItemInfoW(l.menu, k, TRUE, &mi)) continue;
        Item it; it.pos = k;
        if (mi.fType & MFT_SEPARATOR) {
            if (l.items.empty() || l.items.back().sep) continue;  // sem separadores repetidos
            it.sep = true;
        } else {
            if (!buf[0]) continue;  // desenhado pelo próprio programa, sem texto: fica de fora
            it.label = Clean(buf, &it.shortcut);
            it.disabled = (mi.fState & (MFS_DISABLED | MFS_GRAYED)) != 0;
            it.checked = (mi.fState & MFS_CHECKED) != 0;
            it.radio = (mi.fType & MFT_RADIOCHECK) != 0;
            it.sub = mi.hSubMenu != nullptr;
        }
        l.items.push_back(it);
    }
    while (!l.items.empty() && l.items.back().sep) l.items.pop_back();
}

void Formats(float scale) {
    if (g_tf && g_scaleFmt == scale) return;
    if (g_tf) { g_tf->Release(); g_tfR->Release(); }
    g_tf = ui::Format(13, DWRITE_FONT_WEIGHT_NORMAL);
    g_tfR = ui::Format(13, DWRITE_FONT_WEIGHT_NORMAL, false, DWRITE_TEXT_ALIGNMENT_TRAILING);
    if (!g_tfIcon) {
        IDWriteFactory* dw = nullptr;
        if (SUCCEEDED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&dw))) {
            dw->CreateTextFormat(L"Segoe Fluent Icons", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13, L"", &g_tfIcon);
            if (g_tfIcon) { g_tfIcon->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); g_tfIcon->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); }
            dw->Release();
        }
    }
    g_scaleFmt = scale;
}

// Medidas do menu do macOS: linhas de 22, 5 de folga em cima/embaixo, texto a 21 da borda (espaço
// do ✓), destaque azul arredondado com 5 de margem, separador de 11.
const float ROW = 22, SEP = 11, PADY = 5, TEXTL = 21, TEXTR = 14, INSET = 5;
const float MENU_R_TAHOE = 12;  // canto do menu (DIP), como o menu de contexto do Tahoe
const float ARROW = 18;      // faixa com a setinha em cima/embaixo de um menu que rola
// macOS 27 Golden Gate (menu Go do Finder, captura 2x do 512 Pixels): linhas de 24, separador de 11, 5,5 de folga,
// destaque (0,122,255) com 5,5 de margem e canto 7, canto do menu 11; texto a 16 da borda (a 41 com símbolo) e
// símbolos só em poucos itens (Ajustes…), como no Golden Gate
bool GoldenMenus() { return backdrop::Liquid() && backdrop::Current().golden; }
#define MENU_R (GoldenMenus() ? 11.f : MENU_R_TAHOE)  // Golden Gate: canto de 11 (medido)
bool GoldenIcon(const std::wstring& label) {
    for (const wchar_t* k : { L"Ajustes", L"Settings", L"System Settings", L"Preferências", L"Preferences", L"Sobre Este", L"About This", L"App Store", L"Microsoft Store" })  // macOS 27: o menu da Apple tem símbolo nestes if (label.rfind(k, 0) == 0) return true;
    return false;
}

bool Scrolls(const Level& l) { return l.h > l.view + 0.5f; }
float MaxScroll(const Level& l) { return std::max(0.f, l.h - l.view); }
bool UpArrow(const Level& l) { return Scrolls(l) && l.scroll > 0.5f; }
bool DownArrow(const Level& l) { return Scrolls(l) && l.scroll < MaxScroll(l) - 0.5f; }

// Deixa o item à vista (teclado descendo um menu longo).
void Reveal(Level& l, int i) {
    if (!Scrolls(l) || i < 0) return;
    float top = l.items[i].r.top - l.scroll, bot = l.items[i].r.bottom - l.scroll;
    if (top < ARROW) l.scroll -= ARROW - top;
    else if (bot > l.view - ARROW) l.scroll += bot - (l.view - ARROW);
    l.scroll = std::clamp(l.scroll, 0.f, MaxScroll(l));
}

void Measure(Level& l) {
    Formats(l.scale);
    float maxL = 0, maxS = 0;
    bool anySub = false, anyIcon = false;
    for (auto& it : l.items) {
        if (it.sep) continue;
        it.icon = backdrop::Liquid() && (!GoldenMenus() || GoldenIcon(it.label)) ? ctxmenu::AutoIcon(it.label) : 0;  // Tahoe: símbolo em cada item
        anyIcon |= it.icon != 0;
        maxL = std::max(maxL, ui::TextWidth(g_tf, it.label));
        if (!it.shortcut.empty()) maxS = std::max(maxS, ui::TextWidth(g_tf, it.shortcut));
        anySub |= it.sub;
    }
    l.textL = GoldenMenus() ? (anyIcon ? 41.f : 16.f) : anyIcon ? 34.f : TEXTL;
    l.w = std::max(160.f, l.textL + maxL + (maxS > 0 ? 30 + maxS : 0) + (anySub ? 22 : 0) + TEXTR);
    bool tahoe = backdrop::Liquid();  // medidas do menu do Tahoe: linhas de 20, separador de 9, 6 de folga
    bool golden = GoldenMenus();
    float pad = golden ? 5.5f : tahoe ? 6.f : PADY;
    float y = pad;
    for (auto& it : l.items) {
        float h = it.sep ? (golden ? 11.f : tahoe ? 9.f : SEP) : (golden ? 24.f : tahoe ? 20.f : ROW);
        it.r = D2D1::RectF(0, y, l.w, y + h);
        y += h;
    }
    l.h = y + pad;
    l.view = l.h;
}

void Paint(Level& l) {
    int wpx = (int)std::ceil(l.w * l.scale), hpx = (int)std::ceil(l.view * l.scale);
    auto* rt = ui::GpuBegin(l.s, wpx, hpx, l.scale);
    if (!rt) return;
    bool dark = ui::Dark();
    ID2D1SolidColorBrush* br = nullptr;
    rt->CreateSolidColorBrush(dark ? D2D1::ColorF(0.16f, 0.16f, 0.17f, 0.40f) : D2D1::ColorF(0.97f, 0.97f, 0.97f, 0.45f), &br);
    rt->FillRectangle(D2D1::RectF(0, 0, l.w, l.view), br);
    bool up = UpArrow(l), down = DownArrow(l);
    D2D1_MATRIX_3X2_F base; rt->GetTransform(&base);
    rt->PushAxisAlignedClip(D2D1::RectF(0, up ? ARROW : 0, l.w, l.view - (down ? ARROW : 0)), D2D1_ANTIALIAS_MODE_ALIASED);
    rt->SetTransform(D2D1::Matrix3x2F::Translation(0, -l.scroll) * base);
    D2D1_COLOR_F text = dark ? D2D1::ColorF(1, 1, 1, 0.92f) : D2D1::ColorF(0, 0, 0, 0.88f);
    D2D1_COLOR_F dim = dark ? D2D1::ColorF(1, 1, 1, 0.30f) : D2D1::ColorF(0, 0, 0, 0.28f);
    D2D1_COLOR_F gray = dark ? D2D1::ColorF(1, 1, 1, 0.45f) : D2D1::ColorF(0, 0, 0, 0.45f);
    for (int i = 0; i < (int)l.items.size(); i++) {
        auto& it = l.items[i];
        if (it.sep) {
            float y = std::floor((it.r.top + it.r.bottom) / 2) + 0.5f;
            br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.12f) : D2D1::ColorF(0, 0, 0, 0.10f));
            rt->DrawLine({ INSET + 5, y }, { l.w - INSET - 5, y }, br, 1);
            continue;
        }
        bool hot = i == l.hover && !it.disabled;
        if (hot) {
            bool golden = GoldenMenus();
            br->SetColor(golden ? D2D1::ColorF(0, 122 / 255.f, 1, 1) : D2D1::ColorF(0.04f, 0.40f, 0.95f, 1));  // azul de destaque do Mac
            float in = golden ? 5.5f : INSET, rad = golden ? 7.f : 4.f;
            rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(in, it.r.top, l.w - in, it.r.bottom), rad, rad), br);
        }
        br->SetColor(it.disabled ? dim : hot ? D2D1::ColorF(1, 1, 1, 1) : text);
        float cy = (it.r.top + it.r.bottom) / 2;
        if (it.checked) {
            if (it.radio) rt->FillEllipse(D2D1::Ellipse({ 12, cy }, 3, 3), br);
            else {
                rt->DrawLine({ 8, cy }, { 11, cy + 3.5f }, br, 1.6f);
                rt->DrawLine({ 11, cy + 3.5f }, { 16.5f, cy - 4 }, br, 1.6f);
            }
        }
        if (it.icon && !it.checked && g_tfIcon) {
            wchar_t g[2] = { it.icon, 0 };
            rt->DrawTextW(g, 1, g_tfIcon, D2D1::RectF(13, it.r.top, 33, it.r.bottom), br);
        }
        rt->DrawTextW(it.label.c_str(), (UINT32)it.label.size(), g_tf, D2D1::RectF(l.textL, it.r.top, l.w - TEXTR, it.r.bottom), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        if (!it.shortcut.empty()) {
            if (!hot) br->SetColor(it.disabled ? dim : gray);
            rt->DrawTextW(it.shortcut.c_str(), (UINT32)it.shortcut.size(), g_tfR, D2D1::RectF(l.textL, it.r.top, l.w - TEXTR - (it.sub ? 16 : 0), it.r.bottom), br);
        }
        if (it.sub) {  // ›
            float x = l.w - TEXTR - 4;
            br->SetColor(it.disabled ? dim : hot ? D2D1::ColorF(1, 1, 1, 1) : text);
            rt->DrawLine({ x - 3, cy - 4 }, { x + 1, cy }, br, 1.5f);
            rt->DrawLine({ x + 1, cy }, { x - 3, cy + 4 }, br, 1.5f);
        }
    }
    rt->SetTransform(base);
    rt->PopAxisAlignedClip();
    br->SetColor(text);  // setinhas de rolagem, como no Mac
    float cx = l.w / 2;
    if (up) {
        D2D1_POINT_2F a{ cx - 4, ARROW / 2 + 2 }, b{ cx, ARROW / 2 - 2 }, c{ cx + 4, ARROW / 2 + 2 };
        rt->DrawLine(a, b, br, 1.5f); rt->DrawLine(b, c, br, 1.5f);
    }
    if (down) {
        float yy = l.view - ARROW / 2;
        D2D1_POINT_2F a{ cx - 4, yy - 2 }, b{ cx, yy + 2 }, c{ cx + 4, yy - 2 };
        rt->DrawLine(a, b, br, 1.5f); rt->DrawLine(b, c, br, 1.5f);
    }
    if (l.glass) {  // contorno fino no canto arredondado (no lugar da borda do Windows)
        br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.14f) : D2D1::ColorF(0, 0, 0, 0.12f));
        float in = 0.5f / l.scale, rad = MENU_R - in;
        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(in, in, l.w - in, l.view - in), rad, rad), br, 1.f / l.scale);
    }
    br->Release();
    ui::GpuEnd(l.s);
    if (l.glass && !l.clipped && l.s.visual) {  // o desenho recortado no mesmo canto do vidro
        IDCompositionDevice* dev = ui::Comp();
        IDCompositionRectangleClip* clip = nullptr;
        if (dev && SUCCEEDED(dev->CreateRectangleClip(&clip))) {
            float r = MENU_R * l.scale;
            clip->SetLeft(0.f); clip->SetTop(0.f);
            clip->SetRight(std::ceil(l.w * l.scale)); clip->SetBottom(std::ceil(l.view * l.scale));
            clip->SetTopLeftRadiusX(r); clip->SetTopLeftRadiusY(r); clip->SetTopRightRadiusX(r); clip->SetTopRightRadiusY(r);
            clip->SetBottomLeftRadiusX(r); clip->SetBottomLeftRadiusY(r); clip->SetBottomRightRadiusX(r); clip->SetBottomRightRadiusY(r);
            l.s.visual->SetClip(clip);
            dev->Commit();
            clip->Release();
            l.clipped = true;
        }
    }
}

LRESULT CALLBACK PopupProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (m == WM_PAINT) {
        PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps);
        for (auto* lv : g_levels) if (lv->wnd == h) Paint(*lv);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void UpdateRects() {
    std::lock_guard<std::mutex> lk(g_rmu);
    g_rects.clear();
    for (auto* l : g_levels) { RECT r; GetWindowRect(l->wnd, &r); g_rects.push_back(r); }
}

void CloseLevel(Level* l) {
    if (l->glass && l->back) backdrop::Detach(l->back);
    ui::GpuFree(l->s);
    if (l->wnd) DestroyWindow(l->wnd);
    if (l->back) DestroyWindow(l->back);
    g_levels.erase(std::remove(g_levels.begin(), g_levels.end(), l), g_levels.end());
    delete l;
    UpdateRects();
}

void StopHooks();
void ShowNative(HWND hwnd);

// Fechando o menu inteiro (escolheu, Esc, clique fora): some em fade, como no Mac (~150 ms, desacelerando
// no fim). Submenus trocados com o mouse fecham na hora (CloseLevel), também como no Mac.
void FadeAll() {
    if (g_levels.empty() || shell::Setting(L"menus_fade", L"1") != L"1") return;
    IDCompositionDevice* dev = ui::Comp();
    std::vector<IDCompositionEffectGroup*> fx;
    for (auto* l : g_levels) {
        IDCompositionEffectGroup* e = nullptr;
        if (dev && l->s.visual && SUCCEEDED(dev->CreateEffectGroup(&e))) { l->s.visual->SetEffect(e); fx.push_back(e); }
    }
    const double D = 150;
    double t0 = anim::Now();
    for (;;) {
        float p = (float)std::min(1.0, (anim::Now() - t0) / D);
        float a = (1 - p) * (1 - p);
        for (auto* e : fx) e->SetOpacity(a);
        for (auto* l : g_levels) if (l->glass && l->back) backdrop::Fade(l->back, a, 1, 0, 0);
        if (dev) dev->Commit();
        if (p >= 1) break;
        DwmFlush();
    }
    for (auto* e : fx) e->Release();
}

void CloseAll(bool fade = true) {
    if (fade) FadeAll();
    while (!g_levels.empty()) CloseLevel(g_levels.back());
    StopHooks();
}

// Item com o mouse em cima, em qualquer nível (devolve o nível e o índice).
bool HitAt(POINT pt, Level** lv, int* idx) {
    for (int k = (int)g_levels.size() - 1; k >= 0; k--) {
        Level* l = g_levels[k];
        RECT wr; GetWindowRect(l->wnd, &wr);
        if (!PtInRect(&wr, pt)) continue;
        *lv = l;
        *idx = -1;
        float y = (pt.y - wr.top) / l->scale;
        if ((UpArrow(*l) && y < ARROW) || (DownArrow(*l) && y >= l->view - ARROW)) return true;  // na setinha: rola, não escolhe
        y += l->scroll;
        for (int i = 0; i < (int)l->items.size(); i++)
            if (!l->items[i].sep && y >= l->items[i].r.top && y < l->items[i].r.bottom) *idx = i;
        return true;
    }
    return false;
}

// Tecla direto na janela do menu nativo (o laço do menu lê a fila da thread dele): funciona mesmo quando
// o teclado não está com aquele programa, como menus de plug-ins (Omnisphere dentro do Ableton), onde a
// tecla simulada ia para outra janela e o submenu nunca abria.
void NativeKey(HWND native, WORD vk) {
    LPARAM down = 1 | (MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) << 16);
    if (vk == VK_LEFT || vk == VK_RIGHT) down |= 1 << 24;  // tecla estendida
    PostMessageW(native, WM_KEYDOWN, vk, down);
    PostMessageW(native, WM_KEYUP, vk, down | (1u << 30) | (1u << 31));
}

void Key(WORD vk) {
    INPUT in[2]{};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = vk;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    in[0].ki.dwExtraInfo = in[1].ki.dwExtraInfo = KEY_MARK;
    SendInput(2, in, sizeof(INPUT));
}

// Destaca o item no menu nativo também (o teclado continua coerente).
void SelectNative(Level* l, int idx) {
    // níveis abertos abaixo deste fecham antes (Esquerda fecha o submenu mais fundo)
    while (g_levels.back() != l) { NativeKey(g_levels.back()->native, VK_LEFT); CloseLevel(g_levels.back()); }
    PostMessageW(l->native, MN_SELECT, idx >= 0 ? l->items[idx].pos : (WPARAM)-1, 0);  // sem esperar o outro programa
}

void OpenSub(Level* l, int idx) {
    SelectNative(l, idx);
    HMENU sub = GetSubMenu(l->menu, l->items[idx].pos);
    Log(L"menu: abrindo submenu \"%ls\" (%d itens no nativo)", l->items[idx].label.c_str(), sub ? GetMenuItemCount(sub) : -1);
    NativeKey(l->native, VK_RIGHT);
    g_subFrom = l; g_subIdx = idx; g_subTry = 0;
    SetTimer(g_host, T_SUBCHECK, 250, nullptr);
}

// Itens do menu da Área de Trabalho (Explorer) que abrem o Ajustes do Windows: vão para o nosso Ajustes
// do Sistema, na página certa. "" = não é um desses.
std::wstring OurSettingsPage(Level* l, const Item& it) {
    DWORD pid = 0; GetWindowThreadProcessId(l->native, &pid);
    HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    wchar_t path[MAX_PATH] = {}; DWORD n = MAX_PATH;
    bool explorer = hp && QueryFullProcessImageNameW(hp, 0, path, &n) && !_wcsicmp(wcsrchr(path, L'\\') ? wcsrchr(path, L'\\') + 1 : path, L"explorer.exe");
    if (hp) CloseHandle(hp);
    if (!explorer) return L"";
    std::wstring t;
    for (wchar_t c : it.label) if (c != L'&') t += towlower(c);
    if (t == L"display settings" || t == L"configurações de exibição" || t == L"configurações de vídeo" || t == L"configurações da tela") return L"telas";
    if (t == L"personalize" || t == L"personalizar") return L"fundo";
    return L"";
}

void Choose(Level* l, int idx) {
    auto& it = l->items[idx];
    if (it.disabled || it.sep) return;
    if (std::wstring page = OurSettingsPage(l, it); !page.empty()) {  // o nosso Ajustes no lugar do Windows
        Log(L"menu: \"%ls\" -> Ajustes do Sistema (%ls)", it.label.c_str(), page.c_str());
        for (size_t k = 0; k < g_levels.size(); k++) Key(VK_ESCAPE);  // fecha o menu nativo sem escolher nada
        shell::OpenSettings(page);
        return;
    }
    SelectNative(l, idx);
    if (it.sub) { OpenSub(l, idx); return; }  // o nativo abre o submenu; o nosso aparece no evento
    // pisca o item escolhido, como no Mac, e confirma no nativo
    l->hover = -1; Paint(*l); Sleep(70);  // apaga e acende uma vez (o tempo do Mac)
    l->hover = idx; Paint(*l);
    // Confirma no nativo. Alguns programas ignoram a tecla mandada para a janela do menu (Omnisphere): se o
    // menu ainda estiver aberto em 250 ms, tenta o clique interno do menu e depois a tecla simulada.
    Log(L"menu: escolhido \"%ls\"", it.label.c_str());
    NativeKey(l->native, VK_RETURN);
    g_chooseNative = l->native; g_choosePos = it.pos; g_chooseIdx = idx; g_chooseTry = 0;
    SetTimer(g_host, T_CHOOSECHECK, 250, nullptr);
}

LRESULT CALLBACK MouseHook(int code, WPARAM w, LPARAM lp) {
    if (code == HC_ACTION) {
        auto* ms = (MSLLHOOKSTRUCT*)lp;
        bool over = false;
        { std::lock_guard<std::mutex> lk(g_rmu); for (auto& r : g_rects) if (PtInRect(&r, ms->pt)) { over = true; break; } }
        if (w == WM_MOUSEWHEEL && over) {  // roda do mouse rola o menu longo
            PostMessageW(g_host, WM_MENU_WHEEL, (WPARAM)(short)HIWORD(ms->mouseData), MAKELPARAM(ms->pt.x, ms->pt.y));
            return 1;
        }
        if (w == WM_MOUSEMOVE) {
            if (!g_movePending.exchange(true)) PostMessageW(g_host, WM_MENU_MOVE, 0, 0);  // um aviso por vez
        } else if (over) {
            if (w == WM_LBUTTONUP || w == WM_RBUTTONUP) PostMessageW(g_host, WM_MENU_CLICK, ms->pt.x, ms->pt.y);
            return 1;  // clique nosso: o menu nativo não vê
        }
        // fora dos nossos: segue (o nativo, fora da tela, vê o clique fora e fecha)
    }
    return CallNextHookEx(nullptr, code, w, lp);
}

void HookThread() {
    MSG m;
    PeekMessageW(&m, nullptr, 0, 0, PM_NOREMOVE);  // cria a fila de mensagens da thread
    g_hookTid = GetCurrentThreadId();
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (m.message == WM_HOOK_ON && !g_mouse) g_mouse = SetWindowsHookExW(WH_MOUSE_LL, MouseHook, GetModuleHandleW(nullptr), 0);
        if (m.message == WM_HOOK_OFF && g_mouse) { UnhookWindowsHookEx(g_mouse); g_mouse = nullptr; }
    }
}

// Mouse numa setinha: rola sozinho (timer), como no Mac.
Level* ArrowUnder(POINT pt, int* dir) {
    for (int k = (int)g_levels.size() - 1; k >= 0; k--) {
        Level* l = g_levels[k];
        RECT wr; GetWindowRect(l->wnd, &wr);
        if (!PtInRect(&wr, pt)) continue;
        float y = (pt.y - wr.top) / l->scale;
        if (UpArrow(*l) && y < ARROW) { *dir = -1; return l; }
        if (DownArrow(*l) && y >= l->view - ARROW) { *dir = 1; return l; }
        return nullptr;
    }
    return nullptr;
}

void ScrollBy(Level* l, float dy) {
    float v = std::clamp(l->scroll + dy, 0.f, MaxScroll(*l));
    if (v == l->scroll) return;
    l->scroll = v;
    // submenus abertos a partir deste nível fecham (o item que os abriu saiu do lugar)
    while (g_levels.back() != l) { NativeKey(g_levels.back()->native, VK_LEFT); CloseLevel(g_levels.back()); }
    Paint(*l);
}

void OnMove() {
    g_movePending = false;
    g_lastMouse = GetTickCount();
    POINT pt; GetCursorPos(&pt);
    { int dir = 0; if (ArrowUnder(pt, &dir) && g_scrollAt < 0) { g_scrollAt = anim::Now(); ui::FrameClock(g_host, WM_MENU_SCROLL, true); } }
    Level* lv = nullptr; int idx = -1;
    if (!HitAt(pt, &lv, &idx)) {  // fora: tira o destaque do nível mais fundo (como no Mac)
        if (!g_levels.empty() && g_levels.back()->hover >= 0 && !g_levels.back()->items[g_levels.back()->hover].sub) {
            g_levels.back()->hover = -1; Paint(*g_levels.back());
        }
        return;
    }
    if (idx == lv->hover) return;
    KillTimer(g_host, T_CHOOSECHECK);
    lv->hover = idx;
    Paint(*lv);
    if (idx >= 0) SelectNative(lv, idx);
    KillTimer(g_host, T_SUBMENU);
    g_subWanted = -1;
    if (idx >= 0 && lv->items[idx].sub && !lv->items[idx].disabled) {  // submenu abre com um pequeno atraso
        g_subWanted = idx;
        SetTimer(g_host, T_SUBMENU, 200, nullptr);
    }
}

void OnClick(POINT pt) {
    Level* lv = nullptr; int idx = -1;
    if (HitAt(pt, &lv, &idx) && idx >= 0) Choose(lv, idx);
}

void OpenLevel(HWND native, HMENU menu, RECT nr) {
    auto* l = new Level;
    l->native = native;
    l->menu = menu;
    ReadItems(*l);
    if (l->items.empty()) {
        Log(L"menu: sem itens com texto (%d no nativo), fica o nativo", GetMenuItemCount(menu));
        SetPropW(native, L"ShellMenuLivre", (HANDLE)1);  // antes de mover: a DLL não o manda de volta para fora
        if (!g_levels.empty()) {  // o pai está fora da tela: o nativo vem para perto do nosso
            RECT pr; GetWindowRect(g_levels.back()->wnd, &pr);
            SetWindowPos(native, HWND_TOPMOST, pr.right - 4, pr.top, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        } else SetWindowPos(native, nullptr, nr.left, nr.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);  // volta ao lugar
        ShowNative(native);
        delete l; return;
    }
    POINT anchor{ nr.left, nr.top };
    // submenu: ao lado do item do nível de cima
    if (!g_levels.empty()) {
        Level* p = g_levels.back();
        RECT pr; GetWindowRect(p->wnd, &pr);
        float top = p->hover >= 0 ? p->items[p->hover].r.top - p->scroll : 0;
        anchor = { pr.right - (LONG)(4 * p->scale), pr.top + (LONG)((top - PADY) * p->scale) };
    }
    HMONITOR mon = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    UINT dx = 96, dy = 96; GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    l->scale = dx / 96.f;
    Measure(*l);
    MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(mon, &mi);
    float room = (mi.rcWork.bottom - mi.rcWork.top) / l->scale - 8;  // não cabe: fica da altura da tela e rola
    if (l->h > room) l->view = room;
    int wpx = (int)std::ceil(l->w * l->scale), hpx = (int)std::ceil(l->view * l->scale);
    int x = anchor.x, y = anchor.y;
    if (x + wpx > mi.rcWork.right) {  // não cabe à direita: submenu abre para a esquerda do pai; menu encosta na borda
        if (g_levels.empty()) x = mi.rcWork.right - wpx;
        else { RECT pr; GetWindowRect(g_levels.back()->wnd, &pr); x = pr.left - wpx + (int)(4 * l->scale); }
    }
    if (y + hpx > mi.rcWork.bottom) y = mi.rcWork.bottom - (int)(4 * l->scale) - hpx;
    if (y < mi.rcWork.top) y = mi.rcWork.top + (int)(4 * l->scale);
    // tira o nativo da tela (continua aberto, recebendo o teclado)
    if (!SetWindowPos(native, nullptr, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE)) { ShowNative(native); delete l; return; }  // programa elevado: fica o nativo
    // Duas janelas: atrás, só o desfoque (acrílico); na frente, o menu desenhado na GPU. O desfoque
    // não pode ficar na mesma janela: ele seria pintado por cima do conteúdo da GPU.
    // Vidro do Shell (o mesmo do Dock e do seletor): recorte arredondado liso de 12, como o menu do Tahoe.
    // Sem ele, o acrílico do Windows com o canto de 8 do DWM (o jeito antigo).
    l->back = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_NOREDIRECTIONBITMAP, L"ShellMenuMac", L"", WS_POPUP,
                              x, y, wpx, hpx, nullptr, nullptr, shell::inst, nullptr);
    l->glass = backdrop::Attach(l->back);
    DWORD corner = l->glass ? 1 /*sem arredondar*/ : 2;
    if (!l->glass) {
        DestroyWindow(l->back);
        l->back = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, L"ShellMenuMac", L"", WS_POPUP,
                                  x, y, wpx, hpx, nullptr, nullptr, shell::inst, nullptr);
        DwmSetWindowAttribute(l->back, 33, &corner, sizeof(corner));
        ui::Acrylic(l->back, ui::Dark() ? 0x60201E1Eu : 0x70F4F4F4u);
    } else {
        DwmSetWindowAttribute(l->back, 33, &corner, sizeof(corner));
        // bem fosco e com tinta firme, como os menus de contexto: o texto não pode disputar com o que está atrás
        // (a "Transparência do vidro" no mínimo deixava o menu quase nítido)
        std::vector<backdrop::Piece> pcs{ { D2D1::RectF(0, 0, (float)wpx, (float)hpx), MENU_R * l->scale } };
        backdrop::SetPieces(l->back, (float)wpx, (float)hpx, pcs,
                            ui::Dark() ? D2D1::ColorF(0.13f, 0.13f, 0.14f, 0.62f) : D2D1::ColorF(0.97f, 0.97f, 0.98f, 0.66f), 18.f);
    }
    l->wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, L"ShellMenuMac", L"", WS_POPUP,
                             x, y, wpx, hpx, nullptr, nullptr, shell::inst, nullptr);
    l->s.hwnd = l->wnd;
    DwmSetWindowAttribute(l->wnd, 33, &corner, sizeof(corner));
    COLORREF border = l->glass ? 0xFFFFFFFE /*DWMWA_COLOR_NONE*/ : ui::Dark() ? RGB(72, 72, 74) : RGB(198, 198, 202);
    DwmSetWindowAttribute(l->wnd, 34, &border, sizeof(border));
    g_levels.push_back(l);
    Paint(*l);  // desenha antes de aparecer: sem um quadro vazio piscando
    UpdateRects();
    // Com o vidro do Shell: mostra só depois que o vidro desenhado chegou à tela (em menu grande ele demora
    // mais, e aparecendo antes o menu piscava transparente). Sem resposta em 60 ms, mostra assim mesmo.
    if (l->glass) { backdrop::AfterCommit(g_host, WM_MENU_SHOWNOW); SetTimer(g_host, T_SHOWNOW, 60, nullptr); }
    else { ShowWindow(l->back, SW_SHOWNOACTIVATE); ShowWindow(l->wnd, SW_SHOWNOACTIVATE); }
}

void CALLBACK OnFocus(HWINEVENTHOOK, DWORD, HWND hwnd, LONG obj, LONG child, DWORD, DWORD) {
    // teclado no menu nativo: o destaque muda e o nosso acompanha (child = posição + 1)
    Level* l = LevelOf(hwnd);
    if (!l || obj != OBJID_CLIENT || child <= 0) return;
    if (GetTickCount() - g_lastMouse < 400) return;  // é o eco do nosso próprio destaque: o mouse manda
    for (int i = 0; i < (int)l->items.size(); i++)
        if (l->items[i].pos == child - 1 && l->hover != i) { l->hover = i; Reveal(*l, i); Paint(*l); }
}

// Submenu grande (centenas de itens): o Windows leva um tempo montando e mostra a janela antes do aviso
// de "abriu" chegar até nós, e ela piscava no canto. Enquanto há menu nosso aberto, todo menu nativo novo
// fica invisível (alfa 0) assim que a janela nasce; se não der para trocar pelo nosso, volta a aparecer.
void CALLBACK OnCreate(HWINEVENTHOOK, DWORD, HWND hwnd, LONG obj, LONG, DWORD, DWORD) {
    if (obj != OBJID_WINDOW || !hwnd) return;
    wchar_t cls[16] = {}; GetClassNameW(hwnd, cls, 16);
    if (wcscmp(cls, L"#32768")) return;
    LONG_PTR ex = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if (!(ex & WS_EX_LAYERED)) SetWindowLongPtrW(hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
    SetLayeredWindowAttributes(hwnd, 0, 0, LWA_ALPHA);
}

void ShowNative(HWND hwnd) {  // o nativo fica de verdade (não conseguimos ler os itens)
    SetPropW(hwnd, L"ShellMenuLivre", (HANDLE)1);  // a DLL dentro do programa para de mandá-lo para fora da tela
    BYTE a = 0; DWORD fl = 0;
    if (GetLayeredWindowAttributes(hwnd, nullptr, &a, &fl) && (fl & LWA_ALPHA) && a == 0) SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
}

void StartHooks() {
    if (g_hookTid) PostThreadMessageW(g_hookTid, WM_HOOK_ON, 0, 0);
    if (!g_createHook) g_createHook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_CREATE, nullptr, OnCreate, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!g_focusHook) g_focusHook = SetWinEventHook(EVENT_OBJECT_FOCUS, EVENT_OBJECT_FOCUS, nullptr, OnFocus, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
}

void StopHooks() {
    if (g_hookTid) PostThreadMessageW(g_hookTid, WM_HOOK_OFF, 0, 0);
    if (g_focusHook) { UnhookWinEvent(g_focusHook); g_focusHook = nullptr; }
    if (g_createHook) { UnhookWinEvent(g_createHook); g_createHook = nullptr; }
    KillTimer(g_host, T_SUBMENU);
    KillTimer(g_host, T_SUBCHECK);
    KillTimer(g_host, T_CHOOSECHECK);
    ui::FrameClock(g_host, WM_MENU_SCROLL, false); g_scrollAt = -1;
}

void CALLBACK OnMenu(HWINEVENTHOOK, DWORD ev, HWND hwnd, LONG, LONG, DWORD, DWORD) {
    switch (ev) {
    case EVENT_SYSTEM_MENUPOPUPSTART: {
        wchar_t cls[16] = {}; GetClassNameW(hwnd, cls, 16);
        if (wcscmp(cls, L"#32768")) return;
        DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
        if (pid == g_selfPid) return;
        // Tira o nativo da tela já, antes de perguntar qualquer coisa ao programa: a pergunta leva alguns
        // milissegundos e o menu do Windows piscava no lugar dele (no Serum, no canto de cima à esquerda).
        RECT nr; GetWindowRect(hwnd, &nr);
        // a DLL dentro do programa já o mandou para fora da tela antes de aparecer: o lugar dele ficou guardado
        if (HANDLE px = GetPropW(hwnd, L"ShellMenuX")) {
            int ox = (int)(INT_PTR)px - 40000, oy = (int)(INT_PTR)GetPropW(hwnd, L"ShellMenuY") - 40000;
            OffsetRect(&nr, ox - nr.left, oy - nr.top);
        }
        bool moved = SetWindowPos(hwnd, nullptr, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) != 0;
        DWORD_PTR r = 0;
        BOOL ok = SendMessageTimeoutW(hwnd, MN_GETHMENU, 0, 0, SMTO_ABORTIFHUNG, 600, &r);
        Log(L"menu: nativo abriu (pid %lu, nível %d, %ld,%ld %ldx%ld, hmenu %p, %d itens%ls)", pid, (int)g_levels.size(), nr.left, nr.top,
            nr.right - nr.left, nr.bottom - nr.top, (void*)r, r && IsMenu((HMENU)r) ? GetMenuItemCount((HMENU)r) : -1, ok ? L"" : L", sem resposta");
        if (!ok || !r || !IsMenu((HMENU)r)) {
            // submenu que não conseguimos ler: o nativo nasceu ao lado do pai, que está fora da tela; traz para perto do nosso
            SetPropW(hwnd, L"ShellMenuLivre", (HANDLE)1);  // antes de mover: a DLL não o manda de volta para fora
            if (!g_levels.empty()) {
                RECT pr; GetWindowRect(g_levels.back()->wnd, &pr);
                SetWindowPos(hwnd, HWND_TOPMOST, pr.right - 4, pr.top, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
            } else if (moved) SetWindowPos(hwnd, nullptr, nr.left, nr.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            ShowNative(hwnd);
            return;
        }
        StartHooks();
        OpenLevel(hwnd, (HMENU)r, nr);
        break;
    }
    case EVENT_SYSTEM_MENUPOPUPEND:
        if (Level* l = LevelOf(hwnd)) {
            while (!g_levels.empty() && g_levels.back() != l) CloseLevel(g_levels.back());
            CloseLevel(l);
            if (g_levels.empty()) StopHooks();
        }
        break;
    case EVENT_SYSTEM_MENUEND:
        if (!g_levels.empty()) CloseAll();
        break;
    }
}

LRESULT CALLBACK HostProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MENU_MOVE) { OnMove(); return 0; }
    if (m == WM_MENU_CLICK) { OnClick({ (LONG)w, (LONG)l }); return 0; }
    if (m == WM_MENU_SHOWNOW || (m == WM_TIMER && w == T_SHOWNOW)) {  // o vidro já está na tela: mostra os níveis novos
        if (m == WM_TIMER) KillTimer(h, T_SHOWNOW);
        for (auto* lv : g_levels)
            if (!IsWindowVisible(lv->wnd)) { ShowWindow(lv->back, SW_SHOWNOACTIVATE); ShowWindow(lv->wnd, SW_SHOWNOACTIVATE); }
        return 0;
    }
    if (m == WM_MENU_WHEEL) {
        POINT pt{ (short)LOWORD(l), (short)HIWORD(l) };
        Level* lv = nullptr;
        for (auto* x : g_levels) { RECT wr; GetWindowRect(x->wnd, &wr); if (PtInRect(&wr, pt)) lv = x; }
        if (lv && Scrolls(*lv)) {
            ScrollBy(lv, -(float)(short)w / WHEEL_DELTA * ROW * 3);
            Level* hit = nullptr; int idx = -1;
            if (HitAt(pt, &hit, &idx) && hit == lv) { lv->hover = idx; Paint(*lv); }
        }
        return 0;
    }
    if (m == WM_MENU_SCROLL) {
        ui::FrameAck(h, WM_MENU_SCROLL);
        POINT pt; GetCursorPos(&pt);
        int dir = 0;
        Level* lv = ArrowUnder(pt, &dir);
        if (!lv || g_scrollAt < 0) { ui::FrameClock(h, WM_MENU_SCROLL, false); g_scrollAt = -1; return 0; }
        double now = anim::Now(), dt = std::min(50.0, now - g_scrollAt);
        g_scrollAt = now;
        lv->hover = -1;
        ScrollBy(lv, dir * 0.375f * (float)dt);
        return 0;
    }
    if (m == WM_TIMER && w == T_CHOOSECHECK) {
        KillTimer(h, T_CHOOSECHECK);
        HWND nat = g_chooseNative;
        Level* l = LevelOf(nat);
        // fechou (o programa recebeu) ou o mouse já está em outro item: não insiste
        if (!l || !IsWindow(nat) || !IsWindowVisible(nat) || l->hover != g_chooseIdx) return 0;
        int pos = g_choosePos;
        g_chooseTry++;
        if (g_chooseTry == 1) {
            Log(L"menu: a tecla não escolheu; tentando o clique interno do menu");
            PostMessageW(l->native, MN_SELECT, pos, 0);
            PostMessageW(l->native, MN_BTNDOWN, pos, 0);
            PostMessageW(l->native, MN_BTNUP, pos, 0);
        } else {
            Log(L"menu: o programa não aceitou a escolha");
            return 0;
        }
        SetTimer(h, T_CHOOSECHECK, 250, nullptr);
        return 0;
    }
    if (m == WM_TIMER && w == T_SUBCHECK) {
        KillTimer(h, T_SUBCHECK);
        Level* l = g_subFrom;
        bool alive = std::find(g_levels.begin(), g_levels.end(), l) != g_levels.end();
        if (!alive || g_levels.back() != l || l->hover != g_subIdx) return 0;  // abriu (há nível novo) ou o mouse saiu
        g_subTry++;
        if (g_subTry == 1) {
            Log(L"menu: submenu não abriu pela tecla; tentando MN_OPENHIERARCHY");
            PostMessageW(l->native, MN_SELECT, l->items[g_subIdx].pos, 0);
            PostMessageW(l->native, MN_OPENHIER, 0, 0);
        } else if (g_subTry == 2) {
            Log(L"menu: tentando tecla simulada");
            PostMessageW(l->native, MN_SELECT, l->items[g_subIdx].pos, 0);
            Key(VK_RIGHT);
        } else {
            Log(L"menu: submenu não abriu");
            return 0;
        }
        SetTimer(h, T_SUBCHECK, 250, nullptr);
        return 0;
    }
    if (m == WM_TIMER && w == T_SUBMENU) {
        KillTimer(h, T_SUBMENU);
        if (!g_levels.empty() && g_subWanted >= 0) {
            Level* lv = g_levels.back();
            // o pedido é sempre do nível de cima com o mouse; acha qual
            for (auto* x : g_levels) if (x->hover == g_subWanted && x->items[g_subWanted].sub) lv = x;
            if (lv->hover == g_subWanted) OpenSub(lv, g_subWanted);
        }
        g_subWanted = -1;
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

void menus::Start() {
    if (shell::Setting(L"menus_do_windows", L"1") != L"1") return;
    g_selfPid = GetCurrentProcessId();
    WNDCLASSW pc{}; pc.lpfnWndProc = PopupProc; pc.hInstance = shell::inst; pc.lpszClassName = L"ShellMenuMac";
    pc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&pc);
    WNDCLASSW hc{}; hc.lpfnWndProc = HostProc; hc.hInstance = shell::inst; hc.lpszClassName = L"ShellMenusHost";
    RegisterClassW(&hc);
    g_host = CreateWindowExW(0, hc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, shell::inst, nullptr);
    std::thread(HookThread).detach();
    g_menuHook = SetWinEventHook(EVENT_SYSTEM_MENUSTART, EVENT_SYSTEM_MENUPOPUPEND, nullptr, OnMenu, 0, 0,
                                 WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    Log(L"menus do Windows: interceptando (%ls)", g_menuHook ? L"ok" : L"falhou");
}

void menus::Stop() {
    CloseAll(false);
    if (g_menuHook) UnhookWinEvent(g_menuHook);
    g_menuHook = nullptr;
}
