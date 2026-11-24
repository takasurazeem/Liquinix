// Arquivos — gerenciador de arquivos no estilo do Finder (shell próprio, fase 4).
// Primeira entrega: visão de ícones, barra lateral translúcida, menu de contexto próprio,
// arrastar e soltar, renomear no lugar. Roda ao lado do Explorer; não registra nada.
//
//   Arquivos.exe [pasta]

#include "finder.h"
#include <d2d1effects.h>  // Leopard: a perspectiva 3D do Cover Flow
#pragma comment(lib, "dxguid.lib")
#include <windowsx.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <map>
#include <wincodec.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cwctype>
#include <share.h>
#include <sddl.h>
#include <unordered_map>
#include <unordered_set>
#include <atomic>
#include <thread>
#include <propkey.h>
#include <propsys.h>
#include <wincrypt.h>
#include <memory>
#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "crypt32.lib")  // base64 do comando do Comprimir
#undef KIND_FOLDER    // propkey.h define essas como texto; colidem com thumbs::Kind
#undef KIND_DOCUMENT
#undef KIND_IMAGE
#include "../menu/context_menu.h"
#include "../comum/ampliacao.h"  // botão verde com a animação do ampliar do Shell
#include "../comum/idioma.h"
#include "../comum/vidro.h"
#include "pastacor.h"  // Personalizar Pasta: cor e símbolo
#include "../comum/abrircom.h"  // áudio abre no Música
#include "../comum/semaforo.h"
#include "../comum/leopard.h"  // tema Mac OS X Leopard: barra metálica unificada

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

static FILE* g_log;

void Log(const wchar_t* fmt, ...) {
    if (!g_log) return;
    va_list a; va_start(a, fmt); vfwprintf(g_log, fmt, a); va_end(a);
    fputwc(L'\n', g_log); fflush(g_log);
}

// ---------------------------------------------------------------------------
// Medidas (DIPs) e mensagens

namespace Lay {
inline float SIDEBAR_W = 142;  // padrão do vídeo do Sequoia (166 px); arrastando a borda vai de 120 a 360
const float SIDEBAR_MIN = 120, SIDEBAR_MAX = 360;
inline float TOOLBAR_H = 53;  // Tahoe: 52 (ApplyWindowTheme; guia macOS26-Theme-Reference, seção 42)
const float PATH_H = 26;
inline float TOOL_CY = 26.3f;  // centro dos botões de cima (vídeo do Sequoia: 30 px de 61 na barra de 52 pt); Leopard: 38 (ApplyWindowTheme)
inline float STATUS_H = 26, FOOTER_H = 26;  // maiores no modo escolher (barra de Abrir/Salvar); + PATH_H com a barra de caminho
// medidas do Finder no vídeo do Sequoia (1 pt = 1,17 px): grade de 102 × 120,5 com ícones de 64,
// alinhada à esquerda com 15,5 dos lados; barra lateral com linhas de 28, texto em 40, ícone em 24,7
const float GRID_PAD = 20, GRID_PAD_X = 15.5f, LABEL_H = 32;
// linhas e cabeçalhos mudam com o tema (ApplyWindowTheme): Tahoe = guia macOS26-Theme-Reference, seção 42
// (Finder do macOS 26 medido com Acessibilidade): linhas de 32, cabeçalhos de 19 com 13 de espaço antes
inline float SIDE_ROW_H = 28.7f, SIDE_HDR_H = 23.5f;
inline float SIDE_TOP = 47;  // Leopard: abaixo da barra metálica (ApplyWindowTheme)
const float SEARCH_W = 190, BTN = 28;
const float ICON_MIN = 32, ICON_MAX = 160, SLIDER_W = 70;  // só o trilho, como no Finder
}

// Tamanho dos ícones (controle deslizante na barra de baixo, como no Finder).
static float g_icon = 64;
static float Icon() { return g_icon; }
static float CellMinW() { return std::max(96.f, g_icon + 38); }
static float CellH() { return g_icon + 56.5f; }
static float LabelW() { return std::max(98.f, g_icon + 34); }

enum : UINT { WM_APP_THUMB = WM_APP + 1, WM_APP_CHANGE = WM_APP + 2, WM_APP_SEARCH = WM_APP + 3, WM_APP_MEDIA = WM_APP + 4, WM_APP_WARMUP = WM_APP + 5, WM_APP_INFO = WM_APP + 6 };
enum : UINT_PTR { T_SCROLL = 1, T_RELOAD = 2, T_CARET = 3, T_AUTOSCROLL = 4, T_SCROLLBAR = 5, T_SEARCH = 7, T_MEDIA = 8, T_WARMUP = 9, T_SLOWRENAME = 10, T_TAHOE = 11, T_FRENTE = 12 };
// Logo depois de abrir, quem pediu às vezes toma a frente de volta (o painel de downloads do Chrome se fecha e
// reativa a janela dele): por 3 s, se o usuário não clicou nem apertou tecla, o Finder volta para a frente.
static DWORD g_frenteAte, g_frenteInput;

// Botões da barra de ferramentas (da esquerda para a direita).
enum Tool {
    TB_BACK, TB_FWD,
    TB_V_ICONS, TB_V_LIST, TB_V_COLS, TB_V_GALLERY,  // seletor de visão (vira um botão só em janela estreita)
    TB_GROUP, TB_SHARE, TB_TRASH, TB_TAG, TB_ACTION, TB_SEARCH, TB_COUNT
};

// ---------------------------------------------------------------------------
// Estado

struct Entry {
    std::wstring name, path;
    std::wstring label;   // o que aparece: atalhos sem ".lnk", como no Windows
    bool dir = false;
    wchar_t emblem = 0;   // símbolo gravado na pasta (Mesa, Documentos...)
    ULONGLONG mtime = 0;
    ULONGLONG size = 0;
    ULONGLONG ctime = 0;  // data de criação
    unsigned tags = 0;    // etiquetas (bit n = cor n)
    bool sel = false;
    std::wstring origin;  // Lixeira: pasta de onde o item saiu
    std::wstring Key() const { return path + L"|" + std::to_wstring(mtime); }  // muda se o arquivo mudar
};

struct SideEntry {
    std::wstring label, path;
    wchar_t glyph = 0;
    bool header = false;
    bool collapsed = false;  // só para cabeçalhos
    bool hidden = false;     // item de uma seção recolhida
    UINT32 tagColor = 0;     // etiqueta: bolinha colorida (0xRRGGBB) no lugar do ícone
    int tag = -1;            // etiqueta: índice da cor
    int fav = -1;            // favorito: posição em g_favs (editável)
};

static HWND g_hwnd;
static HINSTANCE g_inst;
static float g_dpi = 96, g_scale = 1;  // pixels por DIP
static bool g_dark, g_winActive = true;
bool g_finderNativo;
bool g_finderEmbutido;
static bool g_pickDone;  // embutido: o escolher terminou (o laço modal de FinderEscolher sai)
static std::wstring g_dirNativo;  // pasta bin do Shell (modo nativo)
void ExePath(wchar_t* out, DWORD n) {
    if (g_finderNativo || g_finderEmbutido) { wcsncpy_s(out, n, (g_dirNativo + L"\\WacOSFinder.exe").c_str(), _TRUNCATE); return; }
    GetModuleFileNameW(nullptr, out, n);
}
static bool g_reserva;  // Finder escondido de reserva (mantido pelo Shell), esperando um pedido

static std::wstring g_folder;  // vazio quando a "pasta" aberta é uma etiqueta
static int g_tagView = -1;     // etiqueta aberta pela barra lateral (-1 = pasta de verdade)
const int VIEW_COMPUTER = tags::COUNT;  // g_tagView especial: "Este Computador" (os discos)
const int VIEW_TRASH = tags::COUNT + 1;  // g_tagView especial: a Lixeira (do Windows), dentro do Finder
const int VIEW_SMART = tags::COUNT + 2;  // Leopard, "Buscar por": VIEW_SMART + 0..5 (search::Smart)
const wchar_t* SMART_NAMES[6] = { L"Hoje", L"Ontem", L"Semana Passada", L"Todas as Imagens", L"Todos os Filmes", L"Todos os Documentos" };
static std::wstring VirtualName(int v) {
    if (v >= VIEW_SMART && v < VIEW_SMART + 6) return Tr(SMART_NAMES[v - VIEW_SMART]);
    return v == VIEW_COMPUTER ? Tr(L"Este Computador") : v == VIEW_TRASH ? Tr(L"Lixeira") : Tr(tags::names[v]);
}
static std::vector<Entry> g_all;    // conteúdo da pasta
static std::vector<Entry> g_items;  // o que aparece (filtrado pela busca)
static std::unordered_map<std::wstring, ID2D1Bitmap*> g_images;
static std::unordered_set<std::wstring> g_requested;
static std::unordered_map<std::wstring, thumbs::Kind> g_kinds;  // arquivos sem miniatura (folha) ou atalho de pasta
static UINT g_gen = 1;
static std::vector<std::wstring> g_back, g_fwd;
static std::vector<SideEntry> g_side;
static ULONG g_notifyId;

static int g_focus = -1, g_anchor = -1;
static float g_scroll = 0, g_scrollTarget = 0;
static DWORD g_scrollShownAt;

static int g_hoverSide = -1, g_hoverTool = -1, g_dropHi = -1;
static D2D1_POINT_2F g_toolMouse{ -1, -1 };  // Tahoe: onde o ponteiro está sobre a barra (o brilho do vidro segue)
static vidro::LuzSegue g_toolLuz;             // a luz, atrás do ponteiro com a mola de 0,3 s
static DWORD g_toolLuzAt;
static bool g_itemsScene;  // Tahoe: o conteúdo rola por baixo da barra de ferramentas (desenhado numa cena)
static bool g_tahoe;  // tema macOS Tahoe (liquid_glass=1 no ajustes.txt do Shell): barra lateral flutuante e cápsulas de vidro
static bool g_hoverLights;
static int g_pressLight = -1;

static int g_pressTool = -1;
static bool g_sliderDrag;
static bool g_hoverTitle;  // mouse sobre o título (área de arrastar a janela)

// Barra lateral: clique/arraste de favoritos e destaques do soltar
static int g_sidePress = -1;       // linha pressionada (age ao soltar o botão)
static D2D1_POINT_2F g_sidePressPt;
static bool g_sideDragging;        // arrastando um favorito para mudar de lugar
static int g_sideInsert = -1;      // posição de inserção nos favoritos (linha azul)
static int g_dropSideRow = -1;     // linha da barra lateral que receberá o soltar

static TextField g_search;

// Modo escolher: o Finder como janela de Abrir/Salvar (chamado pela DialogoMac.dll no lugar do
// diálogo do Windows). Resultado: caminhos, um por linha, e "tipo=N" no arquivo --resultado;
// código de saída 0 = escolheu, 1 = cancelou.
struct Picker {
    bool on = false;
    int mode = 0;                 // 0 abrir, 1 salvar, 2 pasta
    bool multi = false, overwrite = true;
    std::vector<std::pair<std::wstring, std::wstring>> types;  // descrição, padrões ("*.txt;*.md")
    int type = 0;
    std::wstring ok, result, ext;
    HWND owner = nullptr;
};
static Picker g_pick;
static TextField g_pickName;
static bool g_pickNameFocus;
static int g_exitCode = 0;

static bool PickerShows(bool dir, const std::wstring& name) {
    if (!g_pick.on || dir) return true;
    if (g_pick.mode == 2) return false;  // escolher pasta: só pastas
    if (g_pick.types.empty()) return true;
    std::wstring spec = g_pick.types[std::clamp(g_pick.type, 0, (int)g_pick.types.size() - 1)].second;
    if (spec.empty() || spec == L"*.*" || spec == L"*") return true;
    return PathMatchSpecExW(name.c_str(), spec.c_str(), PMSF_MULTIPLE) == S_OK;
}
static bool g_searching;
static std::wstring CurrentTitle();
static bool g_searchFocus;
static TextField g_rename;
static int g_renameIdx = -1;
static bool g_caretOn = true;

enum class Drag { None, Pending, Band };
static Drag g_drag = Drag::None;
static POINT g_dragStartPx;
static int g_pressItem = -1;
static bool g_pressWasSelected;
static bool g_pressWasOnly;       // o item clicado já era o único selecionado (dois cliques lentos = renomear)
static int g_slowRenameIdx = -1;
static D2D1_POINT_2F g_bandStart, g_bandEnd;  // em coordenadas do documento (y + rolagem)
static std::vector<bool> g_bandBase;
static UINT g_bandMods;
static bool g_ownDrag;

static std::wstring g_typed;
static DWORD g_typedAt;
static std::wstring g_flash;
static DWORD g_flashAt;
static std::wstring g_selectAfterReload;
static bool g_renameAfterReload;

// recursos de desenho (independentes do tema)
static IDWriteTextFormat *g_tfLabel, *g_tfSide, *g_tfSideHdr, *g_tfTitle, *g_tfStatus, *g_tfGlyph, *g_tfSearch, *g_tfRename;
static IDWriteTextFormat* g_tfPath;    // barra de caminho
static IDWriteTextFormat *g_tfRow, *g_tfRowR, *g_tfHeader, *g_tfHeaderL, *g_tfHeaderR, *g_tfGroup;  // visão em lista
static IDWriteTextFormat *g_tfPreviewName, *g_tfPreviewInfo;  // prévia das colunas e da galeria
static IDWriteTextFormat* g_tfEmblem;  // símbolos gravados nas pastas (tamanho ajustado ao ícone)
static float g_tfEmblemSize;
static std::wstring g_iconFamily = L"Segoe Fluent Icons";
static std::wstring g_uiFamily = L"Segoe UI";  // SF Pro Text se estiver instalada; senão Inter
static std::wstring g_recentFolder;    // "Recentes" é ordenada por data

// pastas especiais: caminho → símbolo (Segoe Fluent Icons)
static std::vector<std::pair<std::wstring, wchar_t>> g_emblems;

// ---------------------------------------------------------------------------
// Utilidades

static std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
    return s;
}

static bool SamePath(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

static std::wstring Join(const std::wstring& dir, const std::wstring& name) {
    return dir + (dir.back() == L'\\' ? L"" : L"\\") + name;
}

static std::wstring Parent(const std::wstring& p) {
    if (p.size() <= 3) return L"";
    size_t k = p.find_last_of(L'\\');
    if (k == std::wstring::npos) return L"";
    std::wstring r = p.substr(0, k);
    if (r.size() == 2) r += L"\\";
    return r;
}

static std::wstring DisplayName(const std::wstring& path) {
    if (path.size() <= 3) {  // raiz de disco: "Disco Local (C:)"
        SHFILEINFOW sfi{};
        if (SHGetFileInfoW(path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_DISPLAYNAME)) return sfi.szDisplayName;
        return path;
    }
    for (auto& s : g_side) if (!s.header && SamePath(s.path, path)) return s.label;
    return path.substr(path.find_last_of(L'\\') + 1);
}
static std::wstring CurrentTitle() {
    if (g_searching) return Tr(L"Buscando \u201C") + g_search.text + L"\u201D";
    return g_tagView >= 0 ? VirtualName(g_tagView) : DisplayName(g_folder);
}



// Na visão de etiqueta os itens podem estar em pastas diferentes; ações do Windows pedem uma pasta só.
static std::vector<std::wstring> SameFolderAs(const std::vector<std::wstring>& paths) {
    std::vector<std::wstring> out;
    if (paths.empty()) return out;
    std::wstring parent = Parent(paths[0]);
    for (auto& p : paths) if (SamePath(Parent(p), parent)) out.push_back(p);
    return out;
}

static bool IsDarkMode() {
    if (tahoe::Leopard()) return false;  // Leopard: o 10.5 não tinha modo escuro; o metal e a barra lateral são sempre claros
    DWORD v = 1, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v == 0;
}

static void Invalidate() { InvalidateRect(g_hwnd, nullptr, FALSE); }

static void Flash(const std::wstring& s) {
    g_flash = s;
    g_flashAt = GetTickCount();
    Log(L"%ls", s.c_str());
    Invalidate();
}

static D2D1_POINT_2F ToDip(int x, int y) { return { x / g_scale, y / g_scale }; }

static bool In(const D2D1_RECT_F& r, D2D1_POINT_2F p) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; }

static D2D1_RECT_F Inflate(D2D1_RECT_F r, float d) { return { r.left - d, r.top - d, r.right + d, r.bottom + d }; }

// ---------------------------------------------------------------------------
// Visões, ordem e grupos

enum class View { Icons, List, Columns, Gallery };
enum SortCol { SORT_NAME, SORT_SIZE, SORT_KIND, SORT_DATE, SORT_CREATED };
enum GroupBy { GROUP_NONE, GROUP_KIND, GROUP_DATE, GROUP_SIZE };

static View g_view = View::Icons;
static SortCol g_sortCol = SORT_NAME;
static bool g_sortAsc = true;
static GroupBy g_groupBy = GROUP_NONE;

// Colunas: as pastas à esquerda da ativa são fotografias (pasta, conteúdo, qual subpasta está
// aberta, rolagem). A coluna ativa é a pasta aberta de verdade (g_folder / g_items). À direita
// fica o conteúdo da pasta selecionada, ou a prévia do arquivo selecionado.
struct Column { std::wstring folder, open; std::vector<Entry> items; float scroll = 0; };
static std::vector<Column> g_colTrail;
static std::wstring g_childPath;
static std::vector<Entry> g_childItems;

namespace ColLay { const float COL_W = 230, ROW_H = 24, ICON = 16, PREVIEW_W = 280; }
namespace GalLay { const float STRIP_H = 108, THUMB = 64, STEP = 78; }
static float g_galleryX;  // rolagem horizontal da faixa de miniaturas

// Busca, como no Finder: ao digitar, a janela vira uma visão de resultados (g_folder fica vazio)
// de "Este Computador" (índice do Windows Search) ou da pasta atual com subpastas.
static std::wstring g_searchHome;   // pasta onde a busca começou (volta para ela ao limpar)
static int g_searchScope = 0;       // 0 = Este Computador, 1 = pasta atual
static std::atomic<int> g_searchGen{ 0 };
static bool g_searchBusy;
const float SCOPE_H = 30;
static float ScopeBarH() { return g_searching ? SCOPE_H : 0.f; }

// Abas, como no Finder: cada aba guarda sua pasta, histórico, visão, rolagem e seleção.
struct TabState {
    std::wstring folder;
    int tagView = -1;
    std::vector<std::wstring> back, fwd;
    View view = View::Icons;
    float scroll = 0;
    std::vector<Column> colTrail;
    std::vector<std::wstring> sel;
};
static std::vector<TabState> g_tabs(1);
static int g_curTab = 0;
const float TAB_H = 30;
static float TabBarH() { return g_tabs.size() > 1 ? TAB_H : 0.f; }  // a barra só aparece com 2+ abas
const float TRASH_BAR_H = 38;
static float TrashBarH() { return g_tagView == VIEW_TRASH ? TRASH_BAR_H : 0.f; }  // faixa "Lixeira  [Esvaziar]"
// Leopard: o Cover Flow do 10.5 é a lista com um painel de capas em perspectiva em cima (a 4ª visão do Finder)
static bool g_coverFlow;
static float CoverFlowH();
static float TopH() { return Lay::TOOLBAR_H + TabBarH() + ScopeBarH() + TrashBarH() + CoverFlowH(); }  // fim das barras de cima

namespace ListLay {
inline float HEADER_H = 28;  // Leopard: 17 (o cabeçalho baixo do 10.5; ApplyWindowTheme)
const float GROUP_H = 40, SIZE_W = 90, KIND_W = 160, DATE_W = 190;
}

// Opções de Visualização (Ctrl+J), guardadas em %APPDATA%\Arquivos\opcoes.txt
static bool g_listLarge = true;   // lista: ícones grandes (como no vídeo) ou pequenos (padrão do Mac)
static bool g_colSize = true, g_colKind = true, g_colDate = true, g_colCreated = false;  // colunas da lista
static bool g_relDates = true;    // "Hoje às…", "Ontem às…"
static bool g_showThumbs = true;  // prévia no ícone (miniaturas de fotos, vídeos...)
static bool g_colIcons = true, g_colPreview = true;  // visão em colunas
static float g_labelSize = 12, g_rowTextSize = 13;
static void SaveOptions();
static RECT g_savedWin{};  // última posição/tamanho da janela (px), guardada em opcoes.txt
static RECT g_pickWin{};   // o mesmo para a janela de Abrir/Salvar
static int g_pickView = -1;  // visão usada na janela de Abrir/Salvar (-1: lista)
static bool g_sideWidthSet;
static bool g_pathBarUser;  // a barra de caminho das janelas normais (o Abrir/Salvar liga a dele sem mexer nesta)  // largura da barra lateral escolhida (arrastando); senão cabe o nome mais longo
static bool g_pathBar = false;  // barra de caminho embaixo: desligada, como no Finder (Ctrl+Alt+P liga)   // tamanho do texto: ícones / lista e colunas
// Leopard: a lista do 10.5 tem linhas de 19 com o ícone de 16 (sem o tamanho grande do Finder novo)
static float ListRowH() { return tahoe::Leopard() ? 19.f : g_listLarge ? 36.f : 24.f; }
static float ListIcon() { return tahoe::Leopard() ? 16.f : g_listLarge ? 32.f : 16.f; }

static std::wstring ExtOf(const Entry& e) {
    if (e.dir) return L"";
    size_t dot = e.name.find_last_of(L'.');
    return dot == std::wstring::npos ? L"" : Lower(e.name.substr(dot + 1));
}

// Nome do tipo como o Windows descreve ("Imagem PNG", "Arquivo ZIP"...), guardado por extensão.
static std::wstring KindName(const Entry& e) {
    if (e.dir) return Tr(L"Pasta");
    static std::unordered_map<std::wstring, std::wstring> cache;
    std::wstring ext = ExtOf(e);
    auto it = cache.find(ext);
    if (it != cache.end()) return it->second;
    SHFILEINFOW sfi{};
    std::wstring name = (L"x." + ext);
    std::wstring r = SHGetFileInfoW(name.c_str(), FILE_ATTRIBUTE_NORMAL, &sfi, sizeof(sfi), SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES) && sfi.szTypeName[0]
                         ? sfi.szTypeName : (ext.empty() ? Tr(L"Documento") : Tr(L"Arquivo ") + ext);
    cache[ext] = r;
    return r;
}

// Grupo de um item: (ordem, título). Os títulos seguem o Finder em português.
// Leopard: o Finder do 10.5 não tinha grupos (a preferência do usuário fica guardada para os outros temas)
#define g_groupBy (tahoe::Leopard() ? GROUP_NONE : g_groupBy)
static std::pair<int, std::wstring> GroupOf(const Entry& e) {
    if (g_groupBy == GROUP_KIND) {
        if (e.dir) return { 0, Tr(L"Pastas") };
        std::wstring x = ExtOf(e);
        auto is = [&](std::initializer_list<const wchar_t*> l) { for (auto v : l) if (x == v) return true; return false; };
        if (is({ L"exe", L"msi", L"lnk", L"url", L"appref-ms", L"bat", L"cmd", L"com" })) return { 1, Tr(L"Aplicativos") };
        if (is({ L"png", L"jpg", L"jpeg", L"gif", L"bmp", L"webp", L"heic", L"tif", L"tiff", L"svg", L"ico", L"raw" })) return { 2, Tr(L"Imagens") };
        if (is({ L"mp4", L"mov", L"mkv", L"avi", L"webm", L"wmv", L"m4v" })) return { 3, Tr(L"Filmes") };
        if (is({ L"wav", L"mp3", L"flac", L"aac", L"m4a", L"ogg", L"aif", L"aiff", L"wma", L"mid", L"midi" })) return { 4, Tr(L"Músicas") };
        if (x == L"pdf") return { 5, Tr(L"Documentos PDF") };
        if (is({ L"txt", L"md", L"rtf", L"doc", L"docx", L"odt", L"log", L"csv", L"json", L"xml" })) return { 6, Tr(L"Documentos") };
        if (is({ L"zip", L"rar", L"7z", L"tar", L"gz", L"bz2", L"xz", L"cab", L"iso" })) return { 7, Tr(L"Arquivos compactados") };
        return { 8, Tr(L"Outros") };
    }
    if (g_groupBy == GROUP_DATE) {
        FILETIME now; GetSystemTimeAsFileTime(&now);
        ULONGLONG n = ((ULONGLONG)now.dwHighDateTime << 32) | now.dwLowDateTime;
        double days = (double)(n - std::min(n, e.mtime)) / (10000000.0 * 86400);
        if (days < 1) return { 0, Tr(L"Hoje") };
        if (days < 2) return { 1, Tr(L"Ontem") };
        if (days < 7) return { 2, Tr(L"Últimos 7 dias") };
        if (days < 30) return { 3, Tr(L"Últimos 30 dias") };
        if (days < 365) return { 4, Tr(L"Este ano") };
        return { 5, Tr(L"Mais antigos") };
    }
    if (g_groupBy == GROUP_SIZE) {
        if (e.dir) return { 0, Tr(L"Pastas") };
        if (e.size < 1000 * 1000) return { 1, Tr(L"Pequenos (menos de 1 MB)") };
        if (e.size < 100ull * 1000 * 1000) return { 2, Tr(L"Médios (1 a 100 MB)") };
        if (e.size < 1000ull * 1000 * 1000) return { 3, Tr(L"Grandes (100 MB a 1 GB)") };
        return { 4, Tr(L"Enormes (mais de 1 GB)") };
    }
    return { 0, L"" };
}

// Ordena g_items na ordem em que aparecem (grupo, depois a coluna escolhida).
static void SortItems() {
    std::stable_sort(g_items.begin(), g_items.end(), [](const Entry& a, const Entry& b) {
        if (g_groupBy != GROUP_NONE) {
            int ga = GroupOf(a).first, gb = GroupOf(b).first;
            if (ga != gb) return ga < gb;
        }
        int c = 0;
        switch (g_sortCol) {
        case SORT_NAME:
            if (a.dir != b.dir && g_groupBy == GROUP_NONE) return a.dir;  // pastas primeiro (hábito do Windows)
            c = StrCmpLogicalW(a.label.c_str(), b.label.c_str());
            break;
        case SORT_SIZE: c = a.dir != b.dir ? (a.dir ? -1 : 1) : (a.size < b.size ? -1 : a.size > b.size ? 1 : 0); break;
        case SORT_KIND: c = _wcsicmp(KindName(a).c_str(), KindName(b).c_str()); break;
        case SORT_DATE: c = a.mtime < b.mtime ? -1 : a.mtime > b.mtime ? 1 : 0; break;
        case SORT_CREATED: c = a.ctime < b.ctime ? -1 : a.ctime > b.ctime ? 1 : 0; break;
        }
        if (c == 0) c = StrCmpLogicalW(a.label.c_str(), b.label.c_str());
        return g_sortAsc ? c < 0 : c > 0;
    });
}

// Linhas da lista: cabeçalhos de grupo e itens, com a posição vertical de cada uma.
struct ListRow { int item; std::wstring title; float y, h; };
static std::vector<ListRow> g_rows;
static std::vector<int> g_rowOfItem;  // item -> índice em g_rows
static bool g_rowsDirty = true;

static void BuildRows() {
    if (!g_rowsDirty) return;
    g_rows.clear();
    g_rowOfItem.assign(g_items.size(), -1);
    float y = 0;
    int lastGroup = -1;
    for (int i = 0; i < (int)g_items.size(); i++) {
        if (g_groupBy != GROUP_NONE) {
            auto g = GroupOf(g_items[i]);
            if (g.first != lastGroup) {
                g_rows.push_back({ -1, g.second, y, ListLay::GROUP_H });
                y += ListLay::GROUP_H;
                lastGroup = g.first;
            }
        }
        g_rowOfItem[i] = (int)g_rows.size();
        g_rows.push_back({ i, L"", y, ListRowH() });
        y += ListRowH();
#undef g_groupBy
    }
    g_rowsDirty = false;
}

// ---------------------------------------------------------------------------
// Layout

static D2D1_SIZE_F ClientDip() {
    RECT rc; GetClientRect(g_hwnd, &rc);
    return { rc.right / g_scale, rc.bottom / g_scale };
}

static D2D1_RECT_F ContentRect() {
    auto s = ClientDip();
    float top = TopH() + (g_view == View::List ? ListLay::HEADER_H : 0);
    return { Lay::SIDEBAR_W, top, s.width, s.height - Lay::FOOTER_H };
}

// Colunas da lista (Nome | Tamanho | Tipo | Data de Modificação | Data de Criação), conforme as
// Opções de Visualização; se faltar espaço, somem na ordem criação, tipo, modificação, tamanho.
struct ListCols { float nameL, nameR, sizeL, sizeR, kindL, kindR, dateL, dateR, crL, crR; bool size, kind, date, created; };

static ListCols Cols() {
    auto c = ContentRect();
    ListCols k{};
    k.size = g_colSize; k.kind = g_colKind; k.date = g_colDate; k.created = g_colCreated;
    float avail = (c.right - c.left) - 300;  // o nome fica com pelo menos isso
    auto need = [&] { return (k.size ? ListLay::SIZE_W + 12 : 0) + (k.kind ? ListLay::KIND_W : 0) + (k.date ? ListLay::DATE_W : 0) + (k.created ? ListLay::DATE_W : 0); };
    for (bool* f : { &k.created, &k.kind, &k.date, &k.size }) if (need() > avail) *f = false;
    float x = c.right - 16;
    k.crR = x; k.crL = k.created ? x - ListLay::DATE_W : x; x = k.crL;
    k.dateR = x; k.dateL = k.date ? x - ListLay::DATE_W : x; x = k.dateL;
    k.kindR = x; k.kindL = k.kind ? x - ListLay::KIND_W : x; x = k.kindL;
    k.sizeR = k.size ? x - 12 : x; k.sizeL = k.size ? k.sizeR - ListLay::SIZE_W : x; x = k.sizeL;
    k.nameL = c.left + 26;
    k.nameR = x - 12;
    return k;
}

// Colunas: item selecionado sozinho (abre a coluna da direita) e o que aparece ali.
static int SingleSelected() {
    int f = -1;
    for (int i = 0; i < (int)g_items.size(); i++) if (g_items[i].sel) { if (f >= 0) return -1; f = i; }
    return f;
}

enum RightPane { RP_NONE, RP_CHILD, RP_PREVIEW };
static RightPane ColRightPane() {
    int s = SingleSelected();
    if (s < 0) return RP_NONE;
    return g_items[s].dir ? RP_CHILD : (g_colPreview ? RP_PREVIEW : RP_NONE);
}

// Rolagem horizontal automática para a coluna mais à direita ficar visível.
static float ColScrollX() {
    auto c = ContentRect();
    RightPane rp = ColRightPane();
    float total = (g_colTrail.size() + 1) * ColLay::COL_W + (rp == RP_CHILD ? ColLay::COL_W : rp == RP_PREVIEW ? ColLay::PREVIEW_W : 0);
    return std::max(0.f, total - (c.right - c.left));
}

static float ColX(size_t k) { return Lay::SIDEBAR_W + k * ColLay::COL_W - ColScrollX(); }  // borda esquerda da coluna k
static float ActiveColX() { return ColX(g_colTrail.size()); }

// Galeria: área da prévia grande e faixa de miniaturas embaixo (coordenadas de documento).
static float StripTopDoc() { auto c = ContentRect(); return (c.bottom - c.top) - GalLay::STRIP_H; }

static int Columns() {
    if (g_view == View::List || g_view == View::Columns) return 1;
    if (g_view == View::Gallery) return std::max(1, (int)g_items.size());  // uma fileira só
    auto c = ContentRect();
    return std::max(1, (int)((c.right - c.left - 2 * Lay::GRID_PAD_X) / CellMinW()));
}

static float CellW() { return CellMinW(); }  // passo fixo, como no Finder

static float GridLeft() { return ContentRect().left + Lay::GRID_PAD_X; }  // alinhada à esquerda, como no Finder

static float DocHeight() {
    if (g_view == View::List) {
        BuildRows();
        return g_rows.empty() ? 0 : g_rows.back().y + g_rows.back().h + 8;
    }
    if (g_view == View::Columns) return g_items.size() * ColLay::ROW_H + 8;
    if (g_view == View::Gallery) { auto c = ContentRect(); return c.bottom - c.top; }
    int cols = Columns();
    int rows = ((int)g_items.size() + cols - 1) / cols;
    return rows * CellH() + 2 * Lay::GRID_PAD;
}

static float MaxScroll() {
    auto c = ContentRect();
    return std::max(0.f, DocHeight() - (c.bottom - c.top));
}

// Retângulos em coordenadas de documento (sem a rolagem); somar c.top - g_scroll para a tela.
static D2D1_RECT_F CellDoc(int i) {
    if (g_view == View::List) {  // linha inteira
        BuildRows();
        auto c = ContentRect();
        const ListRow& r = g_rows[g_rowOfItem[i]];
        return { c.left, r.y, c.right, r.y + r.h };
    }
    if (g_view == View::Columns) {  // linha da coluna ativa
        float x = ActiveColX();
        return { x, i * ColLay::ROW_H + 4, x + ColLay::COL_W, (i + 1) * ColLay::ROW_H + 4 };
    }
    if (g_view == View::Gallery) {  // miniatura na faixa de baixo
        auto c = ContentRect();
        float x = c.left + 16 + i * GalLay::STEP - g_galleryX, y = StripTopDoc() + 12;
        return { x, y, x + GalLay::STEP, y + GalLay::THUMB + 16 };
    }
    int cols = Columns();
    float cw = CellW();
    float x = GridLeft() + (i % cols) * cw;
    float y = Lay::GRID_PAD + (i / cols) * CellH();
    return { x, y, x + cw, y + CellH() };
}

static D2D1_RECT_F ToView(D2D1_RECT_F r) {
    float dy = ContentRect().top - g_scroll;
    return { r.left, r.top + dy, r.right, r.bottom + dy };
}

static D2D1_RECT_F IconDoc(int i) {
    auto r = CellDoc(i);
    float cy = (r.top + r.bottom) / 2;
    if (g_view == View::List) {
        float x = Cols().nameL;
        return { x, cy - ListIcon() / 2, x + ListIcon(), cy + ListIcon() / 2 };
    }
    if (g_view == View::Columns) return { r.left + 12, cy - ColLay::ICON / 2, r.left + 12 + ColLay::ICON, cy + ColLay::ICON / 2 };
    if (g_view == View::Gallery) {
        float x = r.left + (GalLay::STEP - GalLay::THUMB) / 2;
        return { x, r.top + 6, x + GalLay::THUMB, r.top + 6 + GalLay::THUMB };
    }
    float cx = (r.left + r.right) / 2;
    return { cx - Icon() / 2, r.top + 4, cx + Icon() / 2, r.top + 4 + Icon() };
}

// Galeria: retângulo da prévia grande e do nome embaixo dela (documento).
static D2D1_RECT_F GalleryPreviewDoc() {
    auto c = ContentRect();
    float bottom = StripTopDoc() - 64;
    return { c.left + 24, 20, c.right - 24, std::max(60.f, bottom) };
}

static D2D1_RECT_F LabelDoc(int i) {
    auto r = CellDoc(i);
    if (g_view == View::List) {
        auto k = Cols();
        return { k.nameL + ListIcon() + 8, r.top, k.nameR, r.bottom };
    }
    if (g_view == View::Columns) return { r.left + (g_colIcons ? 12 + ColLay::ICON + 7 : 14), r.top, r.right - 22, r.bottom };
    if (g_view == View::Gallery) {
        auto pv = GalleryPreviewDoc();
        return { pv.left, pv.bottom + 10, pv.right, pv.bottom + 32 };
    }
    float cx = (r.left + r.right) / 2;
    float y = r.top + 4 + Icon() + 6;  // comparado lado a lado com a referência
    return { cx - LabelW() / 2, y, cx + LabelW() / 2, y + Lay::LABEL_H };
}

static int HitItem(D2D1_POINT_2F p) {
    auto c = ContentRect();
    if (!In(c, p)) return -1;
    D2D1_POINT_2F d{ p.x, p.y - c.top + g_scroll };
    if (g_view == View::List) {  // a linha toda responde, como no Finder
        BuildRows();
        auto it = std::upper_bound(g_rows.begin(), g_rows.end(), d.y, [](float y, const ListRow& r) { return y < r.y; });
        if (it == g_rows.begin()) return -1;
        --it;
        return d.y < it->y + it->h ? it->item : -1;
    }
    if (g_view == View::Columns) {  // só a coluna ativa (as outras são tratadas no clique)
        float x = ActiveColX();
        if (p.x < x || p.x >= x + ColLay::COL_W || d.y < 4) return -1;
        int i = (int)((d.y - 4) / ColLay::ROW_H);
        return i < (int)g_items.size() ? i : -1;
    }
    if (g_view == View::Gallery) {
        if (d.y < StripTopDoc() + 12 || d.y > StripTopDoc() + 12 + GalLay::THUMB + 16) return -1;
        int i = (int)((d.x - c.left - 16 + g_galleryX) / GalLay::STEP);
        return d.x >= c.left + 16 && i >= 0 && i < (int)g_items.size() ? i : -1;
    }
    int cols = Columns();
    int row = (int)((d.y - Lay::GRID_PAD) / CellH());
    if (d.x < GridLeft()) return -1;
    int col = (int)((d.x - GridLeft()) / CellW());
    if (d.y < Lay::GRID_PAD || col < 0 || col >= cols) return -1;
    int i = row * cols + col;
    if (i < 0 || i >= (int)g_items.size()) return -1;
    if (In(Inflate(IconDoc(i), 4), d) || In(LabelDoc(i), d)) return i;
    return -1;
}

static void SetScroll(float y) {
    g_scroll = g_scrollTarget = std::clamp(y, 0.f, MaxScroll());
}

static float GalleryMaxX() {
    auto c = ContentRect();
    return std::max(0.f, g_items.size() * GalLay::STEP + 32 - (c.right - c.left));
}

static void EnsureVisible(int i) {
    if (i < 0) return;
    auto c = ContentRect();
    if (g_view == View::Gallery) {  // centraliza a miniatura na faixa
        float x = 16 + i * GalLay::STEP + GalLay::STEP / 2 - (c.right - c.left) / 2;
        g_galleryX = std::clamp(x, 0.f, GalleryMaxX());
        return;
    }
    auto r = CellDoc(i);
    float h = c.bottom - c.top, m = g_view == View::Icons ? 8.f : 0.f;
    if (r.top - m < g_scroll) SetScroll(r.top - m);
    else if (r.bottom + m > g_scroll + h) SetScroll(r.bottom + m - h);
}

// Barra lateral (seções recolhíveis: itens ocultos não ocupam espaço)
// Medidas tiradas do vídeo do Sequoia: o primeiro cabeçalho é colado no topo; os seguintes
// ganham um respiro extra acima (o Mac separa bem as seções).
extern bool g_tahoeLay;
static float SideHeaderGap(int k) { return k == 0 ? 0.f : g_tahoeLay ? 13.f : 9.f; }

static float SideRowH(int k) {
    if (g_side[k].hidden) return 0;
    return g_side[k].header ? Lay::SIDE_HDR_H + SideHeaderGap(k) : Lay::SIDE_ROW_H;
}

static float g_sideScroll;  // a barra lateral rola quando não cabe na janela

static float SideRowTop(int k) {
    float y = Lay::SIDE_TOP - g_sideScroll;
    for (int j = 0; j < k; j++) y += SideRowH(j);
    return y;
}

static float SideMaxScroll() {
    float total = 0;
    for (int k = 0; k < (int)g_side.size(); k++) total += SideRowH(k);
    return std::max(0.f, total + 12 - (ClientDip().height - Lay::SIDE_TOP));
}

// Linha da barra lateral sob o ponto (item ou cabeçalho), ou -1.
static int HitSideRow(D2D1_POINT_2F p) {
    if (p.x >= Lay::SIDEBAR_W || p.y < Lay::SIDE_TOP) return -1;
    float y = Lay::SIDE_TOP - g_sideScroll;
    for (int k = 0; k < (int)g_side.size(); k++) {
        float h = SideRowH(k);
        if (h > 0 && p.y >= y && p.y < y + h) return k;
        y += h;
    }
    return -1;
}

static int HitSide(D2D1_POINT_2F p) {  // só itens (pastas)
    int k = HitSideRow(p);
    return k >= 0 && !g_side[k].header ? k : -1;
}

// Posição de inserção nos favoritos para um ponto vertical (metade de cima de um item = antes dele).
static int FavInsertAt(float y) {
    int last = -1;
    for (int k = 0; k < (int)g_side.size(); k++) {
        if (g_side[k].fav < 0 || g_side[k].hidden) continue;
        if (y < SideRowTop(k) + Lay::SIDE_ROW_H / 2) return g_side[k].fav;
        last = g_side[k].fav;
    }
    return last + 1;
}

static void ToggleSection(int header) {
    g_side[header].collapsed = !g_side[header].collapsed;
    for (int k = header + 1; k < (int)g_side.size() && !g_side[k].header; k++) g_side[k].hidden = g_side[header].collapsed;
    g_sideScroll = std::min(g_sideScroll, SideMaxScroll());
}

// Barra de ferramentas, como no Sequoia:
// ‹ › [pasta] Título ...... [⊞|☰|▥|▭] [agrupar⌄] [compartilhar] [etiqueta] [⋯⌄]    [busca]
static bool SearchExpanded() { return g_searchFocus || !g_search.text.empty() || (!g_tahoe && tahoe::Leopard()); }  // Leopard: o campo fica sempre aberto

// Tahoe: um botão só para a visão (ícone da atual + ⌃⌄), como no macOS 26
static bool Golden();
// macOS 27 Golden Gate: os quatro modos de visualização voltam a ficar abertos numa cápsula (se couber)
static bool WideToolbar() { return Golden() ? ClientDip().width - Lay::SIDEBAR_W >= 560 : !g_tahoe && ClientDip().width - Lay::SIDEBAR_W >= 650; }  // estreita: seletor de visão vira um botão (650 = botões da direita + voltar/avançar + um pedaço do título)

static D2D1_RECT_F ToolRect(int k) {
    auto s = ClientDip();
    float cy = Lay::TOOL_CY, h = Lay::BTN / 2;
    if (!g_tahoe && tahoe::Leopard()) {
        // Leopard (Finder do 10.5, foto de referência): ◀ ▶ na ponta esquerda (a barra vai de ponta a ponta), o pop-up
        // de arrumar ao lado, os quatro modos no meio, a engrenagem ▾ depois deles e o campo de busca na direita.
        // Compartilhar e Etiquetas não existiam.
        const float H = 11;
        if (k == TB_BACK) return { 10, cy - H, 37, cy + H };
        if (k == TB_FWD) return { 37, cy - H, 64, cy + H };
        if (k == TB_SEARCH) return { s.width - 12 - std::min(200.f, s.width * 0.22f), cy - 10, s.width - 12, cy + 10 };
        float vl = std::max(124.f, s.width / 2 - 70);
        if (k >= TB_V_ICONS && k <= TB_V_GALLERY) {
            if (!WideToolbar()) return k == TB_V_ICONS ? D2D1_RECT_F{ vl, cy - H, vl + 40, cy + H } : D2D1_RECT_F{ 0, 0, 0, 0 };
            float x = vl + (k - TB_V_ICONS) * 29.f;
            return { x, cy - H, x + 29, cy + H };
        }
        // captura do 10.5: depois das visões, o olho do Quick Look (44) e a engrenagem ▾ (46), 8 entre eles
        float ex = vl + (WideToolbar() ? 4 * 29.f : 40.f) + 14;
        if (k == TB_GROUP) return { ex, cy - H, ex + 44, cy + H };  // Leopard: o olho do Quick Look (não havia "Arrumar")
        if (k == TB_ACTION) return { ex + 52, cy - H, ex + 98, cy + H };
        return { 0, 0, 0, 0 };  // Compartilhar, Etiquetas, Lixeira
    }
    if (Golden()) {
        // macOS 27 (captura 2x do 512 Pixels): tudo a 8 de distância; Voltar/Avançar 73 × 36; os quatro modos numa
        // cápsula de 160 (pílula de 33 × 26 a cada 39,17, a primeira a 4,5 da borda); Agrupar 47; Compartilhar,
        // Etiquetas e ⋯ com 37 cada; busca num círculo de 36 a 8 da borda da janela
        const float B = 36, G = 8;
        auto bx = [&](float right, float w) { return D2D1_RECT_F{ right - w, cy - B / 2, right, cy + B / 2 }; };
        if (k == TB_BACK) return { Lay::SIDEBAR_W + G, cy - B / 2, Lay::SIDEBAR_W + G + 36.5f, cy + B / 2 };
        if (k == TB_FWD) return { Lay::SIDEBAR_W + G + 36.5f, cy - B / 2, Lay::SIDEBAR_W + G + 73, cy + B / 2 };
        float right = s.width - 8;
        float sw = std::min(307.f, std::max(160.f, (s.width - Lay::SIDEBAR_W) * 0.35f));
        D2D1_RECT_F search = SearchExpanded() ? D2D1_RECT_F{ right - sw, cy - B / 2, right, cy + B / 2 } : bx(right, 36);
        if (k == TB_SEARCH) return search;
        D2D1_RECT_F action = bx(search.left - G, 37);
        if (k == TB_ACTION) return action;
        D2D1_RECT_F tag = bx(action.left, 37);
        if (k == TB_TAG) return tag;
        D2D1_RECT_F share = bx(tag.left, 37);
        if (k == TB_SHARE) return share;
        D2D1_RECT_F group = bx(share.left - G, 47);
        if (k == TB_GROUP) return group;
        if (!WideToolbar()) return k == TB_V_ICONS ? bx(group.left - G, B) : D2D1_RECT_F{ 0, 0, 0, 0 };
        if (k < TB_V_ICONS || k > TB_V_GALLERY) return { 0, 0, 0, 0 };
        float capL = group.left - G - 160, mid = capL + 21 + (k - TB_V_ICONS) * 39.17f;
        return { mid - 16.5f, cy - 13, mid + 16.5f, cy + 13 };
    }
    if (g_tahoe) {
        // Tahoe (guia, seções 31 e 42, medido): ilhas de vidro de 36 de altura (raio 18) com botões de 36 × 36,
        // 8 entre as ilhas; Voltar/Avançar = dois segmentos colados; busca = cápsula de 39 × 38 que abre num campo
        const float B = 36, G = 8;
        auto bx = [&](float right, float w) { return D2D1_RECT_F{ right - w, cy - B / 2, right, cy + B / 2 }; };
        if (k == TB_BACK) return { Lay::SIDEBAR_W + G, cy - B / 2, Lay::SIDEBAR_W + G + B, cy + B / 2 };
        if (k == TB_FWD) return { Lay::SIDEBAR_W + G + B, cy - B / 2, Lay::SIDEBAR_W + G + 2 * B, cy + B / 2 };
        float right = s.width - 10;
        float sw = std::min(307.f, std::max(160.f, (s.width - Lay::SIDEBAR_W) * 0.35f));
        D2D1_RECT_F search = SearchExpanded() ? D2D1_RECT_F{ right - sw, cy - B / 2, right, cy + B / 2 } : D2D1_RECT_F{ right - 39, cy - 19, right, cy + 19 };
        if (k == TB_SEARCH) return search;
        D2D1_RECT_F action = bx(search.left - G, B);
        if (k == TB_ACTION) return action;
        D2D1_RECT_F tag = bx(action.left, B);
        if (k == TB_TAG) return tag;
        D2D1_RECT_F share = bx(tag.left, B);
        if (k == TB_SHARE) return share;
        D2D1_RECT_F group = bx(share.left - G, B);
        if (k == TB_GROUP) return group;
        if (k == TB_V_ICONS) return bx(group.left - G, B);
        return { 0, 0, 0, 0 };
    }
    auto box = [&](float right, float w) { return D2D1_RECT_F{ right - w, cy - h, right, cy + h }; };
    switch (k) {
    case TB_BACK: return { Lay::SIDEBAR_W + 15, cy - h, Lay::SIDEBAR_W + 15 + Lay::BTN, cy + h };
    case TB_FWD: return { Lay::SIDEBAR_W + 51.6f, cy - h, Lay::SIDEBAR_W + 51.6f + Lay::BTN, cy + h };
    }
    float right = s.width - 10.4f;
    D2D1_RECT_F search = SearchExpanded() ? D2D1_RECT_F{ right - Lay::SEARCH_W, cy - 14, right, cy + 14 } : box(right, 32);
    if (k == TB_SEARCH) return search;
    // como na barra do Finder: Agrupar, Compartilhar, Etiquetas, Ações e Buscar (sem a lixeira)
    D2D1_RECT_F action = box(search.left - 35.3f, 44);
    if (k == TB_ACTION) return action;
    D2D1_RECT_F tag = box(action.left - 12.2f, 34);
    if (k == TB_TAG) return tag;
    if (k == TB_TRASH) return { 0, 0, 0, 0 };
    D2D1_RECT_F share = box(tag.left - 6.9f, 34);
    if (k == TB_SHARE) return share;
    D2D1_RECT_F group = box(share.left - 10.2f, 46);
    if (k == TB_GROUP) return group;
    float vr = group.left - 36;
    if (!WideToolbar()) {
        if (k == TB_V_ICONS) return box(vr, 46);
        return { 0, 0, 0, 0 };
    }
    int seg = k - TB_V_ICONS;  // 0..3 da esquerda para a direita; destaque de 39 × 26,4 como no vídeo
    D2D1_RECT_F r = box(vr - (3 - seg) * 41.8f, 39);
    r.top += 0.8f; r.bottom -= 0.8f;
    return r;
}

static int HitTool(D2D1_POINT_2F p) {
    for (int k = 0; k < TB_COUNT; k++) if (In(ToolRect(k), p)) return k;
    return -1;
}

// Controle deslizante do tamanho dos ícones, à direita da barra de baixo
static D2D1_RECT_F SliderRect() {
    auto s = ClientDip();
    float cy = s.height - Lay::STATUS_H / 2;
    return { s.width - 14 - Lay::SLIDER_W, cy - 8, s.width - 14, cy + 8 };
}

static void SetIconSizeFromSlider(float x);

// Botões da janela (fechar, minimizar, maximizar), no canto superior esquerdo
bool g_tahoeLay;  // g_tahoe já aplicado às medidas (ApplyWindowTheme)
// padrão do projeto: 14 pt, 23 entre centros; Tahoe (guia, seção 42): áreas de 16 × 16 em x 18 / 41 / 64 (centros 26, 49, 72)
static D2D1_POINT_2F LightCenter(int k) {
    if (!g_tahoeLay && tahoe::Leopard()) return { 14.5f + k * 21.f, 12.f };  // Leopard: na linha do título (22 px), 21 entre centros
    return { (g_tahoeLay ? 26.f : 23.f) + k * 23.f, Lay::TOOL_CY };
}

static int HitLight(D2D1_POINT_2F p) {
    for (int k = 0; k < 3; k++) {
        auto c = LightCenter(k);
        if (std::fabs(p.x - c.x) <= 8 && std::fabs(p.y - c.y) <= 8) return k;
    }
    return -1;
}

static bool OverLightsArea(D2D1_POINT_2F p) { return p.x >= 5 && p.x < LightCenter(2).x + 15 && std::fabs(p.y - LightCenter(0).y) <= 10; }
// borda da barra lateral (abaixo da barra de cima): arrastar muda a largura
static bool OverSideEdge(D2D1_POINT_2F p) { return p.y >= Lay::TOOLBAR_H && std::fabs(p.x - Lay::SIDEBAR_W) <= 4; }
static bool g_sideResize;

static D2D1_RECT_F RenameRect() {
    auto l = ToView(LabelDoc(g_renameIdx));
    if (g_view == View::List || g_view == View::Columns) return { l.left - 4, l.top + 3, std::min(l.right, l.left + 320), l.bottom - 3 };
    if (g_view == View::Gallery) { float cx = (l.left + l.right) / 2; return { cx - 160, l.top, cx + 160, l.top + 20 }; }
    auto cell = ToView(CellDoc(g_renameIdx));
    float w = std::min(cell.right - cell.left + 30, 220.f);
    float cx = (l.left + l.right) / 2;
    return { cx - w / 2, l.top - 2, cx + w / 2, l.top + 18 };
}

// ---------------------------------------------------------------------------
// Seleção

static void ClearSel() { for (auto& e : g_items) e.sel = false; }

static void PickerSelectionChanged();
static void SelectOnly(int i) {
    ClearSel();
    if (i >= 0 && i < (int)g_items.size()) g_items[i].sel = true;
    g_focus = g_anchor = i;
    EnsureVisible(i);
    PickerSelectionChanged();
}

static void SelectRange(int a, int b) {
    ClearSel();
    if (a > b) std::swap(a, b);
    for (int i = std::max(a, 0); i <= b && i < (int)g_items.size(); i++) g_items[i].sel = true;
}

static std::vector<std::wstring> SelectedPaths() {
    std::vector<std::wstring> r;
    for (auto& e : g_items) if (e.sel) r.push_back(e.path);
    return r;
}

// Obter Informações: uma janela por item (sem seleção, a pasta aberta), até 10 de uma vez
static void GetInfo(std::vector<std::wstring> paths) {
    if (paths.empty() && !g_folder.empty()) paths.push_back(g_folder);
    if (paths.size() > 10) paths.resize(10);
    for (auto& p : paths) info::Show(p, g_hwnd, g_uiFamily, g_dark);
}

static int SelCount() {
    int n = 0;
    for (auto& e : g_items) n += e.sel;
    return n;
}

// ---------------------------------------------------------------------------
// Pasta: leitura, filtro, observação de mudanças

static std::map<std::wstring, double> g_imgBorn;  // miniatura -> quando chegou (ms), para o fade de entrada

static void ReleaseImages() {
    for (auto& [k, b] : g_images) if (b) b->Release();
    g_images.clear();
    g_imgBorn.clear();
    g_requested.clear();
    g_kinds.clear();
}

static void ApplyFilter() {
    std::unordered_set<std::wstring> sel;
    for (auto& e : g_items) if (e.sel) sel.insert(e.path);
    std::wstring q = Lower(g_search.text);
    g_items.clear();
    for (auto& e : g_all) {
        if (!g_searching && !q.empty() && Lower(e.label).find(q) == std::wstring::npos) continue;
        if (!PickerShows(e.dir, e.name)) continue;
        g_items.push_back(e);
        g_items.back().sel = sel.count(e.path) > 0;
    }
    SortItems();
    g_rowsDirty = true;
    g_focus = g_anchor = -1;
    for (int i = 0; i < (int)g_items.size(); i++) if (g_items[i].sel) { g_focus = g_anchor = i; break; }
    g_scroll = g_scrollTarget = std::min(g_scroll, MaxScroll());
}

static std::vector<Entry> ReadEntries(const std::wstring& folder) {
    std::vector<Entry> out;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW(Join(folder, L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
            if (fd.cFileName[0] == L'.') continue;  // como no Mac: nomes com ponto ficam ocultos
            Entry e;
            e.name = fd.cFileName;
            e.path = Join(folder, fd.cFileName);
            e.dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            e.mtime = ((ULONGLONG)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
            e.size = ((ULONGLONG)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            e.ctime = ((ULONGLONG)fd.ftCreationTime.dwHighDateTime << 32) | fd.ftCreationTime.dwLowDateTime;
            e.tags = tags::Read(e.path);
            e.label = e.name;
            if (!e.dir && e.label.size() > 4 && !_wcsicmp(e.label.c_str() + e.label.size() - 4, L".lnk")) e.label.resize(e.label.size() - 4);
            if (e.dir) for (auto& [p, g] : g_emblems) if (SamePath(p, e.path)) { e.emblem = g; break; }
            out.push_back(std::move(e));
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    bool byDate = SamePath(folder, g_recentFolder);
    std::sort(out.begin(), out.end(), [byDate](const Entry& a, const Entry& b) {
        if (byDate) return a.mtime > b.mtime;  // Recentes: mais novo primeiro
        if (a.dir != b.dir) return a.dir;
        return StrCmpLogicalW(a.name.c_str(), b.name.c_str()) < 0;
    });
    return out;
}

static void ReadFolder() { g_all = ReadEntries(g_folder); }

static void WatchFolder() {
    if (g_notifyId) { SHChangeNotifyDeregister(g_notifyId); g_notifyId = 0; }
    PIDLIST_ABSOLUTE pidl;
    if (FAILED(SHParseDisplayName(g_folder.c_str(), nullptr, &pidl, 0, nullptr))) return;
    SHChangeNotifyEntry e{ pidl, FALSE };
    g_notifyId = SHChangeNotifyRegister(g_hwnd, SHCNRF_ShellLevel | SHCNRF_InterruptLevel | SHCNRF_NewDelivery,
                                        SHCNE_ALLEVENTS, WM_APP_CHANGE, 1, &e);
    CoTaskMemFree(pidl);
}

static void StartRename(int i);
static void EndRename(bool commit);

// Relê a pasta mantendo seleção, rolagem e miniaturas (sem piscar).
static void RefreshTagView();

static void Reload() {
    if (g_renameIdx >= 0) return;  // não atrapalha quem está digitando um nome
    if (g_searching) return;  // resultados não são observados; a busca é refeita ao digitar
    if (g_tagView >= 0) { RefreshTagView(); return; }
    std::unordered_set<std::wstring> sel;
    for (auto& e : g_items) if (e.sel) sel.insert(e.path);
    ReadFolder();
    std::unordered_set<std::wstring> keys;
    for (auto& e : g_all) keys.insert(e.Key());
    for (auto it = g_images.begin(); it != g_images.end();) {  // descarta miniaturas de arquivos que mudaram
        if (!keys.count(it->first)) { if (it->second) it->second->Release(); g_requested.erase(it->first); it = g_images.erase(it); }
        else ++it;
    }
    g_items.clear();
    ApplyFilter();
    for (auto& e : g_items) e.sel = sel.count(e.path) > 0;
    if (!g_selectAfterReload.empty()) {
        for (int i = 0; i < (int)g_items.size(); i++)
            if (SamePath(g_items[i].name, g_selectAfterReload)) {
                SelectOnly(i);
                if (g_renameAfterReload) StartRename(i);
                break;
            }
        g_selectAfterReload.clear();
        g_renameAfterReload = false;
    }
    Invalidate();
}

static int g_navMs;  // tempo da última leitura de pasta (medição)

// keepColumns: navegação feita pelas próprias colunas (não recomeça a fileira de colunas).
static void Navigate(const std::wstring& path, bool push = true, const std::wstring& selectName = L"", bool keepColumns = false) {
    DWORD a = GetFileAttributesW(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) { Flash(Tr(L"Não foi possível abrir ") + path); return; }
    std::wstring p = path;
    while (p.size() > 3 && p.back() == L'\\') p.pop_back();
    if (p.size() == 2) p += L"\\";
    if (push && !g_folder.empty() && !SamePath(p, g_folder)) { g_back.push_back(g_folder); g_fwd.clear(); }
    if (!keepColumns) g_colTrail.clear();
    g_tagView = -1;
    g_searching = false; g_searchBusy = false; g_searchGen++;
    g_childPath.clear();
    g_galleryX = 0;
    g_renameIdx = -1;
    g_folder = p;
    g_search.SetText(L"", false);
    g_searchFocus = false;
    g_gen++;
    thumbs::SetGeneration(g_gen);
    LARGE_INTEGER nf, na, nb; QueryPerformanceFrequency(&nf); QueryPerformanceCounter(&na);
    ReleaseImages();
    ReadFolder();
    g_items.clear();
    ApplyFilter();
    QueryPerformanceCounter(&nb);
    g_navMs = (int)((nb.QuadPart - na.QuadPart) * 1000 / nf.QuadPart);
    SetScroll(0);
    g_focus = g_anchor = -1;
    if (!selectName.empty())
        for (int i = 0; i < (int)g_items.size(); i++) if (SamePath(g_items[i].name, selectName)) { SelectOnly(i); break; }
    if (g_view == View::Gallery && g_focus < 0 && !g_items.empty()) SelectOnly(0);  // a galeria sempre mostra um item
    WatchFolder();
    SetWindowTextW(g_hwnd, CurrentTitle().c_str());
    Log(L"pasta: %ls (%zu itens, %d ms)", g_folder.c_str(), g_all.size(), g_navMs);
    Invalidate();
}

// Etiqueta aberta pela barra lateral: lista todos os itens marcados com ela (de qualquer pasta).
static bool EntryFromPath(const std::wstring& p, Entry& e) {
        WIN32_FILE_ATTRIBUTE_DATA d;
        if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return false;
        e.path = p;
        e.name = p.substr(p.find_last_of(L'\\') + 1);
        e.dir = (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.mtime = ((ULONGLONG)d.ftLastWriteTime.dwHighDateTime << 32) | d.ftLastWriteTime.dwLowDateTime;
        e.size = ((ULONGLONG)d.nFileSizeHigh << 32) | d.nFileSizeLow;
        e.ctime = ((ULONGLONG)d.ftCreationTime.dwHighDateTime << 32) | d.ftCreationTime.dwLowDateTime;
        e.label = e.name;
        if (!e.dir && e.label.size() > 4 && !_wcsicmp(e.label.c_str() + e.label.size() - 4, L".lnk")) e.label.resize(e.label.size() - 4);
        if (e.dir) for (auto& [ep, g] : g_emblems) if (SamePath(ep, e.path)) { e.emblem = g; break; }
        e.tags = tags::Read(p);
        return true;
}

static std::wstring LowerPath(std::wstring s);
// A Lixeira do Windows: cada item é o arquivo de verdade em $Recycle.Bin ($Rxxxx), mostrado com o nome
// original; a data é a de quando foi para a Lixeira (mais recente primeiro, como no Mac).
static void LoadTrash() {
    PROPERTYKEY kFrom{}, kDate{};
    PSGetPropertyKeyFromName(L"System.Recycle.DeletedFrom", &kFrom);
    PSGetPropertyKeyFromName(L"System.Recycle.DateDeleted", &kDate);
    IShellItem* bin = nullptr;
    if (FAILED(SHGetKnownFolderItem(FOLDERID_RecycleBinFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&bin)))) return;
    IEnumShellItems* en = nullptr;
    if (SUCCEEDED(bin->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&en)))) {
        IShellItem* it = nullptr;
        while (en->Next(1, &it, nullptr) == S_OK) {
            PWSTR fs = nullptr, name = nullptr, from = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &fs)) && SUCCEEDED(it->GetDisplayName(SIGDN_PARENTRELATIVEEDITING, &name))) {
                Entry e;
                if (EntryFromPath(fs, e)) {
                    e.name = e.label = name;
                    if (!e.dir && e.label.size() > 4 && !_wcsicmp(e.label.c_str() + e.label.size() - 4, L".lnk")) e.label.resize(e.label.size() - 4);
                    IShellItem2* i2 = nullptr;
                    if (SUCCEEDED(it->QueryInterface(IID_PPV_ARGS(&i2)))) {
                        FILETIME ft{};
                        if (SUCCEEDED(i2->GetFileTime(kDate, &ft))) e.mtime = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
                        if (SUCCEEDED(i2->GetString(kFrom, &from)) && from) e.origin = from;
                        i2->Release();
                    }
                    g_all.push_back(std::move(e));
                }
            }
            if (fs) CoTaskMemFree(fs);
            if (name) CoTaskMemFree(name);
            if (from) CoTaskMemFree(from);
            it->Release();
        }
        en->Release();
    }
    bin->Release();
    std::sort(g_all.begin(), g_all.end(), [](const Entry& a, const Entry& b) { return a.mtime > b.mtime; });
}

// "undelete" (Devolver) ou "delete" (Apagar Imediatamente, com a confirmação) nos itens da Lixeira
static int TrashVerb(const std::vector<std::wstring>& paths, const wchar_t* verb) {
    std::unordered_set<std::wstring> want;
    for (auto& p : paths) want.insert(LowerPath(p));
    std::vector<PIDLIST_ABSOLUTE> ids;
    IShellItem* bin = nullptr;
    if (FAILED(SHGetKnownFolderItem(FOLDERID_RecycleBinFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&bin)))) return 0;
    IEnumShellItems* en = nullptr;
    if (SUCCEEDED(bin->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&en)))) {
        IShellItem* it = nullptr;
        while (en->Next(1, &it, nullptr) == S_OK) {
            PWSTR fs = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &fs)) && want.count(LowerPath(fs))) {
                PIDLIST_ABSOLUTE pidl = nullptr;
                if (SUCCEEDED(SHGetIDListFromObject(it, &pidl))) ids.push_back(pidl);
            }
            if (fs) CoTaskMemFree(fs);
            it->Release();
        }
        en->Release();
    }
    bin->Release();
    int done = 0;
    if (!ids.empty()) {
        IShellItemArray* arr = nullptr;
        if (SUCCEEDED(SHCreateShellItemArrayFromIDLists((UINT)ids.size(), (PCIDLIST_ABSOLUTE*)ids.data(), &arr))) {
            IContextMenu* cm = nullptr;
            if (SUCCEEDED(arr->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&cm)))) {
                HMENU m = CreatePopupMenu();
                if (SUCCEEDED(cm->QueryContextMenu(m, 0, 1, 0x7FFF, CMF_NORMAL))) {
                    char a[32]; WideCharToMultiByte(CP_ACP, 0, verb, -1, a, 32, nullptr, nullptr);
                    CMINVOKECOMMANDINFOEX ci{ sizeof(ci) };
                    ci.fMask = CMIC_MASK_UNICODE;
                    ci.hwnd = g_hwnd; ci.lpVerb = a; ci.lpVerbW = verb; ci.nShow = SW_SHOWNORMAL;
                    if (SUCCEEDED(cm->InvokeCommand((CMINVOKECOMMANDINFO*)&ci))) done = (int)ids.size();
                }
                DestroyMenu(m);
                cm->Release();
            }
            arr->Release();
        }
    }
    for (auto pidl : ids) CoTaskMemFree(pidl);
    Log(L"lixeira: %ls em %d item(ns)", verb, done);
    return done;
}

static void EmptyTrash() {
    SHQUERYRBINFO q{ sizeof(q) };
    if (SUCCEEDED(SHQueryRecycleBinW(nullptr, &q)) && q.i64NumItems == 0) return;
    SHEmptyRecycleBinW(g_hwnd, nullptr, 0);  // a confirmação do Windows vira o alerta do Mac (Shell)
}

static void LoadTagItems() {
    g_all.clear();
    if (g_tagView == VIEW_TRASH) { LoadTrash(); return; }
    if (g_tagView >= VIEW_SMART && g_tagView < VIEW_SMART + 6) {  // Leopard, "Buscar por": a busca pronta no índice
        int got = search::Smart(g_tagView - VIEW_SMART, 400, [](const std::wstring& p) {
            Entry e;
            if (EntryFromPath(p, e)) g_all.push_back(std::move(e));
            return true;
        });
        Log(L"buscar por %ls: %d itens", SMART_NAMES[g_tagView - VIEW_SMART], got);
        return;
    }
    if (g_tagView == VIEW_COMPUTER) {  // discos fixos, removíveis e de rede, na ordem das letras
        DWORD drives = GetLogicalDrives();
        for (int d = 0; d < 26; d++) {
            if (!(drives & (1u << d))) continue;
            std::wstring root = std::wstring(1, (wchar_t)(L'A' + d)) + L":\\";
            UINT type = GetDriveTypeW(root.c_str());
            if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE && type != DRIVE_REMOTE && type != DRIVE_CDROM) continue;
            Entry e;
            if (!EntryFromPath(root, e)) continue;
            e.name = e.label = DisplayName(root);
            e.path = root;
            g_all.push_back(std::move(e));
        }
        return;
    }
    for (auto& p : tags::Tagged(g_tagView)) {
        Entry e;
        if (EntryFromPath(p, e)) g_all.push_back(std::move(e));
    }
}

static void RefreshTagView() {
    std::unordered_set<std::wstring> sel;
    for (auto& e : g_items) if (e.sel) sel.insert(e.path);
    LoadTagItems();
    g_items.clear();
    ApplyFilter();
    for (auto& e : g_items) e.sel = sel.count(e.path) > 0;
    Invalidate();
}

static void NavigateTag(int t) {
    if (g_renameIdx >= 0) EndRename(false);
    if (!g_folder.empty()) { g_back.push_back(g_folder); g_fwd.clear(); }
    if (g_notifyId) { SHChangeNotifyDeregister(g_notifyId); g_notifyId = 0; }
    g_tagView = t;
    g_searching = false; g_searchBusy = false; g_searchGen++;
    g_folder.clear();
    g_colTrail.clear();
    g_childPath.clear();
    g_search.SetText(L"", false);
    g_searchFocus = false;
    g_gen++;
    thumbs::SetGeneration(g_gen);
    ReleaseImages();
    LoadTagItems();
    g_items.clear();
    ApplyFilter();
    SetScroll(0);
    g_focus = g_anchor = -1;
    if (g_view == View::Gallery && !g_items.empty()) SelectOnly(0);
    if (t == VIEW_TRASH) {  // itens entrando e saindo da Lixeira atualizam a janela
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHGetKnownFolderIDList(FOLDERID_RecycleBinFolder, 0, nullptr, &pidl))) {
            SHChangeNotifyEntry ne{ pidl, FALSE };
            g_notifyId = SHChangeNotifyRegister(g_hwnd, SHCNRF_ShellLevel | SHCNRF_InterruptLevel | SHCNRF_NewDelivery, SHCNE_ALLEVENTS, WM_APP_CHANGE, 1, &ne);
            CoTaskMemFree(pidl);
        }
    }
    SetWindowTextW(g_hwnd, CurrentTitle().c_str());
    Log(L"visão: %ls (%zu itens)", VirtualName(t).c_str(), g_all.size());
    Invalidate();
}

// ---------------------------------------------------------------------------
// Busca no computador

struct SearchBatch { std::vector<std::wstring> paths; bool done = false; };
const int SEARCH_MAX = 3000;

// Thread de busca: pergunta ao índice do Windows Search e manda os caminhos em lotes para a
// janela. Se a pasta não está no índice, varre pelo nome. Para assim que outra busca começa.
static void SearchThread(HWND hwnd, int gen, std::wstring q, std::wstring folder) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SearchBatch* b = new SearchBatch;
    DWORD last = GetTickCount();
    auto onPath = [&](const std::wstring& p) {
        if (g_searchGen != gen) return false;
        b->paths.push_back(p);
        if (b->paths.size() >= 200 || GetTickCount() - last > 150) {
            PostMessageW(hwnd, WM_APP_SEARCH, gen, (LPARAM)b);
            b = new SearchBatch;
            last = GetTickCount();
        }
        return true;
    };
    // primeiro o índice do Spotlight (nomes, instantâneo); depois o Windows Search acrescenta o que só
    // casa pelo conteúdo, sem repetir
    std::unordered_set<std::wstring> seen;
    auto once = [&](const std::wstring& p) {
        std::wstring k = p; for (auto& c : k) c = towlower(c);
        if (!seen.insert(k).second) return g_searchGen == gen;
        return onPath(p);
    };
    int n = search::Spotlight(q, folder, SEARCH_MAX, once);
    if (g_searchGen == gen && (int)seen.size() < SEARCH_MAX) n = std::max(n, 0) + std::max(search::Index(q, folder, SEARCH_MAX - (int)seen.size(), once), 0);
    if (n <= 0 && !folder.empty() && g_searchGen == gen) search::Walk(q, folder, SEARCH_MAX, once);
    b->done = true;
    if (!PostMessageW(hwnd, WM_APP_SEARCH, gen, (LPARAM)b)) delete b;
    CoUninitialize();
}

static void RunSearch() {
    KillTimer(g_hwnd, T_SEARCH);
    if (!g_searching || g_search.text.empty()) return;
    int gen = ++g_searchGen;
    g_gen++;
    thumbs::SetGeneration(g_gen);
    ReleaseImages();
    g_all.clear();
    g_items.clear();
    ApplyFilter();
    SetScroll(0);
    g_searchBusy = true;
    std::wstring folder = g_searchScope == 1 ? g_searchHome : L"";
    Log(L"busca: \"%ls\" em %ls", g_search.text.c_str(), folder.empty() ? L"Este Computador" : folder.c_str());
    std::thread(SearchThread, g_hwnd, gen, g_search.text, folder).detach();
    Invalidate();
}

static void EnterSearch() {
    if (g_searching) return;
    if (g_renameIdx >= 0) EndRename(true);
    g_searchHome = g_folder;
    g_searching = true;
    g_folder.clear();
    g_colTrail.clear();
    g_childPath.clear();
    if (g_notifyId) { SHChangeNotifyDeregister(g_notifyId); g_notifyId = 0; }
}

static void ExitSearch() {
    if (!g_searching) return;
    KillTimer(g_hwnd, T_SEARCH);
    std::wstring home = g_searchHome;
    Navigate(home, false);  // também limpa o campo e cancela a thread
}

// Chamado a cada mudança no campo de busca.
static void OnSearchChanged() {
    if (g_tagView >= 0 || (!g_searching && g_folder.empty())) { ApplyFilter(); return; }  // etiqueta: filtro local
    if (g_search.text.empty()) { ExitSearch(); return; }
    EnterSearch();
    SetWindowTextW(g_hwnd, CurrentTitle().c_str());
    SetTimer(g_hwnd, T_SEARCH, 250, nullptr);  // espera parar de digitar
}

static void OnSearchBatch(int gen, SearchBatch* b) {
    if (gen == g_searchGen && g_searching) {
        for (auto& p : b->paths) {
            Entry e;
            if (EntryFromPath(p, e)) g_all.push_back(std::move(e));
        }
        if (b->done) g_searchBusy = false;
        float sc = g_scroll;
        ApplyFilter();
        g_scroll = g_scrollTarget = std::min(sc, MaxScroll());
        if (b->done) Log(L"busca: %zu resultados", g_all.size());
        Invalidate();
    }
    delete b;
}

static void GoBack() {
    if (g_searching) { ExitSearch(); return; }  // voltar sai da busca
    if (g_back.empty()) return;
    std::wstring from = g_folder, to = g_back.back();
    g_back.pop_back();
    g_fwd.push_back(from);
    Navigate(to, false, from.substr(from.find_last_of(L'\\') + 1));
}

static void GoForward() {
    if (g_fwd.empty()) return;
    std::wstring to = g_fwd.back();
    g_fwd.pop_back();
    g_back.push_back(g_folder);
    Navigate(to, false);
}

static void GoUp() {
    std::wstring p = Parent(g_folder);
    if (!p.empty()) Navigate(p, true, g_folder.substr(g_folder.find_last_of(L'\\') + 1));
}

// Colunas: a pasta `i` da coluna ativa passa a ser a nova coluna ativa, à direita.
static void ColumnEnter(int i, bool selectFirst) {
    if (i < 0 || i >= (int)g_items.size() || !g_items[i].dir) return;
    std::wstring target = g_items[i].path;
    g_colTrail.push_back({ g_folder, target, g_items, g_scroll });
    Navigate(target, true, L"", true);
    if (selectFirst && !g_items.empty()) SelectOnly(0);
}

// Colunas: volta a ativar a coluna k da esquerda, selecionando `selectPath` nela.
static void ColumnActivate(size_t k, const std::wstring& selectPath) {
    if (k >= g_colTrail.size()) return;
    Column t = g_colTrail[k];
    g_colTrail.resize(k);
    Navigate(t.folder, true, L"", true);
    g_scroll = g_scrollTarget = std::min(t.scroll, MaxScroll());
    for (int i = 0; i < (int)g_items.size(); i++) if (SamePath(g_items[i].path, selectPath)) { SelectOnly(i); break; }
}

static void ColumnLeft() {
    if (!g_colTrail.empty()) ColumnActivate(g_colTrail.size() - 1, g_colTrail.back().open);
}

// Conteúdo da pasta mostrada à direita da coluna ativa (lido uma vez e guardado).
static const std::vector<Entry>& ChildItems(const std::wstring& path) {
    if (!SamePath(path, g_childPath)) { g_childItems = ReadEntries(path); g_childPath = path; }
    return g_childItems;
}

static std::wstring LinkTarget(const std::wstring& lnk) {
    std::wstring out;
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return out;
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) {
        wchar_t t[MAX_PATH] = {};
        if (SUCCEEDED(pf->Load(lnk.c_str(), STGM_READ)) && SUCCEEDED(sl->GetPath(t, MAX_PATH, nullptr, 0))) out = t;
        pf->Release();
    }
    sl->Release();
    return out;
}

static void PickerAccept(int clicked);
static void PickerRemember();
static void Open(int i) {
    if (i < 0 || i >= (int)g_items.size()) return;
    const Entry& e = g_items[i];
    if (g_pick.on && !e.dir) { PickerAccept(i); return; }
    if (e.dir) {
        if (g_view == View::Columns) ColumnEnter(i, false);  // nas colunas, a pasta abre na coluna seguinte
        else Navigate(e.path);
        return;
    }
    if (Lower(PathFindExtensionW(e.path.c_str())) == L".lnk") {  // atalho para pasta abre aqui mesmo
        std::wstring t = LinkTarget(e.path);
        DWORD a = t.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(t.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) { Navigate(t); return; }
    }
    if (AbrirNoAppMac(e.path)) return;  // áudio: no Música (como no Mac), não no Media Player
    ShellExecuteW(g_hwnd, nullptr, e.path.c_str(), nullptr, g_folder.c_str(), SW_SHOWNORMAL);
}

static void OpenSelection() {
    std::vector<int> sel;
    for (int i = 0; i < (int)g_items.size(); i++) if (g_items[i].sel) sel.push_back(i);
    for (int i : sel) if (g_items[i].dir) { Open(i); return; }  // uma pasta: entra nela
    for (int i : sel) Open(i);
}

// ---------------------------------------------------------------------------
// Renomear e nova pasta

static void StartRename(int i) {
    if (i < 0 || i >= (int)g_items.size()) return;
    SelectOnly(i);
    g_renameIdx = i;
    const std::wstring& n = g_items[i].label;  // atalhos: edita sem o ".lnk"
    g_rename.SetText(n, false);
    size_t dot = n.rfind(L'.');
    g_rename.Select(0, (!g_items[i].dir && dot != std::wstring::npos && dot > 0) ? (int)dot : (int)n.size());  // nome sem extensão
    g_caretOn = true;
    SetTimer(g_hwnd, T_CARET, 530, nullptr);
    Invalidate();
}

struct UndoOp {
    enum Kind { Trash, Rename } kind;
    std::vector<std::pair<std::wstring, std::wstring>> items;  // Lixeira: (original, -); renomear: (novo, antigo)
};
static std::vector<UndoOp> g_undo;

static void PushUndo(UndoOp op) {
    g_undo.push_back(std::move(op));
    if (g_undo.size() > 30) g_undo.erase(g_undo.begin());
}

static void EndRename(bool commit) {
    if (g_renameIdx < 0) return;
    int i = g_renameIdx;
    g_renameIdx = -1;
    std::wstring nn = g_rename.text;
    while (!nn.empty() && nn.back() == L' ') nn.pop_back();
    while (!nn.empty() && nn.front() == L' ') nn.erase(0, 1);
    if (commit && i < (int)g_items.size() && !nn.empty() && nn != g_items[i].label) {
        if (g_items[i].label != g_items[i].name) nn += g_items[i].name.substr(g_items[i].label.size());  // devolve o ".lnk"
        if (nn.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) { Flash(Tr(L"Um nome não pode conter \\ / : * ? \" < > |")); }
        else if (MoveFileExW(g_items[i].path.c_str(), Join(Parent(g_items[i].path), nn).c_str(), 0)) {
            PushUndo({ UndoOp::Rename, { { Join(Parent(g_items[i].path), nn), g_items[i].path } } });
            g_selectAfterReload = nn;
            Reload();
        } else Flash(Tr(L"Não foi possível renomear (erro ") + std::to_wstring(GetLastError()) + L")");
    }
    if (!g_searchFocus) KillTimer(g_hwnd, T_CARET);
    Invalidate();
}

// ---------------------------------------------------------------------------
// Desfazer (Ctrl+Z), como no Finder: traz de volta o que foi para a Lixeira (no mesmo lugar) e desfaz
// renomear. Excluir permanente (Shift) não tem volta, como no Mac.

// depois de excluir: os que sumiram foram para a Lixeira
static void RecordTrash(const std::vector<std::wstring>& paths) {
    if (GetKeyState(VK_SHIFT) < 0) return;  // permanente
    UndoOp op{ UndoOp::Trash };
    for (auto& p : paths) if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) op.items.push_back({ p, L"" });
    if (!op.items.empty()) PushUndo(std::move(op));
}

static std::wstring LowerPath(std::wstring s) { for (auto& c : s) c = towlower(c); return s; }

// Procura na Lixeira os itens que vieram de `originals` (o mais recente de cada) e usa o "Restaurar"
// do Windows neles: voltam para a pasta de onde saíram, com o nome de antes.
static int RestoreFromBin(const std::vector<std::wstring>& originals) {
    PROPERTYKEY kFrom{}, kDate{};
    if (FAILED(PSGetPropertyKeyFromName(L"System.Recycle.DeletedFrom", &kFrom)) || FAILED(PSGetPropertyKeyFromName(L"System.Recycle.DateDeleted", &kDate))) return 0;
    std::unordered_map<std::wstring, std::pair<ULONGLONG, PIDLIST_ABSOLUTE>> best;
    std::unordered_set<std::wstring> want;
    for (auto& o : originals) want.insert(LowerPath(o));
    IShellItem* bin = nullptr;
    if (FAILED(SHGetKnownFolderItem(FOLDERID_RecycleBinFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&bin)))) return 0;
    IEnumShellItems* en = nullptr;
    if (SUCCEEDED(bin->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&en)))) {
        IShellItem* it = nullptr;
        while (en->Next(1, &it, nullptr) == S_OK) {
            IShellItem2* i2 = nullptr;
            if (SUCCEEDED(it->QueryInterface(IID_PPV_ARGS(&i2)))) {
                PWSTR from = nullptr, name = nullptr;
                if (SUCCEEDED(i2->GetString(kFrom, &from)) && SUCCEEDED(it->GetDisplayName(SIGDN_PARENTRELATIVEEDITING, &name))) {
                    std::wstring key = LowerPath(Join(from, name));
                    FILETIME ft{}; i2->GetFileTime(kDate, &ft);
                    ULONGLONG t = ((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
                    if (want.count(key)) {
                        auto f = best.find(key);
                        if (f == best.end() || t > f->second.first) {
                            PIDLIST_ABSOLUTE pidl = nullptr;
                            if (SUCCEEDED(SHGetIDListFromObject(it, &pidl))) {
                                if (f != best.end()) CoTaskMemFree(f->second.second);
                                best[key] = { t, pidl };
                            }
                        }
                    }
                }
                if (from) CoTaskMemFree(from);
                if (name) CoTaskMemFree(name);
                i2->Release();
            }
            it->Release();
        }
        en->Release();
    }
    bin->Release();
    if (best.empty()) return 0;
    std::vector<PCIDLIST_ABSOLUTE> ids;
    for (auto& [k, v] : best) ids.push_back(v.second);
    int done = 0;
    IShellItemArray* arr = nullptr;
    if (SUCCEEDED(SHCreateShellItemArrayFromIDLists((UINT)ids.size(), ids.data(), &arr))) {
        IContextMenu* cm = nullptr;
        if (SUCCEEDED(arr->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&cm)))) {
            HMENU m = CreatePopupMenu();
            if (SUCCEEDED(cm->QueryContextMenu(m, 0, 1, 0x7FFF, CMF_NORMAL))) {
                CMINVOKECOMMANDINFOEX ci{ sizeof(ci) };
                ci.fMask = CMIC_MASK_UNICODE | CMIC_MASK_FLAG_NO_UI;
                ci.hwnd = g_hwnd; ci.lpVerb = "undelete"; ci.lpVerbW = L"undelete"; ci.nShow = SW_SHOWNORMAL;
                if (SUCCEEDED(cm->InvokeCommand((CMINVOKECOMMANDINFO*)&ci))) done = (int)ids.size();
            }
            DestroyMenu(m);
            cm->Release();
        }
        arr->Release();
    }
    for (auto& [k, v] : best) CoTaskMemFree(v.second);
    return done;
}

static void Undo() {
    if (g_renameIdx >= 0) return;  // no campo de nome, Ctrl+Z é do texto
    if (g_undo.empty()) { MessageBeep(MB_OK); return; }
    UndoOp op = std::move(g_undo.back());
    g_undo.pop_back();
    if (op.kind == UndoOp::Rename) {
        auto& [now, before] = op.items[0];
        if (MoveFileExW(now.c_str(), before.c_str(), 0)) {
            g_selectAfterReload = before.substr(before.find_last_of(L'\\') + 1);
            Reload();
        } else Flash(Tr(L"Não foi possível desfazer o novo nome"));
        return;
    }
    std::vector<std::wstring> orig;
    for (auto& [o, _] : op.items) orig.push_back(o);
    int n = RestoreFromBin(orig);
    if (n == 0) { Flash(Tr(L"Os itens não estão mais na Lixeira")); return; }
    if (orig.size() == 1 && SamePath(Parent(orig[0]), g_folder)) g_selectAfterReload = orig[0].substr(orig[0].find_last_of(L'\\') + 1);
    Reload();
}

static void NewFolder() {
    std::wstring base = Tr(L"Nova pasta"), name = base;
    for (int k = 2; GetFileAttributesW(Join(g_folder, name).c_str()) != INVALID_FILE_ATTRIBUTES; k++) name = base + L" " + std::to_wstring(k);
    if (!CreateDirectoryW(Join(g_folder, name).c_str(), nullptr)) { Flash(Tr(L"Não foi possível criar a pasta (erro ") + std::to_wstring(GetLastError()) + L")"); return; }
    g_search.SetText(L"", false);
    g_selectAfterReload = name;
    g_renameAfterReload = true;
    Reload();
}

// ---------------------------------------------------------------------------
// Arrastar para fora

static void AttachDragImage(IDataObject* d, const std::wstring& path, int count) {
    IDragSourceHelper* helper = nullptr;
    if (FAILED(CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&helper)))) return;
    IShellItemImageFactory* f = nullptr;
    HBITMAP hb = nullptr;
    int px = (int)(Icon() * g_scale);
    if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&f)))) {
        f->GetImage(SIZE{ px, px }, SIIGBF_RESIZETOFIT, &hb);
        f->Release();
    }
    if (hb) {
        BITMAP bm{}; GetObjectW(hb, sizeof(bm), &bm);
        SHDRAGIMAGE di{};
        di.sizeDragImage = { bm.bmWidth, std::abs(bm.bmHeight) };
        di.ptOffset = { bm.bmWidth / 2, std::abs(bm.bmHeight) / 2 };
        di.hbmpDragImage = hb;  // o helper passa a ser dono do bitmap
        di.crColorKey = CLR_NONE;
        if (FAILED(helper->InitializeFromBitmap(&di, d))) DeleteObject(hb);
    }
    helper->Release();
    (void)count;
}

static void StartDragOut() {
    auto paths = SelectedPaths();
    if (paths.empty()) return;
    IDataObject* d = nullptr;
    if (FAILED(ctxmenu::GetUIObject(g_hwnd, paths, IID_IDataObject, (void**)&d))) return;
    ReleaseCapture();
    g_drag = Drag::None;
    AttachDragImage(d, paths[0], (int)paths.size());
    g_ownDrag = true;
    DWORD eff = 0;
    SHDoDragDrop(g_hwnd, d, nullptr, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &eff);
    g_ownDrag = false;
    d->Release();
    SetDropFeedback(DropSpot{});
}

static void SetTag(const std::vector<std::wstring>& paths, int t, bool on);
static void AddFavorite(const std::wstring& path, int at);

void SpringLoad(const std::wstring& path) {
    Log(L"pasta com mola: %ls", path.c_str());
    Navigate(path, true);
    Invalidate(); UpdateWindow(g_hwnd);
}

DropSpot DropSpotAt(POINT px) {
    DropSpot d;
    D2D1_POINT_2F p = ToDip(px.x, px.y);
    int row = HitSideRow(p);
    if (row >= 0 && !g_side[row].header) {
        const SideEntry& s = g_side[row];
        d.row = row;
        if (s.tag >= 0 && s.tag < tags::COUNT) { d.kind = DropSpot::TAG; d.index = s.tag; return d; }
        if (s.fav >= 0) {  // perto da borda de um favorito: insere ali; no meio: solta dentro da pasta
            float rel = (p.y - SideRowTop(row)) / Lay::SIDE_ROW_H;
            if (rel < 0.28f || rel > 0.72f) { d.kind = DropSpot::SIDEBAR_INSERT; d.index = s.fav + (rel > 0.72f ? 1 : 0); d.row = -1; return d; }
        }
        d.kind = DropSpot::FOLDER;
        d.path = s.path;
        return d;
    }
    if (!In(ContentRect(), p)) return d;
    int i = HitItem(p);
    if (i >= 0 && g_items[i].dir) {
        if (g_ownDrag && g_items[i].sel) return d;  // pasta sobre ela mesma
        d.kind = DropSpot::FOLDER; d.path = g_items[i].path; d.item = i;
        return d;
    }
    if (!g_ownDrag && !g_folder.empty()) { d.kind = DropSpot::FOLDER; d.path = g_folder; }  // na própria pasta de origem não faz nada
    return d;
}

void SetDropFeedback(const DropSpot& s) {
    int insert = s.kind == DropSpot::SIDEBAR_INSERT ? s.index : -1;
    if (s.item != g_dropHi || s.row != g_dropSideRow || insert != g_sideInsert) {
        g_dropHi = s.item; g_dropSideRow = s.row; g_sideInsert = insert;
        Invalidate();
    }
}

void DropCustom(const DropSpot& s, const std::vector<std::wstring>& paths) {
    if (s.kind == DropSpot::SIDEBAR_INSERT) {
        int at = s.index;
        for (auto& p : paths) {
            DWORD a = GetFileAttributesW(p.c_str());
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) AddFavorite(p, at++);
        }
    } else if (s.kind == DropSpot::TAG) {
        SetTag(paths, s.index, true);
    }
}

// ---------------------------------------------------------------------------
// Desenho

struct Palette {
    D2D1_COLOR_F side, content, text, text2, line, hover, selIcon, accent, field, inactiveSel, bar;
};

// side: tinta por cima do acrílico na barra lateral; transparente para o fundo aparecer o máximo possível
static Palette ColorsBase();
static Palette Colors() {  // Tahoe: o fundo da janela leva 10 % da cor do papel de parede (tahoe::Tint)
    Palette p = ColorsBase();
    if (g_tahoe && tahoe::Golden()) p.content = g_dark ? D2D1::ColorF(30 / 255.f, 30 / 255.f, 30 / 255.f, 1) : D2D1::ColorF(1, 1, 1, 1);  // Golden Gate: branco puro (medido)
    else if (g_tahoe) p.content = tahoe::Tint(p.content);
    p.accent = tahoe::Accent(false);  // Ajustes > Aparência > Cor de destaque (seleção, ícones da barra lateral...)
    if (!g_tahoe && tahoe::Leopard()) p.side = leopard::Sidebar(g_winActive, g_dark);  // Leopard: barra lateral azul-acinzentada, opaca
    return p;
}
static Palette ColorsBase() {
    if (g_dark)
        return { D2D1::ColorF(0, 0, 0, 0.22f), D2D1::ColorF(0.118f, 0.118f, 0.125f, 1),
                 D2D1::ColorF(0.96f, 0.96f, 0.97f, 1), D2D1::ColorF(1, 1, 1, 0.50f), D2D1::ColorF(1, 1, 1, 0.10f),
                 D2D1::ColorF(1, 1, 1, 0.08f), D2D1::ColorF(1, 1, 1, 0.12f), D2D1::ColorF(0.04f, 0.42f, 0.92f, 1),
                 D2D1::ColorF(1, 1, 1, 0.08f), D2D1::ColorF(1, 1, 1, 0.22f), D2D1::ColorF(0.188f, 0.196f, 0.196f, 1) };
    return { D2D1::ColorF(0, 0, 0, 0), D2D1::ColorF(1, 1, 1, 1),
             D2D1::ColorF(0.11f, 0.11f, 0.12f, 1), D2D1::ColorF(0, 0, 0, 0.50f), D2D1::ColorF(0, 0, 0, 0.09f),
             D2D1::ColorF(0, 0, 0, 0.06f), D2D1::ColorF(0, 0, 0, 0.09f), D2D1::ColorF(0.0f, 0.39f, 0.88f, 1),
             D2D1::ColorF(0, 0, 0, 0.05f), D2D1::ColorF(0, 0, 0, 0.14f), D2D1::ColorF(0.985f, 0.985f, 0.985f, 1) };
}

static ID2D1StrokeStyle* g_round;
static ID2D1GradientStopCollection* g_folderStops;
static ID2D1LinearGradientBrush* g_folderFront;

static void Line(ID2D1DeviceContext* dc, ID2D1Brush* b, float x1, float y1, float x2, float y2, float w = 1.5f) {
    dc->DrawLine({ x1, y1 }, { x2, y2 }, b, w, g_round);
}

static void DrawGlyph(ID2D1DeviceContext* dc, wchar_t g, D2D1_RECT_F r, ID2D1Brush* b, IDWriteTextFormat* tf = nullptr) {
    dc->DrawTextW(&g, 1, tf ? tf : g_tfGlyph, r, b, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

// Ícones da barra de ferramentas: traço fino e pontas arredondadas, desenhados aqui mesmo.
// Leopard: os glifos pretos cheios da barra do Finder do 10.5 (captura real: ◀ ▶, quadradinhos, linhas, colunas, as
// capas do Cover Flow, o olho do Quick Look e a engrenagem ▾)
static bool DrawToolIconLeopard(ID2D1DeviceContext* dc, int k, D2D1_RECT_F r, ID2D1Brush* b) {
    float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    ID2D1Factory* f = nullptr; dc->GetFactory(&f);
    if (!f) return false;
    auto poly = [&](std::initializer_list<D2D1_POINT_2F> pts) {
        ID2D1PathGeometry* g = nullptr;
        if (SUCCEEDED(f->CreatePathGeometry(&g))) {
            ID2D1GeometrySink* s = nullptr;
            if (SUCCEEDED(g->Open(&s))) {
                auto it = pts.begin();
                s->BeginFigure(*it, D2D1_FIGURE_BEGIN_FILLED);
                for (++it; it != pts.end(); ++it) s->AddLine(*it);
                s->EndFigure(D2D1_FIGURE_END_CLOSED); s->Close(); s->Release();
                dc->FillGeometry(g, b);
            }
            g->Release();
        }
    };
    auto arrow = [&](float ax) { poly({ { ax - 3.5f, cy - 1.8f }, { ax + 3.5f, cy - 1.8f }, { ax, cy + 2.2f } }); };  // ▾ cheio
    bool done = true;
    switch (k) {
    case TB_BACK: poly({ { cx - 4.5f, cy }, { cx + 3.5f, cy - 5.5f }, { cx + 3.5f, cy + 5.5f } }); break;
    case TB_FWD: poly({ { cx + 4.5f, cy }, { cx - 3.5f, cy - 5.5f }, { cx - 3.5f, cy + 5.5f } }); break;
    case TB_V_ICONS: {
        if (r.right - r.left > 40) { done = false; break; }  // recolhido: o desenho comum
        for (int i = 0; i < 4; i++) {
            D2D1_RECT_F q{ cx - 6.5f + (i % 2) * 7.5f, cy - 6.5f + (i / 2) * 7.5f, cx - 6.5f + (i % 2) * 7.5f + 5.5f, cy - 6.5f + (i / 2) * 7.5f + 5.5f };
            dc->DrawRectangle(D2D1::RectF(q.left + 0.75f, q.top + 0.75f, q.right - 0.75f, q.bottom - 0.75f), b, 1.5f);
        }
        break;
    }
    case TB_V_LIST:
        for (int i = 0; i < 4; i++) dc->FillRectangle(D2D1::RectF(cx - 7, cy - 6 + i * 3.6f, cx + 7, cy - 6 + i * 3.6f + 1.5f), b);
        break;
    case TB_V_COLS:
        dc->DrawRectangle(D2D1::RectF(cx - 7.5f, cy - 5.5f, cx + 7.5f, cy + 5.5f), b, 1.6f);
        dc->FillRectangle(D2D1::RectF(cx - 2.8f, cy - 5.5f, cx - 1.6f, cy + 5.5f), b);
        dc->FillRectangle(D2D1::RectF(cx + 1.6f, cy - 5.5f, cx + 2.8f, cy + 5.5f), b);
        break;
    case TB_V_GALLERY:  // Cover Flow: a moldura com as capas em pé
        dc->FillRectangle(D2D1::RectF(cx - 8.5f, cy - 5, cx - 6.5f, cy + 5), b);
        dc->FillRectangle(D2D1::RectF(cx + 6.5f, cy - 5, cx + 8.5f, cy + 5), b);
        for (int i = 0; i < 4; i++) dc->FillRectangle(D2D1::RectF(cx - 4.6f + i * 2.8f, cy - 3.5f, cx - 4.6f + i * 2.8f + 1.5f, cy + 3.5f), b);
        dc->DrawRectangle(D2D1::RectF(cx - 8, cy - 4.5f, cx + 8, cy + 4.5f), b, 1);
        break;
    case TB_GROUP: {  // o olho: amêndoa preta, a íris branca e a pupila
        ID2D1PathGeometry* g = nullptr;
        if (SUCCEEDED(f->CreatePathGeometry(&g))) {
            ID2D1GeometrySink* s = nullptr;
            if (SUCCEEDED(g->Open(&s))) {
                s->BeginFigure({ cx - 9, cy }, D2D1_FIGURE_BEGIN_FILLED);
                s->AddQuadraticBezier(D2D1::QuadraticBezierSegment({ cx, cy - 9 }, { cx + 9, cy }));
                s->AddQuadraticBezier(D2D1::QuadraticBezierSegment({ cx, cy + 9 }, { cx - 9, cy }));
                s->EndFigure(D2D1_FIGURE_END_CLOSED); s->Close(); s->Release();
                dc->FillGeometry(g, b);
            }
            g->Release();
        }
        ID2D1SolidColorBrush* w = nullptr;
        dc->CreateSolidColorBrush(D2D1::ColorF(0.93f, 0.93f, 0.93f), &w);
        if (w) { dc->FillEllipse(D2D1::Ellipse({ cx, cy }, 3.4f, 3.4f), w); w->Release(); }
        dc->FillEllipse(D2D1::Ellipse({ cx, cy }, 1.7f, 1.7f), b);
        break;
    }
    case TB_ACTION: {  // engrenagem preta cheia com o furo, e o ▾
        float gx = cx - 6, gr = 5.3f;
        for (int i = 0; i < 8; i++) {
            float a = i * 3.14159265f / 4;
            dc->DrawLine({ gx + std::cos(a) * gr, cy + std::sin(a) * gr }, { gx + std::cos(a) * (gr + 2.8f), cy + std::sin(a) * (gr + 2.8f) }, b, 2.6f);
        }
        dc->FillEllipse(D2D1::Ellipse({ gx, cy }, gr + 0.4f, gr + 0.4f), b);
        ID2D1SolidColorBrush* w = nullptr;
        dc->CreateSolidColorBrush(D2D1::ColorF(0.90f, 0.90f, 0.90f), &w);
        if (w) { dc->FillEllipse(D2D1::Ellipse({ gx, cy }, 2.1f, 2.1f), w); w->Release(); }
        arrow(r.right - 10);
        break;
    }
    default: done = false;
    }
    f->Release();
    return done;
}

static void DrawToolIcon(ID2D1DeviceContext* dc, int k, D2D1_RECT_F r, ID2D1Brush* b) {
    float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    if (!g_tahoe && tahoe::Leopard() && DrawToolIconLeopard(dc, k, r, b)) return;
    switch (k) {
    case TB_BACK: Line(dc, b, cx + 3, cy - 6, cx - 3, cy, 1.9f); Line(dc, b, cx - 3, cy, cx + 3, cy + 6, 1.9f); break;
    case TB_FWD: Line(dc, b, cx - 3, cy - 6, cx + 3, cy, 1.9f); Line(dc, b, cx + 3, cy, cx - 3, cy + 6, 1.9f); break;
    case TB_V_ICONS: {  // grade 2x2 (com seta quando o seletor está recolhido)
        bool collapsed = r.right - r.left > 40;
        auto arrow = [&] {  // Sequoia: ⌄; Tahoe: ⌃⌄ empilhados (menu de visões, como no macOS 26)
            float ax = r.right - 10;
            if (g_tahoe) {
                Line(dc, b, ax - 2.6f, cy - 1.6f, ax, cy - 4.2f, 1.3f); Line(dc, b, ax, cy - 4.2f, ax + 2.6f, cy - 1.6f, 1.3f);
                Line(dc, b, ax - 2.6f, cy + 1.6f, ax, cy + 4.2f, 1.3f); Line(dc, b, ax, cy + 4.2f, ax + 2.6f, cy + 1.6f, 1.3f);
            } else { Line(dc, b, ax - 3, cy - 1.5f, ax, cy + 1.5f, 1.4f); Line(dc, b, ax, cy + 1.5f, ax + 3, cy - 1.5f, 1.4f); }
        };
        if (collapsed && g_view != View::Icons) {  // recolhido: mostra a visão atual
            int cur = g_view == View::List ? TB_V_LIST : g_view == View::Columns ? TB_V_COLS : TB_V_GALLERY;
            DrawToolIcon(dc, cur, D2D1::RectF(r.left, r.top, r.right - 12, r.bottom), b);
            arrow();
            break;
        }
        float x = collapsed ? r.left + 8 : cx - 7, y = cy - 7;
        for (int i = 0; i < 4; i++) {
            D2D1_RECT_F q{ x + (i % 2) * 8, y + (i / 2) * 8, x + (i % 2) * 8 + 6, y + (i / 2) * 8 + 6 };
            dc->DrawRoundedRectangle(D2D1::RoundedRect(q, 1.5f, 1.5f), b, 1.3f);
        }
        if (collapsed) arrow();
        break;
    }
    case TB_V_LIST:  // três linhas com marcador
        for (int i = -1; i <= 1; i++) {
            dc->FillEllipse(D2D1::Ellipse({ cx - 6.5f, cy + i * 5.f }, 1.2f, 1.2f), b);
            Line(dc, b, cx - 2.5f, cy + i * 5.f, cx + 7.5f, cy + i * 5.f, 1.4f);
        }
        break;
    case TB_V_COLS:  // caixa com três colunas
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 8.5f, cy - 6.5f, cx + 8.5f, cy + 6.5f), 2, 2), b, 1.3f);
        Line(dc, b, cx - 2.8f, cy - 6, cx - 2.8f, cy + 6, 1.2f);
        Line(dc, b, cx + 2.8f, cy - 6, cx + 2.8f, cy + 6, 1.2f);
        break;
    case TB_V_GALLERY:  // imagem grande + faixa de miniaturas
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(cx - 8.5f, cy - 7, cx + 8.5f, cy + 2.5f), 2, 2), b, 1.3f);
        for (int i = 0; i < 4; i++) dc->FillRectangle(D2D1::RectF(cx - 8.5f + i * 4.6f, cy + 5, cx - 8.5f + i * 4.6f + 3.2f, cy + 7.2f), b);
        break;
    case TB_GROUP: {  // traços em cima + grade 3x2 + seta
        float x = r.left + 6.5f, y = cy - 3;
        for (int i = 0; i < 3; i++) Line(dc, b, x + i * 6.6f, cy - 7, x + i * 6.6f + 5.4f, cy - 7, 1.1f);
        for (int i = 0; i < 6; i++) {
            D2D1_RECT_F q{ x + (i % 3) * 6.6f, y + (i / 3) * 5.6f, x + (i % 3) * 6.6f + 5.4f, y + (i / 3) * 5.6f + 4.2f };
            dc->DrawRoundedRectangle(D2D1::RoundedRect(q, 1, 1), b, 1.1f);
        }
        float ax = r.right - 10;
        Line(dc, b, ax - 3, cy - 1.5f, ax, cy + 1.5f, 1.4f); Line(dc, b, ax, cy + 1.5f, ax + 3, cy - 1.5f, 1.4f);
        break;
    }
    case TB_TRASH:  // lixeira: tampa com alça e corpo com frisos
        Line(dc, b, cx - 7.5f, cy - 5.5f, cx + 7.5f, cy - 5.5f, 1.4f);
        Line(dc, b, cx - 2.5f, cy - 5.5f, cx - 2.5f, cy - 8, 1.3f); Line(dc, b, cx - 2.5f, cy - 8, cx + 2.5f, cy - 8, 1.3f);
        Line(dc, b, cx + 2.5f, cy - 8, cx + 2.5f, cy - 5.5f, 1.3f);
        Line(dc, b, cx - 5.5f, cy - 5.5f, cx - 4.5f, cy + 8, 1.4f); Line(dc, b, cx - 4.5f, cy + 8, cx + 4.5f, cy + 8, 1.4f);
        Line(dc, b, cx + 4.5f, cy + 8, cx + 5.5f, cy - 5.5f, 1.4f);
        Line(dc, b, cx - 1.8f, cy - 2.5f, cx - 1.6f, cy + 5, 1.1f); Line(dc, b, cx + 1.8f, cy - 2.5f, cx + 1.6f, cy + 5, 1.1f);
        break;
    case TB_TAG: {  // etiqueta inclinada com furo
        const float c = 0.7071f;
        auto P = [&](float x, float y) { return D2D1::Point2F(cx + (x - y) * c, cy + (x + y) * c); };
        D2D1_POINT_2F pts[5] = { P(-8, 0), P(-3, -5), P(7, -5), P(7, 5), P(-3, 5) };
        for (int i = 0; i < 5; i++) dc->DrawLine(pts[i], pts[(i + 1) % 5], b, 1.4f, g_round);
        dc->DrawEllipse(D2D1::Ellipse(P(-2.5f, 0), 1.3f, 1.3f), b, 1.2f);
        break;
    }
    case TB_SHARE:  // caixa aberta com seta para cima
        Line(dc, b, cx - 3, cy - 2, cx - 6, cy - 2); Line(dc, b, cx - 6, cy - 2, cx - 6, cy + 8);
        Line(dc, b, cx - 6, cy + 8, cx + 6, cy + 8); Line(dc, b, cx + 6, cy + 8, cx + 6, cy - 2);
        Line(dc, b, cx + 6, cy - 2, cx + 3, cy - 2);
        Line(dc, b, cx, cy + 3, cx, cy - 9);
        Line(dc, b, cx - 3.5f, cy - 5.5f, cx, cy - 9); Line(dc, b, cx, cy - 9, cx + 3.5f, cy - 5.5f);
        break;
    case TB_ACTION: {  // círculo com reticências + seta (Leopard: a engrenagem ▾)
        if (!g_tahoe && tahoe::Leopard()) {
            float gx = cx - 6, gr = 5.2f;
            for (int i = 0; i < 8; i++) {  // os dentes
                float a = i * 3.14159265f / 4;
                Line(dc, b, gx + std::cos(a) * gr, cy + std::sin(a) * gr, gx + std::cos(a) * (gr + 2.6f), cy + std::sin(a) * (gr + 2.6f), 2.2f);
            }
            dc->DrawEllipse(D2D1::Ellipse({ gx, cy }, gr - 0.6f, gr - 0.6f), b, 2.0f);
            float ax = r.right - 10;
            Line(dc, b, ax - 3, cy - 1.5f, ax, cy + 1.5f, 1.4f); Line(dc, b, ax, cy + 1.5f, ax + 3, cy - 1.5f, 1.4f);
            break;
        }
        float ox = cx - 7.5f;
        dc->DrawEllipse(D2D1::Ellipse({ ox, cy }, 8.2f, 8.2f), b, 1.4f);
        for (int i = -1; i <= 1; i++) dc->FillEllipse(D2D1::Ellipse({ ox + i * 4.f, cy }, 1.3f, 1.3f), b);
        float ax = r.right - 9;
        Line(dc, b, ax - 3, cy - 1.5f, ax, cy + 1.5f, 1.4f); Line(dc, b, ax, cy + 1.5f, ax + 3, cy - 1.5f, 1.4f);
        break;
    }
    case TB_SEARCH:  // lupa
        dc->DrawEllipse(D2D1::Ellipse({ cx - 1.5f, cy - 1.5f }, 5.5f, 5.5f), b, 1.7f);
        Line(dc, b, cx + 2.6f, cy + 2.6f, cx + 6.5f, cy + 6.5f, 1.9f);
        break;
    }
}

// Pasta própria: aba e fundo em azul forte, frente ciano-claro quase da altura toda,
// borda de baixo mais escura e símbolo gravado (sombra clara por baixo do traço escuro).
// PNG -> BGRA pré-multiplicado (WIC); o PngPixels do info.cpp é interno daquele arquivo
static bool PngPixels(const std::wstring& file, std::vector<BYTE>& out, UINT& w, UINT& h) {
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
    bool ok = false;
    IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* cv = nullptr;
    if (SUCCEEDED(wic->CreateDecoderFromFilename(file.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(wic->CreateFormatConverter(&cv)) &&
        SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
        cv->GetSize(&w, &h);
        out.resize((size_t)w * h * 4);
        ok = SUCCEEDED(cv->CopyPixels(nullptr, w * 4, (UINT)out.size(), out.data()));
    }
    if (cv) cv->Release();
    if (fr) fr->Release();
    if (dec) dec->Release();
    wic->Release();
    return ok;
}


// Tahoe: as pastas são os PNG do tema MacTahoe em icones\tahoe (tools\icones-mactahoe.ps1), um por pasta
// especial (o mesmo símbolo que o desenho do Sequoia grava na pasta). Sem o arquivo, segue o desenho.
// Leopard: desenha icones\leopard\<nome>.png (tools\icones-leopard.py) em `box`. Os PNGs têm 10 % de margem de cada
// lado (o formato do pacote): amplia para o desenho ocupar a caixa. false se o arquivo não existe.
static bool DrawLeoPng(ID2D1DeviceContext* dc, const std::wstring& name, D2D1_RECT_F box) {
    static std::map<std::wstring, ID2D1Bitmap*> cache;
    static std::wstring dir;
    if (dir.empty()) {
        wchar_t exe[MAX_PATH]; ExePath(exe, MAX_PATH);
        PathRemoveFileSpecW(exe); PathRemoveFileSpecW(exe);
        dir = std::wstring(exe) + L"\\icones\\leopard\\";
    }
    auto it = cache.find(name);
    if (it == cache.end()) {
        std::vector<BYTE> px; UINT w = 0, h = 0;
        ID2D1Bitmap* b = PngPixels(dir + name + L".png", px, w, h) ? gfx::CreateBitmapBGRA(w, h, px.data(), w * 4) : nullptr;
        it = cache.emplace(name, b).first;
    }
    if (!it->second) return false;
    float S = box.right - box.left, g = S * 0.125f;
    dc->DrawBitmap(it->second, D2D1::RectF(box.left - g, box.top - g, box.right + g, box.bottom + g), 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    return true;
}
// Leopard: o ícone de documento do 10.5 pela extensão ("" = deixa a miniatura do Windows: fotos, vídeos, apps)
static std::wstring LeoDocIcon(const std::wstring& name) { return leopard::DocIcon(name); }

// Leopard (leo = true): as pastas azuis do 10.5 em icones\leopard (tools\icones-leopard.py), com os mesmos nomes.
static bool DrawTahoeFolder(ID2D1DeviceContext* dc, D2D1_RECT_F box, wchar_t emblem, bool leo = false) {
    static std::map<std::wstring, ID2D1Bitmap*> cacheT, cacheL;  // nome -> bitmap (nullptr: não existe)
    auto& cache = leo ? cacheL : cacheT;
    static std::wstring dirT, dirL;
    if (dirT.empty()) {
        wchar_t exe[MAX_PATH]; ExePath(exe, MAX_PATH);
        PathRemoveFileSpecW(exe); PathRemoveFileSpecW(exe);  // bin\ -> pasta do projeto
        dirT = std::wstring(exe) + L"\\icones\\tahoe\\";
        dirL = std::wstring(exe) + L"\\icones\\leopard\\";
    }
    const std::wstring& dir = leo ? dirL : dirT;
    const wchar_t* name = L"folder default";
    switch (emblem) {
    case 0xE7F4: name = L"folder desktop"; break;
    case 0xE8A5: name = L"folder docs"; break;
    case 0xE896: name = L"folder downloads"; break;
    case 0xE91B: name = L"folder pictures"; break;
    case 0xE8D6: name = L"folder music"; break;
    case 0xE714: name = L"folder videos"; break;
    case 0xE80F: name = L"folder home"; break;
    case 0xE753: name = L"folder cloud"; break;
    }
    auto it = cache.find(name);
    if (it == cache.end()) {
        std::vector<BYTE> px; UINT w = 0, h = 0;
        ID2D1Bitmap* b = PngPixels(dir + name + L".png", px, w, h) ? gfx::CreateBitmapBGRA(w, h, px.data(), w * 4) : nullptr;
        if (!b && wcscmp(name, L"folder default")) {  // pasta especial sem PNG: a padrão
            if (PngPixels(dir + L"folder default.png", px, w, h)) b = gfx::CreateBitmapBGRA(w, h, px.data(), w * 4);
        }
        if (!b) Log(L"pasta do tema: sem %ls%ls.png (fica o desenho do Sequoia)", dir.c_str(), name);
        it = cache.emplace(name, b).first;

    }
    if (!it->second) return false;
    // o desenho do tema tem ~6% de margem em volta: amplia um pouco para ocupar o mesmo espaço da pasta do Sequoia
    // (as do Leopard têm 10 % de margem de cada lado, a margem dos ícones do Mac: amplia mais)
    float S = box.right - box.left, g = S * (leo ? 0.10f : 0.05f);
    dc->DrawBitmap(it->second, D2D1::RectF(box.left - g, box.top - g, box.right + g, box.bottom + g), 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    return true;
}

static void DrawFolder(ID2D1DeviceContext* dc, D2D1_RECT_F box, wchar_t emblem, ID2D1SolidColorBrush* br) {
    if (g_tahoe && DrawTahoeFolder(dc, box, emblem)) return;
    if (!g_tahoe && tahoe::Leopard() && DrawTahoeFolder(dc, box, emblem, true)) return;  // Leopard: as pastas azuis do 10.5
    float S = box.right - box.left, x0 = box.left, y0 = box.top;

    auto R = [&](float a, float b, float c, float d) { return D2D1::RectF(x0 + a * S, y0 + b * S, x0 + c * S, y0 + d * S); };
    float rad = 0.06f * S;
    br->SetColor(D2D1::ColorF(0.06f, 0.49f, 0.78f));  // cores medidas na pasta do Sequoia
    dc->FillRoundedRectangle(D2D1::RoundedRect(R(0.03f, 0.08f, 0.38f, 0.20f), 0.035f * S, 0.035f * S), br);  // aba
    dc->FillRoundedRectangle(D2D1::RoundedRect(R(0.03f, 0.13f, 0.97f, 0.90f), rad, rad), br);                 // fundo
    auto front = R(0.03f, 0.19f, 0.97f, 0.90f);
    if (!g_folderStops) {
        D2D1_GRADIENT_STOP stops[2] = { { 0, D2D1::ColorF(0.32f, 0.78f, 0.98f) }, { 1, D2D1::ColorF(0.14f, 0.63f, 0.87f) } };
        dc->CreateGradientStopCollection(stops, 2, &g_folderStops);
        dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, 0 }, { 0, 1 }), g_folderStops, &g_folderFront);
    }
    g_folderFront->SetStartPoint({ 0, front.top });
    g_folderFront->SetEndPoint({ 0, front.bottom });
    dc->FillRoundedRectangle(D2D1::RoundedRect(front, rad, rad), g_folderFront);
    br->SetColor(D2D1::ColorF(0.18f, 0.55f, 0.76f));  // borda de baixo
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(front.left, front.bottom - std::max(1.5f, 0.035f * S), front.right, front.bottom), rad, rad), br);
    br->SetColor(D2D1::ColorF(1, 1, 1, 0.28f));  // brilho na borda de cima da frente
    dc->DrawLine({ front.left + rad, front.top + 0.6f }, { front.right - rad, front.top + 0.6f }, br, 1.0f);
    if (emblem) {
        float size = std::round(0.40f * S);
        if (!g_tfEmblem || g_tfEmblemSize != size) {
            if (g_tfEmblem) g_tfEmblem->Release();
            IDWriteTextFormat* tf = nullptr;
            gfx::dw->CreateTextFormat(g_iconFamily.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL, size, L"pt-br", &tf);
            tf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            g_tfEmblem = tf;
            g_tfEmblemSize = size;
        }
        auto er = R(0.03f, 0.25f, 0.97f, 0.86f);
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.35f));
        DrawGlyph(dc, emblem, D2D1::RectF(er.left, er.top + 0.9f, er.right, er.bottom + 0.9f), br, g_tfEmblem);
        br->SetColor(D2D1::ColorF(0.05f, 0.42f, 0.72f, 0.9f));
        DrawGlyph(dc, emblem, er, br, g_tfEmblem);
    }
}

// Documento sem miniatura, no estilo do Mac: folha branca com o canto dobrado e, no meio,
// um símbolo do tipo (zíper, linhas de texto, nota musical...) ou a extensão escrita.
static IDWriteTextFormat* g_tfDocExt;
static float g_tfDocExtSize;

static void DrawDocument(ID2D1DeviceContext* dc, D2D1_RECT_F box, const std::wstring& name, ID2D1SolidColorBrush* br) {
    float S = box.right - box.left;
    float pw = 0.74f * S, ph = 0.94f * S, fold = 0.24f * pw, r = std::max(1.f, 0.04f * S);
    float cx = (box.left + box.right) / 2;
    D2D1_RECT_F pg{ cx - pw / 2, box.bottom - ph, cx + pw / 2, box.bottom };

    ID2D1Factory* f = nullptr;
    dc->GetFactory(&f);
    auto page = [&](float dx, float dy) {  // contorno da folha com o canto superior direito cortado
        ID2D1PathGeometry* g = nullptr;
        f->CreatePathGeometry(&g);
        ID2D1GeometrySink* s = nullptr;
        g->Open(&s);
        s->BeginFigure({ pg.left + dx, pg.top + r + dy }, D2D1_FIGURE_BEGIN_FILLED);
        s->AddArc(D2D1::ArcSegment({ pg.left + r + dx, pg.top + dy }, { r, r }, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        s->AddLine({ pg.right - fold + dx, pg.top + dy });
        s->AddLine({ pg.right + dx, pg.top + fold + dy });
        s->AddLine({ pg.right + dx, pg.bottom - r + dy });
        s->AddArc(D2D1::ArcSegment({ pg.right - r + dx, pg.bottom + dy }, { r, r }, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        s->AddLine({ pg.left + r + dx, pg.bottom + dy });
        s->AddArc(D2D1::ArcSegment({ pg.left + dx, pg.bottom - r + dy }, { r, r }, 0, D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
        s->EndFigure(D2D1_FIGURE_END_CLOSED);
        s->Close(); s->Release();
        return g;
    };
    auto* shadow = page(0, 0.8f);
    br->SetColor(D2D1::ColorF(0, 0, 0, 0.22f));
    dc->FillGeometry(shadow, br);
    shadow->Release();
    auto* body = page(0, 0);
    br->SetColor(D2D1::ColorF(0.985f, 0.985f, 0.985f));
    dc->FillGeometry(body, br);
    br->SetColor(D2D1::ColorF(0, 0, 0, 0.18f));
    dc->DrawGeometry(body, br, 0.8f);
    body->Release();
    // canto dobrado
    ID2D1PathGeometry* tri = nullptr;
    f->CreatePathGeometry(&tri);
    ID2D1GeometrySink* s = nullptr;
    tri->Open(&s);
    s->BeginFigure({ pg.right - fold, pg.top }, D2D1_FIGURE_BEGIN_FILLED);
    s->AddLine({ pg.right - fold, pg.top + fold - r });
    s->AddArc(D2D1::ArcSegment({ pg.right - fold + r, pg.top + fold }, { r, r }, 0, D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    s->AddLine({ pg.right, pg.top + fold });
    s->EndFigure(D2D1_FIGURE_END_CLOSED);
    s->Close(); s->Release();
    br->SetColor(D2D1::ColorF(0.86f, 0.86f, 0.87f));
    dc->FillGeometry(tri, br);
    br->SetColor(D2D1::ColorF(0, 0, 0, 0.18f));
    dc->DrawGeometry(tri, br, 0.8f);
    tri->Release();
    f->Release();

    // símbolo do tipo
    size_t dot = name.find_last_of(L'.');
    std::wstring ext = dot == std::wstring::npos ? L"" : Lower(name.substr(dot + 1));
    auto is = [&](std::initializer_list<const wchar_t*> l) { for (auto x : l) if (ext == x) return true; return false; };
    float mx = (pg.left + pg.right) / 2;
    if (is({ L"zip", L"rar", L"7z", L"tar", L"gz", L"bz2", L"xz", L"cab", L"iso" })) {  // zíper
        float zw = 0.16f * pw, zx = mx - zw / 2, step = std::max(2.f, 0.05f * S);
        br->SetColor(D2D1::ColorF(0.55f, 0.55f, 0.57f));
        int k = 0;
        for (float y = pg.top + step * 0.5f; y < pg.top + 0.55f * ph; y += step, k++) {  // dentes alternados
            bool left = (k % 2) == 0;
            dc->FillRectangle(D2D1::RectF(left ? zx : mx, y, left ? mx : zx + zw, y + step * 0.55f), br);
        }
        D2D1_RECT_F pull{ zx - 0.02f * S, pg.top + 0.56f * ph, zx + zw + 0.02f * S, pg.top + 0.72f * ph };
        dc->FillRoundedRectangle(D2D1::RoundedRect(pull, 0.02f * S, 0.02f * S), br);
        br->SetColor(D2D1::ColorF(0.985f, 0.985f, 0.985f));
        dc->FillRectangle(D2D1::RectF(mx - 0.025f * S, pull.top + 0.04f * S, mx + 0.025f * S, pull.bottom - 0.03f * S), br);
        return;
    }
    if (is({ L"txt", L"md", L"log", L"json", L"xml", L"csv", L"ini", L"cfg", L"yaml", L"yml", L"cpp", L"c", L"h", L"hpp",
             L"py", L"js", L"ts", L"html", L"css", L"rtf", L"cmd", L"bat", L"ps1", L"sh", L"toml" })) {  // linhas de texto
        br->SetColor(D2D1::ColorF(0.62f, 0.62f, 0.64f));
        float lx = pg.left + 0.16f * pw, lw = pw * 0.68f, lh = std::max(0.8f, 0.018f * S), gap = 0.065f * S;
        int lines = 0;
        for (float y = pg.top + 0.30f * ph; y < pg.bottom - 0.12f * ph; y += gap, lines++) {
            float w = (lines % 4 == 3) ? lw * 0.55f : lw;
            dc->FillRectangle(D2D1::RectF(lx, y, lx + w, y + lh), br);
        }
        return;
    }
    wchar_t glyph = 0;
    D2D1_COLOR_F gc = D2D1::ColorF(0.45f, 0.45f, 0.48f);
    if (is({ L"wav", L"mp3", L"flac", L"aac", L"m4a", L"ogg", L"aif", L"aiff", L"wma", L"mid", L"midi" })) { glyph = 0xE8D6; gc = D2D1::ColorF(0.93f, 0.27f, 0.45f); }
    else if (is({ L"mp4", L"mov", L"mkv", L"avi", L"webm", L"wmv", L"m4v" })) { glyph = 0xE714; gc = D2D1::ColorF(0.35f, 0.45f, 0.95f); }
    else if (is({ L"png", L"jpg", L"jpeg", L"gif", L"bmp", L"webp", L"heic", L"tif", L"tiff", L"svg", L"ico" })) { glyph = 0xE91B; gc = D2D1::ColorF(0.25f, 0.62f, 0.95f); }
    // extensão escrita (até 4 letras), como o Finder faz com tipos sem ícone próprio
    std::wstring label = ext.empty() ? L"" : ext.substr(0, 4);
    for (auto& ch : label) ch = (wchar_t)towupper(ch);
    if (ext == L"pdf") { glyph = 0; gc = D2D1::ColorF(0.86f, 0.22f, 0.20f); }
    float size = std::round(std::max(6.f, 0.15f * S));
    if (!g_tfDocExt || g_tfDocExtSize != size) {
        if (g_tfDocExt) g_tfDocExt->Release();
        gfx::dw->CreateTextFormat(g_uiFamily.c_str(), nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"pt-br", &g_tfDocExt);
        g_tfDocExt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        g_tfDocExt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        g_tfDocExtSize = size;
    }
    if (glyph) {
        static IDWriteTextFormat* tfGlyph;
        static float tfGlyphSize;
        float gs = std::round(0.34f * S);
        if (!tfGlyph || tfGlyphSize != gs) {
            if (tfGlyph) tfGlyph->Release();
            gfx::dw->CreateTextFormat(g_iconFamily.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, gs, L"pt-br", &tfGlyph);
            tfGlyph->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            tfGlyph->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            tfGlyphSize = gs;
        }
        br->SetColor(gc);
        DrawGlyph(dc, glyph, D2D1::RectF(pg.left, pg.top + 0.15f * ph, pg.right, pg.top + 0.65f * ph), br, tfGlyph);
    }
    if (!label.empty() && S >= 28) {
        br->SetColor(glyph ? D2D1::ColorF(0.50f, 0.50f, 0.52f) : gc);
        D2D1_RECT_F lr = glyph ? D2D1::RectF(pg.left, pg.top + 0.66f * ph, pg.right, pg.top + 0.86f * ph)
                               : D2D1::RectF(pg.left, pg.top + 0.45f * ph, pg.right, pg.top + 0.75f * ph);
        dc->DrawTextW(label.c_str(), (UINT32)label.size(), g_tfDocExt, lr, br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}

static void DrawLights(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br) {
    // cores do macOS medidas na referência: preenchimento e aro de 0,5 pt um tom abaixo
    static const D2D1_COLOR_F fill_c[3] = { D2D1::ColorF(1.0f, 0.373f, 0.345f), D2D1::ColorF(0.996f, 0.737f, 0.180f), D2D1::ColorF(0.157f, 0.784f, 0.251f) }; const D2D1_COLOR_F* fill = semaforo::Pick(fill_c, semaforo::FILL);
    static const D2D1_COLOR_F ring_c[3] = { D2D1::ColorF(0.886f, 0.275f, 0.247f), D2D1::ColorF(0.882f, 0.631f, 0.086f), D2D1::ColorF(0.071f, 0.675f, 0.157f) }; const D2D1_COLOR_F* ring = semaforo::Pick(ring_c, semaforo::RING);
    static const D2D1_COLOR_F glyph_c[3] = { D2D1::ColorF(0.302f, 0.0f, 0.0f), D2D1::ColorF(0.600f, 0.341f, 0.0f), D2D1::ColorF(0.0f, 0.396f, 0.0f) }; const D2D1_COLOR_F* glyph = semaforo::Pick(glyph_c, semaforo::GLYPH);
    bool lit = g_winActive || g_hoverLights;
    ID2D1Factory* f = nullptr; dc->GetFactory(&f);
    for (int k = 0; k < 3; k++) {
        auto c = LightCenter(k);
        if (lit) br->SetColor(fill[k]);
        else br->SetColor(g_dark ? D2D1::ColorF(0.24f, 0.24f, 0.25f) : D2D1::ColorF(0.839f, 0.839f, 0.839f));  // inativa: cinza
        dc->FillEllipse(D2D1::Ellipse(c, 7, 7), br);
        semaforo::Gloss(dc, c, 7, br);
        if (lit) br->SetColor(ring[k]);
        else br->SetColor(g_dark ? D2D1::ColorF(0, 0, 0, 0.30f) : D2D1::ColorF(0.729f, 0.729f, 0.737f));
        dc->DrawEllipse(D2D1::Ellipse(c, 6.75f, 6.75f), br, 0.5f);
        if (!g_hoverLights) continue;
        br->SetColor(glyph[k]);
        if (k == 0) {  // ×
            float a = 2.5f;
            Line(dc, br, c.x - a, c.y - a, c.x + a, c.y + a, 1.1f);
            Line(dc, br, c.x - a, c.y + a, c.x + a, c.y - a, 1.1f);
        } else if (k == 1) {  // −
            Line(dc, br, c.x - 3.3f, c.y, c.x + 3.3f, c.y, 1.2f);
        } else if (f) {  // tela cheia: dois triângulos nos cantos opostos, como no Sequoia
            for (int t = 0; t < 2; t++) {
                float sgn = t ? 1.f : -1.f, a = 3.1f, b = 1.6f;
                D2D1_POINT_2F pts[3] = { { c.x + sgn * a, c.y + sgn * a }, { c.x - sgn * b, c.y + sgn * a }, { c.x + sgn * a, c.y - sgn * b } };
                ID2D1PathGeometry* g = nullptr; ID2D1GeometrySink* sk = nullptr;
                if (SUCCEEDED(f->CreatePathGeometry(&g)) && SUCCEEDED(g->Open(&sk))) {
                    sk->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_FILLED);
                    sk->AddLine(pts[1]); sk->AddLine(pts[2]);
                    sk->EndFigure(D2D1_FIGURE_END_CLOSED);
                    sk->Close(); sk->Release();
                    dc->FillGeometry(g, br);
                }
                if (g) g->Release();
            }
        }
    }
    if (f) f->Release();
}

// Tahoe: painel de vidro da barra lateral, solto das bordas e com o raio concêntrico ao da janela
static float SidePanelR() { return std::max(10.f, tahoe::Radius(g_hwnd) - 7); }
// macOS 27 Golden Gate (medido no Finder das capturas do 512 Pixels): a barra lateral volta a ir até a borda, uma coluna
// de cor cheia (237,237,236) que encosta direto no conteúdo branco, sem divisa, sem painel de vidro e sem sombra
static bool Golden() { return g_tahoe && tahoe::Golden(); }
static D2D1_RECT_F SidePanelRect() {
    if (Golden()) return { 0, 0, std::floor(Lay::SIDEBAR_W), ClientDip().height };
    float m = IsZoomed(g_hwnd) ? 6.f : 8.f;  // Tahoe: painel flutuante com 8 de recuo (guia, seção 42)
    return { m, m, std::floor(Lay::SIDEBAR_W) - 1, std::floor(ClientDip().height - m) };  // bordas no grid de pixels

}

static float g_sideClipTop = Lay::SIDE_TOP;  // Tahoe (cena): a lista rola até o alto do painel, por baixo dos botões
static void DrawSidebar(ID2D1DeviceContext* dc, const Palette& pal, ID2D1SolidColorBrush* br) {
    const float dx = g_tahoe ? 4.f : 0.f;  // Tahoe: conteúdo um pouco para dentro do painel
    dc->PushAxisAlignedClip(D2D1::RectF(0, g_sideClipTop, Lay::SIDEBAR_W, ClientDip().height - (g_tahoe && !Golden() ? 9 : 0)), D2D1_ANTIALIAS_MODE_ALIASED);
    struct PopClip { ID2D1DeviceContext* d; ~PopClip() { d->PopAxisAlignedClip(); } } pop{ dc };
    if (g_sideInsert >= 0) {  // linha azul onde o favorito vai entrar
        float y = -1;
        for (int k = 0; k < (int)g_side.size(); k++) {
            if (g_side[k].fav < 0 || g_side[k].hidden) continue;
            if (g_side[k].fav == g_sideInsert) { y = SideRowTop(k); break; }
            y = SideRowTop(k) + Lay::SIDE_ROW_H;  // depois do último
        }
        if (y >= 0) {
            br->SetColor(pal.accent);
            dc->DrawLine({ 22, y }, { Lay::SIDEBAR_W - 12, y }, br, 2);
            dc->DrawEllipse(D2D1::Ellipse({ 18, y }, 3.5f, 3.5f), br, 1.6f);
        }
    }
    for (int k = 0; k < (int)g_side.size(); k++) {
        const SideEntry& s = g_side[k];
        if (s.hidden) continue;
        float y = SideRowTop(k);
        if (s.header) {
            br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.32f) : D2D1::ColorF(0, 0, 0, 0.30f));
            if (!g_tahoe && tahoe::Leopard()) {  // Leopard: "DISPOSITIVOS", "LOCAIS" em maiúsculas, negrito azul-acinzentado
                std::wstring up = s.label;
                for (auto& ch : up) ch = towupper(ch);
                br->SetColor(D2D1::ColorF(96 / 255.f, 110 / 255.f, 128 / 255.f, 1));
                dc->DrawTextW(up.c_str(), (UINT32)up.size(), g_tfSideHdr, D2D1::RectF(21 + dx, y + 8 + SideHeaderGap(k), Lay::SIDEBAR_W - 30, y + SideRowH(k)), br);
                float tcx = 13 + dx, tcy = y + 14 + SideHeaderGap(k);
                ID2D1Factory* fac = nullptr; dc->GetFactory(&fac);
                ID2D1PathGeometry* tri = nullptr;
                if (fac && SUCCEEDED(fac->CreatePathGeometry(&tri))) {
                    ID2D1GeometrySink* sk = nullptr;
                    if (SUCCEEDED(tri->Open(&sk))) {
                        if (s.collapsed) { sk->BeginFigure({ tcx - 2, tcy - 4 }, D2D1_FIGURE_BEGIN_FILLED); sk->AddLine({ tcx + 3, tcy }); sk->AddLine({ tcx - 2, tcy + 4 }); }
                        else { sk->BeginFigure({ tcx - 4, tcy - 2 }, D2D1_FIGURE_BEGIN_FILLED); sk->AddLine({ tcx + 4, tcy - 2 }); sk->AddLine({ tcx, tcy + 3 }); }
                        sk->EndFigure(D2D1_FIGURE_END_CLOSED); sk->Close(); sk->Release();
                        br->SetColor(D2D1::ColorF(0.42f, 0.45f, 0.50f, 1));
                        dc->FillGeometry(tri, br);
                    }
                    tri->Release();
                }
                if (fac) fac->Release();
                continue;  // Leopard: sem a seta da direita (o triângulo é este)
            } else
            dc->DrawTextW(s.label.c_str(), (UINT32)s.label.size(), g_tfSideHdr, D2D1::RectF(16 + dx, y + 8 + SideHeaderGap(k), Lay::SIDEBAR_W - 30, y + SideRowH(k)), br);
            if (k == g_hoverSide) {  // seta para recolher/expandir, só ao passar o mouse
                float cx = Lay::SIDEBAR_W - 20, cy = y + 14 + SideHeaderGap(k);
                if (s.collapsed) { Line(dc, br, cx - 1.5f, cy - 3.5f, cx + 2, cy, 1.4f); Line(dc, br, cx + 2, cy, cx - 1.5f, cy + 3.5f, 1.4f); }
                else { Line(dc, br, cx - 3.5f, cy - 1.5f, cx, cy + 2, 1.4f); Line(dc, br, cx, cy + 2, cx + 3.5f, cy - 1.5f, 1.4f); }
            }
            continue;
        }
        D2D1_RECT_F row = g_tahoe ? D2D1_RECT_F{ 13.5f, y + 1.2f, Lay::SIDEBAR_W - 7.5f, y + Lay::SIDE_ROW_H - 1.2f }
                                  : D2D1_RECT_F{ 9.5f, y + 1.2f, Lay::SIDEBAR_W - 11, y + Lay::SIDE_ROW_H - 1.2f };
        const float rowR = g_tahoe ? 8.f : 6.f;
        bool cur = (!s.path.empty() && g_tagView < 0 && SamePath(s.path, g_folder)) || (s.tag >= 0 && s.tag == g_tagView);
        const bool leoSide = !g_tahoe && tahoe::Leopard();
        if (leoSide && cur) {
            D2D1_GRADIENT_STOP st[2] = { { 0, g_winActive ? D2D1::ColorF(0.43f, 0.60f, 0.85f, 1) : D2D1::ColorF(0.68f, 0.68f, 0.70f, 1) },
                                         { 1, g_winActive ? D2D1::ColorF(0.24f, 0.44f, 0.76f, 1) : D2D1::ColorF(0.53f, 0.53f, 0.55f, 1) } };
            ID2D1GradientStopCollection* gs = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
            D2D1_RECT_F full = D2D1::RectF(0, row.top, Lay::SIDEBAR_W, row.bottom);
            if (SUCCEEDED(dc->CreateGradientStopCollection(st, 2, &gs)) &&
                SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, full.top }, { 0, full.bottom }), gs, &lb)))
                dc->FillRectangle(full, lb);
            if (lb) lb->Release();
            if (gs) gs->Release();
            br->SetColor(g_winActive ? D2D1::ColorF(0.33f, 0.52f, 0.80f, 1) : D2D1::ColorF(0.60f, 0.60f, 0.62f, 1));  // o fio de cima
            dc->FillRectangle(D2D1::RectF(0, full.top, Lay::SIDEBAR_W, full.top + 1), br);
        } else
        if (cur || k == g_hoverSide) {
            br->SetColor(cur ? (g_tahoe ? (g_dark ? D2D1::ColorF(1, 1, 1, 0.09f) : D2D1::ColorF(0, 0, 0, 0.07f))
                                        : (g_dark ? D2D1::ColorF(1, 1, 1, 0.14f) : D2D1::ColorF(0, 0, 0, 0.10f))) : pal.hover);
            dc->FillRoundedRectangle(D2D1::RoundedRect(row, rowR, rowR), br);
        }
        if (k == g_dropSideRow) {  // vai receber o que está sendo arrastado
            br->SetColor(pal.accent);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(row, rowR, rowR), br, 2);
        }
        if (g_sideDragging && k == g_sidePress) {  // o favorito que está sendo movido fica apagado
            br->SetColor(D2D1::ColorF(pal.hover.r, pal.hover.g, pal.hover.b, pal.hover.a * 1.5f));
            dc->FillRoundedRectangle(D2D1::RoundedRect(row, rowR, rowR), br);
        }
        if (s.tagColor) {  // etiqueta: bolinha colorida
            UINT32 c = s.tagColor;
            br->SetColor(D2D1::ColorF(((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f));
            dc->FillEllipse(D2D1::Ellipse({ 24.7f + dx, y + Lay::SIDE_ROW_H / 2 }, 5, 5), br);
        } else {
            // os ícones da barra lateral na cor de destaque (Ajustes > Aparência), mais clara no escuro, como no Mac
            br->SetColor(tahoe::Accent(g_dark));
            // Leopard: as pastinhas azuis coloridas do 10.5 (16 px); o resto em cinza-azulado
            bool pasta = s.glyph == 0xE7F4 || s.glyph == 0xE8A5 || s.glyph == 0xE896 || s.glyph == 0xE91B || s.glyph == 0xE8D6 ||
                         s.glyph == 0xE714 || s.glyph == 0xE80F || s.glyph == 0xE8B7;
            float cy = y + Lay::SIDE_ROW_H / 2;
            const wchar_t* leoIcon = nullptr;  // Leopard: o ícone colorido do item, como na barra lateral do 10.5
            if (leoSide) {
                if (s.tag == VIEW_COMPUTER) leoIcon = L"side computador";
                else if (s.tag >= VIEW_SMART && s.tag < VIEW_SMART + 6) leoIcon = s.tag < VIEW_SMART + 3 ? L"side recentes" : L"side inteligente";
                else switch (s.glyph) {
                    case 0xE823: leoIcon = L"side recentes"; break;   case 0xE71D: leoIcon = L"side aplicativos"; break;
                    case 0xE7F4: leoIcon = L"side mesa"; break;       case 0xE8A5: leoIcon = L"side documentos"; break;
                    case 0xE896: leoIcon = L"side downloads"; break;  case 0xE91B: leoIcon = L"side imagens"; break;
                    case 0xE8D6: leoIcon = L"side musica"; break;     case 0xE714: leoIcon = L"side videos"; break;
                    case 0xE80F: leoIcon = L"side casa"; break;       case 0xE753: leoIcon = L"side rede"; break;
                    case 0xEDA2: leoIcon = L"side disco"; break;      case 0xE88E: leoIcon = L"side removivel"; break;
                    default: leoIcon = L"side pasta"; break;
                }
            }
            if (leoIcon && DrawLeoPng(dc, leoIcon, D2D1::RectF(16.7f + dx, cy - 8, 32.7f + dx, cy + 8))) {}
            else if (leoSide && pasta) DrawFolder(dc, D2D1::RectF(16.7f + dx, cy - 8, 32.7f + dx, cy + 8), s.glyph == 0xE8B7 ? 0 : s.glyph, br);
            else {
                if (leoSide) br->SetColor(cur ? D2D1::ColorF(1, 1, 1, 1) : D2D1::ColorF(96 / 255.f, 110 / 255.f, 128 / 255.f, 1));
                DrawGlyph(dc, s.glyph, D2D1::RectF(15.7f + dx, y, 33.7f + dx, y + Lay::SIDE_ROW_H), br);
            }
        }
        br->SetColor(g_tahoe && cur ? tahoe::Accent(g_dark) : leoSide && cur ? D2D1::ColorF(1, 1, 1, 1) : pal.text);
        dc->DrawTextW(s.label.c_str(), (UINT32)s.label.size(), g_tfSide, D2D1::RectF(40 + dx
, y, Lay::SIDEBAR_W - 12, y + Lay::SIDE_ROW_H), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);

    }
}

static float TextWidth(const std::wstring& s, IDWriteTextFormat* tf);  // mais abaixo
static void DrawToolbar(ID2D1DeviceContext* dc, const Palette& pal, ID2D1SolidColorBrush* br) {
    D2D1_COLOR_F dim = g_dark ? D2D1::ColorF(1, 1, 1, 0.16f) : D2D1::ColorF(0, 0, 0, 0.18f);
    D2D1_COLOR_F icon = g_dark ? D2D1::ColorF(1, 1, 1, 0.78f) : D2D1::ColorF(0, 0, 0, 0.68f);
    int selCount = SelCount();
    const bool leo = !g_tahoe && tahoe::Leopard();
    if (leo) {
        // Leopard: botões com moldura de metal claro (degradê branco -> cinza, contorno escuro e fio branco embaixo);
        // Voltar/Avançar e os quatro modos são segmentados (divisórias finas) e o modo atual fica afundado em cinza
        auto U = [](D2D1_RECT_F a, D2D1_RECT_F b) { return D2D1_RECT_F{ std::min(a.left, b.left), 0, std::max(a.right, b.right), 0 }; };
        auto bezel = [&](D2D1_RECT_F r) {
            if (r.right <= r.left) return;
            r.top = Lay::TOOL_CY - 11; r.bottom = Lay::TOOL_CY + 11;
            br->SetColor(D2D1::ColorF(1, 1, 1, 0.45f));  // o fio branco de baixo (relevo)
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left, r.top + 1, r.right, r.bottom + 1), 4, 4), br);
            D2D1_GRADIENT_STOP st[2] = { { 0, D2D1::ColorF(0.99f, 0.99f, 0.99f, 1) }, { 1, D2D1::ColorF(0.84f, 0.84f, 0.84f, 1) } };
            ID2D1GradientStopCollection* gs = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
            if (SUCCEEDED(dc->CreateGradientStopCollection(st, 2, &gs)) &&
                SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, r.top }, { 0, r.bottom }), gs, &lb)))
                dc->FillRoundedRectangle(D2D1::RoundedRect(r, 4, 4), lb);
            if (lb) lb->Release();
            if (gs) gs->Release();
            br->SetColor(D2D1::ColorF(0, 0, 0, 0.42f));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), 3.5f, 3.5f), br, 1);
        };
        auto divide = [&](D2D1_RECT_F a) {  // divisória entre dois segmentos (à direita de a)
            br->SetColor(D2D1::ColorF(0, 0, 0, 0.30f));
            dc->DrawLine({ std::floor(a.right) + 0.5f, Lay::TOOL_CY - 10.5f }, { std::floor(a.right) + 0.5f, Lay::TOOL_CY + 10.5f }, br, 1);
        };
        bezel(U(ToolRect(TB_BACK), ToolRect(TB_FWD))); divide(ToolRect(TB_BACK));
        if (WideToolbar()) {
            bezel(U(ToolRect(TB_V_ICONS), ToolRect(TB_V_GALLERY)));
            int cur = g_view == View::Icons ? 0 : g_view == View::List ? (g_coverFlow ? 3 : 1) : g_view == View::Columns ? 2 : 3;
            D2D1_RECT_F c = ToolRect(TB_V_ICONS + cur);
            if (c.right > c.left) {  // o modo atual: afundado (cinza escuro com sombra por dentro)
                D2D1_RECT_F q{ c.left + 0.5f, Lay::TOOL_CY - 10.5f, c.right - 0.5f, Lay::TOOL_CY + 10.5f };
                D2D1_GRADIENT_STOP st[2] = { { 0, D2D1::ColorF(0.42f, 0.42f, 0.42f, 1) }, { 1, D2D1::ColorF(0.56f, 0.56f, 0.56f, 1) } };
                ID2D1GradientStopCollection* gs = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
                if (SUCCEEDED(dc->CreateGradientStopCollection(st, 2, &gs)) &&
                    SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, q.top }, { 0, q.bottom }), gs, &lb)))
                    dc->FillRoundedRectangle(D2D1::RoundedRect(q, cur == 0 || cur == 3 ? 3.f : 0.f, cur == 0 || cur == 3 ? 3.f : 0.f), lb);
                if (lb) lb->Release();
                if (gs) gs->Release();
            }
            for (int i = 0; i < 3; i++) divide(ToolRect(TB_V_ICONS + i));
        } else bezel(ToolRect(TB_V_ICONS));
        bezel(ToolRect(TB_GROUP));
        bezel(ToolRect(TB_SHARE)); bezel(ToolRect(TB_TAG)); bezel(ToolRect(TB_ACTION));
    }
    if (g_tahoe) {
        // Tahoe: os botões ficam em cápsulas de vidro soltas sobre o conteúdo
        auto U = [](D2D1_RECT_F a, D2D1_RECT_F b) { return D2D1_RECT_F{ std::min(a.left, b.left), 0, std::max(a.right, b.right), 0 }; };
        auto cap = [&](D2D1_RECT_F r, float padX, float half = 18) {  // ilhas de 36 de altura, raio 18
            if (r.right <= r.left) return;
            r.left -= padX; r.right += padX;
            r.top = Lay::TOOL_CY - half; r.bottom = Lay::TOOL_CY + half;
            tahoe::Capsule(dc, br, r, -1, g_dark);
            // ponteiro em cima: o brilho especular acende e a luz vem do lado dele (guia, seções 3.2 e 29.1)
            if (g_hoverTool >= 0 && g_toolMouse.x >= r.left && g_toolMouse.x < r.right && g_toolMouse.y >= r.top && g_toolMouse.y < r.bottom) {
                float lpx = g_toolLuz.x >= 0 ? g_toolLuz.x : g_toolMouse.x, lpy = g_toolLuz.y >= 0 ? g_toolLuz.y : g_toolMouse.y;
                float lx = std::clamp((lpx - r.left) / (r.right - r.left), 0.f, 1.f), ly = std::clamp((lpy - r.top) / (r.bottom - r.top), 0.f, 1.f);
                vidro::Borda(dc, r, (r.bottom - r.top) / 2, 1, g_dark, g_pressTool >= 0 ? 1.3f : 1.f, true, 1, lx, ly);
            }
        };
        {   // a luz anda atrás do ponteiro (um passo por quadro; enquanto anda, pede outro quadro)
            DWORD now = GetTickCount();
            if (g_toolLuz.Passo(g_hoverTool >= 0 ? g_toolMouse.x : -1, g_hoverTool >= 0 ? g_toolMouse.y : -1, (now - g_toolLuzAt) / 1000.f)) Invalidate();
            g_toolLuzAt = now;
        }
        cap(U(ToolRect(TB_BACK), ToolRect(TB_FWD)), 0);
        if (WideToolbar()) cap(U(ToolRect(TB_V_ICONS), ToolRect(TB_V_GALLERY)), Golden() ? 4.5f : 3);
        else cap(ToolRect(TB_V_ICONS), 0);
        cap(ToolRect(TB_GROUP), 0);
        cap(U(ToolRect(TB_SHARE), ToolRect(TB_ACTION)), 0);  // Compartilhar, Etiquetas e ⋯ juntos (Tahoe)
        if (!SearchExpanded()) cap(ToolRect(TB_SEARCH), 0, Golden() ? 18.f : 19.f);  // busca recolhida: cápsula de 39 × 38 (macOS 27: círculo de 36)
    }
    for (int k = 0; k < TB_COUNT; k++) {
        if (k == TB_SEARCH && SearchExpanded()) continue;
        auto r = ToolRect(k);
        if (r.right <= r.left) continue;  // segmento escondido (janela estreita)
        bool enabled = k == TB_BACK ? (g_searching || !g_back.empty()) : k == TB_FWD ? !g_fwd.empty()
                     : (k == TB_SHARE || k == TB_TRASH || k == TB_TAG) ? selCount > 0 : true;  // pedem seleção
        bool current = WideToolbar() && ((k == TB_V_ICONS && g_view == View::Icons) || (k == TB_V_LIST && g_view == View::List) ||
                                         (k == TB_V_COLS && g_view == View::Columns) || (k == TB_V_GALLERY && g_view == View::Gallery));  // visão atual destacada
        if (leo) current = WideToolbar() && ((k == TB_V_ICONS && g_view == View::Icons) || (k == TB_V_LIST && g_view == View::List && !g_coverFlow) ||
                                             (k == TB_V_COLS && g_view == View::Columns) || (k == TB_V_GALLERY && (g_view == View::Gallery || g_coverFlow)));
        if (leo) {  // Leopard: a moldura já foi desenhada; o ícone escuro (branco no modo atual), apertado escurece
            if (enabled && g_pressTool == k) { br->SetColor(D2D1::ColorF(0, 0, 0, 0.18f)); dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 1, Lay::TOOL_CY - 10, r.right - 1, Lay::TOOL_CY + 10), 3, 3), br); }
            br->SetColor(current ? D2D1::ColorF(1, 1, 1, 0.95f) : enabled ? D2D1::ColorF(0.05f, 0.05f, 0.05f, 0.95f) : D2D1::ColorF(0, 0, 0, 0.30f));
            DrawToolIcon(dc, k, r, br);
            continue;
        }
        if (current || (enabled && g_hoverTool == k)) {
            float a = current ? (g_dark ? 0.06f : 0.06f) : pal.hover.a * (g_pressTool == k ? 2 : 1);
            if (g_tahoe && current) a = g_dark ? 0.14f : 0.08f;  // segmento escolhido: pílula dentro da cápsula
            if (Golden() && current) a = g_dark ? 0.16f : 0.075f;  // macOS 27: pílula (230,230,230) na cápsula clara
            br->SetColor(D2D1::ColorF(pal.hover.r, pal.hover.g, pal.hover.b, a));
            float rr = g_tahoe ? (r.bottom - r.top) / 2 : 6;
            dc->FillRoundedRectangle(D2D1::RoundedRect(r, rr, rr), br);
        }
        br->SetColor(enabled ? icon : dim);
        DrawToolIcon(dc, k, r, br);
    }
    if (WideToolbar() && (!g_tahoe || Golden()) && !leo) {  // divisória fina entre dois botões de visão quando nenhum dos dois é o atual
        int cur = g_view == View::Icons ? 0 : g_view == View::List ? 1 : g_view == View::Columns ? 2 : 3;
        br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.07f) : D2D1::ColorF(0, 0, 0, 0.10f));
        for (int i = 0; i < 3; i++) {
            if (i == cur || i + 1 == cur) continue;
            auto a = ToolRect(TB_V_ICONS + i), b = ToolRect(TB_V_ICONS + i + 1);
            if (a.right <= a.left || b.right <= b.left) continue;
            float x = std::floor((a.right + b.left) / 2) + 0.5f;
            dc->DrawLine({ x, Lay::TOOL_CY - 8 }, { x, Lay::TOOL_CY + 8 }, br, 1);
        }
    }
    if (leo) {  // Leopard: a pastinha e o nome centralizados na linha do título (22 px), com relevo branco
        std::wstring title = CurrentTitle();
        auto s = ClientDip();
        float tw = std::min(s.width - 200, TextWidth(title, g_tfTitle));
        float x0 = (s.width - tw - 20) / 2;
        DrawFolder(dc, D2D1::RectF(x0, 3.5f, x0 + 15, 18.5f), 0, br);
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.5f));
        dc->DrawTextW(title.c_str(), (UINT32)title.size(), g_tfTitle, D2D1::RectF(x0 + 20, 1, x0 + 20 + tw + 4, 23), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        br->SetColor(D2D1::ColorF(0, 0, 0, g_winActive ? 0.85f : 0.45f));
        dc->DrawTextW(title.c_str(), (UINT32)title.size(), g_tfTitle, D2D1::RectF(x0 + 20, 0, x0 + 20 + tw + 4, 22), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        {   // a pílula oval de mostrar/ocultar a barra de ferramentas no canto direito do título (captura do 10.5)
            D2D1_RECT_F pr = D2D1::RectF(s.width - 31, 6, s.width - 9, 16);
            D2D1_GRADIENT_STOP st[3] = { { 0, D2D1::ColorF(0.99f, 0.99f, 0.99f) }, { 0.5f, D2D1::ColorF(0.86f, 0.86f, 0.86f) }, { 1, D2D1::ColorF(0.97f, 0.97f, 0.97f) } };
            ID2D1GradientStopCollection* gs = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
            if (SUCCEEDED(dc->CreateGradientStopCollection(st, 3, &gs)) &&
                SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, pr.top }, { 0, pr.bottom }), gs, &lb)))
                dc->FillRoundedRectangle(D2D1::RoundedRect(pr, 5, 5), lb);
            if (lb) lb->Release();
            if (gs) gs->Release();
            br->SetColor(D2D1::ColorF(0, 0, 0, 0.45f));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(pr.left + 0.5f, pr.top + 0.5f, pr.right - 0.5f, pr.bottom - 0.5f), 4.5f, 4.5f), br, 1);
        }
    } else {
    // título; a pastinha só aparece com o mouse em cima (como no Sequoia) e empurra o nome
    float tx = ToolRect(TB_FWD).right + 12;
    if (g_hoverTitle) DrawFolder(dc, D2D1::RectF(tx, Lay::TOOL_CY - 9, tx + 18, Lay::TOOL_CY + 9), 0, br);
    std::wstring title = CurrentTitle();
    float titleRight = ToolRect(TB_V_ICONS).left - 12;
    br->SetColor(pal.text);
    dc->DrawTextW(title.c_str(), (UINT32)title.size(), g_tfTitle, D2D1::RectF(tx + (g_hoverTitle ? 25 : 0), Lay::TOOL_CY - 13, titleRight, Lay::TOOL_CY + 13), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    if (!SearchExpanded()) return;
    auto sr = ToolRect(TB_SEARCH);  // campo de busca aberto
    if (g_tahoe) {
        D2D1_RECT_F cr{ sr.left, Lay::TOOL_CY - 18, sr.right, Lay::TOOL_CY + 18 };
        tahoe::Capsule(dc, br, cr, -1, g_dark);
        if (g_searchFocus) {
            br->SetColor(D2D1::ColorF(pal.accent.r, pal.accent.g, pal.accent.b, 0.55f));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(Inflate(cr, 1.5f), 19.5f, 19.5f), br, 3);
        }
        sr.left += 4;
    } else if (leo) {
        // Leopard: a pílula branca afundada (sombra por dentro em cima e o contorno cinza), anel azul com o foco
        float rr = (sr.bottom - sr.top) / 2;
        if (g_searchFocus) {
            br->SetColor(D2D1::ColorF(0.36f, 0.60f, 0.95f, 0.75f));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(Inflate(sr, 1.5f), rr + 1.5f, rr + 1.5f), br, 3);
        }
        br->SetColor(D2D1::ColorF(1, 1, 1, 1));
        dc->FillRoundedRectangle(D2D1::RoundedRect(sr, rr, rr), br);
        br->SetColor(D2D1::ColorF(0, 0, 0, 0.10f));
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(sr.left + 2, sr.top + 1, sr.right - 2, sr.top + 3), 1, 1), br);
        br->SetColor(D2D1::ColorF(0, 0, 0, 0.45f));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(sr.left + 0.5f, sr.top + 0.5f, sr.right - 0.5f, sr.bottom - 0.5f), rr - 0.5f, rr - 0.5f), br, 1);
    } else {
    br->SetColor(pal.field);
    dc->FillRoundedRectangle(D2D1::RoundedRect(sr, 7, 7), br);
    }
    if (g_searchFocus && !g_tahoe && !leo) {

        br->SetColor(D2D1::ColorF(pal.accent.r, pal.accent.g, pal.accent.b, 0.55f));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(Inflate(sr, 1.5f), 8.5f, 8.5f), br, 3);
    }
    br->SetColor(pal.text2);
    DrawToolIcon(dc, TB_SEARCH, D2D1::RectF(sr.left + 3, sr.top, sr.left + 27, sr.bottom), br);
    D2D1_RECT_F tr{ sr.left + 28, sr.top, sr.right - 8, sr.bottom };
    if (g_search.text.empty() && !g_searchFocus) return;
    if (g_search.text.empty()) dc->DrawTextW(Tr(L"Buscar"), (UINT32)wcslen(Tr(L"Buscar")), g_tfSearch, tr, br);
    ID2D1SolidColorBrush *sel = nullptr, *txt = nullptr;
    dc->CreateSolidColorBrush(D2D1::ColorF(pal.accent.r, pal.accent.g, pal.accent.b, 0.30f), &sel);
    dc->CreateSolidColorBrush(pal.text, &txt);
    g_search.Draw(dc, g_tfSearch, tr, txt, sel, txt, g_searchFocus && g_caretOn, false);
    sel->Release(); txt->Release();
}

static int ThumbBucket() { return g_icon <= 64 ? 64 : g_icon <= 112 ? 128 : 192; }

// Ícone de um item (pasta, miniatura, folha ou ícone de programa) dentro de `ir`.
static bool IsAppFile(const Entry& e) {
    std::wstring x = ExtOf(e);
    return x == L"exe" || x == L"msi" || x == L"lnk" || x == L"url" || x == L"appref-ms";
}

// Disco no estilo do Mac: caixa prateada achatada com faixa escura e luz.
static void DrawDrive(ID2D1DeviceContext* dc, D2D1_RECT_F box, ID2D1SolidColorBrush* br) {
    float w = box.right - box.left, h = w * 0.62f;
    float cx = (box.left + box.right) / 2, bottom = box.bottom - w * 0.08f;
    D2D1_RECT_F r{ cx - w * 0.46f, bottom - h, cx + w * 0.46f, bottom };
    float rad = w * 0.07f;
    br->SetColor(D2D1::ColorF(0, 0, 0, 0.18f));
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left, r.top + w * 0.02f, r.right, r.bottom + w * 0.02f), rad, rad), br);
    ID2D1GradientStopCollection* stops = nullptr;
    D2D1_GRADIENT_STOP gs[3] = { { 0, D2D1::ColorF(0.93f, 0.93f, 0.94f) }, { 0.62f, D2D1::ColorF(0.80f, 0.80f, 0.82f) }, { 1, D2D1::ColorF(0.62f, 0.62f, 0.65f) } };
    dc->CreateGradientStopCollection(gs, 3, &stops);
    ID2D1LinearGradientBrush* lg = nullptr;
    dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ cx, r.top }, { cx, r.bottom }), stops, &lg);
    dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), lg);
    lg->Release(); stops->Release();
    float band = r.top + h * 0.62f;  // frente mais escura
    br->SetColor(D2D1::ColorF(0.36f, 0.36f, 0.39f, 0.35f));
    dc->DrawLine({ r.left + rad * 0.5f, band }, { r.right - rad * 0.5f, band }, br, std::max(1.f, w * 0.012f));
    br->SetColor(D2D1::ColorF(0.30f, 0.82f, 0.40f));
    float led = std::max(1.5f, w * 0.025f);
    dc->FillEllipse(D2D1::Ellipse({ r.right - w * 0.1f, (band + r.bottom) / 2 }, led, led), br);
    br->SetColor(D2D1::ColorF(1, 1, 1, 0.6f));
    dc->DrawLine({ r.left + rad, r.top + 1 }, { r.right - rad, r.top + 1 }, br, 1);
}

// Miniatura chegando surge num fade curto (150 ms), como no Finder, em vez de "pipocar" na tela.
static bool g_thumbFading;                       // algum fade em curso: o WM_PAINT pede o próximo quadro
static double NowMs() { LARGE_INTEGER f, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t); return t.QuadPart * 1000.0 / f.QuadPart; }
static float ThumbAlpha(const std::wstring& key) {
    auto b = g_imgBorn.find(key);
    if (b == g_imgBorn.end()) return 1.f;
    float a = (float)((NowMs() - b->second) / 150.0);
    if (a >= 1) { g_imgBorn.erase(b); return 1.f; }
    g_thumbFading = true;
    a = std::max(a, 0.f);
    return a * (2 - a);  // desacelera no fim
}

static void DrawEntryIcon(ID2D1DeviceContext* dc, const Entry& e, D2D1_RECT_F ir, ID2D1SolidColorBrush* br) {
    if (!g_tahoe && tahoe::Leopard()) {  // Leopard: discos e documentos com os ícones do 10.5
        if (e.dir && e.path.size() == 3 && e.path[1] == L':') {
            bool rem = GetDriveTypeW(e.path.c_str()) == DRIVE_REMOVABLE;
            if (DrawLeoPng(dc, rem ? L"side removivel" : L"side disco", ir)) return;
        }
        if (!e.dir) { std::wstring n = LeoDocIcon(e.name); if (!n.empty() && DrawLeoPng(dc, n, ir)) return; }
    }
    if (e.dir && e.path.size() == 3 && e.path[1] == L':') { DrawDrive(dc, ir, br); return; }
    if (e.dir && pastacor::Draw(dc, e.path, ir, g_tahoe)) return;  // cor e símbolo escolhidos (Personalizar Pasta)
    if (e.dir && !e.emblem && g_tahoe && pastacor::DrawColor(dc, tahoe::FolderColor(), ir, g_tahoe)) return;  // Aparência > Cor das pastas
    if (e.dir) { DrawFolder(dc, ir, e.emblem, br); return; }
    if (!g_showThumbs && !IsAppFile(e)) { DrawDocument(dc, ir, e.name, br); return; }  // "Mostrar prévia do ícone" desligado
    float icon = ir.right - ir.left;
    std::wstring key = e.Key();
    auto it = g_images.find(key);
    if (it != g_images.end() && it->second) {
        auto sz = it->second->GetPixelSize();
        float k = std::min(icon / sz.width, icon / sz.height);
        float w = sz.width * k, h = sz.height * k;
        float cx = (ir.left + ir.right) / 2;
        D2D1_RECT_F dst{ cx - w / 2, ir.bottom - h, cx + w / 2, ir.bottom };  // apoiado na base, como no Finder
        if (g_view != View::Icons) { float cy = (ir.top + ir.bottom) / 2; dst = { cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2 }; }
        dc->DrawBitmap(it->second, dst, ThumbAlpha(key), D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    } else if (auto kd = g_kinds.find(key); kd != g_kinds.end()) {
        if (kd->second == thumbs::KIND_FOLDER) DrawFolder(dc, ir, 0, br);
        else DrawDocument(dc, ir, e.name, br);
    } else if (!g_requested.count(key)) {
        g_requested.insert(key);
        thumbs::Request(g_gen, e.path, key, (int)std::lround(ThumbBucket() * g_scale));
    }
}

static void DrawItemIcon(ID2D1DeviceContext* dc, int i, D2D1_RECT_F ir, ID2D1SolidColorBrush* br) {
    DrawEntryIcon(dc, g_items[i], ir, br);
}

// Ícone grande (prévia das colunas e da galeria): pede uma miniatura em alta resolução à parte.
static void DrawBigIcon(ID2D1DeviceContext* dc, const Entry& e, D2D1_RECT_F box, ID2D1SolidColorBrush* br) {
    if (e.dir && pastacor::Draw(dc, e.path, box, g_tahoe)) return;
    if (e.dir && !e.emblem && g_tahoe && pastacor::DrawColor(dc, tahoe::FolderColor(), box, g_tahoe)) return;
    if (e.dir) { DrawFolder(dc, box, e.emblem, br); return; }
    std::wstring big = e.Key() + L"|G";
    auto it = g_images.find(big);
    if (it != g_images.end() && it->second) {
        auto sz = it->second->GetPixelSize();
        float k = std::min((box.right - box.left) / sz.width, (box.bottom - box.top) / sz.height);
        float w = sz.width * k, h = sz.height * k, cx = (box.left + box.right) / 2, cy = (box.top + box.bottom) / 2;
        float a = ThumbAlpha(big);
        if (a < 1) DrawEntryIcon(dc, e, box, br);  // a grande entra por cima da pequena
        dc->DrawBitmap(it->second, D2D1::RectF(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2), a, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
        return;
    }
    DrawEntryIcon(dc, e, box, br);  // enquanto não chega (ou se não houver miniatura)
    bool doc = g_kinds.count(e.Key()) > 0 || g_kinds.count(big) > 0;
    if (!doc && !g_requested.count(big)) {
        g_requested.insert(big);
        thumbs::Request(g_gen, e.path, big, (int)std::lround(512 * g_scale));
    }
}

// Bolinhas das etiquetas, sobrepostas como no Finder, com um anel da cor do fundo entre elas.
static float TagDotsWidth(unsigned mask) {
    int n = 0;
    for (int t = 0; t < tags::COUNT; t++) n += (mask >> t) & 1;
    return n ? 9 + (n - 1) * 6.f : 0;
}

static void DrawTagDots(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, float x, float cy, unsigned mask, D2D1_COLOR_F ring) {
    for (int t = 0; t < tags::COUNT; t++) {
        if (!(mask & (1u << t))) continue;
        UINT32 c = tags::colors[t];
        br->SetColor(ring);
        dc->FillEllipse(D2D1::Ellipse({ x + 4.5f, cy }, 5.6f, 5.6f), br);
        br->SetColor(D2D1::ColorF(((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f));
        dc->FillEllipse(D2D1::Ellipse({ x + 4.5f, cy }, 4.5f, 4.5f), br);
        x += 6;
    }
}

static float TextWidth(const std::wstring& s, IDWriteTextFormat* tf) {
    IDWriteTextLayout* tl = nullptr;
    float w = 0;
    if (SUCCEEDED(gfx::dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), tf, 4000, 40, &tl))) {
        DWRITE_TEXT_METRICS m{}; tl->GetMetrics(&m); w = m.widthIncludingTrailingWhitespace; tl->Release();
    }
    return w;
}

static std::wstring SizeText(const Entry& e) {
    if (e.dir) return L"--";
    wchar_t b[64];
    StrFormatByteSizeW((LONGLONG)e.size, b, 64);
    return b;
}

// "Hoje às 14:09", "Ontem às 10:30", "17 de jun. de 2024 às 20:33", como o Finder com datas relativas.
static std::wstring DateText(ULONGLONG mtime) {
    static const wchar_t* months[] = { L"jan.", L"fev.", L"mar.", L"abr.", L"mai.", L"jun.", L"jul.", L"ago.", L"set.", L"out.", L"nov.", L"dez." };
    FILETIME ft{ (DWORD)mtime, (DWORD)(mtime >> 32) }, lft;
    FileTimeToLocalFileTime(&ft, &lft);
    SYSTEMTIME st, now;
    FileTimeToSystemTime(&lft, &st);
    GetLocalTime(&now);
    auto dayNum = [](const SYSTEMTIME& t) {
        FILETIME f; SystemTimeToFileTime(&t, &f);
        return (((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime) / (10000000ull * 86400);
    };
    ULONGLONG d = dayNum(st), n = dayNum(now);
    wchar_t b[80];
    if (g_relDates && d == n) swprintf_s(b, Tr(L"Hoje às %02d:%02d"), st.wHour, st.wMinute);  // relativas: opção
    else if (g_relDates && d + 1 == n) swprintf_s(b, Tr(L"Ontem às %02d:%02d"), st.wHour, st.wMinute);
    else if (idioma::Pt()) swprintf_s(b, L"%d de %ls de %d às %02d:%02d", st.wDay, months[st.wMonth - 1], st.wYear, st.wHour, st.wMinute);
    else return idioma::DateTime(st, idioma::D_SHORT);  // "Nov 20, 2026 at 14:30", "20 nov. 2026 à 14:30"
    return b;
}

// Cabeçalho da lista: Nome | Tamanho | Tipo | Data de Modificação, com a seta da ordenação.
static int HitHeader(D2D1_POINT_2F p) {
    if (g_view != View::List || p.x < Lay::SIDEBAR_W || p.y < TopH() || p.y >= TopH() + ListLay::HEADER_H) return -1;
    auto k = Cols();
    if (k.created && p.x >= k.crL - 6) return SORT_CREATED;
    if (k.date && p.x >= k.dateL - 6) return SORT_DATE;
    if (k.kind && p.x >= k.kindL - 6) return SORT_KIND;
    if (k.size && p.x >= k.sizeL - 6) return SORT_SIZE;
    return SORT_NAME;
}

static void DrawListHeader(ID2D1DeviceContext* dc, const Palette& pal, ID2D1SolidColorBrush* br) {
    auto s = ClientDip();
    auto k = Cols();
    float top = TopH(), bottom = top + ListLay::HEADER_H;
    if (!g_tahoe && tahoe::Leopard()) {
        // Leopard (captura do 10.5): faixa branca com degradê leve, fio cinza embaixo, divisórias de cima a baixo, os
        // títulos em preto normal e o ▲/▼ cheio na coluna da ordem
        D2D1_GRADIENT_STOP st[2] = { { 0, D2D1::ColorF(1, 1, 1) }, { 1, D2D1::ColorF(0.92f, 0.92f, 0.92f) } };
        ID2D1GradientStopCollection* gs = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
        if (SUCCEEDED(dc->CreateGradientStopCollection(st, 2, &gs)) &&
            SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, top }, { 0, bottom }), gs, &lb)))
            dc->FillRectangle(D2D1::RectF(Lay::SIDEBAR_W, top, s.width, bottom), lb);
        if (lb) lb->Release();
        if (gs) gs->Release();
        br->SetColor(D2D1::ColorF(0.62f, 0.62f, 0.62f));
        dc->DrawLine({ Lay::SIDEBAR_W, bottom - 0.5f }, { s.width, bottom - 0.5f }, br, 1);
        static IDWriteTextFormat* tfH = [] {
            IDWriteTextFormat* f = nullptr;
            gfx::dw->CreateTextFormat(L"Lucida Sans Unicode", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11, L"pt-br", &f);
            if (f) { f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP); }
            return f;
        }();
        struct C2 { SortCol id; const wchar_t* t; float l, r; bool show; };
        const C2 cs[] = { { SORT_NAME, Tr(L"Nome"), k.nameL, k.nameR, true }, { SORT_SIZE, Tr(L"Tamanho"), k.sizeL, k.sizeR, k.size },
                          { SORT_KIND, Tr(L"Tipo"), k.kindL, k.kindR, k.kind }, { SORT_DATE, Tr(L"Data de Modificação"), k.dateL, k.dateR, k.date },
                          { SORT_CREATED, Tr(L"Data de Criação"), k.crL, k.crR, k.created } };
        for (auto& c : cs) {
            if (!c.show) continue;
            float l = c.id == SORT_NAME ? c.l + 20 : c.l;
            br->SetColor(D2D1::ColorF(0, 0, 0, 0.88f));
            if (tfH) dc->DrawTextW(c.t, (UINT32)wcslen(c.t), tfH, D2D1::RectF(l, top, c.r - 16, bottom), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            if (c.id == g_sortCol) {  // ▲/▼ cheio na ponta direita da coluna
                float ax = c.r - 12, cy = (top + bottom) / 2;
                ID2D1Factory* f = nullptr; dc->GetFactory(&f);
                ID2D1PathGeometry* g = nullptr;
                if (f && SUCCEEDED(f->CreatePathGeometry(&g))) {
                    ID2D1GeometrySink* sk = nullptr;
                    if (SUCCEEDED(g->Open(&sk))) {
                        if (g_sortAsc) { sk->BeginFigure({ ax - 3.5f, cy + 2 }, D2D1_FIGURE_BEGIN_FILLED); sk->AddLine({ ax + 3.5f, cy + 2 }); sk->AddLine({ ax, cy - 2.5f }); }
                        else { sk->BeginFigure({ ax - 3.5f, cy - 2 }, D2D1_FIGURE_BEGIN_FILLED); sk->AddLine({ ax + 3.5f, cy - 2 }); sk->AddLine({ ax, cy + 2.5f }); }
                        sk->EndFigure(D2D1_FIGURE_END_CLOSED); sk->Close(); sk->Release();
                        br->SetColor(D2D1::ColorF(0.25f, 0.25f, 0.25f));
                        dc->FillGeometry(g, br);
                    }
                    g->Release();
                }
                if (f) f->Release();
            }
            br->SetColor(D2D1::ColorF(0.72f, 0.72f, 0.72f));  // divisória à direita da coluna, de cima a baixo
            float x = std::floor(c.r - 7) + 0.5f;
            dc->DrawLine({ x, top }, { x, bottom - 1 }, br, 1);
        }
        return;
    }
    br->SetColor(pal.content);
    dc->FillRectangle(D2D1::RectF(Lay::SIDEBAR_W, top, s.width, bottom), br);
    br->SetColor(pal.line);
    dc->DrawLine({ Lay::SIDEBAR_W, bottom - 0.5f }, { s.width, bottom - 0.5f }, br, 1);
    struct Col { SortCol id; const wchar_t* title; float l, r; bool right; bool show; };
    const Col cols[] = {
        { SORT_NAME, Tr(L"Nome"), k.nameL + 20, k.nameR, false, true },  // no Finder o título começa sobre o ícone
        { SORT_SIZE, Tr(L"Tamanho"), k.sizeL, k.sizeR, true, k.size },
        { SORT_KIND, Tr(L"Tipo"), k.kindL, k.kindR, false, k.kind },
        { SORT_DATE, Tr(L"Data de Modificação"), k.dateL, k.dateR, false, k.date },
        { SORT_CREATED, Tr(L"Data de Criação"), k.crL, k.crR, false, k.created },
    };
    for (auto& c : cols) {
        if (!c.show) continue;
        bool active = c.id == g_sortCol;
        br->SetColor(c.id == SORT_NAME ? pal.text : pal.text2);  // como no Finder: só o título da primeira coluna em destaque
        D2D1_RECT_F r{ c.l, top, c.r, bottom };
        if (c.right) r.right -= 14;  // espaço da seta
        dc->DrawTextW(c.title, (UINT32)wcslen(c.title), c.right ? g_tfHeaderR : c.id == SORT_NAME ? g_tfHeader : g_tfHeaderL, r, br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        if (active) {  // seta ˄/˅ do lado direito da coluna
            float ax = c.right ? c.r - 6 : c.r - 12, cy = (top + bottom) / 2;
            br->SetColor(pal.text2);
            if (g_sortAsc) { Line(dc, br, ax - 3, cy + 1.5f, ax, cy - 1.5f, 1.3f); Line(dc, br, ax, cy - 1.5f, ax + 3, cy + 1.5f, 1.3f); }
            else { Line(dc, br, ax - 3, cy - 1.5f, ax, cy + 1.5f, 1.3f); Line(dc, br, ax, cy + 1.5f, ax + 3, cy - 1.5f, 1.3f); }
        }
        if (c.id != SORT_NAME) {  // divisória curta à esquerda da coluna
            br->SetColor(pal.line);
            float x = (c.right ? c.l : c.l) - 7;
            dc->DrawLine({ x, top + 7 }, { x, bottom - 7 }, br, 1);
        }
    }
}

static void DrawListRows(ID2D1DeviceContext* dc, const Palette& pal, ID2D1SolidColorBrush* br) {
    BuildRows();
    auto c = ContentRect();
    auto k = Cols();
    float dy = c.top - g_scroll;
    // linhas alternadas também abaixo do último item, como no Finder
    D2D1_COLOR_F stripe = g_dark ? D2D1::ColorF(1, 1, 1, 0.035f) : D2D1::ColorF(0, 0, 0, 0.03f);
    const bool leo = tahoe::Leopard();
    if (leo) stripe = D2D1::ColorF(237 / 255.f, 243 / 255.f, 254 / 255.f);  // Leopard: as listras azul-claras do 10.5
    int n = 0;
    float lastBottom = 0;
    for (auto& row : g_rows) {
        lastBottom = row.y + row.h;
        if (row.y + row.h + dy < (g_itemsScene ? 0.f : c.top) || row.y + dy > c.bottom) { if (row.item >= 0) n++; continue; }
        float top = row.y + dy, bottom = top + row.h;
        if (row.item < 0) {  // título do grupo
            br->SetColor(pal.text2);
            dc->DrawTextW(row.title.c_str(), (UINT32)row.title.size(), g_tfGroup, D2D1::RectF(k.nameL + 4, top + 14, k.nameR, bottom), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            n = 0;
            continue;
        }
        int i = row.item;
        const Entry& e = g_items[i];
        D2D1_RECT_F rr{ c.left + 8, top, c.right - 8, bottom };
        bool selActive = e.sel && g_winActive;
        if (e.sel && leo) {  // Leopard: a faixa azul de ponta a ponta (cinza com a janela inativa)
            br->SetColor(g_winActive ? D2D1::ColorF(56 / 255.f, 117 / 255.f, 215 / 255.f) : D2D1::ColorF(212 / 255.f, 212 / 255.f, 212 / 255.f));
            dc->FillRectangle(D2D1::RectF(c.left, top, c.right, bottom), br);
        } else if (e.sel) {
            br->SetColor(g_winActive ? pal.accent : pal.inactiveSel);
            dc->FillRoundedRectangle(D2D1::RoundedRect(rr, 5, 5), br);
        } else if (n % 2 == 1) {
            br->SetColor(stripe);
            dc->FillRectangle(D2D1::RectF(c.left, top, c.right, bottom), br);
        }
        if (i == g_dropHi) {
            br->SetColor(pal.accent);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(Inflate(rr, -1), 5, 5), br, 2);
        }
        n++;
        DrawItemIcon(dc, i, ToView(IconDoc(i)), br);
        D2D1_COLOR_F main = selActive ? D2D1::ColorF(1, 1, 1, 1) : pal.text;
        D2D1_COLOR_F second = selActive ? D2D1::ColorF(1, 1, 1, 0.85f) : leo ? D2D1::ColorF(0, 0, 0, 0.85f) : pal.text2;  // Leopard: todas as colunas em preto (captura do 10.5)
        if (i != g_renameIdx) {
            auto lr = ToView(LabelDoc(i));
            float dots = TagDotsWidth(e.tags);
            if (dots) lr.right -= dots + 8;
            br->SetColor(main);
            dc->DrawTextW(e.label.c_str(), (UINT32)e.label.size(), g_tfRow, lr, br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            if (dots)  // etiquetas logo depois do nome
                DrawTagDots(dc, br, std::min(lr.left + TextWidth(e.label, g_tfRow) + 6, lr.right + 6), (lr.top + lr.bottom) / 2, e.tags,
                            e.sel ? (g_winActive ? pal.accent : pal.inactiveSel) : pal.content);
        }
        br->SetColor(second);
        if (k.size) {
            std::wstring sz = SizeText(e);
            dc->DrawTextW(sz.c_str(), (UINT32)sz.size(), g_tfRowR, D2D1::RectF(k.sizeL, top, k.sizeR - 14, bottom), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        if (k.kind) {
            std::wstring kn = KindName(e);
            dc->DrawTextW(kn.c_str(), (UINT32)kn.size(), g_tfRow, D2D1::RectF(k.kindL, top, k.kindR - 8, bottom), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        if (k.date) {
            std::wstring dt = DateText(e.mtime);
            dc->DrawTextW(dt.c_str(), (UINT32)dt.size(), g_tfRow, D2D1::RectF(k.dateL, top, k.dateR, bottom), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        if (k.created) {
            std::wstring dt = DateText(e.ctime);
            dc->DrawTextW(dt.c_str(), (UINT32)dt.size(), g_tfRow, D2D1::RectF(k.crL, top, k.crR, bottom), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
    for (float y = lastBottom + dy; y < c.bottom; y += ListRowH(), n++) {
        if (n % 2 == 1) {
            br->SetColor(stripe);
            dc->FillRectangle(D2D1::RectF(c.left, y, c.right, y + ListRowH()), br);
        }
    }
}

// ---- Colunas

static void DrawChevron(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, float x, float cy) {
    Line(dc, br, x - 1.5f, cy - 3.5f, x + 2, cy, 1.4f);
    Line(dc, br, x + 2, cy, x - 1.5f, cy + 3.5f, 1.4f);
}

// Uma coluna. items: conteúdo; selected(j): destaque; active: azul (foco) ou cinza (caminho aberto).
template <class Sel>
static void DrawColumn(ID2D1DeviceContext* dc, const Palette& pal, ID2D1SolidColorBrush* br, float x,
                       const std::vector<Entry>& items, float scroll, Sel selected, bool active, int skipLabel) {
    auto c = ContentRect();
    for (size_t j = 0; j < items.size(); j++) {
        float top = c.top + 4 + j * ColLay::ROW_H - scroll;
        if (top + ColLay::ROW_H < c.top) continue;
        if (top > c.bottom) break;
        const Entry& e = items[j];
        D2D1_RECT_F rr{ x + 6, top, x + ColLay::COL_W - 6, top + ColLay::ROW_H };
        bool sel = selected((int)j);
        bool blue = sel && active && g_winActive;
        if (sel) {
            br->SetColor(blue ? pal.accent : pal.inactiveSel);
            dc->FillRoundedRectangle(D2D1::RoundedRect(rr, 5, 5), br);
        }
        if (active && (int)j == g_dropHi) {
            br->SetColor(pal.accent);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(Inflate(rr, -1), 5, 5), br, 2);
        }
        float cy = top + ColLay::ROW_H / 2;
        if (g_colIcons) DrawEntryIcon(dc, e, D2D1::RectF(x + 12, cy - ColLay::ICON / 2, x + 12 + ColLay::ICON, cy + ColLay::ICON / 2), br);
        float dots = TagDotsWidth(e.tags);
        if ((int)j != skipLabel) {
            br->SetColor(blue ? D2D1::ColorF(1, 1, 1, 1) : pal.text);
            dc->DrawTextW(e.label.c_str(), (UINT32)e.label.size(), g_tfRow, D2D1::RectF(x + (g_colIcons ? 35 : 14), top, x + ColLay::COL_W - 22 - (dots ? dots + 6 : 0), top + ColLay::ROW_H), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        if (dots) DrawTagDots(dc, br, x + ColLay::COL_W - 24 - dots, cy, e.tags, sel ? (blue ? pal.accent : pal.inactiveSel) : pal.content);
        if (e.dir) {
            br->SetColor(blue ? D2D1::ColorF(1, 1, 1, 0.9f) : pal.text2);
            DrawChevron(dc, br, x + ColLay::COL_W - 15, cy);
        }
    }
    br->SetColor(pal.line);  // divisória à direita
    dc->DrawLine({ x + ColLay::COL_W - 0.5f, c.top }, { x + ColLay::COL_W - 0.5f, c.bottom }, br, 1);
}

// Prévia do arquivo selecionado: ícone grande, nome e informações.
static void DrawPreview(ID2D1DeviceContext* dc, const Palette& pal, ID2D1SolidColorBrush* br, const Entry& e, D2D1_RECT_F area) {
    float w = area.right - area.left, size = std::min(170.f, w - 70);
    float cx = (area.left + area.right) / 2, top = area.top + 30;
    DrawBigIcon(dc, e, D2D1::RectF(cx - size / 2, top, cx + size / 2, top + size), br);
    float y = top + size + 16;
    br->SetColor(pal.text);
    dc->DrawTextW(e.label.c_str(), (UINT32)e.label.size(), g_tfPreviewName, D2D1::RectF(area.left + 16, y, area.right - 16, y + 40), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    std::wstring info = e.dir ? KindName(e) : KindName(e) + L" · " + SizeText(e);  // pasta: sem o "--"
    std::wstring date = Tr(L"Modificado: ") + DateText(e.mtime);
    br->SetColor(pal.text2);
    dc->DrawTextW(info.c_str(), (UINT32)info.size(), g_tfPreviewInfo, D2D1::RectF(area.left + 12, y + 42, area.right - 12, y + 60), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    dc->DrawTextW(date.c_str(), (UINT32)date.size(), g_tfPreviewInfo, D2D1::RectF(area.left + 12, y + 60, area.right - 12, y + 78), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

static void DrawColumnsView(ID2D1DeviceContext* dc, const Palette& pal, ID2D1SolidColorBrush* br) {
    auto c = ContentRect();
    for (size_t k = 0; k < g_colTrail.size(); k++) {  // colunas da esquerda: caminho aberto em cinza
        const Column& col = g_colTrail[k];
        DrawColumn(dc, pal, br, ColX(k), col.items, col.scroll, [&](int j) { return SamePath(col.items[j].path, col.open); }, false, -1);
    }
    float x = ActiveColX();
    DrawColumn(dc, pal, br, x, g_items, g_scroll, [](int j) { return g_items[j].sel; }, true, g_renameIdx);
    int s = SingleSelected();
    float rx = x + ColLay::COL_W;
    if (s >= 0 && g_items[s].dir) {
        const auto& kids = ChildItems(g_items[s].path);
        DrawColumn(dc, pal, br, rx, kids, 0, [](int) { return false; }, false, -1);
    } else if (s >= 0) {
        DrawPreview(dc, pal, br, g_items[s], D2D1::RectF(rx, c.top, rx + ColLay::PREVIEW_W, c.bottom));
    }
}

// ---- Galeria

// ---- Cover Flow (Leopard): fundo preto, a capa escolhida de frente no meio, as outras giradas ~70° para os lados
// (perspectiva de verdade: efeito 3D do Direct2D sobre o desenho do ícone/miniatura), reflexo no chão que some,
// o nome e o tipo em branco embaixo e a barra de rolagem escura. Clicar numa capa escolhe o item.
static IDWriteTextFormat* Format(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align, DWRITE_PARAGRAPH_ALIGNMENT para, bool wrap);  // mais abaixo
static float CoverFlowH() {
    if (!g_coverFlow || g_view != View::List || g_tahoe || !tahoe::Leopard()) return 0;
    auto s = ClientDip();
    return std::max(170.f, std::floor((s.height - Lay::TOOLBAR_H - Lay::FOOTER_H) * 0.5f));
}
static std::vector<std::pair<int, D2D1_RECT_F>> g_cfHits;  // capas na tela (a de cima por último)
static D2D1_RECT_F CoverFlowRect() { auto s = ClientDip(); float t = TopH() - CoverFlowH(); return { Lay::SIDEBAR_W, t, s.width, t + CoverFlowH() }; }
static void DrawCoverFlow(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br) {
    g_cfHits.clear();
    D2D1_RECT_F pr = CoverFlowRect();
    if (pr.bottom - pr.top < 10) return;
    int n = (int)g_items.size();
    int f = g_focus >= 0 && g_focus < n ? g_focus : (n ? 0 : -1);
    float ph = pr.bottom - pr.top, cx = (pr.left + pr.right) / 2;
    float S = std::floor(ph * 0.52f), floorY = pr.top + ph * 0.10f + S;
    auto coverX = [&](int k) { return k == 0 ? cx : cx + (k > 0 ? 1.f : -1.f) * (S * 0.62f + (std::abs(k) - 1) * S * 0.20f); };
    std::vector<int> order;
    if (f >= 0) {
        int span = (int)((pr.right - pr.left) / 2 / (S * 0.20f)) + 2;
        for (int d = span; d >= 1; d--) { if (f - d >= 0) order.push_back(f - d); if (f + d < n) order.push_back(f + d); }
        order.push_back(f);
    }
    // 1º as capas (desenho do ícone numa lista de comandos: trocar o alvo só pode sem recorte empilhado)
    struct Cover { int i, k; float x; ID2D1CommandList* cl; };
    std::vector<Cover> covers;
    for (int i : order) {
        int k = i - f;
        float x = coverX(k);
        if (x + S < pr.left || x - S > pr.right) continue;
        ID2D1CommandList* cl = nullptr;
        if (FAILED(dc->CreateCommandList(&cl))) continue;
        ID2D1Image* old = nullptr; dc->GetTarget(&old);
        D2D1_MATRIX_3X2_F oldT; dc->GetTransform(&oldT);
        dc->SetTarget(cl);
        dc->SetTransform(D2D1::Matrix3x2F::Identity());
        DrawItemIcon(dc, i, D2D1::RectF(0, 0, S, S), br);
        dc->SetTarget(old); if (old) old->Release();
        dc->SetTransform(oldT);
        cl->Close();
        covers.push_back({ i, k, x, cl });
    }
    dc->PushAxisAlignedClip(pr, D2D1_ANTIALIAS_MODE_ALIASED);
    {   // preto com um véu cinza bem leve embaixo
        D2D1_GRADIENT_STOP st[2] = { { 0, D2D1::ColorF(0, 0, 0, 1) }, { 1, D2D1::ColorF(0.10f, 0.10f, 0.11f, 1) } };
        ID2D1GradientStopCollection* gs = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
        if (SUCCEEDED(dc->CreateGradientStopCollection(st, 2, &gs)) && SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, pr.top }, { 0, pr.bottom }), gs, &lb)))
            dc->FillRectangle(pr, lb);
        if (lb) lb->Release();
        if (gs) gs->Release();
    }
    ID2D1Effect* fx = nullptr;
    dc->CreateEffect(CLSID_D2D13DPerspectiveTransform, &fx);
    D2D1_MATRIX_3X2_F oldT; dc->GetTransform(&oldT);
    for (auto& cv : covers) {
        int i = cv.i, k = cv.k; float x = cv.x; ID2D1CommandList* cl = cv.cl;
        float ang = k == 0 ? 0.f : (k < 0 ? -70.f : 70.f);  // a borda voltada para o meio fica mais perto
        ID2D1Image* img = cl;
        if (fx && k != 0) {
            fx->SetInput(0, cl);
            fx->SetValue(D2D1_3DPERSPECTIVETRANSFORM_PROP_ROTATION, D2D1::Vector3F(0, ang, 0));
            fx->SetValue(D2D1_3DPERSPECTIVETRANSFORM_PROP_ROTATION_ORIGIN, D2D1::Vector3F(S / 2, S / 2, 0));
            fx->SetValue(D2D1_3DPERSPECTIVETRANSFORM_PROP_PERSPECTIVE_ORIGIN, D2D1::Vector2F(S / 2, S / 2));
            fx->SetValue(D2D1_3DPERSPECTIVETRANSFORM_PROP_DEPTH, S * 1.6f);
            fx->GetOutput(&img);
        } else img->AddRef();
        D2D1_POINT_2F at{ x - S / 2, floorY - S };
        dc->DrawImage(img, at, D2D1_INTERPOLATION_MODE_LINEAR);
        {   // reflexo: espelhado no chão, sumindo para baixo
            D2D1_GRADIENT_STOP st[2] = { { 0, D2D1::ColorF(1, 1, 1, 0.38f) }, { 1, D2D1::ColorF(1, 1, 1, 0) } };
            ID2D1GradientStopCollection* gs = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
            if (SUCCEEDED(dc->CreateGradientStopCollection(st, 2, &gs)) &&
                SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, floorY }, { 0, floorY + S * 0.45f }), gs, &lb))) {
                dc->PushLayer(D2D1::LayerParameters(D2D1::RectF(pr.left, floorY, pr.right, pr.bottom), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), 1, lb), nullptr);
                dc->SetTransform(D2D1::Matrix3x2F::Scale(1, -1, { 0, floorY }) * oldT);
                dc->DrawImage(img, at, D2D1_INTERPOLATION_MODE_LINEAR);
                dc->SetTransform(oldT);
                dc->PopLayer();
            }
            if (lb) lb->Release();
            if (gs) gs->Release();
        }
        float w2 = k == 0 ? S : S * 0.34f;
        g_cfHits.push_back({ i, D2D1::RectF(x - w2 / 2, floorY - S, x + w2 / 2, floorY) });
        img->Release();
        cl->Release();
    }
    if (fx) fx->Release();
    if (f >= 0) {  // nome e tipo da capa do meio
        const Entry& e = g_items[f];
        static IDWriteTextFormat* tfN = Format(L"Lucida Sans Unicode", 12, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
        static IDWriteTextFormat* tfK = Format(L"Lucida Sans Unicode", 11, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
        float ty = pr.bottom - 52;
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.95f));
        dc->DrawTextW(e.label.c_str(), (UINT32)e.label.size(), tfN, D2D1::RectF(pr.left + 20, ty, pr.right - 20, ty + 16), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        std::wstring kind = KindName(e);
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.55f));
        dc->DrawTextW(kind.c_str(), (UINT32)kind.size(), tfK, D2D1::RectF(pr.left + 20, ty + 15, pr.right - 20, ty + 30), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        // a barra de rolagem escura do Cover Flow (trilho cinza e o polegar claro)
        float bw = std::min(pr.right - pr.left - 80, 420.f), bl = cx - bw / 2, by = pr.bottom - 14;
        br->SetColor(D2D1::ColorF(0.22f, 0.22f, 0.24f, 1));
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bl, by - 5, bl + bw, by + 5), 5, 5), br);
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.25f));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(bl + 0.5f, by - 4.5f, bl + bw - 0.5f, by + 4.5f), 4.5f, 4.5f), br, 1);
        float tw = std::max(30.f, bw / std::max(1, n) * 6), tx = bl + (bw - tw) * (n > 1 ? (float)f / (n - 1) : 0);
        br->SetColor(D2D1::ColorF(0.62f, 0.62f, 0.64f, 1));
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tx, by - 4, tx + tw, by + 4), 4, 4), br);
    }
    dc->PopAxisAlignedClip();
}
static bool CoverFlowClick(D2D1_POINT_2F p) {
    if (CoverFlowH() <= 0 || !In(CoverFlowRect(), p)) return false;
    for (int k = (int)g_cfHits.size() - 1; k >= 0; k--)
        if (In(g_cfHits[k].second, p)) { SelectOnly(g_cfHits[k].first); EnsureVisible(g_cfHits[k].first); Invalidate(); break; }
    return true;
}

static void DrawGalleryView(ID2D1DeviceContext* dc, const Palette& pal, ID2D1SolidColorBrush* br) {
    auto c = ContentRect();
    int f = g_focus >= 0 && g_focus < (int)g_items.size() ? g_focus : (g_items.empty() ? -1 : 0);
    if (f >= 0) {  // prévia grande
        auto pv = ToView(GalleryPreviewDoc());
        float size = std::min(pv.right - pv.left, pv.bottom - pv.top);
        float cx = (pv.left + pv.right) / 2, cy = (pv.top + pv.bottom) / 2;
        DrawBigIcon(dc, g_items[f], D2D1::RectF(cx - size / 2, cy - size / 2, cx + size / 2, cy + size / 2), br);
        auto lr = ToView(LabelDoc(f));
        const Entry& e = g_items[f];
        if (f != g_renameIdx) {
            br->SetColor(pal.text);
            dc->DrawTextW(e.label.c_str(), (UINT32)e.label.size(), g_tfPreviewName, lr, br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            if (float dots = TagDotsWidth(e.tags)) {  // etiquetas antes do nome centralizado
                float tw = std::min(TextWidth(e.label, g_tfPreviewName), lr.right - lr.left);
                DrawTagDots(dc, br, (lr.left + lr.right) / 2 - tw / 2 - dots - 6, lr.top + 10, e.tags, pal.content);
            }
        }
        std::wstring info = (e.dir ? KindName(e) : KindName(e) + L" · " + SizeText(e)) + L" · " + DateText(e.mtime);
        br->SetColor(pal.text2);
        dc->DrawTextW(info.c_str(), (UINT32)info.size(), g_tfPreviewInfo, D2D1::RectF(lr.left, lr.bottom + 2, lr.right, lr.bottom + 20), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    // faixa de miniaturas
    int first = std::max(0, (int)(g_galleryX / GalLay::STEP) - 1);
    int last = std::min((int)g_items.size() - 1, first + (int)((c.right - c.left) / GalLay::STEP) + 3);
    for (int i = first; i <= last; i++) {
        auto ir = ToView(IconDoc(i));
        if (g_items[i].sel || i == g_dropHi) {
            br->SetColor(g_items[i].sel && g_winActive ? pal.accent : pal.inactiveSel);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(Inflate(ir, 4), 7, 7), br, 2.5f);
        }
        DrawItemIcon(dc, i, ir, br);
    }
}

static void DrawIconGrid(ID2D1DeviceContext* dc, const Palette& pal, ID2D1SolidColorBrush* br) {
    auto s = ClientDip();
    int cols = Columns();
    float ch = CellH();
    int firstRow = std::max(0, (int)((g_scroll - Lay::GRID_PAD - (g_itemsScene ? TopH() : 0.f)) / ch));
    int lastRow = (int)((g_scroll + s.height) / ch) + 1;
    for (int i = firstRow * cols; i < (int)g_items.size() && i < (lastRow + 1) * cols; i++) {
        const Entry& e = g_items[i];
        auto ir = ToView(IconDoc(i));
        if (e.sel || i == g_dropHi) {
            br->SetColor(i == g_dropHi ? D2D1::ColorF(pal.accent.r, pal.accent.g, pal.accent.b, 0.25f) : pal.selIcon);
            dc->FillRoundedRectangle(D2D1::RoundedRect(Inflate(ir, 5), 7, 7), br);
        }
        DrawItemIcon(dc, i, ir, br);
        if (i == g_renameIdx) continue;

        auto lr = ToView(LabelDoc(i));
        IDWriteTextLayout* tl = nullptr;
        if (FAILED(gfx::dw->CreateTextLayout(e.label.c_str(), (UINT32)e.label.size(), g_tfLabel, lr.right - lr.left, lr.bottom - lr.top, &tl))) continue;
        if (e.sel) {  // nome dentro de uma "pílula" azul
            DWRITE_TEXT_METRICS m{}; tl->GetMetrics(&m);
            float h = std::min(m.height, lr.bottom - lr.top);
            D2D1_RECT_F pill{ lr.left + m.left - 4, lr.top + m.top - 1, lr.left + m.left + m.width + 4, lr.top + m.top + h + 1 };
            br->SetColor(g_winActive ? pal.accent : pal.inactiveSel);
            dc->FillRoundedRectangle(D2D1::RoundedRect(pill, 4, 4), br);
            br->SetColor(g_winActive ? D2D1::ColorF(1, 1, 1, 1) : pal.text);
        } else {
            br->SetColor(pal.text);
        }
        dc->DrawTextLayout({ lr.left, lr.top }, tl, br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        if (float dots = TagDotsWidth(e.tags)) {  // etiquetas antes da primeira linha do nome
            float x0, y0; DWRITE_HIT_TEST_METRICS hm;
            tl->HitTestTextPosition(0, FALSE, &x0, &y0, &hm);
            DrawTagDots(dc, br, lr.left + x0 - dots - (e.sel ? 7 : 3), lr.top + hm.top + hm.height / 2, e.tags, pal.content);
        }
        tl->Release();
    }
}

// Desenha o conteúdo (grade ou lista) e o retângulo de seleção.
static void DrawItems(ID2D1DeviceContext* dc, const Palette& pal, ID2D1SolidColorBrush* br) {
    auto c = ContentRect();
    D2D1_RECT_F clip = c;
    if (g_itemsScene) clip.top = 0;
    dc->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);  // barras sólidas: o conteúdo não passa por baixo (no Tahoe passa, desfocado)
    if (g_view == View::List) DrawListRows(dc, pal, br);
    else if (g_view == View::Columns) DrawColumnsView(dc, pal, br);
    else if (g_view == View::Gallery) DrawGalleryView(dc, pal, br);
    else DrawIconGrid(dc, pal, br);

    if (g_drag == Drag::Band) {  // retângulo de seleção
        float dy = c.top - g_scroll;
        D2D1_RECT_F b{ std::min(g_bandStart.x, g_bandEnd.x), std::min(g_bandStart.y, g_bandEnd.y) + dy,
                       std::max(g_bandStart.x, g_bandEnd.x), std::max(g_bandStart.y, g_bandEnd.y) + dy };
        br->SetColor(D2D1::ColorF(pal.text.r, pal.text.g, pal.text.b, 0.10f));
        dc->FillRectangle(b, br);
        br->SetColor(D2D1::ColorF(pal.text.r, pal.text.g, pal.text.b, 0.35f));
        dc->DrawRectangle(b, br, 1);
    }
    dc->PopAxisAlignedClip();
    if (g_view == View::List && !g_itemsScene) DrawListHeader(dc, pal, br);
}

// Barra de rolagem do Mac: aparece ao rolar e com o mouse na borda direita; dá para clicar e arrastar
// (clique fora do polegar pula para ali). Com o mouse em cima ou arrastando, fica mais larga.
static bool g_scrollDrag, g_scrollHover;
static float g_scrollDragOff;

static bool ScrollThumb(D2D1_RECT_F& thumb, D2D1_RECT_F& track) {
    float maxS = MaxScroll();
    if (maxS <= 0 || g_view == View::Gallery) return false;
    auto c = ContentRect();
    float h = c.bottom - c.top;
    float thumbH = std::max(30.f, h * h / DocHeight());
    float y = c.top + 3 + (h - 6 - thumbH) * (g_scroll / maxS);
    track = D2D1::RectF(c.right - 15, c.top, c.right, c.bottom);
    thumb = D2D1::RectF(c.right - 15, y, c.right, y + thumbH);
    return true;
}

static void ScrollToThumbY(float top) {  // top: posição desejada do polegar
    D2D1_RECT_F thumb, track;
    if (!ScrollThumb(thumb, track)) return;
    auto c = ContentRect();
    float h = c.bottom - c.top, thumbH = thumb.bottom - thumb.top;
    float k = std::clamp((top - c.top - 3) / std::max(1.f, h - 6 - thumbH), 0.f, 1.f);
    g_scroll = g_scrollTarget = k * MaxScroll();
    g_scrollShownAt = GetTickCount();
    SetTimer(g_hwnd, T_SCROLLBAR, 50, nullptr);
    Invalidate();
}

static void DrawScrollbar(ID2D1DeviceContext* dc, const Palette& pal, ID2D1SolidColorBrush* br) {
    if (!g_tahoe && tahoe::Leopard()) {
        D2D1_RECT_F thumb, track;
        if (!ScrollThumb(thumb, track)) return;
        auto lin = [&](D2D1_RECT_F r, std::initializer_list<D2D1_GRADIENT_STOP> st, float rad) {
            ID2D1GradientStopCollection* gs = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
            if (SUCCEEDED(dc->CreateGradientStopCollection(st.begin(), (UINT32)st.size(), &gs)) &&
                SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ r.left, 0 }, { r.right, 0 }), gs, &lb)))
                dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), lb);
            if (lb) lb->Release();
            if (gs) gs->Release();
        };
        D2D1_RECT_F tr = D2D1::RectF(track.right - 15, track.top, track.right, track.bottom);
        lin(tr, { { 0, D2D1::ColorF(0.80f, 0.80f, 0.80f, 1) }, { 0.35f, D2D1::ColorF(0.95f, 0.95f, 0.95f, 1) }, { 1, D2D1::ColorF(0.99f, 0.99f, 0.99f, 1) } }, 0);
        br->SetColor(D2D1::ColorF(0, 0, 0, 0.18f));
        dc->FillRectangle(D2D1::RectF(tr.left, tr.top, tr.left + 1, tr.bottom), br);
        D2D1_RECT_F th = D2D1::RectF(tr.left + 1.5f, thumb.top + 1, tr.right - 1.5f, thumb.bottom - 1);
        bool on = g_winActive;  // atrás: polegar cinza (como no Mac)
        lin(th, { { 0, on ? D2D1::ColorF(0.30f, 0.50f, 0.84f, 1) : D2D1::ColorF(0.62f, 0.62f, 0.64f, 1) },
                  { 0.5f, on ? D2D1::ColorF(0.70f, 0.84f, 0.99f, 1) : D2D1::ColorF(0.90f, 0.90f, 0.91f, 1) },
                  { 1, on ? D2D1::ColorF(0.36f, 0.57f, 0.88f, 1) : D2D1::ColorF(0.66f, 0.66f, 0.68f, 1) } }, (th.right - th.left) / 2);
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.55f));  // o brilho de gel do lado esquerdo
        float w3 = (th.right - th.left) * 0.30f;
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(th.left + 1.5f, th.top + 4, th.left + 1.5f + w3, th.bottom - 4), w3 / 2, w3 / 2), br);
        br->SetColor(on ? D2D1::ColorF(0.15f, 0.30f, 0.60f, 0.85f) : D2D1::ColorF(0.40f, 0.40f, 0.42f, 0.85f));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(th, (th.right - th.left) / 2, (th.right - th.left) / 2), br, 0.8f);
        return;
    }
    DWORD since = g_scrollDrag || g_scrollHover ? 0 : GetTickCount() - g_scrollShownAt;
    D2D1_RECT_F thumb, track;
    if (!ScrollThumb(thumb, track) || since >= 1200) return;
    float alpha = since < 900 ? 1.f : 1.f - (since - 900) / 300.f;
    bool wide = g_scrollDrag || g_scrollHover;
    if (wide) {  // trilho visível com o mouse em cima, como no Mac
        br->SetColor(D2D1::ColorF(pal.text.r, pal.text.g, pal.text.b, 0.06f));
        dc->FillRectangle(D2D1::RectF(track.left + 1, track.top, track.right, track.bottom), br);
    }
    float w = wide ? 9.f : 6.f;
    br->SetColor(D2D1::ColorF(pal.text.r, pal.text.g, pal.text.b, (wide ? 0.55f : 0.40f) * alpha));
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(thumb.right - 3 - w, thumb.top, thumb.right - 3, thumb.bottom), w / 2, w / 2), br);
}

static void DrawRenameField(ID2D1DeviceContext* dc, const Palette& pal, ID2D1SolidColorBrush* br) {
    if (g_renameIdx < 0 || g_renameIdx >= (int)g_items.size()) return;
    auto r = RenameRect();
    br->SetColor(g_dark ? D2D1::ColorF(0.17f, 0.17f, 0.18f, 1) : D2D1::ColorF(1, 1, 1, 1));
    dc->FillRoundedRectangle(D2D1::RoundedRect(r, 4, 4), br);
    br->SetColor(D2D1::ColorF(pal.accent.r, pal.accent.g, pal.accent.b, 0.8f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(Inflate(r, 1), 5, 5), br, 2);
    ID2D1SolidColorBrush *sel = nullptr, *txt = nullptr;
    dc->CreateSolidColorBrush(D2D1::ColorF(pal.accent.r, pal.accent.g, pal.accent.b, 0.30f), &sel);
    dc->CreateSolidColorBrush(pal.text, &txt);
    g_rename.Draw(dc, g_tfRename, D2D1::RectF(r.left + 4, r.top, r.right - 4, r.bottom), txt, sel, txt, g_caretOn, true);
    sel->Release(); txt->Release();
}

static void DrawSlider(ID2D1DeviceContext* dc, const Palette& pal, ID2D1SolidColorBrush* br) {
    auto r = SliderRect();
    float cy = (r.top + r.bottom) / 2;
    br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.18f) : D2D1::ColorF(0, 0, 0, 0.14f));
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left, cy - 1.5f, r.right, cy + 1.5f), 1.5f, 1.5f), br);
    float t = (g_icon - Lay::ICON_MIN) / (Lay::ICON_MAX - Lay::ICON_MIN);
    float kx = r.left + t * (r.right - r.left);
    br->SetColor(D2D1::ColorF(0, 0, 0, 0.18f));
    dc->FillEllipse(D2D1::Ellipse({ kx, cy + 0.6f }, 6.8f, 6.8f), br);
    br->SetColor(g_dark ? D2D1::ColorF(0.85f, 0.85f, 0.86f) : D2D1::ColorF(1, 1, 1));
    dc->FillEllipse(D2D1::Ellipse({ kx, cy }, 6.2f, 6.2f), br);
}

static std::wstring StatusText() {
    if (!g_flash.empty() && GetTickCount() - g_flashAt < 4000) return g_flash;
    ULARGE_INTEGER freeBytes{};
    wchar_t freeStr[64] = L"?";
    if (g_tagView >= 0) { int n = (int)g_items.size(); return std::to_wstring(n) + (n == 1 ? Tr(L" item") : Tr(L" itens")); }
    if (g_searching) {
        int n = (int)g_items.size(), sc = SelCount();
        std::wstring t = sc > 0 ? std::to_wstring(sc) + Tr(L" de ") + std::to_wstring(n) + (sc > 1 ? Tr(L" selecionados") : Tr(L" selecionado"))
                                : std::to_wstring(n) + (n == 1 ? Tr(L" resultado") : Tr(L" resultados"));
        if (sc == 1) for (auto& e : g_items) if (e.sel) { t = Parent(e.path); break; }  // onde est\u00e1 o item escolhido
        if (g_searchBusy) t += Tr(L" \u2014 buscando\u2026");
        else if (n >= SEARCH_MAX) t += Tr(L" (limite)");
        return t;
    }
    if (GetDiskFreeSpaceExW(g_folder.c_str(), &freeBytes, nullptr, nullptr)) StrFormatByteSizeW((LONGLONG)freeBytes.QuadPart, freeStr, 64);
    int n = (int)g_items.size(), s = SelCount();
    wchar_t buf[200];
    if (s > 0) swprintf_s(buf, Tr(L"%d de %d selecionado%ls, %ls disponíveis"), s, n, s > 1 ? L"s" : L"", freeStr);
    else if (!g_search.text.empty()) swprintf_s(buf, Tr(L"%d de %zu itens, %ls disponíveis"), n, g_all.size(), freeStr);
    else swprintf_s(buf, Tr(L"%d %ls, %ls disponíveis"), n, n == 1 ? L"item" : Tr(L"itens"), freeStr);
    return buf;
}

// Barra de caminho: "Disco Local (C:) › Users › User › Downloads". Duplo clique num trecho abre a pasta.
struct PathSeg { std::wstring label, path; float w = 0; D2D1_RECT_F rect{}; };
static std::vector<PathSeg> g_pathSegs;

static void DrawPathBar(ID2D1DeviceContext* dc, const Palette& pal, ID2D1SolidColorBrush* br) {
    if (!g_pathBar) { g_pathSegs.clear(); return; }
    auto s = ClientDip();
    float top = s.height - Lay::FOOTER_H, cy = top + Lay::PATH_H / 2;
    br->SetColor(pal.line);  // linha entre a lista e o rodapé, e entre caminho e status
    dc->DrawLine({ Lay::SIDEBAR_W, top + 0.5f }, { s.width, top + 0.5f }, br, 1);
    dc->DrawLine({ Lay::SIDEBAR_W, top + Lay::PATH_H + 0.5f }, { s.width, top + Lay::PATH_H + 0.5f }, br, 0.6f);

    // monta os trechos da raiz até a pasta aberta
    std::vector<PathSeg> segs;
    for (std::wstring p = g_folder; !p.empty(); p = Parent(p)) {
        PathSeg seg;
        seg.path = p;
        seg.label = p.size() <= 3 ? DisplayName(p) : p.substr(p.find_last_of(L'\\') + 1);
        segs.insert(segs.begin(), seg);
    }
    const float icon = 14, gap = 4, sep = 16;
    float total = 0;
    for (auto& seg : segs) {
        IDWriteTextLayout* tl = nullptr;
        seg.w = 60;
        if (SUCCEEDED(gfx::dw->CreateTextLayout(seg.label.c_str(), (UINT32)seg.label.size(), g_tfPath, 2000, Lay::PATH_H, &tl))) {
            DWRITE_TEXT_METRICS m{}; tl->GetMetrics(&m); seg.w = m.width; tl->Release();
        }
        total += icon + gap + seg.w + sep;
    }
    float avail = s.width - Lay::SIDEBAR_W - 24;
    while (total > avail && segs.size() > 2) {  // não cabe: some com os trechos do meio
        total -= icon + gap + segs[1].w + sep;
        segs.erase(segs.begin() + 1);
    }

    float x = Lay::SIDEBAR_W + 12;
    for (size_t i = 0; i < segs.size(); i++) {
        auto& seg = segs[i];
        D2D1_RECT_F ir{ x, cy - icon / 2, x + icon, cy + icon / 2 };
        if (seg.path.size() <= 3) {  // disco
            br->SetColor(pal.text2);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(ir.left + 1, cy - 3.5f, ir.right - 1, cy + 3.5f), 1.5f, 1.5f), br, 1.1f);
            dc->FillEllipse(D2D1::Ellipse({ ir.right - 4, cy }, 0.9f, 0.9f), br);
        } else {
            DrawFolder(dc, ir, 0, br);
        }
        float tx = ir.right + gap;
        br->SetColor(i + 1 == segs.size() ? pal.text : pal.text2);
        dc->DrawTextW(seg.label.c_str(), (UINT32)seg.label.size(), g_tfPath, D2D1::RectF(tx, top, tx + seg.w + 2, top + Lay::PATH_H), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        seg.rect = { x - 2, top, tx + seg.w + 2, top + Lay::PATH_H };
        x = tx + seg.w;
        if (i + 1 < segs.size()) {
            br->SetColor(pal.text2);
            float cx = x + sep / 2;
            Line(dc, br, cx - 1.5f, cy - 3, cx + 1.5f, cy, 1.2f);
            Line(dc, br, cx + 1.5f, cy, cx - 1.5f, cy + 3, 1.2f);
            x += sep;
        }
    }
    g_pathSegs = std::move(segs);
}

// ---------------------------------------------------------------------------
// Quick Look (barra de espaço), no visual do macOS: janela flutuante arredondada por cima do
// Arquivos, com barra de título (fechar, nome, compartilhar, "Abrir com <app>"). Fotos ocupam o
// painel todo; texto aparece numa folha; o resto mostra ícone grande e informações.

static bool g_ql;
static DWORD g_qlOpenedAt;   // para a animação de abrir (zoom + fade)
static std::wstring g_qlTextPath;
static std::vector<std::wstring> g_qlLines;
static IDWriteTextFormat *g_tfMono, *g_tfQlTitle, *g_tfQlButton, *g_tfQlGlyph, *g_tfQlTime, *g_tfQlArtist;
const DWORD QL_ANIM_MS = 240, QL_CLOSE_MS = 190;
// Quick Look sai do ícone do arquivo e volta para ele ao fechar (como no Mac)
static D2D1_RECT_F g_qlFrom{};   // ícone (janela, DIP) no momento de abrir; vazio = cresce do centro
static bool g_qlClosing;
static DWORD g_qlClosedAt;

static bool IsTextFile(const Entry& e) {
    static const wchar_t* exts[] = { L"txt", L"md", L"log", L"json", L"xml", L"csv", L"ini", L"cfg", L"yaml", L"yml", L"cpp", L"c", L"h",
                                     L"hpp", L"cs", L"py", L"js", L"ts", L"tsx", L"jsx", L"html", L"htm", L"css", L"cmd", L"bat", L"ps1",
                                     L"sh", L"toml", L"rs", L"go", L"java", L"kt", L"lua", L"sql", L"gitignore", L"reg", L"srt" };
    std::wstring x = ExtOf(e);
    for (auto v : exts) if (x == v) return true;
    return false;
}

// Primeiros 64 KB como texto (UTF-16 com BOM, UTF-8 ou a página de código do Windows).
static void LoadQlText(const std::wstring& path) {
    if (path == g_qlTextPath) return;
    g_qlTextPath = path;
    g_qlLines.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<char> buf(65536);
    DWORD n = 0;
    ReadFile(h, buf.data(), (DWORD)buf.size(), &n, nullptr);
    CloseHandle(h);
    std::wstring text;
    if (n >= 2 && (BYTE)buf[0] == 0xFF && (BYTE)buf[1] == 0xFE) {
        text.assign((const wchar_t*)(buf.data() + 2), (n - 2) / 2);
    } else {
        int off = (n >= 3 && (BYTE)buf[0] == 0xEF && (BYTE)buf[1] == 0xBB && (BYTE)buf[2] == 0xBF) ? 3 : 0;
        bool utf8 = n == (DWORD)off || MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, buf.data() + off, (int)n - off, nullptr, 0) > 0;
        UINT cp = utf8 ? CP_UTF8 : CP_ACP;
        int len = MultiByteToWideChar(cp, 0, buf.data() + off, (int)n - off, nullptr, 0);
        text.resize(std::max(0, len));
        if (len > 0) MultiByteToWideChar(cp, 0, buf.data() + off, (int)n - off, text.data(), len);
    }
    std::wstring line;
    for (wchar_t c : text) {
        if (c == L'\n') { g_qlLines.push_back(line); line.clear(); if (g_qlLines.size() >= 400) break; }
        else if (c == L'\t') line += L"    ";
        else if (c != L'\r') line += c;
    }
    if (!line.empty() && g_qlLines.size() < 400) g_qlLines.push_back(line);
}

static int QlItem() {
    if (g_focus >= 0 && g_focus < (int)g_items.size() && g_items[g_focus].sel) return g_focus;
    for (int i = 0; i < (int)g_items.size(); i++) if (g_items[i].sel) return i;
    return -1;
}

// Nome do app padrão para o tipo ("Fotos", "Bloco de Notas"...), para o botão "Abrir com".
static std::wstring DefaultAppName(const Entry& e) {
    if (e.dir) return L"";
    std::wstring ext = L"." + ExtOf(e);
    wchar_t name[256] = {};
    DWORD n = 256;
    if (SUCCEEDED(AssocQueryStringW(ASSOCF_INIT_IGNOREUNKNOWN, ASSOCSTR_FRIENDLYAPPNAME, ext.c_str(), nullptr, name, &n)) && name[0]) return name;
    return L"";
}

// macOS 27 (captura Finder-Quick-Look, 2x): barra de título de 38, moldura clara de 5 em volta da prévia, cantos 16 na
// janela e 12 na foto; fechar = círculo escuro de 16 a 9 da borda, título em negrito de 15 à esquerda, "Abrir com…"
// numa cápsula de 26 a 7 da borda
#define QL_TITLE_H (semaforo::Aqua() ? 38.f : (!g_tahoe && tahoe::Leopard()) ? 24.f : 40.f)  // Leopard: a faixa de 24 do HUD
static void RunVerb(const char* verb);

static ID2D1Bitmap* QlImage(const Entry& e) {  // miniatura grande, se já chegou (senão a pequena)
    if (e.dir || !g_showThumbs || g_kinds.count(e.Key())) return nullptr;
    for (auto key : { e.Key() + L"|G", e.Key() }) {
        auto it = g_images.find(key);
        if (it != g_images.end() && it->second) return it->second;
    }
    return nullptr;
}


// Áudio e vídeo: tocam no próprio painel (media.cpp), com a barra de controles do Mac.
static bool IsMediaFile(const Entry& e, bool* video = nullptr) {
    static const wchar_t* vids[] = { L"mp4", L"m4v", L"mov", L"mkv", L"avi", L"wmv", L"webm", L"3gp", L"mts", L"m2ts", L"ts" };
    static const wchar_t* auds[] = { L"mp3", L"m4a", L"aac", L"wav", L"flac", L"wma", L"ogg", L"opus", L"aiff", L"aif", L"alac" };
    if (e.dir) return false;
    std::wstring x = ExtOf(e);
    for (auto v : vids) if (x == v) { if (video) *video = true; return true; }
    for (auto v : auds) if (x == v) { if (video) *video = false; return true; }
    return false;
}

static std::wstring g_qlMediaPath, g_qlMediaTitle, g_qlMediaArtist;
static DWORD g_qlMouseAt;     // controles do vídeo somem ~2,5 s depois do último movimento
static bool g_qlSeekDrag;

// Abre/fecha o player conforme o item do Quick Look (chamado antes de cada quadro).
static void QlSyncMedia() {
    std::wstring want;
    if (g_ql) { int i = QlItem(); if (i >= 0 && IsMediaFile(g_items[i])) want = g_items[i].path; }
    if (want == g_qlMediaPath) return;
    media::Close();
    KillTimer(g_hwnd, T_MEDIA);
    g_qlSeekDrag = false;
    g_qlMediaPath = want;
    g_qlMediaTitle.clear(); g_qlMediaArtist.clear();
    if (want.empty()) return;
    IShellItem2* si = nullptr;  // título e artista das informações do arquivo
    if (SUCCEEDED(SHCreateItemFromParsingName(want.c_str(), nullptr, IID_PPV_ARGS(&si)))) {
        PWSTR v = nullptr;
        if (SUCCEEDED(si->GetString(PKEY_Title, &v))) { g_qlMediaTitle = v; CoTaskMemFree(v); }
        if (SUCCEEDED(si->GetString(PKEY_Music_Artist, &v))) { g_qlMediaArtist = v; CoTaskMemFree(v); }
        si->Release();
    }
    media::Open(want);
    g_qlMouseAt = GetTickCount();
    SetTimer(g_hwnd, T_MEDIA, 15, nullptr);
}

static bool QlMediaOn(const Entry& e) { return !g_qlMediaPath.empty() && g_qlMediaPath == e.path && media::Active() && !media::Failed(); }
static bool QlVideoOn(const Entry& e) { return QlMediaOn(e) && media::HasVideo(); }
static bool QlAudioOn(const Entry& e) { bool v = false; return QlMediaOn(e) && IsMediaFile(e, &v) && (media::Ready() ? !media::HasVideo() : !v); }
static bool QlControlsVisible() { return g_qlSeekDrag || media::Paused() || GetTickCount() - g_qlMouseAt < 2500; }

static std::wstring ClockText(double t) {
    int s = (int)std::max(0.0, t);
    wchar_t b[32];
    if (s >= 3600) swprintf_s(b, L"%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    else swprintf_s(b, L"%d:%02d", s / 60, s % 60);
    return b;
}

// Tamanho do painel: fotos ganham o formato da foto; texto e outros, tamanhos fixos.
static D2D1_RECT_F QlPanel() {
    auto s = ClientDip();
    float maxW = std::min(s.width - 80, 960.f), maxH = std::min(s.height - 70, 720.f);
    float w = 540, h = 440;
    int i = QlItem();
    if (i >= 0) {
        const Entry& e = g_items[i];
        if (!e.dir && IsTextFile(e)) { w = 760; h = 580; }
        else if (QlVideoOn(e)) {
            float vw = (float)media::VideoW(), vh = (float)media::VideoH();
            float k = std::min({ maxW / vw, (maxH - QL_TITLE_H) / vh, 1.5f });
            w = std::max(420.f, vw * k);
            h = vh * k + QL_TITLE_H;
        }
        else if (QlAudioOn(e)) { w = 460; h = 430; }
        else if (auto* bmp = QlImage(e)) {
            auto sz = bmp->GetPixelSize();
            float k = std::min(maxW / sz.width, (maxH - QL_TITLE_H) / sz.height);
            k = std::min(k, 1.4f);  // a miniatura tem no máximo ~512 px: não amplia demais
            w = std::max(420.f, sz.width * k);
            h = sz.height * k + QL_TITLE_H;
        }
    }
    w = std::min(w, maxW); h = std::min(h, maxH);
    float cx = s.width / 2, cy = s.height / 2 + 8;
    return { cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2 };
}
static D2D1_RECT_F QlCloseRect() {
    auto r = QlPanel();
    if (!g_tahoe && tahoe::Leopard()) return { r.left + 5, r.top + 4, r.left + 21, r.top + 20 };  // Leopard: o X redondo do HUD
    if (semaforo::Aqua()) return { r.left + 9, r.top + 11, r.left + 25, r.top + 27 };
    return { r.left + 10, r.top + 10, r.left + 30, r.top + 30 };
}
static D2D1_RECT_F QlOpenRect() {
    auto r = QlPanel();
    if (!g_tahoe && tahoe::Leopard()) return { 0, 0, 0, 0 };  // o Quick Look do 10.5 não tinha "Abrir com" nem Compartilhar
    int i = QlItem();
    std::wstring app = i >= 0 ? DefaultAppName(g_items[i]) : L"";
    float w = std::clamp(TextWidth(app.empty() ? Tr(L"Abrir") : Tr(L"Abrir com ") + app, g_tfQlButton) + 22, 60.f, 220.f);
    if (semaforo::Aqua()) return { r.right - 7 - w - 8, r.top + 6, r.right - 7, r.top + 32 };
    return { r.right - 12 - w, r.top + 8, r.right - 12, r.top + 32 };
}
static D2D1_RECT_F QlShareRect() { auto o = QlOpenRect(); if (o.right <= o.left) return { 0, 0, 0, 0 }; return { o.left - 36, o.top, o.left - 8, o.bottom }; }
static D2D1_RECT_F QlCtlBar() {
    auto r = QlPanel();
    float w = std::min(r.right - r.left - 32, 560.f), cx = (r.left + r.right) / 2;
    return { cx - w / 2, r.bottom - 54, cx + w / 2, r.bottom - 14 };
}
static D2D1_RECT_F QlPlayRect() { auto b = QlCtlBar(); return { b.left + 6, b.top + 4, b.left + 38, b.bottom - 4 }; }
static D2D1_RECT_F QlMuteRect() { auto b = QlCtlBar(); return { b.right - 38, b.top + 4, b.right - 6, b.bottom - 4 }; }
static D2D1_RECT_F QlTrackRect() { auto b = QlCtlBar(); return { b.left + 38 + 52, b.top, b.right - 38 - 56, b.bottom }; }

static void QlSeekAt(float x) {
    auto t = QlTrackRect();
    double d = media::Duration();
    if (d > 0) media::Seek(d * std::clamp((x - t.left) / (t.right - t.left), 0.f, 1.f));
    Invalidate();
}

// Barra de controles: tocar/pausar, tempo, linha do tempo, tempo restante, som.
static void DrawQlControls(ID2D1DeviceContext* dc, const Palette& pal, ID2D1SolidColorBrush* br, bool overVideo) {
    auto b = QlCtlBar();
    D2D1_COLOR_F fg = overVideo ? D2D1::ColorF(1, 1, 1, 0.95f) : pal.text;
    D2D1_COLOR_F dim = overVideo ? D2D1::ColorF(1, 1, 1, 0.65f) : pal.text2;
    if (overVideo) {
        br->SetColor(D2D1::ColorF(0.12f, 0.12f, 0.13f, 0.72f));
        dc->FillRoundedRectangle(D2D1::RoundedRect(b, 10, 10), br);
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.10f));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(Inflate(b, -0.5f), 9.5f, 9.5f), br, 1);
    }
    const wchar_t* play = media::Paused() ? L"" : L"";
    br->SetColor(fg);
    dc->DrawTextW(play, 1, g_tfQlGlyph, QlPlayRect(), br);
    const wchar_t* vol = media::Muted() ? L"" : L"";
    dc->DrawTextW(vol, 1, g_tfQlGlyph, QlMuteRect(), br);
    double d = media::Duration(), t = media::Ended() ? d : media::Time();
    auto tr = QlTrackRect();
    std::wstring el = ClockText(t), rem = L"-" + ClockText(d - t);
    br->SetColor(dim);
    dc->DrawTextW(el.c_str(), (UINT32)el.size(), g_tfQlTime, D2D1::RectF(QlPlayRect().right, b.top, tr.left - 4, b.bottom), br);
    dc->DrawTextW(rem.c_str(), (UINT32)rem.size(), g_tfQlTime, D2D1::RectF(tr.right + 4, b.top, QlMuteRect().left, b.bottom), br);
    float cy = (b.top + b.bottom) / 2, f = d > 0 ? (float)std::clamp(t / d, 0.0, 1.0) : 0.f;
    float x = tr.left + (tr.right - tr.left) * f;
    br->SetColor(overVideo ? D2D1::ColorF(1, 1, 1, 0.25f) : pal.line);
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tr.left, cy - 2, tr.right, cy + 2), 2, 2), br);
    br->SetColor(fg);
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tr.left, cy - 2, x, cy + 2), 2, 2), br);
    dc->FillEllipse(D2D1::Ellipse({ x, cy }, 5.5f, 5.5f), br);
}

static void QlClose() {  // volta para o ícone; o timer 6 desenha e no fim some
    if (!g_ql || g_qlClosing) return;
    g_qlClosing = true;
    g_qlClosedAt = GetTickCount();
    SetTimer(g_hwnd, 6, 15, nullptr);
    Invalidate();
}

// Golden Gate: o Quick Look é o do Shell (janela própria de vidro fosco, arrastável, por cima de tudo, como no Mac).
// O Finder fica com o foco: espaço fecha, Esc fecha e as setas trocam o arquivo mostrado. Vídeo e música também vão
// para lá (o Shell toca com a barra do QuickTime); o painel daqui fica para os outros temas.
static std::wstring g_qlExtPath;
static HWND QlShellWnd() { HWND q = FindWindowW(L"ShellQuickLook", nullptr); return q && IsWindowVisible(q) ? q : nullptr; }
static bool QlSend(const std::wstring& p) {
    HWND sh = FindWindowW(L"ShellMensagens", nullptr);
    if (!sh) return false;
    COPYDATASTRUCT cd{ 0x514C4F4B /*'QLOK'*/, (DWORD)((p.size() + 1) * sizeof(wchar_t)), (PVOID)p.c_str() };
    DWORD_PTR r = 0;
    if (!SendMessageTimeoutW(sh, WM_COPYDATA, (WPARAM)g_hwnd, (LPARAM)&cd, SMTO_ABORTIFHUNG, 800, &r) || !r) return false;
    g_qlExtPath = p;
    return true;
}
static void QlSyncShell() {  // a seleção mudou com o Quick Look do Shell aberto: mostra o novo
    if (g_qlExtPath.empty()) return;
    if (!QlShellWnd()) { g_qlExtPath.clear(); return; }
    int i = QlItem();
    if (i < 0 || !_wcsicmp(g_items[i].path.c_str(), g_qlExtPath.c_str())) return;
    QlSend(g_items[i].path);  // vídeo e música também tocam no Quick Look do Shell
}

static void QlToggle() {
    if (!g_qlExtPath.empty() && QlShellWnd()) { QlSend(L""); return; }
    if (g_ql) { QlClose(); return; }
    if (QlItem() < 0) return;
    if (Golden() && QlSend(g_items[QlItem()].path)) return;  // tudo no Quick Look do Shell (vídeo e música também)
    g_ql = true;
    g_qlClosing = false;
    g_qlFrom = IconDoc(QlItem());
    {   // o ícone fora da área visível (rolou): cresce do centro
        auto c = ContentRect();
        if (g_qlFrom.bottom < c.top || g_qlFrom.top > c.bottom || g_qlFrom.right - g_qlFrom.left < 4) g_qlFrom = {};
    }
    g_qlOpenedAt = GetTickCount();
    SetTimer(g_hwnd, 6, 15, nullptr);  // anima a abertura
    Invalidate();
}

static IDWriteTextFormat* Format(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT align, DWRITE_PARAGRAPH_ALIGNMENT para, bool wrap);
// Mac OS X Leopard: o Quick Look do 10.5 — um HUD preto translúcido de cantos redondos com o fio claro, o título branco
// no meio da faixa de cima, o X branco num círculo cinza à esquerda, a prévia no corpo e, nos vídeos, a barra escura com
// ⏵ e a linha do tempo brancos. Desenho próprio (nada do Quick Look dos outros temas).
static void DrawQuickLookLeopard(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const Entry& e, D2D1_RECT_F r) {
    for (int k = 1; k <= 8; k++) {  // sombra
        br->SetColor(D2D1::ColorF(0, 0, 0, 0.035f));
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left - k * 2.f, r.top - k + 8, r.right + k * 2.f, r.bottom + k * 2.5f + 8), 10 + k * 2.f, 10 + k * 2.f), br);
    }
    const float RR = 10;
    br->SetColor(D2D1::ColorF(0.07f, 0.07f, 0.08f, 0.88f));
    dc->FillRoundedRectangle(D2D1::RoundedRect(r, RR, RR), br);
    br->SetColor(D2D1::ColorF(1, 1, 1, 0.30f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), RR - 0.5f, RR - 0.5f), br, 1);
    static IDWriteTextFormat* tfT = Format(L"Lucida Sans Unicode", 12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    static IDWriteTextFormat* tfI = Format(L"Lucida Sans Unicode", 11, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    br->SetColor(D2D1::ColorF(1, 1, 1, 0.92f));
    dc->DrawTextW(e.label.c_str(), (UINT32)e.label.size(), tfT, D2D1::RectF(r.left + 30, r.top, r.right - 30, r.top + QL_TITLE_H), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    auto cr = QlCloseRect();
    float ccx = (cr.left + cr.right) / 2, ccy = (cr.top + cr.bottom) / 2;
    br->SetColor(D2D1::ColorF(0.42f, 0.42f, 0.44f, 1));
    dc->FillEllipse(D2D1::Ellipse({ ccx, ccy }, 6.5f, 6.5f), br);
    br->SetColor(D2D1::ColorF(1, 1, 1, 0.55f));
    dc->DrawEllipse(D2D1::Ellipse({ ccx, ccy }, 6.5f, 6.5f), br, 0.8f);
    br->SetColor(D2D1::ColorF(0.07f, 0.07f, 0.08f, 1));
    Line(dc, br, ccx - 2.4f, ccy - 2.4f, ccx + 2.4f, ccy + 2.4f, 1.5f);
    Line(dc, br, ccx - 2.4f, ccy + 2.4f, ccx + 2.4f, ccy - 2.4f, 1.5f);
    D2D1_RECT_F body{ r.left + 8, r.top + QL_TITLE_H, r.right - 8, r.bottom - 8 };
    ID2D1Bitmap* bmp = QlImage(e);
    bool text = !e.dir && IsTextFile(e);
    bool video = QlVideoOn(e), audio = !video && QlAudioOn(e);
    auto fit = [&](float vw, float vh) {
        float k = std::min((body.right - body.left) / vw, (body.bottom - body.top) / vh);
        float w = vw * k, h = vh * k, cx = (body.left + body.right) / 2, cy = (body.top + body.bottom) / 2;
        return D2D1::RectF(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2);
    };
    if (video) {
        D2D1_RECT_F vr = fit((float)std::max(1, (int)media::VideoW()), (float)std::max(1, (int)media::VideoH()));
        if (auto* fr = media::Frame()) dc->DrawBitmap(fr, vr, 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        else if (bmp) dc->DrawBitmap(bmp, vr, 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else if (bmp && !audio) {
        auto sz = bmp->GetPixelSize();
        DrawBigIcon(dc, e, fit((float)sz.width, (float)sz.height), br);
    } else if (text) {
        LoadQlText(e.path);
        D2D1_RECT_F page{ body.left + 6, body.top + 2, body.right - 6, body.bottom - 2 };
        br->SetColor(D2D1::ColorF(1, 1, 1, 1));
        dc->FillRectangle(page, br);
        dc->PushAxisAlignedClip(Inflate(page, -8), D2D1_ANTIALIAS_MODE_ALIASED);
        br->SetColor(D2D1::ColorF(0, 0, 0, 0.9f));
        float y = page.top + 10;
        for (auto& ln : g_qlLines) {
            if (y > page.bottom) break;
            dc->DrawTextW(ln.c_str(), (UINT32)ln.size(), g_tfMono, D2D1::RectF(page.left + 12, y, page.right - 10, y + 17), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            y += 17;
        }
        dc->PopAxisAlignedClip();
    } else {  // ícone grande e, embaixo, o nome e as informações em branco
        float size = std::min(200.f, (body.bottom - body.top) - 110);
        float cx = (body.left + body.right) / 2, top = body.top + std::max(8.f, ((body.bottom - body.top) - size - 90) / 2);
        if (bmp && audio) {
            auto sz = bmp->GetPixelSize();
            float k = std::min(size / sz.width, size / sz.height), w = sz.width * k, h = sz.height * k;
            dc->DrawBitmap(bmp, D2D1::RectF(cx - w / 2, top + (size - h) / 2, cx + w / 2, top + (size + h) / 2), 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else DrawBigIcon(dc, e, D2D1::RectF(cx - size / 2, top, cx + size / 2, top + size), br);
        float y = top + size + 14;
        std::wstring name = audio && !g_qlMediaTitle.empty() ? g_qlMediaTitle : e.label;
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.95f));
        dc->DrawTextW(name.c_str(), (UINT32)name.size(), tfT, D2D1::RectF(body.left + 10, y, body.right - 10, y + 20), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        std::wstring info = audio ? g_qlMediaArtist : KindName(e);
        if (!audio) {
            if (e.dir) { size_t n = ChildItems(e.path).size(); info += L" · " + std::to_wstring(n) + (n == 1 ? Tr(L" item") : Tr(L" itens")); }
            else info += L" · " + SizeText(e);
        }
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.6f));
        dc->DrawTextW(info.c_str(), (UINT32)info.size(), tfI, D2D1::RectF(body.left, y + 22, body.right, y + 38), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        if (!audio) {
            std::wstring date = Tr(L"Modificado: ") + DateText(e.mtime);
            dc->DrawTextW(date.c_str(), (UINT32)date.size(), tfI, D2D1::RectF(body.left, y + 38, body.right, y + 54), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
    }
    if ((video && QlControlsVisible()) || audio) {  // os controles: barra escura com ⏵/⏸, tempos, linha do tempo e o som
        auto b = QlCtlBar();
        br->SetColor(D2D1::ColorF(0, 0, 0, 0.62f));
        dc->FillRoundedRectangle(D2D1::RoundedRect(b, 8, 8), br);
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.25f));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(Inflate(b, -0.5f), 7.5f, 7.5f), br, 1);
        auto pr = QlPlayRect();
        float pcx = (pr.left + pr.right) / 2, pcy = (pr.top + pr.bottom) / 2;
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.95f));
        if (media::Paused()) {
            ID2D1Factory* f = nullptr; dc->GetFactory(&f);
            ID2D1PathGeometry* g = nullptr;
            if (f && SUCCEEDED(f->CreatePathGeometry(&g))) {
                ID2D1GeometrySink* sk = nullptr;
                if (SUCCEEDED(g->Open(&sk))) {
                    sk->BeginFigure({ pcx - 4, pcy - 6 }, D2D1_FIGURE_BEGIN_FILLED); sk->AddLine({ pcx + 6, pcy }); sk->AddLine({ pcx - 4, pcy + 6 });
                    sk->EndFigure(D2D1_FIGURE_END_CLOSED); sk->Close(); sk->Release();
                    dc->FillGeometry(g, br);
                }
                g->Release();
            }
            if (f) f->Release();
        } else {
            dc->FillRectangle(D2D1::RectF(pcx - 5, pcy - 6, pcx - 1.5f, pcy + 6), br);
            dc->FillRectangle(D2D1::RectF(pcx + 1.5f, pcy - 6, pcx + 5, pcy + 6), br);
        }
        auto mr = QlMuteRect();
        float mcx = (mr.left + mr.right) / 2, mcy = (mr.top + mr.bottom) / 2;  // alto-falante
        dc->FillRectangle(D2D1::RectF(mcx - 6, mcy - 2.5f, mcx - 3, mcy + 2.5f), br);
        Line(dc, br, mcx - 3, mcy - 2.5f, mcx + 1, mcy - 6, 1.6f); Line(dc, br, mcx + 1, mcy - 6, mcx + 1, mcy + 6, 1.6f); Line(dc, br, mcx + 1, mcy + 6, mcx - 3, mcy + 2.5f, 1.6f);
        if (!media::Muted()) dc->DrawEllipse(D2D1::Ellipse({ mcx + 2, mcy }, 4.5f, 4.5f), br, 1.2f);
        double d = media::Duration(), t = media::Ended() ? d : media::Time();
        auto tr = QlTrackRect();
        float ty = (tr.top + tr.bottom) / 2, f = d > 0 ? (float)std::clamp(t / d, 0.0, 1.0) : 0;
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.28f));
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tr.left, ty - 2, tr.right, ty + 2), 2, 2), br);
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.95f));
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(tr.left, ty - 2, tr.left + (tr.right - tr.left) * f, ty + 2), 2, 2), br);
        dc->FillEllipse(D2D1::Ellipse({ tr.left + (tr.right - tr.left) * f, ty }, 4.5f, 4.5f), br);
        auto hms = [](double sec) { int x = (int)std::max(0.0, sec); wchar_t b2[16]; swprintf_s(b2, L"%d:%02d", x / 60, x % 60); return std::wstring(b2); };
        std::wstring a = hms(t), z = L"-" + hms(d - t);
        dc->DrawTextW(a.c_str(), (UINT32)a.size(), tfI, D2D1::RectF(pr.right, tr.top, tr.left - 4, tr.bottom), br);
        dc->DrawTextW(z.c_str(), (UINT32)z.size(), tfI, D2D1::RectF(tr.right + 4, tr.top, mr.left, tr.bottom), br);
    }
}

static void DrawQuickLook(ID2D1DeviceContext* dc, const Palette& pal, ID2D1SolidColorBrush* br) {
    int i = QlItem();
    if (i < 0) { g_ql = false; return; }
    const Entry& e = g_items[i];
    auto r = QlPanel();
    float ease;
    if (g_qlClosing) {  // fechando: acelera de volta para o ícone
        float t = std::min(1.f, (GetTickCount() - g_qlClosedAt) / (float)QL_CLOSE_MS);
        ease = 1 - t * t;
        if (t >= 1) { g_ql = g_qlClosing = false; return; }
    } else {
        float t = std::min(1.f, (GetTickCount() - g_qlOpenedAt) / (float)QL_ANIM_MS);
        float u = 1 - t;
        ease = 1 - u * u * u * u;  // sai rápido e assenta
    }
    D2D1_POINT_2F mid{ (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
    D2D1_MATRIX_3X2_F old;
    dc->GetTransform(&old);
    float pw = r.right - r.left, ph = r.bottom - r.top;
    if (g_qlFrom.right > g_qlFrom.left && pw > 1 && ph > 1) {  // do ícone até o painel: escala e centro andam juntos
        float s0 = std::max((g_qlFrom.right - g_qlFrom.left) / pw, (g_qlFrom.bottom - g_qlFrom.top) / ph);
        float scale = s0 + (1 - s0) * ease;
        float fx = (g_qlFrom.left + g_qlFrom.right) / 2, fy = (g_qlFrom.top + g_qlFrom.bottom) / 2;
        float cx = fx + (mid.x - fx) * ease, cy = fy + (mid.y - fy) * ease;
        dc->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale, mid) * D2D1::Matrix3x2F::Translation(cx - mid.x, cy - mid.y) * old);
    } else {
        float scale = 0.94f + 0.06f * ease;
        dc->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale, mid) * old);
    }
    ease = std::min(1.f, ease * 1.6f);  // a opacidade chega antes do tamanho
    ID2D1Layer* layer = nullptr;
    if (ease < 1) {  // fade de entrada
        dc->CreateLayer(nullptr, &layer);
        dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), ease), layer);
    }

    if (!g_tahoe && tahoe::Leopard()) {  // Leopard: o HUD do 10.5 (desenho próprio)
        DrawQuickLookLeopard(dc, br, e, r);
        if (layer) { dc->PopLayer(); layer->Release(); }
        dc->SetTransform(old);
        return;
    }
    for (int k = 1; k <= 10; k++) {  // sombra grande e suave, como as janelas do Mac
        br->SetColor(D2D1::ColorF(0, 0, 0, 0.03f));
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left - k * 2.5f, r.top - k * 1.5f + 10, r.right + k * 2.5f, r.bottom + k * 3.f + 10), 12 + k * 2.f, 12 + k * 2.f), br);
    }
    const bool gg = semaforo::Aqua();
    D2D1_COLOR_F bg = g_dark ? D2D1::ColorF(0.11f, 0.11f, 0.12f, 1) : D2D1::ColorF(0.97f, 0.97f, 0.975f, 1);
    if (gg) bg = g_dark ? D2D1::ColorF(0.18f, 0.18f, 0.19f, 1) : D2D1::ColorF(0.945f, 0.94f, 0.935f, 1);  // moldura clara
    br->SetColor(bg);
    dc->FillRoundedRectangle(D2D1::RoundedRect(r, gg ? 16.f : 12.f, gg ? 16.f : 12.f), br);
    if (gg) {  // vidro: a barra escurece no meio e clareia em cima e perto da foto (medido: 240 -> 196 -> 238), fio claro em cima
        D2D1_COLOR_F f1 = g_dark ? D2D1::ColorF(0.13f, 0.13f, 0.14f, 1) : D2D1::ColorF(0.80f, 0.78f, 0.76f, 1);
        D2D1_GRADIENT_STOP st[3] = { { 0, bg }, { 0.45f, f1 }, { 1, bg } };
        ID2D1GradientStopCollection* gs = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
        if (SUCCEEDED(dc->CreateGradientStopCollection(st, 3, &gs)) && SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, r.top }, { 0, r.top + QL_TITLE_H }), gs, &lb)))
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left, r.top, r.right, r.top + QL_TITLE_H + 16), 16, 16), lb);
        if (lb) lb->Release();
        if (gs) gs->Release();
        br->SetColor(bg);
        dc->FillRectangle(D2D1::RectF(r.left, r.top + QL_TITLE_H, r.right, r.top + QL_TITLE_H + 16), br);
        br->SetColor(D2D1::ColorF(1, 1, 1, g_dark ? 0.18f : 0.7f));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), 15.5f, 15.5f), br, 1);
    }

    ID2D1Bitmap* bmp = QlImage(e);
    bool text = !e.dir && IsTextFile(e);
    bool video = QlVideoOn(e), audio = !video && QlAudioOn(e);
    D2D1_RECT_F body{ r.left, r.top + QL_TITLE_H, r.right, r.bottom };
    if (gg) body = D2D1::RectF(r.left + 5, r.top + QL_TITLE_H, r.right - 5, r.bottom - 5);  // macOS 27: moldura de 5
    if (video) {  // vídeo: quadro atual (ou a miniatura enquanto o primeiro não chega) no corpo todo
        ID2D1RoundedRectangleGeometry* clip = nullptr;
        ID2D1Factory* f = nullptr; dc->GetFactory(&f);
        f->CreateRoundedRectangleGeometry(D2D1::RoundedRect(r, 12, 12), &clip);
        ID2D1Layer* cl = nullptr; dc->CreateLayer(nullptr, &cl);
        dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip), cl);
        br->SetColor(D2D1::ColorF(0, 0, 0));
        dc->FillRectangle(body, br);
        float vw = (float)media::VideoW(), vh = (float)media::VideoH();
        float k = std::min((body.right - body.left) / vw, (body.bottom - body.top) / vh);
        float w = vw * k, h = vh * k, cx = (body.left + body.right) / 2, cy = (body.top + body.bottom) / 2;
        D2D1_RECT_F vr{ cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2 };
        if (auto* fr = media::Frame()) dc->DrawBitmap(fr, vr, 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        else if (bmp) dc->DrawBitmap(bmp, vr, 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        if (QlControlsVisible()) DrawQlControls(dc, pal, br, true);
        dc->PopLayer(); cl->Release(); clip->Release(); f->Release();
    } else if (audio) {  // música: capa (ou ícone), título, artista e controles
        float size = 210, cx = (body.left + body.right) / 2, top = body.top + 26;
        D2D1_RECT_F art{ cx - size / 2, top, cx + size / 2, top + size };
        if (bmp) {
            auto sz = bmp->GetPixelSize();
            float k = std::min(size / sz.width, size / sz.height), w = sz.width * k, h = sz.height * k;
            D2D1_RECT_F ar{ cx - w / 2, top + (size - h) / 2, cx + w / 2, top + (size + h) / 2 };
            for (int s2 = 1; s2 <= 4; s2++) {  // sombra da capa
                br->SetColor(D2D1::ColorF(0, 0, 0, 0.05f));
                dc->FillRoundedRectangle(D2D1::RoundedRect(Inflate(ar, s2 * 1.5f), 6.f + s2, 6.f + s2), br);
            }
            dc->DrawBitmap(bmp, ar, 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else DrawBigIcon(dc, e, art, br);
        std::wstring title = g_qlMediaTitle.empty() ? e.label : g_qlMediaTitle;
        float y = art.bottom + 18;
        br->SetColor(pal.text);
        dc->DrawTextW(title.c_str(), (UINT32)title.size(), g_tfQlTitle, D2D1::RectF(body.left + 24, y, body.right - 24, y + 20), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        if (!g_qlMediaArtist.empty()) {
            br->SetColor(pal.text2);
            dc->DrawTextW(g_qlMediaArtist.c_str(), (UINT32)g_qlMediaArtist.size(), g_tfQlArtist, D2D1::RectF(body.left + 24, y + 22, body.right - 24, y + 42), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        DrawQlControls(dc, pal, br, false);
    } else if (bmp) {  // foto: ocupa o corpo todo
        ID2D1RoundedRectangleGeometry* clip = nullptr;
        ID2D1Factory* f = nullptr; dc->GetFactory(&f);
        f->CreateRoundedRectangleGeometry(gg ? D2D1::RoundedRect(body, 12, 12) : D2D1::RoundedRect(r, 12, 12), &clip);
        ID2D1Layer* cl = nullptr; dc->CreateLayer(nullptr, &cl);
        dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip), cl);
        auto sz = bmp->GetPixelSize();
        float k = gg ? std::max((body.right - body.left) / sz.width, (body.bottom - body.top) / sz.height)  // macOS 27: a foto preenche a moldura
                     : std::min((body.right - body.left) / sz.width, (body.bottom - body.top) / sz.height);
        float w = sz.width * k, h = sz.height * k, cx = (body.left + body.right) / 2, cy = (body.top + body.bottom) / 2;
        DrawBigIcon(dc, e, D2D1::RectF(cx - w / 2, cy - h / 2, cx + w / 2, cy + h / 2), br);
        dc->PopLayer(); cl->Release(); clip->Release(); f->Release();
    } else if (text) {  // texto: folha clara com fonte monoespaçada
        LoadQlText(e.path);
        D2D1_RECT_F page{ body.left + 14, body.top + 4, body.right - 14, body.bottom - 14 };
        br->SetColor(g_dark ? D2D1::ColorF(0.16f, 0.16f, 0.17f) : D2D1::ColorF(1, 1, 1));
        dc->FillRoundedRectangle(D2D1::RoundedRect(page, 6, 6), br);
        dc->PushAxisAlignedClip(Inflate(page, -10), D2D1_ANTIALIAS_MODE_ALIASED);
        br->SetColor(pal.text);
        float y = page.top + 12;
        for (auto& ln : g_qlLines) {
            if (y > page.bottom) break;
            dc->DrawTextW(ln.c_str(), (UINT32)ln.size(), g_tfMono, D2D1::RectF(page.left + 14, y, page.right - 10, y + 17), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            y += 17;
        }
        dc->PopAxisAlignedClip();
    } else {  // pasta ou arquivo sem prévia: ícone grande e informações
        float size = std::min(200.f, (body.bottom - body.top) - 120);
        float cx = (body.left + body.right) / 2, top = body.top + std::max(8.f, ((body.bottom - body.top) - size - 100) / 2);
        DrawBigIcon(dc, e, D2D1::RectF(cx - size / 2, top, cx + size / 2, top + size), br);
        float y = top + size + 16;
        br->SetColor(pal.text);
        dc->DrawTextW(e.label.c_str(), (UINT32)e.label.size(), g_tfPreviewName, D2D1::RectF(body.left + 20, y, body.right - 20, y + 40), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        std::wstring info = KindName(e);
        if (e.dir) { size_t n = ChildItems(e.path).size(); info += L" · " + std::to_wstring(n) + (n == 1 ? Tr(L" item") : Tr(L" itens")); }
        else info += L" · " + SizeText(e);
        std::wstring date = Tr(L"Modificado: ") + DateText(e.mtime);
        br->SetColor(pal.text2);
        dc->DrawTextW(info.c_str(), (UINT32)info.size(), g_tfPreviewInfo, D2D1::RectF(body.left, y + 42, body.right, y + 60), br);
        dc->DrawTextW(date.c_str(), (UINT32)date.size(), g_tfPreviewInfo, D2D1::RectF(body.left, y + 60, body.right, y + 78), br);
    }

    // barra de título (por cima da foto, translúcida, como no Mac)
    D2D1_RECT_F bar{ r.left, r.top, r.right, r.top + QL_TITLE_H };
    {
        ID2D1RoundedRectangleGeometry* clip = nullptr;
        ID2D1Factory* f = nullptr; dc->GetFactory(&f);
        f->CreateRoundedRectangleGeometry(D2D1::RoundedRect(r, 12, 12), &clip);
        ID2D1Layer* cl = nullptr; dc->CreateLayer(nullptr, &cl);
        dc->PushLayer(D2D1::LayerParameters(bar, clip), cl);
        br->SetColor(g_dark ? D2D1::ColorF(0.19f, 0.19f, 0.2f, bmp || video ? 0.86f : 1.f) : D2D1::ColorF(0.93f, 0.93f, 0.935f, bmp || video ? 0.86f : 1.f));
        if (!gg) dc->FillRectangle(bar, br);  // macOS 27: a barra é a própria moldura de vidro (já pintada)
        dc->PopLayer(); cl->Release(); clip->Release(); f->Release();
    }
    if (!gg) { br->SetColor(pal.line); dc->DrawLine({ r.left, bar.bottom - 0.5f }, { r.right, bar.bottom - 0.5f }, br, 1); }
    auto cr = QlCloseRect();
    float ccx = (cr.left + cr.right) / 2, ccy = (cr.top + cr.bottom) / 2;
    if (gg) {  // macOS 27: círculo escuro cheio com o × claro
        br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.75f) : D2D1::ColorF(0.24f, 0.22f, 0.21f, 0.9f));
        dc->FillEllipse(D2D1::Ellipse({ ccx, ccy }, 8, 8), br);
        br->SetColor(bg);
        Line(dc, br, ccx - 2.8f, ccy - 2.8f, ccx + 2.8f, ccy + 2.8f, 1.6f);
        Line(dc, br, ccx - 2.8f, ccy + 2.8f, ccx + 2.8f, ccy - 2.8f, 1.6f);
    } else {
    br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.16f) : D2D1::ColorF(0, 0, 0, 0.10f));
    dc->FillEllipse(D2D1::Ellipse({ ccx, ccy }, 9, 9), br);
    br->SetColor(pal.text2);
    Line(dc, br, ccx - 3, ccy - 3, ccx + 3, ccy + 3, 1.5f);
    Line(dc, br, ccx - 3, ccy + 3, ccx + 3, ccy - 3, 1.5f);
    }
    auto orr = QlOpenRect(), sr = QlShareRect();
    br->SetColor(pal.text);
    if (gg) {  // macOS 27: título em negrito de 15 à esquerda, logo depois do fechar
        static IDWriteTextFormat* tfT = Format(g_uiFamily.c_str(), 15, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
        dc->DrawTextW(e.label.c_str(), (UINT32)e.label.size(), tfT, D2D1::RectF(cr.right + 9, bar.top, sr.left - 8, bar.bottom), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    } else
    dc->DrawTextW(e.label.c_str(), (UINT32)e.label.size(), g_tfQlTitle, D2D1::RectF(cr.right + 12, bar.top, sr.left - 8, bar.bottom), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.12f) : D2D1::ColorF(0, 0, 0, 0.07f));
    dc->FillRoundedRectangle(D2D1::RoundedRect(orr, gg ? 13.f : 6.f, gg ? 13.f : 6.f), br);
    std::wstring app = DefaultAppName(e);
    std::wstring openLabel = e.dir ? Tr(L"Abrir") : app.empty() ? Tr(L"Abrir") : Tr(L"Abrir com ") + app;
    br->SetColor(pal.text);
    dc->DrawTextW(openLabel.c_str(), (UINT32)openLabel.size(), g_tfQlButton, orr, br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    br->SetColor(pal.text2);
    DrawToolIcon(dc, TB_SHARE, sr, br);

    if (layer) { dc->PopLayer(); layer->Release(); }
    dc->SetTransform(old);
}

// Clique com o Quick Look aberto: true quando o clique foi no painel (consumido).
static bool QlClick(D2D1_POINT_2F p) {
    if (!g_ql) return false;
    if (!In(QlPanel(), p)) return false;  // fora do painel: o Arquivos segue normal (como no Mac)
    int i = QlItem();
    if (i >= 0 && QlMediaOn(g_items[i]) && !In(D2D1::RectF(QlPanel().left, QlPanel().top, QlPanel().right, QlPanel().top + QL_TITLE_H), p)) {
        bool vid = QlVideoOn(g_items[i]);
        g_qlMouseAt = GetTickCount();
        bool bar = In(QlCtlBar(), p) && (!vid || QlControlsVisible());
        if (bar && In(QlPlayRect(), p)) media::TogglePlay();
        else if (bar && In(QlMuteRect(), p)) media::SetMuted(!media::Muted());
        else if (bar && In(Inflate(QlTrackRect(), 2), p)) { g_qlSeekDrag = true; SetCapture(g_hwnd); QlSeekAt(p.x); }
        else if (vid && !bar) media::TogglePlay();  // clique no vídeo pausa/continua
        Invalidate();
        return true;
    }
    if (In(QlCloseRect(), p)) QlClose();
    else if (i >= 0 && In(QlOpenRect(), p)) { g_ql = false; Open(i); }
    else if (i >= 0 && In(QlShareRect(), p)) RunVerb("windows.modernshare");
    return true;
}

static void DrawViewOptions(ID2D1DeviceContext* dc, const Palette& pal, ID2D1SolidColorBrush* br);
static bool g_viewOpts;
static void DrawPicker(ID2D1DeviceContext* dc, const Palette& pal, ID2D1SolidColorBrush* br);
static void DrawTabBar(ID2D1DeviceContext* dc, const Palette& pal, ID2D1SolidColorBrush* br);
static void DrawScopeBar(ID2D1DeviceContext* dc, const Palette& pal, ID2D1SolidColorBrush* br);
static void DrawTrashBar(ID2D1DeviceContext* dc, const Palette& pal, ID2D1SolidColorBrush* br);
static bool g_trashPress;

// ---------------------------------------------------------------------------
// Ir para a Pasta (Ctrl+Shift+G), como o do macOS 27 (captura Finder-Go-To-Folder, 2x): painel de 460 × 183 no alto da
// janela, cantos 16; campo de 34 com o texto de 17 a 16 da borda e o × cinza à direita, divisa embaixo; "Ir para:" em
// cinza e as pastas sugeridas em linhas de 22 com o caminho separado por "›" e a parte digitada em negrito; a escolhida
// numa pílula azul (cantos 11, a 10 das bordas). "~" é a pasta do usuário.
static bool g_goOpen;
static TextField g_go;
static std::vector<std::wstring> g_goSugg;
static int g_goSel;

static std::wstring GoExpand(std::wstring t) {
    wchar_t prof[MAX_PATH] = {};
    GetEnvironmentVariableW(L"USERPROFILE", prof, MAX_PATH);
    if (!t.empty() && t[0] == L'~') t = std::wstring(prof) + t.substr(1);
    for (auto& c : t) if (c == L'/') c = L'\\';
    wchar_t buf[2048] = {};
    if (ExpandEnvironmentStringsW(t.c_str(), buf, 2048)) t = buf;
    return t;
}

static void GoRefresh() {
    g_goSugg.clear();
    g_goSel = 0;
    std::wstring full = GoExpand(g_go.text);
    if (full.empty()) return;
    size_t cut = full.find_last_of(L'\\');
    std::wstring dir = cut == std::wstring::npos ? full : full.substr(0, cut + 1), pre = cut == std::wstring::npos ? L"" : full.substr(cut + 1);
    if (dir.size() == 2 && dir[1] == L':') dir += L'\\';
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.' || (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)) continue;
        if (_wcsnicmp(fd.cFileName, pre.c_str(), pre.size())) continue;
        g_goSugg.push_back(dir + fd.cFileName);
    } while (FindNextFileW(h, &fd) && g_goSugg.size() < 6);
    FindClose(h);
}

static void GoOpen(bool on) {
    g_goOpen = on;
    if (on) { g_go.SetText(L"", false); GoRefresh(); g_caretOn = true; }
    Invalidate();
}

static void GoAccept() {
    std::wstring target = g_goSel >= 0 && g_goSel < (int)g_goSugg.size() ? g_goSugg[g_goSel] : GoExpand(g_go.text);
    while (target.size() > 3 && target.back() == L'\\') target.pop_back();
    DWORD a = GetFileAttributesW(target.c_str());
    if (a == INVALID_FILE_ATTRIBUTES) { MessageBeep(MB_ICONWARNING); return; }
    GoOpen(false);
    if (a & FILE_ATTRIBUTE_DIRECTORY) Navigate(target);
    else { size_t k = target.find_last_of(L'\\'); Navigate(target.substr(0, k), true, target.substr(k + 1)); }
}

static D2D1_RECT_F GoRect() {
    auto s = ClientDip();
    float w = std::min(460.f, s.width - 20), x = (s.width - w) / 2;
    return D2D1::RectF(x, 8, x + w, 8 + 183);
}

static bool GoKey(UINT vk, bool shift, bool ctrl) {
    if (!g_goOpen) return false;
    if (vk == VK_ESCAPE) { GoOpen(false); return true; }
    if (vk == VK_RETURN) { GoAccept(); return true; }
    if (vk == VK_DOWN) { if (!g_goSugg.empty()) g_goSel = std::min(g_goSel + 1, (int)g_goSugg.size() - 1); Invalidate(); return true; }
    if (vk == VK_UP) { g_goSel = std::max(g_goSel - 1, 0); Invalidate(); return true; }
    if (vk == VK_TAB && g_goSel >= 0 && g_goSel < (int)g_goSugg.size()) {  // completa com a escolhida
        g_go.SetText(g_goSugg[g_goSel] + L"\\", false); g_go.Select((int)g_go.text.size(), (int)g_go.text.size()); GoRefresh(); Invalidate(); return true;
    }
    if (g_go.OnKey(vk, shift, ctrl)) { GoRefresh(); Invalidate(); }
    return true;  // com o painel aberto, as teclas não vão para a lista de arquivos
}

static bool GoClick(D2D1_POINT_2F p) {
    if (!g_goOpen) return false;
    D2D1_RECT_F r = GoRect();
    if (!In(r, p)) { GoOpen(false); return true; }
    if (p.y < r.top + 34 && p.x > r.right - 40) { GoOpen(false); return true; }  // o ×
    float y0 = r.top + 68;
    int i = (int)std::floor((p.y - y0) / 22);
    if (p.y >= y0 && i >= 0 && i < (int)g_goSugg.size()) { g_goSel = i; GoAccept(); }
    return true;
}

static void DrawGoTo(ID2D1DeviceContext* dc, const Palette& pal, ID2D1SolidColorBrush* br) {
    if (!g_goOpen) return;
    D2D1_RECT_F r = GoRect();
    static IDWriteTextFormat *tfField, *tfRow, *tfRowB, *tfHead;
    if (!tfField) {
        tfField = Format(g_uiFamily.c_str(), 17, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
        tfRow = Format(g_uiFamily.c_str(), 15, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
        tfRowB = Format(g_uiFamily.c_str(), 15, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
        tfHead = Format(g_uiFamily.c_str(), 13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    }
    for (int k = 4; k >= 1; k--) {  // sombra
        br->SetColor(D2D1::ColorF(0, 0, 0, g_dark ? 0.10f : 0.05f));
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left - k * 1.5f, r.top - k + 4, r.right + k * 1.5f, r.bottom + k * 2.5f), 16 + k, 16 + k), br);
    }
    br->SetColor(g_dark ? D2D1::ColorF(0.17f, 0.17f, 0.18f, 1) : D2D1::ColorF(0.98f, 0.98f, 0.98f, 1));
    dc->FillRoundedRectangle(D2D1::RoundedRect(r, 16, 16), br);
    br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.14f) : D2D1::ColorF(0, 0, 0, 0.10f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), 15.5f, 15.5f), br, 1);
    // campo
    D2D1_RECT_F fr = D2D1::RectF(r.left + 16, r.top, r.right - 44, r.top + 34);
    ID2D1SolidColorBrush *sel = nullptr, *txt = nullptr;
    dc->CreateSolidColorBrush(D2D1::ColorF(pal.accent.r, pal.accent.g, pal.accent.b, 0.30f), &sel);
    dc->CreateSolidColorBrush(pal.text, &txt);
    if (g_go.text.empty()) {
        br->SetColor(pal.text2);
        std::wstring ph = Tr(L"Digite um caminho");
        dc->DrawTextW(ph.c_str(), (UINT32)ph.size(), tfField, fr, br);
    }
    if (txt && sel) g_go.Draw(dc, tfField, fr, txt, sel, txt, g_caretOn, false);
    if (sel) sel->Release();
    if (txt) txt->Release();
    D2D1_POINT_2F xc{ r.right - 24, r.top + 17 };  // × cinza
    br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.45f) : D2D1::ColorF(0, 0, 0, 0.45f));
    dc->FillEllipse(D2D1::Ellipse(xc, 7.5f, 7.5f), br);
    br->SetColor(g_dark ? D2D1::ColorF(0.17f, 0.17f, 0.18f, 1) : D2D1::ColorF(1, 1, 1, 1));
    dc->DrawLine({ xc.x - 3, xc.y - 3 }, { xc.x + 3, xc.y + 3 }, br, 1.5f);
    dc->DrawLine({ xc.x - 3, xc.y + 3 }, { xc.x + 3, xc.y - 3 }, br, 1.5f);
    br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.10f) : D2D1::ColorF(0, 0, 0, 0.10f));
    dc->FillRectangle(D2D1::RectF(r.left, r.top + 34, r.right, r.top + 35), br);
    if (g_goSugg.empty()) return;
    br->SetColor(pal.text2);
    std::wstring head = Tr(L"Ir para:");
    dc->DrawTextW(head.c_str(), (UINT32)head.size(), tfHead, D2D1::RectF(r.left + 18, r.top + 46, r.right - 18, r.top + 66), br);
    std::wstring typed = GoExpand(g_go.text);
    size_t cut = typed.find_last_of(L'\\');
    size_t preLen = cut == std::wstring::npos ? 0 : typed.size() - cut - 1;
    for (int i = 0; i < (int)g_goSugg.size(); i++) {
        float y = r.top + 68 + i * 22;
        if (y + 22 > r.bottom - 4) break;
        bool on = i == g_goSel;
        if (on) { br->SetColor(pal.accent); dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 10, y, r.right - 10, y + 22), 11, 11), br); }
        // as três últimas partes do caminho, separadas por "›"; a parte digitada do nome em negrito
        std::vector<std::wstring> parts;
        std::wstring p = g_goSugg[i];
        size_t st = 0;
        while (st < p.size()) { size_t e = p.find(L'\\', st); if (e == std::wstring::npos) e = p.size(); if (e > st) parts.push_back(p.substr(st, e - st)); st = e + 1; }
        if (parts.size() > 3) parts.erase(parts.begin(), parts.end() - 3);
        float x = r.left + 18;
        D2D1_COLOR_F col = on ? D2D1::ColorF(1, 1, 1, 1) : pal.text;
        for (size_t k = 0; k < parts.size(); k++) {
            bool last = k + 1 == parts.size();
            auto draw = [&](const std::wstring& s, IDWriteTextFormat* tf) {
                IDWriteTextLayout* lay = nullptr;
                if (FAILED(gfx::dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), tf, 1000, 22, &lay))) return;
                DWRITE_TEXT_METRICS m{}; lay->GetMetrics(&m);
                br->SetColor(col);
                dc->DrawTextLayout({ x, y }, lay, br);
                x += m.widthIncludingTrailingWhitespace;
                lay->Release();
            };
            if (last && preLen > 0 && preLen <= parts[k].size()) { draw(parts[k].substr(0, preLen), tfRowB); draw(parts[k].substr(preLen), tfRow); }
            else draw(parts[k], tfRow);
            if (!last) { x += 6; draw(L"›", tfRow); x += 6; }
        }
    }
}

static void Render() {
    QlSyncMedia();
    media::UpdateFrame();
    auto* dc = gfx::Begin(g_dpi);
    if (!dc) return;
    if (!g_round) {
        ID2D1Factory* f = nullptr;
        dc->GetFactory(&f);
        f->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND), nullptr, 0, &g_round);
        f->Release();
    }
    Palette pal = Colors();
    auto s = ClientDip();
    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(pal.text, &br);

    // barra lateral transparente (acrílico do DWM); barras de cima e de baixo sólidas, como no Finder
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    float winR = g_tahoe ? tahoe::Radius(g_hwnd) : 0;
    bool winClip = g_tahoe && tahoe::PushWindowClip(dc, s, winR);
    if (Golden()) {
        pal.bar = pal.content;
        tahoe::SetPanel(g_hwnd, D2D1::RectF(0, 0, 0, 0), 0);
        br->SetColor(g_dark ? D2D1::ColorF(43 / 255.f, 41 / 255.f, 40 / 255.f, 1) : D2D1::ColorF(237 / 255.f, 237 / 255.f, 236 / 255.f, 1));
        dc->FillRectangle(D2D1::RectF(0, 0, Lay::SIDEBAR_W, s.height), br);
        br->SetColor(pal.content);
        dc->FillRectangle(D2D1::RectF(Lay::SIDEBAR_W, 0, s.width, s.height), br);
    } else if (g_tahoe) {
        // Tahoe: a janela inteira na cor do conteúdo, com a barra lateral num painel de vidro flutuante
        // (o desfoque da janela aparece só dentro dele); barras de cima e de baixo sem faixa nem divisa
        pal.bar = pal.content;
        D2D1_RECT_F panel = SidePanelRect();
        tahoe::SetPanel(g_hwnd, panel, SidePanelR());
        ID2D1Factory* f = nullptr; dc->GetFactory(&f);

        ID2D1RectangleGeometry* all = nullptr; ID2D1PathGeometry* hole = nullptr; ID2D1PathGeometry* rest = nullptr;
        f->CreateRectangleGeometry(D2D1::RectF(0, 0, s.width, s.height), &all);
        hole = tahoe::Squircle(f, panel, SidePanelR());  // canto contínuo
        f->CreatePathGeometry(&rest);
        ID2D1GeometrySink* sk = nullptr;
        if (all && hole && rest && SUCCEEDED(rest->Open(&sk))) {
            all->CombineWithGeometry(hole, D2D1_COMBINE_MODE_EXCLUDE, nullptr, sk);
            sk->Close(); sk->Release();
            br->SetColor(pal.content);
            dc->FillGeometry(rest, br);
        }
        if (rest) rest->Release();
        if (all) all->Release();
        f->Release();
        br->SetColor(g_dark ? D2D1::ColorF(0, 0, 0, 0.10f) : D2D1::ColorF(1, 1, 1, 0.18f));  // tinta do painel
        if (hole) { dc->FillGeometry(hole, br); hole->Release(); }
    } else {
        br->SetColor(pal.side);
        dc->FillRectangle(D2D1::RectF(0, 0, Lay::SIDEBAR_W, s.height), br);
        br->SetColor(pal.content);
        dc->FillRectangle(D2D1::RectF(Lay::SIDEBAR_W, 0, s.width, s.height), br);
    }
    ID2D1DeviceContext* idc = g_tahoe ? tahoe::SceneBegin(dc, 0, s, g_dpi) : nullptr;
    if (idc) {  // Tahoe: o conteúdo rola por baixo da barra de ferramentas, desfocado e sumindo (sem faixa opaca)
        g_itemsScene = true;
        DrawItems(idc, pal, br);
        g_itemsScene = false;
        tahoe::SceneEnd(0);
        { static int n; if (n++ < 2) Log(L"cena: conteúdo %.2f %.2f %.2f %.2f", pal.content.r, pal.content.g, pal.content.b, pal.content.a); }
        tahoe::SceneDraw(dc, 0, D2D1::RectF(Lay::SIDEBAR_W, 0, s.width, s.height - Lay::FOOTER_H), Lay::TOOLBAR_H + 10, D2D1::ColorF(pal.content.r, pal.content.g, pal.content.b, 0.9f));
        if (g_view == View::List) DrawListHeader(dc, pal, br);
    } else {
    DrawItems(dc, pal, br);
    D2D1_RECT_F top{ Lay::SIDEBAR_W, 0, s.width, Lay::TOOLBAR_H };
    br->SetColor(pal.bar);  // tema escuro: barra um tom mais clara que a lista
    dc->FillRectangle(top, br);
    }
    const bool leo = !g_tahoe && tahoe::Leopard();
    br->SetColor(pal.bar);  // status também um tom mais claro; a barra de caminho fica na cor da lista
    dc->FillRectangle(D2D1::RectF(Lay::SIDEBAR_W, s.height - Lay::STATUS_H, s.width, s.height), br);
    if (leo) {
        // Leopard: a janela "unified" do 10.5 — barra de título e de ferramentas numa peça só de metal, de ponta a
        // ponta (por cima da barra lateral também), e o rodapé no mesmo metal
        leopard::Metal(dc, D2D1::RectF(0, 0, s.width, Lay::TOOLBAR_H), g_winActive, g_dark, true);
        leopard::Metal(dc, D2D1::RectF(Lay::SIDEBAR_W, s.height - Lay::STATUS_H, s.width, s.height), g_winActive, g_dark, false);
        br->SetColor(g_dark ? D2D1::ColorF(0, 0, 0, 0.6f) : D2D1::ColorF(0.55f, 0.58f, 0.62f, 1));  // divisa da barra lateral
        dc->DrawLine({ Lay::SIDEBAR_W + 0.5f, Lay::TOOLBAR_H }, { Lay::SIDEBAR_W + 0.5f, s.height }, br, 1);
    } else if (Golden()) {
    } else if (g_tahoe) {
        tahoe::Panel(dc, br, SidePanelRect(), SidePanelR(), g_dark);
    } else {
        br->SetColor(g_dark ? D2D1::ColorF(0, 0, 0, 0.6f) : D2D1::ColorF(0, 0, 0, 0.12f));  // divisas escuras
        dc->DrawLine({ Lay::SIDEBAR_W + 0.5f, 0 }, { Lay::SIDEBAR_W + 0.5f, s.height }, br, 1);
        dc->DrawLine({ Lay::SIDEBAR_W, Lay::TOOLBAR_H - 0.5f }, { s.width, Lay::TOOLBAR_H - 0.5f }, br, 1);
    }

    if (ID2D1DeviceContext* sdc = g_tahoe ? tahoe::SceneBegin(dc, 1, s, g_dpi) : nullptr) {
        // Tahoe: a lista lateral rola por baixo dos botões da janela, desfocada
        D2D1_RECT_F panel = SidePanelRect();
        g_sideClipTop = panel.top;
        DrawSidebar(sdc, pal, br);
        g_sideClipTop = Lay::SIDE_TOP;
        tahoe::SceneEnd(1);
        tahoe::SceneDraw(dc, 1, panel, Lay::SIDE_TOP + 4 - panel.top);
        DrawLights(dc, br);
    } else {
    DrawLights(dc, br);
    DrawSidebar(dc, pal, br);
    }
    if (CoverFlowH() > 0) DrawCoverFlow(dc, br);  // Leopard: o painel de capas em cima da lista
    DrawToolbar(dc, pal, br);
    DrawScrollbar(dc, pal, br);
    DrawRenameField(dc, pal, br);

    if (g_pick.on) DrawPicker(dc, pal, br);
    else {
        std::wstring st = StatusText();
        br->SetColor(pal.text2);
        dc->DrawTextW(st.c_str(), (UINT32)st.size(), g_tfStatus, D2D1::RectF(Lay::SIDEBAR_W, s.height - Lay::STATUS_H, s.width, s.height), br);
    }
    DrawPathBar(dc, pal, br);
    if (g_view == View::Icons && !g_pick.on) DrawSlider(dc, pal, br);  // só nessa visão; no Abrir/Salvar o rodapé é dos botões
    DrawTabBar(dc, pal, br);
    DrawScopeBar(dc, pal, br);
    DrawTrashBar(dc, pal, br);
    if (g_viewOpts) DrawViewOptions(dc, pal, br);
    QlSyncShell();
    if (g_ql) DrawQuickLook(dc, pal, br);
    DrawGoTo(dc, pal, br);

    if (winClip) { dc->PopLayer(); tahoe::WindowRim(dc, br, s, winR, g_dark); }
    br->Release();
    gfx::End();
}

// ---------------------------------------------------------------------------
// Entrada

static void UpdateBand(D2D1_POINT_2F viewPt) {
    auto c = ContentRect();
    g_bandEnd = { std::clamp(viewPt.x, c.left, c.right), viewPt.y - c.top + g_scroll };
    D2D1_RECT_F b{ std::min(g_bandStart.x, g_bandEnd.x), std::min(g_bandStart.y, g_bandEnd.y),
                   std::max(g_bandStart.x, g_bandEnd.x), std::max(g_bandStart.y, g_bandEnd.y) };
    for (int i = 0; i < (int)g_items.size(); i++) {
        auto ir = Inflate(IconDoc(i), 2), lr = LabelDoc(i);
        auto hit = [&](D2D1_RECT_F r) { return r.left < b.right && r.right > b.left && r.top < b.bottom && r.bottom > b.top; };
        bool h = g_view == View::List ? hit(CellDoc(i)) : (hit(ir) || hit(lr));  // lista: a linha inteira conta
        bool base = i < (int)g_bandBase.size() && g_bandBase[i];
        g_items[i].sel = (g_bandMods & MK_CONTROL) ? (base != h) : (g_bandMods & MK_SHIFT) ? (base || h) : h;
    }
    Invalidate();
}

// Rolagem animada (roda com "dentes"): anda a cada quadro desenhado, pelo tempo real, no ritmo da tela.
// Antes um temporizador de 10 ms (na prática ~15,6 ms) a empurrava a 64 quadros/s, aos trancos numa tela de 174 Hz.
static bool g_scrollAnim;
static LARGE_INTEGER g_scrollT, g_scrollT0;
static int g_scrollFrames;
static bool StepScroll() {
    if (!g_scrollAnim) return false;
    LARGE_INTEGER f, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t);
    double dt = std::clamp((double)(t.QuadPart - g_scrollT.QuadPart) / f.QuadPart, 0.0, 0.05);
    g_scrollT = t;
    g_scrollFrames++;
    float d = g_scrollTarget - g_scroll;
    // o mesmo "aproxima 28% por passo de 1/64 s" de antes, só que contínuo
    float k = 1.f - (float)std::pow(1.0 - 0.28, dt * 64.0);
    if (std::fabs(d) < 0.5f) {
        g_scroll = g_scrollTarget; g_scrollAnim = false;
        double ms = (double)(t.QuadPart - g_scrollT0.QuadPart) * 1000.0 / f.QuadPart;
        if (ms > 30) Log(L"rolagem: %d quadros em %.0f ms (%.0f quadros/s)", g_scrollFrames, ms, g_scrollFrames * 1000.0 / ms);
    }
    else g_scroll += d * k;
    g_scrollShownAt = GetTickCount();
    if (g_drag == Drag::Band) { POINT c; GetCursorPos(&c); ScreenToClient(g_hwnd, &c); UpdateBand(ToDip(c.x, c.y)); }
    return g_scrollAnim;
}

static void ScrollBy(float dy, bool animate) {
    g_scrollTarget = std::clamp(g_scrollTarget + dy, 0.f, MaxScroll());
    g_scrollShownAt = GetTickCount();
    if (animate) { if (!g_scrollAnim) { QueryPerformanceCounter(&g_scrollT); g_scrollT0 = g_scrollT; g_scrollFrames = 0; } g_scrollAnim = true; }
    else { g_scroll = g_scrollTarget; g_scrollAnim = false; }
    SetTimer(g_hwnd, T_SCROLLBAR, 50, nullptr);
    Invalidate();
}

static void MoveFocus(UINT vk, bool shift) {
    int n = (int)g_items.size();
    if (!n) return;
    int cols = Columns(), f = g_focus, t = f;
    auto c = ContentRect();
    float rowH = g_view == View::List ? ListRowH() : CellH();
    int page = std::max(1, (int)((c.bottom - c.top) / rowH)) * cols;
    if (f < 0) t = 0;
    else switch (vk) {
        case VK_LEFT: t = std::max(0, f - 1); break;
        case VK_RIGHT: t = std::min(n - 1, f + 1); break;
        case VK_UP: t = f - cols >= 0 ? f - cols : f; break;
        case VK_DOWN: t = f + cols < n ? f + cols : (f / cols < (n - 1) / cols ? n - 1 : f); break;
        case VK_HOME: t = 0; break;
        case VK_END: t = n - 1; break;
        case VK_PRIOR: t = std::max(0, f - page); break;
        case VK_NEXT: t = std::min(n - 1, f + page); break;
    }
    if (shift && g_anchor >= 0) { SelectRange(g_anchor, t); g_focus = t; EnsureVisible(t); }
    else SelectOnly(t);
    Invalidate();
}

static void TypeAhead(wchar_t ch) {
    DWORD now = GetTickCount();
    if (now - g_typedAt > 900) g_typed.clear();
    g_typedAt = now;
    g_typed += (wchar_t)towlower(ch);
    for (int i = 0; i < (int)g_items.size(); i++)
        if (Lower(g_items[i].label).rfind(g_typed, 0) == 0) { SelectOnly(i); Invalidate(); return; }
}

static void FocusSearch(bool on) {
    g_searchFocus = on;
    g_caretOn = true;
    if (on) SetTimer(g_hwnd, T_CARET, 530, nullptr);
    else if (g_renameIdx < 0) KillTimer(g_hwnd, T_CARET);
    Invalidate();
}

static void RunVerb(const char* verb) {
    auto paths = SelectedPaths();
    if (paths.empty()) return;
    std::wstring folder = g_folder.empty() ? Parent(paths[0]) : g_folder;
    if (g_folder.empty()) paths = SameFolderAs(paths);
    HRESULT hr = ctxmenu::InvokeVerb(g_hwnd, folder, paths, verb);
    if (FAILED(hr)) Flash(Tr(L"Não foi possível concluir a ação"));
    else if (!strcmp(verb, "delete")) RecordTrash(paths);
}

static void RemoveFavorite(int fav);
static void NewTab(const std::wstring& folder);
static void AddFavorite(const std::wstring& path, int at);
static void MoveFavorite(int from, int to);

// Liga ou desliga a etiqueta `t` nos itens `paths` (gravando no próprio arquivo).
static void SetTag(const std::vector<std::wstring>& paths, int t, bool on) {
    int fails = 0;
    for (auto& p : paths) {
        unsigned cur = tags::Read(p);
        unsigned m = t < 0 ? 0 : on ? (cur | (1u << t)) : (cur & ~(1u << t));  // t < 0: tira todas
        if (m == cur) continue;
        if (!tags::Write(p, m)) { fails++; continue; }
        for (auto* list : { &g_items, &g_all })
            for (auto& e : *list) if (SamePath(e.path, p)) e.tags = m;
    }
    if (fails) Flash(Tr(L"Não foi possível marcar ") + std::to_wstring(fails) + Tr(L" item(ns): o disco precisa ser NTFS local"));
    if (g_tagView >= 0) RefreshTagView();
    Invalidate();
}

// Menu do botão de etiqueta: cores com ✓ nas que todos os selecionados já têm.
static void ShowTagMenu(POINT pt) {
    auto paths = SelectedPaths();
    if (paths.empty()) return;
    std::vector<ctxmenu::Item> items;
    for (int t = 0; t < tags::COUNT; t++) {
        ctxmenu::Item it; it.label = Tr(tags::names[t]); it.id = 1 + t; it.checked = true;
        for (auto& e : g_items) if (e.sel && !(e.tags & (1u << t))) it.checked = false;
        items.push_back(it);
    }
    ctxmenu::Item sep; sep.separator = true;
    ctxmenu::Item none; none.label = Tr(L"Remover Todas as Etiquetas"); none.id = 20;
    items.push_back(sep);
    items.push_back(none);
    int id = ctxmenu::ShowItems(g_hwnd, pt, items);
    if (id == 20) SetTag(paths, -1, false);
    else if (id >= 1 && id <= tags::COUNT) SetTag(paths, id - 1, !items[id - 1].checked);
}

static void OpenNewWindow(const std::wstring& folder) {
    if (g_finderNativo) { NewTab(folder); return; }
    wchar_t exe[MAX_PATH]; ExePath(exe, MAX_PATH);
    ShellExecuteW(nullptr, nullptr, exe, (L"\"" + folder + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}

// Clique direito num item da barra lateral, como no Finder.
static void ShowSidebarMenu(int s, POINT screen) {
    SideEntry e = g_side[s];
    ctxmenu::Item sep; sep.separator = true;
    ctxmenu::Item open; open.label = Tr(L"Abrir"); open.id = 1;
    ctxmenu::Item win; win.label = Tr(L"Abrir em Nova Janela"); win.id = 2;
    ctxmenu::Item tab; tab.label = Tr(L"Abrir em Nova Aba"); tab.id = 6;
    ctxmenu::Item remove; remove.label = Tr(L"Remover da Barra Lateral"); remove.id = 3;
    ctxmenu::Item info; info.label = Tr(L"Obter Informações"); info.id = 4;
    ctxmenu::Item more; more.label = Tr(L"Mais opções do Windows"); more.id = 5;
    std::vector<ctxmenu::Item> items{ open, tab, win, sep };
    if (e.fav >= 0) { items.push_back(remove); items.push_back(sep); }
    items.push_back(info);
    items.push_back(more);
    int id = ctxmenu::ShowItems(g_hwnd, screen, items);
    std::wstring parent = Parent(e.path);
    if (id == 1) Navigate(e.path);
    else if (id == 2) OpenNewWindow(e.path);
    else if (id == 6) NewTab(e.path);
    else if (id == 3) RemoveFavorite(e.fav);
    else if (id == 4) GetInfo({ e.path });
    else if (id == 5) ctxmenu::Show(g_hwnd, screen, parent.empty() ? e.path : parent, { e.path }, false);
    Invalidate();
}

// ---------------------------------------------------------------------------
// Menu de contexto do Finder (itens e fundo da pasta), na ordem e com os nomes do Mac. Os extras do
// Windows (7-Zip, Enviar para, extensões...) ficam em "Ações Rápidas", que abre o menu do Windows.

static std::wstring StemOf(const std::wstring& name, std::wstring* ext = nullptr) {
    size_t dot = name.rfind(L'.');
    if (dot == std::wstring::npos || dot == 0) { if (ext) ext->clear(); return name; }
    if (ext) *ext = name.substr(dot);
    return name.substr(0, dot);
}

static std::wstring FreeName(const std::wstring& folder, const std::wstring& stem, const std::wstring& ext) {
    std::wstring n = stem + ext;
    for (int k = 2; GetFileAttributesW(Join(folder, n).c_str()) != INVALID_FILE_ATTRIBUTES; k++) n = stem + L" " + std::to_wstring(k) + ext;
    return n;
}

static IShellItemArray* ItemArray(const std::vector<std::wstring>& paths) {
    std::vector<PIDLIST_ABSOLUTE> ids;
    for (auto& p : paths) { PIDLIST_ABSOLUTE id = nullptr; if (SUCCEEDED(SHParseDisplayName(p.c_str(), nullptr, &id, 0, nullptr))) ids.push_back(id); }
    IShellItemArray* arr = nullptr;
    if (!ids.empty()) SHCreateShellItemArrayFromIDLists((UINT)ids.size(), (PCIDLIST_ABSOLUTE*)ids.data(), &arr);
    for (auto id : ids) CoTaskMemFree(id);
    return arr;
}

// Comprimir, como no Finder: um item vira "nome completo.zip" ao lado ("foto.jpg" -> "foto.jpg.zip", pasta
// "Projeto" -> "Projeto.zip"); vários itens, "Arquivo.zip"; nome ocupado, "… 2.zip".
// Antes era a pasta compactada do Windows (zip vazio + IFileOperation): com um .zip entre os itens ela abria o
// .zip como pasta ("…zip\ is not accessible. The directory name is invalid", 12/11/2026), e travava o Finder
// durante a cópia. Depois foi um PowerShell escondido com -EncodedCommand, que antivírus leem como "dropper"
// (Kaspersky Trojan-Dropper.Scrop, 19/11/2026). Agora o zip é montado pelo tar.exe do próprio Windows
// (System32, libarchive): nomes com acento em UTF-8, pastas inteiras, numa thread; no fim o Finder seleciona o .zip.
static const UINT WM_APP_ZIPDONE = WM_APP + 30;  // lParam: std::wstring* (o .zip), wParam: 1 = deu certo
static void CompressItems(const std::vector<std::wstring>& paths) {
    if (paths.empty()) return;
    std::wstring folder = Parent(paths[0]);
    std::wstring base = paths.size() == 1 ? paths[0].substr(paths[0].find_last_of(L'\\') + 1) : Tr(L"Arquivo");
    std::wstring zip = Join(folder, FreeName(folder, base, L".zip"));
    wchar_t sys[MAX_PATH]{};
    GetSystemDirectoryW(sys, MAX_PATH);
    // "C:\" entre aspas viraria \" (aspas escapadas): a pasta-mãe sempre sem a barra no fim ("C:\." na raiz)
    auto dirArg = [](std::wstring d) { if (!d.empty() && d.back() == L'\\') d += L'.'; return L"\"" + d + L"\""; };
    std::wstring cmd = L"\"" + std::wstring(sys) + L"\\tar.exe\" -a -c --options hdrcharset=UTF-8 -f \"" + zip + L"\"";
    for (auto& p : paths) {  // cada item entra com o nome a partir da pasta-mãe dele (a pasta entra inteira)
        size_t s = p.find_last_of(L'\\');
        if (s == std::wstring::npos) continue;
        cmd += L" -C " + dirArg(p.substr(0, s + 1)) + L" \"" + p.substr(s + 1) + L"\"";
    }
    Log(L"comprimir: %zu item(ns) -> %ls", paths.size(), zip.c_str());
    Flash(Tr(L"Comprimindo…"));
    HWND wnd = g_hwnd;
    std::thread([cmd, zip, wnd]() mutable {
        STARTUPINFOW si{ sizeof(si) }; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi{};
        DWORD code = 1;
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, INFINITE);
            GetExitCodeProcess(pi.hProcess, &code);
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        }
        bool ok = code == 0 && GetFileAttributesW(zip.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (!ok) DeleteFileW(zip.c_str());  // pela metade: não deixa um zip quebrado
        auto* z = new std::wstring(zip);
        if (!PostMessageW(wnd, WM_APP_ZIPDONE, ok, (LPARAM)z)) delete z;  // a janela já fechou
    }).detach();
}

// Duplicar: "nome cópia.ext" na mesma pasta, como no Finder
static void DuplicateItems(const std::vector<std::wstring>& paths) {
    IFileOperation* op = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op)))) return;
    op->SetOwnerWindow(g_hwnd);
    std::wstring last;
    for (auto& p : paths) {
        IShellItem *it = nullptr, *dir = nullptr;
        std::wstring folder = Parent(p), ext, stem = StemOf(p.substr(p.find_last_of(L'\\') + 1), &ext);
        if (GetFileAttributesW(p.c_str()) & FILE_ATTRIBUTE_DIRECTORY) { stem += ext; ext.clear(); }
        last = FreeName(folder, stem + Tr(L" cópia"), ext);
        if (SUCCEEDED(SHCreateItemFromParsingName(p.c_str(), nullptr, IID_PPV_ARGS(&it))) && SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&dir))))
            op->CopyItem(it, dir, last.c_str(), nullptr);
        if (it) it->Release(); if (dir) dir->Release();
    }
    op->PerformOperations();
    op->Release();
    if (paths.size() == 1) g_selectAfterReload = last;
    Reload();
}

// Criar Atalho: "nome atalho" ao lado (o alias do Mac)
static void MakeAlias(const std::vector<std::wstring>& paths) {
    std::wstring last;
    for (auto& p : paths) {
        IShellLinkW* sl = nullptr;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) continue;
        sl->SetPath(p.c_str());
        sl->SetWorkingDirectory(Parent(p).c_str());
        std::wstring folder = Parent(p), stem = StemOf(p.substr(p.find_last_of(L'\\') + 1));
        last = FreeName(folder, stem + Tr(L" atalho"), L".lnk");
        IPersistFile* pf = nullptr;
        if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) { pf->Save(Join(folder, last).c_str(), TRUE); pf->Release(); }
        sl->Release();
    }
    if (paths.size() == 1) g_selectAfterReload = last;
    Reload();
}

static void Resort();

static void ShowMacMenu(POINT screen, const std::wstring& folder, const std::vector<std::wstring>& paths) {
    using Item = ctxmenu::Item;
    // Como o menu de contexto do Tahoe: ícone em cada item e sem atalhos à direita
    auto I = [](const std::wstring& l, int id, const std::wstring& = L"", wchar_t icon = 0) { Item i; i.label = l; i.id = id; i.icon = icon; return i; };
    Item sep; sep.separator = true;
    std::vector<Item> items;
    std::vector<IAssocHandler*> handlers;
    enum { M_OPEN = 1, M_OPENTAB, M_TRASH, M_INFO, M_RENAME, M_ZIP, M_DUP, M_ALIAS, M_QL, M_COPY, M_SHARE, M_MORE, M_NEWFOLDER, M_PASTE,
           M_VIEWOPTS, M_TERMINAL, M_TAGNONE, M_RUNPS, M_RUNADMIN, M_TAG0 = 40, M_SORT0 = 60, M_OTHER = 99, M_WITH0 = 100,
           M_PCOLOR0 = 200, M_PEMOJI0 = 220, M_PRESET = 260 };
    if (!paths.empty()) {
        bool allDirs = true;
        for (auto& p : paths) if (!(GetFileAttributesW(p.c_str()) & FILE_ATTRIBUTE_DIRECTORY)) allDirs = false;
        std::wstring one = paths.size() == 1 ? paths[0].substr(paths[0].find_last_of(L'\\') + 1) : L"";
        items.push_back(I(Tr(L"Abrir"), M_OPEN, L"", 0xE8E5));
        // scripts e programas: as opções do Windows que fazem falta (instaladores pedem administrador)
        auto extOf = [](const std::wstring& p) { size_t d = p.find_last_of(L'.'), s = p.find_last_of(L'\\'); std::wstring e = d != std::wstring::npos && (s == std::wstring::npos || d > s) ? p.substr(d) : L""; for (auto& c : e) c = towlower(c); return e; };
        bool allPs = true, allRunnable = true;
        for (auto& p : paths) {
            std::wstring e = extOf(p);
            if (e != L".ps1") allPs = false;
            if (e != L".ps1" && e != L".exe" && e != L".bat" && e != L".cmd" && e != L".msi") allRunnable = false;
        }
        if (!allDirs && allPs) items.push_back(I(Tr(L"Executar no Terminal"), M_RUNPS, L"", 0xE756));
        if (!allDirs && allRunnable) items.push_back(I(Tr(L"Executar como Administrador"), M_RUNADMIN, L"", 0xEA18));
        if (allDirs) {
            items.push_back(I(Tr(L"Abrir em Nova Aba"), M_OPENTAB, L"", 0xE8A7));
            // Personalizar Pasta (Tahoe): fileira de cores e os símbolos
            pastacor::Info cur; bool has = pastacor::Get(paths[0], cur);
            Item pers = I(Tr(L"Personalizar Pasta"), 0, L"", 0xE790);
            Item row; row.label = L" ";
            for (int c = 0; c < pastacor::COUNT; c++) { ctxmenu::Dot d; d.id = M_PCOLOR0 + c; d.rgb = pastacor::colors[c]; d.on = has && cur.color == c; row.dots.push_back(d); }
            pers.sub.push_back(row);
            pers.sub.push_back(sep);
            const auto& em = pastacor::Emojis();
            for (int k = 0; k < (int)em.size(); k++) { Item it = I(em[k], M_PEMOJI0 + k); it.checked = has && cur.emoji == em[k]; pers.sub.push_back(it); }
            pers.sub.push_back(sep);
            Item reset = I(Tr(L"Voltar ao Padrão"), M_PRESET); reset.disabled = !has;
            pers.sub.push_back(reset);
            items.push_back(pers);
        }
        else {  // Abrir Com ›: os apps recomendados para o tipo do primeiro arquivo
            Item with = I(Tr(L"Abrir Com"), 0, L"", 0xE7AC);
            std::wstring ext; StemOf(one.empty() ? paths[0] : one, &ext);
            IEnumAssocHandlers* en = nullptr;
            if (!ext.empty() && SUCCEEDED(SHAssocEnumHandlers(ext.c_str(), ASSOC_FILTER_RECOMMENDED, &en))) {
                IAssocHandler* h = nullptr; ULONG got = 0;
                while (handlers.size() < 12 && en->Next(1, &h, &got) == S_OK && got) {
                    PWSTR name = nullptr;
                    if (SUCCEEDED(h->GetUIName(&name))) { with.sub.push_back(I(name, M_WITH0 + (int)handlers.size())); handlers.push_back(h); CoTaskMemFree(name); }
                    else h->Release();
                }
                en->Release();
            }
            if (!with.sub.empty()) with.sub.push_back(sep);
            with.sub.push_back(I(Tr(L"Outro…"), M_OTHER));
            items.push_back(with);
        }
        items.push_back(sep);
        items.push_back(I(Tr(L"Mover para o Lixo"), M_TRASH, L"", 0xE74D));
        items.push_back(sep);
        items.push_back(I(Tr(L"Obter Informações"), M_INFO, L"", 0xE946));
        if (paths.size() == 1) items.push_back(I(Tr(L"Renomear"), M_RENAME, L"", 0xE8AC));
        items.push_back(I(paths.size() == 1 ? Tr(L"Comprimir “") + one + L"”" : Tr(L"Comprimir ") + std::to_wstring(paths.size()) + Tr(L" Itens"), M_ZIP, L"", 0xF012));
        items.push_back(I(Tr(L"Duplicar"), M_DUP, L"", 0xF413));
        items.push_back(I(Tr(L"Criar Atalho"), M_ALIAS, L"", 0xE71B));
        if (paths.size() == 1) items.push_back(I(Tr(L"Visualização Rápida de “") + one + L"”", M_QL, L"", 0xE890));
        items.push_back(sep);
        items.push_back(I(Tr(L"Copiar"), M_COPY, L"", 0xE8C8));
        items.push_back(I(Tr(L"Compartilhar…"), M_SHARE, L"", 0xE72D));
        items.push_back(sep);
        {   // fileira de bolinhas das etiquetas, como no Tahoe; ✓ nas que todos os selecionados já têm
            Item dots; dots.label = L" ";
            for (int t = 0; t < tags::COUNT; t++) {
                ctxmenu::Dot d; d.id = M_TAG0 + t; d.rgb = tags::colors[t]; d.on = true;
                for (auto& e : g_items) if (e.sel && !(e.tags & (1u << t))) d.on = false;
                dots.dots.push_back(d);
            }
            items.push_back(dots);
        }
        Item tagsItem = I(Tr(L"Etiquetas"), 0, L"", 0xE8EC);
        for (int t = 0; t < tags::COUNT; t++) {
            Item it = I(Tr(tags::names[t]), M_TAG0 + t);
            it.checked = true;
            for (auto& e : g_items) if (e.sel && !(e.tags & (1u << t))) it.checked = false;
            tagsItem.sub.push_back(it);
        }
        tagsItem.sub.push_back(sep);
        tagsItem.sub.push_back(I(Tr(L"Remover Todas as Etiquetas"), M_TAGNONE));
        items.push_back(tagsItem);
        items.push_back(sep);
        items.push_back(I(Tr(L"Ações Rápidas"), M_MORE, L"", 0xE945));
    } else {  // fundo da pasta
        items.push_back(I(Tr(L"Nova Pasta"), M_NEWFOLDER, L"", 0xE8F4));
        items.push_back(sep);
        items.push_back(I(Tr(L"Obter Informações"), M_INFO, L"", 0xE946));
        items.push_back(sep);
        Item paste = I(Tr(L"Colar Item"), M_PASTE, L"", 0xE77F);
        paste.disabled = !IsClipboardFormatAvailable(CF_HDROP);
        items.push_back(paste);
        items.push_back(sep);
        Item sort = I(Tr(L"Ordenar Por"), 0, L"", 0xE8CB);
        const wchar_t* names[] = { Tr(L"Nome"), Tr(L"Tamanho"), Tr(L"Tipo"), Tr(L"Data de Modificação"), Tr(L"Data de Criação") };
        for (int k = 0; k <= (int)SORT_CREATED; k++) { Item it = I(names[k], M_SORT0 + k); it.checked = g_sortCol == k; sort.sub.push_back(it); }
        items.push_back(sort);
        items.push_back(I(Tr(L"Mostrar Opções de Visualização"), M_VIEWOPTS, L"", 0xE713));
        items.push_back(I(Tr(L"Novo Terminal na Pasta"), M_TERMINAL, L"", 0xE756));
        items.push_back(sep);
        items.push_back(I(Tr(L"Ações Rápidas"), M_MORE, L"", 0xE945));
    }
    int id = ctxmenu::ShowItems(g_hwnd, screen, items);
    std::wstring base = folder.empty() && !paths.empty() ? Parent(paths[0]) : folder;
    if (id >= M_WITH0 && id < M_WITH0 + (int)handlers.size()) {
        IDataObject* d = nullptr;
        if (SUCCEEDED(ctxmenu::GetUIObject(g_hwnd, paths, IID_PPV_ARGS(&d)))) { handlers[id - M_WITH0]->Invoke(d); d->Release(); }
    }
    for (auto h : handlers) h->Release();
    if ((id >= M_PCOLOR0 && id < M_PCOLOR0 + pastacor::COUNT) || (id >= M_PEMOJI0 && id < M_PEMOJI0 + (int)pastacor::Emojis().size()) || id == M_PRESET) {
        for (auto& p : paths) {
            pastacor::Info cur; pastacor::Get(p, cur);
            if (id == M_PRESET) { cur.color = -1; cur.emoji.clear(); }
            else if (id < M_PEMOJI0) { int c = id - M_PCOLOR0; cur.color = cur.color == c ? -1 : c; }
            else { const auto& e = pastacor::Emojis()[id - M_PEMOJI0]; cur.emoji = cur.emoji == e ? L"" : e; if (cur.color < 0) cur.color = 5; }  // símbolo sem cor: azul
            pastacor::Set(p, cur.color, cur.emoji, g_tahoe);
        }
        Invalidate();
    }
    if (id >= M_TAG0 && id < M_TAG0 + tags::COUNT) {
        bool all = true;
        for (auto& e : g_items) if (e.sel && !(e.tags & (1u << (id - M_TAG0)))) all = false;
        SetTag(paths, id - M_TAG0, !all);
    }
    if (id >= M_SORT0 && id <= M_SORT0 + (int)SORT_CREATED) {
        int k = id - M_SORT0;
        if (k == g_sortCol) g_sortAsc = !g_sortAsc; else { g_sortCol = (SortCol)k; g_sortAsc = k == SORT_NAME || k == SORT_KIND; }
        Resort();
    }
    switch (id) {
    case M_OPEN: OpenSelection(); break;
    case M_OPENTAB: for (auto& p : paths) NewTab(p); break;
    case M_RUNPS: case M_RUNADMIN:
        for (auto& p : paths) {
            std::wstring dir = Parent(p), low = p;
            for (auto& c : low) c = towlower(c);
            bool ps = low.size() > 4 && low.compare(low.size() - 4, 4, L".ps1") == 0;
            const wchar_t* verb = id == M_RUNADMIN ? L"runas" : L"open";
            if (ps) {  // no nosso Terminal (como administrador, o Terminal inteiro sobe elevado); sem ele, o PowerShell do Windows
                wchar_t exe[MAX_PATH]; ExePath(exe, MAX_PATH); PathRemoveFileSpecW(exe);
                std::wstring term = std::wstring(exe) + L"\\WacOSTerminal.exe";
                if (GetFileAttributesW(term.c_str()) != INVALID_FILE_ATTRIBUTES) {
                    std::wstring args = L"--pasta \"" + dir + L"\" --executar \"" + p + L"\"";
                    ShellExecuteW(g_hwnd, verb, term.c_str(), args.c_str(), dir.c_str(), SW_SHOWNORMAL);
                } else {
                    std::wstring args = L"-NoProfile -ExecutionPolicy Bypass -File \"" + p + L"\"";
                    ShellExecuteW(g_hwnd, verb, L"powershell.exe", args.c_str(), dir.c_str(), SW_SHOWNORMAL);
                }
            } else ShellExecuteW(g_hwnd, verb, p.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL);
        }
        break;
    case M_OTHER: ctxmenu::InvokeVerb(g_hwnd, base, paths, "openas"); break;
    case M_TRASH: RunVerb("delete"); break;
    case M_INFO: GetInfo(paths.empty() ? std::vector<std::wstring>{ folder } : paths); break;
    case M_RENAME: for (int i = 0; i < (int)g_items.size(); i++) if (SamePath(g_items[i].path, paths[0])) { StartRename(i); break; } break;
    case M_ZIP: CompressItems(paths); break;
    case M_DUP: DuplicateItems(paths); break;
    case M_ALIAS: MakeAlias(paths); break;
    case M_QL: QlToggle(); break;
    case M_COPY: RunVerb("copy"); break;
    case M_SHARE: RunVerb("windows.modernshare"); break;
    case M_TAGNONE: SetTag(paths, -1, false); break;
    case M_NEWFOLDER: NewFolder(); break;
    case M_PASTE: if (FAILED(PasteInto(g_hwnd, folder))) Flash(Tr(L"Nada para colar")); break;
    case M_VIEWOPTS: g_viewOpts = true; break;
    case M_TERMINAL: {  // o nosso Terminal (PowerShell) nesta pasta; sem ele, o PowerShell do Windows
        wchar_t exe[MAX_PATH]; ExePath(exe, MAX_PATH); PathRemoveFileSpecW(exe);
        std::wstring term = std::wstring(exe) + L"\\WacOSTerminal.exe";
        if (GetFileAttributesW(term.c_str()) != INVALID_FILE_ATTRIBUTES) ShellExecuteW(nullptr, L"open", term.c_str(), (L"--pasta \"" + folder + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        else ShellExecuteW(nullptr, L"open", L"powershell.exe", L"-NoExit", folder.c_str(), SW_SHOWNORMAL);
        break;
    }
    case M_MORE: {  // o menu do Windows, no mesmo lugar
        auto r = ctxmenu::Show(g_hwnd, screen, base, paths, false);
        if (r.rename && paths.size() == 1) { for (int i = 0; i < (int)g_items.size(); i++) if (SamePath(g_items[i].path, paths[0])) { StartRename(i); break; } }
        else if (r.chosen && r.verb == L"delete") RecordTrash(paths);
        break;
    }
    }
    Invalidate();
}

// keyboard: menu da seleção atual; keepPoint: usa `screen` como está (botão de ações).
static void ShowContextMenu(POINT screen, bool keyboard, bool keepPoint = false) {
    std::vector<std::wstring> paths;
    std::wstring folder = g_folder;
    if (keyboard && keepPoint) {
        paths = SelectedPaths();
    } else if (keyboard) {
        paths = SelectedPaths();
        POINT p;
        if (g_focus >= 0) { auto r = ToView(IconDoc(g_focus)); p = { (LONG)(r.right * g_scale), (LONG)(r.top * g_scale + 20 * g_scale) }; }
        else { auto c = ContentRect(); p = { (LONG)((c.left + 40) * g_scale), (LONG)((c.top + 20) * g_scale) }; }
        ClientToScreen(g_hwnd, &p);
        screen = p;
    } else {
        POINT cp = screen; ScreenToClient(g_hwnd, &cp);
        D2D1_POINT_2F p = ToDip(cp.x, cp.y);
        int s = HitSide(p);
        if (s >= 0 && g_side[s].path.empty()) return;  // etiqueta: sem menu por enquanto
        if (s >= 0) { ShowSidebarMenu(s, screen); return; }
        if (In(ContentRect(), p)) {
            int i = HitItem(p);
            if (i >= 0) { if (!g_items[i].sel) SelectOnly(i); g_focus = i; paths = SelectedPaths(); }
            else ClearSel();
        } else return;
    }
    if (g_tagView == VIEW_TRASH) {  // Lixeira: Devolver / Apagar Imediatamente; no fundo, Esvaziar
        Render();
        using ctxmenu::Item;
        auto I = [](const wchar_t* t, int id, bool dis = false) { Item it; it.label = t; it.id = id; it.disabled = dis; return it; };
        Item sep; sep.separator = true;
        std::vector<Item> items;
        if (paths.empty()) items = { I(Tr(L"Esvaziar Lixeira…"), 10, g_all.empty()), sep, I(Tr(L"Ordenar Por Data de Remoção"), 11) };
        else items = { I(Tr(L"Devolver"), 1), I(Tr(L"Apagar Imediatamente…"), 2), sep, I(Tr(L"Obter Informações"), 3), I(Tr(L"Visualização Rápida"), 4), sep, I(Tr(L"Copiar"), 5) };
        int id = ctxmenu::ShowItems(g_hwnd, screen, items);
        if (id == 1) TrashVerb(paths, L"undelete");
        else if (id == 2) TrashVerb(paths, L"delete");
        else if (id == 3) GetInfo(paths);
        else if (id == 4) QlToggle();
        else if (id == 5) RunVerb("copy");
        else if (id == 10) EmptyTrash();
        else if (id == 11) { g_sortCol = SORT_DATE; g_sortAsc = false; Resort(); }
        RefreshTagView();
        return;
    }
    if (folder.empty()) {  // visão de etiqueta: sem menu de fundo; itens de uma pasta só
        if (paths.empty()) return;
        folder = Parent(paths[0]);
        paths = SameFolderAs(paths);
    }
    Render();
    if (GetKeyState(VK_SHIFT) >= 0 && g_tagView < 0) { ShowMacMenu(screen, folder, paths); return; }  // Shift: o menu completo do Windows
    auto r = ctxmenu::Show(g_hwnd, screen, folder, paths, GetKeyState(VK_SHIFT) < 0);
    if (r.rename && paths.size() == 1) {
        for (int i = 0; i < (int)g_items.size(); i++) if (SamePath(g_items[i].path, paths[0])) { StartRename(i); break; }
    } else if (r.chosen && FAILED(r.hr)) {
        Flash(Tr(L"A ação falhou (0x") + std::to_wstring((unsigned)r.hr) + L")");
    } else if (r.chosen && r.verb == L"delete") RecordTrash(paths);
    Invalidate();
}

static void SetIconSize(float size) {
    int oldBucket = ThumbBucket();
    int keep = g_focus;
    g_icon = std::round(std::clamp(size, Lay::ICON_MIN, Lay::ICON_MAX) / 4) * 4;
    if (ThumbBucket() != oldBucket) {  // miniaturas maiores para ícones maiores
        g_gen++;
        thumbs::SetGeneration(g_gen);
        ReleaseImages();
    }
    g_scroll = g_scrollTarget = std::min(g_scroll, MaxScroll());
    if (keep >= 0) EnsureVisible(keep);
    Invalidate();
}

static void SetIconSizeFromSlider(float x) {
    auto r = SliderRect();
    float t = std::clamp((x - r.left) / (r.right - r.left), 0.f, 1.f);
    SetIconSize(Lay::ICON_MIN + t * (Lay::ICON_MAX - Lay::ICON_MIN));
}

// Reordena depois de trocar coluna, sentido ou grupo, mantendo a seleção.
static void ResortImpl();
static void Resort() { ResortImpl(); if (!g_reserva) SaveOptions(); }
static void ResortImpl() {
    SortItems();
    g_rowsDirty = true;
    g_focus = g_anchor = -1;
    for (int i = 0; i < (int)g_items.size(); i++) if (g_items[i].sel) { g_focus = g_anchor = i; break; }
    g_scroll = g_scrollTarget = std::min(g_scroll, MaxScroll());
    Invalidate();
}

static View g_defaultView = View::Icons;  // visão de abertura (a última usada)

static void SetView(View v) {
    if (!g_tahoe && tahoe::Leopard()) {  // Leopard: a 4ª visão é o Cover Flow (lista + capas em cima)
        bool cf = v == View::Gallery;
        if (cf) v = View::List;
        if (cf != g_coverFlow) { g_coverFlow = cf; g_rowsDirty = true; Invalidate(); }
    } else g_coverFlow = false;
    if (g_view == v) return;
    if (g_renameIdx >= 0) EndRename(true);
    g_view = v;
    if (g_pick.on) g_pickView = (int)v;  // Abrir/Salvar lembra a visão dele
    else g_defaultView = v;  // a próxima janela abre nesta visão (antes voltava sempre para a padrão)
    SaveOptions();
    g_rowsDirty = true;
    g_colTrail.clear();  // as colunas começam na pasta aberta
    g_childPath.clear();
    if (v == View::Gallery && g_focus < 0 && !g_items.empty()) SelectOnly(0);  // a galeria sempre mostra um item
    g_scroll = g_scrollTarget = std::min(g_scroll, MaxScroll());
    if (g_focus >= 0) EnsureVisible(g_focus);
    Invalidate();
}

static POINT BelowTool(int k) {  // canto de baixo à esquerda do botão, em pixels de tela
    auto r = ToolRect(k);
    POINT p{ (LONG)(r.left * g_scale), (LONG)((r.bottom + 4) * g_scale) };
    ClientToScreen(g_hwnd, &p);
    return p;
}

static std::vector<ctxmenu::Item> GroupItems(int base) {
    auto it = [&](const wchar_t* t, GroupBy g) { ctxmenu::Item m; m.label = t; m.id = base + g; m.checked = g_groupBy == g; return m; };
    ctxmenu::Item sep; sep.separator = true;
    return { it(Tr(L"Nenhum"), GROUP_NONE), sep, it(Tr(L"Tipo"), GROUP_KIND), it(Tr(L"Data de Modificação"), GROUP_DATE), it(Tr(L"Tamanho"), GROUP_SIZE) };
}

// Menu ⋯ sem seleção: o do próprio Finder (com seleção, continua o menu dos itens).
static void ShowFinderActionMenu() {
    ctxmenu::Item sep; sep.separator = true;
    ctxmenu::Item newFolder; newFolder.label = Tr(L"Nova Pasta"); newFolder.id = 1;
    ctxmenu::Item info; info.label = Tr(L"Obter Informações"); info.id = 2;
    ctxmenu::Item groups; groups.label = Tr(L"Usar Grupos"); groups.id = 3; groups.checked = g_groupBy != GROUP_NONE;
    ctxmenu::Item groupBy; groupBy.label = Tr(L"Agrupar por"); groupBy.sub = GroupItems(100);
    ctxmenu::Item viewOpts; viewOpts.label = Tr(L"Mostrar Opções de Visualização"); viewOpts.id = 4;
    int id = ctxmenu::ShowItems(g_hwnd, BelowTool(TB_ACTION), { newFolder, sep, info, sep, groups, groupBy, viewOpts });
    if (id == 1) NewFolder();
    else if (id == 2) GetInfo({ g_folder });
    else if (id == 3) { g_groupBy = g_groupBy == GROUP_NONE ? GROUP_KIND : GROUP_NONE; Resort(); }
    else if (id == 4) g_viewOpts = true;
    else if (id >= 100) { g_groupBy = (GroupBy)(id - 100); Resort(); }
    Invalidate();
}

static void ToolClick(int k) {
    switch (k) {
    case TB_BACK: GoBack(); break;
    case TB_FWD: GoForward(); break;
    case TB_V_ICONS:
        if (WideToolbar()) { SetView(View::Icons); break; }
        {  // seletor recolhido: menu com as visões
            ctxmenu::Item a; a.label = Tr(L"Ícones"); a.id = 1; a.checked = g_view == View::Icons;
            ctxmenu::Item b; b.label = Tr(L"Lista"); b.id = 2; b.checked = g_view == View::List;
            ctxmenu::Item c; c.label = Tr(L"Colunas"); c.id = 3; c.checked = g_view == View::Columns;
            ctxmenu::Item d; d.label = Tr(L"Galeria"); d.id = 4; d.checked = g_view == View::Gallery;
            int id = ctxmenu::ShowItems(g_hwnd, BelowTool(TB_V_ICONS), { a, b, c, d });
            if (id == 1) SetView(View::Icons);
            if (id == 2) SetView(View::List);
            if (id == 3) SetView(View::Columns);
            if (id == 4) SetView(View::Gallery);
        }
        break;
    case TB_V_LIST: SetView(View::List); break;
    case TB_V_COLS: SetView(View::Columns); break;
    case TB_V_GALLERY: SetView(View::Gallery); break;
    case TB_GROUP: {
        if (!g_tahoe && tahoe::Leopard()) { QlToggle(); break; }  // Leopard: o botão é o olho do Quick Look
        int id = ctxmenu::ShowItems(g_hwnd, BelowTool(TB_GROUP), GroupItems(100));
        if (id >= 100) { g_groupBy = (GroupBy)(id - 100); Resort(); }
        break;
    }
    case TB_TAG: if (SelCount() > 0) ShowTagMenu(BelowTool(TB_TAG)); break;
    case TB_SHARE:
        if (SelCount() > 0) RunVerb("windows.modernshare");
        break;
    case TB_TRASH:
        if (SelCount() > 0) RunVerb("delete");
        break;
    case TB_ACTION:
        if (SelCount() == 0) ShowFinderActionMenu();
        else ShowContextMenu(BelowTool(TB_ACTION), true, true);
        break;
    }
}

// ---------------------------------------------------------------------------
// Opções de Visualização (Ctrl+J): painel com as opções da visão atual, salvas em
// %APPDATA%\Arquivos\opcoes.txt ("chave=valor").

static std::wstring KnownFolder(REFKNOWNFOLDERID id);
struct OptCtl { D2D1_RECT_F r; int id; };
static std::vector<OptCtl> g_optCtls;  // controles do painel (recalculados a cada desenho)
static bool g_optSliderDrag;

enum OptId {
    OPT_CLOSE = 1, OPT_GROUP, OPT_SORT, OPT_ICON_SLIDER, OPT_TEXT_SIZE, OPT_THUMBS, OPT_LIST_SMALL, OPT_LIST_LARGE,
    OPT_COL_SIZE, OPT_COL_KIND, OPT_COL_DATE, OPT_COL_CREATED, OPT_REL_DATES, OPT_COLS_ICONS, OPT_COLS_PREVIEW, OPT_DEFAULT_VIEW
};

static std::wstring OptionsFile() {
    std::wstring dir = KnownFolder(FOLDERID_RoamingAppData) + L"\\Arquivos";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\opcoes.txt";
}

static void SaveOptions() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, OptionsFile().c_str(), L"w, ccs=UTF-8") || !f) return;
    fwprintf(f, L"visao=%d\nordem=%d\nascendente=%d\ngrupo=%d\nicone=%g\ntextoIcones=%g\ntextoLista=%g\nprevia=%d\n"
                L"listaGrande=%d\ncolTamanho=%d\ncolTipo=%d\ncolData=%d\ncolCriacao=%d\ndatasRelativas=%d\ncolunasIcones=%d\ncolunasPrevia=%d\nbarraCaminho=%d\n",
             (int)g_defaultView, (int)g_sortCol, g_sortAsc, (int)g_groupBy, g_icon, g_labelSize, g_rowTextSize, g_showThumbs,
             g_listLarge, g_colSize, g_colKind, g_colDate, g_colCreated, g_relDates, g_colIcons, g_colPreview, g_pick.on ? g_pathBarUser : g_pathBar);
    if (g_sideWidthSet) fwprintf(f, L"larguraLateral=%g\n", Lay::SIDEBAR_W);
    if (g_hwnd && !g_reserva && !g_pick.on && IsWindowVisible(g_hwnd)) {
        // retângulo de tela de verdade (o da colocação é relativo à área de trabalho: vinha 24 px acima)
        if (!IsZoomed(g_hwnd) && !IsIconic(g_hwnd)) GetWindowRect(g_hwnd, &g_savedWin);
    }
    if (g_hwnd && g_pick.on && IsWindowVisible(g_hwnd) && !IsZoomed(g_hwnd) && !IsIconic(g_hwnd)) GetWindowRect(g_hwnd, &g_pickWin);
    if (g_pickWin.right > g_pickWin.left) fwprintf(f, L"janelaEscolher=%ld %ld %ld %ld\n", g_pickWin.left, g_pickWin.top, g_pickWin.right, g_pickWin.bottom);
    if (g_pickView >= 0) fwprintf(f, L"visaoEscolher=%d\n", g_pickView);
    if (g_savedWin.right > g_savedWin.left) fwprintf(f, L"janela=%ld %ld %ld %ld\n", g_savedWin.left, g_savedWin.top, g_savedWin.right, g_savedWin.bottom);
    fclose(f);
}

static void LoadOptions() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, OptionsFile().c_str(), L"r, ccs=UTF-8") || !f) return;
    wchar_t line[256];
    while (fgetws(line, 256, f)) {
        wchar_t* eq = wcschr(line, L'=');
        if (!eq) continue;
        *eq = 0;
        std::wstring k = line;
        double v = _wtof(eq + 1);
        if (k == L"janela") { RECT r{}; if (swscanf_s(eq + 1, L"%ld %ld %ld %ld", &r.left, &r.top, &r.right, &r.bottom) == 4) g_savedWin = r; continue; }
        if (k == L"janelaEscolher") { RECT r{}; if (swscanf_s(eq + 1, L"%ld %ld %ld %ld", &r.left, &r.top, &r.right, &r.bottom) == 4) g_pickWin = r; continue; }
        if (k == L"visaoEscolher") { g_pickView = std::clamp((int)v, 0, 3); continue; }
        if (k == L"visao") g_view = g_defaultView = (View)std::clamp((int)v, 0, 3);
        else if (k == L"ordem") g_sortCol = (SortCol)std::clamp((int)v, 0, (int)SORT_CREATED);
        else if (k == L"ascendente") g_sortAsc = v != 0;
        else if (k == L"grupo") g_groupBy = (GroupBy)std::clamp((int)v, 0, (int)GROUP_SIZE);
        else if (k == L"icone") g_icon = std::clamp((float)v, Lay::ICON_MIN, Lay::ICON_MAX);
        else if (k == L"textoIcones") g_labelSize = std::clamp((float)v, 10.f, 16.f);
        else if (k == L"textoLista") g_rowTextSize = std::clamp((float)v, 10.f, 16.f);
        else if (k == L"previa") g_showThumbs = v != 0;
        else if (k == L"barraCaminho") { g_pathBar = v != 0; Lay::FOOTER_H = Lay::STATUS_H + (g_pathBar ? Lay::PATH_H : 0); }
        else if (k == L"larguraLateral") { Lay::SIDEBAR_W = std::clamp((float)v, Lay::SIDEBAR_MIN, Lay::SIDEBAR_MAX); g_sideWidthSet = true; }
        else if (k == L"listaGrande") g_listLarge = v != 0;
        else if (k == L"colTamanho") g_colSize = v != 0;
        else if (k == L"colTipo") g_colKind = v != 0;
        else if (k == L"colData") g_colDate = v != 0;
        else if (k == L"colCriacao") g_colCreated = v != 0;
        else if (k == L"datasRelativas") g_relDates = v != 0;
        else if (k == L"colunasIcones") g_colIcons = v != 0;
        else if (k == L"colunasPrevia") g_colPreview = v != 0;
    }
    fclose(f);
}

static const wchar_t* SortName(SortCol c) {
    switch (c) {
    case SORT_NAME: return Tr(L"Nome");
    case SORT_SIZE: return Tr(L"Tamanho");
    case SORT_KIND: return Tr(L"Tipo");
    case SORT_DATE: return Tr(L"Data de Modificação");
    default: return Tr(L"Data de Criação");
    }
}

static const wchar_t* GroupName(GroupBy g) {
    switch (g) {
    case GROUP_KIND: return Tr(L"Tipo");
    case GROUP_DATE: return Tr(L"Data de Modificação");
    case GROUP_SIZE: return Tr(L"Tamanho");
    default: return Tr(L"Nenhum");
    }
}

static D2D1_RECT_F ViewOptsPanel(float height) {
    auto s = ClientDip();
    return { s.width - 296, Lay::TOOLBAR_H + 10, s.width - 16, Lay::TOOLBAR_H + 10 + height };
}

static D2D1_RECT_F OptSliderTrack() {
    for (auto& c : g_optCtls) if (c.id == OPT_ICON_SLIDER) return c.r;
    return {};
}

static void DrawViewOptions(ID2D1DeviceContext* dc, const Palette& pal, ID2D1SolidColorBrush* br) {
    // altura conforme a visão
    int rows = 4;  // título, agrupar, ordenar, texto
    if (g_view == View::Icons) rows += 2;
    else if (g_view == View::List) rows += 7;
    else if (g_view == View::Columns) rows += 2;
    else rows += 1;
    float rowH = 30, h = 14 + rows * rowH + 46;
    auto r = ViewOptsPanel(h);
    g_optCtls.clear();
    for (int k = 1; k <= 8; k++) {  // sombra
        br->SetColor(D2D1::ColorF(0, 0, 0, 0.03f));
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left - k * 2.f, r.top - k * 1.f + 6, r.right + k * 2.f, r.bottom + k * 2.5f + 6), 10 + k * 2.f, 10 + k * 2.f), br);
    }
    br->SetColor(g_dark ? D2D1::ColorF(0.17f, 0.17f, 0.18f, 0.98f) : D2D1::ColorF(0.97f, 0.97f, 0.975f, 0.98f));
    dc->FillRoundedRectangle(D2D1::RoundedRect(r, 10, 10), br);
    br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.12f) : D2D1::ColorF(0, 0, 0, 0.12f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(r, 10, 10), br, 1);

    float x0 = r.left + 16, x1 = r.right - 16, y = r.top + 10;
    auto label = [&](const std::wstring& t, float left = 0) {
        br->SetColor(pal.text);
        dc->DrawTextW(t.c_str(), (UINT32)t.size(), g_tfRow, D2D1::RectF(x0 + left, y, x1, y + rowH), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };
    auto dropdown = [&](int id, const std::wstring& title, const std::wstring& value) {
        label(title);
        D2D1_RECT_F b{ x1 - 130, y + 4, x1, y + rowH - 4 };
        br->SetColor(pal.field);
        dc->FillRoundedRectangle(D2D1::RoundedRect(b, 5, 5), br);
        br->SetColor(pal.text);
        dc->DrawTextW(value.c_str(), (UINT32)value.size(), g_tfRow, D2D1::RectF(b.left + 8, b.top, b.right - 20, b.bottom), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        float cx = b.right - 11, cy = (b.top + b.bottom) / 2;
        br->SetColor(pal.text2);
        Line(dc, br, cx - 3, cy - 1.5f, cx, cy + 1.5f, 1.3f); Line(dc, br, cx, cy + 1.5f, cx + 3, cy - 1.5f, 1.3f);
        g_optCtls.push_back({ b, id });
        y += rowH;
    };
    auto checkbox = [&](int id, const std::wstring& title, bool on, float indent = 0) {
        D2D1_RECT_F box{ x0 + indent, y + 8, x0 + indent + 14, y + 22 };
        br->SetColor(on ? pal.accent : pal.field);
        dc->FillRoundedRectangle(D2D1::RoundedRect(box, 3.5f, 3.5f), br);
        if (!on) { br->SetColor(pal.text2); dc->DrawRoundedRectangle(D2D1::RoundedRect(box, 3.5f, 3.5f), br, 1); }
        else {
            br->SetColor(D2D1::ColorF(1, 1, 1));
            Line(dc, br, box.left + 3.5f, box.top + 7, box.left + 6, box.top + 10, 1.6f);
            Line(dc, br, box.left + 6, box.top + 10, box.left + 10.5f, box.top + 4, 1.6f);
        }
        label(title, indent + 22);
        g_optCtls.push_back({ D2D1::RectF(x0 + indent, y, x1, y + rowH), id });
        y += rowH;
    };
    auto radio = [&](int id, const std::wstring& title, bool on, float left) {
        float cx = x0 + left + 7, cy = y + rowH / 2;
        br->SetColor(on ? pal.accent : pal.field);
        dc->FillEllipse(D2D1::Ellipse({ cx, cy }, 7, 7), br);
        if (on) { br->SetColor(D2D1::ColorF(1, 1, 1)); dc->FillEllipse(D2D1::Ellipse({ cx, cy }, 2.6f, 2.6f), br); }
        else { br->SetColor(pal.text2); dc->DrawEllipse(D2D1::Ellipse({ cx, cy }, 7, 7), br, 1); }
        br->SetColor(pal.text);
        dc->DrawTextW(title.c_str(), (UINT32)title.size(), g_tfRow, D2D1::RectF(cx + 12, y, cx + 120, y + rowH), br);
        g_optCtls.push_back({ D2D1::RectF(cx - 8, y, cx + 100, y + rowH), id });
    };

    // título + fechar
    br->SetColor(pal.text);
    const wchar_t* title = Tr(L"Opções de Visualização");
    dc->DrawTextW(title, (UINT32)wcslen(title), g_tfQlTitle, D2D1::RectF(r.left, y, r.right, y + rowH), br);
    D2D1_RECT_F close{ r.left + 10, y + 6, r.left + 28, y + 24 };
    br->SetColor(pal.text2);
    float ccx = (close.left + close.right) / 2, ccy = (close.top + close.bottom) / 2;
    Line(dc, br, ccx - 3.5f, ccy - 3.5f, ccx + 3.5f, ccy + 3.5f, 1.4f);
    Line(dc, br, ccx - 3.5f, ccy + 3.5f, ccx + 3.5f, ccy - 3.5f, 1.4f);
    g_optCtls.push_back({ Inflate(close, 4), OPT_CLOSE });
    y += rowH;
    std::wstring viewName = g_view == View::Icons ? Tr(L"Ícones") : g_view == View::List ? Tr(L"Lista") : g_view == View::Columns ? Tr(L"Colunas") : Tr(L"Galeria");
    br->SetColor(pal.text2);
    dc->DrawTextW(viewName.c_str(), (UINT32)viewName.size(), g_tfStatus, D2D1::RectF(r.left, y - 8, r.right, y + 8), br);
    y += 6;

    dropdown(OPT_GROUP, Tr(L"Agrupar por:"), GroupName(g_groupBy));
    dropdown(OPT_SORT, Tr(L"Ordenar por:"), SortName(g_sortCol));
    float& textSize = g_view == View::Icons ? g_labelSize : g_rowTextSize;
    dropdown(OPT_TEXT_SIZE, Tr(L"Tamanho do texto:"), std::to_wstring((int)textSize));
    if (g_view == View::Icons) {
        label(Tr(L"Tamanho dos ícones:"));
        D2D1_RECT_F tr{ x1 - 130, y + rowH / 2 - 2, x1, y + rowH / 2 + 2 };
        br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.18f) : D2D1::ColorF(0, 0, 0, 0.14f));
        dc->FillRoundedRectangle(D2D1::RoundedRect(tr, 2, 2), br);
        float t = (g_icon - Lay::ICON_MIN) / (Lay::ICON_MAX - Lay::ICON_MIN), kx = tr.left + t * (tr.right - tr.left);
        br->SetColor(g_dark ? D2D1::ColorF(0.85f, 0.85f, 0.86f) : D2D1::ColorF(1, 1, 1));
        dc->FillEllipse(D2D1::Ellipse({ kx, y + rowH / 2 }, 7, 7), br);
        g_optCtls.push_back({ D2D1::RectF(tr.left, y, tr.right, y + rowH), OPT_ICON_SLIDER });
        y += rowH;
        checkbox(OPT_THUMBS, Tr(L"Mostrar prévia do ícone"), g_showThumbs);
    } else if (g_view == View::List) {
        label(Tr(L"Ícones:"));
        radio(OPT_LIST_SMALL, Tr(L"Pequenos"), !g_listLarge, 70);
        radio(OPT_LIST_LARGE, Tr(L"Grandes"), g_listLarge, 165);
        y += rowH;
        label(Tr(L"Mostrar colunas:"));
        y += rowH;
        checkbox(OPT_COL_SIZE, Tr(L"Tamanho"), g_colSize, 12);
        checkbox(OPT_COL_KIND, Tr(L"Tipo"), g_colKind, 12);
        checkbox(OPT_COL_DATE, Tr(L"Data de Modificação"), g_colDate, 12);
        checkbox(OPT_COL_CREATED, Tr(L"Data de Criação"), g_colCreated, 12);
        checkbox(OPT_REL_DATES, Tr(L"Usar datas relativas"), g_relDates);
    } else if (g_view == View::Columns) {
        checkbox(OPT_COLS_ICONS, Tr(L"Mostrar ícones"), g_colIcons);
        checkbox(OPT_COLS_PREVIEW, Tr(L"Mostrar coluna de prévia"), g_colPreview);
    } else {
        checkbox(OPT_THUMBS, Tr(L"Mostrar prévia do ícone"), g_showThumbs);
    }
    // botão "Usar como Padrão": a visão atual passa a ser a de abertura
    D2D1_RECT_F def{ (r.left + r.right) / 2 - 70, r.bottom - 38, (r.left + r.right) / 2 + 70, r.bottom - 12 };
    br->SetColor(pal.field);
    dc->FillRoundedRectangle(D2D1::RoundedRect(def, 6, 6), br);
    br->SetColor(pal.text);
    const wchar_t* defLabel = Tr(L"Usar como Padrão");
    dc->DrawTextW(defLabel, (UINT32)wcslen(defLabel), g_tfQlButton, def, br);
    g_optCtls.push_back({ def, OPT_DEFAULT_VIEW });
    g_optCtls.push_back({ r, 0 });  // o resto do painel (consome o clique)
}

static void MakeBodyFormats();

static void OptionsChanged() {
    g_rowsDirty = true;
    g_scroll = g_scrollTarget = std::min(g_scroll, MaxScroll());
    SaveOptions();
    Invalidate();
}

static void OptSlider(float x) {
    auto tr = OptSliderTrack();
    float t = std::clamp((x - tr.left) / std::max(1.f, tr.right - tr.left), 0.f, 1.f);
    SetIconSize(Lay::ICON_MIN + t * (Lay::ICON_MAX - Lay::ICON_MIN));
    SaveOptions();
}

// Clique com o painel aberto: true se foi no painel (consumido).
static bool ViewOptsClick(D2D1_POINT_2F p) {
    if (!g_viewOpts) return false;
    int id = -1;
    D2D1_RECT_F at{};
    for (auto& c : g_optCtls) if (In(c.r, p)) { id = c.id; at = c.r; break; }
    if (id < 0) return false;
    auto below = [&] { POINT q{ (LONG)(at.left * g_scale), (LONG)((at.bottom + 2) * g_scale) }; ClientToScreen(g_hwnd, &q); return q; };
    switch (id) {
    case OPT_CLOSE: g_viewOpts = false; Invalidate(); return true;
    case OPT_GROUP: {
        int r = ctxmenu::ShowItems(g_hwnd, below(), GroupItems(100));
        if (r >= 100) { g_groupBy = (GroupBy)(r - 100); Resort(); }
        break;
    }
    case OPT_SORT: {
        std::vector<ctxmenu::Item> items;
        for (int c = SORT_NAME; c <= SORT_CREATED; c++) { ctxmenu::Item it; it.label = SortName((SortCol)c); it.id = 100 + c; it.checked = g_sortCol == c; items.push_back(it); }
        int r = ctxmenu::ShowItems(g_hwnd, below(), items);
        if (r >= 100) { g_sortCol = (SortCol)(r - 100); g_sortAsc = g_sortCol == SORT_NAME || g_sortCol == SORT_KIND; Resort(); }
        break;
    }
    case OPT_TEXT_SIZE: {
        float& size = g_view == View::Icons ? g_labelSize : g_rowTextSize;
        std::vector<ctxmenu::Item> items;
        for (int v = 10; v <= 16; v++) { ctxmenu::Item it; it.label = std::to_wstring(v); it.id = 100 + v; it.checked = (int)size == v; items.push_back(it); }
        int r = ctxmenu::ShowItems(g_hwnd, below(), items);
        if (r >= 100) { size = (float)(r - 100); MakeBodyFormats(); }
        break;
    }
    case OPT_ICON_SLIDER: g_optSliderDrag = true; SetCapture(g_hwnd); OptSlider(p.x); return true;
    case OPT_THUMBS: g_showThumbs = !g_showThumbs; break;
    case OPT_LIST_SMALL: g_listLarge = false; break;
    case OPT_LIST_LARGE: g_listLarge = true; break;
    case OPT_COL_SIZE: g_colSize = !g_colSize; break;
    case OPT_COL_KIND: g_colKind = !g_colKind; break;
    case OPT_COL_DATE: g_colDate = !g_colDate; break;
    case OPT_COL_CREATED: g_colCreated = !g_colCreated; break;
    case OPT_REL_DATES: g_relDates = !g_relDates; break;
    case OPT_COLS_ICONS: g_colIcons = !g_colIcons; break;
    case OPT_COLS_PREVIEW: g_colPreview = !g_colPreview; break;
    case OPT_DEFAULT_VIEW: g_defaultView = g_view; Flash(Tr(L"Novas janelas vão abrir nesta visão")); break;
    }
    OptionsChanged();
    return true;
}


// ---------------------------------------------------------------------------
// Abas

static std::wstring TabTitle(int i) {
    if (i == g_curTab) return CurrentTitle();
    const TabState& t = g_tabs[i];
    return t.tagView >= 0 ? VirtualName(t.tagView) : DisplayName(t.folder);
}

static void SaveCurTab() {
    TabState& t = g_tabs[g_curTab];
    t.folder = g_searching ? g_searchHome : g_folder; t.tagView = g_tagView; t.back = g_back; t.fwd = g_fwd;
    t.view = g_view; t.scroll = g_scroll; t.colTrail = g_colTrail; t.sel = SelectedPaths();
}

static void LoadTab(int i) {
    g_curTab = i;
    TabState t = g_tabs[i];
    if (g_renameIdx >= 0) EndRename(true);
    g_ql = false;
    g_view = t.view;
    if (t.tagView >= 0) NavigateTag(t.tagView);
    else { g_colTrail = t.colTrail; Navigate(t.folder, false, L"", true); }
    g_back = t.back; g_fwd = t.fwd;
    g_rowsDirty = true;
    g_scroll = g_scrollTarget = std::min(t.scroll, MaxScroll());
    for (auto& e : g_items) e.sel = std::find_if(t.sel.begin(), t.sel.end(), [&](const std::wstring& p) { return SamePath(p, e.path); }) != t.sel.end();
    for (int k = 0; k < (int)g_items.size(); k++) if (g_items[k].sel) { g_focus = g_anchor = k; break; }
    Invalidate();
}

static void NewTab(const std::wstring& folder) {
    SaveCurTab();
    TabState t;
    t.folder = folder;
    t.view = g_view;
    g_tabs.insert(g_tabs.begin() + g_curTab + 1, t);
    g_back.clear(); g_fwd.clear();
    g_curTab++;
    g_colTrail.clear();
    Navigate(folder, false);
    g_back.clear(); g_fwd.clear();
    Invalidate();
}

static void CloseTab(int i) {
    if (g_tabs.size() <= 1) { PostMessageW(g_hwnd, WM_CLOSE, 0, 0); return; }
    if (i != g_curTab) SaveCurTab();
    g_tabs.erase(g_tabs.begin() + i);
    if (i == g_curTab) LoadTab(std::min(i, (int)g_tabs.size() - 1));
    else { if (i < g_curTab) g_curTab--; Invalidate(); }
}

static void SwitchTab(int i) {
    if (i == g_curTab || i < 0 || i >= (int)g_tabs.size()) return;
    SaveCurTab();
    LoadTab(i);
}

// Geometria: abas de largura igual e o botão "+" na ponta direita.
static D2D1_RECT_F TabRect(int i) {
    auto s = ClientDip();
    float w = (s.width - Lay::SIDEBAR_W - 34) / g_tabs.size();
    float x = Lay::SIDEBAR_W + i * w;
    return { x, Lay::TOOLBAR_H, x + w, Lay::TOOLBAR_H + TAB_H };
}
static D2D1_RECT_F TabCloseRect(int i) { auto r = TabRect(i); return { r.left + 8, r.top + 7, r.left + 24, r.top + 23 }; }
static D2D1_RECT_F TabPlusRect() { auto s = ClientDip(); return { s.width - 34, Lay::TOOLBAR_H, s.width, Lay::TOOLBAR_H + TAB_H }; }
static int g_hoverTab = -1;

static int HitTab(D2D1_POINT_2F p) {
    if (TabBarH() == 0 || p.y < Lay::TOOLBAR_H || p.y >= Lay::TOOLBAR_H + TAB_H) return -1;
    for (int i = 0; i < (int)g_tabs.size(); i++) if (In(TabRect(i), p)) return i;
    return -1;
}

static void DrawTabBar(ID2D1DeviceContext* dc, const Palette& pal, ID2D1SolidColorBrush* br) {
    if (TabBarH() == 0) return;
    auto s = ClientDip();
    D2D1_RECT_F bar{ Lay::SIDEBAR_W, Lay::TOOLBAR_H, s.width, Lay::TOOLBAR_H + TAB_H };
    br->SetColor(g_dark ? D2D1::ColorF(0.10f, 0.10f, 0.11f) : D2D1::ColorF(0.88f, 0.88f, 0.89f));  // abas de fundo mais escuras
    dc->FillRectangle(bar, br);
    for (int i = 0; i < (int)g_tabs.size(); i++) {
        auto r = TabRect(i);
        bool cur = i == g_curTab;
        if (cur) { br->SetColor(pal.bar); dc->FillRectangle(r, br); }
        else if (i == g_hoverTab) { br->SetColor(pal.hover); dc->FillRectangle(r, br); }
        if (i > 0 && !cur && i - 1 != g_curTab) {  // divisória entre abas de fundo
            br->SetColor(pal.line);
            dc->DrawLine({ r.left + 0.5f, r.top + 7 }, { r.left + 0.5f, r.bottom - 7 }, br, 1);
        }
        std::wstring title = TabTitle(i);
        br->SetColor(cur ? pal.text : pal.text2);
        dc->DrawTextW(title.c_str(), (UINT32)title.size(), g_tfQlButton, D2D1::RectF(r.left + 28, r.top, r.right - 28, r.bottom), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        if (i == g_hoverTab) {  // ✕ ao passar o mouse
            auto c = TabCloseRect(i);
            float cx = (c.left + c.right) / 2, cy = (c.top + c.bottom) / 2;
            br->SetColor(pal.text2);
            Line(dc, br, cx - 3.5f, cy - 3.5f, cx + 3.5f, cy + 3.5f, 1.3f);
            Line(dc, br, cx - 3.5f, cy + 3.5f, cx + 3.5f, cy - 3.5f, 1.3f);
        }
    }
    auto pr = TabPlusRect();  // +
    float cx = (pr.left + pr.right) / 2, cy = (pr.top + pr.bottom) / 2;
    br->SetColor(pal.text2);
    Line(dc, br, cx - 5, cy, cx + 5, cy, 1.4f);
    Line(dc, br, cx, cy - 5, cx, cy + 5, 1.4f);
    br->SetColor(g_dark ? D2D1::ColorF(0, 0, 0, 0.6f) : D2D1::ColorF(0, 0, 0, 0.12f));
    dc->DrawLine({ Lay::SIDEBAR_W, bar.bottom - 0.5f }, { s.width, bar.bottom - 0.5f }, br, 1);
}

// Clique na barra de abas: true se consumido.
static bool TabClick(D2D1_POINT_2F p, bool middle) {
    if (TabBarH() == 0 || p.x < Lay::SIDEBAR_W || p.y < Lay::TOOLBAR_H || p.y >= Lay::TOOLBAR_H + TAB_H) return false;
    if (In(TabPlusRect(), p)) { if (!middle) NewTab(g_folder.empty() ? KnownFolder(FOLDERID_Profile) : g_folder); return true; }
    int i = HitTab(p);
    if (i < 0) return true;
    if (middle || In(TabCloseRect(i), p)) CloseTab(i);
    else SwitchTab(i);
    return true;
}


// Barra "Buscar: Este Computador | “Pasta”" logo abaixo da barra de ferramentas (e das abas).
static D2D1_RECT_F ScopeBtnRect(int k) {
    float y = Lay::TOOLBAR_H + TabBarH();
    float x = Lay::SIDEBAR_W + 62;
    float w0 = 122;
    std::wstring home = L"“" + DisplayName(g_searchHome) + L"”";
    float w1 = std::min(220.f, 26 + 7.f * home.size());
    if (k == 0) return { x, y + 4, x + w0, y + SCOPE_H - 4 };
    return { x + w0 + 6, y + 4, x + w0 + 6 + w1, y + SCOPE_H - 4 };
}

static D2D1_RECT_F TrashEmptyRect() {
    auto s = ClientDip();
    float y = Lay::TOOLBAR_H + TabBarH() + ScopeBarH();
    return { s.width - 16 - 82, y + 8, s.width - 16, y + TRASH_BAR_H - 8 };
}

static void DrawTrashBar(ID2D1DeviceContext* dc, const Palette& pal, ID2D1SolidColorBrush* br) {
    if (g_tagView != VIEW_TRASH) return;
    auto s = ClientDip();
    float y = Lay::TOOLBAR_H + TabBarH() + ScopeBarH();
    br->SetColor(pal.bar);
    dc->FillRectangle(D2D1::RectF(Lay::SIDEBAR_W, y, s.width, y + TRASH_BAR_H), br);
    br->SetColor(g_dark ? D2D1::ColorF(0, 0, 0, 0.6f) : D2D1::ColorF(0, 0, 0, 0.12f));
    dc->DrawLine({ Lay::SIDEBAR_W, y + TRASH_BAR_H - 0.5f }, { s.width, y + TRASH_BAR_H - 0.5f }, br, 1);
    br->SetColor(pal.text);
    dc->DrawTextW(Tr(L"Lixeira"), (UINT32)wcslen(Tr(L"Lixeira")), g_tfSide, D2D1::RectF(Lay::SIDEBAR_W + 16, y, Lay::SIDEBAR_W + 200, y + TRASH_BAR_H), br);  // à esquerda, como no Mac
    auto r = TrashEmptyRect();
    bool any = !g_all.empty();
    br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, g_trashPress ? 0.26f : 0.16f) : D2D1::ColorF(0, 0, 0, g_trashPress ? 0.14f : 0.07f));
    dc->FillRoundedRectangle(D2D1::RoundedRect(r, 6, 6), br);
    br->SetColor(any ? pal.text : pal.text2);
    dc->DrawTextW(Tr(L"Esvaziar"), (UINT32)wcslen(Tr(L"Esvaziar")), g_tfQlButton, r, br);
}

static void DrawScopeBar(ID2D1DeviceContext* dc, const Palette& pal, ID2D1SolidColorBrush* br) {
    if (!g_searching) return;
    auto s = ClientDip();
    float y = Lay::TOOLBAR_H + TabBarH();
    D2D1_RECT_F bar{ Lay::SIDEBAR_W, y, s.width, y + SCOPE_H };
    br->SetColor(pal.bar);
    dc->FillRectangle(bar, br);
    br->SetColor(pal.text2);
    dc->DrawTextW(Tr(L"Buscar:"), (UINT32)wcslen(Tr(L"Buscar:")), g_tfQlButton, D2D1::RectF(Lay::SIDEBAR_W + 14, y, Lay::SIDEBAR_W + 62, y + SCOPE_H), br);
    std::wstring labels[2] = { Tr(L"Este Computador"), L"“" + DisplayName(g_searchHome) + L"”" };
    for (int k = 0; k < 2; k++) {
        auto r = ScopeBtnRect(k);
        bool on = g_searchScope == k;
        if (on) {
            br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.16f) : D2D1::ColorF(0, 0, 0, 0.10f));
            dc->FillRoundedRectangle(D2D1::RoundedRect(r, 5, 5), br);
        }
        br->SetColor(on ? pal.text : pal.text2);
        dc->DrawTextW(labels[k].c_str(), (UINT32)labels[k].size(), g_tfQlButton, D2D1::RectF(r.left + 10, r.top, r.right - 8, r.bottom), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    br->SetColor(g_dark ? D2D1::ColorF(0, 0, 0, 0.6f) : D2D1::ColorF(0, 0, 0, 0.12f));
    dc->DrawLine({ Lay::SIDEBAR_W, bar.bottom - 0.5f }, { s.width, bar.bottom - 0.5f }, br, 1);
}

static bool ScopeClick(D2D1_POINT_2F p) {
    if (!g_searching) return false;
    float y = Lay::TOOLBAR_H + TabBarH();
    if (p.x < Lay::SIDEBAR_W || p.y < y || p.y >= y + SCOPE_H) return false;
    for (int k = 0; k < 2; k++)
        if (In(ScopeBtnRect(k), p) && k != g_searchScope && (k == 0 || !g_searchHome.empty())) { g_searchScope = k; RunSearch(); }
    return true;
}


// ---------------------------------------------------------------------------
// Modo escolher (Abrir/Salvar)

static D2D1_RECT_F PickOkRect() {
    auto s = ClientDip(); float y = s.height - Lay::STATUS_H / 2;
    if (tahoe::On()) return { s.width - 110, y - 15, s.width - 16, y + 15 };  // cápsula do Tahoe
    return { s.width - 104, y - 14, s.width - 16, y + 14 };
}
static D2D1_RECT_F PickCancelRect() { auto o = PickOkRect(); float w = tahoe::On() ? 104 : 98; return { o.left - w, o.top, o.left - 10, o.bottom }; }
static D2D1_RECT_F PickTypeRect() {
    auto s = ClientDip(); float y = s.height - Lay::STATUS_H / 2;
    float left = Lay::SIDEBAR_W + 16 + (g_pick.mode == 1 ? 330 : 0);
    return { left, y - 13, std::min(left + 260, PickCancelRect().left - 16), y + 13 };
}
static D2D1_RECT_F PickNameRect() { auto s = ClientDip(); float y = s.height - Lay::STATUS_H / 2; return { Lay::SIDEBAR_W + 98, y - 13, Lay::SIDEBAR_W + 330, y + 13 }; }

// Tahoe: botão em cápsula. O principal é a cor de destaque com um brilho no alto; o outro é vidro (tahoe::Capsule)
static void PickButtonTahoe(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const Palette& pal, D2D1_RECT_F r, const std::wstring& label, bool primary) {
    float rad = (r.bottom - r.top) / 2;
    if (primary) {
        br->SetColor(D2D1::ColorF(0, 0, 0, g_dark ? 0.25f : 0.10f));  // sombra curta
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left, r.top + 1, r.right, r.bottom + 1.2f), rad, rad), br);
        br->SetColor(pal.accent);
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), br);
        ID2D1GradientStopCollection* gs = nullptr;
        D2D1_GRADIENT_STOP st[2] = { { 0, D2D1::ColorF(1, 1, 1, 0.22f) }, { 1, D2D1::ColorF(1, 1, 1, 0) } };
        if (SUCCEEDED(dc->CreateGradientStopCollection(st, 2, &gs))) {
            ID2D1LinearGradientBrush* lg = nullptr;
            if (SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, r.top }, { 0, (r.top + r.bottom) / 2 }), gs, &lg))) {
                dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), lg);
                lg->Release();
            }
            gs->Release();
        }
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.28f));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), rad - 0.5f, rad - 0.5f), br, 0.7f);
    } else {
        tahoe::Capsule(dc, br, r, -1, g_dark, 0.7f);
        if (g_dark) {  // no escuro o botão secundário é um cinza um pouco mais claro que o vidro
            br->SetColor(D2D1::ColorF(1, 1, 1, 0.07f));
            dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), br);
        }
    }
    br->SetColor(primary ? D2D1::ColorF(1, 1, 1) : pal.text);
    dc->DrawTextW(label.c_str(), (UINT32)label.size(), g_tfQlButton, r, br);
}

static void PickButton(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* br, const Palette& pal, D2D1_RECT_F r, const std::wstring& label, bool primary) {
    if (tahoe::On()) { PickButtonTahoe(dc, br, pal, r, label, primary); return; }
    br->SetColor(primary ? pal.accent : (g_dark ? D2D1::ColorF(1, 1, 1, 0.14f) : D2D1::ColorF(0, 0, 0, 0.07f)));
    dc->FillRoundedRectangle(D2D1::RoundedRect(r, 6, 6), br);
    br->SetColor(primary ? D2D1::ColorF(1, 1, 1) : pal.text);
    dc->DrawTextW(label.c_str(), (UINT32)label.size(), g_tfQlButton, r, br);
}

static void DrawPicker(ID2D1DeviceContext* dc, const Palette& pal, ID2D1SolidColorBrush* br) {
    if (g_pick.mode == 1) {  // Salvar como: [nome]
        auto s = ClientDip(); float y = s.height - Lay::STATUS_H / 2;
        br->SetColor(pal.text2);
        dc->DrawTextW(Tr(L"Salvar como:"), (UINT32)wcslen(Tr(L"Salvar como:")), g_tfStatus, D2D1::RectF(Lay::SIDEBAR_W + 12, y - 13, Lay::SIDEBAR_W + 96, y + 13), br);
        auto nr = PickNameRect();
        float fr = tahoe::On() ? (nr.bottom - nr.top) / 2 : 6;  // Tahoe: campo em cápsula
        br->SetColor(pal.field);
        dc->FillRoundedRectangle(D2D1::RoundedRect(nr, fr, fr), br);
        if (g_pickNameFocus) {
            br->SetColor(D2D1::ColorF(pal.accent.r, pal.accent.g, pal.accent.b, 0.55f));
            dc->DrawRoundedRectangle(D2D1::RoundedRect(Inflate(nr, 1.5f), fr + 1.5f, fr + 1.5f), br, 3);
        }
        ID2D1SolidColorBrush *sel = nullptr, *txt = nullptr;
        dc->CreateSolidColorBrush(D2D1::ColorF(pal.accent.r, pal.accent.g, pal.accent.b, 0.30f), &sel);
        dc->CreateSolidColorBrush(pal.text, &txt);
        float pad = tahoe::On() ? 12.f : 8.f;
        g_pickName.Draw(dc, g_tfSearch, D2D1::RectF(nr.left + pad, nr.top, nr.right - pad, nr.bottom), txt, sel, txt, g_pickNameFocus && g_caretOn, false);
        sel->Release(); txt->Release();
    }
    if (!g_pick.types.empty() && g_pick.mode != 2) {  // Formato ⌄
        auto tr = PickTypeRect();
        std::wstring t = g_pick.types[std::clamp(g_pick.type, 0, (int)g_pick.types.size() - 1)].first;
        float cx = tr.right - 14, cy = (tr.top + tr.bottom) / 2;
        if (tahoe::On()) {
            tr.top -= 2; tr.bottom += 2;  // mesma altura dos botões
            tahoe::Capsule(dc, br, tr, -1, g_dark, 0.7f);
            if (g_dark) { br->SetColor(D2D1::ColorF(1, 1, 1, 0.07f)); float rr = (tr.bottom - tr.top) / 2; dc->FillRoundedRectangle(D2D1::RoundedRect(tr, rr, rr), br); }
            br->SetColor(pal.text);
            auto al = g_tfStatus->GetTextAlignment();
            g_tfStatus->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            dc->DrawTextW(t.c_str(), (UINT32)t.size(), g_tfStatus, D2D1::RectF(tr.left + 14, tr.top, tr.right - 28, tr.bottom), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            g_tfStatus->SetTextAlignment(al);
            Line(dc, br, cx - 3, cy - 2, cx, cy - 5, 1.3f);  // ⌃
            Line(dc, br, cx, cy - 5, cx + 3, cy - 2, 1.3f);
            Line(dc, br, cx - 3, cy + 2, cx, cy + 5, 1.3f);  // ⌄
            Line(dc, br, cx, cy + 5, cx + 3, cy + 2, 1.3f);
        } else {
            br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.10f) : D2D1::ColorF(0, 0, 0, 0.06f));
            dc->FillRoundedRectangle(D2D1::RoundedRect(tr, 6, 6), br);
            br->SetColor(pal.text);
            dc->DrawTextW(t.c_str(), (UINT32)t.size(), g_tfStatus, D2D1::RectF(tr.left + 10, tr.top, tr.right - 22, tr.bottom), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            cx = tr.right - 12;
            Line(dc, br, cx - 3.5f, cy - 1.5f, cx, cy + 2, 1.3f);
            Line(dc, br, cx, cy + 2, cx + 3.5f, cy - 1.5f, 1.3f);
        }
    }
    PickButton(dc, br, pal, PickCancelRect(), Tr(L"Cancelar"), false);
    std::wstring ok = !g_pick.ok.empty() ? g_pick.ok : g_pick.mode == 1 ? Tr(L"Salvar") : Tr(L"Abrir");
    PickButton(dc, br, pal, PickOkRect(), ok, true);
}

// Abrir/Salvar lembra a última pasta de cada programa (como o Mac), em %APPDATA%\Arquivos\escolher.txt
static std::wstring PickerOwnerExe() {
    DWORD pid = 0;
    if (!g_pick.owner || !GetWindowThreadProcessId(g_pick.owner, &pid)) return L"";
    HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hp) return L"";
    wchar_t path[MAX_PATH]; DWORD n = MAX_PATH;
    std::wstring exe = QueryFullProcessImageNameW(hp, 0, path, &n) ? PathFindFileNameW(path) : L"";
    CloseHandle(hp);
    for (auto& c : exe) c = towlower(c);
    return exe;
}
static std::wstring PickerMemoryFile() { return KnownFolder(FOLDERID_RoamingAppData) + L"\\Arquivos\\escolher.txt"; }
static std::vector<std::pair<std::wstring, std::wstring>> PickerMemory() {
    std::vector<std::pair<std::wstring, std::wstring>> m;
    FILE* f = nullptr;
    if (_wfopen_s(&f, PickerMemoryFile().c_str(), L"r, ccs=UTF-8") || !f) return m;
    wchar_t line[1024];
    while (fgetws(line, 1024, f)) {
        std::wstring l = line;
        while (!l.empty() && (l.back() == L'\n' || l.back() == L'\r')) l.pop_back();
        size_t eq = l.find(L'=');
        if (eq != std::wstring::npos) m.push_back({ l.substr(0, eq), l.substr(eq + 1) });
    }
    fclose(f);
    return m;
}
static void PickerRemember() {
    std::wstring exe = PickerOwnerExe();
    if (exe.empty() || g_folder.empty()) return;
    auto m = PickerMemory();
    bool found = false;
    for (auto& [k, v] : m) if (k == exe) { v = g_folder; found = true; }
    if (!found) m.push_back({ exe, g_folder });
    FILE* f = nullptr;
    if (_wfopen_s(&f, PickerMemoryFile().c_str(), L"w, ccs=UTF-8") || !f) return;
    for (auto& [k, v] : m) fwprintf(f, L"%ls=%ls\n", k.c_str(), v.c_str());
    fclose(f);
}

// Antes de fechar: o foco volta para o programa que pediu. A janela do Finder pertence à janela do
// programa, mas ela está desativada (o diálogo original, fora da tela, ainda está aberto); sem isto o
// Windows pulava para a próxima janela da tela (o Chrome, por exemplo) e o programa ia para trás.
// Ativamos o diálogo original (que está ativo) ou, sem ele, o próprio programa.
static void PickerHandBack() {
    HWND owner = g_pick.owner;
    if (!owner || !IsWindow(owner)) return;
    struct F { HWND owner, dlg; } f{ owner, nullptr };
    EnumWindows([](HWND w, LPARAM l) -> BOOL {
        auto* f = (F*)l;
        if (GetWindow(w, GW_OWNER) != f->owner || !IsWindowEnabled(w)) return TRUE;
        wchar_t cls[16] = {}; GetClassNameW(w, cls, 16);
        if (wcscmp(cls, L"#32770")) return TRUE;
        f->dlg = w;
        return FALSE;
    }, (LPARAM)&f);
    HWND target = f.dlg ? f.dlg : owner;
    SetForegroundWindow(target);   // primeiro o foco (ainda somos a janela da frente: o Windows deixa)...
    ShowWindow(g_hwnd, SW_HIDE);   // ...depois some, sem outra janela pular na frente
    Log(L"escolher: foco devolvido para %ls", f.dlg ? L"o diálogo original" : L"o programa");
}

static void PickerClose() {
    if (g_finderEmbutido) { ShowWindow(g_hwnd, SW_HIDE); g_pickDone = true; return; }
    DestroyWindow(g_hwnd);
}

static void PickerFinish(const std::vector<std::wstring>& paths) {
    if (!g_pick.result.empty()) {
        FILE* f = nullptr;
        if (!_wfopen_s(&f, g_pick.result.c_str(), L"w, ccs=UTF-8") && f) {
            for (auto& p : paths) fwprintf(f, L"%ls\n", p.c_str());
            fwprintf(f, L"tipo=%d\n", g_pick.type);
            fclose(f);
        }
    }
    g_exitCode = 0;
    PickerRemember();
    SaveOptions();
    PickerHandBack();
    Log(L"escolher: %zu item(ns)", paths.size());
    PickerClose();
}

static void PickerCancel() { g_exitCode = 1; SaveOptions(); PickerHandBack(); PickerClose(); }

static void PickerAccept(int clicked) {
    std::vector<std::wstring> out;
    if (g_pick.mode == 1) {  // salvar
        std::wstring name = g_pickName.text;
        while (!name.empty() && name.back() == L' ') name.pop_back();
        if (name.empty() || g_folder.empty()) { MessageBeep(MB_ICONWARNING); return; }
        if (!g_pick.ext.empty() && PathFindExtensionW(name.c_str())[0] == 0) name += L"." + g_pick.ext;
        std::wstring path = Join(g_folder, name);
        if (g_pick.overwrite && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            std::wstring q = L"“" + name + Tr(L"” já existe. Deseja substituí-lo?");
            if (MessageBoxW(g_hwnd, q.c_str(), Tr(L"Salvar"), MB_YESNO | MB_ICONWARNING) != IDYES) return;
        }
        out.push_back(path);
    } else {
        if (clicked >= 0) {
            if (g_items[clicked].dir && g_pick.mode != 2) { Navigate(g_items[clicked].path); return; }
            out.push_back(g_items[clicked].path);
        } else {
            for (auto& e : g_items) if (e.sel && (g_pick.mode == 2 ? e.dir : !e.dir)) out.push_back(e.path);
            if (out.empty()) {
                if (g_pick.mode == 2 && !g_folder.empty()) out.push_back(g_folder);  // a pasta aberta
                else if (g_focus >= 0 && g_items[g_focus].dir) { Navigate(g_items[g_focus].path); return; }  // Enter numa pasta entra nela
                else { MessageBeep(MB_ICONWARNING); return; }
            }
            if (!g_pick.multi && out.size() > 1) out.resize(1);
        }
    }
    PickerFinish(out);
}

static bool PickerClick(D2D1_POINT_2F p) {
    if (!g_pick.on) return false;
    auto s = ClientDip();
    if (p.y < s.height - Lay::STATUS_H || p.x < Lay::SIDEBAR_W) {
        if (g_pickNameFocus) { g_pickNameFocus = false; Invalidate(); }
        return false;
    }
    if (In(PickOkRect(), p)) PickerAccept(-1);
    else if (In(PickCancelRect(), p)) PickerCancel();
    else if (g_pick.mode == 1 && In(PickNameRect(), p)) {
        g_pickNameFocus = true; g_searchFocus = false; g_caretOn = true;
        SetTimer(g_hwnd, T_CARET, 530, nullptr);
        g_pickName.Click(g_tfSearch, p, false);
        Invalidate();
    } else if (!g_pick.types.empty() && g_pick.mode != 2 && In(PickTypeRect(), p)) {
        std::vector<ctxmenu::Item> items;
        for (int i = 0; i < (int)g_pick.types.size(); i++) {
            ctxmenu::Item it; it.label = g_pick.types[i].first + L"  (" + g_pick.types[i].second + L")"; it.id = i + 1; it.checked = i == g_pick.type;
            items.push_back(it);
        }
        auto tr = PickTypeRect();
        POINT pt{ (LONG)(tr.left * g_scale), (LONG)(tr.bottom * g_scale) };
        ClientToScreen(g_hwnd, &pt);
        int id = ctxmenu::ShowItems(g_hwnd, pt, items);
        if (id > 0) { g_pick.type = id - 1; Reload(); }
    }
    return true;
}

// Ao clicar num arquivo no modo salvar, o nome vai para o campo (como no Mac).
static void PickerSelectionChanged() {
    if (!g_pick.on || g_pick.mode != 1 || g_focus < 0 || g_focus >= (int)g_items.size() || g_items[g_focus].dir) return;
    g_pickName.SetText(g_items[g_focus].name, false);
}

static LRESULT OnLaunchRequest(const COPYDATASTRUCT* cd);  // reserva recebendo um pedido (mais abaixo)
static bool HandleKey(UINT vk) {
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
    g_caretOn = true;

    if (g_renameIdx >= 0) {
        if (vk == VK_RETURN) { EndRename(true); return true; }
        if (vk == VK_ESCAPE) { EndRename(false); return true; }
        if (vk == VK_TAB) { EndRename(true); return true; }
        if (g_rename.OnKey(vk, shift, ctrl)) { Invalidate(); return true; }
        return false;
    }
    if (g_pick.on && g_pickNameFocus) {  // campo "Salvar como"
        if (vk == VK_RETURN) { PickerAccept(-1); return true; }
        if (vk == VK_ESCAPE) { PickerCancel(); return true; }
        if (vk == VK_TAB) { g_pickNameFocus = false; Invalidate(); return true; }
        if (g_pickName.OnKey(vk, shift, ctrl)) { Invalidate(); return true; }
        return false;
    }
    if (GoKey(vk, shift, ctrl)) return true;  // Ir para a Pasta aberto
    if (ctrl && shift && vk == 'G') { GoOpen(true); return true; }
    if (g_pick.on && !g_searchFocus && vk == VK_RETURN) { PickerAccept(-1); return true; }
    if (g_pick.on && !g_searchFocus && vk == VK_ESCAPE && !g_ql && !g_viewOpts) { PickerCancel(); return true; }
    if (g_searchFocus) {
        if (vk == VK_ESCAPE) { if (!g_search.text.empty()) { g_search.SetText(L"", false); OnSearchChanged(); } else FocusSearch(false); Invalidate(); return true; }
        if (vk == VK_RETURN) RunSearch();  // Enter busca na hora
        if (vk == VK_RETURN || vk == VK_DOWN || vk == VK_TAB) { FocusSearch(false); if (!g_items.empty()) SelectOnly(0); Invalidate(); return true; }
        if (g_search.OnKey(vk, shift, ctrl)) { OnSearchChanged(); Invalidate(); return true; }
        return false;
    }

    if (vk == VK_ESCAPE && !g_qlExtPath.empty() && QlShellWnd()) { QlSend(L""); return true; }
    if (g_ql && vk == VK_ESCAPE) { QlClose(); return true; }
    if (g_viewOpts && vk == VK_ESCAPE) { g_viewOpts = false; Invalidate(); return true; }
    if (g_searching && vk == VK_ESCAPE) { ExitSearch(); return true; }
    if (ctrl && vk == 'J') { g_viewOpts = !g_viewOpts; Invalidate(); return true; }
    if (ctrl && vk == 'F') { FocusSearch(true); return true; }
    if (ctrl && !alt && vk == 'T') { NewTab(g_folder.empty() ? KnownFolder(FOLDERID_Profile) : g_folder); return true; }
    if (ctrl && vk == VK_TAB) { int n = (int)g_tabs.size(); SwitchTab((g_curTab + (shift ? n - 1 : 1)) % n); return true; }
    if (ctrl && alt && vk == 'T') {  // adicionar as pastas selecionadas à barra lateral (Ctrl+Cmd+T no Mac)
        for (auto& e : g_items) if (e.sel && e.dir) AddFavorite(e.path, 1 << 20);
        return true;
    }
    if (ctrl && vk == '1') { SetView(View::Icons); return true; }
    if (ctrl && vk == '2') { SetView(View::List); return true; }
    if (ctrl && vk == '3') { SetView(View::Columns); return true; }
    if (ctrl && vk == '4') { SetView(View::Gallery); return true; }
    if (g_view == View::Columns && !ctrl && !alt && (vk == VK_LEFT || vk == VK_RIGHT)) {  // ← volta uma coluna; → entra na pasta
        if (vk == VK_LEFT) ColumnLeft();
        else if (g_focus >= 0 && g_items[g_focus].dir) ColumnEnter(g_focus, true);
        return true;
    }
    if (ctrl && (vk == VK_OEM_PLUS || vk == VK_ADD)) { SetIconSize(g_icon + 16); return true; }
    if (ctrl && (vk == VK_OEM_MINUS || vk == VK_SUBTRACT)) { SetIconSize(g_icon - 16); return true; }
    if (ctrl && vk == 'A') { for (auto& e : g_items) e.sel = true; Invalidate(); return true; }
    if (ctrl && vk == 'Z') { Undo(); return true; }
    if (ctrl && alt && vk == 'P') {  // Mostrar/Ocultar Barra de Caminho (Option+Cmd+P no Mac)
        g_pathBar = !g_pathBar;
        Lay::FOOTER_H = Lay::STATUS_H + (g_pathBar ? Lay::PATH_H : 0);
        SaveOptions();
        Invalidate();
        return true;
    }
    if (ctrl && vk == 'C') { RunVerb("copy"); return true; }
    if (ctrl && vk == 'X') { RunVerb("cut"); return true; }
    if (ctrl && vk == 'V') { if (g_folder.empty()) Flash(Tr(L"Abra uma pasta para colar")); else if (FAILED(PasteInto(g_hwnd, g_folder))) Flash(Tr(L"Nada para colar")); return true; }
    if (ctrl && shift && vk == 'N') { NewFolder(); return true; }
    if (ctrl && !shift && vk == 'N') {
        if (g_finderNativo) { NewTab(g_folder); return true; }
        wchar_t exe[MAX_PATH]; ExePath(exe, MAX_PATH);
        ShellExecuteW(nullptr, nullptr, exe, (L"\"" + g_folder + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        return true;
    }
    if (ctrl && vk == 'W') { CloseTab(g_curTab); return true; }  // última aba fecha a janela
    if (ctrl && vk == 'I') { GetInfo(SelectedPaths()); return true; }
    if (alt && vk == VK_RETURN) { RunVerb("properties"); return true; }
    if ((ctrl && vk == 'O') || (ctrl && vk == VK_DOWN)) { OpenSelection(); return true; }
    if ((ctrl || alt) && vk == VK_UP) { GoUp(); return true; }
    if ((alt && vk == VK_LEFT) || (ctrl && vk == VK_OEM_4) || (!ctrl && vk == VK_BACK)) { GoBack(); return true; }
    if ((alt && vk == VK_RIGHT) || (ctrl && vk == VK_OEM_6)) { GoForward(); return true; }
    if ((vk == VK_DELETE || (ctrl && vk == VK_BACK)) && g_tagView == VIEW_TRASH) { TrashVerb(SelectedPaths(), L"delete"); RefreshTagView(); return true; }
    if (vk == VK_DELETE || (ctrl && vk == VK_BACK)) { RunVerb("delete"); return true; }
    if (vk == VK_RETURN || vk == VK_F2) { if (SelCount() == 1) StartRename(g_focus); return true; }
    if (vk == VK_SPACE && !ctrl && !alt) { QlToggle(); return true; }
    if (vk == VK_F5) { Reload(); return true; }
    if (vk == VK_ESCAPE) { ClearSel(); g_focus = -1; Invalidate(); return true; }
    switch (vk) {
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
        MoveFocus(vk, shift); return true;
    }
    return false;
}

static LRESULT HitTestNC(HWND h, LPARAM l) {
    POINT p{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
    ScreenToClient(h, &p);
    RECT rc; GetClientRect(h, &rc);
    if (!IsZoomed(h)) {
        int b = (int)(6 * g_scale);
        bool L = p.x < b, R = p.x >= rc.right - b, T = p.y < b, B = p.y >= rc.bottom - b;
        if (T && L) return HTTOPLEFT;
        if (T && R) return HTTOPRIGHT;
        if (B && L) return HTBOTTOMLEFT;
        if (B && R) return HTBOTTOMRIGHT;
        if (L) return HTLEFT;
        if (R) return HTRIGHT;
        if (T) return HTTOP;
        if (B) return HTBOTTOM;
    }
    D2D1_POINT_2F d = ToDip(p.x, p.y);
    if (g_ql && In(QlPanel(), d)) return HTCLIENT;  // o Quick Look fica por cima da barra
    if (d.y < Lay::TOOLBAR_H && !OverLightsArea(d) && HitTool(d) < 0) return HTCAPTION;  // arrastar a janela
    return HTCLIENT;
}

static bool OverTitle(D2D1_POINT_2F p) {
    if (p.y >= Lay::TOOLBAR_H) return false;
    float tx = ToolRect(TB_FWD).right + 12, w = 120;
    std::wstring t = CurrentTitle();
    IDWriteTextLayout* tl = nullptr;
    if (SUCCEEDED(gfx::dw->CreateTextLayout(t.c_str(), (UINT32)t.size(), g_tfTitle, 2000, Lay::TOOLBAR_H, &tl))) {
        DWRITE_TEXT_METRICS m{}; tl->GetMetrics(&m); w = m.width; tl->Release();
    }
    return p.x >= tx - 4 && p.x <= tx + 25 + w + 4;
}

// Menu do caminho, como no Finder: Cmd-clique, Ctrl-clique ou clique direito no título da janela abre a hierarquia de
// pastas (a atual em cima e cada pasta acima dela, até o disco e o computador); escolher uma vai para ela. Aqui um
// clique simples no título (sem arrastar a janela) também abre.
static void NavigateTag(int t);
static void PathMenu() {
    if (g_folder.empty()) return;  // etiqueta, Lixeira, Este Computador: não há caminho
    std::vector<std::wstring> chain;
    std::wstring p = g_folder;
    while (!p.empty()) {
        chain.push_back(p);
        if (p.size() <= 3 && p[1] == L':') break;  // a raiz do disco
        size_t k = p.find_last_of(L'\\');
        if (k == std::wstring::npos) break;
        std::wstring up = p.substr(0, k);
        if (up.size() == 2 && up[1] == L':') up += L'\\';
        if (up == p || up.size() < 3) break;
        p = up;
    }
    std::vector<ctxmenu::Item> items;
    for (size_t i = 0; i < chain.size(); i++) {
        ctxmenu::Item it;
        const std::wstring& c = chain[i];
        bool root = c.size() <= 3;
        if (root) {  // o nome do disco, como o Finder mostra "Macintosh HD" (o mesmo nome da barra de caminho)
            it.label = DisplayName(c);
            it.icon = 0xEDA2;
        } else {
            it.label = c.substr(c.find_last_of(L'\\') + 1);
            it.icon = 0xE8B7;
        }
        it.id = (int)i + 1;
        it.checked = false;
        items.push_back(it);
    }
    ctxmenu::Item pc; pc.label = Tr(L"Este Computador"); pc.icon = 0xE7F4; pc.id = 1000; items.push_back(pc);
    float tx = ToolRect(TB_FWD).right + 12;
    POINT pt{ (LONG)std::lround((tx - 6) * g_scale), (LONG)std::lround((Lay::TOOL_CY + 15) * g_scale) };
    ClientToScreen(g_hwnd, &pt);
    int id = ctxmenu::ShowItems(g_hwnd, pt, items);
    if (id == 1000) { NavigateTag(VIEW_COMPUTER); return; }
    if (id >= 1 && id <= (int)chain.size() && id != 1) Navigate(chain[id - 1]);
}

static void ApplyWindowTheme() {
    if (g_tahoeLay != g_tahoe) {  // medidas do tema (Sequoia: vídeo do Sequoia; Tahoe: guia, seção 42)
        g_tahoeLay = g_tahoe;
        Lay::SIDE_ROW_H = g_tahoe ? 32.f : 28.7f;
        Lay::SIDE_HDR_H = g_tahoe ? 19.f : 23.5f;
        Lay::TOOLBAR_H = g_tahoe ? 52.f : 53.f;
    }
    // Leopard: a barra "unified" do 10.5 tem 55 px (22 do título + a linha de ferramentas, medido numa captura a 1x);
    // os botões descem para a segunda linha e a barra lateral começa abaixo dela (a barra vai de ponta a ponta)
    const bool leoLay = !g_tahoe && tahoe::Leopard();
    // Leopard: a Galeria vira o Cover Flow (lista + capas); saindo do Leopard, o Cover Flow volta a ser a Galeria
    if (leoLay) { if (g_view == View::Gallery) { g_view = View::List; g_coverFlow = true; g_rowsDirty = true; } }
    else if (g_coverFlow) { g_coverFlow = false; g_view = View::Gallery; g_rowsDirty = true; }
    Lay::TOOLBAR_H = g_tahoe ? 52.f : leoLay ? 56.f : 53.f;
    Lay::TOOL_CY = leoLay ? 38.5f : 26.3f;
    ListLay::HEADER_H = leoLay ? 17.f : 28.f;
    g_rowsDirty = true;
    Lay::SIDE_TOP = leoLay ? Lay::TOOLBAR_H + 8 : 47.f;
    g_sideClipTop = Lay::SIDE_TOP;
    BOOL dark = g_dark;
    tags::Tema(g_tahoe, g_dark);
    DwmSetWindowAttribute(g_hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    if (g_tahoe) {  // Tahoe: cantos de 26 pt com o vidro da Composição (tahoe.cpp) e sombra arredondada
        tahoe::Frame(g_hwnd, true);
        RECT rc; GetClientRect(g_hwnd, &rc);
        tahoe::Size(g_hwnd, (float)rc.right, (float)rc.bottom, g_dark);
        shadow::SetRadius(tahoe::WindowR());
        return;
    }
    tahoe::Frame(g_hwnd, false);
    shadow::SetRadius(0);
    COLORREF border = g_dark ? RGB(88, 88, 92) : RGB(196, 196, 200);  // borda fina e clara, como no Mac
    DwmSetWindowAttribute(g_hwnd, 34 /*DWMWA_BORDER_COLOR*/, &border, sizeof(border));
    // Acrílico do sistema, sem tinta extra nossa: é o mais transparente que dá. O desfoque antigo
    // (SetWindowCompositionAttribute) foi testado com tinta e flag de gradiente: o DWM o desenha
    // POR CIMA do conteúdo DirectComposition, apagando texto e ícones. Descartado.
    int backdrop = 3;  // DWMSBT_TRANSIENTWINDOW
    DwmSetWindowAttribute(g_hwnd, 38 /*DWMWA_SYSTEMBACKDROP_TYPE*/, &backdrop, sizeof(backdrop));
}

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_INITMENUPOPUP: case WM_DRAWITEM: case WM_MEASUREITEM: case WM_MENUCHAR: {
        LRESULT r = 0;
        if (ctxmenu::ForwardOwnerMsg(m, w, l, &r)) return r;
        break;
    }
    case WM_NCCALCSIZE:
        if (w) {  // sem barra de título do Windows: a área cliente ocupa a janela toda
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
    case WM_NCLBUTTONDOWN:
        if (w == HTCAPTION) {  // no título: Cmd/Ctrl-clique abre o menu do caminho; clique sem arrastar também
            POINT cp{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
            ScreenToClient(h, &cp);
            if (OverTitle(ToDip(cp.x, cp.y)) && !g_folder.empty()) {
                if (GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_LWIN) < 0) { PathMenu(); return 0; }
                POINT before; GetCursorPos(&before);
                DWORD t0 = GetTickCount();
                DefWindowProcW(h, m, w, l);  // o laço de mover a janela (volta quando o botão solta)
                POINT after; GetCursorPos(&after);
                RECT wr; GetWindowRect(h, &wr);
                if (std::abs(after.x - before.x) <= 3 && std::abs(after.y - before.y) <= 3 && GetTickCount() - t0 < 600) PathMenu();
                return 0;
            }
        }
        break;
    case WM_NCRBUTTONUP:
        if (w == HTCAPTION) {
            POINT cp{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
            ScreenToClient(h, &cp);
            if (OverTitle(ToDip(cp.x, cp.y)) && !g_folder.empty()) { PathMenu(); return 0; }
        }
        break;
    case WM_NCMOUSEMOVE: {  // o título fica na área de arrastar: o mouse ali chega como "não cliente"
        POINT p{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        ScreenToClient(h, &p);
        bool over = OverTitle(ToDip(p.x, p.y));
        if (over != g_hoverTitle) { g_hoverTitle = over; Invalidate(); }
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE | TME_NONCLIENT, h, 0 };
        TrackMouseEvent(&tme);
        break;
    }
    case WM_NCMOUSELEAVE:
        if (g_hoverTitle) { g_hoverTitle = false; Invalidate(); }
        break;
    case WM_GETMINMAXINFO: {
        auto* mm = (MINMAXINFO*)l;
        mm->ptMinTrackSize = { (LONG)(640 * g_scale), (LONG)(420 * g_scale) };
        return 0;
    }
    case WM_SIZE:
        if (w != SIZE_MINIMIZED) {
            gfx::Resize(LOWORD(l), HIWORD(l));
            if (g_tahoe) tahoe::Size(h, LOWORD(l), HIWORD(l), g_dark);
            g_scroll = g_scrollTarget = std::min(g_scroll, MaxScroll());
            g_sideScroll = std::min(g_sideScroll, SideMaxScroll());
            Render();
        }
        return 0;
    case WM_DPICHANGED: {
        g_dpi = HIWORD(w);
        g_scale = g_dpi / 96.f;
        auto* r = (RECT*)l;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps; BeginPaint(h, &ps);
        bool more = StepScroll();
        g_thumbFading = false;
        Render();
        more |= g_thumbFading;
        EndPaint(h, &ps);
        if (more) Invalidate();  // próximo quadro: o Present(1) espera a tela, então anda no ritmo dela (174 Hz)
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_ACTIVATE:
        g_winActive = LOWORD(w) != WA_INACTIVE;
        shadow::SetActive(g_winActive);
        shadow::Update();
        Invalidate();
        break;
    case WM_WINDOWPOSCHANGED:
        shadow::Update();  // a sombra acompanha posição, tamanho e ordem da janela
        break;             // DefWindowProc ainda gera WM_SIZE/WM_MOVE
    case WM_ACTIVATEAPP:
        if (!w) ctxmenu::Close();
        break;
    case WM_SETTINGCHANGE:
        if (l && !wcscmp((const wchar_t*)l, L"ImmersiveColorSet")) { g_dark = IsDarkMode(); ApplyWindowTheme(); Invalidate(); }
        break;
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
            D2D1_POINT_2F d = ToDip(p.x, p.y);
            bool text = (HitTool(d) == TB_SEARCH && SearchExpanded()) || (g_renameIdx >= 0 && In(RenameRect(), d));
            bool edge = g_sideResize || (OverSideEdge(d) && !g_ql);
            SetCursor(LoadCursorW(nullptr, edge ? IDC_SIZEWE : text ? IDC_IBEAM : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
        TrackMouseEvent(&tme);
        D2D1_POINT_2F p = ToDip(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (g_scrollDrag) { ScrollToThumbY(p.y - g_scrollDragOff); return 0; }
        if (g_sideResize) {  // largura da barra lateral (a área de conteúdo fica com pelo menos 320)
            RECT rc; GetClientRect(h, &rc);
            float maxW = std::min(Lay::SIDEBAR_MAX, rc.right / g_scale - 320);
            float nw = std::round(std::clamp(p.x, Lay::SIDEBAR_MIN, std::max(Lay::SIDEBAR_MIN, maxW)));
            if (nw != Lay::SIDEBAR_W) {
                Lay::SIDEBAR_W = nw;
                g_scroll = g_scrollTarget = std::min(g_scroll, MaxScroll());
                Render();
            }
            return 0;
        }
        {  // mouse na borda direita: a barra aparece (e engorda), como no Mac
            D2D1_RECT_F thumb, track;
            bool over = ScrollThumb(thumb, track) && In(track, p) && !g_ql;
            if (over != g_scrollHover) {
                g_scrollHover = over;
                g_scrollShownAt = GetTickCount();
                SetTimer(h, T_SCROLLBAR, 50, nullptr);
                Invalidate();
            }
        }
        int hs = HitSideRow(p), ht = HitTool(p);
        bool hl = OverLightsArea(p);
        { float gx = LightCenter(2).x; ampliacao::Verde(h, hl && p.x >= gx - 11 && p.x < gx + 12); }
        if (g_hoverTitle) { g_hoverTitle = false; Invalidate(); }
        if (int ht2 = HitTab(p); ht2 != g_hoverTab) { g_hoverTab = ht2; Invalidate(); }
        if (g_tahoe && (ht >= 0 || g_hoverTool >= 0)) { g_toolMouse = p; Invalidate(); }
        if (hs != g_hoverSide || ht != g_hoverTool || hl != g_hoverLights) { g_hoverSide = hs; g_hoverTool = ht; g_hoverLights = hl; Invalidate(); }
        if (g_ql && In(QlPanel(), p)) g_qlMouseAt = GetTickCount();  // mostra os controles do vídeo
        if (g_qlSeekDrag) {
            QlSeekAt(p.x);
        } else if (g_optSliderDrag) {
            OptSlider(p.x);
        } else if (g_sidePress >= 0 && g_side[g_sidePress].fav >= 0) {  // arrastando um favorito
            if (!g_sideDragging && std::fabs(p.y - g_sidePressPt.y) > 4) g_sideDragging = true;
            if (g_sideDragging) { g_sideInsert = FavInsertAt(p.y); Invalidate(); }
        } else if (g_sliderDrag) {
            SetIconSizeFromSlider(p.x);
        } else if (g_drag == Drag::Pending) {
            int dx = std::abs(GET_X_LPARAM(l) - g_dragStartPx.x), dy = std::abs(GET_Y_LPARAM(l) - g_dragStartPx.y);
            if (dx > GetSystemMetrics(SM_CXDRAG) || dy > GetSystemMetrics(SM_CYDRAG)) StartDragOut();
        } else if (g_drag == Drag::Band) {
            UpdateBand(p);
            auto c = ContentRect();
            if (p.y < c.top + 12 || p.y > c.bottom - 12) SetTimer(h, T_AUTOSCROLL, 30, nullptr);
            else KillTimer(h, T_AUTOSCROLL);
        } else if ((w & MK_LBUTTON) && g_searchFocus && GetCapture() == h) {
            g_search.Click(g_tfSearch, p, true);
            Invalidate();
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (g_scrollHover) { g_scrollHover = false; g_scrollShownAt = GetTickCount(); SetTimer(h, T_SCROLLBAR, 50, nullptr); Invalidate(); }
        if (g_hoverSide >= 0 || g_hoverTool >= 0 || g_hoverLights) { g_hoverSide = g_hoverTool = -1; g_hoverLights = false; Invalidate(); }
        return 0;
    case WM_LBUTTONDOWN: {
        D2D1_POINT_2F p = ToDip(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (GoClick(p)) return 0;  // Ir para a Pasta
        if (QlClick(p)) return 0;  // clique no painel do Quick Look
        if (CoverFlowClick(p)) return 0;  // Leopard: clique numa capa do Cover Flow
        if (ViewOptsClick(p)) return 0;  // clique no painel de Opções de Visualização
        if (PickerClick(p)) return 0;  // barra de Abrir/Salvar
        if (g_renameIdx >= 0) {
            if (In(RenameRect(), p)) { g_rename.Click(g_tfRename, p, (w & MK_SHIFT) != 0); Invalidate(); return 0; }
            EndRename(true);
        }
        if (OverSideEdge(p) && !g_ql) { g_sideResize = true; SetCapture(h); return 0; }  // arrasta a borda da barra lateral
        int light = HitLight(p);
        if (light >= 0) { g_pressLight = light; SetCapture(h); return 0; }
        int row = HitSideRow(p);
        if (row >= 0 && g_side[row].header) { ToggleSection(row); Invalidate(); return 0; }
        if (TabClick(p, false)) return 0;
        if (ScopeClick(p)) return 0;
        if (g_tagView == VIEW_TRASH && p.x >= Lay::SIDEBAR_W && p.y >= TopH() - TRASH_BAR_H && p.y < TopH() && !(g_view == View::List && false)) {
            if (In(TrashEmptyRect(), p)) { g_trashPress = true; Render(); g_trashPress = false; EmptyTrash(); Invalidate(); }
            return 0;
        }
        if (row >= 0) { g_sidePress = row; g_sidePressPt = p; g_sideDragging = false; SetCapture(h); return 0; }
        int t = HitTool(p);
        if (t == TB_SEARCH) {
            bool wasOpen = SearchExpanded();
            FocusSearch(true);
            if (wasOpen) { g_search.Click(g_tfSearch, p, (w & MK_SHIFT) != 0); SetCapture(h); }
            Invalidate();
            return 0;
        }
        if (t >= 0) { g_pressTool = t; SetCapture(h); Invalidate(); return 0; }  // age ao soltar, como botão
        if (int hc = HitHeader(p); hc >= 0) {  // cabeçalho da lista: ordena (de novo na mesma coluna inverte)
            if (hc == g_sortCol) g_sortAsc = !g_sortAsc;
            else { g_sortCol = (SortCol)hc; g_sortAsc = hc == SORT_NAME || hc == SORT_KIND; }  // datas e tamanhos: maior primeiro
            Resort();
            return 0;
        }
        if (g_view == View::Icons && !g_pick.on && In(Inflate(SliderRect(), 6), p)) { g_sliderDrag = true; SetCapture(h); SetIconSizeFromSlider(p.x); return 0; }
        for (auto& seg : g_pathSegs)  // barra de caminho: um clique abre o trecho
            if (In(seg.rect, p)) { if (!SamePath(seg.path, g_folder)) Navigate(seg.path); return 0; }
        if (!g_ql) {  // barra de rolagem: arrasta o polegar (ou pula para o ponto clicado no trilho)
            D2D1_RECT_F thumb, track;
            if (ScrollThumb(thumb, track) && In(track, p) && (g_scrollHover || GetTickCount() - g_scrollShownAt < 1200)) {
                if (!In(thumb, p)) ScrollToThumbY(p.y - (thumb.bottom - thumb.top) / 2), ScrollThumb(thumb, track);
                g_scrollDrag = true;
                g_scrollDragOff = p.y - thumb.top;
                SetCapture(h);
                Invalidate();
                return 0;
            }
        }
        if (!In(ContentRect(), p)) return 0;
        FocusSearch(false);
        if (g_view == View::Columns) {  // clique numa coluna que não é a ativa
            auto c = ContentRect();
            for (size_t k = 0; k < g_colTrail.size(); k++) {
                float x = ColX(k);
                if (p.x < x || p.x >= x + ColLay::COL_W) continue;
                const Column& col = g_colTrail[k];
                int j = (int)((p.y - c.top - 4 + col.scroll) / ColLay::ROW_H);
                ColumnActivate(k, j >= 0 && j < (int)col.items.size() ? col.items[j].path : col.open);
                return 0;
            }
            int s = SingleSelected();
            float rx = ActiveColX() + ColLay::COL_W;
            if (s >= 0 && g_items[s].dir && p.x >= rx && p.x < rx + ColLay::COL_W) {  // coluna da direita (pasta aberta)
                int j = (int)((p.y - c.top - 4) / ColLay::ROW_H);
                std::wstring pick = j >= 0 && j < (int)ChildItems(g_items[s].path).size() ? g_childItems[j].path : L"";
                ColumnEnter(s, false);
                for (int i = 0; i < (int)g_items.size(); i++) if (!pick.empty() && SamePath(g_items[i].path, pick)) { SelectOnly(i); break; }
                Invalidate();
                return 0;
            }
        }
        int i = HitItem(p);
        g_dragStartPx = { GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        if (i < 0 && (g_view == View::Columns || g_view == View::Gallery)) {  // sem retângulo de seleção nessas visões
            if (g_view == View::Columns) { ClearSel(); g_focus = -1; }
            Invalidate();
            return 0;
        }
        KillTimer(h, T_SLOWRENAME);
        if (i >= 0) {
            g_pressWasSelected = g_items[i].sel;
            g_pressWasOnly = g_items[i].sel && SelCount() == 1;
            if (w & MK_CONTROL) { g_items[i].sel = !g_items[i].sel; g_focus = g_anchor = i; }
            else if ((w & MK_SHIFT) && g_anchor >= 0) { SelectRange(g_anchor, i); g_focus = i; }
            else if (!g_items[i].sel) SelectOnly(i);
            else g_focus = i;
            g_pressItem = i;
            g_drag = Drag::Pending;
        } else {
            g_bandMods = (UINT)w & (MK_CONTROL | MK_SHIFT);
            if (!g_bandMods) ClearSel();
            g_bandBase.clear();
            for (auto& e : g_items) g_bandBase.push_back(e.sel);
            auto c = ContentRect();
            g_bandStart = g_bandEnd = { p.x, p.y - c.top + g_scroll };
            g_drag = Drag::Band;
        }
        SetCapture(h);
        Invalidate();
        return 0;
    }
    case WM_LBUTTONUP: {
        D2D1_POINT_2F p = ToDip(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (g_pressLight >= 0) {
            int k = g_pressLight;
            g_pressLight = -1;
            ReleaseCapture();
            if (HitLight(p) == k) {
                if (k == 0) PostMessageW(h, WM_CLOSE, 0, 0);
                else if (k == 1) ShowWindow(h, SW_MINIMIZE);
                else ampliacao::Alternar(h);
            }
            return 0;
        }
        if (g_sliderDrag) { g_sliderDrag = false; ReleaseCapture(); SaveOptions(); return 0; }
        if (g_sideResize) { g_sideResize = false; ReleaseCapture(); g_sideWidthSet = true; SaveOptions(); return 0; }
        if (g_scrollDrag) { g_scrollDrag = false; ReleaseCapture(); g_scrollShownAt = GetTickCount(); SetTimer(h, T_SCROLLBAR, 50, nullptr); Invalidate(); return 0; }
        if (g_optSliderDrag) { g_optSliderDrag = false; ReleaseCapture(); return 0; }
        if (g_qlSeekDrag) { g_qlSeekDrag = false; ReleaseCapture(); return 0; }
        if (g_sidePress >= 0) {
            int row = g_sidePress;
            g_sidePress = -1;
            ReleaseCapture();
            if (g_sideDragging) {  // favorito arrastado: muda de lugar
                g_sideDragging = false;
                int to = g_sideInsert;
                g_sideInsert = -1;
                if (to >= 0 && row < (int)g_side.size()) MoveFavorite(g_side[row].fav, to);
                Invalidate();
                return 0;
            }
            if (row < (int)g_side.size() && HitSideRow(p) == row) {
                FocusSearch(false);
                if (g_side[row].tag >= 0) NavigateTag(g_side[row].tag);
                else Navigate(g_side[row].path);
            }
            return 0;
        }
        if (g_pressTool >= 0) {
            int k = g_pressTool;
            g_pressTool = -1;
            ReleaseCapture();
            Invalidate();
            if (HitTool(p) == k) ToolClick(k);
            return 0;
        }
        // clique simples num item já selecionado (sem arrastar) deixa só ele selecionado
        if (g_drag == Drag::Pending && g_pressWasSelected && !(w & (MK_CONTROL | MK_SHIFT))) SelectOnly(g_pressItem);
        // clique no nome de um item que já era o único selecionado: renomeia depois de uma pausa (um
        // segundo clique logo em seguida é duplo clique e abre o item), como no Finder
        if (g_drag == Drag::Pending && g_pressWasOnly && !(w & (MK_CONTROL | MK_SHIFT)) && g_pressItem >= 0 && g_pressItem < (int)g_items.size() &&
            In(ToView(LabelDoc(g_pressItem)), p)) {
            g_slowRenameIdx = g_pressItem;
            SetTimer(h, T_SLOWRENAME, GetDoubleClickTime() + 150, nullptr);
        }
        g_drag = Drag::None;
        KillTimer(h, T_AUTOSCROLL);
        if (GetCapture() == h) ReleaseCapture();
        Invalidate();
        return 0;
    }
    case WM_LBUTTONDBLCLK: {
        KillTimer(h, T_SLOWRENAME);
        D2D1_POINT_2F p = ToDip(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (g_ql && In(QlPanel(), p)) return 0;
        if (HitTool(p) == TB_SEARCH && SearchExpanded()) { g_search.Select(0, (int)g_search.text.size()); Invalidate(); return 0; }
        int i = HitItem(p);
        if (i >= 0) {
            g_drag = Drag::None;
            if (GetCapture() == h) ReleaseCapture();
            if ((w & MK_CONTROL) && g_items[i].dir) NewTab(g_items[i].path);  // Ctrl+duplo clique: nova aba (Cmd no Mac)
            else Open(i);
        }
        return 0;
    }
    case WM_MBUTTONUP:
        TabClick(ToDip(GET_X_LPARAM(l), GET_Y_LPARAM(l)), true);
        return 0;
    case WM_XBUTTONUP:
        if (GET_XBUTTON_WPARAM(w) == XBUTTON1) GoBack(); else GoForward();
        return TRUE;
    case WM_CONTEXTMENU: {
        POINT sp{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        bool kb = sp.x == -1 && sp.y == -1;
        if (!kb) {  // na área de arrastar da janela, fica o menu do sistema
            POINT cp = sp; ScreenToClient(h, &cp);
            D2D1_POINT_2F d = ToDip(cp.x, cp.y);
            if (d.y < Lay::TOOLBAR_H && d.x >= Lay::SIDEBAR_W) break;
        }
        if (g_renameIdx >= 0) EndRename(true);
        ShowContextMenu(sp, kb);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int delta = GET_WHEEL_DELTA_WPARAM(w);
        POINT mp{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };  // em coordenadas de tela
        ScreenToClient(h, &mp);
        if (ToDip(mp.x, mp.y).x < Lay::SIDEBAR_W) {  // sobre a barra lateral: rola só ela
            g_sideScroll = std::clamp(g_sideScroll - delta / (float)WHEEL_DELTA * 52.f, 0.f, SideMaxScroll());
            Invalidate();
            return 0;
        }
        if (g_view == View::Gallery) {  // galeria: a roda passa as miniaturas para os lados
            g_galleryX = std::clamp(g_galleryX - delta / (float)WHEEL_DELTA * 80.f, 0.f, GalleryMaxX());
            Invalidate();
            return 0;
        }
        if (g_view == View::Columns) {  // colunas da esquerda rolam sozinhas
            float x = ToDip(mp.x, mp.y).x;
            for (size_t k = 0; k < g_colTrail.size(); k++) {
                if (x < ColX(k) || x >= ColX(k) + ColLay::COL_W) continue;
                auto c = ContentRect();
                float maxS = std::max(0.f, g_colTrail[k].items.size() * ColLay::ROW_H + 8 - (c.bottom - c.top));
                g_colTrail[k].scroll = std::clamp(g_colTrail[k].scroll - delta / (float)WHEEL_DELTA * 72.f, 0.f, maxS);
                Invalidate();
                return 0;
            }
        }
        bool notched = std::abs(delta) >= WHEEL_DELTA && std::abs(delta) % WHEEL_DELTA == 0;
        ScrollBy(-delta / (float)WHEEL_DELTA * 90.f, notched);  // touchpad: segue o dedo sem animação
        if (g_drag == Drag::Band) { POINT c; GetCursorPos(&c); ScreenToClient(h, &c); UpdateBand(ToDip(c.x, c.y)); }
        return 0;
    }
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (HandleKey((UINT)w)) return 0;
        break;
    case WM_CHAR:
        if (w < 32) return 0;
        if (g_goOpen) { g_go.OnChar((wchar_t)w); GoRefresh(); Invalidate(); }
        else if (g_renameIdx >= 0) { g_rename.OnChar((wchar_t)w); Invalidate(); }
        else if (g_pickNameFocus) { g_pickName.OnChar((wchar_t)w); Invalidate(); }
        else if (g_searchFocus) { g_search.OnChar((wchar_t)w); OnSearchChanged(); Invalidate(); }
        else if (GetKeyState(VK_CONTROL) >= 0 && w != L' ') TypeAhead((wchar_t)w);  // espaço é o Quick Look
        return 0;
    case WM_TIMER:
        switch (w) {
        case T_TAHOE:  // Tema mudou nos Ajustes do Sistema: troca Sequoia/Tahoe sem reabrir
            if (tahoe::Reload()) { g_tahoe = tahoe::On(); g_dark = IsDarkMode(); ApplyWindowTheme(); shadow::Update(); Invalidate(); }
            return 0;
        case T_SLOWRENAME:
            KillTimer(h, T_SLOWRENAME);
            if (g_slowRenameIdx >= 0 && g_slowRenameIdx < (int)g_items.size() && g_items[g_slowRenameIdx].sel && SelCount() == 1 && g_renameIdx < 0 &&
                GetCapture() != h && GetForegroundWindow() == h)
                StartRename(g_slowRenameIdx);
            g_slowRenameIdx = -1;
            return 0;
        case T_SCROLL: KillTimer(h, T_SCROLL); break;  // não usado mais (a rolagem anda pelo WM_PAINT: StepScroll)
        case T_SCROLLBAR:
            if (GetTickCount() - g_scrollShownAt > 1250) KillTimer(h, T_SCROLLBAR);
            Invalidate();
            break;
        case T_AUTOSCROLL: {
            POINT c; GetCursorPos(&c); ScreenToClient(h, &c);
            D2D1_POINT_2F p = ToDip(c.x, c.y);
            auto cr = ContentRect();
            if (p.y < cr.top + 12) ScrollBy(-14, false);
            else if (p.y > cr.bottom - 12) ScrollBy(14, false);
            UpdateBand(p);
            break;
        }
        case T_RELOAD: KillTimer(h, T_RELOAD); Reload(); break;
        case T_SEARCH: RunSearch(); break;
        case T_WARMUP: KillTimer(h, T_WARMUP); PostMessageW(h, WM_APP_WARMUP, 0, 0); break;
        case T_FRENTE: {
            bool apertou = false;  // clique ou tecla do usuário: ele escolheu outra janela, não insiste
            for (int vk : { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_MENU, VK_CONTROL, VK_LWIN, VK_RWIN, VK_TAB, VK_ESCAPE })
                if (GetAsyncKeyState(vk) & 0x8000) apertou = true;
            if (apertou) g_frenteInput = 1;
            if ((int)(GetTickCount() - g_frenteAte) >= 0 || g_frenteInput || !IsWindowVisible(h)) { KillTimer(h, T_FRENTE); break; }
            HWND fg = GetForegroundWindow();
            if (fg && fg != h && GetAncestor(fg, GA_ROOTOWNER) != h) {
                DWORD ft = GetWindowThreadProcessId(fg, nullptr), me = GetCurrentThreadId();
                if (ft && ft != me) AttachThreadInput(me, ft, TRUE);
                SetWindowPos(h, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
                SetForegroundWindow(h);
                if (ft && ft != me) AttachThreadInput(me, ft, FALSE);
                if (GetForegroundWindow() != h) SwitchToThisWindow(h, TRUE);
                Log(L"frente: devolvida ao Finder (tinha ido para outra janela)");
            }
            break;
        }
        case T_MEDIA: {  // quadros do vídeo; tempo da música ~5x por segundo
            static DWORD last;
            if (!media::Active()) { KillTimer(h, T_MEDIA); break; }
            bool fresh = media::UpdateFrame();
            if (fresh || GetTickCount() - last > 200) { last = GetTickCount(); Render(); }
            break;
        }
        case T_CARET: g_caretOn = !g_caretOn; Invalidate(); break;
        case 6:  // animação do Quick Look
            if (!g_ql || (!g_qlClosing && GetTickCount() - g_qlOpenedAt > QL_ANIM_MS + 20)) KillTimer(h, 6);
            Render();
            break;
        }
        return 0;
    case WM_APP_THUMB: {
        auto* r = (thumbs::Result*)l;
        if (r->gen == g_gen && r->kind != thumbs::KIND_IMAGE) {
            g_kinds[r->key] = r->kind;  // sem miniatura: desenhamos folha ou pasta
            Invalidate();
        } else if (r->gen == g_gen) {
            if (auto* b = gfx::CreateBitmapBGRA(r->w, r->h, r->bgra.data(), r->w * 4)) {
                auto& slot = g_images[r->key];
                if (slot) slot->Release();
                else g_imgBorn[r->key] = NowMs();  // primeira vez: entra com fade (troca de tamanho não)
                slot = b;
                Invalidate();
            }
        }
        delete r;
        return 0;
    }
    case WM_APP_MEDIA:
        media::OnEvent((DWORD)w);
        Invalidate();
        return 0;
    case WM_COPYDATA: return OnLaunchRequest((const COPYDATASTRUCT*)l);
    case WM_APP_ZIPDONE: {  // Comprimir terminou (CompressItems)
        std::unique_ptr<std::wstring> z((std::wstring*)l);
        Log(L"comprimir: %ls %ls", w ? L"pronto" : L"FALHOU", z->c_str());
        if (!w) { Flash(Tr(L"Não foi possível comprimir")); return 0; }
        if (SamePath(Parent(*z), g_folder)) { g_selectAfterReload = z->substr(z->find_last_of(L'\\') + 1); Reload(); }
        return 0;
    }
    case WM_APP_WARMUP: {
        std::wstring file;
        for (auto& e : g_items) if (!e.dir) { file = e.path; break; }
        ctxmenu::WarmupAsync(g_folder.empty() ? KnownFolder(FOLDERID_Profile) : g_folder, file);  // sem travar a janela
        return 0;
    }
    case WM_APP_SEARCH:
        OnSearchBatch((int)w, (SearchBatch*)l);
        return 0;
    case WM_APP_CHANGE: {
        PIDLIST_ABSOLUTE* pidls; LONG ev;
        HANDLE lock = SHChangeNotification_Lock((HANDLE)w, (DWORD)l, &pidls, &ev);
        if (lock) SHChangeNotification_Unlock(lock);
        SetTimer(h, T_RELOAD, 150, nullptr);  // agrupa rajadas de mudanças
        return 0;
    }
    case WM_EXITSIZEMOVE:
        if (!g_reserva && !g_pick.on) SaveOptions();
        break;
    case WM_APP_INFO: GetInfo({ g_folder }); return 0;
    case WM_CLOSE:
        if (g_pick.on) { PickerCancel(); return 0; }  // bolinha vermelha no Abrir/Salvar = Cancelar
        if (!g_reserva) SaveOptions();
        if (g_finderNativo) {  // nativo: some e volta a ser a reserva (a thread dentro do explorer continua)
            ShowWindow(h, SW_HIDE);
            while (g_tabs.size() > 1) { g_tabs.pop_back(); }
            g_curTab = 0;
            g_reserva = true;
            SetWindowTextW(h, L"\u200BArquivosReserva");
            Log(L"nativo: janela fechada, de volta à reserva");
            return 0;
        }
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        shadow::Destroy();
        if (g_notifyId) SHChangeNotifyDeregister(g_notifyId);
        RevokeDragDrop(h);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// ---------------------------------------------------------------------------
// Inicialização

static std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) r = p;
    CoTaskMemFree(p);
    return r;
}

// Favoritos da barra lateral: editáveis (arrastar para adicionar ou mover, menu para remover)
// e guardados em %APPDATA%\Arquivos\favoritos.txt ("símbolo<TAB>nome<TAB>caminho" por linha).
struct Fav { std::wstring label, path; wchar_t glyph; };
static std::vector<Fav> g_favs;
static bool g_favsLoaded;

static std::wstring FavoritesFile() {
    std::wstring dir = KnownFolder(FOLDERID_RoamingAppData) + L"\\Arquivos";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\favoritos.txt";
}

static void SaveFavorites() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, FavoritesFile().c_str(), L"w, ccs=UTF-8") || !f) return;
    for (auto& v : g_favs) fwprintf(f, L"%04X\t%ls\t%ls\n", (unsigned)v.glyph, v.label.c_str(), v.path.c_str());
    fclose(f);
}

static void LoadFavorites() {
    if (g_favsLoaded) return;
    g_favsLoaded = true;
    FILE* f = nullptr;
    if (!_wfopen_s(&f, FavoritesFile().c_str(), L"r, ccs=UTF-8") && f) {
        wchar_t line[2048];
        while (fgetws(line, 2048, f)) {
            std::wstring s = line;
            while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
            size_t a = s.find(L'\t'), b = a == std::wstring::npos ? a : s.find(L'\t', a + 1);
            if (b == std::wstring::npos) continue;
            g_favs.push_back({ s.substr(a + 1, b - a - 1), s.substr(b + 1), (wchar_t)wcstoul(s.substr(0, a).c_str(), nullptr, 16) });
        }
        fclose(f);
        if (!g_favs.empty()) return;
    }
    // padrão: os mesmos do Finder, com as pastas do Windows
    struct KF { const KNOWNFOLDERID* id; const wchar_t* label; wchar_t glyph; };
    const KF defs[] = {
        { &FOLDERID_Recent, Tr(L"Recentes"), 0xE823 },
        { &FOLDERID_CommonPrograms, Tr(L"Aplicativos"), 0xE71D },  // atalhos do menu Iniciar
        { &FOLDERID_Desktop, Tr(L"Mesa"), 0xE7F4 }, { &FOLDERID_Documents, Tr(L"Documentos"), 0xE8A5 },
        { &FOLDERID_Downloads, Tr(L"Downloads"), 0xE896 }, { &FOLDERID_Pictures, Tr(L"Imagens"), 0xE91B },
        { &FOLDERID_Music, Tr(L"Músicas"), 0xE8D6 }, { &FOLDERID_Videos, Tr(L"Vídeos"), 0xE714 },
    };
    for (auto& d : defs) {
        std::wstring p = KnownFolder(*d.id);
        if (!p.empty()) g_favs.push_back({ d.label, p, d.glyph });
    }
    std::wstring home = KnownFolder(FOLDERID_Profile);
    if (!home.empty()) g_favs.push_back({ home.substr(home.find_last_of(L'\\') + 1), home, 0xE80F });
}

static void BuildSidebar() {
    // guarda quais seções estavam recolhidas
    std::vector<std::wstring> collapsed;
    for (auto& s : g_side) if (s.header && s.collapsed) collapsed.push_back(s.label);
    LoadFavorites();
    g_side.clear();
    g_emblems.clear();
    g_recentFolder = KnownFolder(FOLDERID_Recent);
    // símbolos das pastas especiais (gravados no ícone delas), mesmo fora dos favoritos
    struct KF { const KNOWNFOLDERID* id; wchar_t glyph; };
    const KF special[] = { { &FOLDERID_Recent, 0xE823 }, { &FOLDERID_CommonPrograms, 0xE71D }, { &FOLDERID_Desktop, 0xE7F4 },
                           { &FOLDERID_Documents, 0xE8A5 }, { &FOLDERID_Downloads, 0xE896 }, { &FOLDERID_Pictures, 0xE91B },
                           { &FOLDERID_Music, 0xE8D6 }, { &FOLDERID_Videos, 0xE714 }, { &FOLDERID_Profile, 0xE80F }, { &FOLDERID_SkyDrive, 0xE753 } };
    for (auto& k : special) { std::wstring p = KnownFolder(*k.id); if (!p.empty()) g_emblems.push_back({ p, k.glyph }); }

    const bool leoSide = !g_tahoe && tahoe::Leopard();
    auto devices = [&](const std::wstring& title) {
        g_side.push_back({ title, L"", 0, true });
        { SideEntry pc{ Tr(L"Este Computador"), L"", 0xE7F4, false }; pc.tag = VIEW_COMPUTER; g_side.push_back(pc); }
        DWORD drives = GetLogicalDrives();
        for (int d = 0; d < 26; d++) {
            if (!(drives & (1u << d))) continue;
            std::wstring root = std::wstring(1, (wchar_t)(L'A' + d)) + L":\\";
            UINT type = GetDriveTypeW(root.c_str());
            if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE && type != DRIVE_REMOTE) continue;
            SHFILEINFOW sfi{};
            std::wstring label = SHGetFileInfoW(root.c_str(), 0, &sfi, sizeof(sfi), SHGFI_DISPLAYNAME) ? sfi.szDisplayName : root;
            g_side.push_back({ label, root, (wchar_t)(type == DRIVE_REMOVABLE ? 0xE88E : 0xEDA2), false });
        }
    };
    if (leoSide) {
        // Leopard (como na foto de referência do usuário): FAVORITOS, DISPOSITIVOS, COMPARTILHADO e BUSCAR POR (o do
        // Finder do 10.5), sem Etiquetas
        g_side.push_back({ Tr(L"Favoritos"), L"", 0, true });
        for (int i = 0; i < (int)g_favs.size(); i++) {
            SideEntry e{ g_favs[i].label, g_favs[i].path, g_favs[i].glyph, false };
            e.fav = i;
            g_side.push_back(e);
        }
        devices(Tr(L"Dispositivos"));
        std::wstring cl = KnownFolder(FOLDERID_SkyDrive);
        if (!cl.empty() && GetFileAttributesW(cl.c_str()) != INVALID_FILE_ATTRIBUTES) {
            g_side.push_back({ Tr(L"Compartilhado"), L"", 0, true });
            g_side.push_back({ L"OneDrive", cl, 0xE753, false });
        }
        g_side.push_back({ Tr(L"Buscar Por"), L"", 0, true });
        const wchar_t smartGlyph[6] = { 0xE823, 0xE823, 0xE823, 0xE91B, 0xE714, 0xE8A5 };  // relógio ×3, imagem, filme, documento
        for (int k = 0; k < 6; k++) { SideEntry e{ Tr(SMART_NAMES[k]), L"", smartGlyph[k], false }; e.tag = VIEW_SMART + k; g_side.push_back(e); }
    } else {
    g_side.push_back({ Tr(L"Favoritos"), L"", 0, true });
    for (int i = 0; i < (int)g_favs.size(); i++) {
        SideEntry e{ g_favs[i].label, g_favs[i].path, g_favs[i].glyph, false };
        e.fav = i;
        g_side.push_back(e);
    }

    // OneDrive faz o papel do iCloud Drive
    std::wstring cloud = KnownFolder(FOLDERID_SkyDrive);
    if (!cloud.empty() && GetFileAttributesW(cloud.c_str()) != INVALID_FILE_ATTRIBUTES) {
        g_side.push_back({ L"OneDrive", L"", 0, true });
        g_side.push_back({ L"OneDrive", cloud, 0xE753, false });
    }

    g_side.push_back({ Tr(L"Locais"), L"", 0, true });
    { SideEntry pc{ Tr(L"Este Computador"), L"", 0xE7F4, false }; pc.tag = VIEW_COMPUTER; g_side.push_back(pc); }
    DWORD drives = GetLogicalDrives();
    for (int d = 0; d < 26; d++) {
        if (!(drives & (1u << d))) continue;
        std::wstring root = std::wstring(1, (wchar_t)(L'A' + d)) + L":\\";
        UINT type = GetDriveTypeW(root.c_str());
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE && type != DRIVE_REMOTE) continue;
        SHFILEINFOW sfi{};
        std::wstring label = SHGetFileInfoW(root.c_str(), 0, &sfi, sizeof(sfi), SHGFI_DISPLAYNAME) ? sfi.szDisplayName : root;
        g_side.push_back({ label, root, (wchar_t)(type == DRIVE_REMOVABLE ? 0xE88E : 0xEDA2), false });
    }

    g_side.push_back({ Tr(L"Etiquetas"), L"", 0, true });
    for (int t = 0; t < tags::COUNT; t++) {
        SideEntry e{ Tr(tags::names[t]), L"", 0, false };
        e.tagColor = tags::colors[t];
        e.tag = t;
        g_side.push_back(e);
    }
    }  // (não Leopard)

    for (int k = 0; k < (int)g_side.size(); k++)
        if (g_side[k].header && std::find(collapsed.begin(), collapsed.end(), g_side[k].label) != collapsed.end()) ToggleSection(k);
    g_sideScroll = std::min(g_sideScroll, SideMaxScroll());
    Invalidate();
    if (!g_sideWidthSet && g_tfSide) {  // largura que cabe o nome mais longo ("Este Computador", "SSD Externo...")
        float need = Lay::SIDEBAR_W;
        for (auto& e : g_side) {
            if (e.header || e.label.empty()) continue;
            IDWriteTextLayout* tl = nullptr;
            if (SUCCEEDED(gfx::dw->CreateTextLayout(e.label.c_str(), (UINT32)e.label.size(), g_tfSide, 1000, 40, &tl))) {
                DWRITE_TEXT_METRICS m{}; tl->GetMetrics(&m); tl->Release();
                need = std::max(need, std::ceil(40 + m.width + 16));  // texto começa em 40; 16 de folga à direita
            }
        }
        // Tahoe: a barra lateral do Finder do macOS 26 tem 201 de largura (+ 8 de recuo + 1 de borda)
        Lay::SIDEBAR_W = std::clamp(g_tahoe ? std::max(need, 210.f) : need, 142.f, 220.f);
    }
}

static void AddFavorite(const std::wstring& path, int at) {
    DWORD a = GetFileAttributesW(path.c_str());
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) return;  // só pastas, como no Finder
    for (int i = 0; i < (int)g_favs.size(); i++)
        if (SamePath(g_favs[i].path, path)) {  // já está: só muda de lugar
            Fav v = g_favs[i];
            g_favs.erase(g_favs.begin() + i);
            if (at > i) at--;
            g_favs.insert(g_favs.begin() + std::clamp(at, 0, (int)g_favs.size()), v);
            SaveFavorites();
            BuildSidebar();
            return;
        }
    wchar_t glyph = 0xE8B7;  // pasta comum
    for (auto& [p, g] : g_emblems) if (SamePath(p, path)) glyph = g;
    std::wstring label = path.size() <= 3 ? DisplayName(path) : path.substr(path.find_last_of(L'\\') + 1);
    g_favs.insert(g_favs.begin() + std::clamp(at, 0, (int)g_favs.size()), Fav{ label, path, glyph });
    SaveFavorites();
    BuildSidebar();
}

static void RemoveFavorite(int fav) {
    if (fav < 0 || fav >= (int)g_favs.size()) return;
    g_favs.erase(g_favs.begin() + fav);
    SaveFavorites();
    BuildSidebar();
}

static void MoveFavorite(int from, int to) {  // `to` = posição de inserção antes de remover
    if (from < 0 || from >= (int)g_favs.size() || to == from || to == from + 1) return;
    Fav v = g_favs[from];
    g_favs.erase(g_favs.begin() + from);
    if (to > from) to--;
    g_favs.insert(g_favs.begin() + std::clamp(to, 0, (int)g_favs.size()), v);
    SaveFavorites();
    BuildSidebar();
}

static IDWriteTextFormat* Format(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight,
                                 DWRITE_TEXT_ALIGNMENT align, DWRITE_PARAGRAPH_ALIGNMENT para, bool wrap) {
    IDWriteTextFormat* tf = nullptr;
    gfx::dw->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"pt-br", &tf);
    tf->SetTextAlignment(align);
    tf->SetParagraphAlignment(para);
    tf->SetWordWrapping(wrap ? (DWRITE_WORD_WRAPPING)2 /*EMERGENCY_BREAK*/ : DWRITE_WORD_WRAPPING_NO_WRAP);
    IDWriteInlineObject* ell = nullptr;
    gfx::dw->CreateEllipsisTrimmingSign(tf, &ell);
    DWRITE_TRIMMING trim{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
    tf->SetTrimming(&trim, ell);
    if (ell) ell->Release();
    return tf;
}

static std::wstring PickFamily(std::initializer_list<const wchar_t*> names) {
    IDWriteFontCollection* fc = nullptr;
    gfx::dw->GetSystemFontCollection(&fc);
    std::wstring r = L"Segoe UI";
    for (auto n : names) {
        UINT32 idx; BOOL ok = FALSE;
        fc->FindFamilyName(n, &idx, &ok);
        if (ok) { r = n; break; }
    }
    fc->Release();
    return r;
}

static DWRITE_FONT_WEIGHT g_bodyWeight = DWRITE_FONT_WEIGHT_MEDIUM;

// Corpo do texto (nomes nos ícones, lista e colunas), no tamanho escolhido nas Opções.
static void MakeBodyFormats() {
    for (auto** tf : { &g_tfLabel, &g_tfRow, &g_tfRowR }) if (*tf) { (*tf)->Release(); *tf = nullptr; }
    g_tfLabel = Format(g_uiFamily.c_str(), g_labelSize, g_bodyWeight, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_NEAR, true);
    g_tfRow = Format(g_uiFamily.c_str(), g_rowTextSize, g_bodyWeight, DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    g_tfRowR = Format(g_uiFamily.c_str(), g_rowTextSize, g_bodyWeight, DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    if (g_hwnd) Invalidate();
}

static void CreateFormats() {
    // A fonte do macOS (SF Pro) é usada só se estiver instalada neste PC: a licença da Apple não
    // permite distribuí-la junto com o app. Sem ela, a Inter é a alternativa livre mais próxima.
    std::wstring ui = PickFamily({ L"SF Pro Text", L"Inter", L"Segoe UI Variable Text", L"Segoe UI" });
    g_uiFamily = ui;
    // Peso do corpo do texto. Com a SF Pro, Regular fica igual ao Mac (comparado com a referência);
    // a Inter Regular parece fina perto do macOS, então ela vai em Medium.
    DWRITE_FONT_WEIGHT body = ui == L"SF Pro Text" ? DWRITE_FONT_WEIGHT_NORMAL : DWRITE_FONT_WEIGHT_MEDIUM;
    std::wstring icons = PickFamily({ L"Segoe Fluent Icons", L"Segoe MDL2 Assets" });
    g_iconFamily = icons;
    const auto L = DWRITE_TEXT_ALIGNMENT_LEADING, C = DWRITE_TEXT_ALIGNMENT_CENTER;
    const auto N = DWRITE_PARAGRAPH_ALIGNMENT_NEAR, M = DWRITE_PARAGRAPH_ALIGNMENT_CENTER;
    g_bodyWeight = body;
    g_tfSide = Format(ui.c_str(), 13, body, L, M, false);
    g_tfSideHdr = Format(ui.c_str(), 11, DWRITE_FONT_WEIGHT_SEMI_BOLD, L, N, false);
    g_tfTitle = Format(ui.c_str(), 15, DWRITE_FONT_WEIGHT_SEMI_BOLD, L, M, false);
    g_tfStatus = Format(ui.c_str(), 11, DWRITE_FONT_WEIGHT_NORMAL, C, M, false);
    g_tfPath = Format(ui.c_str(), 11.5f, DWRITE_FONT_WEIGHT_NORMAL, L, M, false);
    MakeBodyFormats();
    const auto R = DWRITE_TEXT_ALIGNMENT_TRAILING;
    g_tfHeader = Format(ui.c_str(), 11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, L, M, false);
    g_tfHeaderL = Format(ui.c_str(), 11.5f, DWRITE_FONT_WEIGHT_NORMAL, L, M, false);  // demais colunas: normais, como no Finder
    g_tfHeaderR = Format(ui.c_str(), 11.5f, DWRITE_FONT_WEIGHT_NORMAL, R, M, false);
    g_tfGroup = Format(ui.c_str(), 12, DWRITE_FONT_WEIGHT_SEMI_BOLD, L, N, false);
    g_tfPreviewName = Format(ui.c_str(), 14, DWRITE_FONT_WEIGHT_SEMI_BOLD, C, N, true);
    g_tfPreviewInfo = Format(ui.c_str(), 11.5f, DWRITE_FONT_WEIGHT_NORMAL, C, M, false);
    g_tfQlTitle = Format(ui.c_str(), 13, DWRITE_FONT_WEIGHT_SEMI_BOLD, C, M, false);
    g_tfQlButton = Format(ui.c_str(), 12, DWRITE_FONT_WEIGHT_NORMAL, C, M, false);
    g_tfQlGlyph = Format(icons.c_str(), 15, DWRITE_FONT_WEIGHT_NORMAL, C, M, false);
    g_tfQlTime = Format(ui.c_str(), 11, DWRITE_FONT_WEIGHT_NORMAL, C, M, false);
    g_tfQlArtist = Format(ui.c_str(), 13, DWRITE_FONT_WEIGHT_NORMAL, C, M, false);
    std::wstring mono = PickFamily({ L"SF Mono", L"Cascadia Mono", L"Consolas" });
    g_tfMono = Format(mono.c_str(), 12, DWRITE_FONT_WEIGHT_NORMAL, L, N, false);
    g_tfSearch = Format(ui.c_str(), 13, DWRITE_FONT_WEIGHT_NORMAL, L, M, false);
    g_tfRename = Format(ui.c_str(), 12, DWRITE_FONT_WEIGHT_NORMAL, L, M, false);
    g_tfGlyph = Format(icons.c_str(), 14, DWRITE_FONT_WEIGHT_NORMAL, C, M, false);
    DWRITE_TRIMMING none{ DWRITE_TRIMMING_GRANULARITY_NONE, 0, 0 };  // campos de texto rolam; sem reticências
    g_tfSearch->SetTrimming(&none, nullptr);
    g_tfRename->SetTrimming(&none, nullptr);
    Log(L"fonte: %ls, ícones: %ls", ui.c_str(), icons.c_str());
}


// ---------------------------------------------------------------------------
// Inicialização: ler o pedido, posicionar a janela, aplicar. A mesma lógica serve para um Finder
// novo e para a reserva (um Finder já pronto e escondido, mantido pelo Shell) que recebe o pedido
// por WM_COPYDATA: assim a janela aparece na hora.

const wchar_t* RESERVA_TITLE = L"\u200BArquivosReserva";
const ULONG_PTR COPY_LAUNCH = 0x41525156;  // 'ARQV'
// g_reserva: declarado no topo

struct Launch {
    std::wstring start, searchArg, qlArg, selectArg;
    int startTag = -1, searchScope = 0;
    std::vector<std::wstring> extraTabs;
    bool spotlight = false, reserva = false, info = false;  // info: abre também o Obter Informações da pasta
    bool abrir = false;  // --abrir: veio do "abrir" do Windows (duplo clique, ShellExecute): arquivo é para ABRIR, não selecionar
};

static void PickerRecall(Launch& L);
static void ParseLaunch(int argc, wchar_t** argv, Launch& L) {
    for (int a = 1; a < argc; a++) {
        if (!_wcsicmp(argv[a], L"--reserva")) { L.reserva = true; continue; }
        if (!_wcsicmp(argv[a], L"--abrir")) { L.abrir = true; continue; }
        if (!_wcsicmp(argv[a], L"--etiqueta") && a + 1 < argc) { L.startTag = _wtoi(argv[++a]) % tags::COUNT; continue; }
        if (!_wcsicmp(argv[a], L"--lista")) g_view = View::List;
        else if (!_wcsicmp(argv[a], L"--colunas")) g_view = View::Columns;
        else if (!_wcsicmp(argv[a], L"--galeria")) g_view = View::Gallery;
        else if (!_wcsicmp(argv[a], L"--grupos")) g_groupBy = GROUP_KIND;
        else if (!_wcsicmp(argv[a], L"--opcoes")) g_viewOpts = true;  // abre as Opções de Visualização
        else if (!_wcsicmp(argv[a], L"--info")) L.info = true;  // Obter Informações da pasta aberta
        else if (!_wcsicmp(argv[a], L"--aba") && a + 1 < argc) L.extraTabs.push_back(argv[++a]);  // abre outra aba
        else if (!_wcsicmp(argv[a], L"--ql") && a + 1 < argc) L.qlArg = argv[++a];  // abre o Quick Look de um arquivo
        else if (!_wcsicmp(argv[a], L"--buscar") && a + 1 < argc) L.searchArg = argv[++a];  // busca no computador
        else if (!_wcsicmp(argv[a], L"--buscar-aqui") && a + 1 < argc) { L.searchArg = argv[++a]; L.searchScope = 1; }  // busca na pasta
        else if (!_wcsicmp(argv[a], L"--spotlight")) L.spotlight = true;  // abre com a busca em foco (Spotlight da barra de menus)
        // modo escolher (janela de Abrir/Salvar)
        else if (!_wcsicmp(argv[a], L"--escolher") && a + 1 < argc) {
            g_pick.on = true; g_exitCode = 1;  // fechar a janela = cancelar
            std::wstring m = argv[++a];
            g_pick.mode = m == L"salvar" ? 1 : m == L"pasta" ? 2 : 0;
        }
        else if (!_wcsicmp(argv[a], L"--multi")) g_pick.multi = true;
        else if (!_wcsicmp(argv[a], L"--sem-confirmar")) g_pick.overwrite = false;
        else if (!_wcsicmp(argv[a], L"--tipos") && a + 1 < argc) {  // "Texto|*.txt||Todos|*.*"
            std::wstring t = argv[++a];
            size_t i = 0;
            while (i < t.size()) {
                size_t j = t.find(L"||", i);
                std::wstring one = t.substr(i, j == std::wstring::npos ? std::wstring::npos : j - i);
                size_t bar = one.find(L'|');
                if (bar != std::wstring::npos) g_pick.types.push_back({ one.substr(0, bar), one.substr(bar + 1) });
                if (j == std::wstring::npos) break;
                i = j + 2;
            }
        }
        else if (!_wcsicmp(argv[a], L"--tipo") && a + 1 < argc) g_pick.type = _wtoi(argv[++a]);
        else if (!_wcsicmp(argv[a], L"--nome") && a + 1 < argc) g_pickName.SetText(argv[++a], true);
        else if (!_wcsicmp(argv[a], L"--ok") && a + 1 < argc) g_pick.ok = argv[++a];
        else if (!_wcsicmp(argv[a], L"--ext") && a + 1 < argc) g_pick.ext = argv[++a];
        else if (!_wcsicmp(argv[a], L"--resultado") && a + 1 < argc) g_pick.result = argv[++a];
        else if (!_wcsicmp(argv[a], L"--pai") && a + 1 < argc) g_pick.owner = (HWND)(ULONG_PTR)_wcstoui64(argv[++a], nullptr, 10);
        else if (argv[a][0] != L'-' || argv[a][1] != L'-') L.start = argv[a];
    }
}

// modo escolher: abre na última pasta usada por este programa e na visão que o Abrir/Salvar usava
static void PickerRecall(Launch& L) {
    if (!g_pick.on) return;
    if (g_pickView >= 0) g_view = (View)g_pickView;
    std::wstring exe = PickerOwnerExe();
    if (exe.empty()) return;
    for (auto& [k, v] : PickerMemory()) {
        if (k != exe) continue;
        DWORD a = GetFileAttributesW(v.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) { L.start = v; Log(L"escolher: volta para %ls (%ls)", v.c_str(), exe.c_str()); }
    }
}

// false: não é pasta nossa (pasta especial etc.) e foi repassado ao Explorador.
static bool ResolveStart(Launch& L) {
    std::wstring& start = L.start;
    // "D:\" vem como D:" (a barra final escapa a aspa na linha de comando)
    while (!start.empty() && start.back() == L'"') start.pop_back();
    for (auto pc : { L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}", L"shell:::{20D04FE0-3AEA-1069-A2D8-08002B30309D}", L"shell:MyComputerFolder", L"shell:ThisPCDesktopFolder" })
        if (!_wcsicmp(start.c_str(), pc)) { L.startTag = VIEW_COMPUTER; start.clear(); }
    for (auto tr : { L"shell:RecycleBinFolder", L"::{645FF040-5081-101B-9F08-00AA002F954E}", L"shell:::{645FF040-5081-101B-9F08-00AA002F954E}", L"--lixeira" })
        if (!_wcsicmp(start.c_str(), tr)) { L.startTag = VIEW_TRASH; start.clear(); }
    if (start.rfind(L"shell:", 0) == 0) {  // shell:Downloads, shell:Personal... viram a pasta de verdade
        PIDLIST_ABSOLUTE pidl = nullptr;
        wchar_t real[MAX_PATH];
        if (SUCCEEDED(SHParseDisplayName(start.c_str(), nullptr, &pidl, 0, nullptr)) && SHGetPathFromIDListW(pidl, real)) start = real;
        if (pidl) CoTaskMemFree(pidl);
    }
    if (start.size() == 2 && start[1] == L':') start += L"\\";
    if (start.empty()) start = KnownFolder(FOLDERID_Profile);
    DWORD at = GetFileAttributesW(start.c_str());
    if (L.abrir && at != INVALID_FILE_ATTRIBUTES && !(at & FILE_ATTRIBUTE_DIRECTORY)) {
        // "abrir" um arquivo que o Windows trata como pasta. .zip: como no Mac, descompacta numa pasta ao lado
        // (mesmo nome; "nome 2" se já existir) e abre ela. Outros (.cab, .iso...): o Explorador sabe abrir.
        std::wstring low = start; for (auto& c : low) c = towlower(c);
        if (low.size() > 4 && !low.compare(low.size() - 4, 4, L".zip")) {
            std::wstring base = start.substr(0, start.size() - 4), dest = base;
            for (int n = 2; GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES && n < 100; n++) dest = base + L" " + std::to_wstring(n);
            CreateDirectoryW(dest.c_str(), nullptr);
            wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
            std::wstring cmd = L"\"" + std::wstring(sys) + L"\\tar.exe\" -xf \"" + start + L"\" -C \"" + dest + L"\"";
            STARTUPINFOW si{ sizeof(si) }; PROCESS_INFORMATION pi{};
            DWORD code = 1;
            if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                WaitForSingleObject(pi.hProcess, 10 * 60 * 1000);
                GetExitCodeProcess(pi.hProcess, &code);
                CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            }
            Log(L"zip: %ls -> %ls (%lu)", start.c_str(), dest.c_str(), code);
            if (code == 0) { start = dest; at = FILE_ATTRIBUTE_DIRECTORY; }
            else {  // não deu (zip com senha, formato estranho): o Explorador abre
                RemoveDirectoryW(dest.c_str());
                ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"\"" + start + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
                return false;
            }
        } else {
            ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"\"" + start + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
            return false;
        }
    }
    if (at != INVALID_FILE_ATTRIBUTES && !(at & FILE_ATTRIBUTE_DIRECTORY)) {  // arquivo: abre a pasta com ele selecionado
        size_t sl = start.find_last_of(L'\\');
        L.selectArg = start.substr(sl + 1);
        start = start.substr(0, sl);
        if (start.size() == 2) start += L"\\";
        at = FILE_ATTRIBUTE_DIRECTORY;
    }
    if (at == INVALID_FILE_ATTRIBUTES || !(at & FILE_ATTRIBUTE_DIRECTORY)) {  // pastas especiais ficam com o Explorador
        Log(L"repassado ao Explorador: %ls", start.c_str());
        ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"\"" + start + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        return false;
    }
    return true;
}

// Tamanho e posição: no monitor do cursor; janela de Abrir/Salvar menor, sobre quem pediu.
static void Arrange(const Launch& L) {
    POINT cur; GetCursorPos(&cur);
    HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST);
    UINT dx = 96, dy = 96; GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    float sc = dx / 96.f;
    MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(mon, &mi);
    int w = (int)(1060 * sc), h = (int)(680 * sc);
    int x = (mi.rcWork.left + mi.rcWork.right - w) / 2, y = (mi.rcWork.top + mi.rcWork.bottom - h) / 2;
    if (g_pick.on) {
        Lay::STATUS_H = 56; Lay::FOOTER_H = Lay::PATH_H + 56; g_pathBarUser = g_pathBar; g_pathBar = true;
        w = (int)(900 * sc); h = (int)(580 * sc);
        RECT orc;
        if (g_pick.owner && GetWindowRect(g_pick.owner, &orc)) {
            x = (orc.left + orc.right - w) / 2; y = std::max<LONG>(mi.rcWork.top, (orc.top + orc.bottom - h) / 2);
        }
        RECT pr = g_pickWin, inter; MONITORINFO pmi{ sizeof(pmi) };
        HMONITOR pm = pr.right - pr.left > 300 ? MonitorFromRect(&pr, MONITOR_DEFAULTTONULL) : nullptr;
        if (pm && GetMonitorInfoW(pm, &pmi) && IntersectRect(&inter, &pr, &pmi.rcWork)) {
            w = pr.right - pr.left; h = pr.bottom - pr.top;
            x = pr.left; y = pr.top;
        }
    }
    if (!g_pick.on && g_savedWin.right - g_savedWin.left > 200 && g_savedWin.bottom - g_savedWin.top > 150) {
        RECT r = g_savedWin, inter;
        HMONITOR sm = MonitorFromRect(&r, MONITOR_DEFAULTTONULL);
        MONITORINFO smi{ sizeof(smi) };
        if (sm && GetMonitorInfoW(sm, &smi) && IntersectRect(&inter, &r, &smi.rcWork)) {
            x = r.left; y = r.top; w = r.right - r.left; h = r.bottom - r.top;
            int step = (int)(22 * sc);
            for (int tries = 0; tries < 10; tries++) {  // outra janela do Finder já está aí: desce 22 pt
                bool taken = false;
                for (HWND o = FindWindowW(L"ArquivosJanela", nullptr); o; o = FindWindowExW(nullptr, o, L"ArquivosJanela", nullptr)) {
                    if (o == g_hwnd || !IsWindowVisible(o)) continue;
                    RECT orc; GetWindowRect(o, &orc);
                    if (std::abs(orc.left - x) < 4 && std::abs(orc.top - y) < 4) taken = true;
                }
                if (!taken) break;
                x += step; y += step;
            }
        }
    }
    SetWindowPos(g_hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
}

static void ApplyLaunch(const Launch& L, int show) {
    Navigate(L.start, false, L.selectArg);
    if (L.startTag >= 0) NavigateTag(L.startTag);
    for (auto& t : L.extraTabs) NewTab(t);
    if (L.spotlight) FocusSearch(true);
    if (L.info) PostMessageW(g_hwnd, WM_APP_INFO, 0, 0);  // depois que a janela do Finder aparecer
    if (g_pick.on) {
        if (g_pick.owner) SetWindowLongPtrW(g_hwnd, GWLP_HWNDPARENT, (LONG_PTR)g_pick.owner);  // fica sempre na frente do programa
        if (g_pick.mode == 1) { g_pickNameFocus = true; g_caretOn = true; SetTimer(g_hwnd, T_CARET, 530, nullptr); }
        Reload();  // aplica o filtro de tipo
    }
    if (!L.qlArg.empty()) {
        size_t sl = L.qlArg.find_last_of(L'\\');
        Navigate(L.qlArg.substr(0, sl), false, L.qlArg.substr(sl + 1));
        QlToggle();
    }
    if (!L.searchArg.empty()) { g_searchScope = L.searchScope; g_search.SetText(L.searchArg, false); OnSearchChanged(); RunSearch(); }
    // a reserva já tinha desenhado a pasta dela escondida: desenha a pasta pedida ANTES de aparecer (senão
    // o primeiro quadro na tela era a pasta velha, ex.: "User", e só depois a certa)
    Render();
    ShowWindow(g_hwnd, show);
    // sempre na frente e com o teclado (quem pediu está esperando)
    HWND fg = GetForegroundWindow();
    DWORD ft = fg ? GetWindowThreadProcessId(fg, nullptr) : 0, me = GetCurrentThreadId();
    if (ft && ft != me) AttachThreadInput(me, ft, TRUE);
    SetWindowPos(g_hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(g_hwnd);
    SetActiveWindow(g_hwnd);
    SetFocus(g_hwnd);
    if (ft && ft != me) AttachThreadInput(me, ft, FALSE);
    if (GetForegroundWindow() != g_hwnd) SwitchToThisWindow(g_hwnd, TRUE);
    UpdateWindow(g_hwnd);
    shadow::Update();
    ampliacao::Revelar(g_hwnd);  // já desenhada (Render acima): o Shell anima agora, sem esperar a 1ª pintura (~0,1 s)
    if (!g_pick.on) {  // janela de Abrir/Salvar fica presa ao dono; as outras se defendem por 2 s
        g_frenteInput = 0; g_frenteAte = GetTickCount() + 3000;
        SetTimer(g_hwnd, T_FRENTE, 100, nullptr);
    }
}

// A reserva recebe um pedido: vira um Finder de verdade com aquela linha de comando.
static LRESULT OnLaunchRequest(const COPYDATASTRUCT* cd) {
    if (cd->dwData != COPY_LAUNCH) return 0;
    if (g_finderNativo && wcsstr((const wchar_t*)cd->lpData, L"--escolher")) return 0;  // Abrir/Salvar: o exe (ou o MacDialogo) atende
    if (!g_reserva && g_finderNativo) {  // já aberto: a pasta pedida entra numa aba nova (como "abrir pastas em abas")
        std::wstring cmd((const wchar_t*)cd->lpData, cd->cbData / sizeof(wchar_t));
        int argc; wchar_t** argv = CommandLineToArgvW(cmd.c_str(), &argc);
        Launch L;
        ParseLaunch(argc, argv, L);
        LocalFree(argv);
        if (!ResolveStart(L)) return GetCurrentProcessId();
        NewTab(L.start);
        if (!L.selectArg.empty()) Navigate(L.start, false, L.selectArg);
        if (L.startTag >= 0) NavigateTag(L.startTag);  // Este Computador, Lixeira, etiqueta
        if (L.spotlight) FocusSearch(true);  // Buscar (Ctrl+F/Win+F do Windows): a busca já em foco
        if (IsIconic(g_hwnd)) ShowWindow(g_hwnd, SW_RESTORE);
        SetForegroundWindow(g_hwnd);
        Log(L"nativo: pedido numa aba nova: %ls", cmd.c_str());
        return GetCurrentProcessId();
    }
    if (!g_reserva) return 0;
    std::wstring cmd((const wchar_t*)cd->lpData, cd->cbData / sizeof(wchar_t));
    int argc; wchar_t** argv = CommandLineToArgvW(cmd.c_str(), &argc);
    LoadOptions();  // opções atuais (visão, ícones, barra lateral, janela) antes dos argumentos do pedido
    Lay::FOOTER_H = Lay::STATUS_H + (g_pathBar ? Lay::PATH_H : 0);
    Launch L;
    ParseLaunch(argc, argv, L);
    LocalFree(argv);
    PickerRecall(L);
    g_reserva = false;
    SetWindowTextW(g_hwnd, L"Finder");
    if (!ResolveStart(L)) { PostMessageW(g_hwnd, WM_CLOSE, 0, 0); return GetCurrentProcessId(); }
    LARGE_INTEGER rf, ra, rb; QueryPerformanceFrequency(&rf); QueryPerformanceCounter(&ra);
    Arrange(L);
    ApplyLaunch(L, SW_SHOWNORMAL);
    QueryPerformanceCounter(&rb);
    Log(L"reserva usada: %ls", cmd.c_str());
    Log(L"abertura (reserva): janela na tela em %d ms depois do pedido", (int)((rb.QuadPart - ra.QuadPart) * 1000 / rf.QuadPart));
    return GetCurrentProcessId();
}

// Antes de tudo: se há uma reserva pronta, entrega o pedido a ela e (no modo escolher) espera ela
// terminar para devolver o mesmo código de saída. Custa só abrir e fechar este processo.
static int ForwardToReserve() {
    std::wstring cmd = GetCommandLineW();
    if (cmd.find(L"--reserva") != std::wstring::npos || cmd.find(L"--marcar") != std::wstring::npos || cmd.find(L"--padrao") != std::wstring::npos ||
        cmd.find(L"--info-admin") != std::wstring::npos) return -1;  // administrador: nunca a reserva (ela não é elevada)
    HWND spare = FindWindowW(L"ArquivosJanela", RESERVA_TITLE);
    if (!spare) return -1;
    DWORD pid = 0; GetWindowThreadProcessId(spare, &pid);
    AllowSetForegroundWindow(pid);  // fomos abertos pelo usuário: passamos a vez para a reserva
    HANDLE hp = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    COPYDATASTRUCT cd{ COPY_LAUNCH, (DWORD)((cmd.size() + 1) * sizeof(wchar_t)), (void*)cmd.c_str() };
    DWORD_PTR r = 0;
    if (!SendMessageTimeoutW(spare, WM_COPYDATA, 0, (LPARAM)&cd, SMTO_ABORTIFHUNG, 3000, &r) || !r) { if (hp) CloseHandle(hp); return -1; }
    if (cmd.find(L"--escolher") == std::wstring::npos || !hp) { if (hp) CloseHandle(hp); return 0; }
    WaitForSingleObject(hp, INFINITE);  // a janela de Abrir/Salvar: o resultado é o código de saída
    DWORD code = 1; GetExitCodeProcess(hp, &code);
    CloseHandle(hp);
    return (int)code;
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) {
    g_inst = hi;
    if (int fw = ForwardToReserve(); fw >= 0) return fw;  // a reserva assumiu
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    OleInitialize(nullptr);

    wchar_t exe[MAX_PATH]; ExePath(exe, MAX_PATH);
    PathRemoveFileSpecW(exe);
    g_log = _wfsopen((std::wstring(exe) + L"\\arquivos.log").c_str(), L"a, ccs=UTF-8", _SH_DENYNO);
    SYSTEMTIME t; GetLocalTime(&t);
    Log(L"\n===== %04d-%02d-%02d %02d:%02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);

    ctxmenu::Init(hi, g_log);
    LoadOptions();
    g_dark = IsDarkMode();

    int argc; wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int a = 1; a < argc; a++) {  // Obter Informações como administrador (cadeado das permissões): só essa janela
        if (_wcsicmp(argv[a], L"--info-admin") || a + 1 >= argc) continue;
        std::wstring path = argv[a + 1];
        int x = a + 3 < argc ? _wtoi(argv[a + 2]) : (-2147483647 - 1), y = a + 3 < argc ? _wtoi(argv[a + 3]) : 0;
        LocalFree(argv);
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&gfx::dw);
        std::wstring fam = PickFamily({ L"SF Pro Text", L"Inter", L"Segoe UI Variable Text", L"Segoe UI" });
        Log(L"info como administrador: %ls", path.c_str());
        info::Show(path, nullptr, fam, IsDarkMode(), true, x, y);
        MSG msg;
        while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (g_log) fclose(g_log);
        return 0;
    }
    for (int a = 1; a < argc; a++) {  // comandos que fazem uma coisa e saem
        if (!_wcsicmp(argv[a], L"--marcar") && a + 2 < argc) {
            bool ok = tags::Write(argv[a + 1], tags::Read(argv[a + 1]) | (1u << (_wtoi(argv[a + 2]) % tags::COUNT)));
            if (g_log) fclose(g_log);
            return ok ? 0 : 1;
        }
        if (!_wcsicmp(argv[a], L"--padrao") && a + 1 < argc) {  // on | off | status
            std::wstring m = argv[a + 1];
            bool ok = m == L"on" ? padrao::Enable(g_log) : m == L"off" ? padrao::Disable(g_log) : true;
            const wchar_t* st = padrao::Enabled() ? Tr(L"O Finder é o padrão para abrir pastas.") : Tr(L"O Explorador do Windows abre as pastas.");
            Log(L"padrao %ls: %ls", m.c_str(), st);
            if (!AttachConsole(ATTACH_PARENT_PROCESS) && !wcsstr(GetCommandLineW(), L"--quieto")) MessageBoxW(nullptr, st, L"Finder", MB_ICONINFORMATION);
            if (g_log) fclose(g_log);
            return ok ? 0 : 1;
        }
    }
    Launch L;
    ParseLaunch(argc, argv, L);
    LocalFree(argv);
    PickerRecall(L);
    if (!L.reserva && !ResolveStart(L)) { if (g_log) fclose(g_log); return 0; }

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));  // ícone do .exe (Finder.png)
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"ArquivosJanela";
    RegisterClassExW(&wc);

    g_reserva = L.reserva;
    g_hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, L.reserva ? RESERVA_TITLE : L"Finder", WS_OVERLAPPEDWINDOW,
                             0, 0, 1060, 680, nullptr, nullptr, hi, nullptr);
    ampliacao::Janela(g_hwnd);  // abrir e fechar animados (o Shell anima)
    Arrange(L);
    g_dpi = (float)GetDpiForWindow(g_hwnd); g_scale = g_dpi / 96.f;

    MARGINS margins{ -1, -1, -1, -1 };  // o "vidro" do DWM ocupa a janela inteira
    DwmExtendFrameIntoClientArea(g_hwnd, &margins);
    g_tahoe = tahoe::On();
    ApplyWindowTheme();
    SetTimer(g_hwnd, T_TAHOE, 1500, nullptr);
    SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    if (!gfx::Init(g_hwnd)) {
        MessageBoxW(nullptr, Tr(L"Não foi possível iniciar o Direct2D/DirectComposition. Veja arquivos.log."), Tr(L"Finder"), MB_ICONERROR);
        return 1;
    }
    CreateFormats();
    thumbs::Start(g_hwnd, WM_APP_THUMB);
    media::Init(g_hwnd, WM_APP_MEDIA);
    BuildSidebar();
    IDropTarget* drop = CreateDropTarget(g_hwnd);
    RegisterDragDrop(g_hwnd, drop);
    shadow::Create(g_hwnd);

    if (L.reserva) {  // pronto e escondido: espera um pedido (WM_COPYDATA) e já carrega os menus
        Navigate(KnownFolder(FOLDERID_Profile), false);
        SetWindowTextW(g_hwnd, RESERVA_TITLE);  // Navigate troca o título; é por ele que a reserva é achada
        Render();
        PostMessageW(g_hwnd, WM_APP_WARMUP, 0, 0);
        Log(L"reserva pronta");
    } else {
        ApplyLaunch(L, show);
        SetTimer(g_hwnd, T_WARMUP, 300, nullptr);  // menus pré-carregados logo depois de abrir (em outra thread)
        FILETIME c, e, k, u, now;  // do início do processo até a janela na tela
        if (GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) {
            GetSystemTimeAsFileTime(&now);
            auto q = [](FILETIME f) { return ((ULONGLONG)f.dwHighDateTime << 32) | f.dwLowDateTime; };
            Log(L"abertura (processo novo): janela na tela em %d ms", (int)((q(now) - q(c)) / 10000));
        }
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }

    thumbs::Stop();
    drop->Release();
    ReleaseImages();
    if (g_log) fclose(g_log);
    OleUninitialize();
    return g_pick.on ? g_exitCode : (int)msg.wParam;
}

// ---- modo nativo (barra-nativa\MacFinder.dll): o Finder como a janela de pasta do próprio Windows, numa thread
// dentro do explorer.exe. Uma janela só (de reserva, escondida): os pedidos chegam por WM_COPYDATA (a classe
// CLSID_ExecuteFolder trocada, o Dock, a barra...); fechada, ela some e volta a ser a reserva. Pastas a mais
// abrem em abas (o "Abrir pastas em abas" do Finder do Mac).
int FinderNativo(HINSTANCE hi, const std::wstring& dirBin) {
    g_finderNativo = true;
    g_dirNativo = dirBin;
    g_inst = hi;
    SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    OleInitialize(nullptr);
    g_log = _wfsopen(L"C:\\Users\\Public\\FinderNativo.log", L"a, ccs=UTF-8", _SH_DENYNO);
    SYSTEMTIME t; GetLocalTime(&t);
    Log(L"\n===== %04d-%02d-%02d %02d:%02d:%02d (nativo, explorer %lu)", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, GetCurrentProcessId());
    ctxmenu::Init(hi, g_log);
    LoadOptions();
    g_dark = IsDarkMode();

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"ArquivosJanela";
    RegisterClassExW(&wc);

    g_reserva = true;
    Launch L; L.reserva = true;
    g_hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, RESERVA_TITLE, WS_OVERLAPPEDWINDOW,
                             0, 0, 1060, 680, nullptr, nullptr, hi, nullptr);
    ampliacao::Janela(g_hwnd);
    Arrange(L);
    g_dpi = (float)GetDpiForWindow(g_hwnd); g_scale = g_dpi / 96.f;
    MARGINS margins{ -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(g_hwnd, &margins);
    g_tahoe = tahoe::On();
    ApplyWindowTheme();
    SetTimer(g_hwnd, T_TAHOE, 1500, nullptr);
    SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (!gfx::Init(g_hwnd)) { Log(L"nativo: Direct2D/DirectComposition falhou"); return 1; }
    CreateFormats();
    thumbs::Start(g_hwnd, WM_APP_THUMB);
    media::Init(g_hwnd, WM_APP_MEDIA);
    BuildSidebar();
    IDropTarget* drop = CreateDropTarget(g_hwnd);
    RegisterDragDrop(g_hwnd, drop);
    shadow::Create(g_hwnd);
    Navigate(KnownFolder(FOLDERID_Profile), false);
    SetWindowTextW(g_hwnd, RESERVA_TITLE);
    Render();
    PostMessageW(g_hwnd, WM_APP_WARMUP, 0, 0);
    Log(L"nativo: reserva pronta (janela %p)", g_hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    thumbs::Stop();
    drop->Release();
    Log(L"nativo: encerrado");
    if (g_log) fflush(g_log);
    return 0;
}

// ---- Abrir/Salvar embutido (barra-nativa\MacDialogo.dll): o Finder em modo escolher DENTRO do programa que
// pediu, na thread dele, como a janela do Windows. A janela é criada no primeiro diálogo e só se esconde no fim
// (os próximos diálogos do mesmo programa reaproveitam Direct2D, miniaturas e barra lateral). cmd = a mesma
// linha de comando do WacOSFinder.exe --escolher (com --resultado). Volta 0 = escolheu, 1 = cancelou,
// -1 = não deu (outra thread ou erro: quem chamou usa outro caminho).
int FinderEscolher(HINSTANCE hi, const std::wstring& dirBin, const std::wstring& cmd) {
    static DWORD tid;
    static IDropTarget* drop;
    if (tid && tid != GetCurrentThreadId()) return -1;
    if (!tid) {
        g_finderEmbutido = true;
        g_dirNativo = dirBin;
        g_inst = hi;
        wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
        g_log = _wfsopen((std::wstring(tmp) + L"MacDialogo.log").c_str(), L"a, ccs=UTF-8", _SH_DENYNO);
        ctxmenu::Init(hi, g_log);
        LoadOptions();
        g_dark = IsDarkMode();
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = WndProc;
        wc.hInstance = hi;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
        if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        wc.lpszClassName = L"ArquivosEscolher";  // não é "ArquivosJanela": é do programa que pediu, não do Finder
        RegisterClassExW(&wc);
        DPI_AWARENESS_CONTEXT old = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        g_hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, L"Finder", WS_OVERLAPPEDWINDOW,
                                 0, 0, 900, 580, nullptr, nullptr, hi, nullptr);
        if (old) SetThreadDpiAwarenessContext(old);
        if (!g_hwnd) { Log(L"embutido: CreateWindow falhou (%lu)", GetLastError()); return -1; }
        ampliacao::Janela(g_hwnd);
        g_dpi = (float)GetDpiForWindow(g_hwnd); g_scale = g_dpi / 96.f;
        MARGINS margins{ -1, -1, -1, -1 };
        DwmExtendFrameIntoClientArea(g_hwnd, &margins);
        g_tahoe = tahoe::On();
        ApplyWindowTheme();
        SetTimer(g_hwnd, T_TAHOE, 1500, nullptr);
        SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        if (!gfx::Init(g_hwnd)) { Log(L"embutido: Direct2D/DirectComposition falhou"); DestroyWindow(g_hwnd); g_hwnd = nullptr; return -1; }
        CreateFormats();
        thumbs::Start(g_hwnd, WM_APP_THUMB);
        media::Init(g_hwnd, WM_APP_MEDIA);
        BuildSidebar();
        drop = CreateDropTarget(g_hwnd);
        RegisterDragDrop(g_hwnd, drop);
        shadow::Create(g_hwnd);
        tid = GetCurrentThreadId();
        SYSTEMTIME t; GetLocalTime(&t);
        wchar_t exe[MAX_PATH] = L""; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        Log(L"\n===== %04d-%02d-%02d %02d:%02d:%02d (embutido em %ls)", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, exe);
    }
    if (!IsWindow(g_hwnd)) return -1;
    // de um diálogo para o outro: o que é de cada pedido volta ao começo
    if (g_ql) QlClose();
    if (g_searching) ExitSearch();
    g_viewOpts = false;
    g_pick = Picker{};
    g_pickName.SetText(L"", false);
    g_pickNameFocus = false;
    while (g_tabs.size() > 1) g_tabs.pop_back();
    g_curTab = 0;
    g_exitCode = 1;
    g_pickDone = false;
    if (g_pickView >= 0) g_view = (View)g_pickView;

    std::wstring linha = L"\"WacOSFinder.exe\" " + cmd;
    int argc = 0; wchar_t** argv = CommandLineToArgvW(linha.c_str(), &argc);
    Launch L;
    ParseLaunch(argc, argv, L);
    LocalFree(argv);
    PickerRecall(L);
    while (!L.start.empty() && L.start.back() == L'"') L.start.pop_back();
    DWORD at = L.start.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(L.start.c_str());
    if (at == INVALID_FILE_ATTRIBUTES || !(at & FILE_ATTRIBUTE_DIRECTORY)) L.start = KnownFolder(FOLDERID_Documents);
    Log(L"embutido: %ls", cmd.c_str());
    Arrange(L);
    ApplyLaunch(L, SW_SHOWNORMAL);

    // laço modal (como o do diálogo do Windows): até escolher ou cancelar
    MSG msg;
    bool quit = false; WPARAM quitCode = 0;
    while (!g_pickDone) {
        BOOL r = GetMessageW(&msg, nullptr, 0, 0);
        if (r <= 0) { quit = true; quitCode = msg.wParam; if (!g_pickDone) PickerCancel(); break; }
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    if (quit) PostQuitMessage((int)quitCode);  // o programa estava saindo: o WM_QUIT é dele
    return g_exitCode;
}
