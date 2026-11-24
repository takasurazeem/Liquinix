// Notas: o app Notas do macOS Sequoia. Três painéis: barra lateral translúcida (acrílico do DWM) com as
// pastas do iCloud e No Meu Mac, lista de notas agrupada por data (ou galeria) e o editor de texto rico.
// Notas Matemáticas, fixar, Apagadas Recentemente, arrastar notas para pastas, busca em todas as notas,
// tema claro/escuro do sistema. Dados em %APPDATA%\Shell\Notas (grava sozinho enquanto digita).
//   Notas.exe [--dados=<pasta>] [--captura=<arquivo.png> --escuro|--claro --tamanho=LxA --pasta=<id> --nota=<n> --galeria --aa]
#include "notas.h"
#include <windowsx.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <shellscalingapi.h>
#include <wincodec.h>
#include <imm.h>
#include <share.h>
#include <cstdarg>
#include <cwctype>
#include <algorithm>
#include <functional>
#include "../menu/context_menu.h"
#include "../comum/simbolos.h"
#include "../comum/ampliacao.h"  // botão verde com a animação do ampliar do Shell
#include "../comum/idioma.h"
#include "../comum/semaforo.h"
#include "../comum/leopard.h"  // tema Mac OS X Leopard: a barra de metal e a barra lateral azul-acinzentada

#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "windowscodecs.lib")

FILE* g_log;
void Log(const wchar_t* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fwprintf(g_log, L"%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    va_list a; va_start(a, fmt); vfwprintf(g_log, fmt, a); va_end(a);
    fputwc(L'\n', g_log); fflush(g_log);
}

std::wstring g_dataDirOverride;
bool g_dark;

namespace {

// ---------------------------------------------------------------------------
// medidas (pt), da referência do Notas no Sequoia
const float TOOLBAR_H = 52, TOOL_CY = 26, SIDE_ROW_H = 28, SIDE_HDR_H = 30;
const float SIDE_MIN = 160, SIDE_MAX = 340, LIST_MIN = 220, LIST_MAX = 480, EDITOR_MIN = 300;
const UINT_PTR T_BLINK = 1, T_SAVE = 2, T_FRONT = 3;
const UINT WM_APP_TRAZER = WM_APP + 7;  // segunda cópia aberta (Launchpad, Spotlight, Dock): vem para a frente

HWND g_hwnd;
float g_scale = 1, g_dpi = 96;
bool g_active = true, g_hoverLights;
std::wstring g_family = L"Segoe UI", g_mono = L"Consolas";
D2D1_SIZE_F g_capSize{};   // modo captura
bool g_capture;

float g_sideW = 214, g_listW = 300;
bool g_sideHidden;
bool g_quick;   // --nota-rapida: a janelinha da Nota Rápida (canto da tela), só com o editor
bool g_gallery, g_galleryOpen;   // galeria; nota aberta a partir da galeria
std::wstring g_folder = L"notas";
Note* g_sel;
enum Focus { F_SIDE, F_LIST, F_EDITOR, F_SEARCH, F_RENAME };
Focus g_focus = F_LIST;
float g_listScroll, g_sideScroll;
TextField g_search, g_rename;
bool g_searchOpen;
std::wstring g_renameId;
bool g_fmtOpen;
int g_hoverBtn = -1, g_pressBtn = -1;
int g_hoverSide = -1;

// arrastar
Note* g_dragNote; bool g_dragging; POINT g_dragFrom; D2D1_POINT_2F g_dragAt; std::wstring g_dropFolder;
int g_divDrag;  // 1 borda da barra lateral, 2 borda da lista

// folha de confirmação
struct Sheet { bool on = false; std::wstring title, msg, ok; std::function<void()> act; } g_sheet;
D2D1_RECT_F g_sheetOk, g_sheetCancel;

IDWriteTextFormat *g_f13b, *g_f13, *g_f12, *g_f12b, *g_f11, *g_f11b, *g_fCount, *g_fCardT, *g_fCardD, *g_fThumbT, *g_fThumb, *g_f13c, *g_fFmt[5];
ID2D1StrokeStyle* g_round;

D2D1_SIZE_F ClientDip() {
    if (g_capture) return g_capSize;
    RECT rc; GetClientRect(g_hwnd, &rc); return { rc.right / g_scale, rc.bottom / g_scale };
}
bool In(D2D1_RECT_F r, D2D1_POINT_2F p) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; }
void Invalidate() { if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE); }

float SideW() { return g_sideHidden ? 0.f : g_sideW; }
float ListL() { return SideW(); }
// macOS 27 com a lateral escondida: a coluna das notas carrega os três botões, o da lateral, o nome da pasta e o
// "⋯" na barra; com menos de 310 pt ficava tudo apertado ("Not…")
float ListW() { return g_sideHidden && semaforo::Aqua() ? std::max(g_listW, 310.f) : g_listW; }
float ListR() { return g_gallery ? ClientDip().width : SideW() + ListW(); }
float EdL() { return g_gallery ? SideW() : ListR(); }
bool EditorShown() { return !g_gallery || g_galleryOpen; }
float ToolStart() { return g_sideHidden ? 128.f : SideW(); }  // onde começam os botões da lista (sem barra lateral, depois das bolinhas)

// ---- cores
struct Pal { D2D1_COLOR_F side, list, editor, text, text2, text3, line, sel, selFocus, icon, iconOff, hover, field; };
Pal Colors() {
    // Leopard (as Notas do Mail do 10.5): barra lateral azul-acinzentada, lista branca com a seleção azul do Aqua; o papel
    // amarelo pautado é desenhado pelo editor (editor.cpp)
    if (semaforo::Leopard())
        return { leopard::Sidebar(true, false), D2D1::ColorF(1, 1, 1), D2D1::ColorF(1, 0.98f, 0.76f),
                 D2D1::ColorF(0, 0, 0, 0.90f), D2D1::ColorF(0, 0, 0, 0.55f), D2D1::ColorF(0, 0, 0, 0.30f), D2D1::ColorF(0, 0, 0, 0.12f),
                 D2D1::ColorF(0.83f, 0.83f, 0.83f), D2D1::ColorF(56 / 255.f, 117 / 255.f, 215 / 255.f), D2D1::ColorF(0, 0, 0, 0.60f), D2D1::ColorF(0, 0, 0, 0.22f),
                 D2D1::ColorF(0, 0, 0, 0.06f), D2D1::ColorF(1, 1, 1) };
    if (g_dark) return { D2D1::ColorF(0, 0, 0, 0.22f), D2D1::ColorF(0.157f, 0.157f, 0.165f), D2D1::ColorF(0.118f, 0.118f, 0.122f),
                         D2D1::ColorF(1, 1, 1, 0.88f), D2D1::ColorF(1, 1, 1, 0.50f), D2D1::ColorF(1, 1, 1, 0.30f), D2D1::ColorF(1, 1, 1, 0.09f),
                         D2D1::ColorF(1, 1, 1, 0.13f), D2D1::ColorF(0.60f, 0.47f, 0.07f), D2D1::ColorF(1, 1, 1, 0.58f), D2D1::ColorF(1, 1, 1, 0.22f),
                         D2D1::ColorF(1, 1, 1, 0.08f), D2D1::ColorF(1, 1, 1, 0.08f) };
    return { D2D1::ColorF(0, 0, 0, 0), D2D1::ColorF(1, 1, 1), D2D1::ColorF(1, 1, 1),
             D2D1::ColorF(0.11f, 0.11f, 0.12f), D2D1::ColorF(0, 0, 0, 0.50f), D2D1::ColorF(0, 0, 0, 0.28f), D2D1::ColorF(0, 0, 0, 0.08f),
             D2D1::ColorF(0, 0, 0, 0.085f), D2D1::ColorF(1.0f, 0.85f, 0.36f), D2D1::ColorF(0, 0, 0, 0.55f), D2D1::ColorF(0, 0, 0, 0.20f),
             D2D1::ColorF(0, 0, 0, 0.055f), D2D1::ColorF(0, 0, 0, 0.05f) };
}

}  // namespace

D2D1_COLOR_F Accent() { return g_dark ? D2D1::ColorF(1.0f, 0.80f, 0.04f) : D2D1::ColorF(0.98f, 0.74f, 0.0f); }
D2D1_COLOR_F AccentText() { return g_dark ? D2D1::ColorF(1.0f, 0.79f, 0.16f) : D2D1::ColorF(0.86f, 0.60f, 0.0f); }
void RequestPaint() { Invalidate(); }

// ---------------------------------------------------------------------------
// datas
namespace {
SYSTEMTIME Local(ULONGLONG ft) {
    FILETIME f{ (DWORD)ft, (DWORD)(ft >> 32) }, l; SYSTEMTIME s{};
    FileTimeToLocalFileTime(&f, &l); FileTimeToSystemTime(&l, &s);
    return s;
}
long long DayOf(ULONGLONG ft) {
    FILETIME f{ (DWORD)ft, (DWORD)(ft >> 32) }, l;
    FileTimeToLocalFileTime(&f, &l);
    return (long long)((((ULONGLONG)l.dwHighDateTime << 32) | l.dwLowDateTime) / 864000000000ULL);
}
const wchar_t* MONTHS[12] = { L"janeiro", L"fevereiro", L"março", L"abril", L"maio", L"junho", L"julho", L"agosto", L"setembro", L"outubro", L"novembro", L"dezembro" };
const wchar_t* WEEKDAYS[7] = { L"domingo", L"segunda-feira", L"terça-feira", L"quarta-feira", L"quinta-feira", L"sexta-feira", L"sábado" };
std::wstring Cap(std::wstring s) { if (!s.empty()) s[0] = towupper(s[0]); return s; }

std::wstring ShortDate(ULONGLONG ft) {  // na lista: hora (hoje), Ontem, dia da semana, data
    SYSTEMTIME s = Local(ft);
    long long d = DayOf(ft), t = DayOf(store::Now());
    wchar_t b[64];
    if (d == t) swprintf_s(b, L"%02d:%02d", s.wHour, s.wMinute);
    else if (d == t - 1) return Tr(L"Ontem");
    else if (t - d < 7 && t > d) return Cap(Tr(WEEKDAYS[s.wDayOfWeek]));
    else swprintf_s(b, L"%02d/%02d/%02d", s.wDay, s.wMonth, s.wYear % 100);
    return b;
}
}  // namespace

std::wstring LongDate(ULONGLONG ft) {
    SYSTEMTIME s = Local(ft);
    wchar_t b[96];
    if (idioma::Pt()) swprintf_s(b, L"%d de %ls de %d às %02d:%02d", s.wDay, MONTHS[s.wMonth - 1], s.wYear, s.wHour, s.wMinute);
    else return idioma::DateTime(s, idioma::D_LONG);  // "November 20, 2026 at 14:30", "20. November 2026 um 14:30"
    return b;
}

namespace {

// ---------------------------------------------------------------------------
// barra lateral
struct SideItem { bool header = false; std::wstring id, name, glyph; int count = 0; float y = 0; };
std::vector<SideItem> g_side;

int CountIn(const std::wstring& id) {
    int n = 0;
    for (auto& x : store::notes) {
        if (id == L"todas") { if (x->folder != L"apagadas" && x->folder != L"mac") n++; }
        else if (x->folder == id) n++;
    }
    return n;
}

void BuildSide() {
    g_side.clear();
    float y = TOOLBAR_H + 2;
    auto item = [&](const std::wstring& id, const std::wstring& name, const wchar_t* glyph) {
        SideItem s; s.id = id; s.name = Tr(name); s.glyph = glyph; s.count = CountIn(id); s.y = y; y += SIDE_ROW_H;
        g_side.push_back(s);
    };
    auto header = [&](const wchar_t* name) { SideItem s; s.header = true; s.name = name; s.y = y; y += SIDE_HDR_H; g_side.push_back(s); };
    item(L"math", store::FolderName(L"math"), L"x.squareroot");
    if (CountIn(L"rapidas") > 0 || g_folder == L"rapidas") item(L"rapidas", store::FolderName(L"rapidas"), L"note.text");
    header(Tr(L"iCloud"));
    item(L"todas", L"Todo o iCloud", L"folder");
    item(L"notas", store::FolderName(L"notas"), L"folder");
    std::vector<NFolder*> user;
    for (auto& f : store::folders) if (f.id != L"notas" && f.id != L"mac" && f.id != L"math" && f.id != L"rapidas") user.push_back(&f);
    std::sort(user.begin(), user.end(), [](NFolder* a, NFolder* b) { return _wcsicmp(a->name.c_str(), b->name.c_str()) < 0; });
    for (auto* f : user) item(f->id, f->name, L"folder");
    if (CountIn(L"apagadas") > 0) item(L"apagadas", L"Apagadas Recentemente", L"trash");
    header(Tr(L"No Meu Mac"));
    item(L"mac", store::FolderName(L"mac"), L"folder");
}

D2D1_RECT_F SideRowRect(const SideItem& s) { return { 10, s.y - g_sideScroll + 1, SideW() - 10, s.y - g_sideScroll + SIDE_ROW_H - 1 }; }
D2D1_RECT_F NewFolderRect() { auto c = ClientDip(); return { 10, c.height - 34, SideW() - 10, c.height - 8 }; }

int HitSide(D2D1_POINT_2F p) {
    if (g_sideHidden || p.x >= SideW() || p.y < TOOLBAR_H || p.y >= ClientDip().height - 40) return -1;
    for (int i = 0; i < (int)g_side.size(); i++) if (!g_side[i].header && In(SideRowRect(g_side[i]), p)) return i;
    return -1;
}

// ---------------------------------------------------------------------------
// lista de notas
struct Row { bool header = false; std::wstring label; bool pin = false; Note* n = nullptr; float y = 0, h = 0; };
std::vector<Row> g_rows;
std::vector<Note*> g_visible;   // notas na ordem da lista
float g_rowsH;
std::wstring g_lastQuery;

bool Searching() { return g_searchOpen && !g_search.text.empty(); }
std::wstring Lower(std::wstring s) { for (auto& c : s) c = towlower(c); return s; }
bool ShowFolderLine() { return Searching() || g_folder == L"todas"; }
float RowH() { return ShowFolderLine() ? 68.f : 52.f; }

NFolder* CurFolder() { return store::FindFolder(g_folder == L"todas" ? L"notas" : g_folder); }

void BuildRows() {
    g_rows.clear(); g_visible.clear();
    std::vector<Note*> v;
    std::wstring q = Lower(g_search.text);
    for (auto& x : store::notes) {
        Note* n = x.get();
        if (Searching()) {
            if (n->folder == L"apagadas") continue;
            bool hit = false;
            for (auto& p : n->paras) if (Lower(p.text).find(q) != std::wstring::npos) { hit = true; break; }
            if (!hit) continue;
        } else if (g_folder == L"todas") { if (n->folder == L"apagadas" || n->folder == L"mac") continue; }
        else if (n->folder != g_folder) continue;
        v.push_back(n);
    }
    NFolder* f = CurFolder();
    int sort = f && !Searching() ? f->sort : 0;
    bool group = f && !Searching() ? f->group && sort != 2 : true;
    auto key = [&](Note* n) { return sort == 1 ? n->created : n->modified; };
    std::stable_sort(v.begin(), v.end(), [&](Note* a, Note* b) {
        if (sort == 2) return _wcsicmp(a->Title().c_str(), b->Title().c_str()) < 0;
        return key(a) > key(b);
    });
    float y = TOOLBAR_H + 4;
    auto header = [&](const std::wstring& label, bool pin) {
        Row r; r.header = true; r.label = label; r.pin = pin; r.y = y; r.h = pin ? 26.f : 30.f; y += r.h; g_rows.push_back(r);
    };
    bool deleted = g_folder == L"apagadas" && !Searching();
    // fixadas primeiro
    std::vector<Note*> pinned, rest;
    for (Note* n : v) (n->pinned && !deleted ? pinned : rest).push_back(n);
    float rh = RowH();
    auto add = [&](Note* n) { Row r; r.n = n; r.y = y; r.h = rh; y += rh; g_rows.push_back(r); g_visible.push_back(n); };
    if (!pinned.empty()) { header(Tr(L"Fixadas"), true); for (Note* n : pinned) add(n); }
    long long t = DayOf(store::Now());
    SYSTEMTIME now = Local(store::Now());
    std::wstring last = L"\x01";
    for (Note* n : rest) {
        if (group) {
            long long d = DayOf(key(n));
            SYSTEMTIME s = Local(key(n));
            std::wstring g;
            if (d >= t) g = Tr(L"Hoje");
            else if (d == t - 1) g = Tr(L"Ontem");
            else if (t - d <= 7) g = Tr(L"7 Dias Anteriores");
            else if (t - d <= 30) g = Tr(L"30 Dias Anteriores");
            else if (s.wYear == now.wYear) g = Cap(Tr(MONTHS[s.wMonth - 1]));
            else g = std::to_wstring(s.wYear);
            if (g != last) { header(g, false); last = g; }
        } else if (!pinned.empty() && last == L"\x01") { header(Tr(L"Notas"), false); last = Tr(L"Notas"); }
        add(n);
    }
    g_rowsH = y + 10;
}

int HitRow(D2D1_POINT_2F p) {
    if (g_gallery || p.x < ListL() || p.x >= ListR() || p.y < TOOLBAR_H) return -1;
    float y = p.y + g_listScroll;
    for (int i = 0; i < (int)g_rows.size(); i++) if (!g_rows[i].header && y >= g_rows[i].y && y < g_rows[i].y + g_rows[i].h) return i;
    return -1;
}

// galeria
const float CARD_W = 168, CARD_H = 126, CARD_GAP = 22, CARD_TEXT = 46;
struct Card { Note* n; D2D1_RECT_F thumb; };
std::vector<Card> g_cards;
std::vector<std::pair<std::wstring, float>> g_cardHeaders;
float g_galleryH;
void BuildCards() {
    g_cards.clear(); g_cardHeaders.clear();
    float x0 = SideW() + 24, w = ClientDip().width - x0 - 12;
    int cols = std::max(1, (int)((w + CARD_GAP) / (CARD_W + CARD_GAP)));
    float y = TOOLBAR_H + 8;
    int col = 0;
    for (auto& r : g_rows) {
        if (r.header) {
            if (col) { y += CARD_H + CARD_TEXT + 12; col = 0; }
            g_cardHeaders.push_back({ r.label, y }); y += 30; continue;
        }
        float x = x0 + col * (CARD_W + CARD_GAP);
        g_cards.push_back({ r.n, D2D1::RectF(x, y, x + CARD_W, y + CARD_H) });
        if (++col >= cols) { col = 0; y += CARD_H + CARD_TEXT + 12; }
    }
    if (col) y += CARD_H + CARD_TEXT + 12;
    g_galleryH = y;
}
int HitCard(D2D1_POINT_2F p) {
    if (!g_gallery || g_galleryOpen) return -1;
    D2D1_POINT_2F q{ p.x, p.y + g_listScroll };
    for (int i = 0; i < (int)g_cards.size(); i++) { auto r = g_cards[i].thumb; r.bottom += CARD_TEXT; if (In(r, q)) return i; }
    return -1;
}

// ---------------------------------------------------------------------------
// botões das barras de ferramentas
enum Btn { B_SIDEBAR, B_LIST, B_GALLERY, B_TRASH, B_BACK, B_COMPOSE, B_FORMAT, B_CHECK, B_TABLE, B_AUDIO, B_MEDIA, B_COLLAB, B_LOCK, B_SHARE, B_SEARCH, B_FOLDER, B_MORE, B_COUNT };  // B_FOLDER/B_MORE: macOS 27
struct BtnInfo { D2D1_RECT_F r{}; bool shown = false, enabled = true; const wchar_t* glyph = L""; bool chevron = false; float gw = 17, gh = 15; };
BtnInfo g_btn[B_COUNT];
D2D1_RECT_F g_searchRect;

void LayoutButtons() {
    auto c = ClientDip();
    for (auto& b : g_btn) b = BtnInfo{};
    auto put = [&](int id, float cx, const wchar_t* glyph, float w = 32, float gw = 17, float gh = 15, bool chev = false) {
        auto& b = g_btn[id];
        b.r = { cx - w / 2, TOOL_CY - 14, cx + w / 2, TOOL_CY + 14 };
        b.shown = true; b.glyph = glyph; b.gw = gw; b.gh = gh; b.chevron = chev;
    };
    if (g_quick) {  // Nota Rápida: bolinhas, Aa e lista de verificação; compartilhar à direita
        put(B_FORMAT, 112, L"textformat", 34, 20, 13);
        put(B_CHECK, 152, L"checklist", 32, 17, 15);
        put(B_SHARE, c.width - 26, L"square.and.arrow.up", 32, 14, 17);
        bool ed = editor::Current() != nullptr;
        g_btn[B_FORMAT].enabled = g_btn[B_CHECK].enabled = g_btn[B_SHARE].enabled = ed;
        return;
    }
    put(B_SIDEBAR, g_sideHidden ? 104.f : std::min(108.f, SideW() - 22), L"sidebar.left", 32, 18, 14);
    const bool gg = semaforo::Aqua();
    if (gg) {
        // macOS 27 (captura Notes, 2x): na lateral, "nova pasta" e esconder a lateral em botões redondos de vidro; na
        // coluna das notas, o nome da pasta e a contagem à esquerda e "⋯" (Lista/Galeria, Apagar) à direita; nada de
        // colaborar e cadeado; a busca é um campo sempre à vista
        auto round = [&](int id, float cx, const wchar_t* glyph, float gw, float gh) { put(id, cx, glyph, 36, gw, gh); g_btn[id].r.top = TOOL_CY - 18; g_btn[id].r.bottom = TOOL_CY + 18; };
        if (!g_sideHidden) { round(B_SIDEBAR, SideW() - 26, L"sidebar.left", 18, 14); round(B_FOLDER, SideW() - 70, L"folder.badge.plus", 19, 14); }
        else round(B_SIDEBAR, 104.f, L"sidebar.left", 18, 14);
    }
    float L0 = ToolStart() + (g_sideHidden ? 0 : 0);
    put(B_LIST, L0 + 30, L"list.bullet", 36, 15, 12);
    put(B_GALLERY, L0 + 69, L"square.grid.2x2", 36, 14, 14);
    if (gg) { g_btn[B_LIST].shown = g_btn[B_GALLERY].shown = false; }
    float e0;
    if (g_gallery) {
        put(B_TRASH, L0 + 110, L"trash", 32, 14, 16);
        if (g_galleryOpen) put(B_BACK, L0 + 148, L"chevron.left", 30, 8, 14);
        e0 = L0 + (g_galleryOpen ? 166 : 130);
    } else {
        put(B_TRASH, ListR() - 26, L"trash", 32, 14, 16);
        e0 = ListR();
        if (gg) { g_btn[B_TRASH].shown = false; put(B_MORE, ListR() - 28, L"", 36, 16, 4); g_btn[B_MORE].r.top = TOOL_CY - 18; g_btn[B_MORE].r.bottom = TOOL_CY + 18; }
    }
    float e1 = c.width;
    put(B_COMPOSE, e0 + 29, L"square.and.pencil", semaforo::Aqua() ? 28.f : 32.f, 16, 16);  // macOS 27: com a cápsula de 4, um círculo de 36
    put(B_FORMAT, e0 + 92, L"textformat", 34, 20, 13);
    put(B_CHECK, e0 + 133, L"checklist", 32, 17, 15);
    put(B_TABLE, e0 + 176, L"tablecells", 32, 17, 14);
    put(B_AUDIO, e0 + 216, L"waveform", 32, 17, 15);
    put(B_MEDIA, e0 + 262, L"photo.on.rectangle", 44, 18, 14, true);
    // direita: busca (vira campo), compartilhar, cadeado, colaborar
    float right = e1 - 12;
    if (g_searchOpen) {
        float w = std::clamp((e1 - e0) * 0.32f, 150.f, 230.f);
        g_searchRect = { right - w, TOOL_CY - 13, right, TOOL_CY + 13 };
        right = g_searchRect.left - 6;
    } else if (gg) {  // macOS 27: o campo da busca já fica à vista (clique abre para digitar)
        float w = std::clamp((e1 - e0) * 0.32f, 150.f, 230.f);
        put(B_SEARCH, right - w / 2, L"magnifyingglass", w, 13, 13);
        g_btn[B_SEARCH].r.top = TOOL_CY - 18; g_btn[B_SEARCH].r.bottom = TOOL_CY + 18;
        right = right - w - 6;
    } else { put(B_SEARCH, e1 - 28, L"magnifyingglass", 32, 15, 15); right = e1 - 44; }
    put(B_SHARE, right - 22, L"square.and.arrow.up", 32, 14, 17);
    put(B_LOCK, right - 22 - 44, L"lock", 40, 12, 16, true);
    put(B_COLLAB, right - 22 - 92, L"person.crop.circle.badge.plus", 34, 18, 16);
    if (gg) {  // macOS 27: só compartilhar (redondo) à direita dos do meio
        g_btn[B_COLLAB].shown = g_btn[B_LOCK].shown = false;
        put(B_SHARE, right - 20, L"square.and.arrow.up", 36, 14, 17);
        g_btn[B_SHARE].r.top = TOOL_CY - 18; g_btn[B_SHARE].r.bottom = TOOL_CY + 18;
        g_btn[B_COLLAB].r = g_btn[B_SHARE].r;  // para os do meio sumirem antes de encostar
    }
    // sem espaço: os do meio somem antes de encostar nos da direita
    for (int id : { B_MEDIA, B_AUDIO, B_TABLE, B_CHECK, B_FORMAT })
        if (g_btn[id].r.right > g_btn[B_COLLAB].r.left - 6) g_btn[id].shown = false;
    bool ed = EditorShown() && editor::Current() && editor::Current()->folder != L"apagadas";
    for (int id : { B_FORMAT, B_CHECK, B_TABLE, B_AUDIO, B_MEDIA, B_LOCK, B_COLLAB }) g_btn[id].enabled = ed;
    g_btn[B_SHARE].enabled = editor::Current() != nullptr;
    g_btn[B_TRASH].enabled = g_sel != nullptr;
    g_btn[B_COMPOSE].enabled = g_folder != L"apagadas" || Searching();
}

int HitBtn(D2D1_POINT_2F p) {
    for (int i = 0; i < B_COUNT; i++) if (g_btn[i].shown && In(g_btn[i].r, p)) return i;
    return -1;
}

D2D1_POINT_2F LightCenter(int k) { return { 23.f + k * 23.f, TOOL_CY }; }
bool OverLights(D2D1_POINT_2F p) { return p.x >= 10 && p.x < 82 && std::fabs(p.y - TOOL_CY) <= 10; }

// ---------------------------------------------------------------------------
// painel Aa (formatação)
struct FmtItem { D2D1_RECT_F r; int kind; int v; };  // kind: 0 estilo, 1 atributo, 2 lista, 3 realce
std::vector<FmtItem> g_fmtItems;
D2D1_RECT_F g_fmtRect;

// ---------------------------------------------------------------------------
// desenho
void Line(ID2D1DeviceContext* dc, ID2D1Brush* b, float x1, float y1, float x2, float y2, float w = 1) { dc->DrawLine({ x1, y1 }, { x2, y2 }, b, w, g_round); }
void Text(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const std::wstring& s, IDWriteTextFormat* tf, D2D1_RECT_F r, D2D1_COLOR_F c) {
    if (!tf || s.empty()) return;
    br->SetColor(c);
    dc->DrawTextW(s.c_str(), (UINT32)s.size(), tf, r, br, D2D1_DRAW_TEXT_OPTIONS_CLIP | D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
}
float TextW(IDWriteTextFormat* tf, const std::wstring& s) {
    IDWriteTextLayout* l = nullptr; float w = 0;
    if (tf && SUCCEEDED(gfx::dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), tf, 4000, 100, &l))) { DWRITE_TEXT_METRICS m{}; l->GetMetrics(&m); w = m.widthIncludingTrailingWhitespace; l->Release(); }
    return w;
}
void Glyph(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const wchar_t* name, float cx, float cy, float w, float h, D2D1_COLOR_F c) {
    br->SetColor(c);
    simbolo::Draw(dc, name, D2D1::RectF(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2), br);
}
void Chevron(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, float cx, float cy, float s, D2D1_COLOR_F c) {
    br->SetColor(c);
    Line(dc, br, cx - s, cy - s / 2, cx, cy + s / 2, 1.4f); Line(dc, br, cx, cy + s / 2, cx + s, cy - s / 2, 1.4f);
}

void DrawLights(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br) {
    static const D2D1_COLOR_F fill_c[3] = { D2D1::ColorF(1.0f, 0.373f, 0.345f), D2D1::ColorF(0.996f, 0.737f, 0.180f), D2D1::ColorF(0.157f, 0.784f, 0.251f) }; const D2D1_COLOR_F* fill = semaforo::Pick(fill_c, semaforo::FILL);
    static const D2D1_COLOR_F ring_c[3] = { D2D1::ColorF(0.886f, 0.275f, 0.247f), D2D1::ColorF(0.882f, 0.631f, 0.086f), D2D1::ColorF(0.071f, 0.675f, 0.157f) }; const D2D1_COLOR_F* ring = semaforo::Pick(ring_c, semaforo::RING);
    static const D2D1_COLOR_F glyph_c[3] = { D2D1::ColorF(0.302f, 0, 0), D2D1::ColorF(0.6f, 0.341f, 0), D2D1::ColorF(0, 0.396f, 0) }; const D2D1_COLOR_F* glyph = semaforo::Pick(glyph_c, semaforo::GLYPH);
    bool lit = g_active || g_hoverLights;
    for (int k = 0; k < 3; k++) {
        auto c = LightCenter(k);
        br->SetColor(lit ? fill[k] : g_dark ? D2D1::ColorF(0.24f, 0.24f, 0.25f) : D2D1::ColorF(0.839f, 0.839f, 0.839f));
        dc->FillEllipse(D2D1::Ellipse(c, 7, 7), br);
        semaforo::Gloss(dc, c, 7, br);
        br->SetColor(lit ? ring[k] : g_dark ? D2D1::ColorF(0, 0, 0, 0.3f) : D2D1::ColorF(0.729f, 0.729f, 0.737f));
        dc->DrawEllipse(D2D1::Ellipse(c, 6.75f, 6.75f), br, 0.5f);
        if (!g_hoverLights) continue;
        br->SetColor(glyph[k]);
        if (k == 0) { Line(dc, br, c.x - 2.5f, c.y - 2.5f, c.x + 2.5f, c.y + 2.5f, 1.1f); Line(dc, br, c.x - 2.5f, c.y + 2.5f, c.x + 2.5f, c.y - 2.5f, 1.1f); }
        else if (k == 1) Line(dc, br, c.x - 3.3f, c.y, c.x + 3.3f, c.y, 1.2f);
        else {  // tela cheia: dois triângulos
            ID2D1Factory* f = nullptr; dc->GetFactory(&f);
            for (int t = 0; t < 2 && f; t++) {
                float s = t ? 1.f : -1.f;
                ID2D1PathGeometry* g = nullptr; f->CreatePathGeometry(&g);
                ID2D1GeometrySink* sk = nullptr; g->Open(&sk);
                D2D1_POINT_2F a{ c.x + s * 3.1f, c.y + s * 3.1f };
                sk->BeginFigure(a, D2D1_FIGURE_BEGIN_FILLED);
                sk->AddLine({ a.x - s * 4.6f, a.y }); sk->AddLine({ a.x, a.y - s * 4.6f });
                sk->EndFigure(D2D1_FIGURE_END_CLOSED); sk->Close(); sk->Release();
                dc->FillGeometry(g, br); g->Release();
            }
            if (f) f->Release();
        }
    }
}

void DrawSidebar(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const Pal& pal) {
    if (g_sideHidden) return;
    auto c = ClientDip();
    dc->PushAxisAlignedClip(D2D1::RectF(0, TOOLBAR_H, SideW(), c.height - 40), D2D1_ANTIALIAS_MODE_ALIASED);
    D2D1_COLOR_F folderC = g_dark ? D2D1::ColorF(1.0f, 0.80f, 0.04f) : D2D1::ColorF(0.95f, 0.70f, 0.0f);
    for (int i = 0; i < (int)g_side.size(); i++) {
        auto& s = g_side[i];
        float y = s.y - g_sideScroll;
        if (s.header) { Text(dc, br, s.name, g_f11b, D2D1::RectF(18, y + 10, SideW() - 12, y + SIDE_HDR_H - 2), pal.text3); continue; }
        auto r = SideRowRect(s);
        bool sel = s.id == g_folder && !Searching();
        bool drop = g_dragging && s.id == g_dropFolder;
        if (sel) { br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, g_focus == F_SIDE ? 0.16f : 0.11f) : D2D1::ColorF(0, 0, 0, g_focus == F_SIDE ? 0.12f : 0.085f)); float fr2 = semaforo::Aqua() ? 10.f : 6.f; dc->FillRoundedRectangle(D2D1::RoundedRect(r, fr2, fr2), br); }
        if (drop) {
            auto a = Accent(); br->SetColor(D2D1::ColorF(a.r, a.g, a.b, 0.30f)); dc->FillRoundedRectangle(D2D1::RoundedRect(r, 6, 6), br);
            br->SetColor(a); dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 1, r.top + 1, r.right - 1, r.bottom - 1), 5, 5), br, 2);
        }
        float cy = (r.top + r.bottom) / 2;
        Glyph(dc, br, s.glyph.c_str(), 29, cy, s.glyph == L"x.squareroot" ? 15.f : 16.f, s.glyph == L"trash" ? 15.f : 13.f, folderC);
        if (g_focus == F_RENAME && s.id == g_renameId) {
            D2D1_RECT_F fr{ 44, r.top + 2, r.right - 6, r.bottom - 2 };
            br->SetColor(g_dark ? D2D1::ColorF(0.12f, 0.12f, 0.13f) : D2D1::ColorF(1, 1, 1));
            dc->FillRoundedRectangle(D2D1::RoundedRect(fr, 4, 4), br);
            auto a = Accent(); br->SetColor(D2D1::ColorF(a.r, a.g, a.b, 0.8f));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(fr, 4, 4), br, 2);
            ID2D1SolidColorBrush *tb = nullptr, *sb = nullptr, *cb = nullptr;
            dc->CreateSolidColorBrush(pal.text, &tb); dc->CreateSolidColorBrush(D2D1::ColorF(a.r, a.g, a.b, 0.4f), &sb); dc->CreateSolidColorBrush(a, &cb);
            g_rename.Draw(dc, g_f13, D2D1::RectF(fr.left + 3, fr.top, fr.right - 3, fr.bottom), tb, sb, cb, true, false);
            tb->Release(); sb->Release(); cb->Release();
            continue;
        }
        float cw = s.count ? TextW(g_fCount, std::to_wstring(s.count)) : 0;
        Text(dc, br, s.name, g_f13, D2D1::RectF(44, r.top, r.right - cw - 14, r.bottom), pal.text);
        if (s.count) Text(dc, br, std::to_wstring(s.count), g_fCount, D2D1::RectF(r.right - 60, r.top, r.right - 8, r.bottom), pal.text2);
    }
    dc->PopAxisAlignedClip();
    // + Nova Pasta (macOS 27: no botão de vidro da lateral)
    if (semaforo::Aqua()) return;
    auto nr = NewFolderRect();
    float cy = (nr.top + nr.bottom) / 2;
    Glyph(dc, br, L"plus.circle", 27, cy, 15, 15, pal.text2);
    Text(dc, br, Tr(L"Nova Pasta"), g_f13, D2D1::RectF(42, nr.top, nr.right, nr.bottom), pal.text2);
}

void DrawToolbarButtons(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const Pal& pal) {
    if (semaforo::Aqua()) {
        // macOS 27 (captura Notes, 2x): os botões ficam em cápsulas de vidro de 36 de altura — escrever; Aa, lista,
        // tabela, áudio e mídia juntos; cadeado e compartilhar juntos; a busca numa cápsula própria
        auto cap = [&](int a, int b) {
            if (!g_btn[a].shown || !g_btn[b].shown) return;
            D2D1_RECT_F r = D2D1::RectF(std::min(g_btn[a].r.left, g_btn[b].r.left) - 4, TOOL_CY - 18, std::max(g_btn[a].r.right, g_btn[b].r.right) + 4, TOOL_CY + 18);
            br->SetColor(D2D1::ColorF(0, 0, 0, g_dark ? 0.22f : 0.05f));
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left, r.top + 1, r.right, r.bottom + 1.5f), 18, 18), br);
            br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.09f) : D2D1::ColorF(1, 1, 1, 0.92f));
            dc->FillRoundedRectangle(D2D1::RoundedRect(r, 18, 18), br);
            br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.14f) : D2D1::ColorF(0, 0, 0, 0.07f));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), 17.5f, 17.5f), br, 1);
        };
        cap(B_COMPOSE, B_COMPOSE);
        cap(B_FORMAT, B_MEDIA);
        if (g_btn[B_LOCK].shown) cap(B_LOCK, B_SHARE); else cap(B_SHARE, B_SHARE);
        cap(B_SEARCH, B_SEARCH);
        cap(B_SIDEBAR, B_SIDEBAR);
        cap(B_FOLDER, B_FOLDER);
        cap(B_MORE, B_MORE);
        // nome da pasta e quantas notas, à esquerda da coluna das notas (no lugar do seletor lista/galeria)
        if (!g_gallery && !g_quick) {
            float L = ToolStart() + 18, R = g_btn[B_MORE].shown ? g_btn[B_MORE].r.left - 6 : ListR() - 10;
            int n = 0; for (auto& r : g_rows) if (!r.header) n++;
            std::wstring name = Searching() ? std::wstring(Tr(L"Resultados")) : store::FolderName(g_folder);
            wchar_t b[48]; swprintf_s(b, Tr(n == 1 ? L"%d nota" : L"%d notas"), n);
            Text(dc, br, name, g_f13b, D2D1::RectF(L, TOOL_CY - 16, R, TOOL_CY + 1), pal.text);
            Text(dc, br, b, g_f11, D2D1::RectF(L, TOOL_CY + 1, R, TOOL_CY + 15), pal.text2);
        }
    }
    // seletor lista/galeria: o escolhido fica num quadrado cinza arredondado
    auto& seg = g_btn[g_gallery ? B_GALLERY : B_LIST];
    br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.12f) : D2D1::ColorF(0, 0, 0, 0.075f));
    if (seg.shown) dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(seg.r.left, seg.r.top, seg.r.right, seg.r.bottom), 7, 7), br);
    for (int i = 0; i < B_COUNT; i++) {
        auto& b = g_btn[i];
        if (!b.shown) continue;
        if (b.enabled && (g_hoverBtn == i || g_pressBtn == i || (i == B_FORMAT && g_fmtOpen)) && i != B_LIST && i != B_GALLERY) {
            br->SetColor(g_pressBtn == i || (i == B_FORMAT && g_fmtOpen) ? (g_dark ? D2D1::ColorF(1, 1, 1, 0.14f) : D2D1::ColorF(0, 0, 0, 0.09f)) : pal.hover);
            float rr = semaforo::Aqua() ? (b.r.bottom - b.r.top) / 2 : 7.f;  // macOS 27: o realce segue o vidro redondo
            if (semaforo::Aqua() && (i == B_COMPOSE || i == B_SHARE || i == B_SIDEBAR || i == B_FOLDER || i == B_MORE || i == B_SEARCH)) {
                float cx2 = (b.r.left + b.r.right) / 2, cy2 = (b.r.top + b.r.bottom) / 2, rad = i == B_SEARCH ? 0 : 16.f;
                if (rad > 0) dc->FillEllipse(D2D1::Ellipse({ cx2, cy2 }, rad, rad), br); else dc->FillRoundedRectangle(D2D1::RoundedRect(b.r, 18, 18), br);
            } else dc->FillRoundedRectangle(D2D1::RoundedRect(b.r, rr, rr), br);
        }
        D2D1_COLOR_F col = b.enabled ? pal.icon : pal.iconOff;
        if ((i == B_LIST && !g_gallery) || (i == B_GALLERY && g_gallery)) col = g_dark ? D2D1::ColorF(1, 1, 1, 0.85f) : D2D1::ColorF(0, 0, 0, 0.75f);
        float cx = (b.r.left + b.r.right) / 2 - (b.chevron ? 6 : 0);
        if (i == B_MORE) { br->SetColor(col); for (int k = -1; k <= 1; k++) dc->FillEllipse(D2D1::Ellipse({ cx + k * 5.5f, TOOL_CY }, 1.6f, 1.6f), br); continue; }
        if (i == B_SEARCH && semaforo::Aqua()) {  // campo fechado: lupa e "Buscar"
            Glyph(dc, br, b.glyph, b.r.left + 18, TOOL_CY, 13, 13, pal.text2);
            Text(dc, br, Tr(L"Buscar"), g_f13, D2D1::RectF(b.r.left + 32, b.r.top, b.r.right - 8, b.r.bottom), pal.text3);
            continue;
        }
        Glyph(dc, br, b.glyph, cx, TOOL_CY, b.gw, b.gh, col);
        if (b.chevron) Chevron(dc, br, b.r.right - 9, TOOL_CY + 0.5f, 2.6f, col);
    }
    if (g_searchOpen) {
        auto r = g_searchRect;
        br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.09f) : D2D1::ColorF(0, 0, 0, 0.055f));
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, 7, 7), br);
        if (g_focus == F_SEARCH) {
            auto a = Accent(); br->SetColor(D2D1::ColorF(a.r, a.g, a.b, 0.55f));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left - 1.5f, r.top - 1.5f, r.right + 1.5f, r.bottom + 1.5f), 8.5f, 8.5f), br, 3);
        }
        Glyph(dc, br, L"magnifyingglass", r.left + 14, TOOL_CY, 12, 12, pal.text2);
        D2D1_RECT_F tr{ r.left + 26, r.top, r.right - 22, r.bottom };
        if (g_search.text.empty()) Text(dc, br, Tr(L"Buscar"), g_f13, tr, pal.text3);
        ID2D1SolidColorBrush *tb = nullptr, *sb = nullptr, *cb = nullptr;
        auto a = Accent();
        dc->CreateSolidColorBrush(pal.text, &tb); dc->CreateSolidColorBrush(D2D1::ColorF(a.r, a.g, a.b, 0.4f), &sb); dc->CreateSolidColorBrush(a, &cb);
        g_search.Draw(dc, g_f13, tr, tb, sb, cb, g_focus == F_SEARCH && editor::Focused() == false, false);
        tb->Release(); sb->Release(); cb->Release();
        if (!g_search.text.empty()) {  // x para limpar
            br->SetColor(pal.text3); dc->FillEllipse(D2D1::Ellipse({ r.right - 12, TOOL_CY }, 6.5f, 6.5f), br);
            br->SetColor(g_dark ? D2D1::ColorF(0.2f, 0.2f, 0.2f) : D2D1::ColorF(1, 1, 1));
            Line(dc, br, r.right - 14.3f, TOOL_CY - 2.3f, r.right - 9.7f, TOOL_CY + 2.3f, 1.3f); Line(dc, br, r.right - 14.3f, TOOL_CY + 2.3f, r.right - 9.7f, TOOL_CY - 2.3f, 1.3f);
        }
    }
}

void DrawList(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const Pal& pal) {
    auto c = ClientDip();
    float L = ListL(), R = ListR();
    dc->PushAxisAlignedClip(D2D1::RectF(L, TOOLBAR_H, R, c.height), D2D1_ANTIALIAS_MODE_ALIASED);
    if (g_rows.empty()) {
        Text(dc, br, Searching() ? Tr(L"Nenhum Resultado") : Tr(L"Nenhuma Nota"), g_f13c, D2D1::RectF(L, c.height / 2 - 20, R, c.height / 2), pal.text3);
    }
    bool listFocus = g_focus == F_LIST;
    float tx = L + 30;
    for (size_t i = 0; i < g_rows.size(); i++) {
        auto& r = g_rows[i];
        float y = r.y - g_listScroll;
        if (y > c.height || y + r.h < TOOLBAR_H) continue;
        if (r.header) {
            if (r.pin) {
                Glyph(dc, br, L"pin.fill", L + 23, y + r.h - 10, 8, 11, pal.text2);
                Text(dc, br, r.label, g_f11b, D2D1::RectF(L + 31, y + 4, R - 10, y + r.h - 2), pal.text2);
            } else Text(dc, br, r.label, g_f13b, D2D1::RectF(L + 20, y + 6, R - 10, y + r.h - 1), pal.text);
            continue;
        }
        Note* n = r.n;
        bool sel = n == g_sel;
        D2D1_RECT_F rr{ L + 10, y + 1, R - 10, y + r.h - 1 };
        if (sel) { float sr = semaforo::Aqua() ? 12.f : 8.f; br->SetColor(listFocus ? pal.selFocus : pal.sel); dc->FillRoundedRectangle(D2D1::RoundedRect(rr, sr, sr), br); }  // macOS 27: mais redonda
        else {  // divisória fina entre as notas (não encosta na selecionada)
            bool nextSel = i + 1 < g_rows.size() && g_rows[i + 1].n == g_sel && g_sel;
            bool nextHeader = i + 1 >= g_rows.size() || g_rows[i + 1].header;
            if (!nextSel && !nextHeader) { br->SetColor(pal.line); dc->FillRectangle(D2D1::RectF(tx, y + r.h - 0.5f, R - 18, y + r.h + 0.5f), br); }
        }
        D2D1_COLOR_F t1 = sel && listFocus && !g_dark ? D2D1::ColorF(0, 0, 0, 0.88f) : pal.text;
        D2D1_COLOR_F t2 = sel && listFocus && !g_dark ? D2D1::ColorF(0, 0, 0, 0.55f) : pal.text2;
        Text(dc, br, n->Title(), g_f13b, D2D1::RectF(tx, y + 8, R - 22, y + 26), t1);
        std::wstring d = ShortDate(n->modified);
        float dw = TextW(g_f12, d);
        Text(dc, br, d, g_f12, D2D1::RectF(tx, y + 26, tx + dw + 2, y + 43), t1);
        Text(dc, br, n->Preview(), g_f12, D2D1::RectF(tx + dw + 7, y + 26, R - 22, y + 43), t2);
        if (ShowFolderLine()) {
            Glyph(dc, br, n->folder == L"apagadas" ? L"trash" : L"folder", tx + 6, y + 51, 12, 10, t2);
            Text(dc, br, store::FolderName(n->folder), g_f12, D2D1::RectF(tx + 17, y + 43, R - 22, y + 60), t2);
        }
    }
    dc->PopAxisAlignedClip();
}

void DrawThumb(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const Pal& pal, Note* n, D2D1_RECT_F r) {
    br->SetColor(g_dark ? D2D1::ColorF(0.17f, 0.17f, 0.18f) : D2D1::ColorF(1, 1, 1));
    dc->FillRoundedRectangle(D2D1::RoundedRect(r, 8, 8), br);
    br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.12f) : D2D1::ColorF(0, 0, 0, 0.12f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), 7.5f, 7.5f), br, 1);
    dc->PushAxisAlignedClip(D2D1::RectF(r.left + 10, r.top + 8, r.right - 10, r.bottom - 6), D2D1_ANTIALIAS_MODE_ALIASED);
    float y = r.top + 10;
    for (auto& p : n->paras) {
        if (y > r.bottom) break;
        IDWriteTextFormat* tf = p.style <= S_SUBHEADING ? g_fThumbT : g_fThumb;
        float lh = p.style == S_TITLE ? 13.f : p.style <= S_SUBHEADING ? 11.f : 9.5f;
        std::wstring s = p.list == L_BULLET ? L"• " + p.text : p.list == L_DASH ? L"– " + p.text : p.list == L_CHECK ? (p.checked ? L"◉ " : L"○ ") + p.text : p.text;
        if (!s.empty()) Text(dc, br, s, tf, D2D1::RectF(r.left + 10, y, r.right - 10, y + lh + 2), pal.text);
        y += lh;
    }
    dc->PopAxisAlignedClip();
}

void DrawGallery(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const Pal& pal) {
    auto c = ClientDip();
    dc->PushAxisAlignedClip(D2D1::RectF(SideW(), TOOLBAR_H, c.width, c.height), D2D1_ANTIALIAS_MODE_ALIASED);
    if (g_cards.empty()) Text(dc, br, Searching() ? Tr(L"Nenhum Resultado") : Tr(L"Nenhuma Nota"), g_f13c, D2D1::RectF(SideW(), c.height / 2 - 20, c.width, c.height / 2), pal.text3);
    for (auto& h : g_cardHeaders) {
        float y = h.second - g_listScroll;
        Text(dc, br, h.first, g_f13b, D2D1::RectF(SideW() + 24, y + 6, c.width - 10, y + 28), pal.text);
    }
    for (auto& cd : g_cards) {
        D2D1_RECT_F r = cd.thumb; r.top -= g_listScroll; r.bottom -= g_listScroll;
        if (r.top > c.height || r.bottom + CARD_TEXT < TOOLBAR_H) continue;
        DrawThumb(dc, br, pal, cd.n, r);
        if (cd.n == g_sel) {
            br->SetColor(Accent());
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left - 2.5f, r.top - 2.5f, r.right + 2.5f, r.bottom + 2.5f), 10, 10), br, 3);
        }
        if (cd.n->pinned) Glyph(dc, br, L"pin.fill", r.right - 12, r.top + 12, 7, 10, Accent());
        Text(dc, br, cd.n->Title(), g_fCardT, D2D1::RectF(r.left - 6, r.bottom + 6, r.right + 6, r.bottom + 24), pal.text);
        Text(dc, br, ShortDate(cd.n->modified), g_fCardD, D2D1::RectF(r.left - 6, r.bottom + 23, r.right + 6, r.bottom + 40), pal.text2);
    }
    dc->PopAxisAlignedClip();
}

void DrawFormatPanel(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const Pal& pal) {
    g_fmtItems.clear();
    if (!g_fmtOpen) return;
    auto& b = g_btn[B_FORMAT];
    float W = 236, x = std::max(EdL() + 8, (b.r.left + b.r.right) / 2 - W / 2), y = b.r.bottom + 8;
    struct S { const wchar_t* name; int style; float h; };
    const S styles[5] = { { Tr(L"Título"), S_TITLE, 34 }, { Tr(L"Cabeçalho"), S_HEADING, 28 }, { Tr(L"Subcabeçalho"), S_SUBHEADING, 24 }, { Tr(L"Corpo"), S_BODY, 24 }, { Tr(L"Monoespaçado"), S_MONO, 24 } };
    float H = 8 + 34 + 28 + 24 * 3 + 9 + 34 + 9 + 24 * 4 + 9 + 34 + 8;
    g_fmtRect = { x, y, x + W, y + H };
    // sombra + fundo
    for (int k = 4; k >= 1; k--) { br->SetColor(D2D1::ColorF(0, 0, 0, 0.035f)); dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x - k, y - k + 3, x + W + k, y + H + k + 4), 12.f + k, 12.f + k), br); }
    br->SetColor(g_dark ? D2D1::ColorF(0.17f, 0.17f, 0.18f) : D2D1::ColorF(0.975f, 0.975f, 0.975f));
    dc->FillRoundedRectangle(D2D1::RoundedRect(g_fmtRect, 11, 11), br);
    br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.14f) : D2D1::ColorF(0, 0, 0, 0.12f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x + 0.5f, y + 0.5f, x + W - 0.5f, y + H - 0.5f), 10.5f, 10.5f), br, 1);
    float cy = y + 8;
    int cur = editor::CurrentStyle(), curList = editor::CurrentList();
    uint8_t at = editor::CurrentAttrs();
    auto check = [&](float yy, float hh) { Glyph(dc, br, L"checkmark", x + 20, yy + hh / 2, 10, 10, pal.text); };
    for (auto& s : styles) {
        D2D1_RECT_F r{ x + 6, cy, x + W - 6, cy + s.h };
        g_fmtItems.push_back({ r, 0, s.style });
        if (s.style == cur) { auto a = Accent(); br->SetColor(D2D1::ColorF(a.r, a.g, a.b, g_dark ? 0.85f : 0.95f)); dc->FillRoundedRectangle(D2D1::RoundedRect(r, 6, 6), br); }
        if (s.style == cur) check(cy, s.h);
        Text(dc, br, s.name, g_fFmt[s.style], D2D1::RectF(x + 34, cy, x + W - 10, cy + s.h), s.style == cur && !g_dark ? D2D1::ColorF(0, 0, 0, 0.9f) : pal.text);
        cy += s.h;
    }
    auto sep = [&]() { br->SetColor(pal.line); dc->FillRectangle(D2D1::RectF(x + 12, cy + 4, x + W - 12, cy + 5), br); cy += 9; };
    sep();
    // B I U S
    const wchar_t* gl[4] = { L"bold", L"italic", L"underline", L"strikethrough" };
    const uint8_t fl[4] = { A_BOLD, A_ITALIC, A_UNDER, A_STRIKE };
    float bw = (W - 24) / 4;
    D2D1_RECT_F segR{ x + 12, cy + 3, x + W - 12, cy + 31 };
    br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.07f) : D2D1::ColorF(0, 0, 0, 0.05f));
    dc->FillRoundedRectangle(D2D1::RoundedRect(segR, 7, 7), br);
    for (int k = 0; k < 4; k++) {
        D2D1_RECT_F r{ x + 12 + k * bw, cy + 3, x + 12 + (k + 1) * bw, cy + 31 };
        g_fmtItems.push_back({ r, 1, fl[k] });
        if (at & fl[k]) { br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.18f) : D2D1::ColorF(0, 0, 0, 0.12f)); dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 2, r.top + 2, r.right - 2, r.bottom - 2), 5, 5), br); }
        Glyph(dc, br, gl[k], (r.left + r.right) / 2, (r.top + r.bottom) / 2, 12, 13, pal.text);
    }
    cy += 34;
    sep();
    struct LI { const wchar_t* name; int kind; const wchar_t* mark; };
    const LI lists[4] = { { Tr(L"Lista com Marcadores"), L_BULLET, L"•" }, { Tr(L"Lista com Traços"), L_DASH, L"–" }, { Tr(L"Lista Numerada"), L_NUMBER, L"1." }, { Tr(L"Lista de Verificação"), L_CHECK, L"○" } };
    for (auto& l : lists) {
        D2D1_RECT_F r{ x + 6, cy, x + W - 6, cy + 24 };
        g_fmtItems.push_back({ r, 2, l.kind });
        if (curList == l.kind) check(cy, 24);
        Text(dc, br, l.mark, g_f13, D2D1::RectF(x + 34, cy, x + 54, cy + 24), pal.text);
        Text(dc, br, l.name, g_f13, D2D1::RectF(x + 54, cy, x + W - 10, cy + 24), pal.text);
        cy += 24;
    }
    sep();
    // realces: sem, roxo, rosa, laranja, menta, azul
    static const D2D1_COLOR_F hc[HI_COUNT] = { D2D1::ColorF(0.69f, 0.42f, 0.98f), D2D1::ColorF(1.0f, 0.30f, 0.55f), D2D1::ColorF(1.0f, 0.58f, 0.15f), D2D1::ColorF(0.20f, 0.82f, 0.72f), D2D1::ColorF(0.25f, 0.60f, 1.0f) };
    float dx = (W - 24) / 6;
    int curHi = HiOf(at);
    for (int k = 0; k <= HI_COUNT; k++) {
        D2D1_POINT_2F c{ x + 12 + dx * (k + 0.5f), cy + 17 };
        g_fmtItems.push_back({ D2D1::RectF(c.x - dx / 2, cy + 2, c.x + dx / 2, cy + 32), 3, k });
        if (k == 0) {
            br->SetColor(pal.text2); dc->DrawEllipse(D2D1::Ellipse(c, 9, 9), br, 1.2f);
            Line(dc, br, c.x - 6, c.y + 6, c.x + 6, c.y - 6, 1.2f);
        } else { br->SetColor(hc[k - 1]); dc->FillEllipse(D2D1::Ellipse(c, 9, 9), br); }
        if (k == curHi) { br->SetColor(pal.text); dc->DrawEllipse(D2D1::Ellipse(c, 12, 12), br, 1.5f); }
    }
}

void DrawSheet(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const Pal& pal) {
    if (!g_sheet.on) return;
    auto c = ClientDip();
    br->SetColor(D2D1::ColorF(0, 0, 0, g_dark ? 0.30f : 0.12f));
    dc->FillRectangle(D2D1::RectF(0, 0, c.width, c.height), br);
    float W = 280, x = (c.width - W) / 2, y = TOOLBAR_H + 10;
    IDWriteTextLayout* ml = nullptr;
    gfx::dw->CreateTextLayout(g_sheet.msg.c_str(), (UINT32)g_sheet.msg.size(), g_f11, W - 40, 400, &ml);
    float mh = 0;
    if (ml) { ml->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); DWRITE_TEXT_METRICS m{}; ml->GetMetrics(&m); mh = m.height; }
    float H = 20 + 20 + 8 + mh + 20 + 28 + 16;
    D2D1_RECT_F r{ x, y, x + W, y + H };
    for (int k = 6; k >= 1; k--) { br->SetColor(D2D1::ColorF(0, 0, 0, 0.04f)); dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x - k, y - k + 4, x + W + k, y + H + k + 6), 14.f + k, 14.f + k), br); }
    br->SetColor(g_dark ? D2D1::ColorF(0.20f, 0.20f, 0.21f) : D2D1::ColorF(0.96f, 0.96f, 0.96f));
    dc->FillRoundedRectangle(D2D1::RoundedRect(r, 13, 13), br);
    Text(dc, br, g_sheet.title, g_f13c, D2D1::RectF(x + 16, y + 20, x + W - 16, y + 40), pal.text);
    if (ml) { br->SetColor(pal.text); dc->DrawTextLayout({ x + 20, y + 46 }, ml, br); ml->Release(); }
    float by = y + H - 16 - 28, bw = (W - 44) / 2;
    g_sheetCancel = { x + 16, by, x + 16 + bw, by + 28 };
    g_sheetOk = { x + W - 16 - bw, by, x + W - 16, by + 28 };
    br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.16f) : D2D1::ColorF(0, 0, 0, 0.07f));
    dc->FillRoundedRectangle(D2D1::RoundedRect(g_sheetCancel, 7, 7), br);
    br->SetColor(D2D1::ColorF(1.0f, 0.27f, 0.23f));
    dc->FillRoundedRectangle(D2D1::RoundedRect(g_sheetOk, 7, 7), br);
    Text(dc, br, Tr(L"Cancelar"), g_f13c, g_sheetCancel, pal.text);
    Text(dc, br, g_sheet.ok, g_f13c, g_sheetOk, D2D1::ColorF(1, 1, 1));
}

void DrawDragGhost(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const Pal& pal) {
    if (!g_dragging || !g_dragNote) return;
    std::wstring t = g_dragNote->Title();
    float w = std::min(220.f, TextW(g_f13b, t) + 24);
    D2D1_RECT_F r{ g_dragAt.x + 8, g_dragAt.y - 12, g_dragAt.x + 8 + w, g_dragAt.y + 14 };
    br->SetColor(g_dark ? D2D1::ColorF(0.25f, 0.25f, 0.26f, 0.95f) : D2D1::ColorF(1, 1, 1, 0.95f));
    dc->FillRoundedRectangle(D2D1::RoundedRect(r, 7, 7), br);
    br->SetColor(pal.line); dc->DrawRoundedRectangle(D2D1::RoundedRect(r, 7, 7), br, 1);
    Text(dc, br, t, g_f13b, D2D1::RectF(r.left + 12, r.top, r.right - 8, r.bottom), pal.text);
}

void Render(ID2D1DeviceContext* dc) {
    Pal pal = Colors();
    auto c = ClientDip();
    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(pal.text, &br);
    if (!g_round) {
        ID2D1Factory* f = nullptr; dc->GetFactory(&f);
        f->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND), nullptr, 0, &g_round);
        f->Release();
    }
    LayoutButtons();
    // barra lateral translúcida; lista e editor sólidos
    br->SetColor(pal.side);
    dc->FillRectangle(D2D1::RectF(0, 0, SideW(), c.height), br);
    if (g_gallery) {
        br->SetColor(pal.editor);
        dc->FillRectangle(D2D1::RectF(SideW(), 0, c.width, c.height), br);
    } else {
        br->SetColor(pal.list); dc->FillRectangle(D2D1::RectF(ListL(), 0, ListR(), c.height), br);
        br->SetColor(pal.editor); dc->FillRectangle(D2D1::RectF(ListR(), 0, c.width, c.height), br);
    }
    // conteúdo
    D2D1_RECT_F edArea{ EdL(), TOOLBAR_H, c.width, c.height };
    if (g_gallery && !g_galleryOpen) DrawGallery(dc, br, pal);
    else {
        if (!g_gallery) DrawList(dc, br, pal);
        if (editor::Current()) editor::Draw(dc, edArea);
        else Text(dc, br, L"", g_f13c, edArea, pal.text3);
    }
    // barras de cima (por cima do conteúdo rolado)
    if (g_gallery) { br->SetColor(pal.editor); dc->FillRectangle(D2D1::RectF(SideW(), 0, c.width, TOOLBAR_H), br); }
    else {
        br->SetColor(pal.list); dc->FillRectangle(D2D1::RectF(ListL(), 0, ListR(), TOOLBAR_H), br);
        br->SetColor(pal.editor); dc->FillRectangle(D2D1::RectF(ListR(), 0, c.width, TOOLBAR_H), br);
    }
    // divisórias
    br->SetColor(g_dark ? D2D1::ColorF(0, 0, 0, 0.6f) : D2D1::ColorF(0, 0, 0, 0.10f));
    if (!g_sideHidden) dc->FillRectangle(D2D1::RectF(SideW() - 0.5f, 0, SideW() + 0.5f, c.height), br);
    if (!g_gallery) dc->FillRectangle(D2D1::RectF(ListR() - 0.5f, 0, ListR() + 0.5f, c.height), br);
    if (g_listScroll > 0 && !g_gallery) { br->SetColor(pal.line); dc->FillRectangle(D2D1::RectF(ListL(), TOOLBAR_H - 0.5f, ListR(), TOOLBAR_H + 0.5f), br); }
    if (semaforo::Leopard()) leopard::Metal(dc, D2D1::RectF(0, 0, c.width, TOOLBAR_H), GetForegroundWindow() == g_hwnd, false, true, 0);  // a barra unificada de metal
    DrawSidebar(dc, br, pal);
    DrawLights(dc, br);
    DrawToolbarButtons(dc, br, pal);
    DrawFormatPanel(dc, br, pal);
    DrawDragGhost(dc, br, pal);
    DrawSheet(dc, br, pal);
    br->Release();
}

void Paint() {
    ID2D1DeviceContext* dc = gfx::Begin(g_dpi);
    if (!dc) return;
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    // os cantos redondos da janela (16 no Tahoe e no Golden Gate, 12 no Sequoia): sem isso o conteúdo ia até o canto
    // reto e sobrava um pedaço branco fora do arco da borda
    ID2D1Factory* f = nullptr; dc->GetFactory(&f);
    ID2D1RoundedRectangleGeometry* rg = nullptr;
    auto c = ClientDip();
    float R = IsZoomed(g_hwnd) ? 0.f : semaforo::Liquid() ? 16.f : 12.f;
    if (f && R > 0) f->CreateRoundedRectangleGeometry(D2D1::RoundedRect(D2D1::RectF(0, 0, c.width, c.height), R, R), &rg);
    if (rg) dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), rg), nullptr);
    Render(dc);
    if (rg) { dc->PopLayer(); rg->Release(); }
    if (f) f->Release();
    gfx::End();
}

// ---------------------------------------------------------------------------
// ações
std::wstring g_statePath;

void SaveState() {
    if (g_capture || g_statePath.empty()) return;
    WINDOWPLACEMENT wp{ sizeof(wp) }; GetWindowPlacement(g_hwnd, &wp);
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_statePath.c_str(), L"w, ccs=UTF-8") || !f) return;
    RECT& r = wp.rcNormalPosition;
    fwprintf(f, L"janela=%ld %ld %ld %ld %d\nlateral=%g %d\nlista=%g\ngaleria=%d\npasta=%ls\nnota=%ls\n", r.left, r.top, r.right, r.bottom, wp.showCmd == SW_SHOWMAXIMIZED ? 1 : 0,
             g_sideW, g_sideHidden ? 1 : 0, g_listW, g_gallery ? 1 : 0, g_folder.c_str(), g_sel ? g_sel->id.c_str() : L"");
    fclose(f);
}

void SaveSoon() { if (g_hwnd) SetTimer(g_hwnd, T_SAVE, 600, nullptr); }

void Refresh() {
    BuildSide(); BuildRows();
    if (g_gallery) BuildCards();
    Invalidate();
}

void RevealSel() {
    if (g_gallery) {
        for (auto& cd : g_cards) if (cd.n == g_sel) {
            float h = ClientDip().height;
            if (cd.thumb.top - 40 < g_listScroll + TOOLBAR_H) g_listScroll = std::max(0.f, cd.thumb.top - TOOLBAR_H - 40);
            if (cd.thumb.bottom + CARD_TEXT > g_listScroll + h) g_listScroll = cd.thumb.bottom + CARD_TEXT - h + 8;
        }
        return;
    }
    for (auto& r : g_rows) if (!r.header && r.n == g_sel) {
        float h = ClientDip().height;
        if (r.y - g_listScroll < TOOLBAR_H + 4) g_listScroll = std::max(0.f, r.y - TOOLBAR_H - 34);
        if (r.y + r.h - g_listScroll > h) g_listScroll = r.y + r.h - h + 4;
    }
}

// notas vazias somem quando você sai delas (como no Mac)
void DropIfEmpty(Note* n) {
    if (!n || n == g_sel || !n->Empty()) return;
    bool exists = false;
    for (auto& x : store::notes) if (x.get() == n) exists = true;
    if (!exists) return;
    if (editor::Current() == n) editor::SetNote(nullptr);
    store::RemoveNote(n);
}

void SelectNote(Note* n, bool keepScroll = false) {
    Note* old = g_sel;
    g_sel = n;
    editor::SetNote(n);
    editor::SetSearch(Searching() ? g_search.text : L"");
    if (old != n) DropIfEmpty(old);
    Refresh();
    if (!keepScroll) RevealSel();
    SaveSoon();
}

void SelectFolder(const std::wstring& id) {
    g_folder = id;
    g_listScroll = 0;
    if (g_searchOpen) { g_searchOpen = false; g_search.SetText(L"", false); }
    g_galleryOpen = false;
    Note* old = g_sel;
    g_sel = nullptr;
    BuildRows();
    Note* first = g_visible.empty() ? nullptr : g_visible[0];
    if (old && std::find(g_visible.begin(), g_visible.end(), old) != g_visible.end() && !old->Empty()) first = old;
    g_sel = first;
    editor::SetNote(first);
    editor::SetSearch(L"");
    DropIfEmpty(old);
    Refresh();
    SaveSoon();
}

std::wstring TargetFolderForNew() {
    if (g_folder == L"todas" || g_folder == L"apagadas" || Searching()) return L"notas";
    return g_folder;
}

void NewNoteCmd() {
    if (g_searchOpen) { g_searchOpen = false; g_search.SetText(L"", false); }
    if (g_folder == L"apagadas") g_folder = L"notas";
    // já está numa nota vazia: só põe o cursor nela
    if (g_sel && g_sel->Empty() && g_sel->folder == TargetFolderForNew()) { g_focus = F_EDITOR; editor::Focus(true); Invalidate(); return; }
    Note* n = store::NewNote(TargetFolderForNew());
    if (n->folder == L"math") { Para p; p.style = S_BODY; n->paras.push_back(p); }
    if (g_gallery) g_galleryOpen = true;
    SelectNote(n);
    g_focus = F_EDITOR;
    editor::Focus(true);
    editor::PlaceCaretAtEnd();
    if (n->folder == L"math") editor::PlaceCaretAtEnd();
    Invalidate();
}

void MoveNote(Note* n, const std::wstring& folder) {
    if (!n || n->folder == folder) return;
    if (folder == L"apagadas") { n->prevFolder = n->folder; n->deleted = store::Now(); n->pinned = false; }
    else n->deleted = 0;
    n->folder = folder;
    n->dirty = true;
    store::SaveNote(n);
}

void DeleteNoteCmd(Note* n) {
    if (!n) return;
    int idx = (int)(std::find(g_visible.begin(), g_visible.end(), n) - g_visible.begin());
    auto pickNext = [idx]() {
        BuildRows();
        Note* next = nullptr;
        if (!g_visible.empty()) next = g_visible[std::min(idx, (int)g_visible.size() - 1)];
        g_sel = nullptr;
        SelectNote(next);
    };
    if (n->folder == L"apagadas") {
        g_sheet = { true, Tr(L"Apagar esta nota definitivamente?"), Tr(L"Esta ação não pode ser desfeita."), Tr(L"Apagar"), [n, pickNext]() {
            if (editor::Current() == n) editor::SetNote(nullptr);
            if (g_sel == n) g_sel = nullptr;
            store::RemoveNote(n);
            pickNext();
        } };
        Invalidate();
        return;
    }
    if (n->Empty()) {
        if (editor::Current() == n) editor::SetNote(nullptr);
        g_sel = nullptr;
        store::RemoveNote(n);
    } else MoveNote(n, L"apagadas");
    pickNext();
}

void RecoverNote(Note* n) {
    std::wstring to = n->prevFolder.empty() || (!store::FindFolder(n->prevFolder)) ? L"notas" : n->prevFolder;
    MoveNote(n, to);
    BuildRows();
    SelectNote(g_visible.empty() ? nullptr : g_visible[0]);
}

void StartRename(const std::wstring& id) {
    if (id == L"todas" || id == L"apagadas") return;
    g_renameId = id;
    g_rename.SetText(store::FolderName(id), true);
    g_focus = F_RENAME;
    editor::Focus(false);
    Invalidate();
}

void CommitRename() {
    if (g_focus != F_RENAME) return;
    std::wstring t = g_rename.text;
    while (!t.empty() && t.back() == L' ') t.pop_back();
    if (NFolder* f = store::FindFolder(g_renameId)) if (!t.empty()) { f->name = t; store::SaveFolders(); }
    g_focus = F_SIDE;
    Refresh();
}

void NewFolderCmd() {
    if (g_sideHidden) { g_sideHidden = false; }
    NFolder f;
    wchar_t id[40]; swprintf_s(id, L"p%llx", store::Now());
    f.id = id;
    std::wstring base = Tr(L"Nova Pasta"), name = base;
    for (int k = 2;; k++) {
        bool used = false;
        for (auto& x : store::folders) if (!_wcsicmp(x.name.c_str(), name.c_str())) used = true;
        if (!used) break;
        name = base + L" " + std::to_wstring(k);
    }
    f.name = name;
    store::folders.push_back(f);
    store::SaveFolders();
    SelectFolder(f.id);
    StartRename(f.id);
}

void DeleteFolderCmd(const std::wstring& id) {
    NFolder* f = store::FindFolder(id);
    if (!f || id == L"notas" || id == L"mac" || id == L"math" || id == L"rapidas") return;
    g_sheet = { true, Tr(L"Apagar a pasta “") + f->name + L"”?", Tr(L"As notas desta pasta vão para Apagadas Recentemente."), Tr(L"Apagar"), [id]() {
        for (auto& n : store::notes) if (n->folder == id) MoveNote(n.get(), L"apagadas");
        for (size_t i = 0; i < store::folders.size(); i++) if (store::folders[i].id == id) { store::folders.erase(store::folders.begin() + i); break; }
        store::SaveFolders();
        SelectFolder(L"notas");
    } };
    Invalidate();
}

void CopyText(const std::wstring& s) {
    if (!OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t));
    if (g) { memcpy(GlobalLock(g), s.c_str(), (s.size() + 1) * sizeof(wchar_t)); GlobalUnlock(g); if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g); }
    CloseClipboard();
}

POINT ScreenPt(float x, float y) { POINT p{ (LONG)(x * g_scale), (LONG)(y * g_scale) }; ClientToScreen(g_hwnd, &p); return p; }

using ctxmenu::Item;
Item I(const wchar_t* t, int id, bool dis = false, bool chk = false, const wchar_t* sc = L"") { Item i; i.label = t; i.id = id; i.disabled = dis; i.checked = chk; i.shortcut = sc; return i; }
Item Sep() { Item i; i.separator = true; return i; }

void NoteMenu(Note* n, POINT sp) {
    std::vector<Item> items;
    std::vector<std::wstring> ids;
    if (n->folder == L"apagadas") {
        items = { I(Tr(L"Recuperar"), 10), I(Tr(L"Apagar Definitivamente"), 11), Sep(), I(Tr(L"Copiar Texto"), 12) };
    } else {
        Item mv; mv.label = Tr(L"Mover para"); mv.id = 0;
        auto add = [&](const std::wstring& id) { ids.push_back(id); mv.sub.push_back(I(store::FolderName(id).c_str(), 100 + (int)ids.size() - 1, id == n->folder)); };
        add(L"notas"); add(L"math");
        for (auto& f : store::folders) if (f.id != L"notas" && f.id != L"mac" && f.id != L"math") add(f.id);
        ids.push_back(L"mac"); mv.sub.push_back(I(Tr(L"Notas (No Meu Mac)"), 100 + (int)ids.size() - 1, n->folder == L"mac"));
        items = { I(n->pinned ? Tr(L"Desafixar Nota") : Tr(L"Fixar Nota"), 1), Sep(), I(Tr(L"Apagar"), 2, false, false, L"Delete"), I(Tr(L"Duplicar"), 3), mv, Sep(), I(Tr(L"Copiar Texto"), 12) };
    }
    int id = ctxmenu::ShowItems(g_hwnd, sp, items);
    if (id == 1) { n->pinned = !n->pinned; n->dirty = true; store::SaveNote(n); Refresh(); }
    else if (id == 2 || id == 11) DeleteNoteCmd(n);
    else if (id == 3) {
        Note* d = store::NewNote(n->folder);
        d->paras = n->paras;
        if (!d->paras.empty()) { d->paras[0].text += Tr(L" (cópia)"); d->paras[0].at.resize(d->paras[0].text.size(), d->paras[0].at.empty() ? 0 : d->paras[0].at.back()); }
        d->dirty = true; store::SaveNote(d);
        SelectNote(d);
    } else if (id == 10) RecoverNote(n);
    else if (id == 12) CopyText(n->PlainText());
    else if (id >= 100 && id - 100 < (int)ids.size()) { MoveNote(n, ids[id - 100]); BuildRows(); if (g_sel == n && std::find(g_visible.begin(), g_visible.end(), n) == g_visible.end()) SelectNote(g_visible.empty() ? nullptr : g_visible[0]); else Refresh(); }
    Invalidate();
}

void FolderMenu(const std::wstring& fid, POINT sp) {
    NFolder* f = store::FindFolder(fid == L"todas" ? L"notas" : fid);
    bool user = fid != L"notas" && fid != L"mac" && fid != L"math" && fid != L"rapidas" && fid != L"todas" && fid != L"apagadas";
    std::vector<Item> items = { I(Tr(L"Nova Pasta"), 1, false, false, L"Ctrl+Shift+N") };
    if (user) { items.push_back(I(Tr(L"Renomear Pasta"), 2)); items.push_back(I(Tr(L"Apagar Pasta"), 3)); }
    if (fid == L"apagadas") items.push_back(I(Tr(L"Apagar Tudo"), 7));
    if (f && fid != L"apagadas") {
        items.push_back(Sep());
        Item s; s.label = Tr(L"Ordenar Pasta Por");
        s.sub = { I(Tr(L"Data de Edição"), 4, false, f->sort == 0), I(Tr(L"Data de Criação"), 5, false, f->sort == 1), I(Tr(L"Título"), 6, false, f->sort == 2) };
        items.push_back(s);
        items.push_back(I(Tr(L"Agrupar por Data"), 8, f->sort == 2, f->group && f->sort != 2));
    }
    int id = ctxmenu::ShowItems(g_hwnd, sp, items);
    if (id == 1) NewFolderCmd();
    else if (id == 2) StartRename(fid);
    else if (id == 3) DeleteFolderCmd(fid);
    else if (id >= 4 && id <= 6 && f) { f->sort = id - 4; store::SaveFolders(); Refresh(); }
    else if (id == 8 && f) { f->group = !f->group; store::SaveFolders(); Refresh(); }
    else if (id == 7) {
        g_sheet = { true, Tr(L"Apagar todas as notas de Apagadas Recentemente?"), Tr(L"Esta ação não pode ser desfeita."), Tr(L"Apagar"), []() {
            std::vector<Note*> del;
            for (auto& n : store::notes) if (n->folder == L"apagadas") del.push_back(n.get());
            for (Note* n : del) { if (editor::Current() == n) editor::SetNote(nullptr); if (g_sel == n) g_sel = nullptr; store::RemoveNote(n); }
            SelectFolder(L"notas");
        } };
    }
    Invalidate();
}

void OpenSearch() {
    g_searchOpen = true;
    g_focus = F_SEARCH;
    editor::Focus(false);
    g_search.Select(0, (int)g_search.text.size());
    SetTimer(g_hwnd, T_BLINK, 530, nullptr);
    Invalidate();
}
void CloseSearch() {
    g_searchOpen = false;
    g_search.SetText(L"", false);
    editor::SetSearch(L"");
    if (g_focus == F_SEARCH) g_focus = F_LIST;
    Refresh();
}
void SearchChanged() {
    editor::SetSearch(g_search.text);
    editor::Invalidate();
    g_listScroll = 0;
    BuildRows();
    if (Searching() && !g_visible.empty() && std::find(g_visible.begin(), g_visible.end(), g_sel) == g_visible.end()) { g_sel = g_visible[0]; editor::SetNote(g_sel); editor::SetSearch(g_search.text); }
    Refresh();
}

void UnavailableMenu(int b) {
    auto r = g_btn[b].r;
    std::vector<Item> items;
    if (b == B_SHARE) items = { I(Tr(L"Copiar Nota"), 1), I(Tr(L"Copiar Título"), 2) };
    else if (b == B_LOCK) items = { I(Tr(L"Bloquear Nota"), 1, true) };
    else if (b == B_COLLAB) items = { I(Tr(L"Colaborar requer iCloud"), 1, true) };
    else if (b == B_TABLE) items = { I(Tr(L"Tabelas ainda não disponíveis"), 1, true) };
    else if (b == B_AUDIO) items = { I(Tr(L"Gravar Áudio indisponível"), 1, true) };
    else items = { I(Tr(L"Fotos e arquivos indisponíveis"), 1, true) };
    int id = ctxmenu::ShowItems(g_hwnd, ScreenPt(r.left, r.bottom + 4), items);
    if (b == B_SHARE && editor::Current()) {
        if (id == 1) CopyText(editor::Current()->PlainText());
        else if (id == 2) CopyText(editor::Current()->Title());
    }
}

void ToggleSidebar() { g_sideHidden = !g_sideHidden; if (g_gallery) BuildCards(); SaveSoon(); Invalidate(); }

void SetGallery(bool on) {
    g_gallery = on; g_galleryOpen = false; g_listScroll = 0;
    if (on) { BuildCards(); g_focus = F_LIST; editor::Focus(false); }
    SaveSoon(); Invalidate();
}

void ClickButton(int b) {
    switch (b) {
    case B_SIDEBAR: ToggleSidebar(); break;
    case B_LIST: SetGallery(false); break;
    case B_GALLERY: SetGallery(true); break;
    case B_TRASH: if (g_sel) DeleteNoteCmd(g_sel); break;
    case B_BACK: g_galleryOpen = false; editor::Focus(false); g_focus = F_LIST; BuildCards(); RevealSel(); break;
    case B_COMPOSE: NewNoteCmd(); break;
    case B_FORMAT: g_fmtOpen = !g_fmtOpen; if (g_fmtOpen) { g_focus = F_EDITOR; editor::Focus(true); } break;
    case B_CHECK: editor::ToggleList(L_CHECK); g_focus = F_EDITOR; editor::Focus(true); break;
    case B_SEARCH: OpenSearch(); break;
    case B_FOLDER: NewFolderCmd(); break;
    case B_MORE: {  // macOS 27: Lista / Galeria e apagar a nota
        auto r = g_btn[B_MORE].r;
        Item sep; sep.separator = true;
        std::vector<Item> items = { I(Tr(L"Ver como Lista"), 1, false, !g_gallery), I(Tr(L"Ver como Galeria"), 2, false, g_gallery), sep, I(Tr(L"Apagar Nota"), 3, !g_sel) };
        int id = ctxmenu::ShowItems(g_hwnd, ScreenPt(r.left, r.bottom + 4), items);
        if (id == 1) SetGallery(false); else if (id == 2) SetGallery(true); else if (id == 3 && g_sel) DeleteNoteCmd(g_sel);
        break;
    }
    default: UnavailableMenu(b); break;
    }
    Invalidate();
}

void FormatClick(D2D1_POINT_2F p) {
    for (auto& it : g_fmtItems) {
        if (!In(it.r, p)) continue;
        if (it.kind == 0) editor::ApplyStyle(it.v);
        else if (it.kind == 1) editor::ToggleAttr((uint8_t)it.v);
        else if (it.kind == 2) editor::ToggleList(it.v);
        else editor::SetHighlight(it.v);
        editor::Focus(true); g_focus = F_EDITOR;
        Invalidate();
        return;
    }
}

// ---------------------------------------------------------------------------
LRESULT HitTestNC(HWND h, LPARAM l) {
    POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
    ScreenToClient(h, &pt);
    RECT rc; GetClientRect(h, &rc);
    if (!IsZoomed(h)) {
        int b = (int)(6 * g_scale);
        bool L = pt.x < b, R = pt.x >= rc.right - b, T = pt.y < b, B = pt.y >= rc.bottom - b;
        if (T && L) return HTTOPLEFT; if (T && R) return HTTOPRIGHT; if (B && L) return HTBOTTOMLEFT; if (B && R) return HTBOTTOMRIGHT;
        if (L) return HTLEFT; if (R) return HTRIGHT; if (T) return HTTOP; if (B) return HTBOTTOM;
    }
    D2D1_POINT_2F p{ pt.x / g_scale, pt.y / g_scale };
    if (g_sheet.on || g_fmtOpen) return HTCLIENT;
    if (p.y < TOOLBAR_H && !OverLights(p) && HitBtn(p) < 0 && !(g_searchOpen && In(g_searchRect, p))) return HTCAPTION;
    return HTCLIENT;
}

int g_clicks; DWORD g_lastClick; POINT g_lastClickPt;

void UpdateIme() {
    if (!editor::Focused()) return;
    D2D1_RECT_F r; editor::CaretScreenRect(r);
    HIMC imc = ImmGetContext(g_hwnd);
    if (!imc) return;
    COMPOSITIONFORM cf{ CFS_POINT, { (LONG)(r.left * g_scale), (LONG)(r.top * g_scale) } };
    ImmSetCompositionWindow(imc, &cf);
    ImmReleaseContext(g_hwnd, imc);
}

void FocusEditor() { g_focus = F_EDITOR; editor::Focus(true); SetTimer(g_hwnd, T_BLINK, 530, nullptr); }

// Traz a janela para a frente. O Windows só deixa quem já está na frente dar o foco; quando o Notas
// abre enquanto o Launchpad ainda fecha (ou já estava aberto), o pedido simples é negado e a janela
// fica atrás. Ligando a fila de entrada à da janela da frente por um instante, o pedido passa.
void Bring() {
    if (!g_hwnd) return;
    if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
    HWND fg = GetForegroundWindow();
    if (fg == g_hwnd) return;
    DWORD ft = fg ? GetWindowThreadProcessId(fg, nullptr) : 0, me = GetCurrentThreadId();
    bool att = ft && ft != me && AttachThreadInput(me, ft, TRUE);
    BringWindowToTop(g_hwnd);
    SetForegroundWindow(g_hwnd);
    SetActiveWindow(g_hwnd);
    if (att) AttachThreadInput(me, ft, FALSE);
}

// na frente está o próprio Shell (Launchpad fechando, Dock, Spotlight) ou a mesa: pode tomar a frente
bool ShellInFront() {
    HWND fg = GetForegroundWindow();
    if (!fg) return true;
    wchar_t cls[64] = {}; GetClassNameW(fg, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd")) return true;
    DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
    HANDLE ph = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    bool r = false;
    if (ph) {
        wchar_t path[MAX_PATH]; DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(ph, 0, path, &n)) { const wchar_t* b = wcsrchr(path, L'\\'); r = b && (!_wcsicmp(b + 1, L"LiquinixShell.exe") || !_wcsicmp(b + 1, L"Shell.exe")); }
        CloseHandle(ph);
    }
    return r;
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_INITMENUPOPUP: case WM_DRAWITEM: case WM_MEASUREITEM: case WM_MENUCHAR: {
        LRESULT r = 0;
        if (ctxmenu::ForwardOwnerMsg(m, w, l, &r)) return r;
        break;
    }
    case WM_NCCALCSIZE:
        if (w) {
            if (IsZoomed(h)) {
                auto* p = (NCCALCSIZE_PARAMS*)l;
                UINT dpi = GetDpiForWindow(h);
                int fx = GetSystemMetricsForDpi(SM_CXFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                int fy = GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                p->rgrc[0].left += fx; p->rgrc[0].right -= fx; p->rgrc[0].top += fy; p->rgrc[0].bottom -= fy;
            }
            return 0;
        }
        break;
    case WM_NCHITTEST: return HitTestNC(h, l);
    case WM_NCLBUTTONDBLCLK:  // duplo clique na barra: a mesma animação do botão verde
        if (w == HTCAPTION) { ampliacao::Alternar(h); return 0; }
        break;
    case WM_GETMINMAXINFO: ((MINMAXINFO*)l)->ptMinTrackSize = g_quick ? POINT{ (LONG)(300 * g_scale), (LONG)(220 * g_scale) } : POINT{ (LONG)(640 * g_scale), (LONG)(400 * g_scale) }; return 0;
    case WM_SIZE:
        gfx::Resize(LOWORD(l), HIWORD(l));
        if (g_gallery) BuildCards();
        Paint();
        return 0;
    case WM_DPICHANGED: {
        g_dpi = HIWORD(w); g_scale = g_dpi / 96.f;
        RECT* r = (RECT*)l;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_WINDOWPOSCHANGED: shadow::Update(); break;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); Paint(); EndPaint(h, &ps); return 0; }
    case WM_ACTIVATE:
        g_active = LOWORD(w) != WA_INACTIVE;
        shadow::SetActive(g_active); shadow::Update();
        if (!g_active) { g_fmtOpen = false; store::SaveAllDirty(); }
        else if (!g_quick && store::Sync(editor::Current())) {
            BuildRows();
            if (g_sel && std::find(g_visible.begin(), g_visible.end(), g_sel) == g_visible.end()) {
                bool alive = false; for (auto& x : store::notes) if (x.get() == g_sel) alive = true;
                if (!alive) { g_sel = nullptr; editor::SetNote(nullptr); }
            }
            editor::Invalidate();
            Refresh();
        }
        Invalidate();
        return 0;
    case WM_SETTINGCHANGE:
        if (l && !wcscmp((const wchar_t*)l, L"ImmersiveColorSet")) {
            DWORD v = 1, cb = sizeof(v);
            RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &cb);
            g_dark = v == 0 && !semaforo::Leopard();
            BOOL dm = g_dark; DwmSetWindowAttribute(h, 20, &dm, sizeof(dm));
            COLORREF border = g_dark ? RGB(88, 88, 92) : RGB(196, 196, 200); DwmSetWindowAttribute(h, 34, &border, sizeof(border));
            editor::SetDark(g_dark);
            Invalidate();
        }
        return 0;
    case WM_APP_TRAZER: Bring(); return 0;
    case WM_TIMER:
        if (w == T_FRONT) {  // abriu pelo Launchpad: confere de novo depois da animação de fechar
            KillTimer(h, T_FRONT);
            if (GetForegroundWindow() != h && ShellInFront()) Bring();
            return 0;
        }
        if (w == T_BLINK) {
            if (editor::Focused()) editor::ToggleBlink();
            Invalidate();
        } else if (w == T_SAVE) {
            KillTimer(h, T_SAVE);
            store::SaveAllDirty();
            SaveState();
        }
        return 0;
    case WM_MOUSEWHEEL: {
        POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ScreenToClient(h, &pt);
        D2D1_POINT_2F p{ pt.x / g_scale, pt.y / g_scale };
        float dy = GET_WHEEL_DELTA_WPARAM(w) / 120.f * 48;
        if (g_sheet.on) return 0;
        if (EditorShown() && p.x >= EdL()) editor::OnWheel(dy);
        else if (p.x < SideW()) g_sideScroll = std::max(0.f, g_sideScroll - dy);
        else {
            float maxS = std::max(0.f, (g_gallery ? g_galleryH : g_rowsH) - ClientDip().height);
            g_listScroll = std::clamp(g_listScroll - dy, 0.f, maxS);
        }
        Invalidate();
        return 0;
    }
    case WM_MOUSEMOVE: {
        D2D1_POINT_2F p{ GET_X_LPARAM(l) / g_scale, GET_Y_LPARAM(l) / g_scale };
        TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t);
        if (g_divDrag) {
            if (g_divDrag == 1) g_sideW = std::round(std::clamp(p.x, SIDE_MIN, SIDE_MAX));
            else g_listW = std::round(std::clamp(p.x - SideW(), LIST_MIN, std::min(LIST_MAX, ClientDip().width - SideW() - EDITOR_MIN)));
            if (g_gallery) BuildCards();
            Invalidate();
            return 0;
        }
        if (g_dragNote && (w & MK_LBUTTON)) {
            POINT now{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
            if (!g_dragging && (std::abs(now.x - g_dragFrom.x) > 5 || std::abs(now.y - g_dragFrom.y) > 5)) g_dragging = true;
            if (g_dragging) {
                g_dragAt = p;
                int s = HitSide(p);
                g_dropFolder = s >= 0 && g_side[s].id != L"todas" && g_side[s].id != g_dragNote->folder ? g_side[s].id : L"";
                Invalidate();
            }
            return 0;
        }
        if (GetCapture() == h && editor::Focused() && (w & MK_LBUTTON)) { editor::OnMouseMove(p, true); return 0; }
        int hb = p.y < TOOLBAR_H ? HitBtn(p) : -1;
        bool hl = OverLights(p);
        ampliacao::Verde(h, hl && p.x >= 58 && p.x < 81);
        if (hb != g_hoverBtn || hl != g_hoverLights) { g_hoverBtn = hb; g_hoverLights = hl; Invalidate(); }
        if (EditorShown() && p.x >= EdL()) editor::OnMouseMove(p, false);
        // cursor: texto no editor, setas nas divisórias
        bool onDiv = p.y >= TOOLBAR_H && ((!g_sideHidden && std::fabs(p.x - SideW()) <= 4) || (!g_gallery && std::fabs(p.x - ListR()) <= 4));
        if (onDiv) SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
        else if (EditorShown() && editor::Current() && p.x >= EdL() + 4 && p.y >= TOOLBAR_H && !g_sheet.on && !(g_fmtOpen && In(g_fmtRect, p)) && !editor::OverChevronArea(p)) SetCursor(LoadCursorW(nullptr, IDC_IBEAM));
        else SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return 0;
    }
    case WM_SETCURSOR: if (LOWORD(l) == HTCLIENT) return TRUE; break;
    case WM_MOUSELEAVE: ampliacao::Verde(h, false); if (g_hoverBtn >= 0 || g_hoverLights) { g_hoverBtn = -1; g_hoverLights = false; Invalidate(); } return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        D2D1_POINT_2F p{ GET_X_LPARAM(l) / g_scale, GET_Y_LPARAM(l) / g_scale };
        POINT raw{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        DWORD now = GetTickCount();
        if (now - g_lastClick <= GetDoubleClickTime() && std::abs(raw.x - g_lastClickPt.x) < 5 && std::abs(raw.y - g_lastClickPt.y) < 5) g_clicks++;
        else g_clicks = 1;
        g_lastClick = now; g_lastClickPt = raw;
        if (g_sheet.on) {
            if (In(g_sheetOk, p)) { auto act = g_sheet.act; g_sheet = Sheet{}; if (act) act(); }
            else if (In(g_sheetCancel, p)) g_sheet = Sheet{};
            Invalidate();
            return 0;
        }
        if (g_fmtOpen) {
            if (In(g_fmtRect, p)) { FormatClick(p); return 0; }
            g_fmtOpen = false;
            if (HitBtn(p) == B_FORMAT) { Invalidate(); return 0; }
            Invalidate();
        }
        if (g_focus == F_RENAME && !(p.x < SideW() && HitSide(p) >= 0 && g_side[HitSide(p)].id == g_renameId)) CommitRename();
        for (int k = 0; k < 3; k++) {
            auto c = LightCenter(k);
            if (std::fabs(p.x - c.x) <= 8 && std::fabs(p.y - c.y) <= 8) {
                if (k == 0) PostMessageW(h, WM_CLOSE, 0, 0);
                else if (k == 1) ShowWindow(h, SW_MINIMIZE);
                else ampliacao::Alternar(h);
                return 0;
            }
        }
        if (g_searchOpen && In(g_searchRect, p)) {
            if (!g_search.text.empty() && p.x >= g_searchRect.right - 20) { g_search.SetText(L"", false); SearchChanged(); }
            else g_search.Click(g_f13, p, GetKeyState(VK_SHIFT) < 0);
            g_focus = F_SEARCH; editor::Focus(false);
            SetTimer(h, T_BLINK, 530, nullptr);
            Invalidate();
            return 0;
        }
        if (p.y < TOOLBAR_H) {
            int b = HitBtn(p);
            if (b >= 0 && g_btn[b].enabled) { g_pressBtn = b; SetCapture(h); Invalidate(); }
            return 0;
        }
        // divisórias
        if (!g_sideHidden && std::fabs(p.x - SideW()) <= 4) { g_divDrag = 1; SetCapture(h); return 0; }
        if (!g_gallery && std::fabs(p.x - ListR()) <= 4) { g_divDrag = 2; SetCapture(h); return 0; }
        // barra lateral
        if (p.x < SideW()) {
            if (!semaforo::Aqua() && In(NewFolderRect(), p)) { NewFolderCmd(); return 0; }
            int s = HitSide(p);
            if (s >= 0) {
                if (g_focus == F_RENAME && g_side[s].id == g_renameId) { g_rename.Click(g_f13, p, false); Invalidate(); return 0; }
                bool again = g_side[s].id == g_folder;
                if (!again || Searching()) SelectFolder(g_side[s].id);
                g_focus = F_SIDE; editor::Focus(false);
                if (g_clicks >= 2 || (again && g_clicks == 1 && false)) StartRename(g_side[s].id);
            }
            Invalidate();
            return 0;
        }
        // galeria
        if (g_gallery && !g_galleryOpen) {
            int c = HitCard(p);
            g_focus = F_LIST; editor::Focus(false);
            if (c >= 0) {
                SelectNote(g_cards[c].n, true);
                if (g_clicks >= 2) { g_galleryOpen = true; FocusEditor(); editor::PlaceCaretAtEnd(); }
                else { g_dragNote = g_cards[c].n; g_dragFrom = raw; g_dragging = false; SetCapture(h); }
            }
            Invalidate();
            return 0;
        }
        // lista
        if (!g_gallery && p.x < ListR()) {
            int r = HitRow(p);
            g_focus = F_LIST; editor::Focus(false);
            if (r >= 0) {
                if (g_rows[r].n != g_sel) SelectNote(g_rows[r].n, true);
                g_dragNote = g_rows[r].n; g_dragFrom = raw; g_dragging = false; SetCapture(h);
                if (g_clicks >= 2) { FocusEditor(); editor::PlaceCaretAtEnd(); }
            }
            Invalidate();
            return 0;
        }
        // editor
        if (EditorShown() && editor::Current()) {
            if (editor::Current()->folder == L"apagadas") {  // apagadas: só leitura, como no Mac (oferece recuperar)
                g_sheet = { true, Tr(L"Notas apagadas não podem ser editadas."), Tr(L"Para editar esta nota, recupere-a primeiro."), Tr(L"Recuperar"), [n = editor::Current()]() { RecoverNote(n); } };
                Invalidate();
                return 0;
            }
            FocusEditor();
            editor::OnMouseDown(p, GetKeyState(VK_SHIFT) < 0, g_clicks);
            SetCapture(h);
            UpdateIme();
            Invalidate();
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        D2D1_POINT_2F p{ GET_X_LPARAM(l) / g_scale, GET_Y_LPARAM(l) / g_scale };
        if (GetCapture() == h) ReleaseCapture();
        if (g_divDrag) { g_divDrag = 0; SaveSoon(); return 0; }
        if (g_pressBtn >= 0) { int b = g_pressBtn; g_pressBtn = -1; if (HitBtn(p) == b) ClickButton(b); Invalidate(); return 0; }
        if (g_dragNote) {
            if (g_dragging && !g_dropFolder.empty()) {
                Note* n = g_dragNote;
                if (g_dropFolder == L"apagadas") DeleteNoteCmd(n);
                else { MoveNote(n, g_dropFolder); BuildRows(); if (std::find(g_visible.begin(), g_visible.end(), n) == g_visible.end()) { g_sel = nullptr; SelectNote(g_visible.empty() ? nullptr : g_visible[0]); } }
            }
            g_dragNote = nullptr; g_dragging = false; g_dropFolder.clear();
            Refresh();
        }
        return 0;
    }
    case WM_RBUTTONUP: {
        D2D1_POINT_2F p{ GET_X_LPARAM(l) / g_scale, GET_Y_LPARAM(l) / g_scale };
        POINT sp{ GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ClientToScreen(h, &sp);
        if (g_sheet.on) return 0;
        g_fmtOpen = false;
        if (p.x < SideW() && p.y >= TOOLBAR_H) {
            int s = HitSide(p);
            if (s >= 0) FolderMenu(g_side[s].id, sp);
            else { int id = ctxmenu::ShowItems(h, sp, { I(Tr(L"Nova Pasta"), 1, false, false, L"Ctrl+Shift+N") }); if (id == 1) NewFolderCmd(); }
            return 0;
        }
        if (g_gallery && !g_galleryOpen) { int c = HitCard(p); if (c >= 0) { SelectNote(g_cards[c].n, true); NoteMenu(g_cards[c].n, sp); } return 0; }
        if (!g_gallery && p.x < ListR() && p.y >= TOOLBAR_H) { int r = HitRow(p); if (r >= 0) { SelectNote(g_rows[r].n, true); NoteMenu(g_rows[r].n, sp); } return 0; }
        if (EditorShown() && p.x >= EdL() && editor::Current()) {
            bool s = editor::HasSelection();
            int id = ctxmenu::ShowItems(h, sp, { I(Tr(L"Recortar"), 1, !s, false, L"Ctrl+X"), I(Tr(L"Copiar"), 2, !s, false, L"Ctrl+C"), I(Tr(L"Colar"), 3, false, false, L"Ctrl+V"), Sep(),
                                                 I(Tr(L"Negrito"), 4, false, false, L"Ctrl+B"), I(Tr(L"Itálico"), 5, false, false, L"Ctrl+I"), I(Tr(L"Sublinhado"), 6, false, false, L"Ctrl+U"), Sep(),
                                                 I(Tr(L"Selecionar Tudo"), 7, false, false, L"Ctrl+A") });
            FocusEditor();
            if (id == 1) editor::Copy(true); else if (id == 2) editor::Copy(false); else if (id == 3) editor::Paste();
            else if (id == 4) editor::ToggleAttr(A_BOLD); else if (id == 5) editor::ToggleAttr(A_ITALIC); else if (id == 6) editor::ToggleAttr(A_UNDER);
            else if (id == 7) editor::SelectAll();
            Invalidate();
        }
        return 0;
    }
    case WM_KEYDOWN: case WM_SYSKEYDOWN: {
        bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
        if (g_sheet.on) {
            if (w == VK_ESCAPE) g_sheet = Sheet{};
            else if (w == VK_RETURN) { auto act = g_sheet.act; g_sheet = Sheet{}; if (act) act(); }
            Invalidate();
            return 0;
        }
        if (m == WM_SYSKEYDOWN && !(alt && ctrl)) break;
        // atalhos do app
        if (ctrl && !shift && w == 'N') { NewNoteCmd(); return 0; }
        if (ctrl && shift && w == 'N') { NewFolderCmd(); return 0; }
        if (ctrl && w == 'F') { OpenSearch(); return 0; }
        if (ctrl && w == 'W') { PostMessageW(h, WM_CLOSE, 0, 0); return 0; }
        if (g_quick && (ctrl && (w == '1' || w == '2' || w == 'S'))) return 0;
        if (ctrl && alt && w == 'S') { ToggleSidebar(); return 0; }
        if (ctrl && !shift && w == '1') { SetGallery(false); return 0; }
        if (ctrl && !shift && w == '2') { SetGallery(true); return 0; }
        if (ctrl && shift && w == 'P' && g_sel) { g_sel->pinned = !g_sel->pinned; g_sel->dirty = true; store::SaveNote(g_sel); Refresh(); return 0; }
        if (g_fmtOpen && w == VK_ESCAPE) { g_fmtOpen = false; Invalidate(); return 0; }
        if (g_focus == F_RENAME) {
            if (w == VK_RETURN) CommitRename();
            else if (w == VK_ESCAPE) { g_focus = F_SIDE; Invalidate(); }
            else g_rename.OnKey((UINT)w, shift, ctrl);
            Invalidate();
            return 0;
        }
        if (g_focus == F_SEARCH) {
            if (w == VK_ESCAPE) { CloseSearch(); return 0; }
            if (w == VK_RETURN || w == VK_DOWN) { g_focus = F_LIST; Invalidate(); return 0; }
            std::wstring before = g_search.text;
            g_search.OnKey((UINT)w, shift, ctrl);
            if (before != g_search.text) SearchChanged();
            Invalidate();
            return 0;
        }
        if (g_focus == F_EDITOR) {
            if (w == VK_ESCAPE && !editor::HasSelection()) {
                if (g_quick) return 0;
                if (g_gallery && g_galleryOpen) { ClickButton(B_BACK); return 0; }
                g_focus = F_LIST; editor::Focus(false); Invalidate(); return 0;
            }
            if (editor::OnKey((UINT)w)) { UpdateIme(); Invalidate(); return 0; }
            return 0;
        }
        // lista / barra lateral
        if (g_focus == F_SIDE && (w == VK_UP || w == VK_DOWN)) {
            int cur = -1;
            for (int i = 0; i < (int)g_side.size(); i++) if (!g_side[i].header && g_side[i].id == g_folder) cur = i;
            for (int i = cur + (w == VK_DOWN ? 1 : -1); i >= 0 && i < (int)g_side.size(); i += (w == VK_DOWN ? 1 : -1))
                if (!g_side[i].header) { SelectFolder(g_side[i].id); g_focus = F_SIDE; break; }
            return 0;
        }
        if (g_focus == F_SIDE && (w == VK_RIGHT || w == VK_TAB)) { g_focus = F_LIST; Invalidate(); return 0; }
        if (g_focus == F_LIST) {
            int cur = (int)(std::find(g_visible.begin(), g_visible.end(), g_sel) - g_visible.begin());
            int n = (int)g_visible.size();
            int step = 1;
            if (g_gallery && !g_galleryOpen && (w == VK_UP || w == VK_DOWN)) {
                float x0 = SideW() + 24, wd = ClientDip().width - x0 - 12;
                step = std::max(1, (int)((wd + CARD_GAP) / (CARD_W + CARD_GAP)));
            }
            bool horiz = g_gallery && (w == VK_LEFT || w == VK_RIGHT);
            if ((w == VK_DOWN || w == VK_UP || horiz) && n) {
                int d = (w == VK_DOWN || w == VK_RIGHT) ? (horiz ? 1 : step) : -(horiz ? 1 : step);
                int k = cur >= n ? 0 : std::clamp(cur + d, 0, n - 1);
                SelectNote(g_visible[k]);
                return 0;
            }
            if (w == VK_LEFT && !g_gallery) { g_focus = F_SIDE; Invalidate(); return 0; }
            if ((w == VK_RETURN || w == VK_TAB || w == VK_RIGHT) && g_sel) {
                if (g_gallery) g_galleryOpen = true;
                if (g_sel->folder != L"apagadas") { FocusEditor(); if (w != VK_TAB) editor::PlaceCaretAtEnd(); }
                Invalidate(); return 0;
            }
            if ((w == VK_DELETE || w == VK_BACK) && g_sel) { DeleteNoteCmd(g_sel); return 0; }
        }
        return 0;
    }
    case WM_CHAR:
        if (g_sheet.on) return 0;
        if (g_focus == F_RENAME) { if (w >= 32) g_rename.OnChar((wchar_t)w); Invalidate(); return 0; }
        if (g_focus == F_SEARCH) { if (w >= 32 && w != 127) { g_search.OnChar((wchar_t)w); SearchChanged(); } Invalidate(); return 0; }
        if (g_focus == F_EDITOR) {
            if (GetKeyState(VK_CONTROL) < 0 && w < 32) return 0;
            editor::OnChar((wchar_t)w); UpdateIme(); Invalidate();
            return 0;
        }
        if (g_focus == F_LIST && w >= 32 && g_sel && g_sel->folder != L"apagadas" && GetKeyState(VK_CONTROL) >= 0) {  // digitar na lista vai para a nota
            if (g_gallery) g_galleryOpen = true;
            FocusEditor(); editor::PlaceCaretAtEnd(); editor::OnChar((wchar_t)w); Invalidate();
        }
        return 0;
    case WM_CLOSE:
        if (g_focus == F_RENAME) CommitRename();
        if (g_sel && g_sel->Empty()) { Note* n = g_sel; g_sel = nullptr; editor::SetNote(nullptr); store::RemoveNote(n); }
        store::SaveAllDirty();
        SaveState();
        shadow::Destroy();
        DestroyWindow(h);
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

void NoteEdited(Note* n) {
    (void)n;
    BuildSide(); BuildRows();
    if (g_gallery) BuildCards();
    SaveSoon();
    Invalidate();
}

// ---------------------------------------------------------------------------
namespace {

std::wstring PickFamily(std::initializer_list<const wchar_t*> names) {
    IDWriteFontCollection* fc = nullptr;
    gfx::dw->GetSystemFontCollection(&fc);
    std::wstring r = L"Segoe UI";
    for (auto n : names) {
        UINT32 i; BOOL ok = FALSE;
        if (fc) fc->FindFamilyName(n, &i, &ok);
        if (ok) { r = n; break; }
    }
    if (fc) fc->Release();
    return r;
}

IDWriteTextFormat* Fmt(float size, DWRITE_FONT_WEIGHT w, DWRITE_TEXT_ALIGNMENT a = DWRITE_TEXT_ALIGNMENT_LEADING, const std::wstring& fam = L"") {
    IDWriteTextFormat* tf = nullptr;
    if (FAILED(gfx::dw->CreateTextFormat((fam.empty() ? g_family : fam).c_str(), nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"pt-br", &tf))) return nullptr;
    tf->SetTextAlignment(a);
    tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    tf->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TRIMMING tr{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
    IDWriteInlineObject* el = nullptr; gfx::dw->CreateEllipsisTrimmingSign(tf, &el); tf->SetTrimming(&tr, el); if (el) el->Release();
    return tf;
}

void CreateFormats() {
    g_family = PickFamily({ L"SF Pro Text", L"SF Pro", L"Inter", L"Segoe UI Variable Text", L"Segoe UI" });
    g_mono = PickFamily({ L"SF Mono", L"Menlo", L"Cascadia Mono", L"Consolas" });
    auto N = DWRITE_FONT_WEIGHT_NORMAL, B = DWRITE_FONT_WEIGHT_BOLD;
    g_f13b = Fmt(13, B); g_f13 = Fmt(13, N); g_f12 = Fmt(12, N); g_f12b = Fmt(12, B); g_f11 = Fmt(11, N); g_f11b = Fmt(11, B);
    g_f13c = Fmt(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_fCount = Fmt(12, N, DWRITE_TEXT_ALIGNMENT_TRAILING);
    g_fCardT = Fmt(12, B, DWRITE_TEXT_ALIGNMENT_CENTER); g_fCardD = Fmt(11, N, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_fThumbT = Fmt(9, B); g_fThumb = Fmt(7.5f, N);
    if (g_fThumbT) g_fThumbT->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    if (g_fThumb) g_fThumb->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    g_fFmt[S_TITLE] = Fmt(19, B); g_fFmt[S_HEADING] = Fmt(15.5f, B); g_fFmt[S_SUBHEADING] = Fmt(13, B); g_fFmt[S_BODY] = Fmt(13, N); g_fFmt[S_MONO] = Fmt(12, N, DWRITE_TEXT_ALIGNMENT_LEADING, g_mono);
    editor::SetFormats(semaforo::Leopard() ? std::wstring(L"Segoe Print") : g_family, g_mono);  // Leopard: a letra manuscrita das notas (Marker Felt no Mac)
}

HICON PackIcon(const std::wstring& png, int px) {
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return nullptr;
    HICON ic = nullptr;
    IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICBitmapScaler* scl = nullptr; IWICFormatConverter* cv = nullptr;
    if (SUCCEEDED(wic->CreateDecoderFromFilename(png.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr)) &&
        SUCCEEDED(wic->CreateBitmapScaler(&scl)) && SUCCEEDED(scl->Initialize(fr, px, px, WICBitmapInterpolationModeFant)) &&
        SUCCEEDED(wic->CreateFormatConverter(&cv)) && SUCCEEDED(cv->Initialize(scl, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
        std::vector<BYTE> bits((size_t)px * px * 4);
        if (SUCCEEDED(cv->CopyPixels(nullptr, px * 4, (UINT)bits.size(), bits.data()))) {
            BITMAPV5HEADER bh{}; bh.bV5Size = sizeof(bh); bh.bV5Width = px; bh.bV5Height = -px; bh.bV5Planes = 1; bh.bV5BitCount = 32; bh.bV5Compression = BI_BITFIELDS;
            bh.bV5RedMask = 0x00FF0000; bh.bV5GreenMask = 0x0000FF00; bh.bV5BlueMask = 0x000000FF; bh.bV5AlphaMask = 0xFF000000;
            void* p = nullptr; HDC dc = GetDC(nullptr);
            HBITMAP color = CreateDIBSection(dc, (BITMAPINFO*)&bh, DIB_RGB_COLORS, &p, nullptr, 0);
            ReleaseDC(nullptr, dc);
            if (color && p) {
                memcpy(p, bits.data(), bits.size());
                HBITMAP mask = CreateBitmap(px, px, 1, 1, nullptr);
                ICONINFO ii{ TRUE, 0, 0, mask, color };
                ic = CreateIconIndirect(&ii);
                DeleteObject(mask);
            }
            if (color) DeleteObject(color);
        }
    }
    if (cv) cv->Release(); if (scl) scl->Release(); if (fr) fr->Release(); if (dec) dec->Release();
    wic->Release();
    return ic;
}

void LoadState(RECT& r, bool& maxed) {
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_statePath.c_str(), L"r, ccs=UTF-8") || !f) return;
    wchar_t line[1024];
    std::wstring noteId;
    while (fgetws(line, 1024, f)) {
        std::wstring s = line;
        while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
        size_t eq = s.find(L'='); if (eq == std::wstring::npos) continue;
        std::wstring k = s.substr(0, eq), v = s.substr(eq + 1);
        if (k == L"janela") { int mx = 0; if (swscanf_s(v.c_str(), L"%ld %ld %ld %ld %d", &r.left, &r.top, &r.right, &r.bottom, &mx) >= 4) maxed = mx != 0; }
        else if (k == L"lateral") { int hid = 0; swscanf_s(v.c_str(), L"%g %d", &g_sideW, &hid); g_sideW = std::clamp(g_sideW, SIDE_MIN, SIDE_MAX); g_sideHidden = hid != 0; }
        else if (k == L"lista") g_listW = std::clamp((float)_wtof(v.c_str()), LIST_MIN, LIST_MAX);
        else if (k == L"galeria") g_gallery = v == L"1";
        else if (k == L"pasta") g_folder = v;
        else if (k == L"nota") noteId = v;
    }
    fclose(f);
    if (g_folder != L"todas" && g_folder != L"apagadas" && !store::FindFolder(g_folder)) g_folder = L"notas";
    for (auto& n : store::notes) if (n->id == noteId) g_sel = n.get();
}

// modo captura: desenha um quadro num PNG (para comparar com as referências sem mexer na tela)
int Capture(const std::wstring& out, int noteIdx, bool aa) {
    ID2D1Factory1* f = nullptr;
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr, (void**)&f);
    IWICImagingFactory* wic = nullptr;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (!f || !wic) return 1;
    float sc = 2;
    UINT W = (UINT)(g_capSize.width * sc), H = (UINT)(g_capSize.height * sc);
    IWICBitmap* bmp = nullptr;
    wic->CreateBitmap(W, H, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp);
    ID2D1RenderTarget* rt = nullptr;
    f->CreateWicBitmapRenderTarget(bmp, D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96 * sc, 96 * sc), &rt);
    ID2D1DeviceContext* dc = nullptr;
    if (rt) rt->QueryInterface(&dc);
    if (!dc) { Log(L"captura: sem device context"); return 1; }
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    BuildSide(); BuildRows();
    if (noteIdx >= 0 && noteIdx < (int)g_visible.size()) g_sel = g_visible[noteIdx];
    if (!g_sel && !g_visible.empty()) g_sel = g_visible[0];
    editor::SetNote(g_sel);
    if (g_gallery) BuildCards();
    if (aa) { g_fmtOpen = true; g_focus = F_EDITOR; }
    else g_focus = F_EDITOR;
    editor::Focus(true);
    if (g_sel) editor::PlaceCaretAtEnd();
    dc->BeginDraw();
    // "papel de parede" + o acrílico da barra lateral (simulado)
    ID2D1GradientStopCollection* stops = nullptr;
    D2D1_GRADIENT_STOP gs[3] = { { 0, g_dark ? D2D1::ColorF(0.20f, 0.24f, 0.30f) : D2D1::ColorF(0.96f, 0.68f, 0.52f) }, { 0.5f, g_dark ? D2D1::ColorF(0.28f, 0.22f, 0.32f) : D2D1::ColorF(0.78f, 0.62f, 0.86f) }, { 1, g_dark ? D2D1::ColorF(0.12f, 0.18f, 0.30f) : D2D1::ColorF(0.48f, 0.62f, 0.95f) } };
    dc->CreateGradientStopCollection(gs, 3, &stops);
    ID2D1LinearGradientBrush* lg = nullptr;
    dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, 0 }, { g_capSize.width, g_capSize.height }), stops, &lg);
    dc->FillRectangle(D2D1::RectF(0, 0, g_capSize.width, g_capSize.height), lg);
    ID2D1SolidColorBrush* tint = nullptr;
    dc->CreateSolidColorBrush(g_dark ? D2D1::ColorF(0.16f, 0.16f, 0.17f, 0.80f) : D2D1::ColorF(0.95f, 0.95f, 0.96f, 0.78f), &tint);
    dc->FillRectangle(D2D1::RectF(0, 0, g_capSize.width, g_capSize.height), tint);
    Render(dc);
    HRESULT hr = dc->EndDraw();
    tint->Release(); lg->Release(); stops->Release();
    // PNG
    IWICStream* st = nullptr; IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* fe = nullptr;
    wic->CreateStream(&st);
    st->InitializeFromFilename(out.c_str(), GENERIC_WRITE);
    wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    enc->Initialize(st, WICBitmapEncoderNoCache);
    enc->CreateNewFrame(&fe, nullptr);
    fe->Initialize(nullptr);
    fe->SetSize(W, H);
    WICPixelFormatGUID pf = GUID_WICPixelFormat32bppPBGRA;
    fe->SetPixelFormat(&pf);
    fe->WriteSource(bmp, nullptr);
    fe->Commit(); enc->Commit();
    fe->Release(); enc->Release(); st->Release();
    Log(L"captura %ls: 0x%08X", out.c_str(), (unsigned)hr);
    dc->Release(); rt->Release(); bmp->Release(); wic->Release(); f->Release();
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    OleInitialize(nullptr);
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    PathRemoveFileSpecW(exe);
    std::wstring bin = exe;
    simbolo::Dir() = bin.substr(0, bin.find_last_of(L'\\')) + L"\\icones\\simbolos";

    std::wstring capOut; int capNote = -1; bool capAa = false; int forceDark = -1;
    int argc; wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int a = 1; a < argc; a++) {
        std::wstring s = argv[a];
        auto val = [&](const wchar_t* k) { size_t n = wcslen(k); return s.compare(0, n, k) == 0 ? s.substr(n) : std::wstring(L"\x01"); };
        if (auto v = val(L"--dados="); v != L"\x01") g_dataDirOverride = v;
        else if (auto v2 = val(L"--captura="); v2 != L"\x01") capOut = v2;
        else if (s == L"--escuro") forceDark = 1;
        else if (s == L"--claro") forceDark = 0;
        else if (auto v3 = val(L"--tamanho="); v3 != L"\x01") swscanf_s(v3.c_str(), L"%fx%f", &g_capSize.width, &g_capSize.height);
        else if (auto v4 = val(L"--pasta="); v4 != L"\x01") g_folder = v4;
        else if (auto v5 = val(L"--nota="); v5 != L"\x01") capNote = _wtoi(v5.c_str());
        else if (s == L"--galeria") g_gallery = true;
        else if (s == L"--aa") capAa = true;
        else if (s == L"--sem-lateral") g_sideHidden = true;
        else if (s == L"--nota-rapida") g_quick = true;
        else if (auto v6 = val(L"--busca="); v6 != L"\x01") { g_searchOpen = true; g_search.SetText(v6, false); }
    }
    LocalFree(argv);
    g_capture = !capOut.empty();

    HANDLE one = nullptr;
    if (!g_capture) {
        one = CreateMutexW(nullptr, TRUE, g_quick ? L"NotasRapidaUmaJanela" : L"NotasMacUmaJanela");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            if (HWND w = FindWindowW(g_quick ? L"NotasRapidaJanela" : L"NotasJanela", nullptr)) {
                // esta cópia recebeu do Launchpad/Spotlight o direito de ir para a frente: passa para a janela aberta
                DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
                AllowSetForegroundWindow(pid);
                if (IsIconic(w)) ShowWindow(w, SW_RESTORE);
                SetForegroundWindow(w);
                PostMessageW(w, WM_APP + 7, 0, 0);
            }
            return 0;
        }
    }
    g_log = _wfsopen((bin + (g_capture ? L"\\notas-captura.log" : g_quick ? L"\\notas-rapida.log" : L"\\notas.log")).c_str(), L"w, ccs=UTF-8", _SH_DENYNO);
    ctxmenu::Init(hi, g_log);
    DWORD v = 1, cb = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &cb);
    g_dark = forceDark >= 0 ? forceDark == 1 : v == 0;
    if (semaforo::Leopard()) g_dark = false;  // o 10.5 não tinha modo escuro
    editor::SetDark(g_dark);
    store::Load();
    if (!g_capture && !g_quick) {
        PWSTR p = nullptr; std::wstring base;
        if (g_dataDirOverride.empty() && SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) { base = std::wstring(p) + L"\\Shell\\Notas"; CoTaskMemFree(p); }
        else base = g_dataDirOverride;
        g_statePath = base + L"\\estado.txt";
    }

    if (g_capture) {
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&gfx::dw);
        if (g_capSize.width < 100) g_capSize = { 1100, 680 };
        CreateFormats();
        if (g_searchOpen) editor::SetSearch(g_search.text);
        int r = Capture(capOut, capNote, capAa);
        if (g_log) fclose(g_log);
        return r;
    }

    RECT wr{ 0, 0, 0, 0 }; bool maxed = false;
    if (g_quick) { g_gallery = g_galleryOpen = g_sideHidden = true; g_folder = L"rapidas"; }
    else LoadState(wr, maxed);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_DBLCLKS; wc.lpfnWndProc = WndProc; wc.hInstance = hi; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = g_quick ? L"NotasRapidaJanela" : L"NotasJanela";
    std::wstring png = bin.substr(0, bin.find_last_of(L'\\')) + L"\\icones\\sequoia\\Notes.png";
    wc.hIcon = PackIcon(png, 48); wc.hIconSm = PackIcon(png, 16);
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    POINT cur; GetCursorPos(&cur);
    HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(mon, &mi);
    UINT dx = 96, dy = 96; GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    float sc = dx / 96.f;
    int w = (int)(1100 * sc), hgt = (int)(680 * sc);
    int x = (mi.rcWork.left + mi.rcWork.right - w) / 2, y = (mi.rcWork.top + mi.rcWork.bottom - hgt) / 2;
    if (g_quick) {  // Nota Rápida: janelinha no canto de baixo à direita, de onde veio o clique
        MONITORINFO pm{ sizeof(pm) }; GetMonitorInfoW(mon, &pm);
        w = (int)(400 * sc); hgt = (int)(360 * sc);
        x = pm.rcWork.right - w - (int)(24 * sc); y = pm.rcWork.bottom - hgt - (int)(24 * sc);
    } else if (wr.right > wr.left + 200 && wr.bottom > wr.top + 200) {
        HMONITOR m2 = MonitorFromRect(&wr, MONITOR_DEFAULTTONULL);
        if (m2) { x = wr.left; y = wr.top; w = wr.right - wr.left; hgt = wr.bottom - wr.top; }
    }
    g_hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | (g_quick ? WS_EX_TOPMOST : 0), wc.lpszClassName, g_quick ? TrPick(L"Nota Rápida", L"Quick Note") : TrPick(L"Notas", L"Notes"), WS_OVERLAPPEDWINDOW, x, y, w, hgt, nullptr, nullptr, hi, nullptr);
    ampliacao::Janela(g_hwnd);  // abrir e fechar animados (o Shell anima)
    g_dpi = (float)GetDpiForWindow(g_hwnd); g_scale = g_dpi / 96.f;
    MARGINS margins{ -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(g_hwnd, &margins);
    BOOL dm = g_dark; DwmSetWindowAttribute(g_hwnd, 20, &dm, sizeof(dm));
    COLORREF border = g_dark ? RGB(88, 88, 92) : RGB(196, 196, 200); DwmSetWindowAttribute(g_hwnd, 34, &border, sizeof(border));
    int backdrop = 3;  // DWMSBT_TRANSIENTWINDOW: acrílico (a barra lateral é transparente)
    DwmSetWindowAttribute(g_hwnd, 38, &backdrop, sizeof(backdrop));
    SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (!gfx::Init(g_hwnd)) { MessageBoxW(nullptr, Tr(L"Não foi possível iniciar o Direct2D. Veja notas.log."), Tr(L"Notas"), MB_ICONERROR); return 1; }
    CreateFormats();
    shadow::Create(g_hwnd);

    // primeira nota da pasta (ou a última aberta)
    BuildSide(); BuildRows();
    if (!g_sel || std::find(g_visible.begin(), g_visible.end(), g_sel) == g_visible.end()) g_sel = g_visible.empty() ? nullptr : g_visible[0];
    editor::SetNote(g_sel);
    if (g_gallery) BuildCards();
    RevealSel();
    g_focus = F_LIST;
    if (g_quick) {  // nota nova em Notas Rápidas, já com o cursor
        g_sel = store::NewNote(L"rapidas");
        editor::SetNote(g_sel);
        g_focus = F_EDITOR; editor::Focus(true); editor::PlaceCaretAtEnd();
    }
    ShowWindow(g_hwnd, maxed ? SW_SHOWMAXIMIZED : (show == SW_SHOWMINIMIZED ? SW_SHOWMINIMIZED : SW_SHOWNORMAL));
    if (show != SW_SHOWMINIMIZED && show != SW_SHOWMINNOACTIVE) { Bring(); SetTimer(g_hwnd, T_FRONT, 450, nullptr); }
    SetTimer(g_hwnd, T_BLINK, 530, nullptr);
    Log(L"notas aberto: fonte %ls / %ls, %zu notas", g_family.c_str(), g_mono.c_str(), store::notes.size());

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    store::SaveAllDirty();
    if (g_log) fclose(g_log);
    if (one) ReleaseMutex(one);
    OleUninitialize();
    return 0;
}
