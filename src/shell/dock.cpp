// Dock no estilo do macOS Sequoia. Duas janelas: o fundo (acrílico arredondado) e, por cima, uma
// janela com alfa por pixel onde ficam os ícones (podem crescer para fora do fundo na ampliação;
// onde não há nada desenhado, o clique passa). Ver docs/dock-e-barra-de-menus.md.
#include "shell.h"
#include "../menu/context_menu.h"
#include "../core/curves.h"
#include "../core/dock_drawing.h"
#include "../core/dock_layout.h"
#include "../platform/win/painter_d2d.h"
#include <shellapi.h>
#include <shlobj.h>
#include <dwmapi.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <thread>
#include "../comum/idioma.h"

namespace {

enum ItemKind { I_APP, I_SEP, I_DOWNLOADS, I_TRASH, I_LAUNCHPAD, I_MINWIN, I_GHOST };
struct Item { ItemKind kind; int app; float cx = 0, size = 0; HWND win = nullptr; bool leaving = false; int ghost = -1; };  // ghost: app saindo (índice em g_ghosts)  // win: janela minimizada (I_MINWIN); leaving: saindo do Dock

float g_tile = 48;          // tamanho do ícone (pt)
// Personalização (Ajustes > Mesa e Dock > Personalizar o Dock, pedido de 20/11/2026, "como o MyDockFinder"). Valor
// negativo = o padrão medido do Mac (as medidas de cada tema logo abaixo continuam valendo).
float g_espaco = 0;          // dock_espaco: espaço a mais entre os ícones (pt)
float g_margemPt = -1;       // dock_margem: distância do Dock até a borda da tela (pt)
float g_raioPct = -1;        // dock_raio: arredondamento do fundo, 0 (reto) a 1 (pílula)
float g_vidroOpac = -1;      // dock_opacidade: tinta do vidro, 0 (só o desfoque) a 1 (sólido)
int g_vidroCor = 0;          // dock_cor: 0 automática, 1 clara, 2 escura, 3 cor de destaque
bool g_mostraSep = true;     // dock_separador: a divisória antes de Downloads e da Lixeira
bool g_mostraLaunchpad = true;  // dock_launchpad: o Launchpad logo depois do Finder
int g_indicador = 0;         // dock_indicador: 0 ponto, 1 traço
bool g_rotulos = true;       // dock_rotulos: o nome do app ao passar o ponteiro
bool g_sepHover, g_resizing;  // separador: arrastar para cima/baixo muda o tamanho (como no Mac)
LONG g_resizeY;
float g_resizeTile;
float g_large = 128;        // tamanho máximo na ampliação
bool g_magnify = false;
bool g_autohide = false;
bool g_smart = false;        // ocultação inteligente: só esconde quando uma janela encosta no Dock
bool g_covered = false;      // alguma janela por cima da área do Dock agora
DWORD g_coverAt = 0;
// Proporções medidas no vídeo do Sequoia (ícone de 80 px): os ícones encostam uns nos outros
// (o espaço é a margem do próprio desenho), o fundo é o ícone + ~9% embaixo (onde fica o ponto),
// ~4% dos lados e fica a ~10% da borda da tela; o separador ocupa meio ícone.
// Medidas tiradas de capturas do Dock do Sequoia, em relação ao desenho visível do ícone (A = 0,8 tile):
// passo 1,345 A; da borda do fundo ao desenho 0,224 A (em cima e dos lados) e 0,35 A embaixo;
// cantos 0,3 A; a divisória soma 0,5 A ao passo; ponto a 0,075 A da borda de baixo.
// Vidro: o fundo desfocado continua aparecendo (~70%), com uma tinta cinza por cima.
const DWORD ACR_DARK = 0x401E1C1C, ACR_LIGHT = 0x40F6F6F6;  // AABBGGRR
// Tahoe (Tema nos Ajustes), medido no Dock do macOS 26 (2x, ícone visível A = 0,8 tile): margem visível de
// (vídeo 1080p do macOS 26, A = 67 px) 0,32 A em cima, 0,31 A embaixo e dos lados, passo 1,35 A,
// canto de 0,5 A; o ponto de app aberto tem 0,07 A e fica com o centro a 0,11 A da borda de baixo
#define TAHOE_DOCK (liquid::On())
// Mac OS X Leopard: a Dock é uma prateleira de vidro em perspectiva encostada no pé da tela; os ícones ficam em pé
// sobre ela, refletidos no tampo, e o app aberto ganha a luz azul na frente da prateleira (desenho em Paint)
#define LEO_DOCK (liquid::Leopard())
#define PAD_TOP (g_tile * (TAHOE_DOCK ? 0.156f : LEO_DOCK ? 0.06f : 0.099f))
#define PAD_BOTTOM (g_tile * (TAHOE_DOCK ? 0.148f : LEO_DOCK ? 0.36f : 0.196f))  // Leopard: a frente ~0,32 ícone abaixo do pé (fotos de referência)
// Tahoe, guia macOS26-Theme-Reference (seção 36, medido com ícone de 64): cada ícone ocupa 68 × 84 (passo
// 1,0625), 8 de margem antes do primeiro, separador de 31 (0,484); a altura reservada dá os 90 medidos
#define PAD_X (g_tile * (TAHOE_DOCK ? 0.125f : LEO_DOCK ? 0.55f : 0.041f))
#define MARGIN (g_margemPt >= 0 ? g_margemPt : g_tile * (LEO_DOCK ? 0.f : 0.10f))  // Leopard: a prateleira encosta no pé da tela
#define SEP_W (g_tile * (TAHOE_DOCK ? 0.484f : LEO_DOCK ? 0.80f : 0.40f))  // Leopard: o vão largo da faixa zebrada (~0,85 ícone na captura)
// Leopard (comparado com as fotos de referência): passo de ~1,17 ícone entre os desenhos
#define SLOT (g_tile * (TAHOE_DOCK ? 1.0625f : LEO_DOCK ? 1.17f : 1.076f) + g_espaco)  // Leopard: passo de ~1,17 ícone (medido nas fotos de referência do usuário)
#define DOCK_RADIUS (g_raioPct >= 0 ? (g_tile + PAD_TOP + PAD_BOTTOM) * 0.5f * g_raioPct : g_tile * (TAHOE_DOCK ? 0.40f : 0.28f))  // Tahoe: 0,5 A (medido); Sequoia: 0,3 A
const float ART_PACK_0 = 1.0f;   // PNGs do pacote: já têm a margem do Mac (desenho = 80%)
const float ART_WIN_0 = 0.80f;   // ícones do Windows (recortados): mesmo corpo visual
// Leopard: o desenho do ícone ocupa o espaço inteiro (no 10.5 os ícones quase se encostam, sem a margem de hoje)
#define ART_PACK (LEO_DOCK ? 1.25f : ART_PACK_0)
#define ART_WIN (LEO_DOCK ? 1.0f : ART_WIN_0)

HWND g_back, g_icons, g_owner;
// Janelas minimizadas (como no Mac: miniatura da janela entre Downloads e a Lixeira, com o ícone do app
// no canto). A miniatura é do DWM (o conteúdo de verdade); fica numa janela própria ENTRE o fundo e a dos
// ícones, porque a miniatura do DWM é sempre desenhada por cima da janela onde está — assim o seletinho do
// app (desenhado na janela dos ícones) fica por cima dela.
HWND g_thumbWnd;
std::vector<HWND> g_minOrder;               // ordem em que foram minimizadas
std::map<HWND, HTHUMBNAIL> g_thumbs;
// o espaço de uma minimizada abre e fecha aos poucos, como no Mac (antes o Dock pulava de tamanho no fim)
const double SLOT_MS = 300;
std::map<HWND, double> g_appear;                         // quando entrou (abrindo o espaço)
std::map<HWND, std::pair<double, int>> g_leave;          // saindo: quando começou e o app dela
float SlotFactor(const Item& it) {
    double now = anim::Now();
    if (it.leaving) { auto f = g_leave.find(it.win); return f == g_leave.end() ? 0.f : 1 - anim::Ease(std::min(1.f, (float)((now - f->second.first) / SLOT_MS))); }
    auto f = g_appear.find(it.win);
    return f == g_appear.end() ? 1.f : anim::Ease(std::min(1.f, (float)((now - f->second) / SLOT_MS)));
}
// Apps entrando e saindo do Dock: o espaço abre/fecha aos poucos e o ícone cresce/encolhe junto (como no
// Mac; antes os vizinhos pulavam de lugar). O que sai fica como "fantasma" até o espaço fechar.
struct Ghost { std::wstring id; bool mac = false, finder = false; double t0 = 0; int pos = 0; };
std::vector<Ghost> g_ghosts;
std::map<std::wstring, double> g_appIn;          // id -> quando entrou
std::vector<std::pair<std::wstring, bool>> g_prevApps;  // ordem anterior (id, ícone do pacote)
bool g_builtOnce;
// "puf" do Mac ao tirar um ícone do Dock: nuvenzinha que estoura e some
struct Poof { float x = 0, y = 0; double t0 = 0; } g_poof;
const double POOF_MS = 420;
std::map<std::wstring, DWORD> g_nudge;          // id -> pulo único (notificação nova)
std::map<std::wstring, int> g_lastBadge;
float AppFactor(const Item& it) {
    double now = anim::Now();
    if (it.kind == I_GHOST) {
        if (it.ghost < 0 || it.ghost >= (int)g_ghosts.size()) return 0.f;
        return 1 - anim::Ease(std::min(1.f, (float)((now - g_ghosts[it.ghost].t0) / SLOT_MS)));
    }
    if (it.kind != I_APP) return 1.f;
    auto f = g_appIn.find(apps::list[it.app].id);
    return f == g_appIn.end() ? 1.f : anim::Ease(std::min(1.f, (float)((now - f->second) / SLOT_MS)));
}
bool SlotSettled(const Item& it) { return !it.leaving && SlotFactor(it) >= 0.999f && !genio::Busy(it.win); }
ui::Surface g_sb;
ui::GpuSurface g_gi;           // ícones: GPU (DirectComposition), quadros no ritmo do monitor
RECT g_win{};                  // janela dos ícones dentro da faixa de baixo da tela (px)
D2D1_RECT_F g_lastBg{};
const UINT WM_DOCKFRAME = WM_APP + 40;
const UINT_PTR TIMER_HIDECHECK = 7101;  // ocultação parada: olha o mouse e as janelas sem redesenhar
std::vector<Item> g_items;
std::map<std::wstring, ID2D1Bitmap*> g_bmps;
ID2D1RenderTarget* g_bmpRt;
IDWriteTextFormat* g_tfLabel;
float g_mouseX = -1;           // pt, na janela dos ícones; -1 = fora
float g_lastMouseX = -1;       // última posição dentro (a ampliação encolhe dali ao sair)
float g_magAmt = 0;            // quanto da ampliação está aplicado (0..1): entra e sai suave, como no Mac
double g_lastFrame;            // ms de anim::Now do último quadro (animações por tempo, não por quadro)
int g_hover = -1, g_press = -1;
bool g_dragging = false, g_dragOut = false;
int g_dragTo = -1;  // durante o arrasto: posição de destino em apps::list (-1: nenhuma)
POINT g_pressPt;
float g_dragX = 0;
bool g_glass = false;         // vidro da Composição (backdrop.cpp) no fundo
bool g_fullscreen = false;    // app em tela cheia na frente: Dock escondido
bool g_hidden = false;         // ocultação automática: escondido agora
float g_reveal = 1;            // 0 = escondido, 1 = à mostra (animação)
D2D1_RECT_F g_bg;              // fundo atual (pt, coordenadas da janela dos ícones)
bool g_trashFull = false;
int g_dropHit = -1;

void LeftClick(int i);
float Sc() { return ui::Scale(g_icons); }
// Posição na tela (Ajustes > Mesa e Dock > Posição na tela: dock_posicao=baixo|esquerda|direita). Todo o layout é feito
// "ao longo do Dock": x corre pelos ícones e y vai de dentro da tela (0) até a borda onde o Dock encosta (WinH). Embaixo
// isso é a própria janela; dos lados, a mesma conta girada (ToWin/ToLocal). Os ícones, os números e as etiquetas são
// desenhados sempre em pé (Up). O Leopard fica sempre embaixo (a prateleira em perspectiva só existe deitada).
int g_pos = 0;               // 0 embaixo, 1 esquerda, 2 direita
int Pos() { return LEO_DOCK ? 0 : g_pos; }
bool Vert() { return Pos() != 0; }
// espaço para ampliar e para a etiqueta (dos lados a etiqueta fica ao lado do ícone: precisa de mais largura)
float WinH() { return g_large + PAD_TOP + PAD_BOTTOM + MARGIN + (Vert() ? 280 : 52); }

// O monitor do Dock (Ajustes > Mesa e Dock > Monitor do Dock: dock_monitor = nome do monitor, ex. \\.\DISPLAY2;
// vazio = o principal). Antes era o monitor onde a janela estava, e com dois monitores o Dock passava de um
// para o outro. Guardado por 2 s: monitor ligado/desligado entra sozinho.
HMONITOR DockMon() {
    static std::wstring lastKey;
    static HMONITOR cached;
    static DWORD at;
    std::wstring want = shell::Setting(L"dock_monitor", L"");
    DWORD now = GetTickCount();
    if (cached && want == lastKey && now - at < 2000) return cached;
    lastKey = want; at = now;
    struct Find { const std::wstring* name; HMONITOR found; } f{ &want, nullptr };
    if (!want.empty())
        EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hm, HDC, LPRECT, LPARAM p) -> BOOL {
            auto* f = (Find*)p;
            MONITORINFOEXW mi{}; mi.cbSize = sizeof(mi);
            if (GetMonitorInfoW(hm, &mi) && !_wcsicmp(mi.szDevice, f->name->c_str())) { f->found = hm; return FALSE; }
            return TRUE;
        }, (LPARAM)&f);
    HMONITOR hm = f.found ? f.found : MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY);
    if (hm != cached && cached) Log(L"Dock: monitor %ls", want.empty() || !f.found ? L"principal" : want.c_str());
    cached = hm;
    return hm;
}

RECT Monitor() {
    MONITORINFO mi{ sizeof(mi) };
    GetMonitorInfoW(DockMon(), &mi);
    return mi.rcMonitor;
}

// A janela dos ícones na tela (px): embaixo, a faixa de baixo do monitor; dos lados, a coluna da borda, da barra de
// menus para baixo (como no Mac, o Dock lateral fica centralizado no espaço abaixo da barra).
RECT Strip() {
    MONITORINFO mi{ sizeof(mi) };
    HMONITOR hm = DockMon();
    GetMonitorInfoW(hm, &mi);
    RECT m = mi.rcMonitor;
    int H = (int)std::lround(WinH() * Sc());
    int top = m.top + ((mi.dwFlags & MONITORINFOF_PRIMARY) ? menubar::Height() : menubar::HeightOn(hm));
    if (Pos() == 1) return { m.left, top, m.left + H, m.bottom };
    if (Pos() == 2) return { m.right - H, top, m.right, m.bottom };
    return { m.left, m.bottom - H, m.right, m.bottom };
}
// comprimento disponível ao longo do Dock (pt)
float AxisLen() { RECT s = Strip(); return (Vert() ? s.bottom - s.top : s.right - s.left) / Sc(); }
// do sistema do Dock (x ao longo, y até a borda) para a janela dos ícones (pt) e de volta
D2D1_POINT_2F ToWin(float x, float y) {
    if (Pos() == 1) return { WinH() - y, x };  // esquerda: a borda (y = WinH) fica em X = 0
    if (Pos() == 2) return { y, x };           // direita: a borda fica em X = WinH
    return { x, y };
}
D2D1_POINT_2F ToLocal(float X, float Y) {
    if (Pos() == 1) return { Y, WinH() - X };
    if (Pos() == 2) return { Y, X };
    return { X, Y };
}
D2D1_MATRIX_3X2_F DockMatrix() {  // a mesma conta de ToWin, para o Direct2D
    if (Pos() == 1) return D2D1::Matrix3x2F(0, 1, -1, 0, WinH(), 0);
    if (Pos() == 2) return D2D1::Matrix3x2F(0, 1, 1, 0, 0, 0);
    return D2D1::Matrix3x2F::Identity();
}
D2D1_RECT_F RectToWin(D2D1_RECT_F r) {
    D2D1_POINT_2F a = ToWin(r.left, r.top), b = ToWin(r.right, r.bottom);
    return { std::min(a.x, b.x), std::min(a.y, b.y), std::max(a.x, b.x), std::max(a.y, b.y) };
}
// um quadrado do sistema do Dock em pé na janela: o mesmo tamanho, centrado no mesmo ponto (ícones não giram)
D2D1_RECT_F Up(D2D1_RECT_F r) {
    D2D1_POINT_2F c = ToWin((r.left + r.right) / 2, (r.top + r.bottom) / 2);
    float w = (r.right - r.left) / 2, h = (r.bottom - r.top) / 2;
    return { c.x - w, c.y - h, c.x + w, c.y + h };
}
POINT ToScreen(float x, float y) {
    RECT s = Strip(); float sc = Sc();
    D2D1_POINT_2F p = ToWin(x, y);
    return { s.left + (LONG)std::lround(p.x * sc), s.top + (LONG)std::lround(p.y * sc) };
}
RECT ToScreenRect(D2D1_RECT_F local) {
    RECT s = Strip(); float sc = Sc();
    D2D1_RECT_F r = RectToWin(local);
    return { s.left + (LONG)std::lround(r.left * sc), s.top + (LONG)std::lround(r.top * sc), s.left + (LONG)std::lround(r.right * sc), s.top + (LONG)std::lround(r.bottom * sc) };
}
D2D1_POINT_2F ScreenToLocal(POINT p) {
    RECT s = Strip(); float sc = Sc();
    return ToLocal((p.x - s.left) / sc, (p.y - s.top) / sc);
}
// coordenada de tela que cresce para dentro da tela, perpendicular ao Dock (arrastar o separador muda o tamanho)
LONG Inward(POINT c) { return Pos() == 1 ? c.x : Pos() == 2 ? -c.x : -c.y; }

// Muitos apps abertos: o Dock encolhe para caber na tela (como no Mac), em vez de passar da borda (e entrar
// no outro monitor). g_tileWant é o tamanho escolhido; g_tile, o usado agora.
float g_tileWant = 48;
void FitTile() {
    g_tile = g_tileWant;
    float sc = ui::Scale(g_icons);
    if (sc <= 0) sc = 1;
    float W = AxisLen() - 16;  // um respiro de cada lado
    float units = 0;                          // largura em múltiplos de g_tile
    for (auto& it : g_items) units += it.kind == I_SEP ? SEP_W / g_tile : SLOT / g_tile;
    units += 2 * PAD_X / g_tile;
    if (units > 0 && g_tile * units > W) g_tile = std::max(16.f, W / units);
}

void BuildItems() {
    g_items.clear();
    bool sep = false;
    for (int i = 0; i < (int)apps::list.size(); i++) {
        auto& a = apps::list[i];
        // sem divisória entre fixos e abertos: uma só, antes de Downloads e Lixeira
        g_items.push_back({ I_APP, i });
        if (a.finder && !LEO_DOCK && g_mostraLaunchpad) g_items.push_back({ I_LAUNCHPAD, -1 });  // como no Mac: Launchpad logo depois do Finder
    }
    {   // quem entrou e quem saiu desde a última vez
        double now0 = anim::Now();
        std::vector<std::pair<std::wstring, bool>> cur;
        for (auto& a : apps::list) cur.push_back({ a.id, a.mac });
        if (g_builtOnce && !g_dragging) {
            for (auto& [id, mac] : cur) {
                bool was = false; for (auto& [pid, pm] : g_prevApps) if (pid == id) was = true;
                if (!was) g_appIn[id] = now0;
            }
            for (int k = 0; k < (int)g_prevApps.size(); k++) {
                bool still = false; for (auto& [id, mac] : cur) if (id == g_prevApps[k].first) still = true;
                bool ghosted = false; for (auto& gh : g_ghosts) if (gh.id == g_prevApps[k].first) ghosted = true;
                if (!still && !ghosted) { Ghost gh; gh.id = g_prevApps[k].first; gh.mac = g_prevApps[k].second; gh.t0 = now0; gh.pos = k; g_ghosts.push_back(gh); }
            }
        }
        g_builtOnce = true;
        g_prevApps = cur;
        for (auto it = g_appIn.begin(); it != g_appIn.end();) it = now0 - it->second > SLOT_MS + 30 ? g_appIn.erase(it) : std::next(it);
        g_ghosts.erase(std::remove_if(g_ghosts.begin(), g_ghosts.end(), [&](const Ghost& gh) { return now0 - gh.t0 > SLOT_MS + 30; }), g_ghosts.end());
        // fantasmas no lugar onde estavam (entre os apps)
        for (int gi = 0; gi < (int)g_ghosts.size(); gi++) {
            int seen = 0; size_t at = g_items.size();
            for (size_t k = 0; k < g_items.size(); k++) {
                if (g_items[k].kind != I_APP) continue;
                if (seen >= g_ghosts[gi].pos) { at = k; break; }
                seen++;
            }
            Item gitem{ I_GHOST, -1 }; gitem.ghost = gi;
            g_items.insert(g_items.begin() + at, gitem);
        }
    }
    if (g_mostraSep || LEO_DOCK) g_items.push_back({ I_SEP, -1 });
    // Leopard: a pasta Aplicativos (que abre o Launchpad) fica à direita, depois da divisória, junto de Downloads e da
    // Lixeira, como as Pilhas do 10.5
    if (LEO_DOCK) g_items.push_back({ I_LAUNCHPAD, -1 });
    // Ajustes > Mesa e Dock > "Mostrar a pasta Downloads no Dock" (dock_downloads=0 tira)
    if (shell::Setting(L"dock_downloads", L"1") != L"0") g_items.push_back({ I_DOWNLOADS, -1 });
    // minimizadas: as que continuam minimizadas ficam na ordem; as novas entram no fim
    std::vector<std::pair<HWND, int>> mins;
    for (int i = 0; i < (int)apps::list.size(); i++)
        for (auto& w : apps::list[i].wins)
            if (w.minimized && !revelar::Owns(w.hwnd) && genio::Phase(w.hwnd) != 2 && !apps::IsHidden(w.hwnd)) mins.push_back({ w.hwnd, i });  // minimizando: o espaço já abre
    double now = anim::Now();
    for (int i = 0; i < (int)apps::list.size(); i++)  // restaurando pelo Dock: o espaço fecha aos poucos
        for (auto& w : apps::list[i].wins)
            if (genio::Phase(w.hwnd) == 2 && !g_leave.count(w.hwnd)) g_leave[w.hwnd] = { now, i };
    for (auto it = g_leave.begin(); it != g_leave.end();) it = now - it->second.first > SLOT_MS + 30 ? g_leave.erase(it) : std::next(it);
    for (auto& [mh, ai] : mins) if (!g_appear.count(mh)) g_appear[mh] = now;
    for (auto it = g_appear.begin(); it != g_appear.end();) {
        bool still = false; for (auto& [mh, ai] : mins) if (mh == it->first) still = true;
        it = still ? std::next(it) : g_appear.erase(it);
    }
    auto inMins = [&](HWND h) { for (auto& [mh, ai] : mins) if (mh == h) return ai; return -1; };
    std::vector<HWND> keep;
    for (HWND h : g_minOrder) if (inMins(h) >= 0 || g_leave.count(h)) keep.push_back(h);
    for (auto& [mh, ai] : mins) if (std::find(keep.begin(), keep.end(), mh) == keep.end()) keep.push_back(mh);
    g_minOrder = keep;
    for (HWND h : g_minOrder) {
        int ai = inMins(h);
        Item it{ I_MINWIN, ai >= 0 ? ai : g_leave[h].second }; it.win = h; it.leaving = ai < 0;
        if (it.app >= 0 && it.app < (int)apps::list.size()) g_items.push_back(it);
    }
    for (auto it = g_thumbs.begin(); it != g_thumbs.end();) {  // miniaturas de janelas que voltaram
        if (std::find(g_minOrder.begin(), g_minOrder.end(), it->first) == g_minOrder.end()) { DwmUnregisterThumbnail(it->second); it = g_thumbs.erase(it); }
        else ++it;
    }
    g_items.push_back({ I_TRASH, -1 });
    SHQUERYRBINFO q{ sizeof(q) };
    g_trashFull = SUCCEEDED(SHQueryRecycleBinW(nullptr, &q)) && q.i64NumItems > 0;
    FitTile();
}

// Posições e tamanhos (com ampliação), centralizado na tela.
void Layout() {
    // the layout arithmetic is shared with Linux (src/core/dock_layout.cpp); here only the Dock's state goes in and out
    std::vector<dock_layout::Item> items(g_items.size());
    for (size_t i = 0; i < g_items.size(); i++) {
        auto& it = g_items[i]; auto& o = items[i];
        o.separator = it.kind == I_SEP;
        o.app = it.kind == I_APP;
        o.movablePinned = o.app && apps::list[it.app].pinned && !apps::list[it.app].finder;
        o.appIndex = it.app;  // for the other kinds too (the old code read .app from any neighbour)
        o.factor = it.kind == I_MINWIN ? SlotFactor(it) : AppFactor(it);
        o.cx = it.cx;
    }
    dock_layout::Params prm;
    prm.axis = AxisLen();  // along the Dock (on the sides, the height below the menu bar)
    prm.icon = g_tile; prm.magnified = g_large; prm.step = SLOT; prm.margin = PAD_X; prm.separatorWidth = SEP_W;
    prm.magnify = g_magnify; prm.tahoe = TAHOE_DOCK;
    prm.mouse = g_mouseX; prm.lastMouse = g_lastMouseX; prm.magnification = g_magAmt;
    prm.dragging = g_dragging; prm.outsideDock = g_dragOut;
    prm.dragged = g_dragging && g_press >= 0 && g_press < (int)g_items.size() && g_items[g_press].kind == I_APP && apps::list[g_items[g_press].app].pinned ? g_press : -1;
    dock_layout::Result res = dock_layout::Compute(items, prm);
    for (size_t i = 0; i < g_items.size(); i++) { g_items[i].cx = items[i].cx; g_items[i].size = items[i].size; }
    g_dragTo = res.dropTarget;
    float W = prm.axis, total = res.total;
    float H = WinH();
    float hide = (1 - anim::EaseInOut(g_reveal)) * (g_tile + PAD_TOP + PAD_BOTTOM + MARGIN + 2);  // ocultar/mostrar com a curva do Mac
    float bottom = H - MARGIN + hide;
    g_bg = D2D1::RectF((W - total) / 2, bottom - (g_tile + PAD_TOP + PAD_BOTTOM), (W + total) / 2, bottom);
}

// Ícone do pacote do Sequoia por nome (Downloads, Lixeira), em cache por render target.
ID2D1Bitmap* PackBitmap(ID2D1RenderTarget* rt, const std::wstring& name) {
    if (g_bmpRt != rt) { for (auto& [k, b] : g_bmps) if (b) b->Release(); g_bmps.clear(); g_bmpRt = rt; }
    // Tahoe: estilo dos ícones na chave — só para apps (o Launchpad/Apps); a pasta Downloads e a Lixeira ficam como são
    // (como no macOS 26, o estilo Transparente/Tingido não muda as pastas nem a Lixeira do Dock)
    bool keep = name.rfind(L"folder", 0) == 0 || name.rfind(L"trash", 0) == 0;
    // ...mas no Transparente escuro a Lixeira e a pasta perdem a cor (print do Dock real do macOS 26, 16/11/2026:
    // os papéis da Lixeira ficam cinza): só tons de cinza, sem virar peça de vidro
    bool gray = keep && ui::IconStyle() == L"transparente" && ui::Dark();
    std::wstring st = keep ? L"padrao" : ui::IconStyle(), key = L"pack:" + (keep ? st + (gray ? L"/cinza" : L"") : ui::IconStyleKey()) + L":" + name;
    auto it = g_bmps.find(key);
    if (it != g_bmps.end()) return it->second;
    UINT w = 0, h = 0; std::vector<BYTE> px;
    std::wstring path = ui::PackIcon(name);
    bool ok = !path.empty() && ui::LoadPng(path, w, h, px);
    if (ok) ui::StyleIcon(w, h, px, st);
    if (ok && gray)
        for (size_t i = 0; i + 3 < px.size(); i += 4) {  // pré-multiplicado: a média ponderada vale direto
            BYTE l = (BYTE)std::lround(std::min(255.0, (0.2126 * px[i + 2] + 0.7152 * px[i + 1] + 0.0722 * px[i]) * 0.92));
            px[i] = px[i + 1] = px[i + 2] = std::min(l, px[i + 3]);
        }
    ID2D1Bitmap* b = ok ? ui::Bitmap(rt, w, h, px) : nullptr;
    g_bmps[key] = b;
    return b;
}

// ---- Downloads "como Pilha" (o padrão do Mac): no Dock, os arquivos mais novos empilhados em vez da pasta.
// As imagens (miniatura ou ícone do arquivo) são lidas numa thread e chegam por WM_DOCKSTACK.
const UINT WM_DOCKSTACK = WM_APP + 43;
struct StackImg { std::wstring path; UINT w = 0, h = 0; std::vector<BYTE> px; ID2D1Bitmap* bmp = nullptr; };
std::vector<StackImg> g_stack, g_stackNew;
std::vector<std::wstring> g_stackPaths;
std::mutex g_stackMx;
bool g_stackReady, g_stackBusy;
DWORD g_stackAt;
ID2D1RenderTarget* g_stackRt;

bool StackMode() { return shell::Setting(L"pilha_exibir", L"pilha") == L"pilha"; }

void RefreshStack(int px) {
    DWORD now = GetTickCount();
    if (g_stackBusy || (g_stackAt && now - g_stackAt < 4000)) return;
    g_stackAt = now ? now : 1;
    std::vector<std::wstring> paths = pilhas::Recentes(3);
    if (paths == g_stackPaths) return;
    g_stackPaths = paths;
    g_stackBusy = true;
    HWND w = g_icons;
    std::thread([paths, px, w] {
        HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        std::vector<StackImg> v;
        for (auto& p : paths) {
            StackImg s; s.path = p;
            if (ui::FileImage(p, px, s.w, s.h, s.px)) v.push_back(std::move(s));
        }
        if (SUCCEEDED(co)) CoUninitialize();
        { std::lock_guard<std::mutex> l(g_stackMx); g_stackNew = std::move(v); g_stackReady = true; }
        g_stackBusy = false;
        if (w) PostMessageW(w, WM_DOCKSTACK, 0, 0);
    }).detach();
}

// desenha a pilha no lugar do ícone (de trás para a frente: o mais novo por cima, um pouco girado cada um).
// false = nada para desenhar (pasta vazia ou ainda carregando): fica a pasta
bool DrawStack(ID2D1DeviceContext* rt, D2D1_RECT_F ir) {
    static std::wstring stackStyle;  // Tahoe: os arquivos da pilha também seguem o estilo dos ícones (Transparente/Tingido = vidro)
    std::wstring st = ui::IconStyle();
    if (g_stackRt != rt || st != stackStyle) { for (auto& s : g_stack) if (s.bmp) { s.bmp->Release(); s.bmp = nullptr; } g_stackRt = rt; stackStyle = st; }
    {
        std::lock_guard<std::mutex> l(g_stackMx);
        if (g_stackReady) {
            for (auto& s : g_stack) if (s.bmp) s.bmp->Release();
            g_stack = std::move(g_stackNew); g_stackNew.clear(); g_stackReady = false;
        }
    }
    if (g_stack.empty()) return false;
    float cx = (ir.left + ir.right) / 2, cy = (ir.top + ir.bottom) / 2, side = (ir.right - ir.left) * 0.86f;
    static const float rot[3] = { 0.f, -7.f, 6.f };  // o de cima reto, os de baixo tortos (como os papéis do Mac)
    D2D1_MATRIX_3X2_F old; rt->GetTransform(&old);
    for (int i = (int)g_stack.size() - 1; i >= 0; i--) {
        auto& s = g_stack[i];
        if (!s.bmp && !s.px.empty()) {  // os pixels originais ficam (o estilo pode mudar e o desenho é refeito)
            s.bmp = ui::Bitmap(rt, s.w, s.h, s.px);  // os arquivos da pilha não seguem o estilo (são da pasta Downloads)
        }
        if (!s.bmp) continue;
        D2D1_SIZE_F sz = s.bmp->GetSize();
        float k = std::min(side / sz.width, side / sz.height), w = sz.width * k, h = sz.height * k;
        float dy = i * side * 0.04f;
        rt->SetTransform(D2D1::Matrix3x2F::Rotation(rot[i % 3], { cx, cy }) * old);
        rt->DrawBitmap(s.bmp, D2D1::RectF(cx - w / 2, cy - h / 2 - dy, cx + w / 2, cy + h / 2 - dy), 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    }
    rt->SetTransform(old);
    return true;
}

// O ícone já é um quadrado arredondado cheio (formato dos ícones do Mac)? Olha a caixa do desenho: quase quadrada e
// opaca no meio de cada lado e no centro. Logotipos soltos (onda, círculo, letra) não passam.
bool IsTileShaped(UINT w, UINT h, const std::vector<BYTE>& px) {
    if (!w || !h || px.size() < (size_t)w * h * 4) return true;
    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (UINT y = 0; y < h; y++) for (UINT x = 0; x < w; x++) if (px[((size_t)y * w + x) * 4 + 3] > 40) { x0 = std::min<int>(x0, x); x1 = std::max<int>(x1, x); y0 = std::min<int>(y0, y); y1 = std::max<int>(y1, y); }
    if (x1 < 0) return true;
    float bw = float(x1 - x0 + 1), bh = float(y1 - y0 + 1);
    if (bw / bh < 0.9f || bw / bh > 1.1f) return false;
    auto a = [&](float fx, float fy) { int x = x0 + (int)(fx * (bw - 1)), y = y0 + (int)(fy * (bh - 1)); return px[((size_t)y * w + x) * 4 + 3]; };
    const float p[5][2] = { { 0.5f, 0.06f }, { 0.06f, 0.5f }, { 0.94f, 0.5f }, { 0.5f, 0.94f }, { 0.5f, 0.5f } };
    for (auto& q : p) if (a(q[0], q[1]) < 200) return false;
    return true;
}

// Tahoe: ícone fora do formato vai para dentro de um quadrado arredondado cinza (como o macOS 26 faz com os apps
// que não foram atualizados). O desenho fica a 70% no meio. Pixels BGRA pré-multiplicados.
std::vector<BYTE> Jail(UINT w, UINT h, const std::vector<BYTE>& src, bool dark) {
    std::vector<BYTE> out((size_t)w * h * 4, 0);
    float W = (float)w, Hh = (float)h, r = W * 0.225f;
    for (UINT y = 0; y < h; y++) for (UINT x = 0; x < w; x++) {
        // cobertura do quadrado arredondado (borda suavizada por distância)
        float px = x + 0.5f, py = y + 0.5f;
        float qx = std::max(std::fabs(px - W / 2) - (W / 2 - r), 0.f), qy = std::max(std::fabs(py - Hh / 2) - (Hh / 2 - r), 0.f);
        float d = std::sqrt(qx * qx + qy * qy) - r;
        float cov = std::clamp(0.5f - d, 0.f, 1.f);
        if (cov <= 0) continue;
        float t = py / Hh;  // degradê de cima para baixo
        float c = dark ? (0.30f - 0.08f * t) : (0.96f - 0.10f * t);
        BYTE v = (BYTE)std::lround(c * 255 * cov), al = (BYTE)std::lround(255 * cov);
        BYTE* o = &out[((size_t)y * w + x) * 4];
        o[0] = o[1] = o[2] = v; o[3] = al;
    }
    float s = 0.70f, ox = W * (1 - s) / 2, oy = Hh * (1 - s) / 2;
    for (UINT y = 0; y < h; y++) for (UINT x = 0; x < w; x++) {
        // amostra bilinear do ícone reduzido
        float sx = (x + 0.5f - ox) / s - 0.5f, sy = (y + 0.5f - oy) / s - 0.5f;
        if (sx < -1 || sy < -1 || sx > W || sy > Hh) continue;
        int ix = (int)std::floor(sx), iy = (int)std::floor(sy);
        float fx = sx - ix, fy = sy - iy, acc[4] = {};
        for (int k = 0; k < 4; k++) {
            int xx = ix + (k & 1), yy = iy + (k >> 1);
            if (xx < 0 || yy < 0 || xx >= (int)w || yy >= (int)h) continue;
            float wgt = ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy);
            const BYTE* p = &src[((size_t)yy * w + xx) * 4];
            for (int c = 0; c < 4; c++) acc[c] += p[c] * wgt;
        }
        float sa = acc[3] / 255.f;
        if (sa <= 0) continue;
        BYTE* o = &out[((size_t)y * w + x) * 4];
        for (int c = 0; c < 4; c++) o[c] = (BYTE)std::clamp(std::lround(acc[c] + o[c] * (1 - sa)), 0L, 255L);
    }
    return out;
}

// Ícone próprio do usuário (Ajustes > Personalizar o Dock > Ícones próprios): um PNG em %APPDATA%\Shell\icones-dock com o
// nome do .exe (chrome.png, discord.png) ou o nome do app como aparece no Dock (Google Chrome.png). Vale como os do
// pacote do Mac (o desenho já tem a margem). Vazio = sem ícone próprio.
std::wstring CustomIconPath(const App& a) {
    static std::wstring base;
    if (base.empty()) {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) { base = std::wstring(p) + L"\\Shell\\icones-dock\\"; CoTaskMemFree(p); }
        else base = L"?";
    }
    std::vector<std::wstring> names;
    std::wstring exe = a.exe.empty() ? a.id : a.exe;
    size_t sl = exe.find_last_of(L"\\/");
    if (sl != std::wstring::npos) exe = exe.substr(sl + 1);
    if (exe.size() > 4 && !_wcsicmp(exe.c_str() + exe.size() - 4, L".exe")) exe.resize(exe.size() - 4);
    if (!exe.empty() && exe.find(L':') == std::wstring::npos) names.push_back(exe);
    if (!a.name.empty()) names.push_back(a.name);
    for (auto& n : names) {
        std::wstring f = base + n + L".png";
        if (GetFileAttributesW(f.c_str()) != INVALID_FILE_ATTRIBUTES) return f;
    }
    return L"";
}
std::map<std::wstring, bool> g_customIcon;  // app -> tem ícone próprio (desenhado como os do pacote)

ID2D1Bitmap* IconBitmap(ID2D1RenderTarget* rt, App& a) {
    if (g_bmpRt != rt) { for (auto& [k, b] : g_bmps) if (b) b->Release(); g_bmps.clear(); g_bmpRt = rt; }
    {
        auto ck = g_bmps.find(L"proprio:" + a.id);
        if (ck != g_bmps.end()) return ck->second;
        auto known = g_customIcon.find(a.id);
        if (known == g_customIcon.end() || known->second) {
            std::wstring f = CustomIconPath(a);
            UINT w = 0, h = 0; std::vector<BYTE> px;
            bool ok = !f.empty() && ui::LoadPng(f, w, h, px);
            g_customIcon[a.id] = ok;
            if (ok) {
                std::wstring st = ui::IconStyle();
                if (st != L"padrao") ui::StyleIcon(w, h, px, st);
                ID2D1Bitmap* b = ui::Bitmap(rt, w, h, px);
                g_bmps[L"proprio:" + a.id] = b;
                if (b) return b;
            }
        }
    }
    bool jail = TAHOE_DOCK && !a.mac && !a.finder && shell::Setting(L"dock_icones_cinza", L"1") == L"1";
    std::wstring st = ui::IconStyle(), key = ui::IconStyleKey() + (jail ? L"|j:" : L":") + a.id;  // Tahoe: estilo dos ícones (Escuro, Transparente, Tingido)
    auto it = g_bmps.find(key);
    if (it != g_bmps.end()) return it->second;
    apps::EnsureIcon(a, (int)std::lround(std::min(256.f, g_large * Sc())));
    ID2D1Bitmap* b = nullptr;
    std::vector<BYTE> base = a.icon;
    if (jail && !IsTileShaped(a.iw, a.ih, base)) base = Jail(a.iw, a.ih, base, st == L"escuro");
    if (st == L"padrao") b = ui::Bitmap(rt, a.iw, a.ih, base);
    else { ui::StyleIcon(a.iw, a.ih, base, st); b = ui::Bitmap(rt, a.iw, a.ih, base); }
    g_bmps[key] = b;
    return b;
}

// pulo do Mac: cada salto é uma parábola (sobe freando e desce acelerando, como a gravidade)
float BounceOffset(const App& a) {
    double now = anim::Tick();
    auto hop = [](double ms, float period) { return curves::DockHop(ms, period); };  // shared with Linux (src/core/curves)
    if (a.launchedAt && now - a.launchedAt < 8000 && shell::Setting(L"animar_abertura", L"1") == L"1")
        return -hop(now - a.launchedAt, 620) * g_tile * 0.5f;
    if (a.attentionAt && now - a.attentionAt < 1860)  // pedindo atenção: três pulos mais altos
        return -hop(now - a.attentionAt, 620) * g_tile * 0.65f;
    auto n = g_nudge.find(a.id);  // notificação nova: um pulo só
    if (n != g_nudge.end() && now - n->second < 620) return -hop(now - n->second, 620) * g_tile * 0.45f;
    return 0;
}

// Ocultação inteligente: alguma janela de app (não minimizada, nesta mesa) cobre a área do Dock à mostra?
// Como a "ocultação inteligente" dos docks do Windows: janela maximizada ou arrastada até embaixo esconde o Dock.
bool WindowOverDock() {
    RECT dk = ToScreenRect({ g_bg.left, WinH() - (g_tile + PAD_TOP + PAD_BOTTOM + MARGIN), g_bg.right, WinH() });
    if (dk.right <= dk.left) return false;
    struct C { RECT dk; bool hit; } c{ dk, false };
    EnumWindows([](HWND h, LPARAM p) -> BOOL {
        auto* c = (C*)p;
        if (!IsWindowVisible(h) || IsIconic(h) || apps::IsShellWindow(h) || shell::Nossa(h)) return TRUE;
        LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
        bool app = apps::IsAppWindow(h);
        // diálogo de um app (janela com dona) também conta; menus, dicas e camadas transparentes não
        if (!app && (!GetWindow(h, GW_OWNER) || (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT)))) return TRUE;
        if (!app) { DWORD cl = 0; DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cl, sizeof(cl)); if (cl) return TRUE; }
        RECT r; if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) GetWindowRect(h, &r);
        RECT x; if (IntersectRect(&x, &r, &c->dk)) { c->hit = true; return FALSE; }
        return TRUE;
    }, (LPARAM)&c);
    return c.hit;
}

// Esconder agora? Sempre (ocultação comum) ou só com janela por cima (inteligente); o mouse na borda de baixo mostra.
bool WantHidden() {
    POINT c; GetCursorPos(&c);
    RECT mon = Monitor();
    float sc = Sc();
    // distância do ponteiro até a borda do Dock (embaixo, a de baixo; dos lados, a esquerda ou a direita)
    bool along = Vert() ? c.y >= mon.top && c.y < mon.bottom : c.x >= mon.left && c.x < mon.right;
    LONG dist = Pos() == 1 ? c.x - mon.left : Pos() == 2 ? mon.right - 1 - c.x : mon.bottom - 1 - c.y;
    bool nearEdge = along && dist >= 0 && (dist <= 1 || (g_reveal > 0.5f && dist < (LONG)((g_bg.bottom - g_bg.top + 30) * sc)));
    if (g_smart && GetTickCount() - g_coverAt >= 100) { g_coverAt = GetTickCount(); g_covered = WindowOverDock(); }
    return (!g_smart || g_covered) && !nearEdge && g_press < 0;
}

bool Animating() {
    for (auto& a : apps::list) if ((a.launchedAt && GetTickCount() - a.launchedAt < 8000) || (a.attentionAt && GetTickCount() - a.attentionAt < 1800)) return true;
    for (auto& [id, t] : g_nudge) if (GetTickCount() - t < 640) return true;
    { double nw = anim::Now(); for (auto& gh : g_ghosts) if (nw - gh.t0 < SLOT_MS + 20) return true; for (auto& [id, t] : g_appIn) if (nw - t < SLOT_MS + 20) return true; }
    if ((g_poof.t0 > 0 && anim::Now() - g_poof.t0 < POOF_MS)) return true;
    if (g_magnify && ((g_mouseX >= 0 && g_magAmt < 0.999f) || (g_mouseX < 0 && g_magAmt > 0.001f))) return true;  // ampliação entrando/saindo
    for (auto& it : g_items) if (it.kind == I_MINWIN && (it.leaving || SlotFactor(it) < 0.999f || genio::Busy(it.win))) return true;
    return g_dragging || (g_autohide && ((g_hidden && g_reveal > 0) || (!g_hidden && g_reveal < 1)));
}

std::wstring Label(const Item& it) {
    if (it.kind == I_GHOST) return L"";
    if (it.kind == I_APP) return apps::list[it.app].name;
    if (it.kind == I_MINWIN) { wchar_t t[200] = {}; GetWindowTextW(it.win, t, 200); return t[0] ? t : apps::list[it.app].name; }
    if (it.kind == I_DOWNLOADS) return L"Downloads";
    if (it.kind == I_LAUNCHPAD) return LEO_DOCK ? Tr(L"Aplicativos") : L"Launchpad";  // Leopard: a pasta Aplicativos
    if (it.kind == I_TRASH) return Tr(L"Lixeira");
    return L"";
}


void PaintBack() {
    float sc = Sc();
    RECT bgs = ToScreenRect(g_bg);  // dos lados o fundo fica em pé (mais alto que largo)
    int x = bgs.left, y = bgs.top, w = bgs.right - bgs.left, h = bgs.bottom - bgs.top;
    if (g_reveal <= 0.01f || LEO_DOCK) { ShowWindow(g_back, SW_HIDE); g_lastBg = {}; return; }  // Leopard: sem vidro atrás (a prateleira é desenhada com os ícones)
    if (!memcmp(&g_bg, &g_lastBg, sizeof(g_bg)) && IsWindowVisible(g_back)) return;  // nada mudou
    g_lastBg = g_bg;
    float rad = DOCK_RADIUS;
    bool dark = ui::Dark();
    if (g_glass) {  // vidro da Composição: recorte com a mesma curva do Dock
        SetWindowPos(g_back, g_icons, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        D2D1_COLOR_F tint = dark ? D2D1::ColorF(0.13f, 0.14f, 0.15f, 0.50f) : D2D1::ColorF(0.98f, 0.98f, 0.99f, 0.38f);
        // Tahoe: cor do vidro medida na tela (guia macOS26-Theme-Reference, seções 52 e 60): saída ≈ 0,61 × fundo + 0,11
        // no escuro (39 % de um cinza 0,28 por cima do fundo desfocado); no claro a média sobe ~0,11 (cinza 0,56)
        if (TAHOE_DOCK) tint = dark ? D2D1::ColorF(0.28f, 0.28f, 0.29f, 0.39f) : D2D1::ColorF(0.56f, 0.56f, 0.57f, 0.39f);
        // (16-17/11/2026: a tinta preta do Dock foi desfeita a pedido; o filtro do Transparente é só nos ícones)
        // Personalizar o Dock: cor (clara, escura ou a de destaque) e quanto a tinta cobre o desfoque
        if (g_vidroCor == 1) tint = D2D1::ColorF(0.97f, 0.97f, 0.98f, tint.a);
        else if (g_vidroCor == 2) tint = D2D1::ColorF(0.10f, 0.10f, 0.11f, tint.a);
        else if (g_vidroCor == 3) { D2D1_COLOR_F ac = ui::Accent(); tint = D2D1::ColorF(ac.r, ac.g, ac.b, tint.a); }
        if (g_vidroOpac >= 0) tint.a = g_vidroOpac;
        backdrop::Set(g_back, (float)w, (float)h, rad * sc, tint);
        return;
    }
    // Sem a Composição: acrílico com o canto de 8 do DWM, encolhido para caber dentro da curva.
    int in = rad > 8 ? (int)std::ceil((rad - 8) * sc * 0.293f) + 1 : 0;
    x += in; y += in; w -= 2 * in; h -= 2 * in;
    SetWindowPos(g_back, g_icons, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    auto* rt = ui::Begin(g_sb, w, h, sc);
    if (!rt) return;
    ID2D1SolidColorBrush* br = nullptr;
    D2D1_COLOR_F fill = dark ? D2D1::ColorF(0.11f, 0.11f, 0.12f, 0.30f) : D2D1::ColorF(1, 1, 1, 0.20f);
    if (g_vidroCor == 1) fill = D2D1::ColorF(1, 1, 1, fill.a);
    else if (g_vidroCor == 2) fill = D2D1::ColorF(0.10f, 0.10f, 0.11f, fill.a);
    else if (g_vidroCor == 3) { D2D1_COLOR_F ac = ui::Accent(); fill = D2D1::ColorF(ac.r, ac.g, ac.b, fill.a); }
    if (g_vidroOpac >= 0) fill.a = g_vidroOpac;
    rt->CreateSolidColorBrush(fill, &br);
    rt->FillRectangle(D2D1::RectF(0, 0, w / sc, h / sc), br);
    br->Release();
    ui::End(g_sb, false);
}

// onde fica a miniatura de uma janela minimizada (pt, na janela dos ícones, já em pé): cabe num
// quadrado de 92% do ícone, com a proporção da janela, assentada no pé dos ícones
D2D1_RECT_F MinThumbRect(const Item& it, float baseline) {
    float box = it.size * 0.92f;
    float ww = 16, wh = 10;
    auto f = g_thumbs.find(it.win);
    SIZE sz{};
    if (f != g_thumbs.end() && SUCCEEDED(DwmQueryThumbnailSourceSize(f->second, &sz)) && sz.cx > 0 && sz.cy > 0) { ww = (float)sz.cx; wh = (float)sz.cy; }
    else { RECT r{}; WINDOWPLACEMENT wp{ sizeof(wp) }; if (GetWindowPlacement(it.win, &wp)) r = wp.rcNormalPosition; if (r.right > r.left) { ww = (float)(r.right - r.left); wh = (float)(r.bottom - r.top); } }
    float k = std::min(box / ww, box / wh);
    float w = ww * k, h = wh * k;
    D2D1_POINT_2F c = ToWin(it.cx, baseline - it.size / 2);
    return { c.x - w / 2, c.y - h / 2, c.x + w / 2, c.y + h / 2 };
}

// miniaturas do DWM das minimizadas, na janela própria entre o fundo e a dos ícones
void UpdateThumbs(float baseline) {
    if (!g_thumbWnd) return;
    float sc = Sc();
    RECT strip = Strip();
    RECT box{ LONG_MAX, LONG_MAX, LONG_MIN, LONG_MIN };
    std::vector<std::pair<const Item*, RECT>> rects;
    bool show = !g_fullscreen && g_reveal > 0.5f;
    for (auto& it : g_items) {
        if (it.kind != I_MINWIN) continue;
        if (!SlotSettled(it)) {  // chegando ou saindo: sem miniatura aqui
            auto f = g_thumbs.find(it.win);
            if (f != g_thumbs.end()) { DWM_THUMBNAIL_PROPERTIES tp{}; tp.dwFlags = DWM_TNP_VISIBLE; tp.fVisible = FALSE; DwmUpdateThumbnailProperties(f->second, &tp); }
            continue;
        }
        if (!g_thumbs.count(it.win)) { HTHUMBNAIL th = nullptr; if (SUCCEEDED(DwmRegisterThumbnail(g_thumbWnd, it.win, &th))) g_thumbs[it.win] = th; }
        D2D1_RECT_F r = MinThumbRect(it, baseline);
        RECT px{ strip.left + (LONG)std::lround(r.left * sc), strip.top + (LONG)std::lround(r.top * sc), strip.left + (LONG)std::lround(r.right * sc), strip.top + (LONG)std::lround(r.bottom * sc) };
        rects.push_back({ &it, px });
        box.left = std::min(box.left, px.left); box.top = std::min(box.top, px.top); box.right = std::max(box.right, px.right); box.bottom = std::max(box.bottom, px.bottom);
    }
    if (rects.empty() || !show) { ShowWindow(g_thumbWnd, SW_HIDE); return; }
    // a janela cobre só as miniaturas (o resto da faixa continua clicável) e fica logo abaixo da dos ícones
    SetWindowPos(g_thumbWnd, g_icons, box.left, box.top, box.right - box.left, box.bottom - box.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    for (auto& [it, px] : rects) {
        auto f = g_thumbs.find(it->win);
        if (f == g_thumbs.end()) continue;
        DWM_THUMBNAIL_PROPERTIES tp{};
        tp.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
        tp.rcDestination = { px.left - box.left, px.top - box.top, px.right - box.left, px.bottom - box.top };
        tp.fVisible = TRUE; tp.opacity = 255; tp.fSourceClientAreaOnly = FALSE;
        DwmUpdateThumbnailProperties(f->second, &tp);
    }
}

// ---- Badge vermelho (número de avisos), como no Mac
// O Windows não deixa ler o número que os apps põem na barra de tarefas (o ícone extra do Discord). Vem de:
//  - notificações do Windows do app (Central de Notificações) que chegaram depois da última vez que ele
//    esteve na frente: abriu o app, o número some;
//  - "(3)" no começo do título da janela (WhatsApp Web, Gmail e afins).
std::map<std::wstring, long long> g_seen;  // id do app -> quando esteve na frente pela última vez (FILETIME)
long long g_shellStart;                    // avisos de antes de o Shell abrir não contam
HWINEVENTHOOK g_fgHook;

long long NowFt() { FILETIME f; GetSystemTimeAsFileTime(&f); return ((long long)f.dwHighDateTime << 32) | f.dwLowDateTime; }
std::wstring LowerS(std::wstring s) { for (auto& c : s) c = towlower(c); return s; }

int BadgeCount(const App& a) {
    if (a.finder) return 0;
    int title = 0;
    for (auto& w : a.wins) {
        const std::wstring& t = w.title;
        if (t.size() > 2 && t[0] == L'(') {
            size_t e = t.find(L')');
            if (e != std::wstring::npos && e > 1 && e < 6) {
                int n = 0; bool ok = true;
                for (size_t i = 1; i < e; i++) { if (!iswdigit(t[i])) { ok = false; break; } n = n * 10 + (t[i] - L'0'); }
                if (ok) title = std::max(title, n);
            }
        }
    }
    long long since = g_shellStart;
    auto it = g_seen.find(a.id);
    if (it != g_seen.end()) since = std::max(since, it->second);
    std::wstring name = LowerS(a.name), id = a.id;
    std::wstring stem = a.exe.substr(a.exe.find_last_of(L'\\') + 1);
    if (stem.size() > 4) stem.resize(stem.size() - 4);
    int toasts = 0;
    static std::vector<notificacoes::Brief> cache;
    static DWORD cacheAt;
    if (GetTickCount() - cacheAt > 300) { cache = notificacoes::Recentes(); cacheAt = GetTickCount(); }
    for (auto& n : cache) {
        if (n.time <= since) continue;
        std::wstring app = LowerS(n.app), au = LowerS(n.aumid);
        bool match = (!name.empty() && app == name) || id == L"aumid:" + au ||
                     (!stem.empty() && stem.size() >= 4 && (app == stem || au.find(stem) != std::wstring::npos));
        if (match) toasts++;
    }
    return std::max(title, toasts);
}

// ib: o corpo visível do ícone (sem a margem dos PNGs do pacote)
// the badge is drawing shared with Linux (src/core/dock_drawing.cpp), through the Direct2D painter with the shell's font
IDWriteTextFormat* ShellFont(float s, DWRITE_FONT_WEIGHT w, DWRITE_TEXT_ALIGNMENT a) { return ui::Format(s, w, false, a); }
float ShellTextWidth(IDWriteTextFormat* tf, const std::wstring& s) { return ui::TextWidth(tf, s); }
void DrawBadge(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* br, D2D1_RECT_F ib, int n) {
    PainterD2D p(rt, br, ShellFont, ShellTextWidth);
    dock_drawing::Badge(p, drawing::Rect{ ib.left, ib.top, ib.right, ib.bottom }, n);
}

void Paint();
void CALLBACK FgEvent(HWINEVENTHOOK, DWORD, HWND h, LONG obj, LONG, DWORD, DWORD) {
    if (obj != OBJID_WINDOW || !h) return;
    if (App* a = apps::OfWindow(h)) {
        bool had = BadgeCount(*a) > 0;
        g_seen[a->id] = NowFt();
        if (had) Paint();
    }
}

// A etiqueta com o nome (pt, na janela dos ícones, sempre em pé): embaixo, acima do ícone com a setinha para baixo;
// dos lados, ao lado do ícone com a setinha apontando para ele (como no Mac com o Dock à esquerda ou à direita).
struct LabelBox { D2D1_RECT_F r{}; D2D1_POINT_2F tri[3]{}; D2D1_POINT_2F in{}; std::wstring t; };  // in: da base da setinha para a ponta
bool LabelGeom(int lab, LabelBox& L) {
    if (!g_rotulos || lab < 0 || lab >= (int)g_items.size() || g_items[lab].kind == I_SEP || g_reveal <= 0.9f) return false;
    auto& it = g_items[lab];
    L.t = Label(it);
    float tw = ui::TextWidth(g_tfLabel, L.t) + 22, th = 26;
    float baseline = g_bg.top + PAD_TOP + g_tile;
    if (!Vert()) {
        float top = baseline - it.size - 14 - th;
        L.r = { it.cx - tw / 2, top, it.cx + tw / 2, top + th };
        float cx = it.cx, ab = L.r.bottom - 0.5f;
        L.tri[0] = { cx - 7, ab }; L.tri[1] = { cx, ab + 7 }; L.tri[2] = { cx + 7, ab };
        L.in = { 0, 1 };
        return true;
    }
    const float MAXW = 250;  // a janela lateral tem ~280 pt para a etiqueta: título comprido ganha "…"
    if (tw > MAXW) {
        while (L.t.size() > 1 && ui::TextWidth(g_tfLabel, L.t + L"…") + 22 > MAXW) L.t.pop_back();
        L.t += L"…";
        tw = ui::TextWidth(g_tfLabel, L.t) + 22;
    }
    D2D1_POINT_2F tip = ToWin(it.cx, baseline - it.size - 7.5f);
    float s = Pos() == 1 ? 1.f : -1.f;  // para dentro da tela
    float ab = tip.x + s * 7;
    L.r = s > 0 ? D2D1::RectF(ab - 0.5f, tip.y - th / 2, ab - 0.5f + tw, tip.y + th / 2) : D2D1::RectF(ab + 0.5f - tw, tip.y - th / 2, ab + 0.5f, tip.y + th / 2);
    L.tri[0] = { ab, tip.y - 7 }; L.tri[1] = tip; L.tri[2] = { ab, tip.y + 7 };
    L.in = { -s, 0 };
    return true;
}

void JumpIds(const App& a, std::wstring& exe, std::wstring& aumid);
void Paint() {
    if (!g_icons || g_fullscreen) { if (g_thumbWnd) ShowWindow(g_thumbWnd, SW_HIDE); return; }
    Layout();
    float sc = Sc();
    RECT strip = Strip();
    int W = strip.right - strip.left, H = strip.bottom - strip.top;
    // Janela justa no Dock; inteira (espaço para a etiqueta e a ampliação) só com o mouse em cima ou
    // animando, assim o resto da faixa de baixo da tela continua clicável.
    bool active = g_mouseX >= 0 || g_dragging || g_resizing || g_press >= 0 || g_dropHit >= 0 || Animating();
    LabelBox lbox;
    bool hasLabel = LabelGeom(g_dragging ? -1 : g_hover, lbox);
    D2D1_RECT_F bgw = RectToWin(g_bg);
    RECT want{ (LONG)std::floor(bgw.left * sc) - 2, (LONG)std::floor(bgw.top * sc) - 2, (LONG)std::ceil(bgw.right * sc) + 2, (LONG)std::ceil(bgw.bottom * sc) + 2 };
    bool poofing = g_poof.t0 > 0 && anim::Now() - g_poof.t0 < POOF_MS;
    if (active && (!Vert() || g_dragging || g_resizing || poofing)) want = RECT{ 0, 0, W, H };
    else if (active) {
        // dos lados a janela é larga (cabe a etiqueta): só a coluna dos ícones (com a ampliação e o pulo) e a
        // etiqueta recebem o mouse; o resto continua clicável para as janelas atrás
        float head = std::max(g_magnify ? g_large - g_tile : 0.f, g_tile * 0.7f) + 8;
        D2D1_RECT_F a = RectToWin({ 0, g_bg.top - head, AxisLen(), WinH() });
        if (hasLabel) { a.left = std::min(a.left, lbox.r.left - 2); a.right = std::max(a.right, lbox.r.right + 2); }
        want = { (LONG)std::floor(a.left * sc), (LONG)std::floor(a.top * sc), (LONG)std::ceil(a.right * sc), (LONG)std::ceil(a.bottom * sc) };
    }
    want.left = std::max<LONG>(want.left, 0); want.right = std::min<LONG>(want.right, W);
    want.top = std::max<LONG>(want.top, 0); want.bottom = std::min<LONG>(want.bottom, H);
    if (want.bottom <= want.top) want.bottom = want.top + 1;
    if (want.right <= want.left) want.right = want.left + 1;
    // A imagem tem sempre o tamanho da faixa inteira (nunca é recriada, então nada pisca); só a
    // janela muda de tamanho e o conteúdo é deslocado para casar.
    auto* rt = ui::GpuBegin(g_gi, W, H, sc);
    if (!rt) return;
    // desenho do Dock no sistema dele (dockT: dos lados, girado); ícones, números e etiquetas em pé (base)
    D2D1_MATRIX_3X2_F base; rt->GetTransform(&base);
    D2D1_MATRIX_3X2_F dockT = DockMatrix() * base;
    rt->SetTransform(dockT);
    bool moved = !EqualRect(&want, &g_win);
    bool dark = ui::Dark();
    ID2D1SolidColorBrush* br = nullptr;
    rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.004f), &br);
    float rad = DOCK_RADIUS;
    // quase transparente: só para o fundo do Dock receber o clique
    rt->FillRoundedRectangle(D2D1::RoundedRect(g_bg, rad, rad), br);
    float px = 1 / sc;
    // bordas do vidro (como no Sequoia): 1 px escuro por fora, 1 px claro logo por dentro
    // (com o Liquid Glass quem desenha o contorno especular é o próprio vidro, em backdrop.cpp)
    // Leopard: a prateleira de vidro em perspectiva. O tampo vai do fundo (mais estreito, acima do pé dos ícones) até a
    // borda da frente; a frente é uma faixa mais escura. Medidas tiradas das capturas do 10.5 (ícone de 48): tampo de
    // ~0,43 ícone de fundo, frente de ~0,07, os lados recolhem ~0,9 da profundidade.
    const bool leo = LEO_DOCK;
    float leoBack = 0, leoFront = 0, leoIn = 0;
    ID2D1PathGeometry* shelf = nullptr;
    auto lin = [&](D2D1_POINT_2F a, D2D1_POINT_2F b, std::initializer_list<D2D1_GRADIENT_STOP> st, auto draw) {
        ID2D1GradientStopCollection* gs = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
        if (SUCCEEDED(rt->CreateGradientStopCollection(st.begin(), (UINT32)st.size(), &gs)) &&
            SUCCEEDED(rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(a, b), gs, &lb))) draw(lb);
        if (lb) lb->Release();
        if (gs) gs->Release();
    };
    if (leo) {
        float base = g_bg.top + PAD_TOP + g_tile;
        leoBack = base - g_tile * 0.52f; leoFront = g_bg.bottom - std::max(2.f, g_tile * 0.04f); leoIn = (leoFront - leoBack) * 0.95f;
        ID2D1Factory* fac = nullptr; rt->GetFactory(&fac);
        if (fac && SUCCEEDED(fac->CreatePathGeometry(&shelf))) {
            ID2D1GeometrySink* sk = nullptr;
            if (SUCCEEDED(shelf->Open(&sk))) {
                sk->BeginFigure({ g_bg.left + leoIn, leoBack }, D2D1_FIGURE_BEGIN_FILLED);
                sk->AddLine({ g_bg.right - leoIn, leoBack }); sk->AddLine({ g_bg.right, leoFront }); sk->AddLine({ g_bg.left, leoFront });
                sk->EndFigure(D2D1_FIGURE_END_CLOSED);
                sk->Close(); sk->Release();
            }
        }
        if (fac) fac->Release();
        // tampo (captura do 10.5 sobre o fundo cinza: ~80 vira ~125 atrás e ~150 na frente): vidro TRANSLÚCIDO — o
        // papel de parede aparece através dele —, um véu branco fraco, um pouco mais forte junto da borda de trás
        if (shelf) lin({ 0, leoBack }, { 0, leoFront }, { { 0, D2D1::ColorF(1, 1, 1, 0.36f) }, { 0.08f, D2D1::ColorF(0.96f, 0.96f, 0.98f, 0.24f) }, { 0.45f, D2D1::ColorF(0.92f, 0.92f, 0.95f, 0.20f) }, { 1, D2D1::ColorF(0.95f, 0.95f, 0.97f, 0.30f) } },
                       [&](ID2D1Brush* b) { rt->FillGeometry(shelf, b); });
        // frente: a borda fina do vidro — um fio escuro e, embaixo, o fio claro
        // a borda da frente do vidro: a quina clara e brilhante (o fio branco forte) e a face fina embaixo, mais clara que o tampo
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.92f));
        rt->FillRectangle(D2D1::RectF(g_bg.left, leoFront, g_bg.right, leoFront + px), br);
        lin({ 0, leoFront + px }, { 0, g_bg.bottom }, { { 0, D2D1::ColorF(0.86f, 0.86f, 0.88f, 0.80f) }, { 1, D2D1::ColorF(0.62f, 0.62f, 0.65f, 0.80f) } },
            [&](ID2D1Brush* b) { rt->FillRectangle(D2D1::RectF(g_bg.left, leoFront + px, g_bg.right, g_bg.bottom), b); });
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.70f));  // a borda do fundo (o fio de luz atrás dos ícones) e os lados
        rt->DrawLine({ g_bg.left + leoIn, leoBack }, { g_bg.right - leoIn, leoBack }, br, px);
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.35f));
        rt->DrawLine({ g_bg.left + leoIn, leoBack }, { g_bg.left, leoFront }, br, px);
        rt->DrawLine({ g_bg.right - leoIn, leoBack }, { g_bg.right, leoFront }, br, px);
    }
    // reflexo no tampo da prateleira (Leopard): o desenho do ícone espelhado no pé dele, recortado pelo tampo e sumindo
    // para a frente
    auto reflect = [&](float baseY, auto draw) {
        if (!leo || !shelf) return;
        D2D1_GRADIENT_STOP st[2] = { { 0, D2D1::ColorF(1, 1, 1, 0.30f) }, { 1, D2D1::ColorF(1, 1, 1, 0) } };  // reflexo sutil e curto (captura do 10.5)
        ID2D1GradientStopCollection* gs = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
        if (SUCCEEDED(rt->CreateGradientStopCollection(st, 2, &gs)) &&
            SUCCEEDED(rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, baseY }, { 0, baseY + g_tile * 0.42f }), gs, &lb))) {
            rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), shelf, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), 1, lb), nullptr);
            D2D1_MATRIX_3X2_F old; rt->GetTransform(&old);
            rt->SetTransform(D2D1::Matrix3x2F::Scale(1, -1, { 0, baseY }) * old);
            draw();
            rt->SetTransform(old);
            rt->PopLayer();
        }
        if (lb) lb->Release();
        if (gs) gs->Release();
    };
    if (!leo && !(g_glass && backdrop::Liquid())) {
    br->SetColor(dark ? D2D1::ColorF(0, 0, 0, 0.40f) : D2D1::ColorF(0, 0, 0, 0.10f));
    rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(g_bg.left + px / 2, g_bg.top + px / 2, g_bg.right - px / 2, g_bg.bottom - px / 2), rad - px / 2, rad - px / 2), br, px);
    br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.16f) : D2D1::ColorF(1, 1, 1, 0.55f));
    rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(g_bg.left + px * 1.5f, g_bg.top + px * 1.5f, g_bg.right - px * 1.5f, g_bg.bottom - px * 1.5f), rad - px * 1.5f, rad - px * 1.5f), br, px);
    }

    float baseline = g_bg.top + PAD_TOP + g_tile;  // pé dos ícones
    for (int i = 0; i < (int)g_items.size(); i++) {
        auto& it = g_items[i];
        if (it.kind == I_SEP && leo) {
            // Leopard: a "faixa de pedestres" do 10.5 — barrinhas brancas horizontais empilhadas do fundo até a frente,
            // em perspectiva (mais largas e mais altas na frente), seguindo a fuga do tampo
            float w = g_bg.right - g_bg.left, t = w > 0 ? (it.cx - g_bg.left) / w : 0.5f;
            float bx = it.cx + (g_bg.left + leoIn + t * (w - 2 * leoIn) - it.cx) * 0.35f;  // a captura mostra a coluna quase em pé (só um pouco de fuga)
            const int N = 7;
            for (int d = 0; d < N; d++) {
                float a = (d + 0.15f) / N, b2 = (d + 0.62f) / N;
                float ya = leoBack + (leoFront - leoBack) * a, yb = leoBack + (leoFront - leoBack) * b2;
                float xa = bx + (it.cx - bx) * a, xb = bx + (it.cx - bx) * b2;
                float ha = g_tile * (0.10f + 0.09f * a), hb = g_tile * (0.10f + 0.09f * b2);
                ID2D1Factory* f2 = nullptr; rt->GetFactory(&f2);
                ID2D1PathGeometry* g2 = nullptr;
                if (f2 && SUCCEEDED(f2->CreatePathGeometry(&g2))) {
                    ID2D1GeometrySink* s2 = nullptr;
                    if (SUCCEEDED(g2->Open(&s2))) {
                        s2->BeginFigure({ xa - ha, ya }, D2D1_FIGURE_BEGIN_FILLED);
                        s2->AddLine({ xa + ha, ya }); s2->AddLine({ xb + hb, yb }); s2->AddLine({ xb - hb, yb });
                        s2->EndFigure(D2D1_FIGURE_END_CLOSED); s2->Close(); s2->Release();
                        br->SetColor(D2D1::ColorF(0.86f, 0.86f, 0.88f, 0.28f + 0.22f * a));  // cinza discreto (fotos de referência do usuário)
                        rt->FillGeometry(g2, br);
                    }
                    g2->Release();
                }
                if (f2) f2->Release();
            }
            continue;
        }
        if (it.kind == I_SEP) {
            br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.22f) : D2D1::ColorF(0, 0, 0, 0.18f));
            float h = g_tile * 0.93f, cy = (g_bg.top + g_bg.bottom) / 2;
            rt->DrawLine({ it.cx, cy - h / 2 }, { it.cx, cy + h / 2 }, br, 1);
            continue;
        }
        if (g_dragging && i == g_press && it.kind == I_APP) continue;  // está na mão do usuário
        if (it.kind == I_MINWIN) {
            if (!SlotSettled(it)) continue;  // chegando ou saindo: o gênio está desenhando a janela
            float s2 = it.size, box = s2 * 0.92f;
            D2D1_RECT_F tr = MinThumbRect(it, baseline);
            (void)box;
            App& a = apps::list[it.app];
            float bs = s2 * 0.40f;  // ícone do app, no canto de baixo à direita
            D2D1_RECT_F br2{ tr.right - bs * 0.78f, tr.bottom - bs * 0.78f, tr.right + bs * 0.22f, tr.bottom + bs * 0.22f };
            rt->SetTransform(base);  // a miniatura e o ícone do app ficam em pé
            if (a.finder) ui::DrawFolder(rt, br2, 0xE8B7);
            else if (ID2D1Bitmap* b = IconBitmap(rt, a)) {
                float art2 = (br2.right - br2.left) * (a.mac ? ART_PACK : ART_WIN);
                float cx2 = (br2.left + br2.right) / 2, cy2 = (br2.top + br2.bottom) / 2;
                rt->DrawBitmap(b, D2D1::RectF(cx2 - art2 / 2, cy2 - art2 / 2, cx2 + art2 / 2, cy2 + art2 / 2), 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            }
            rt->SetTransform(dockT);
            continue;
        }
        float s = it.size * AppFactor(it);
        if (it.kind == I_GHOST) {  // saindo: encolhe junto com o espaço
            if (it.ghost < 0 || it.ghost >= (int)g_ghosts.size() || s < 1) continue;
            auto& gh = g_ghosts[it.ghost];
            auto f = g_bmps.find(gh.id);
            float art = s * (gh.mac ? ART_PACK : ART_WIN);
            D2D1_RECT_F ir{ it.cx - art / 2, baseline - (s + art) / 2, it.cx + art / 2, baseline - (s - art) / 2 };
            rt->SetTransform(base);
            if (f != g_bmps.end() && f->second) rt->DrawBitmap(f->second, Up(ir), 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            rt->SetTransform(dockT);
            continue;
        }
        float dy = it.kind == I_APP ? BounceOffset(apps::list[it.app]) : 0;
        ID2D1Bitmap* b = nullptr;
        bool mac = false;
        if (it.kind == I_APP) { b = IconBitmap(rt, apps::list[it.app]); mac = apps::list[it.app].mac || g_customIcon[apps::list[it.app].id]; }
        else if (it.kind == I_DOWNLOADS && StackMode()) {  // Downloads como Pilha: os arquivos mais novos empilhados
            RefreshStack((int)std::lround(std::min(256.f, g_large * Sc())));
            float art = s * ART_WIN;
            D2D1_RECT_F sr{ it.cx - art / 2, baseline - (s + art) / 2, it.cx + art / 2, baseline - (s - art) / 2 };
            reflect(sr.bottom, [&] { DrawStack(rt, sr); });  // Leopard: a pilha também se reflete na prateleira
            rt->SetTransform(base);
            bool drew = DrawStack(rt, Up(sr));
            rt->SetTransform(dockT);
            if (drew) continue;
            b = PackBitmap(rt, L"folder downloads"); mac = b != nullptr;
        }
        else { b = PackBitmap(rt, it.kind == I_LAUNCHPAD ? L"Launchpad" : it.kind == I_DOWNLOADS ? L"folder downloads" : g_trashFull ? L"trashfull@2x" : L"trashempty@2x"); mac = b != nullptr; }
        // ícones do pacote do Sequoia já têm a margem do Mac; os do Windows (recortados) ficam com o mesmo corpo visual
        float art = s * (mac ? ART_PACK : ART_WIN);
        D2D1_RECT_F ir{ it.cx - art / 2, baseline - (s + art) / 2 + dy, it.cx + art / 2, baseline - (s - art) / 2 + dy };
        if (b) reflect(baseline + dy, [&] {  // espelha no pé do desenho (o PNG do pacote tem margem embaixo)
            rt->DrawBitmap(b, ir, 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC); });
        {   // o ícone, o número e o escurecido do clique em pé (dos lados o Dock gira, os ícones não)
            D2D1_RECT_F iw = Up(ir);
            rt->SetTransform(base);
            if (b) rt->DrawBitmap(b, iw, 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);  // reduz 256 px sem serrilhar
            else if (it.kind == I_APP) { if (apps::list[it.app].finder) ui::DrawFolder(rt, iw, 0xE8B7); }
            else if (it.kind == I_DOWNLOADS) ui::DrawFolder(rt, iw, 0xE896);
            else if (it.kind == I_TRASH) ui::DrawTrash(rt, iw, g_trashFull);
            if (it.kind == I_APP) if (int n = BadgeCount(apps::list[it.app])) {
                float m = mac ? (iw.right - iw.left) * 0.10f : 0;  // PNG do pacote: corpo = 80% do desenho
                DrawBadge(rt, br, D2D1::RectF(iw.left + m, iw.top + m, iw.right - m, iw.bottom - m), n);
            }
            if ((i == g_press && !g_dragging && GetKeyState(VK_LBUTTON) < 0) || i == g_dropHit) {  // pressionado: escurece
                br->SetColor(D2D1::ColorF(0, 0, 0, i == g_dropHit ? 0.25f : 0.35f));
                rt->FillRoundedRectangle(D2D1::RoundedRect(iw, art * 0.22f, art * 0.22f), br);
            }
            rt->SetTransform(dockT);
        }
        // ponto de app aberto; o Finder, como no Mac, está sempre aberto
        if (it.kind == I_APP && (!apps::list[it.app].wins.empty() || apps::list[it.app].finder) && shell::Setting(L"indicadores", L"1") == L"1") {
            if (leo) {
                // Leopard: a luz azul do app aberto, acesa no tampo junto da borda da frente (miolo branco e o brilho
                // azul-ciano espalhado para os lados)
                // medido: miolo (247,252,255) de ~2 px, halo (141,173,223) de ~6 px, a ~0,13 ícone da borda de baixo
                D2D1_POINT_2F lc{ it.cx, g_bg.bottom - g_tile * 0.13f };
                float rx = std::max(3.f, g_tile * 0.14f), ry = std::max(1.5f, g_tile * 0.06f);
                D2D1_GRADIENT_STOP st[3] = { { 0, D2D1::ColorF(1, 1, 1, 1) }, { 0.32f, D2D1::ColorF(0.72f, 0.87f, 1, 0.95f) }, { 1, D2D1::ColorF(0.40f, 0.58f, 0.92f, 0) } };
                ID2D1GradientStopCollection* gs = nullptr; ID2D1RadialGradientBrush* rb = nullptr;
                if (SUCCEEDED(rt->CreateGradientStopCollection(st, 3, &gs)) &&
                    SUCCEEDED(rt->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(lc, { 0, 0 }, rx, ry), gs, &rb)))
                    rt->FillEllipse(D2D1::Ellipse(lc, rx, ry), rb);
                if (rb) rb->Release();
                if (gs) gs->Release();
            } else {
            bool tahoe = TAHOE_DOCK;  // Tahoe: ponto pequeno, um pouco acima da borda; escuro no claro, claro no escuro
            if (tahoe) br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.85f) : D2D1::ColorF(0.05f, 0.07f, 0.20f, 0.85f));
            else br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.55f) : D2D1::ColorF(0, 0, 0, 0.50f));
            float dr = std::max(1.25f, g_tile * (tahoe ? 0.028f : 0.034f));
            float dy = g_bg.bottom - g_tile * (tahoe ? 0.088f : 0.068f);
            if (g_indicador == 1) {  // traço (Personalizar o Dock): uma barrinha arredondada de ~1/3 do ícone
                float hw = g_tile * 0.16f;
                rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(it.cx - hw, dy - dr * 0.8f, it.cx + hw, dy + dr * 0.8f), dr * 0.8f, dr * 0.8f), br);
            } else rt->FillEllipse(D2D1::Ellipse({ it.cx, dy }, dr, dr), br);
            }
        }
    }

    if (g_poof.t0 > 0 && anim::Now() - g_poof.t0 < POOF_MS) {  // o "puf": bolinhas de fumaça que se abrem e somem
        float u = (float)((anim::Now() - g_poof.t0) / POOF_MS), e = 1 - (1 - u) * (1 - u);
        float R = g_tile * (0.30f + 0.55f * e), alpha = (1 - u) * (1 - u);
        const int N = 7;
        for (int k = 0; k < N; k++) {
            float ang = k * 6.2832f / N + 0.4f, r = g_tile * (0.10f + 0.42f * e);
            float px2 = g_poof.x + std::cos(ang) * r, py2 = g_poof.y + std::sin(ang) * r * 0.8f;
            float pr = R * (0.42f + 0.10f * ((k * 37) % 5) / 4.f);
            br->SetColor(dark ? D2D1::ColorF(0.86f, 0.86f, 0.88f, 0.85f * alpha) : D2D1::ColorF(0.97f, 0.97f, 0.98f, 0.95f * alpha));
            rt->FillEllipse(D2D1::Ellipse({ px2, py2 }, pr, pr), br);
            br->SetColor(D2D1::ColorF(0, 0, 0, 0.10f * alpha));
            rt->DrawEllipse(D2D1::Ellipse({ px2, py2 }, pr, pr), br, 0.8f);
        }
        br->SetColor(dark ? D2D1::ColorF(0.93f, 0.93f, 0.95f, 0.9f * alpha) : D2D1::ColorF(1, 1, 1, alpha));
        rt->FillEllipse(D2D1::Ellipse({ g_poof.x, g_poof.y }, R * 0.55f, R * 0.5f), br);
    }

    // etiqueta com o nome, acima do ícone (dos lados, ao lado dele); em pé, nas coordenadas da janela
    rt->SetTransform(base);
    if (hasLabel) {
        const std::wstring& t = lbox.t;
        float th = 26;
        D2D1_RECT_F r = lbox.r;
        if (leo) {  // Leopard: o nome numa pílula preta translúcida com texto branco, sem setinha
            float rr2 = th / 2 - 2;
            D2D1_RECT_F q{ r.left, r.top + 2, r.right, r.bottom - 2 };
            br->SetColor(D2D1::ColorF(0, 0, 0, 0.62f));
            rt->FillRoundedRectangle(D2D1::RoundedRect(q, rr2, rr2), br);
            br->SetColor(D2D1::ColorF(1, 1, 1, 0.20f));
            rt->DrawRoundedRectangle(D2D1::RoundedRect(q, rr2, rr2), br, 0.8f);
            br->SetColor(D2D1::ColorF(1, 1, 1, 0.97f));
            rt->DrawTextW(t.c_str(), (UINT32)t.size(), g_tfLabel, r, br);
        } else {
        br->SetColor(dark ? D2D1::ColorF(0.17f, 0.17f, 0.18f, 0.92f) : D2D1::ColorF(0.94f, 0.94f, 0.95f, 0.95f));
        rt->FillRoundedRectangle(D2D1::RoundedRect(r, 6, 6), br);
        // setinha apontando para o ícone, como no Mac (embaixo para baixo; dos lados para a borda do Dock)
        const D2D1_POINT_2F* tp = lbox.tri;
        D2D1_POINT_2F n{ lbox.in.x * 0.5f, lbox.in.y * 0.5f };
        ID2D1Factory* f = nullptr; rt->GetFactory(&f);
        ID2D1PathGeometry* tri = nullptr; f->CreatePathGeometry(&tri);
        ID2D1GeometrySink* sk = nullptr; tri->Open(&sk);
        sk->BeginFigure(tp[0], D2D1_FIGURE_BEGIN_FILLED);
        sk->AddLine(tp[1]); sk->AddLine(tp[2]);
        sk->EndFigure(D2D1_FIGURE_END_CLOSED); sk->Close(); sk->Release();
        rt->FillGeometry(tri, br);
        br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.14f) : D2D1::ColorF(0, 0, 0, 0.10f));
        rt->DrawRoundedRectangle(D2D1::RoundedRect(r, 6, 6), br, 0.8f);
        rt->DrawLine({ tp[0].x + n.x, tp[0].y + n.y }, tp[1], br, 0.8f);
        rt->DrawLine(tp[1], { tp[2].x + n.x, tp[2].y + n.y }, br, 0.8f);
        tri->Release(); f->Release();
        br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.92f) : D2D1::ColorF(0, 0, 0, 0.85f));
        rt->DrawTextW(t.c_str(), (UINT32)t.size(), g_tfLabel, r, br);
        }
    }
    // ícone sendo arrastado
    if (g_dragging && g_press >= 0 && g_items[g_press].kind == I_APP) {
        App& a = apps::list[g_items[g_press].app];
        POINT c; GetCursorPos(&c);
        float mx = (c.x - strip.left) / sc, my = (c.y - strip.top) / sc;  // na janela (em pé)
        float art = g_tile * (a.mac ? ART_PACK : ART_WIN);
        D2D1_RECT_F ir{ mx - art / 2, my - art / 2, mx + art / 2, my + art / 2 };
        if (a.finder) ui::DrawFolder(rt, ir, L'');
        else if (ID2D1Bitmap* b = IconBitmap(rt, a)) rt->DrawBitmap(b, ir, g_dragOut ? 0.5f : 1.f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        if (g_dragOut && !a.finder) {
            std::wstring t = Tr(L"Remover");
            float tw = ui::TextWidth(g_tfLabel, t) + 22;
            D2D1_RECT_F r{ mx - tw / 2, ir.top - 34, mx + tw / 2, ir.top - 10 };
            br->SetColor(D2D1::ColorF(0.17f, 0.17f, 0.18f, 0.92f));
            rt->FillRoundedRectangle(D2D1::RoundedRect(r, 6, 6), br);
            br->SetColor(D2D1::ColorF(1, 1, 1, 0.92f));
            rt->DrawTextW(t.c_str(), (UINT32)t.size(), g_tfLabel, r, br);
        }
    }
    br->Release();
    if (shelf) shelf->Release();
    ui::GpuEnd(g_gi);
    if (moved) {
        // A janela fica sempre com a faixa inteira e o desenho sem deslocar; só a região (onde ela recebe o
        // mouse) muda. Antes a janela mudava de tamanho e o desenho era deslocado junto, mas as duas coisas
        // chegavam à tela em quadros diferentes: os ícones sumiam por um quadro ao passar o mouse (piscava).
        bool first = IsRectEmpty(&g_win);
        g_win = want;
        SetWindowRgn(g_icons, CreateRectRgn(want.left, want.top, want.right, want.bottom), TRUE);
        RECT cur; GetWindowRect(g_icons, &cur);
        if (first || !EqualRect(&cur, &strip) || !IsWindowVisible(g_icons)) {
            ui::GpuOffset(g_gi, 0.f, 0.f);
            SetWindowPos(g_icons, HWND_TOPMOST, strip.left, strip.top, W, H, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
    }
    PaintBack();
    UpdateThumbs(g_bg.top + PAD_TOP + g_tile);  // depois do fundo: fica entre ele e os ícones
}

bool HitSep(float x, float y) {
    if (y < g_bg.top || y > g_bg.bottom) return false;
    for (auto& it : g_items) if (it.kind == I_SEP && std::fabs(x - it.cx) < SEP_W / 2) return true;
    return false;
}

int HitTest(float x, float y) {
    if (y < g_bg.top - (g_magnify ? g_large - g_tile : 0) || y > g_bg.bottom + MARGIN) return -1;
    for (int i = 0; i < (int)g_items.size(); i++) {
        float half = (g_items[i].kind == I_SEP ? SEP_W : std::max(g_items[i].size, g_tile)) / 2;
        if (x >= g_items[i].cx - half && x < g_items[i].cx + half && g_items[i].kind != I_SEP && g_items[i].kind != I_GHOST) return i;
    }
    return -1;
}

// ---- menus de botão direito
void OpenFinder(const std::wstring& args) {
    if (shell::nativo) { shell::AbrirFinder(args); return; }
    for (auto& a : apps::list) if (a.finder) { ShellExecuteW(nullptr, L"open", a.launch.c_str(), args.c_str(), nullptr, SW_SHOWNORMAL); return; }
}

ctxmenu::Item MI(const std::wstring& l, int id, bool checked = false, bool disabled = false) {
    ctxmenu::Item i; i.label = l; i.id = id; i.checked = checked; i.disabled = disabled; return i;
}
ctxmenu::Item MSep() { ctxmenu::Item i; i.separator = true; return i; }

int ShowMenu(int itemIndex, const std::vector<ctxmenu::Item>& items) {
    ctxmenu::PlainNext();  // o menu do Dock no Mac não tem ícones (nem no Tahoe)
    float sc = Sc();
    POINT pt = ToScreen(g_items[itemIndex].cx, g_bg.top - 8);
    if (!Vert()) {
        pt.x -= (LONG)std::lround(60 * sc);
        // o menu abre para cima: ajusta pela altura aproximada
        pt.y -= (LONG)std::lround(items.size() * 22 * sc + 12 * sc);
    } else  // dos lados: ao lado do ícone, centrado nele (à direita o menu vira para a esquerda sozinho: não cabe)
        pt.y -= (LONG)std::lround((items.size() * 22 * sc + 12 * sc) / 2);
    SetForegroundWindow(g_owner);
    return ctxmenu::ShowItems(g_owner, pt, items);
}

std::vector<std::wstring> LatestDownloads(int n) {
    PWSTR p = nullptr;
    std::vector<std::pair<ULONGLONG, std::wstring>> v;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &p))) {
        std::wstring d = p; CoTaskMemFree(p);
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((d + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (fd.cFileName[0] == L'.' || (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
                v.push_back({ ((ULONGLONG)fd.ftCreationTime.dwHighDateTime << 32) | fd.ftCreationTime.dwLowDateTime, d + L"\\" + fd.cFileName });
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::vector<std::wstring> out;
    for (size_t k = 0; k < v.size() && (int)k < n; k++) out.push_back(v[k].second);
    return out;
}

// o .exe e a identidade (AUMID) de um app do Dock, para achar as listas de atalhos dele
void JumpIds(const App& a, std::wstring& exe, std::wstring& aumid) {
    exe.clear(); aumid.clear();
    for (const std::wstring* c : { &a.exe, &a.launch }) {
        if (c->size() > 4 && !_wcsicmp(c->c_str() + c->size() - 4, L".exe") && c->rfind(L"aumid:", 0) != 0) { exe = *c; break; }
    }
    if (exe.empty() && !a.wins.empty()) {  // pelo processo da janela
        DWORD pid = 0; GetWindowThreadProcessId(a.wins[0].hwnd, &pid);
        if (HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
            wchar_t p[MAX_PATH]; DWORD n = MAX_PATH;
            if (QueryFullProcessImageNameW(ph, 0, p, &n)) exe = p;
            CloseHandle(ph);
        }
    }
    if (a.id.rfind(L"aumid:", 0) == 0) aumid = a.id.substr(6);
    else if (a.launch.rfind(L"shell:AppsFolder\\", 0) == 0) aumid = a.launch.substr(17);
    else if (!a.wins.empty()) aumid = atalhos::AumidOf(a.wins[0].hwnd);
}

void ContextMenu(int i) {
    auto& it = g_items[i];
    if (it.kind == I_DOWNLOADS) {  // como no Mac: ordenar, ver como (Leque, Grade, Lista) e abrir a pasta
        pilhas::Menu(ToScreen(it.cx, g_bg.top - 4));
        g_stackAt = 0; g_stackPaths.clear();  // ordem ou "Exibir como" podem ter mudado
        Paint();
        return;
    }
    if (it.kind == I_LAUNCHPAD) return;
    if (it.kind == I_TRASH) {
        int id = ShowMenu(i, { MI(Tr(L"Abrir"), 1), MSep(), MI(Tr(L"Esvaziar Lixeira…"), 2, false, !g_trashFull) });
        if (id == 1) OpenFinder(L"shell:RecycleBinFolder");  // a Lixeira no Finder
        if (id == 2)  // como no Mac: o alerta antes, depois esvazia sem a pergunta do Windows, com o som
            alertas::Confirm(Tr(L"Tem certeza de que deseja apagar permanentemente os itens da Lixeira?"), Tr(L"Você não pode desfazer esta ação."), Tr(L"Esvaziar Lixeira"), [] {
                SHEmptyRecycleBinW(nullptr, nullptr, SHERB_NOCONFIRMATION | SHERB_NOSOUND);
                shell::Sound(L"empty_trash");
                BuildItems(); Paint();
            });
        return;
    }
    App& a = apps::list[it.app];
    std::vector<ctxmenu::Item> m;
    for (int k = 0; k < (int)a.wins.size() && k < 15; k++) m.push_back(MI(a.wins[k].title, 3000 + k));
    if (!m.empty()) m.push_back(MSep());
    // do próprio app, como no Mac: recentes (projetos do Ableton...) e tarefas ("Nova janela" do Chrome...)
    std::vector<atalhos::Item> jl;
    atalhos::Media media;
    std::wstring mExe, mAumid;
    if (!a.finder) {
        std::wstring exe, aumid;
        JumpIds(a, exe, aumid);
        auto rec = atalhos::Recent(aumid, exe, 8);
        auto tasks = atalhos::Tasks(exe);
        media = atalhos::MediaOf(exe, aumid);
        mExe = exe; mAumid = aumid;
        if (media.has) {  // tocando agora (Spotify e afins), como no Dock do Mac
            std::wstring now = media.title;
            if (!media.artist.empty()) now += L" \u2014 " + media.artist;
            if (!now.empty()) m.push_back(MI(now, 0, false, true));
            m.push_back(MI(media.playing ? Tr(L"Pausar") : Tr(L"Reproduzir"), 7000));
            if (media.canNext) m.push_back(MI(Tr(L"Próxima"), 7001));
            if (media.canPrev) m.push_back(MI(Tr(L"Anterior"), 7002));
            m.push_back(MSep());
        }
        if (!rec.empty()) {
            m.push_back(MI(Tr(L"Recentes"), 0, false, true));
            for (auto& r : rec) { m.push_back(MI(r.title, 6000 + (int)jl.size())); jl.push_back(r); }
            m.push_back(MSep());
        }
        if (!tasks.empty()) {
            for (auto& t : tasks) { m.push_back(MI(t.title, 6000 + (int)jl.size())); jl.push_back(t); }
            m.push_back(MSep());
        }
    }
    if (a.finder) { m.push_back(MI(Tr(L"Nova Janela do Finder"), 20)); m.push_back(MI(Tr(L"Buscar…"), 21)); m.push_back(MSep()); }
    ctxmenu::Item opts = MI(Tr(L"Opções"), 0);
    if (!a.finder) {
        if (a.pinned || (a.recent && a.wins.empty())) opts.sub.push_back(MI(Tr(L"Remover do Dock"), 17));  // fixos e recentes, como no Mac
        if (!a.pinned || !a.wins.empty()) opts.sub.push_back(MI(Tr(L"Manter no Dock"), 10, a.pinned));
    }
    opts.sub.push_back(MI(Tr(L"Abrir ao Iniciar Sessão"), 11, apps::OpenAtLogin(a)));
    opts.sub.push_back(MI(Tr(L"Mostrar no Finder"), 12));
    m.push_back(opts);
    m.push_back(MSep());
    if (!a.wins.empty()) {
        m.push_back(MI(Tr(L"Mostrar Todas as Janelas"), 13));
        m.push_back(MI(Tr(L"Ocultar"), 14));
        if (!a.finder) m.push_back(MI(Tr(L"Encerrar"), 15));
    } else m.push_back(MI(Tr(L"Abrir"), 16));
    std::wstring id0 = a.id;
    int id = ShowMenu(i, m);
    App* ap = apps::FindById(id0);
    if (!ap) return;
    switch (id) {
    case 10: apps::SetPinned(*ap, !ap->pinned); dock::Update(); break;
    case 17:
        if (ap->pinned) apps::SetPinned(*ap, false);
        else apps::ForgetRecent(ap->id);  // recente: sai da lista de recentes
        dock::Update();
        break;
    case 11: apps::SetOpenAtLogin(*ap, !apps::OpenAtLogin(*ap)); break;
    case 12: apps::ShowInFinder(*ap); break;
    case 13: missioncontrol::ToggleApp(ap->id, ap->name); break;  // como no Mac: o Exposé do app
    case 14: apps::Hide(*ap); break;
    case 15: apps::Quit(*ap); break;
    case 16: apps::Launch(*ap); dock::Update(); break;
    case 20: OpenFinder(L""); break;
    case 21: OpenFinder(L"--spotlight"); break;
    }
    if (id >= 6000 && id - 6000 < (int)jl.size()) { atalhos::Run(jl[id - 6000]); return; }
    if (id >= 7000 && id <= 7002) { atalhos::MediaCommand(mExe, mAumid, id - 7000); return; }
    if (id >= 3000 && id - 3000 < (int)ap->wins.size()) {
        HWND w = ap->wins[id - 3000].hwnd;
        if (IsIconic(w)) shell::Restaurar(w);
        SetForegroundWindow(w);
    }
}

void BackgroundMenu() {
    POINT c; GetCursorPos(&c);
    SetForegroundWindow(g_owner);
    if (!Vert()) c.y -= 120;
    std::wstring tema = shell::Setting(L"tema", L"auto");
    int id = ctxmenu::ShowItems(g_owner, c, { MI(Tr(L"Ativar Ocultação"), 1, g_autohide && !g_smart), MI(Tr(L"Ocultação Inteligente"), 7, g_smart),
                                               MI(Tr(L"Ativar Ampliação"), 2, g_magnify), MSep(),
                                               MI(Tr(L"Aparência Automática"), 4, tema != L"claro" && tema != L"escuro"),
                                               MI(Tr(L"Aparência Clara"), 5, tema == L"claro"), MI(Tr(L"Aparência Escura"), 6, tema == L"escuro"), MSep(),
                                               MI(Tr(L"Ajustes do Dock…"), 3) });
    if (id >= 4 && id <= 6) {
        shell::SetSetting(L"tema", id == 4 ? L"auto" : id == 5 ? L"claro" : L"escuro");
        dock::ApplyTheme();
        menubar::ApplyTheme();
        return;
    }
    if (id == 1 || id == 7) {
        bool smart = id == 7 ? !g_smart : false, on = id == 7 ? smart : !(g_autohide && !g_smart);
        shell::SetSetting(L"ocultar", !on ? L"0" : smart ? L"inteligente" : L"1");
        g_autohide = on; g_smart = smart; g_covered = false; g_coverAt = 0; g_hidden = false;
        shell::ApplyWorkArea(); ui::FrameClock(g_icons, WM_DOCKFRAME, true);
    }
    if (id == 2) g_magnify = !g_magnify;
    if (id == 3) shell::OpenSettings(L"dock");
    Paint();
}

void LeftClick(int i) {
    auto& it = g_items[i];
    if (it.kind == I_MINWIN) {  // volta para a tela pelo funil (gênio ao contrário) e sai do Dock
        HWND h = it.win;
        float s = it.size, top = g_bg.top + PAD_TOP + g_tile - s;
        RECT from = ToScreenRect({ it.cx - s / 2, top, it.cx + s / 2, top + s });
        if (genio::Restore(h, from)) { apps::Refresh(); BuildItems(); Paint(); return; }
        shell::Restaurar(h);
        keybd_event(VK_MENU, 0, 0, 0); SetForegroundWindow(h); keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);
        apps::Refresh(); BuildItems(); Paint();
        return;
    }
    if (it.kind == I_APP) {
        App& a = apps::list[it.app];
        // o app já está na frente: o clique oculta (as janelas somem e voltam no próximo clique)
        HWND fg = GetForegroundWindow();
        bool front = false;
        for (auto& w : a.wins) if (w.hwnd == fg && !IsIconic(fg)) front = true;
        if (front && shell::Setting(L"dock_clique_oculta", L"1") == L"1") { apps::Hide(a); apps::Refresh(); BuildItems(); Paint(); return; }
        apps::Activate(a); ui::FrameClock(g_icons, WM_DOCKFRAME, true); return;
    }
    if (it.kind == I_TRASH) { OpenFinder(L"shell:RecycleBinFolder"); return; }  // a Lixeira no Finder, não no Explorador
    if (it.kind == I_LAUNCHPAD && LEO_DOCK) { dock::OpenAppsStack(); return; }  // Leopard: a pasta Aplicativos como Pilha em Grade (10.5)
    if (it.kind == I_LAUNCHPAD) { launchpad::Toggle(); return; }
    if (it.kind == I_DOWNLOADS && shell::Setting(L"pilhas", L"1") == L"1") {  // Pilha em grade, como no Mac
        pilhas::Show(ToScreen(it.cx, g_bg.top - 4));
        return;
    }
    if (it.kind == I_DOWNLOADS) {
        auto files = LatestDownloads(12);
        std::vector<ctxmenu::Item> m;
        for (int k = 0; k < (int)files.size(); k++) m.push_back(MI(files[k].substr(files[k].find_last_of(L'\\') + 1), 5000 + k));
        if (m.empty()) m.push_back(MI(Tr(L"Nenhum item"), 0, false, true));
        m.push_back(MSep());
        m.push_back(MI(Tr(L"Abrir no Finder"), 30));
        int id = ShowMenu(i, m);
        if (id == 30) OpenFinder(L"shell:Downloads");
        else if (id >= 5000 && id - 5000 < (int)files.size()) ShellExecuteW(nullptr, L"open", files[id - 5000].c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

// ---- soltar arquivos no Dock: na Lixeira apaga (com desfazer), num app abre com ele
struct DockDrop : IDropTarget {
    LONG ref = 1;
    STDMETHODIMP QueryInterface(REFIID iid, void** o) override {
        if (iid == IID_IUnknown || iid == IID_IDropTarget) { *o = this; AddRef(); return S_OK; }
        *o = nullptr; return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref; }
    STDMETHODIMP_(ULONG) Release() override { return --ref; }
    int At(POINTL p) {
        D2D1_POINT_2F q = ScreenToLocal(POINT{ p.x, p.y });
        return HitTest(q.x, q.y);
    }
    DWORD Effect(int i) {
        if (i < 0) return DROPEFFECT_NONE;
        if (g_items[i].kind == I_TRASH) return DROPEFFECT_MOVE;
        if (g_items[i].kind == I_APP) return DROPEFFECT_COPY;
        return DROPEFFECT_NONE;
    }
    STDMETHODIMP DragEnter(IDataObject*, DWORD, POINTL p, DWORD* e) override { return DragOver(0, p, e); }
    STDMETHODIMP DragOver(DWORD, POINTL p, DWORD* e) override {
        int i = At(p);
        *e = Effect(i);
        if ((*e ? i : -1) != g_dropHit) { g_dropHit = *e ? i : -1; Paint(); }
        return S_OK;
    }
    STDMETHODIMP DragLeave() override { g_dropHit = -1; Paint(); return S_OK; }
    STDMETHODIMP Drop(IDataObject* d, DWORD, POINTL p, DWORD* e) override {
        int i = At(p);
        *e = Effect(i);
        g_dropHit = -1;
        FORMATETC fe{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM sm{};
        std::vector<std::wstring> files;
        if (*e && SUCCEEDED(d->GetData(&fe, &sm))) {
            HDROP hd = (HDROP)GlobalLock(sm.hGlobal);
            UINT n = DragQueryFileW(hd, 0xFFFFFFFF, nullptr, 0);
            for (UINT k = 0; k < n; k++) { wchar_t f[MAX_PATH * 2]; DragQueryFileW(hd, k, f, MAX_PATH * 2); files.push_back(f); }
            GlobalUnlock(sm.hGlobal);
            ReleaseStgMedium(&sm);
        }
        if (!files.empty() && g_items[i].kind == I_TRASH) {
            std::wstring from;
            for (auto& f : files) { from += f; from += L'\0'; }
            from += L'\0';
            SHFILEOPSTRUCTW op{ g_icons, FO_DELETE, from.c_str(), nullptr, FOF_ALLOWUNDO };
            SHFileOperationW(&op);
            BuildItems();
        } else if (!files.empty() && g_items[i].kind == I_APP) {
            std::wstring args;
            for (auto& f : files) args += L"\"" + f + L"\" ";
            apps::Launch(apps::list[g_items[i].app], args);
        }
        Paint();
        return S_OK;
    }
} g_drop;

LRESULT CALLBACK IconsProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    float sc = Sc();
    switch (m) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_SETCURSOR:
        if (g_sepHover || g_resizing) { SetCursor(LoadCursorW(nullptr, Vert() ? IDC_SIZEWE : IDC_SIZENS)); return TRUE; }  // setinha ↕ no separador (↔ dos lados)
        break;
    case WM_MOUSEMOVE: {
        D2D1_POINT_2F lp = ToLocal(GET_X_LPARAM(l) / sc, GET_Y_LPARAM(l) / sc);  // no sistema do Dock
        float x = lp.x, y = lp.y;
        if (g_resizing) {  // arrastando o separador: para cima aumenta, para baixo diminui
            POINT c; GetCursorPos(&c);
            g_tile = g_tileWant = std::clamp(g_resizeTile + (Inward(c) - g_resizeY) / sc, 16.f, 128.f);
            g_large = std::max(g_large, g_tile);
            Paint();
            return 0;
        }
        g_sepHover = HitSep(x, y);
        TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 };
        TrackMouseEvent(&t);
        if (g_press >= 0 && !g_dragging && (w & MK_LBUTTON)) {
            POINT c; GetCursorPos(&c);
            if (std::abs(c.x - g_pressPt.x) + std::abs(c.y - g_pressPt.y) > 6 * sc && g_items[g_press].kind == I_APP && !apps::list[g_items[g_press].app].finder) {
                g_dragging = true;
                ui::FrameClock(g_icons, WM_DOCKFRAME, true);
            }
        }
        if (g_dragging) {
            g_dragOut = y < g_bg.top - g_tile * 1.2f;
            g_mouseX = x;
            Paint();
            return 0;
        }
        // acima dos ícones (fora da ampliação) já é fora do Dock: solta a faixa na hora, senão ela segurava
        // o clique de quem vai do Dock para algo logo acima dele (a etiqueta some, como no Mac)
        float head = g_magnify ? g_large - g_tile : 0;
        if (y < g_bg.top - head || x < g_bg.left - g_tile || x > g_bg.right + g_tile) {
            if (g_mouseX >= 0 || g_hover >= 0) { g_mouseX = -1; g_hover = -1; if (Animating()) ui::FrameClock(g_icons, WM_DOCKFRAME, true); Paint(); }
            return 0;
        }
        g_mouseX = g_lastMouseX = x;
        if (Animating()) ui::FrameClock(g_icons, WM_DOCKFRAME, true);
        int hv = HitTest(x, y);
        if (hv != g_hover || g_magnify) { g_hover = hv; Paint(); }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (!g_dragging) { g_mouseX = -1; g_hover = -1; if (Animating()) ui::FrameClock(g_icons, WM_DOCKFRAME, true); Paint(); }
        return 0;
    case WM_LBUTTONDOWN: {
        D2D1_POINT_2F lp = ToLocal(GET_X_LPARAM(l) / sc, GET_Y_LPARAM(l) / sc);
        if (HitSep(lp.x, lp.y)) {
            POINT c; GetCursorPos(&c);
            g_resizing = true; g_resizeY = Inward(c); g_resizeTile = g_tile;
            SetCapture(h);
            return 0;
        }
        g_press = HitTest(lp.x, lp.y);
        GetCursorPos(&g_pressPt);
        SetCapture(h);
        Paint();
        return 0;
    }
    case WM_LBUTTONUP: {
        if (g_resizing) {
            g_resizing = false;
            ReleaseCapture();
            shell::SetSetting(L"tamanho", std::to_wstring((int)std::lround(g_tile)));
            shell::ApplyWorkArea();
            Paint();
            return 0;
        }
        ReleaseCapture();
        int p = g_press;
        g_press = -1;
        if (g_dragging) {
            g_dragging = false;
            if (p >= 0 && g_items[p].kind == I_APP) {
                App& a = apps::list[g_items[p].app];
                if (g_dragOut) {  // o "puf" do Mac
                    if (a.pinned) {
                        POINT c; GetCursorPos(&c);
                        D2D1_POINT_2F q = ScreenToLocal(c);
                        g_poof = { q.x, q.y, anim::Now() };
                        apps::SetPinned(a, false); shell::Sound(L"poof_item_off_dock");
                        ui::FrameClock(g_icons, WM_DOCKFRAME, true);
                    }
                }
                else if (a.pinned && g_dragTo >= 0 && g_dragTo != g_items[p].app)  // o lugar aberto ao vivo no Dock
                    apps::Move(g_items[p].app, std::clamp(g_dragTo, 1, (int)apps::list.size() - 1));
            }
            g_dragOut = false;
            BuildItems();
            Paint();
            return 0;
        }
        D2D1_POINT_2F lp = ToLocal(GET_X_LPARAM(l) / sc, GET_Y_LPARAM(l) / sc);
        int i = HitTest(lp.x, lp.y);
        Paint();
        if (i >= 0 && i == p) LeftClick(i);
        return 0;
    }
    case WM_MBUTTONUP: {  // botão do meio: uma janela nova do app (como na barra do Windows)
        D2D1_POINT_2F lp = ToLocal(GET_X_LPARAM(l) / sc, GET_Y_LPARAM(l) / sc);
        int i = HitTest(lp.x, lp.y);
        if (i < 0) return 0;
        auto& it = g_items[i];
        if (it.kind == I_APP) {
            App& a = apps::list[it.app];
            std::wstring exe = a.exe.substr(a.exe.find_last_of(L'\\') + 1);
            bool chromium = false;
            for (auto n : { L"chrome.exe", L"msedge.exe", L"brave.exe", L"vivaldi.exe", L"opera.exe", L"thorium.exe" }) if (!_wcsicmp(exe.c_str(), n)) chromium = true;
            if (a.finder) OpenFinder(L"");
            else apps::Launch(a, chromium ? L"--new-window" : L"");
            Log(L"dock: nova janela de %ls (botão do meio)", a.name.c_str());
            ui::FrameClock(g_icons, WM_DOCKFRAME, true);  // pula enquanto abre
        } else if (it.kind == I_TRASH) OpenFinder(L"shell:RecycleBinFolder");
        else if (it.kind == I_DOWNLOADS) OpenFinder(L"shell:Downloads");
        Paint();
        return 0;
    }
    case WM_RBUTTONUP: {
        D2D1_POINT_2F lp = ToLocal(GET_X_LPARAM(l) / sc, GET_Y_LPARAM(l) / sc);
        int i = HitTest(lp.x, lp.y);
        g_hover = -1;
        if (i >= 0) ContextMenu(i);
        else if (lp.y >= g_bg.top) BackgroundMenu();
        Paint();
        return 0;
    }
    case WM_DOCKSTACK: Paint(); return 0;
    case WM_TIMER:
        if (w == TIMER_HIDECHECK) {
            if (!g_autohide) { KillTimer(h, TIMER_HIDECHECK); return 0; }
            if (WantHidden() != g_hidden) { KillTimer(h, TIMER_HIDECHECK); ui::FrameClock(g_icons, WM_DOCKFRAME, true); }
            return 0;
        }
        break;  // as imagens da pilha de Downloads chegaram
    case WM_DOCKFRAME: {
            double now = anim::Now();
            float dt = g_lastFrame ? (float)std::clamp((now - g_lastFrame) / 1000.0, 0.0, 0.05) : 0.016f;
            g_lastFrame = now;
            // ampliação: chega ao alvo em ~0,15 s (exponencial por tempo, igual em 60 ou 144 Hz)
            float magTarget = g_mouseX >= 0 && !g_dragging ? 1.f : 0.f;
            g_magAmt += (magTarget - g_magAmt) * (1 - std::exp(-dt / 0.05f));
            if (std::fabs(magTarget - g_magAmt) < 0.002f) g_magAmt = magTarget;
            if (g_autohide) {  // aparece com o mouse na borda de baixo
                g_hidden = WantHidden();
                g_reveal = std::clamp(g_reveal + (g_hidden ? -dt : dt) / 0.32f, 0.f, 1.f);  // 0,32 s, com a curva do Mac
            } else g_reveal = 1;
            for (auto& it : g_items) if (it.kind == I_MINWIN && it.leaving && SlotFactor(it) <= 0.001f) { BuildItems(); break; }  // o espaço fechou: sai da lista
            bool anim = Animating();
            Paint();
            if (!anim && !g_autohide) ui::FrameClock(g_icons, WM_DOCKFRAME, false);
            // ocultação parada (toda escondida ou toda à mostra): sem quadro a cada atualização da tela, só uma
            // olhada a cada 100 ms no mouse e nas janelas (antes o Dock se redesenhava sem parar, pesado em GPU fraca)
            else if (!anim && g_autohide && g_reveal == (g_hidden ? 0.f : 1.f)) {
                ui::FrameClock(g_icons, WM_DOCKFRAME, false);
                SetTimer(g_icons, TIMER_HIDECHECK, 100, nullptr);
            }
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK BackProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (m == WM_PAINT) { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); return 0; }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

HWND dock::Hwnd() { return g_icons; }
HWND dock::Fundo() { return g_back; }
HMONITOR dock::MonitorHandle() { return DockMon(); }

int dock::Posicao() { return Pos(); }

int dock::ReservedHeight() {  // a espessura do Dock na borda dele (embaixo, à esquerda ou à direita: dock::Posicao)
    if (!g_icons || g_autohide) return 0;
    return (int)std::lround((g_tile + PAD_TOP + PAD_BOTTOM + MARGIN) * Sc());
}

static void ReadSettings() {
    g_tile = (float)_wtof(shell::Setting(L"tamanho", L"48").c_str());
    g_large = (float)_wtof(shell::Setting(L"ampliacao_tamanho", L"128").c_str());
    g_magnify = shell::Setting(L"ampliacao", L"0") == L"1";
    std::wstring oc = shell::Setting(L"ocultar", L"0");  // 0, 1 (sempre) ou inteligente
    g_autohide = oc == L"1" || oc == L"inteligente";
    g_smart = oc == L"inteligente";
    g_tile = g_tileWant = std::clamp(g_tile, 16.f, 128.f);
    g_large = std::clamp(g_large, g_tile, 256.f);
    // personalização (vazio = o padrão do Mac)
    auto num = [](const wchar_t* k, float def) { std::wstring v = shell::Setting(k, L""); return v.empty() ? def : (float)_wtof(v.c_str()); };
    g_espaco = std::clamp(num(L"dock_espaco", 0), 0.f, 48.f);
    g_margemPt = num(L"dock_margem", -1); if (g_margemPt >= 0) g_margemPt = std::min(g_margemPt, 80.f);
    g_raioPct = num(L"dock_raio", -1); if (g_raioPct >= 0) g_raioPct = std::min(g_raioPct / 100.f, 1.f);
    g_vidroOpac = num(L"dock_opacidade", -1); if (g_vidroOpac >= 0) g_vidroOpac = std::min(g_vidroOpac / 100.f, 1.f);
    std::wstring cor = shell::Setting(L"dock_cor", L"auto");
    g_vidroCor = cor == L"clara" ? 1 : cor == L"escura" ? 2 : cor == L"destaque" ? 3 : 0;
    g_mostraSep = shell::Setting(L"dock_separador", L"1") != L"0";
    g_mostraLaunchpad = shell::Setting(L"dock_launchpad", L"1") != L"0";
    g_indicador = shell::Setting(L"dock_indicador", L"ponto") == L"traco" ? 1 : 0;
    g_rotulos = shell::Setting(L"dock_rotulos", L"1") != L"0";
    std::wstring pos = shell::Setting(L"dock_posicao", L"baixo");
    g_pos = pos == L"esquerda" ? 1 : pos == L"direita" ? 2 : 0;
    g_customIcon.clear();  // a pasta de ícones próprios pode ter mudado
    FitTile();
}

void dock::BadgesChanged() {
    bool any = false;
    for (auto& a : apps::list) {  // o número subiu: o ícone dá um pulo (o Mac avisa assim)
        int n = BadgeCount(a);
        auto f = g_lastBadge.find(a.id);
        if (f != g_lastBadge.end() && n > f->second && shell::Setting(L"pulo_notificacao", L"1") == L"1") { g_nudge[a.id] = GetTickCount(); any = true; }
        g_lastBadge[a.id] = n;
    }
    if (any && g_icons) ui::FrameClock(g_icons, WM_DOCKFRAME, true);
    if (g_icons) Paint();
}

void dock::IconsChanged() {
    for (auto& [k, b] : g_bmps) if (b) b->Release();
    g_bmps.clear();
    if (g_icons) Paint();
}

void dock::ReloadSettings() {
    bool hide = g_autohide, smart = g_smart;
    int pos = g_pos;
    ReadSettings();
    if (!g_icons) return;
    if (pos != g_pos) {  // mudou de lado: a área de trabalho reserva a borda nova (e as janelas maximizadas se ajeitam)
        Log(L"Dock: posição %ls", g_pos == 1 ? L"esquerda" : g_pos == 2 ? L"direita" : L"embaixo");
        g_mouseX = g_lastMouseX = -1; g_hover = -1; g_magAmt = 0;
        // arrasto em andamento (ícone ou separador) é cancelado: a referência era do lado antigo (20/11/2026, o
        // separador arrastado durante a troca levou o tamanho a 128, depois a 16, e gravou um valor qualquer)
        if (g_resizing || g_dragging || g_press >= 0) { g_resizing = g_dragging = g_dragOut = false; g_press = -1; ReleaseCapture(); }
        for (auto& it : g_items) it.cx = 0;
    }
    if (hide != g_autohide || smart != g_smart) { g_hidden = false; g_covered = false; g_coverAt = 0; ui::FrameClock(g_icons, WM_DOCKFRAME, true); }
    g_win = {}; g_lastBg = {};
    for (auto& [k, b] : g_bmps) if (b) b->Release();  // ícones próprios novos ou tirados
    g_bmps.clear();
    BuildItems();  // a pasta Downloads, a divisória ou o Launchpad podem ter entrado ou saído
    Paint();
    if (pos != g_pos) shell::ApplyWorkArea();
}

bool dock::Create(HINSTANCE hi) {
    ReadSettings();
    g_shellStart = NowFt();
    g_fgHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, FgEvent, 0, 0, WINEVENT_OUTOFCONTEXT);

    WNDCLASSW bc{}; bc.lpfnWndProc = BackProc; bc.hInstance = hi; bc.lpszClassName = L"ShellDockFundo";
    RegisterClassW(&bc);
    WNDCLASSW ic{}; ic.lpfnWndProc = IconsProc; ic.hInstance = hi; ic.lpszClassName = L"ShellDock";
    ic.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&ic);
    WNDCLASSW oc{}; oc.lpfnWndProc = DefWindowProcW; oc.hInstance = hi; oc.lpszClassName = L"ShellDockDono";
    RegisterClassW(&oc);

    g_icons = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, ic.lpszClassName, L"Dock", WS_POPUP,
                              0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
    // Fundo: vidro da Composição (curva suave de qualquer raio); se não der, o acrílico antigo.
    g_back = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, bc.lpszClassName, L"", WS_POPUP,
                             0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
    g_glass = g_back && backdrop::Attach(g_back);
    if (!g_glass) {
        if (g_back) DestroyWindow(g_back);
        g_back = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, bc.lpszClassName, L"", WS_POPUP,
                                 0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
    }
    g_owner = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_LAYERED, oc.lpszClassName, L"", WS_POPUP | WS_VISIBLE, -32000, -32000, 1, 1, nullptr, nullptr, hi, nullptr);
    WNDCLASSW tc{}; tc.lpfnWndProc = [](HWND h, UINT m, WPARAM w, LPARAM l) -> LRESULT {
        if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
        if (m == WM_NCHITTEST) return HTTRANSPARENT;  // o clique vai para a janela dos ícones, por cima
        return DefWindowProcW(h, m, w, l);
    };
    tc.hInstance = hi; tc.lpszClassName = L"ShellDockMinimizadas";
    RegisterClassW(&tc);
    g_thumbWnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, tc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, hi, nullptr);
    SetLayeredWindowAttributes(g_owner, 0, 0, LWA_ALPHA);
    if (!g_icons || !g_back) return false;
    for (HWND w : { g_icons, g_back, g_owner, g_thumbWnd }) if (w) apps::ours.push_back(w);
    g_gi.hwnd = g_icons; g_sb.hwnd = g_back;
    DWORD corner = g_glass ? 1 : 2;  // com a Composição quem arredonda é o recorte; sem ela, o DWM (raio 8)
    DwmSetWindowAttribute(g_back, 33, &corner, sizeof(corner));
    COLORREF noBorder = 0xFFFFFFFE;  // DWMWA_COLOR_NONE: sem a borda que o DWM põe em janelas arredondadas
    DwmSetWindowAttribute(g_back, 34 /*DWMWA_BORDER_COLOR*/, &noBorder, sizeof(noBorder));
    if (!g_glass) ui::Acrylic(g_back, ui::Dark() ? ACR_DARK : ACR_LIGHT);
    g_tfLabel = ui::Format(13, DWRITE_FONT_WEIGHT_NORMAL, false, DWRITE_TEXT_ALIGNMENT_CENTER);
    RegisterDragDrop(g_icons, &g_drop);
    BuildItems();
    ShowWindow(g_icons, SW_SHOWNOACTIVATE);
    Paint();
    if (g_autohide) ui::FrameClock(g_icons, WM_DOCKFRAME, true);
    return true;
}

RECT dock::MinimizeTarget() {
    for (auto& it : g_items) if (it.kind == I_TRASH) {
        // o lugar logo antes da Lixeira (dos lados, o mesmo quadrado girado com o Dock)
        float cx = it.cx - SLOT / 2, s = g_tile, top = g_bg.top + PAD_TOP;
        return ToScreenRect({ cx - s / 2, top, cx + s / 2, top + s });
    }
    return { 0, 0, 0, 0 };
}

void dock::Update() {
    BuildItems();
    g_lastFrame = 0;
    if (Animating()) ui::FrameClock(g_icons, WM_DOCKFRAME, true);
    Paint();
}

void dock::SetFullscreen(bool on) {
    if (!g_icons || on == g_fullscreen) return;
    g_fullscreen = on;
    // as miniaturas das janelas minimizadas também (ficavam por cima do YouTube em tela cheia, 19/11/2026)
    if (on) { ShowWindow(g_icons, SW_HIDE); ShowWindow(g_back, SW_HIDE); if (g_thumbWnd) ShowWindow(g_thumbWnd, SW_HIDE); return; }
    g_win = {}; g_lastBg = {};  // força reposicionar e mostrar
    Paint();
}

void dock::ApplyTheme() {
    if (!g_icons) return;
    if (!g_glass) ui::Acrylic(g_back, ui::Dark() ? ACR_DARK : ACR_LIGHT);
    g_lastBg = {};  // repinta a tinta do fundo
    Paint();
}

// Leopard: abre a pasta Aplicativos (o item do Launchpad, à direita) como Pilha em Grade, saindo do ícone dela
void dock::OpenAppsStack() {
    for (auto& it : g_items) {
        if (it.kind != I_LAUNCHPAD) continue;
        POINT a = ToScreen(it.cx, g_bg.top - 4);
        pilhas::ShowApps(a);
        return;
    }
}

void dock::OpenDownloadsStack() {
    for (auto& it : g_items) {
        if (it.kind != I_DOWNLOADS) continue;
        POINT a = ToScreen(it.cx, g_bg.top - 4);
        pilhas::Show(a);
        return;
    }
}

void dock::Raise() {  // por cima de janelas de tela inteira nossas (Launchpad)
    if (!g_icons) return;
    SetWindowPos(g_back, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetWindowPos(g_icons, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}
