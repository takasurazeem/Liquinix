// Menu de contexto próprio sobre as extensões reais do Windows (ver context_menu.h).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <share.h>
#include <cstdarg>
#include <cmath>
#include "context_menu.h"
#include <d3d11.h>
#include <d2d1_1.h>
#include <d2d1effects.h>
#include <DispatcherQueue.h>
#include <chrono>
#include <thread>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.UI.Composition.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <windows.ui.composition.interop.h>
#include "../comum/idioma.h"
#include "../comum/vidro.h"
#pragma comment(lib, "windowsapp.lib")
#pragma comment(lib, "CoreMessaging.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxguid.lib")
namespace wuc = winrt::Windows::UI::Composition;

static HINSTANCE g_inst;
static ID2D1Factory* g_d2d;
static IDWriteFactory* g_dw;
static std::wstring g_font = L"Segoe UI";
static FILE* g_log;

static void Log(const wchar_t* fmt, ...) {
    if (!g_log) return;
    va_list a; va_start(a, fmt); vfwprintf(g_log, fmt, a); va_end(a);
    fputwc(L'\n', g_log); fflush(g_log);
}

// ---------------------------------------------------------------------------
// Menu do shell (IContextMenu)

static const UINT ID_FIRST = 1, ID_LAST = 0x7FFF;

struct ShellMenu {
    IContextMenu* cm = nullptr;
    IContextMenu2* cm2 = nullptr;
    IContextMenu3* cm3 = nullptr;
    HMENU hmenu = nullptr;
    ~ShellMenu() {
        if (hmenu) DestroyMenu(hmenu);
        if (cm3) cm3->Release();
        if (cm2) cm2->Release();
        if (cm) cm->Release();
    }
};

static thread_local ShellMenu* g_active;  // menu cujas mensagens WM_INITMENUPOPUP etc. vão para a extensão (por thread: o pré-carregamento roda em outra)

static bool ForwardMenuMsg(UINT msg, WPARAM wp, LPARAM lp, LRESULT* res) {
    if (!g_active) return false;
    if (g_active->cm3) return SUCCEEDED(g_active->cm3->HandleMenuMsg2(msg, wp, lp, res));
    if (g_active->cm2) { *res = 0; return SUCCEEDED(g_active->cm2->HandleMenuMsg(msg, wp, lp)); }
    return false;
}

HRESULT ctxmenu::GetUIObject(HWND hwnd, const std::vector<std::wstring>& paths, REFIID riid, void** out) {
    *out = nullptr;
    if (paths.empty()) return E_INVALIDARG;
    std::vector<PIDLIST_ABSOLUTE> abs;
    HRESULT hr = S_OK;
    for (auto& p : paths) {
        PIDLIST_ABSOLUTE pidl;
        hr = SHParseDisplayName(p.c_str(), nullptr, &pidl, 0, nullptr);
        if (FAILED(hr)) break;
        abs.push_back(pidl);
    }
    if (SUCCEEDED(hr)) {
        IShellFolder* parent; PCUITEMID_CHILD child;
        hr = SHBindToParent(abs[0], IID_PPV_ARGS(&parent), &child);
        if (SUCCEEDED(hr)) {
            std::vector<PCUITEMID_CHILD> kids;
            for (auto a : abs) kids.push_back(ILFindLastID(a));
            hr = parent->GetUIObjectOf(hwnd, (UINT)kids.size(), kids.data(), riid, nullptr, out);
            parent->Release();
        }
    }
    for (auto a : abs) CoTaskMemFree(a);
    return hr;
}

static HRESULT GetBackgroundMenu(HWND hwnd, const std::wstring& folder, IContextMenu** out) {
    PIDLIST_ABSOLUTE pidl;
    HRESULT hr = SHParseDisplayName(folder.c_str(), nullptr, &pidl, 0, nullptr);
    if (FAILED(hr)) return hr;
    IShellFolder* sf;
    hr = SHBindToObject(nullptr, pidl, nullptr, IID_PPV_ARGS(&sf));
    if (SUCCEEDED(hr)) { hr = sf->CreateViewObject(hwnd, IID_PPV_ARGS(out)); sf->Release(); }
    CoTaskMemFree(pidl);
    return hr;
}

static bool LoadShellMenu(HWND hwnd, const std::wstring& folder, const std::vector<std::wstring>& paths, UINT flags, ShellMenu& sm) {
    IContextMenu* cm = nullptr;
    HRESULT hr = paths.empty() ? GetBackgroundMenu(hwnd, folder, &cm) : ctxmenu::GetUIObject(hwnd, paths, IID_IContextMenu, (void**)&cm);
    if (FAILED(hr)) { Log(L"erro: IContextMenu 0x%08X para %ls", hr, paths.empty() ? folder.c_str() : paths[0].c_str()); return false; }
    sm.cm = cm;
    cm->QueryInterface(IID_PPV_ARGS(&sm.cm2));
    cm->QueryInterface(IID_PPV_ARGS(&sm.cm3));
    sm.hmenu = CreatePopupMenu();
    hr = cm->QueryContextMenu(sm.hmenu, 0, ID_FIRST, ID_LAST, flags);
    if (FAILED(hr)) { Log(L"erro: QueryContextMenu 0x%08X", hr); return false; }
    return true;
}

// Algumas extensões mal escritas travam em GetCommandString; isolamos com SEH.
static HRESULT SafeVerb(IContextMenu* cm, UINT off, wchar_t* v, UINT cch) {
    __try { return cm->GetCommandString(off, GCS_VERBW, nullptr, (LPSTR)v, cch); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return E_FAIL; }
}

static std::wstring Lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
    return s;
}

static std::wstring VerbOf(ShellMenu& sm, UINT cmd) {
    if (cmd < ID_FIRST || cmd > ID_LAST) return L"";
    wchar_t v[256] = {};
    if (SUCCEEDED(SafeVerb(sm.cm, cmd - ID_FIRST, v, 255))) return Lower(v);
    return L"";
}

// ---------------------------------------------------------------------------
// Modelo do menu próprio

struct MItem {
    std::wstring label, verb, shortcut;
    UINT cmd = 0;
    bool sep = false, header = false, disabled = false, checked = false;
    int special = 0;  // 1 = abrir o menu clássico do Windows
    std::vector<MItem> sub;
    wchar_t icon = 0;
    std::vector<ctxmenu::Dot> dots;
    HMENU src = nullptr;  // submenu original, relido ao abrir (extensões que completam depois)
};

// Tem submenu, mesmo que ainda não lido (extensão que preenche depois).
static bool HasSub(const MItem& m) { return !m.sub.empty() || m.src; }

static MItem Sep() { MItem m; m.sep = true; return m; }
static MItem Header(const std::wstring& s) { MItem m; m.header = true; m.label = s; return m; }

// Remove aceleradores (&) e o atalho depois do Tab: no macOS o menu de contexto não mostra atalhos.
static std::wstring CleanLabel(const wchar_t* s) {
    std::wstring r;
    for (; *s; ++s) {
        if (*s == L'\t') break;
        if (*s == L'&') { if (s[1] == L'&') { r += L'&'; ++s; } continue; }
        r += *s;
    }
    size_t a = r.find_first_not_of(L' '), b = r.find_last_not_of(L' ');
    return a == std::wstring::npos ? L"" : r.substr(a, b - a + 1);
}

// Envia WM_INITMENUPOPUP a todos os submenus ("Enviar para", "Abrir com" só se preenchem assim).
static void InitSubmenus(HMENU h, int depth, std::vector<HMENU>& all) {
    int n = GetMenuItemCount(h);
    for (int i = 0; i < n; i++) {
        HMENU sub = GetSubMenu(h, i);
        if (!sub) continue;
        LRESULT r;
        ForwardMenuMsg(WM_INITMENUPOPUP, (WPARAM)sub, MAKELPARAM(i, FALSE), &r);
        all.push_back(sub);
        if (depth < 4) InitSubmenus(sub, depth + 1, all);
    }
}

static int TotalItems(const std::vector<HMENU>& menus) {
    int n = 0;
    for (HMENU m : menus) n += GetMenuItemCount(m);
    return n;
}

// Algumas extensões ("Enviar para" em pastas) completam o submenu depois, via mensagens
// postadas. Esperamos um pouco (máx. ~150 ms) para o menu abrir já quase completo; o resto
// entra quando o submenu é aberto (OpenSub relê o HMENU original).
static void PumpUntilStable(const std::vector<HMENU>& menus) {
    int last = TotalItems(menus), still = 0;
    // sem esperar: os submenus são relidos quando abrem (OpenSub); a espera atrasava cada menu ~150 ms
    for (DWORD t0 = GetTickCount(); GetTickCount() - t0 < 0 && still < 3;) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { PostQuitMessage((int)msg.wParam); return; }
            if (msg.message >= WM_KEYFIRST && msg.message <= WM_KEYLAST) continue;
            if (msg.message >= WM_MOUSEFIRST && msg.message <= WM_MOUSELAST) continue;
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        Sleep(15);
        int n = TotalItems(menus);
        still = (n == last) ? still + 1 : 0;
        last = n;
    }
}

static std::vector<MItem> ReadMenuItems(ShellMenu& sm, HMENU h, int depth, int* noText);

// Lê o menu inteiro: inicializa os submenus, espera as extensões assíncronas e mede o tempo.
static std::vector<MItem> ReadMenu(ShellMenu& sm, HMENU h, int depth, int* noText) {
    LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
    std::vector<HMENU> subs;
    InitSubmenus(h, depth, subs);
    PumpUntilStable(subs);
    auto out = ReadMenuItems(sm, h, depth, noText);
    QueryPerformanceCounter(&b);
    Log(L"  leitura do menu: %d ms (%zu submenus)", (int)((b.QuadPart - a.QuadPart) * 1000 / f.QuadPart), subs.size());
    return out;
}

static std::vector<MItem> ReadMenuItems(ShellMenu& sm, HMENU h, int depth, int* noText) {
    std::vector<MItem> out;
    int n = GetMenuItemCount(h);
    for (int i = 0; i < n; i++) {
        wchar_t buf[512] = {};
        MENUITEMINFOW mi{ sizeof(mi) };
        mi.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_SUBMENU | MIIM_STRING;
        mi.dwTypeData = buf; mi.cch = 511;
        if (!GetMenuItemInfoW(h, i, TRUE, &mi)) continue;
        if (mi.fType & MFT_SEPARATOR) { out.push_back(Sep()); continue; }
        MItem it;
        it.label = (mi.fType & MFT_OWNERDRAW) ? L"" : CleanLabel(buf);
        it.disabled = (mi.fState & (MFS_DISABLED | MFS_GRAYED)) != 0;
        it.checked = (mi.fState & MFS_CHECKED) != 0;
        if (mi.hSubMenu) {
            it.src = mi.hSubMenu;
            if (depth < 4) it.sub = ReadMenuItems(sm, mi.hSubMenu, depth + 1, noText);
            // Some só o submenu cujos itens legíveis estão todos desativados ("Retrieving libraries...").
            // Sem itens legíveis ("Enviar para" ainda com o item provisório sem texto) ele fica,
            // e o conteúdo é relido quando o submenu abre.
            bool anyReadable = std::any_of(it.sub.begin(), it.sub.end(), [](const MItem& c) { return !c.sep; });
            bool anyEnabled = std::any_of(it.sub.begin(), it.sub.end(), [](const MItem& c) { return !c.sep && !c.disabled; });
            if (anyReadable ? !anyEnabled : GetMenuItemCount(mi.hSubMenu) == 0) continue;
        } else {
            it.cmd = mi.wID;
            it.verb = VerbOf(sm, it.cmd);
        }
        if (it.label.empty()) {
            if (noText) (*noText)++;
            Log(L"  aviso: item sem texto (id=%u verbo=%ls ownerdraw=%d)", it.cmd, it.verb.c_str(), (mi.fType & MFT_OWNERDRAW) ? 1 : 0);
            continue;
        }
        out.push_back(std::move(it));
    }
    return out;
}

static void CleanSeps(std::vector<MItem>& v) {
    std::vector<MItem> r;
    for (auto& m : v) {
        if (m.sep && (r.empty() || r.back().sep || r.back().header)) continue;
        r.push_back(std::move(m));
    }
    while (!r.empty() && (r.back().sep || r.back().header)) r.pop_back();
    v = std::move(r);
}

// Submenus de um nível só: níveis mais fundos viram seções com cabeçalho.
static void FlattenInto(std::vector<MItem>& dst, const std::vector<MItem>& src) {
    for (auto& m : src) {
        if (!m.sub.empty()) {
            dst.push_back(Sep());
            dst.push_back(Header(m.label));
            FlattenInto(dst, m.sub);
            dst.push_back(Sep());
        } else {
            dst.push_back(m);
        }
    }
}

static std::vector<MItem> Flatten(const std::vector<MItem>& src) {
    std::vector<MItem> r;
    FlattenInto(r, src);
    CleanSeps(r);
    return r;
}

static bool StartsWith(const std::wstring& s, const wchar_t* p) { return Lower(s).rfind(Lower(p), 0) == 0; }

enum { G_OPEN, G_ORG, G_MORE };

static void Classify(const MItem& m, int& group, int& rank) {
    struct V { const wchar_t* verb; int rank; };
    static const V open[] = { {L"open",0}, {L"opennewwindow",1}, {L"opennewtab",1}, {L"opennewprocess",1}, {L"explore",2},
                              {L"runas",3}, {L"openas",4}, {L"edit",5}, {L"print",6}, {L"play",7}, {L"preview",8} };
    static const V org[] = { {L"cut",0}, {L"copy",1}, {L"paste",2}, {L"pastelink",3}, {L"copyaspath",4}, {L"link",5},
                             {L"rename",6}, {L"delete",8}, {L"properties",9} };
    for (auto& v : open) if (m.verb == v.verb) { group = G_OPEN; rank = v.rank; return; }
    for (auto& v : org) if (m.verb == v.verb) { group = G_ORG; rank = v.rank; return; }
    if (HasSub(m)) {
        if (StartsWith(m.label, L"Novo") || StartsWith(m.label, L"New")) { group = G_OPEN; rank = -1; return; }
        if (StartsWith(m.label, L"Abrir com") || StartsWith(m.label, L"Open with")) { group = G_OPEN; rank = 4; return; }
        if (StartsWith(m.label, L"Enviar para") || StartsWith(m.label, L"Send to")) { group = G_ORG; rank = 7; return; }
    }
    group = G_MORE; rank = 50;
}

// Reorganiza no estilo macOS: no máximo 3 grupos (abrir / organizar / extras).
static std::vector<MItem> Organize(const std::vector<MItem>& raw) {
    struct R { MItem it; int g, rank; };
    std::vector<R> rs;
    for (auto& m : raw) {
        if (m.sep || m.header) continue;
        R r{ m, 0, 0 };
        if (!r.it.sub.empty()) r.it.sub = Flatten(r.it.sub);
        Classify(r.it, r.g, r.rank);
        rs.push_back(std::move(r));
    }
    std::stable_sort(rs.begin(), rs.end(), [](const R& a, const R& b) { return a.g != b.g ? a.g < b.g : a.rank < b.rank; });
    std::vector<MItem> groups[3];
    for (auto& r : rs) groups[r.g].push_back(std::move(r.it));

    auto& more = groups[G_MORE];
    if (more.size() > 6) {  // muitos extras: mantém até 4 submenus (7-Zip, Git...) e guarda o resto em "Mais ações"
        std::vector<MItem> keep, rest;
        for (auto& m : more) {
            if (HasSub(m) && keep.size() < 4) keep.push_back(std::move(m));
            else if (!m.sub.empty()) { rest.push_back(Sep()); rest.push_back(Header(m.label)); for (auto& c : m.sub) rest.push_back(c); rest.push_back(Sep()); }
            else rest.push_back(std::move(m));
        }
        CleanSeps(rest);
        MItem box; box.label = Tr(L"Mais ações"); box.sub = std::move(rest);
        keep.push_back(std::move(box));
        more = std::move(keep);
    }

    MItem classic; classic.label = Tr(L"Mostrar todas as opções"); classic.special = 1;
    groups[G_ORG].push_back(classic);

    std::vector<MItem> out;
    for (auto& g : groups) {
        if (g.empty()) continue;
        if (!out.empty()) out.push_back(Sep());
        for (auto& m : g) out.push_back(std::move(m));
    }
    return out;
}

static void DumpTree(const std::vector<MItem>& v, int d) {
    std::wstring pad(d * 2 + 2, L' ');
    for (auto& m : v) {
        if (m.sep) Log(L"%ls----", pad.c_str());
        else if (m.header) Log(L"%ls[%ls]", pad.c_str(), m.label.c_str());
        else Log(L"%ls%ls%ls%ls   {verbo=%ls id=%u}", pad.c_str(), m.label.c_str(), !HasSub(m) ? L"" : L"  ›",
                 m.disabled ? L" (desativado)" : L"", m.verb.c_str(), m.cmd);
        if (!m.sub.empty()) DumpTree(m.sub, d + 1);
    }
}

// ---------------------------------------------------------------------------
// Janela do menu próprio (Direct2D sobre acrílico do DWM)

namespace M {  // medidas em DIPs
const float PAD_Y = 5, INSET = 5, ITEM_H = 22, SEP_H = 9, HDR_H = 20, FONT = 13, HDR_FONT = 11, RADIUS = 4;
const float MIN_W = 190, MAX_W = 380;
}

struct Level {
    HWND hwnd = nullptr;
    std::vector<MItem>* items = nullptr;
    std::vector<D2D1_RECT_F> rects;
    int hover = -1, openChild = -1;
    float scale = 1, textL = 0, textR = 0;  // margens do texto em pixels
    int dotHover = -1;                      // bolinha sob o mouse (fileira de etiquetas)
    ID2D1DCRenderTarget* rt = nullptr;
    HDC mem = nullptr;      // desenhamos numa DIB de 32 bits e copiamos para a janela (alfa preservado)
    HBITMAP dib = nullptr;
    IDWriteTextFormat* tf = nullptr;
    IDWriteTextFormat* tfHdr = nullptr;
    IDWriteTextFormat* tfIcon = nullptr;  // ícones (Segoe Fluent Icons)
    // Composição (cantos de 12 lisos, como no Tahoe): a janela tem uma margem em volta para a sombra; o
    // menu é um cartão recortado arredondado com o desfoque do que está atrás e o desenho por cima
    bool comp = false;
    int margin = 0;
    float mw = 0, mh = 0;  // tamanho do menu (px), sem a margem
    wuc::Desktop::DesktopWindowTarget target{ nullptr };
    wuc::ContainerVisual root{ nullptr };
    wuc::CompositionDrawingSurface contentS{ nullptr }, shadowS{ nullptr };
};

struct Session {
    std::vector<Level> levels;
    HWND owner = nullptr;
    bool done = false, dark = false;
    MItem* result = nullptr;
    int pendLevel = -1, pendItem = -1;
};

static Session* g_ses;
static bool g_demo;  // --demo: abre o menu sozinho para captura de tela
static ctxmenu::Glass g_glass;
static bool g_glassSet;  // o Shell passou o vidro; nos outros apps, o tema é lido do ajustes.txt
static bool g_plainNext, g_plain;  // menu sem ícones automáticos (PlainNext)
void ctxmenu::PlainNext() { g_plainNext = true; }
static std::vector<ctxmenu::Item> g_extraNext;  // itens do app para o próximo Show (ExtraNext)
void ctxmenu::ExtraNext(const std::vector<Item>& items) { g_extraNext = items; }
void ctxmenu::SetGlass(const Glass& g) { g_glass = g; g_glassSet = true; }

// Tema Tahoe ligado (Ajustes > Aparência: liquid_glass=1 em %APPDATA%\Shell\ajustes.txt). O Shell avisa
// pelo SetGlass; os outros apps (Finder, Notas...) leem o arquivo, no máximo a cada 2 s.
static bool g_fileGolden;  // golden_gate=1 lido do arquivo (apps fora do Shell)
static bool g_fileLeopard;  // leopard=1 lido do arquivo (apps fora do Shell)
static bool Tahoe() {
    if (g_glassSet) return g_glass.liquid;
    static DWORD last; static bool on;
    if (last && GetTickCount() - last < 2000) return on;
    last = GetTickCount() | 1;
    wchar_t ad[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"APPDATA", ad, MAX_PATH)) return on;
    FILE* f = nullptr;
    if (_wfopen_s(&f, (std::wstring(ad) + L"\\Shell\\ajustes.txt").c_str(), L"r, ccs=UTF-8") || !f) return on;
    wchar_t line[512];
    while (fgetws(line, 512, f)) {
        std::wstring l = line;
        while (!l.empty() && (l.back() == L'\n' || l.back() == L'\r' || l.back() == L' ')) l.pop_back();
        if (l == L"liquid_glass=1") on = true;
        if (l == L"liquid_glass=0") on = false;
        if (l == L"golden_gate=1") g_fileGolden = true;
        if (l == L"golden_gate=0") g_fileGolden = false;
        if (l == L"leopard=1") g_fileLeopard = true;
        if (l == L"leopard=0") g_fileLeopard = false;
    }
    fclose(f);
    return on;
}
// macOS 27 Golden Gate (menu Go do Finder, captura 2x): linhas de 24, separador de 11, 5,5 de folga e de recuo do
// destaque, destaque com canto 7, menu com canto 11, texto a 16 da borda (41 com símbolo) e símbolo só em Ajustes…
static bool Golden() { bool t = Tahoe(); return t && (g_glassSet ? g_glass.golden : g_fileGolden); }
// Mac OS X Leopard (leopard=1 sem o Liquid Glass): menus brancos translúcidos e o item escolhido num degradê azul que
// vai de uma borda à outra, sem cantos (como no 10.5)
static bool Leopard() { bool t = Tahoe(); return !t && (g_glassSet ? g_glass.leopard : g_fileLeopard); }
static bool GoldenIcon(const std::wstring& l) {
    for (const wchar_t* k : { L"Ajustes", L"Settings", L"System Settings", L"Preferências", L"Preferences", L"Sobre Este", L"About This", L"App Store", L"Microsoft Store" })  // macOS 27: o menu da Apple tem símbolo nestes if (l.rfind(k, 0) == 0) return true;
    return false;
}

// Ícone do Tahoe pelo nome do item, para os menus que não escolhem os seus (glifos da Segoe Fluent Icons).
// O macOS 26 põe um símbolo em quase todo item de menu; os que não casam ficam sem, alinhados com os outros.
wchar_t ctxmenu::AutoIcon(const std::wstring& label) {
    std::wstring s;
    for (wchar_t c : label) s += (wchar_t)towlower(c);
    auto has = [&](const wchar_t* k) { return s.find(k) != std::wstring::npos; };
    auto starts = [&](const wchar_t* k) { return s.rfind(k, 0) == 0; };
    if (has(L"nova aba") || has(L"new tab")) return 0xE8A7;
    if (has(L"nova janela") || has(L"new window")) return 0xE78B;
    if (has(L"todas as janelas") || has(L"all windows") || has(L"mission control")) return 0xE7C4;
    if (has(L"iniciar sessão") || has(L"login") || has(L"startup")) return 0xE7E8;
    if (has(L"nova pasta") || has(L"new folder")) return 0xE8F4;
    if (starts(L"abrir com") || starts(L"open with")) return 0xE7AC;
    if (has(L"mostrar no finder") || has(L"mostrar na pasta") || has(L"abrir local") || has(L"show in")) return 0xE838;
    if (starts(L"abrir") || starts(L"open")) return 0xE8E5;
    if (has(L"lixo") || has(L"excluir") || has(L"apagar") || has(L"delete") || has(L"trash")) return 0xE74D;
    if (has(L"remover do dock") || has(L"desafixar")) return 0xE77A;
    if (has(L"manter no dock") || has(L"fixar")) return 0xE718;
    if (starts(L"sobre este") || starts(L"about this")) return 0xE7F8;  // notebook (menu da Apple do macOS 27)
    if (has(L"app store") || has(L"microsoft store")) return 0xE719;   // sacola da loja
    if (has(L"informaç") || has(L"propriedades") || has(L"properties") || starts(L"sobre")) return 0xE946;
    if (has(L"renomear") || has(L"rename")) return 0xE8AC;
    if (has(L"comprimir") || has(L"compactar") || has(L"compress") || has(L"zip")) return 0xF012;
    if (has(L"duplicar") || has(L"duplicate")) return 0xF413;
    if (has(L"atalho") || has(L"alias") || has(L"shortcut")) return 0xE71B;
    if (has(L"visualização rápida") || has(L"quick look")) return 0xE890;
    if (starts(L"recortar") || starts(L"cut")) return 0xE8C6;
    if (starts(L"copiar") || starts(L"copy")) return 0xE8C8;
    if (starts(L"colar") || starts(L"paste")) return 0xE77F;
    if (has(L"compartilhar") || has(L"share")) return 0xE72D;
    if (has(L"etiqueta") || has(L"tag")) return 0xE8EC;
    if (has(L"ações rápidas") || has(L"mais ações") || has(L"quick actions")) return 0xE945;
    if (has(L"ordenar") || has(L"sort")) return 0xE8CB;
    if (has(L"terminal") || has(L"prompt") || has(L"powershell")) return 0xE756;
    if (has(L"administrador") || has(L"admin")) return 0xEA18;
    if (has(L"papel de parede") || has(L"wallpaper") || has(L"fundo da área")) return 0xE91B;
    if (has(L"configurações de tela") || has(L"exibição") || has(L"display")) return 0xE7F4;
    if (has(L"personalizar") || has(L"personalize") || has(L"aparência")) return 0xE771;
    if (has(L"ajustes") || has(L"configura") || has(L"preferências") || has(L"settings") || has(L"opções")) return 0xE713;
    if (starts(L"encerrar") || starts(L"sair") || starts(L"quit") || has(L"forçar")) return 0xE7E8;
    if (starts(L"fechar") || starts(L"close")) return 0xE8BB;
    if (starts(L"ocultar") || starts(L"hide")) return 0xED1A;
    if (starts(L"minimizar") || starts(L"minimize")) return 0xE921;
    if (has(L"atualizar") || has(L"recarregar") || has(L"refresh") || has(L"reload")) return 0xE72C;
    if (starts(L"desfazer") || starts(L"undo")) return 0xE7A7;
    if (starts(L"refazer") || starts(L"redo")) return 0xE7A6;
    if (has(L"selecionar tudo") || has(L"select all")) return 0xE8B3;
    if (has(L"buscar") || has(L"procurar") || has(L"pesquisar") || has(L"search") || has(L"find")) return 0xE721;
    if (has(L"imprimir") || has(L"print")) return 0xE749;
    if (starts(L"salvar") || starts(L"save")) return 0xE74E;
    if (starts(L"editar") || starts(L"edit")) return 0xE70F;
    if (starts(L"executar") || starts(L"run")) return 0xE768;
    if (starts(L"enviar para") || starts(L"send to")) return 0xE724;
    if (starts(L"novo") || starts(L"nova") || starts(L"new")) return 0xE710;
    if (has(L"todas as opções") || has(L"show more")) return 0xE712;
    // o menu completo do Windows (Shift+clique), em inglês e das extensões
    if (has(L"pin to") || has(L"fixar")) return 0xE718;
    if (has(L"unpin")) return 0xE77A;
    if (has(L"give access") || has(L"conceder acesso") || has(L"acesso")) return 0xE72D;
    if (has(L"previous versions") || has(L"versões anteriores")) return 0xE81C;
    if (has(L"7-zip") || has(L"winrar") || has(L"extract") || has(L"extrair")) return 0xF012;
    if (has(L"git") || has(L"visual studio") || has(L"code")) return 0xE943;
    if (has(L"vlc") || has(L"reprodu") || has(L"play") || has(L"media")) return 0xE768;
    if (has(L"scan") || has(L"defender") || has(L"malware") || has(L"verificar")) return 0xEA18;
    if (has(L"ownership") || has(L"permiss")) return 0xE72E;
    if (has(L"linux") || has(L"shell")) return 0xE756;
    if (has(L"browse") || has(L"wiztree") || has(L"analy")) return 0xE721;
    if (has(L"print")) return 0xE749;
    if (has(L"mesa") || has(L"desktop")) return 0xE7F4;
    return 0;
}

static bool IsDarkMode() {
    DWORD v = 1, sz = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                 L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return v == 0;
}

static bool Selectable(const MItem& m) { return !m.sep && !m.header && !m.disabled && m.dots.empty(); }

// Medidas do menu do Tahoe (Liquid Glass), do guia macOS26-Theme-Reference (seção 31, NSPopupMenuWindow medido
// no macOS 26): item de 24, separador de 11, 5 de folga em cima e embaixo, texto em x 14 sem ícones e em x 33
// com a coluna de ícones (SF Symbol 13×15 em x 16), atalho terminando 8 antes da borda, destaque em cápsula de
// raio ~8. Bolinhas das etiquetas: 12 a cada 20, numa linha de 22.
static float SepH() { return Tahoe() ? 11.f : M::SEP_H; }
static float ItemH() { return Tahoe() ? 24.f : M::ITEM_H; }
static float PadY() { return Golden() ? 5.5f : Tahoe() ? 5.f : M::PAD_Y; }
static float Inset() { return Golden() ? 5.5f : M::INSET; }
const float DOT_D = 12, DOT_PITCH = 20, DOT_ROW_H = 22, ICON_CX = 23, ICON_TEXT_L = 34;
static float IconCx() { return Golden() ? 23.f : Tahoe() ? 22.5f : ICON_CX; }   // ícone 13 de largura começando em x 16
static float HotR() { return Golden() ? 7.f : Tahoe() ? 8.f : M::RADIUS; }

// Bolinha da fileira sob o x (pixels da janela) ou -1
static int DotAt(const Level& L, const MItem& m, float x) {
    for (size_t k = 0; k < m.dots.size(); k++) {
        float cx = L.textL + (DOT_D / 2 + 1.5f + k * DOT_PITCH) * L.scale;
        if (fabsf(x - cx) <= DOT_PITCH / 2 * L.scale) return (int)k;
    }
    return -1;
}

// ---- Composição do Windows (cantos arredondados lisos de qualquer raio). O DWM só arredonda janelas
// com 8 px; aqui o menu é desenhado num cartão com recorte arredondado de 12, desfoque do que está atrás
// (HostBackdropBrush) e uma sombra própria que segue o canto. Sem composição: o jeito antigo.
static wuc::Compositor g_wc{ nullptr };
static wuc::CompositionGraphicsDevice g_wgfx{ nullptr };
static winrt::Windows::System::DispatcherQueueController g_wdq{ nullptr };
static ID2D1Device* g_wd2d;
static bool g_wcTried, g_wcOk;
const float CARD_R_TAHOE = 12;  // raio do canto do menu (DIP)
#define CARD_R (Golden() ? 11.f : CARD_R_TAHOE)  // macOS 27: 11 (medido)

static bool CompReady() {
    if (g_wcTried) return g_wcOk;
    g_wcTried = true;
    try {
        if (!winrt::Windows::System::DispatcherQueue::GetForCurrentThread()) {
            DispatcherQueueOptions o{ sizeof(o), DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE };
            winrt::check_hresult(CreateDispatcherQueueController(o, reinterpret_cast<ABI::Windows::System::IDispatcherQueueController**>(winrt::put_abi(g_wdq))));
        }
        g_wc = wuc::Compositor();
        ID3D11Device* d3d = nullptr;
        winrt::check_hresult(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr));
        IDXGIDevice* dx = nullptr; d3d->QueryInterface(IID_PPV_ARGS(&dx));
        ID2D1Factory1* f1 = nullptr;
        winrt::check_hresult(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), nullptr, (void**)&f1));
        winrt::check_hresult(f1->CreateDevice(dx, &g_wd2d));
        dx->Release(); d3d->Release(); f1->Release();
        auto ci = g_wc.as<ABI::Windows::UI::Composition::ICompositorInterop>();
        winrt::check_hresult(ci->CreateGraphicsDevice(g_wd2d, reinterpret_cast<ABI::Windows::UI::Composition::ICompositionGraphicsDevice**>(winrt::put_abi(g_wgfx))));
        g_wcOk = true;
    } catch (...) {
        g_wcOk = false;
        Log(L"menu: composição indisponível; cantos do Windows");
    }
    return g_wcOk;
}

// Espera ms processando o que não é teclado/mouse (a composição só desenha com a fila andando)
static void PumpFor(int ms) {
    DWORD t0 = GetTickCount();
    for (;;) {
        DWORD el = GetTickCount() - t0;
        if ((int)el >= ms) break;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, ms - el, QS_ALLINPUT);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { PostQuitMessage((int)msg.wParam); return; }
            if ((msg.message >= WM_KEYFIRST && msg.message <= WM_KEYLAST) || (msg.message >= WM_MOUSEFIRST && msg.message <= WM_MOUSELAST)) continue;
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
    }
}

// Sombra do cartão: o retângulo arredondado borrado, um pouco para baixo (como a do menu do Mac)
static void DrawShadow(Level& L) {
    auto si = L.shadowS.as<ABI::Windows::UI::Composition::ICompositionDrawingSurfaceInterop>();
    ID2D1DeviceContext* dc = nullptr; POINT off{};
    if (FAILED(si->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), (void**)&dc, &off)) || !dc) return;
    float sc = L.scale, m = (float)L.margin, R = CARD_R * sc;
    dc->SetTransform(D2D1::Matrix3x2F::Translation((float)off.x, (float)off.y));
    dc->PushAxisAlignedClip(D2D1::RectF(0, 0, L.mw + 2 * m, L.mh + 2 * m), D2D1_ANTIALIAS_MODE_ALIASED);
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    ID2D1Image* orig = nullptr; dc->GetTarget(&orig);
    ID2D1CommandList* cl = nullptr;
    dc->CreateCommandList(&cl);
    if (cl) {
        dc->SetTarget(cl);
        ID2D1SolidColorBrush* b = nullptr;
        dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 1), &b);
        // Tahoe (guia, seções 51 e 61): escuro preto 29 %, blur CSS 15 (σ 7,5), +4; claro 39 %, 13 (σ 6,5), +2
        float dy = Tahoe() ? (g_ses && !g_ses->dark ? 2.f : 4.f) : 5.f;
        if (b) { dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(m, m + dy * sc, m + L.mw, m + L.mh + dy * sc), R, R), b); b->Release(); }
        cl->Close();
        dc->SetTarget(orig);
        ID2D1Effect* fx = nullptr;
        dc->CreateEffect(CLSID_D2D1Shadow, &fx);
        if (fx) {
            fx->SetInput(0, cl);
            fx->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, (Tahoe() ? (g_ses && !g_ses->dark ? 6.5f : 7.5f) : 9.f) * sc);
            fx->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(0, 0, 0, Tahoe() ? (g_ses && !g_ses->dark ? 0.39f : 0.29f) : 0.30f));
            dc->DrawImage(fx);
            fx->Release();
        }
        cl->Release();
    }
    if (orig) orig->Release();
    dc->PopAxisAlignedClip();
    dc->Release();
    si->EndDraw();
}

static bool SetupComp(Level& L) {
    try {
        BOOL on = TRUE; DwmSetWindowAttribute(L.hwnd, 17 /*DWMWA_USE_HOSTBACKDROPBRUSH*/, &on, sizeof(on));
        DWORD nc = 1; DwmSetWindowAttribute(L.hwnd, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE: não arredondar*/, &nc, sizeof(nc));
        auto interop = g_wc.as<ABI::Windows::UI::Composition::Desktop::ICompositorDesktopInterop>();
        winrt::check_hresult(interop->CreateDesktopWindowTarget(L.hwnd, TRUE,
            reinterpret_cast<ABI::Windows::UI::Composition::Desktop::IDesktopWindowTarget**>(winrt::put_abi(L.target))));
        float m = (float)L.margin, R = CARD_R * L.scale;
        using namespace winrt::Windows::Graphics::DirectX;
        L.root = g_wc.CreateContainerVisual();
        L.root.Size({ L.mw + 2 * m, L.mh + 2 * m });
        L.shadowS = g_wgfx.CreateDrawingSurface({ L.mw + 2 * m, L.mh + 2 * m }, DirectXPixelFormat::B8G8R8A8UIntNormalized, DirectXAlphaMode::Premultiplied);
        DrawShadow(L);
        auto shadowV = g_wc.CreateSpriteVisual();
        shadowV.Size({ L.mw + 2 * m, L.mh + 2 * m });
        shadowV.Brush(g_wc.CreateSurfaceBrush(L.shadowS));
        auto card = g_wc.CreateContainerVisual();
        card.Offset({ m, m, 0 });
        card.Size({ L.mw, L.mh });
        auto geo = g_wc.CreateRoundedRectangleGeometry();
        geo.Size({ L.mw, L.mh });
        geo.CornerRadius({ R, R });
        card.Clip(g_wc.CreateGeometricClip(geo));
        auto back = g_wc.CreateSpriteVisual();  // o que está atrás, desfocado (o vidro)
        back.Size({ L.mw, L.mh });
        back.Brush(g_wc.CreateHostBackdropBrush());
        L.contentS = g_wgfx.CreateDrawingSurface({ L.mw, L.mh }, DirectXPixelFormat::B8G8R8A8UIntNormalized, DirectXAlphaMode::Premultiplied);
        auto content = g_wc.CreateSpriteVisual();
        content.Size({ L.mw, L.mh });
        content.Brush(g_wc.CreateSurfaceBrush(L.contentS));
        card.Children().InsertAtTop(back);
        card.Children().InsertAtTop(content);
        L.root.Children().InsertAtTop(shadowV);
        L.root.Children().InsertAtTop(card);
        L.target.Root(L.root);
        return true;
    } catch (...) {
        return false;
    }
}

// Retângulo do menu na tela (sem a margem da sombra)
static RECT MenuRect(const Level& L) {
    RECT r; GetWindowRect(L.hwnd, &r);
    InflateRect(&r, -L.margin, -L.margin);
    return r;
}

static int FindLevel(HWND h) {
    if (!g_ses) return -1;
    for (size_t i = 0; i < g_ses->levels.size(); i++) if (g_ses->levels[i].hwnd == h) return (int)i;
    return -1;
}

static void Invalidate(Level& L) { InvalidateRect(L.hwnd, nullptr, FALSE); }

static void ApplyEffects(HWND h, bool dark) {
    DWORD corner = 2;  // DWMWCP_ROUND (Windows 11)
    DwmSetWindowAttribute(h, 33 /*DWMWA_WINDOW_CORNER_PREFERENCE*/, &corner, sizeof(corner));
    COLORREF border = dark ? RGB(70, 70, 72) : RGB(200, 200, 204);
    DwmSetWindowAttribute(h, 34 /*DWMWA_BORDER_COLOR*/, &border, sizeof(border));

    // Desfoque acrílico atrás da janela (API não documentada, usada por Seelen UI, YASB etc.).
    struct ACCENT_POLICY { int state, flags; DWORD gradient; int anim; };
    struct WCA_DATA { int attr; PVOID data; SIZE_T size; };
    using Fn = BOOL(WINAPI*)(HWND, WCA_DATA*);
    static Fn fn = (Fn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetWindowCompositionAttribute");
    if (!fn) return;
    ACCENT_POLICY ap{ 4 /*ACCENT_ENABLE_ACRYLICBLURBEHIND*/, 0, dark ? 0x01202020u : 0x01F2F2F2u, 0 };
    WCA_DATA d{ 19 /*WCA_ACCENT_POLICY*/, &ap, sizeof(ap) };
    fn(h, &d);
}

static float TextWidth(IDWriteTextFormat* tf, const std::wstring& s) {
    IDWriteTextLayout* tl = nullptr;
    if (FAILED(g_dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), tf, 10000, 100, &tl))) return 0;
    DWRITE_TEXT_METRICS tm{}; tl->GetMetrics(&tm); tl->Release();
    return tm.widthIncludingTrailingWhitespace;
}

static IDWriteTextFormat* MakeFormat(float size, DWRITE_FONT_WEIGHT w) {
    IDWriteTextFormat* tf = nullptr;
    g_dw->CreateTextFormat(g_font.c_str(), nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"pt-br", &tf);
    tf->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    IDWriteInlineObject* ell = nullptr;
    g_dw->CreateEllipsisTrimmingSign(tf, &ell);
    DWRITE_TRIMMING trim{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
    tf->SetTrimming(&trim, ell);
    if (ell) ell->Release();
    return tf;
}

static void DrawLevel(Session& s, int li);

// Abre um nível. Raiz: no ponteiro. Submenu: ao lado do item pai (parentWin/itemTop em pixels de tela).
static LARGE_INTEGER g_tShow;  // início do ctxmenu::Show / ShowItems (medição)

static void OpenLevel(Session& s, std::vector<MItem>* items, POINT anchor, const RECT* parentWin, int itemTop) {
    Level L; L.items = items;
    HMONITOR mon = MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST);
    UINT dx = 96, dy = 96; GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    float sc = dx / 96.f; L.scale = sc;
    MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(mon, &mi);
    RECT wa = mi.rcWork;

    L.tf = MakeFormat(M::FONT * sc, DWRITE_FONT_WEIGHT_NORMAL);
    L.tfHdr = MakeFormat(M::HDR_FONT * sc, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    if (Tahoe() && !g_plain)
        for (auto& m : *items) if (!m.sep && !m.header && !m.icon && m.dots.empty() && (!Golden() || GoldenIcon(m.label))) m.icon = ctxmenu::AutoIcon(m.label);
    bool hasCheck = false, hasSub = false, hasIcon = false;
    float maxW = 0, maxShort = 0;
    for (auto& m : *items) {
        hasCheck |= m.checked; hasSub |= HasSub(m); hasIcon |= m.icon != 0;
        if (!m.sep) maxW = std::max(maxW, TextWidth(m.header ? L.tfHdr : L.tf, m.label));
        if (!m.dots.empty()) maxW = std::max(maxW, (m.dots.size() * DOT_PITCH + 4) * sc);
        if (!m.shortcut.empty()) maxShort = std::max(maxShort, TextWidth(L.tf, m.shortcut));
    }
    if (maxShort > 0) maxW += maxShort + 28 * sc;  // atalhos alinhados à direita, como na barra de menus do Mac
    if (hasIcon) {  // ícones do Tahoe: coluna de ícones à esquerda, texto depois dela
        g_dw->CreateTextFormat(L"Segoe Fluent Icons", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13 * sc, L"", &L.tfIcon);
        if (L.tfIcon) { L.tfIcon->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); L.tfIcon->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); }
    }
    L.textL = (hasIcon ? (Golden() ? 41.f : Tahoe() ? 33.f : ICON_TEXT_L) : hasCheck ? 26.f : Golden() ? 16.f : Tahoe() ? 14.f : 16.f) * sc;
    L.textR = (hasSub ? 34.f : Tahoe() ? 8.f + 6.f : 18.f) * sc;  // Tahoe: o atalho termina 8 antes da borda (+ folga do texto)
    float W = L.textL + ceilf(maxW) + 2 * sc + L.textR;
    W = std::clamp(W, M::MIN_W * sc, M::MAX_W * sc);
    float y = PadY() * sc;
    for (auto& m : *items) {
        float h = (m.sep ? SepH() : m.header ? M::HDR_H : !m.dots.empty() ? DOT_ROW_H : ItemH()) * sc;
        L.rects.push_back(D2D1::RectF(0, y, W, y + h));
        y += h;
    }
    int w = (int)ceilf(W), h = (int)ceilf(y + PadY() * sc);
    h = std::min<int>(h, wa.bottom - wa.top);

    int x, top;
    if (!parentWin) {
        x = anchor.x + 1; top = anchor.y + 1;
        if (x + w > wa.right) x = anchor.x - w;
        if (top + h > wa.bottom) top = anchor.y - h;
    } else {
        x = parentWin->right - (int)(3 * sc);
        top = itemTop - (int)(PadY() * sc);
        if (x + w > wa.right) x = parentWin->left - w + (int)(3 * sc);
        if (top + h > wa.bottom) top = wa.bottom - h;
    }
    x = std::max<int>(x, wa.left); top = std::max<int>(top, wa.top);

    L.comp = CompReady();
    L.margin = L.comp ? (int)ceilf(24 * sc) : 0;
    L.mw = (float)w; L.mh = (float)h;
    int m = L.margin;
    L.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | (L.comp ? WS_EX_NOREDIRECTIONBITMAP : 0),
                             L.comp ? L"MenuTahoePopup" : L"MenuTestePopup", L"",
                             WS_POPUP, x - m, top - m, w + 2 * m, h + 2 * m, s.owner, nullptr, g_inst, nullptr);
    if (L.comp && !SetupComp(L)) {  // não deu: janela do jeito antigo
        DestroyWindow(L.hwnd);
        L.comp = false; L.margin = 0; L.target = nullptr; L.root = nullptr; L.contentS = nullptr; L.shadowS = nullptr;
        L.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, L"MenuTestePopup", L"",
                                 WS_POPUP, x, top, w, h, s.owner, nullptr, g_inst, nullptr);
    }
    if (!L.comp) ApplyEffects(L.hwnd, s.dark);  // antes de mostrar: aplicar depois deixava o menu em branco às vezes
    s.levels.push_back(L);
    if (L.comp) DrawLevel(s, (int)s.levels.size() - 1);  // desenha antes de aparecer
    ShowWindow(L.hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(L.hwnd);
    SetTimer(L.hwnd, 2, 30, nullptr);  // segunda pintura de garantia (ver DrawLevel)
    if (s.levels.size() == 1 && g_tShow.QuadPart) {  // tempo do clique até o menu na tela
        LARGE_INTEGER f, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t);
        Log(L"  menu na tela em %d ms (%ls)", (int)((t.QuadPart - g_tShow.QuadPart) * 1000 / f.QuadPart), L.comp ? L"vidro" : L"simples");
        g_tShow.QuadPart = 0;
    }
    if (g_demo) Log(L"  abriu nível em %d,%d %dx%d", x, top, w, h);
}

static void CloseLevelsAfter(Session& s, int li) {
    while ((int)s.levels.size() > li + 1) {
        Level& L = s.levels.back();
        if (L.rt) L.rt->Release();
        if (L.mem) DeleteDC(L.mem);
        if (L.dib) DeleteObject(L.dib);
        if (L.tf) L.tf->Release();
        if (L.tfHdr) L.tfHdr->Release();
        if (L.tfIcon) L.tfIcon->Release();
        DestroyWindow(L.hwnd);
        s.levels.pop_back();
    }
    if (li >= 0 && li < (int)s.levels.size()) { s.levels[li].openChild = -1; Invalidate(s.levels[li]); }
}

static void OpenSub(Session& s, int li, int i) {
    Level& L = s.levels[li];
    MItem& it = (*L.items)[i];
    if (it.src && g_active) {  // relê: "Enviar para" pode ter ganhado itens depois da abertura
        auto fresh = Flatten(ReadMenuItems(*g_active, it.src, 1, nullptr));
        if (!fresh.empty()) it.sub = std::move(fresh);
    }
    if (it.sub.empty() || it.disabled) return;
    RECT wr = MenuRect(L);
    int itemTop = wr.top + (int)L.rects[i].top;
    L.openChild = i;
    Invalidate(L);
    OpenLevel(s, &it.sub, POINT{ wr.right, itemTop }, &wr, itemTop);  // L pode ser invalidado aqui
}

static void CancelPending(Session& s) {
    if (s.pendLevel >= 0 && !s.levels.empty()) KillTimer(s.levels[0].hwnd, 1);
    s.pendLevel = -1;
}

static void Schedule(Session& s, int li, int item, UINT ms) {
    if (s.pendLevel == li && s.pendItem == item) return;
    s.pendLevel = li; s.pendItem = item;
    SetTimer(s.levels[0].hwnd, 1, ms, nullptr);
}

static void OnTimer(Session& s) {
    KillTimer(s.levels[0].hwnd, 1);
    int pl = s.pendLevel, pi = s.pendItem;
    s.pendLevel = -1;
    if (pl < 0 || pl >= (int)s.levels.size()) return;
    CloseLevelsAfter(s, pl);
    if (pi >= 0) OpenSub(s, pl, pi);
}

static int HitItem(Level& L, POINT screen) {
    RECT wr = MenuRect(L);
    if (!PtInRect(&wr, screen)) return -2;
    float y = (float)(screen.y - wr.top);
    for (size_t i = 0; i < L.rects.size(); i++) if (y >= L.rects[i].top && y < L.rects[i].bottom) return (int)i;
    return -1;
}

static void OnMouseMove(Session& s, int li, POINT pt) {
    Level& L = s.levels[li];
    int i = HitItem(L, pt);
    {   // fileira de bolinhas: destaca a bolinha, não a linha
        RECT wr = MenuRect(L);
        int d = i >= 0 && !(*L.items)[i].dots.empty() ? DotAt(L, (*L.items)[i], (float)(pt.x - wr.left)) : -1;
        if (d != L.dotHover) { L.dotHover = d; Invalidate(L); }
    }
    if (i >= 0 && !Selectable((*L.items)[i])) i = -1;
    for (int k = 0; k < li; k++)  // chegou no submenu: o item pai continua destacado
        if (s.levels[k].hover != s.levels[k].openChild) { s.levels[k].hover = s.levels[k].openChild; Invalidate(s.levels[k]); }
    if (s.pendLevel >= 0 && s.pendLevel < li) CancelPending(s);
    if (i != L.hover) { L.hover = i; Invalidate(L); }
    if (i < 0) return;
    MItem& it = (*L.items)[i];
    if (HasSub(it)) {
        if (L.openChild == i) { CancelPending(s); return; }
        Schedule(s, li, i, 140);
    } else if ((int)s.levels.size() > li + 1) {
        Schedule(s, li, -1, 260);
    } else {
        CancelPending(s);
    }
}

static void OnButtonUp(Session& s, int li, POINT pt) {
    Level& L = s.levels[li];
    int i = HitItem(L, pt);
    Log(L"  clique solto no nível %d, item %d", li, i);
    if (i < 0) return;
    MItem& it = (*L.items)[i];
    if (!it.dots.empty()) {  // clicou numa bolinha: devolve o id dela
        RECT wr = MenuRect(L);
        int d = DotAt(L, it, (float)(pt.x - wr.left));
        if (d < 0) return;
        it.cmd = (UINT)it.dots[d].id;
        s.result = &it; s.done = true;
        return;
    }
    if (!Selectable(it)) return;
    if (HasSub(it)) {
        CancelPending(s);
        if (L.openChild != i) { CloseLevelsAfter(s, li); OpenSub(s, li, i); }
        return;
    }
    s.result = &it;
    s.done = true;
}

static void OnKey(Session& s, UINT vk) {
    CancelPending(s);
    int li = (int)s.levels.size() - 1;
    Level& L = s.levels[li];
    int n = (int)L.items->size();
    auto move = [&](int dir) {
        int i = L.hover < 0 ? (dir > 0 ? -1 : 0) : L.hover;
        for (int k = 0; k < n; k++) {
            i = (i + dir + n) % n;
            if (Selectable((*L.items)[i])) { L.hover = i; break; }
        }
        Invalidate(L);
    };
    switch (vk) {
    case VK_ESCAPE: if (li > 0) CloseLevelsAfter(s, li - 1); else s.done = true; break;
    case VK_LEFT: if (li > 0) CloseLevelsAfter(s, li - 1); break;
    case VK_DOWN: move(+1); break;
    case VK_UP: move(-1); break;
    case VK_RIGHT: case VK_RETURN: case VK_SPACE:
        if (L.hover >= 0) {
            MItem& it = (*L.items)[L.hover];
            if (HasSub(it)) {
                OpenSub(s, li, L.hover);
                Level& c = s.levels.back();
                for (size_t k = 0; k < c.items->size(); k++) if (Selectable((*c.items)[k])) { c.hover = (int)k; break; }
                Invalidate(c);
            } else if (vk != VK_RIGHT && Selectable(it)) {
                s.result = &it; s.done = true;
            }
        }
        break;
    }
}

static void DrawLevel(Session& s, int li) {
    Level& L = s.levels[li];
    RECT rc; GetClientRect(L.hwnd, &rc);
    ID2D1RenderTarget* rt = nullptr;
    ID2D1DeviceContext* cdc = nullptr;
    winrt::com_ptr<ABI::Windows::UI::Composition::ICompositionDrawingSurfaceInterop> csi;
    if (L.comp) {
        csi = L.contentS.as<ABI::Windows::UI::Composition::ICompositionDrawingSurfaceInterop>();
        POINT off{};
        if (FAILED(csi->BeginDraw(nullptr, __uuidof(ID2D1DeviceContext), (void**)&cdc, &off)) || !cdc) return;
        cdc->SetTransform(D2D1::Matrix3x2F::Translation((float)off.x, (float)off.y));
        cdc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        cdc->PushAxisAlignedClip(D2D1::RectF(0, 0, L.mw, L.mh), D2D1_ANTIALIAS_MODE_ALIASED);
        rt = cdc;
    }
    if (!L.comp && !L.mem) {
        BITMAPINFO bi{};
        bi.bmiHeader = { sizeof(BITMAPINFOHEADER), rc.right, -rc.bottom, 1, 32, BI_RGB };
        void* bits;
        L.dib = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        L.mem = CreateCompatibleDC(nullptr);
        SelectObject(L.mem, L.dib);
    }
    if (!L.comp && !L.rt) {
        auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        if (FAILED(g_d2d->CreateDCRenderTarget(&props, &L.rt))) return;
        L.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    if (!L.comp) { rt = L.rt; static_cast<ID2D1DCRenderTarget*>(rt)->BindDC(L.mem, &rc); }
    const float sc = L.scale;
    const bool dk = s.dark;
    D2D1_COLOR_F bg = dk ? D2D1::ColorF(0.17f, 0.17f, 0.18f, 0.80f) : D2D1::ColorF(0.965f, 0.965f, 0.965f, 0.80f);
    // Tahoe: o vidro "regular" da Apple (menus, avisos): tingido e bem menos transparente que o Dock, para o texto
    // Guia (seções 52 e 60, medido na tela): escuro = 0,15 × fundo + 0,14 (o preto vira 0,14, o branco 0,29);
    // claro = 0,23 × fundo + 0,74. Em alfa sobre o desfoque: escuro 85 % de 0,165; claro 77 % de 0,96.
    if (Tahoe()) bg = dk ? D2D1::ColorF(0.165f, 0.165f, 0.172f, 0.85f) : D2D1::ColorF(0.96f, 0.96f, 0.965f, 0.77f);
    if (g_glass.alpha) bg.a = g_glass.alpha(bg.a);
    // macOS 27 Golden Gate: o menu de contexto é vidro fosco (leitoso), não o Liquid Glass transparente do Dock
    if (Golden()) { bg = dk ? D2D1::ColorF(0.17f, 0.165f, 0.165f, 0.88f) : D2D1::ColorF(0.95f, 0.945f, 0.94f, 0.84f); }
    const bool leo = Leopard();
    if (leo) bg = D2D1::ColorF(1, 1, 1, 0.94f);  // Leopard: sempre claro (o 10.5 não tinha modo escuro)
    ID2D1SolidColorBrush *text, *dim, *sep, *hdr, *accent, *white;
    const bool dkT = dk && !leo;  // cores do texto: o Leopard é sempre claro
    rt->CreateSolidColorBrush(dkT ? D2D1::ColorF(1, 1, 1, 0.92f) : D2D1::ColorF(0, 0, 0, leo ? 1.f : 0.88f), &text);
    rt->CreateSolidColorBrush(dkT ? D2D1::ColorF(1, 1, 1, 0.30f) : D2D1::ColorF(0, 0, 0, 0.30f), &dim);
    rt->CreateSolidColorBrush(dkT ? D2D1::ColorF(1, 1, 1, 0.12f) : D2D1::ColorF(0, 0, 0, leo ? 0.14f : 0.10f), &sep);
    rt->CreateSolidColorBrush(dkT ? D2D1::ColorF(1, 1, 1, 0.45f) : D2D1::ColorF(0, 0, 0, 0.45f), &hdr);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.39f, 0.88f, 1.0f), &accent);
    rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &white);

    if (!L.comp) rt->BeginDraw();
    rt->Clear(bg);
    float W = L.rects.empty() ? 0 : L.rects[0].right;
    if (Tahoe()) {  // Liquid Glass: borda escurecida e brilho especular nos cantos (comum/vidro.h)
        D2D1_SIZE_F sz = L.comp ? D2D1::SizeF(L.mw, L.mh) : rt->GetSize();
        vidro::Borda(rt, D2D1::RectF(0, 0, sz.width, sz.height), (L.comp ? CARD_R : 8) * sc, sc, dk);
    }
    for (size_t i = 0; i < L.items->size(); i++) {
        const MItem& m = (*L.items)[i];
        D2D1_RECT_F r = L.rects[i];
        if (m.sep) {
            float yy = floorf((r.top + r.bottom) / 2) + 0.5f;
            rt->DrawLine(D2D1::Point2F(Inset() * sc + 5 * sc, yy), D2D1::Point2F(W - Inset() * sc - 5 * sc, yy), sep, 1.0f);
            continue;
        }
        D2D1_RECT_F tr = D2D1::RectF(L.textL, r.top, W - L.textR, r.bottom);
        if (!m.dots.empty()) {  // fileira de etiquetas: bolinhas coloridas, ✓ nas marcadas, anel na do mouse
            float cy = (r.top + r.bottom) / 2, rad = DOT_D / 2 * sc;
            for (size_t k = 0; k < m.dots.size(); k++) {
                float cx = L.textL + (DOT_D / 2 + 1.5f + k * DOT_PITCH) * sc;
                UINT32 c = m.dots[k].rgb;
                ID2D1SolidColorBrush* b = nullptr;
                rt->CreateSolidColorBrush(D2D1::ColorF(((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f, 1), &b);
                if (!b) continue;
                rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad, rad), b);
                if ((int)k == L.dotHover) {
                    rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), rad + 2.5f * sc, rad + 2.5f * sc), b, 1.3f * sc);
                }
                if (m.dots[k].on) {
                    rt->DrawLine(D2D1::Point2F(cx - 2.6f * sc, cy), D2D1::Point2F(cx - 0.6f * sc, cy + 2.2f * sc), white, 1.4f * sc);
                    rt->DrawLine(D2D1::Point2F(cx - 0.6f * sc, cy + 2.2f * sc), D2D1::Point2F(cx + 2.8f * sc, cy - 2.4f * sc), white, 1.4f * sc);
                }
                b->Release();
            }
            continue;
        }
        if (m.header) {
            rt->DrawTextW(m.label.c_str(), (UINT32)m.label.size(), L.tfHdr, tr, hdr, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            continue;
        }
        bool hot = (int)i == L.hover || ((int)i == L.openChild && L.hover < 0);
        if (hot && !m.disabled && leo) {  // Leopard: degradê azul de ponta a ponta, com o fio claro em cima
            D2D1_GRADIENT_STOP st[2] = { { 0, D2D1::ColorF(0.39f, 0.56f, 0.96f, 1) }, { 1, D2D1::ColorF(0.13f, 0.35f, 0.88f, 1) } };
            ID2D1GradientStopCollection* gs = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
            if (SUCCEEDED(rt->CreateGradientStopCollection(st, 2, &gs)) &&
                SUCCEEDED(rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, r.top }, { 0, r.bottom }), gs, &lb)))
                rt->FillRectangle(D2D1::RectF(0, r.top, W, r.bottom), lb);
            if (lb) lb->Release();
            if (gs) gs->Release();
            white->SetOpacity(0.35f);
            rt->FillRectangle(D2D1::RectF(0, r.top, W, r.top + sc), white);
            white->SetOpacity(1);
        } else
        if (hot && !m.disabled) {
            D2D1_ROUNDED_RECT rr{ D2D1::RectF(Inset() * sc, r.top, W - Inset() * sc, r.bottom), HotR() * sc, HotR() * sc };
            rt->FillRoundedRectangle(rr, accent);
        }
        ID2D1SolidColorBrush* fg = m.disabled ? dim : (hot ? white : text);
        rt->DrawTextW(m.label.c_str(), (UINT32)m.label.size(), L.tf, tr, fg, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        if (!m.shortcut.empty()) {
            float sw = TextWidth(L.tf, m.shortcut);
            D2D1_RECT_F sr = D2D1::RectF(W - L.textR - sw, r.top, W - L.textR + 2 * sc, r.bottom);
            rt->DrawTextW(m.shortcut.c_str(), (UINT32)m.shortcut.size(), L.tf, sr, (hot && !m.disabled) ? white : dim, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        float cy = (r.top + r.bottom) / 2;
        if (m.icon && !m.checked && L.tfIcon) {  // ícone do Tahoe na coluna da esquerda
            wchar_t g[2] = { m.icon, 0 };
            rt->DrawTextW(g, 1, L.tfIcon, D2D1::RectF((IconCx() - 10) * sc, r.top, (IconCx() + 10) * sc, r.bottom), fg);
        }
        if (m.checked) {
            float x = M::INSET * sc + 7 * sc;
            rt->DrawLine(D2D1::Point2F(x, cy), D2D1::Point2F(x + 3.5f * sc, cy + 3.5f * sc), fg, 1.6f * sc);
            rt->DrawLine(D2D1::Point2F(x + 3.5f * sc, cy + 3.5f * sc), D2D1::Point2F(x + 10 * sc, cy - 4 * sc), fg, 1.6f * sc);
        }
        if (HasSub(m)) {  // seta ›
            float x = W - M::INSET * sc - 12 * sc;
            rt->DrawLine(D2D1::Point2F(x, cy - 4 * sc), D2D1::Point2F(x + 4 * sc, cy), fg, 1.5f * sc);
            rt->DrawLine(D2D1::Point2F(x + 4 * sc, cy), D2D1::Point2F(x, cy + 4 * sc), fg, 1.5f * sc);
        }
    }
    if (L.comp && !Tahoe()) {  // contorno fino no canto arredondado (o DWM fazia isso na janela)
        float in = 0.5f, rad = CARD_R * sc - in;
        rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(in, in, L.mw - in, L.mh - in), rad, rad), sep, 1.0f);
    }
    if (L.comp) {
        for (auto* b : { text, dim, sep, hdr, accent, white }) b->Release();
        cdc->PopAxisAlignedClip();
        cdc->Release();
        csi->EndDraw();
        return;
    }
    HRESULT hr = rt->EndDraw();
    for (auto* b : { text, dim, sep, hdr, accent, white }) b->Release();
    if (FAILED(hr)) {
        Log(L"  aviso: EndDraw falhou (hr=0x%08X), repintando", (unsigned)hr);
        L.rt->Release(); L.rt = nullptr;
        SetTimer(L.hwnd, 2, 16, nullptr);
        return;
    }
    // Cópia direta para a janela: os bytes de alfa vão junto e o DWM compõe sobre o acrílico.
    HDC dc = GetDC(L.hwnd);
    BOOL ok = BitBlt(dc, 0, 0, rc.right, rc.bottom, L.mem, 0, 0, SRCCOPY);
    ReleaseDC(L.hwnd, dc);
    if (g_demo) {
        DIBSECTION ds{}; GetObjectW(L.dib, sizeof(ds), &ds);
        DWORD px = ((DWORD*)ds.dsBm.bmBits)[rc.right * 10 + 20];
        Log(L"  pintura nível %d %dx%d blt=%d pixel=0x%08X visível=%d", li, rc.right, rc.bottom, ok, px, IsWindowVisible(L.hwnd));
    }
}

// Animação de saída no estilo macOS: o item escolhido pisca uma vez e o menu some em fade.
static int g_fadeMs = 160;

static void Blink(Session& s) {
    for (int li = (int)s.levels.size() - 1; li >= 0; li--) {
        Level& L = s.levels[li];
        for (size_t k = 0; k < L.items->size(); k++) {
            if (&(*L.items)[k] != s.result) continue;
            L.hover = -1; DrawLevel(s, li); PumpFor(50);
            L.hover = (int)k; DrawLevel(s, li); PumpFor(50);
            return;
        }
    }
}

// Menus que estão sumindo sozinhos: a ação escolhida já roda e o programa já responde a cliques enquanto o
// fade acontece (antes tudo esperava ~180 ms parado, e um clique nesse tempo era jogado fora).
static std::vector<Level> g_dying;
static const UINT_PTR T_DYING = 3;

static void FreeLevel(Level& L) {
    if (L.rt) L.rt->Release();
    if (L.mem) DeleteDC(L.mem);
    if (L.dib) DeleteObject(L.dib);
    if (L.tf) L.tf->Release();
    if (L.tfHdr) L.tfHdr->Release();
    if (L.tfIcon) L.tfIcon->Release();
    L.target = nullptr; L.root = nullptr; L.contentS = nullptr; L.shadowS = nullptr;
    if (IsWindow(L.hwnd)) DestroyWindow(L.hwnd);
}

static bool IsDying(HWND h) {
    for (auto& L : g_dying) if (L.hwnd == h) return true;
    return false;
}

static void KillDying(HWND h) {
    for (size_t i = 0; i < g_dying.size(); i++)
        if (g_dying[i].hwnd == h) { Level L = g_dying[i]; g_dying.erase(g_dying.begin() + i); FreeLevel(L); return; }
}

// true: os níveis passaram para g_dying e somem sozinhos (só com composição)
static bool FadeOutAsync(Session& s) {
    if (s.levels.empty() || !s.levels[0].comp) return false;
    for (size_t i = 0; i < g_dying.size();)  // a janela dona fechou antes do fim: o Windows já destruiu o menu
        if (!IsWindow(g_dying[i].hwnd)) { Level L = g_dying[i]; g_dying.erase(g_dying.begin() + i); FreeLevel(L); }
        else i++;
    try {
        for (auto& L : s.levels) {
            auto a = g_wc.CreateScalarKeyFrameAnimation();
            a.InsertKeyFrame(1.f, 0.f, g_wc.CreateCubicBezierEasingFunction({ 0.3f, 0.f }, { 0.6f, 1.f }));
            a.Duration(std::chrono::milliseconds(g_fadeMs));
            L.root.StartAnimation(L"Opacity", a);
            SetWindowLongPtrW(L.hwnd, GWL_EXSTYLE, GetWindowLongPtrW(L.hwnd, GWL_EXSTYLE) | WS_EX_TRANSPARENT);  // cliques passam
            SetTimer(L.hwnd, T_DYING, g_fadeMs + 30, nullptr);
            g_dying.push_back(L);
        }
    } catch (...) { return false; }
    s.levels.clear();  // os recursos agora são de g_dying
    return true;
}

static void FadeOut(Session& s) {
    if (!s.levels.empty() && s.levels[0].comp) {  // composição: a opacidade anima sozinha
        try {
            for (auto& L : s.levels) {
                auto a = g_wc.CreateScalarKeyFrameAnimation();
                a.InsertKeyFrame(1.f, 0.f, g_wc.CreateCubicBezierEasingFunction({ 0.3f, 0.f }, { 0.6f, 1.f }));
                a.Duration(std::chrono::milliseconds(g_fadeMs));
                L.root.StartAnimation(L"Opacity", a);
            }
            PumpFor(g_fadeMs + 16);
        } catch (...) {}
        return;
    }
    for (auto& L : s.levels) {
        SetWindowLongPtrW(L.hwnd, GWL_EXSTYLE, GetWindowLongPtrW(L.hwnd, GWL_EXSTYLE) | WS_EX_LAYERED);
        SetLayeredWindowAttributes(L.hwnd, 0, 255, LWA_ALPHA);
    }
    LARGE_INTEGER f, t0, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    for (;;) {
        QueryPerformanceCounter(&t);
        double p = (double)(t.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart / g_fadeMs;
        if (p >= 1) break;
        BYTE a = (BYTE)(255 * (1 - p) * (1 - p));  // desacelera no fim, como no macOS
        for (auto& L : s.levels) SetLayeredWindowAttributes(L.hwnd, 0, a, LWA_ALPHA);
        DwmFlush();
    }
}

static LRESULT CALLBACK PopupProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_TIMER && w == T_DYING) { KillTimer(h, T_DYING); KillDying(h); return 0; }
    if (m == WM_NCHITTEST && IsDying(h)) return HTTRANSPARENT;
    if (m == WM_PAINT && IsDying(h)) { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); return 0; }
    Session* s = g_ses;
    int li = FindLevel(h);
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps; BeginPaint(h, &ps);
        if (s && li >= 0) DrawLevel(*s, li);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_NCHITTEST:
        if (s && li >= 0 && s->levels[li].comp) {
            POINT pt{ (short)LOWORD(l), (short)HIWORD(l) };
            RECT r = MenuRect(s->levels[li]);
            if (!PtInRect(&r, pt)) return HTTRANSPARENT;
        }
        break;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_MOUSEMOVE: {
        if (!s || li < 0) break;
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 }; TrackMouseEvent(&tme);
        POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ClientToScreen(h, &pt);
        OnMouseMove(*s, li, pt);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (s && li >= 0) { Level& L = s->levels[li]; if (L.openChild < 0 && L.hover >= 0) { L.hover = -1; Invalidate(L); } }
        return 0;
    case WM_LBUTTONUP: case WM_RBUTTONUP: {
        if (!s || li < 0) break;
        POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ClientToScreen(h, &pt);
        OnButtonUp(*s, li, pt);
        return 0;
    }
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: return 0;
    case WM_TIMER:
        if (w == 2) { KillTimer(h, 2); InvalidateRect(h, nullptr, FALSE); }
        else if (s && li == 0) OnTimer(*s);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// Mostra o menu e devolve o item escolhido (ou nullptr). Loop modal próprio, como o TrackPopupMenu.
static MItem* RunMenu(HWND owner, std::vector<MItem>& items, POINT pt) {
    Session s; s.owner = owner; s.dark = IsDarkMode() && !Leopard();  // Leopard: menus sempre claros
    g_plain = g_plainNext; g_plainNext = false;
    g_ses = &s;
    OpenLevel(s, &items, pt, nullptr, 0);
    if (g_demo) { s.levels[0].hover = 2; Invalidate(s.levels[0]); }
    MSG msg;
    while (!s.done) {
        if (GetForegroundWindow() != owner && !g_demo) break;
        if (!GetMessageW(&msg, nullptr, 0, 0)) { PostQuitMessage((int)msg.wParam); break; }
        bool onPopup = FindLevel(msg.hwnd) >= 0;
        switch (msg.message) {
        case WM_KEYDOWN: case WM_SYSKEYDOWN: OnKey(s, (UINT)msg.wParam); continue;
        case WM_KEYUP: case WM_SYSKEYUP: case WM_CHAR: case WM_SYSCHAR: continue;
        case WM_RBUTTONDOWN:
            // botão direito em outro ponto da mesma janela: fecha este e deixa o clique seguir,
            // para a janela abrir o menu do novo item (como no macOS)
            if (!onPopup && (msg.hwnd == owner || IsChild(owner, msg.hwnd))) { s.done = true; break; }
            if (!onPopup) { s.done = true; continue; }
            break;
        case WM_LBUTTONDOWN: case WM_MBUTTONDOWN:
        case WM_NCLBUTTONDOWN: case WM_NCRBUTTONDOWN:
            if (!onPopup) {
                // clique na barra de menus (outro título, relógio, Central...): fecha este e o clique segue para a barra,
                // que abre o próximo — antes o clique era consumido e era preciso clicar duas vezes
                wchar_t cls[64] = {}; GetClassNameW(msg.hwnd, cls, 64);
                s.done = true;
                if (!wcscmp(cls, L"ShellBarraDeMenus")) PostMessageW(msg.hwnd, msg.message, msg.wParam, msg.lParam);  // a barra recebe depois que este fechar
                continue;  // clique fora fecha e é consumido, como no macOS
            }
            break;
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CancelPending(s);
    Log(L"  menu fechado: %ls", s.result ? (L"escolheu \"" + s.result->label + L"\" id=" + std::to_wstring(s.result->cmd)).c_str()
                                         : (s.done ? L"cancelado (Esc ou clique fora)" : L"perdeu o foco"));
    if (s.result) Blink(s);
    if (g_demo || !FadeOutAsync(s)) FadeOut(s);  // a bancada (--demo) ainda espera, para a captura
    CloseLevelsAfter(s, -1);
    g_ses = nullptr;
    return s.result;
}

// ---------------------------------------------------------------------------
// API pública (context_menu.h)

// Lista completa na ordem do Windows, já no visual próprio: só achata submenus para um nível.
static std::vector<MItem> FullList(const std::vector<MItem>& raw) {
    std::vector<MItem> out;
    for (auto& m : raw) {
        out.push_back(m);
        if (!m.sub.empty()) out.back().sub = Flatten(m.sub);
    }
    CleanSeps(out);
    return out;
}

static HRESULT InvokeCmd(HWND h, ShellMenu& sm, UINT cmd, POINT pt, const std::wstring& folder) {
    CMINVOKECOMMANDINFOEX ici{ sizeof(ici) };
    ici.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
    if (GetKeyState(VK_CONTROL) < 0) ici.fMask |= CMIC_MASK_CONTROL_DOWN;
    if (GetKeyState(VK_SHIFT) < 0) ici.fMask |= CMIC_MASK_SHIFT_DOWN;
    ici.hwnd = h;
    ici.lpVerb = MAKEINTRESOURCEA(cmd - ID_FIRST);
    ici.lpVerbW = MAKEINTRESOURCEW(cmd - ID_FIRST);
    ici.lpDirectoryW = folder.c_str();
    ici.nShow = SW_SHOWNORMAL;
    ici.ptInvoke = pt;
    return sm.cm->InvokeCommand((CMINVOKECOMMANDINFO*)&ici);
}

static void PickFont() {
    IDWriteFontCollection* fc = nullptr;
    if (FAILED(g_dw->GetSystemFontCollection(&fc))) return;
    for (const wchar_t* f : { L"SF Pro Text", L"Inter", L"Segoe UI Variable Text", L"Segoe UI" }) {  // SF Pro só se instalada
        UINT32 idx; BOOL exists = FALSE;
        fc->FindFamilyName(f, &idx, &exists);
        if (exists) { g_font = f; break; }
    }
    fc->Release();
}

void ctxmenu::Init(HINSTANCE inst, FILE* log, Options opt) {
    g_inst = inst;
    g_log = log;
    g_demo = opt.demo;
    g_fadeMs = opt.fadeMs;
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2d);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&g_dw);
    PickFont();
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = PopupProc; wc.hInstance = inst; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"MenuTestePopup"; wc.style = CS_DROPSHADOW;
    RegisterClassExW(&wc);
    wc.lpszClassName = L"MenuTahoePopup"; wc.style = 0;
    RegisterClassExW(&wc);
}

bool ctxmenu::ForwardOwnerMsg(UINT msg, WPARAM wp, LPARAM lp, LRESULT* res) {
    if (!ForwardMenuMsg(msg, wp, lp, res)) return false;
    if (msg == WM_DRAWITEM || msg == WM_MEASUREITEM) *res = TRUE;
    return true;
}

bool ctxmenu::IsOpen() { return g_ses != nullptr; }

void ctxmenu::Close() {
    if (!g_ses) return;
    g_ses->done = true;
    PostMessageW(g_ses->owner, WM_NULL, 0, 0);  // acorda o loop modal
}

// Carrega as extensões do menu uma vez (7-Zip, Git, NVIDIA...): o primeiro menu de verdade já sai rápido.
void ctxmenu::Warmup(HWND owner, const std::wstring& folder, const std::wstring& file) {
    LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
    { ShellMenu sm; if (LoadShellMenu(owner, folder, {}, CMF_NORMAL, sm)) { std::vector<HMENU> subs; InitSubmenus(sm.hmenu, 0, subs); } }
    if (!file.empty()) { ShellMenu sm; if (LoadShellMenu(owner, folder, { file }, CMF_NORMAL | CMF_CANRENAME, sm)) { std::vector<HMENU> subs; InitSubmenus(sm.hmenu, 0, subs); } }
    QueryPerformanceCounter(&b);
    Log(L"menus pré-carregados em %d ms", (int)((b.QuadPart - a.QuadPart) * 1000 / f.QuadPart));
}

// O mesmo, numa thread própria: as extensões (DLLs) ficam carregadas no processo sem travar a janela por
// ~0,4-0,9 s; o primeiro botão direito depois disso já sai em ~70 ms.
void ctxmenu::WarmupAsync(const std::wstring& folder, const std::wstring& file) {
    if (Tahoe()) {  // vidro do menu (D3D, D2D, composição): criado aqui, na thread da janela, e não no 1º menu
        LARGE_INTEGER f, a, b; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
        bool ok = CompReady();
        QueryPerformanceCounter(&b);
        Log(L"vidro do menu pronto em %d ms (%ls)", (int)((b.QuadPart - a.QuadPart) * 1000 / f.QuadPart), ok ? L"ok" : L"sem composição");
    }
    std::thread([folder, file] {
        if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return;
        HWND owner = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, g_inst, nullptr);
        Warmup(owner, folder, file);
        MSG msg;  // mensagens que as extensões postaram para a janela
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
        if (owner) DestroyWindow(owner);
        CoUninitialize();
    }).detach();
}

ctxmenu::Result ctxmenu::Show(HWND owner, POINT pt, const std::wstring& folder,
                              const std::vector<std::wstring>& paths, bool full) {
    Result res;
    bool bg = paths.empty();
    ShellMenu sm;
    UINT flags = CMF_NORMAL | (bg ? 0 : CMF_CANRENAME);
    // Alt = tecla Option do Mac: revela itens extras (PowerShell aqui, abrir em novo processo...)
    if (full || GetKeyState(VK_MENU) < 0) flags |= CMF_EXTENDEDVERBS;
    QueryPerformanceCounter(&g_tShow);
    LARGE_INTEGER qf, qa, qb; QueryPerformanceFrequency(&qf); QueryPerformanceCounter(&qa);
    if (!LoadShellMenu(owner, folder, paths, flags, sm)) { res.hr = E_FAIL; return res; }
    QueryPerformanceCounter(&qb);
    Log(L"  QueryContextMenu: %d ms", (int)((qb.QuadPart - qa.QuadPart) * 1000 / qf.QuadPart));
    g_active = &sm;
    int noText = 0;
    Log(L"--- menu de %ls (%zu itens)%ls%ls", bg ? folder.c_str() : paths[0].c_str(), paths.size(),
        bg ? L" (fundo)" : L"", full ? L" (todas as opções)" : L"");
    auto raw = ReadMenu(sm, sm.hmenu, 0, &noText);
    DumpTree(raw, 0);
    auto items = full ? FullList(raw) : Organize(raw);
    // itens do próprio app (ExtraNext): logo depois do primeiro grupo (Abrir / Abrir com)
    std::vector<Item> extra; extra.swap(g_extraNext);
    if (!extra.empty()) {
        std::vector<MItem> add;
        for (auto& it : extra) {
            MItem m; m.label = it.label; m.special = 100 + it.id; m.icon = it.icon; m.disabled = it.disabled; m.shortcut = it.shortcut;
            add.push_back(std::move(m));
        }
        size_t at = 0;
        while (at < items.size() && !items[at].sep) at++;
        if (at < items.size()) { add.push_back(Sep()); items.insert(items.begin() + at + 1, add.begin(), add.end()); }
        else { items.push_back(Sep()); items.insert(items.end(), add.begin(), add.end()); }
    }
    MItem* r = RunMenu(owner, items, pt);
    if (r && r->special >= 100) { res.chosen = true; res.extra = r->special - 100; g_active = nullptr; return res; }
    bool showAll = r && r->special == 1;
    if (r && r->cmd >= ID_FIRST && r->cmd <= ID_LAST) {
        res.chosen = true;
        res.verb = VerbOf(sm, r->cmd);
        if (res.verb == L"rename" && !bg) res.rename = true;
        else res.hr = InvokeCmd(owner, sm, r->cmd, pt, folder);
        Log(L"Executado: %ls (id %u) → %ls 0x%08X", res.verb.empty() ? L"(sem verbo)" : res.verb.c_str(), r->cmd,
            SUCCEEDED(res.hr) ? L"OK" : L"erro", (unsigned)res.hr);
    }
    g_active = nullptr;
    if (showAll) return Show(owner, pt, folder, paths, true);  // menu novo: há extensões que só preenchem submenus uma vez
    return res;
}

bool ctxmenu::DumpToLog(HWND owner, const std::wstring& path, bool background) {
    ShellMenu sm;
    std::vector<std::wstring> paths;
    if (!background) paths.push_back(path);
    if (!LoadShellMenu(owner, path, paths, CMF_NORMAL | (background ? 0 : CMF_CANRENAME), sm)) return false;
    g_active = &sm;
    int noText = 0;
    Log(L"--- dump de %ls%ls", path.c_str(), background ? L" (fundo)" : L"");
    auto raw = ReadMenu(sm, sm.hmenu, 0, &noText);
    Log(L"== original (%zu no topo, %d sem texto) ==", raw.size(), noText);
    DumpTree(raw, 0);
    Log(L"== organizado ==");
    DumpTree(Organize(raw), 0);
    g_active = nullptr;
    return true;
}

HRESULT ctxmenu::InvokeVerb(HWND owner, const std::wstring& folder, const std::vector<std::wstring>& paths, const char* verb) {
    ShellMenu sm;
    if (!LoadShellMenu(owner, folder, paths, CMF_NORMAL | CMF_CANRENAME, sm)) return E_FAIL;
    std::wstring wverb(verb, verb + strlen(verb));
    CMINVOKECOMMANDINFOEX ici{ sizeof(ici) };
    ici.fMask = CMIC_MASK_UNICODE;
    ici.hwnd = owner;
    ici.lpVerb = verb;
    ici.lpVerbW = wverb.c_str();
    ici.lpDirectoryW = folder.c_str();
    ici.nShow = SW_SHOWNORMAL;
    HRESULT hr = sm.cm->InvokeCommand((CMINVOKECOMMANDINFO*)&ici);
    Log(L"Executado (atalho): %ls → 0x%08X", wverb.c_str(), (unsigned)hr);
    return hr;
}

static std::vector<MItem> ToMItems(const std::vector<ctxmenu::Item>& in) {
    std::vector<MItem> out;
    for (auto& it : in) {
        if (it.separator) { out.push_back(Sep()); continue; }
        MItem m;
        m.label = it.label;
        m.cmd = (UINT)it.id;
        m.checked = it.checked;
        m.disabled = it.disabled;
        m.shortcut = it.shortcut;
        m.icon = it.icon;
        m.dots = it.dots;
        m.sub = ToMItems(it.sub);
        out.push_back(std::move(m));
    }
    return out;
}

int ctxmenu::ShowItems(HWND owner, POINT pt, const std::vector<Item>& items) {
    QueryPerformanceCounter(&g_tShow);
    auto mitems = ToMItems(items);
    MItem* r = RunMenu(owner, mitems, pt);
    return r ? (int)r->cmd : 0;
}
