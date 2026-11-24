// Janela de progresso do Finder (copiar, mover, apagar, expandir...) no lugar da do Windows.
// A "OperationStatusWindow" ("60% complete", gráfico verde de velocidade, "Fewer details") aparece em
// qualquer programa que use as operações de arquivo do Windows: o Explorer, o nosso Finder (colar,
// soltar, duplicar, comprimir), extrair .zip, a Lixeira... O Shell faz como em alertas.cpp, por fora:
// quando ela aparece, sai da tela, uma thread lê os blocos dela pela UI Automation (eltTile1..N:
// texto "Copying N items from X to Y", "44% complete", tempo e itens restantes, botões de pausa e de
// cancelar) e nós desenhamos a janela do Mac, todas as operações empilhadas numa só, com o (x) de
// parar e a pausa. Os botões apertam os da janela original (Invoke). Fica sempre no topo: sem dono no
// Dock, atrás de outra janela ela se perderia.
// Perguntas no meio da operação (substituir ou pular, juntar pastas, acesso negado...) aparecem no
// próprio bloco do Windows ("Interrupted", botões eltInterrupt...): aí a janela original volta para a
// tela até a pergunta ser respondida, e depois sai de novo. `progresso_mac=0` no ajustes.txt desliga.
#include "shell.h"
#include <UIAutomation.h>
#include <dwmapi.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include "../comum/idioma.h"
#include "../comum/semaforo.h"


namespace {

const UINT WM_PG_SHOWN = WM_APP + 90, WM_PG_SNAP = WM_APP + 91, WM_PG_FRAME = WM_APP + 92;
// macOS 27 Golden Gate (Finder-Copy.png, 2x): janela opaca de 404 de largura, sem título, botões da janela de 14
// centrados em (16, 16) a cada 23, cantos 16; cada cópia em 56: documento de 32 a 22 da borda, texto a 62, barra azul
// de 8 de altura até 33 da borda direita, o "x" cinza de 11 depois dela e a linha de baixo em cinza de 13
#define W (liquid::Golden() ? 404.f : 452.f)
#define ROW (liquid::Golden() ? 56.f : 68.f)
#define PAD (liquid::Golden() ? 22.f : 16.f)
#define ICON (liquid::Golden() ? 32.f : 38.f)
#define BTN (liquid::Golden() ? 11.f : 16.f)

enum Kind { K_COPY, K_MOVE, K_DELETE, K_EXTRACT, K_COMPRESS, K_OTHER };

// um bloco (operação) da janela do Windows, como a thread leu
struct Tile {
    HWND win = nullptr;
    std::wstring id;                 // eltTile1...
    std::wstring start, from, to;    // "Copying 48,124 items from " / origem / destino
    std::wstring group, summary, time, remaining, item, pauseName;
    bool interrupted = false;
};
struct WinSnap { HWND h; bool visible; bool interrupted; std::vector<Tile> tiles; };

// o que desenhamos por operação
struct Row {
    HWND win; std::wstring id;
    Kind kind = K_OTHER;
    std::wstring line1, line2;
    float pct = -1, shown = 0;       // -1: ainda calculando (barra indeterminada)
    bool paused = false, canPause = false;
};

HWND g_wnd, g_back;
bool g_glass, g_inGlass, g_visible, g_dismissed, g_closing;
double g_openAt, g_closeAt;
ui::GpuSurface g_s;
HWINEVENTHOOK g_hook, g_hookFg;
std::map<DWORD, HWINEVENTHOOK> g_locHooks;  // posição das janelas do Windows, por processo (ver OnLocation)
std::vector<Row> g_rows;
std::set<std::wstring> g_seen;       // operações já mostradas (uma nova reabre a janela depois do fechar)
int g_hover = -1, g_press = -1;      // 0 = fechar; 1 + 2i = pausar a linha i; 2 + 2i = parar a linha i
POINT g_pos{ INT_MIN, INT_MIN };     // onde o usuário deixou a janela
POINT g_anchor{ INT_MIN, INT_MIN };  // centro da janela do Windows (a primeira vez)
float g_shownH;                      // altura atual (DIP)

// janelas do Windows que tiramos da tela: onde estavam
std::mutex g_mx;
std::map<HWND, RECT> g_saved;
std::set<HWND> g_track;
std::set<HWND> g_hidden;
struct Cmd { HWND win; std::wstring tile, button; };
std::vector<Cmd> g_cmds;
HANDLE g_wake;
std::atomic<bool> g_quit;
std::thread g_worker;

float Sc() { return ui::Scale(g_wnd); }
bool Tahoe() { return liquid::On(); }
float TitleH() { return Tahoe() ? 32.f : 28.f; }
float Radius() { return liquid::Golden() ? 16.f : Tahoe() ? 22.f : 10.f; }
float PanelH() { return TitleH() + std::max<size_t>(g_rows.size(), 1) * ROW + 6; }

std::wstring Lower(std::wstring s) { for (auto& c : s) c = towlower(c); return s; }
bool Starts(const std::wstring& s, const wchar_t* p) { return Lower(s).rfind(Lower(p), 0) == 0; }
bool Has(const std::wstring& s, const wchar_t* p) { return Lower(s).find(Lower(p)) != std::wstring::npos; }

std::wstring Fmt(const wchar_t* f, const std::wstring& a, const std::wstring& b = L"") {
    wchar_t buf[1024];
    swprintf(buf, 1024, f, a.c_str(), b.c_str());
    return buf;
}

// ---------------------------------------------------------------------------
// Thread da UI Automation: a janela do Windows pode estar num Explorer ocupado; aqui, travar não trava o Shell.

IUIAutomationElement* FindById(IUIAutomation* uia, IUIAutomationElement* in, const std::wstring& id) {
    VARIANT v; v.vt = VT_BSTR; v.bstrVal = SysAllocString(id.c_str());
    IUIAutomationCondition* c = nullptr;
    uia->CreatePropertyCondition(UIA_AutomationIdPropertyId, v, &c);
    VariantClear(&v);
    IUIAutomationElement* e = nullptr;
    if (c) { in->FindFirst(TreeScope_Descendants, c, &e); c->Release(); }
    return e;
}

void Invoke(IUIAutomationElement* e) {
    IUIAutomationInvokePattern* inv = nullptr;
    if (e && SUCCEEDED(e->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&inv))) && inv) { inv->Invoke(); inv->Release(); }
}

void RunCmd(IUIAutomation* uia, const Cmd& c) {
    if (!IsWindow(c.win)) return;
    IUIAutomationElement* root = nullptr;
    if (FAILED(uia->ElementFromHandle(c.win, &root)) || !root) return;
    IUIAutomationElement* tile = FindById(uia, root, c.tile);
    IUIAutomationElement* b = tile ? FindById(uia, tile, c.button) : nullptr;
    Invoke(b);
    Log(L"progresso: %ls em %ls (%ls)", c.button.c_str(), c.tile.c_str(), b ? L"ok" : L"não achou");
    if (b) b->Release();
    if (tile) tile->Release();
    root->Release();
}

// botões da barra de título da janela (sem id; o nome muda com o idioma do Windows)
bool BotaoDaBarra(const std::wstring& id, const std::wstring& name) {
    if (!id.empty()) return false;
    for (auto n : { L"Minimize", L"Maximize", L"Close", L"Restore", L"Minimizar", L"Maximizar", L"Fechar", L"Restaurar" }) if (name == n) return true;
    return false;
}

WinSnap Read(IUIAutomation* uia, IUIAutomationCacheRequest* cache, IUIAutomationCondition* all, HWND h, std::set<HWND>& expanded) {
    WinSnap s{ h, IsWindowVisible(h) != FALSE, false, {} };
    if (!s.visible) return s;
    IUIAutomationElement* root = nullptr;
    if (FAILED(uia->ElementFromHandle(h, &root)) || !root) return s;
    IUIAutomationElementArray* arr = nullptr;
    root->FindAllBuildCache(TreeScope_Descendants, all, cache, &arr);
    if (arr) {
        int n = 0; arr->get_Length(&n);
        Tile* cur = nullptr;
        for (int i = 0; i < n; i++) {
            IUIAutomationElement* e = nullptr; arr->GetElement(i, &e);
            if (!e) continue;
            BSTR bn = nullptr, bi = nullptr; CONTROLTYPEID type = 0;
            e->get_CachedName(&bn); e->get_CachedAutomationId(&bi); e->get_CachedControlType(&type);
            std::wstring name = bn ? bn : L"", id = bi ? bi : L"";
            if (bn) SysFreeString(bn);
            if (bi) SysFreeString(bi);
            if (id == L"eltDisplayModeBtn") {
                // "More details": abre os detalhes (tempo e itens restantes) uma vez
                if (!expanded.count(h) && (Lower(name) == L"more details" || Lower(name) == L"mais detalhes")) Invoke(e);
                expanded.insert(h);
            } else if (id.rfind(L"eltTile", 0) == 0) {
                s.tiles.push_back({});
                cur = &s.tiles.back();
                cur->win = h; cur->id = id; cur->group = name;
                if (Has(name, L"Interrupted") || Has(name, L"Interrompid")) cur->interrupted = true;
            } else if (cur) {
                if (id == L"eltStartText") cur->start = name;
                else if (id == L"eltFirstLocation") cur->from = name;
                else if (id == L"eltSecondLocation") cur->to = name;
                else if (id == L"eltSummary") cur->summary = name;
                else if (id == L"eltTimeRemaining") cur->time = name;
                else if (id == L"eltItemsRemaining") cur->remaining = name;
                else if (id == L"eltItemName") cur->item = name;
                else if (id == L"eltPauseButton") cur->pauseName = name;
                else if (id.rfind(L"eltInterrupt", 0) == 0) cur->interrupted = true;
                // qualquer outro botão é uma pergunta (substituir, pular, tentar de novo...): quem responde é a janela do Windows
                // (menos os botões da barra de título da própria janela: Minimize/Maximize/Close/Restore. Sem a WacOSWindow.dll
                // — modo nativo — a barra de título existe, e eles faziam toda cópia parecer uma pergunta: a janela do Windows voltava)
                else if (type == UIA_ButtonControlTypeId && id != L"eltCancelButton" && id != L"Minimize" && id != L"Maximize" && id != L"Close" && id != L"Restore" && id != L"SmallIcon" && !BotaoDaBarra(id, name)) {
                    cur->interrupted = true;
                    static std::set<std::wstring> ditos;  // registro: qual botão fez a operação parecer uma pergunta
                    if (ditos.insert(id + L"|" + name).second) Log(L"progresso: botão de pergunta id='%ls' nome='%ls'", id.c_str(), name.c_str());
                }
            }
            e->Release();
        }
        arr->Release();
    }
    root->Release();
    for (auto& t : s.tiles) if (t.interrupted) s.interrupted = true;
    return s;
}

void Worker() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IUIAutomation* uia = nullptr;
    CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia));
    IUIAutomationCacheRequest* cache = nullptr;
    IUIAutomationCondition* all = nullptr;
    if (uia) {
        uia->CreateCacheRequest(&cache);
        if (cache) for (PROPERTYID p : { UIA_NamePropertyId, UIA_AutomationIdPropertyId, UIA_ControlTypePropertyId }) cache->AddProperty(p);
        uia->CreateTrueCondition(&all);
    }
    std::set<HWND> expanded;
    while (!g_quit && uia && cache && all) {
        std::vector<Cmd> cmds;
        std::vector<HWND> wins;
        {
            std::lock_guard<std::mutex> l(g_mx);
            cmds.swap(g_cmds);
            for (auto it = g_track.begin(); it != g_track.end();) {
                if (!IsWindow(*it)) { expanded.erase(*it); it = g_track.erase(it); } else wins.push_back(*it++);
            }
        }
        for (auto& c : cmds) RunCmd(uia, c);
        auto* snap = new std::vector<WinSnap>;
        for (HWND h : wins) snap->push_back(Read(uia, cache, all, h, expanded));
        if (!PostMessageW(g_wnd, WM_PG_SNAP, 0, (LPARAM)snap)) delete snap;
        WaitForSingleObject(g_wake, wins.empty() ? 2000 : 300);
    }
    if (all) all->Release();
    if (cache) cache->Release();
    if (uia) uia->Release();
    CoUninitialize();
}

void Command(HWND win, const std::wstring& tile, const wchar_t* button) {
    { std::lock_guard<std::mutex> l(g_mx); g_cmds.push_back({ win, tile, button }); }
    SetEvent(g_wake);
}

// ---------------------------------------------------------------------------
// A janela do Windows: fora da tela enquanto só mostra progresso; de volta quando pergunta algo.

bool OffScreen(const RECT& r) { return r.right <= GetSystemMetrics(SM_XVIRTUALSCREEN) - 100; }

HWND g_lastFg;  // a última janela de verdade na frente (para onde o foco volta)

// A janela do Windows vem com o foco do teclado; fora da tela, as teclas iriam para ela. O foco volta para
// quem pediu a operação (a dona: o Finder, o Explorer) ou para a janela que estava na frente.
void FocusAway(HWND h) {
    if (GetForegroundWindow() != h) return;
    HWND to = GetWindow(h, GW_OWNER);
    if (!to || !IsWindowVisible(to)) to = g_lastFg;
    if (!to || !IsWindow(to) || !IsWindowVisible(to)) return;
    DWORD ft = GetWindowThreadProcessId(h, nullptr), me = GetCurrentThreadId();
    bool att = ft && ft != me && AttachThreadInput(me, ft, TRUE);
    BOOL ok = SetForegroundWindow(to);
    if (att) AttachThreadInput(me, ft, FALSE);
    static std::map<HWND, DWORD> tried;  // o truque do Alt (o mesmo do Dock), no máximo a cada 2 s por janela
    if (!ok && GetTickCount() - tried[h] > 2000) {
        tried[h] = GetTickCount();
        const ULONG_PTR KEY_MARK = 0x53484C4C;  // tecla injetada por nós (o gancho do teclado do Shell ignora)
        keybd_event(VK_MENU, 0, 0, KEY_MARK);
        ok = SetForegroundWindow(to);
        keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, KEY_MARK);
        Log(L"progresso: foco de volta para %p pelo Alt (%ls)", to, ok ? L"ok" : L"falhou");
    }
}

void HideOriginal(HWND h) {
    RECT r; if (!GetWindowRect(h, &r)) return;
    {
        std::lock_guard<std::mutex> l(g_mx);
        if (!OffScreen(r)) {
            g_saved[h] = r;
            if (g_anchor.x == INT_MIN && IsWindowVisible(h)) g_anchor = { (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
            int x = GetSystemMetrics(SM_XVIRTUALSCREEN) - (r.right - r.left) - 4000;
            SetWindowPos(h, nullptr, x, r.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        }
        g_hidden.insert(h);
    }
    FocusAway(h);
}

// título de quando ela só mostra progresso ("Progress", "44% complete", "Calculating..."); outro título
// ("Confirm Folder Replace", "Replace or Skip Files"...) é pergunta: esperamos a leitura da thread
bool ProgressTitle(HWND h) {
    wchar_t t[128] = {}; GetWindowTextW(h, t, 128);
    std::wstring s = t;
    if (s.empty() || s.find(L'%') != std::wstring::npos) return true;
    for (auto p : { L"Progress", L"Progresso", L"Calculating", L"Calculando", L"Preparing", L"Preparando", L"Discovering", L"Descobrindo" })
        if (Starts(s, p)) return true;
    return false;
}

void ShowOriginal(HWND h, bool front) {
    std::lock_guard<std::mutex> l(g_mx);
    g_hidden.erase(h);
    auto it = g_saved.find(h);
    RECT r; if (it == g_saved.end() || !GetWindowRect(h, &r) || !OffScreen(r)) return;
    SetWindowPos(h, front ? HWND_TOP : nullptr, it->second.left, it->second.top, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS | (front ? 0 : SWP_NOZORDER));
    if (front) SetForegroundWindow(h);
}

// ---------------------------------------------------------------------------
// Textos do Mac a partir dos do Windows (em inglês ou em português)

Kind KindOf(const std::wstring& start) {
    if (Starts(start, L"Copying") || Starts(start, L"Copiando")) return K_COPY;
    if (Starts(start, L"Moving") || Starts(start, L"Movendo")) return K_MOVE;
    if (Starts(start, L"Deleting") || Starts(start, L"Excluindo") || Starts(start, L"Recycling") || Starts(start, L"Reciclando")) return K_DELETE;
    if (Starts(start, L"Extracting") || Starts(start, L"Extraindo")) return K_EXTRACT;
    if (Starts(start, L"Compressing") || Starts(start, L"Compactando")) return K_COMPRESS;
    return K_OTHER;
}

std::wstring CountOf(const std::wstring& start) {  // "48,124" de "Copying 48,124 items from "
    size_t i = start.find_first_of(L"0123456789");
    if (i == std::wstring::npos) return L"";
    size_t j = start.find_first_not_of(L"0123456789,.  ", i);
    return start.substr(i, j == std::wstring::npos ? std::wstring::npos : j - i);
}

// "About 3 minutes" do Windows em inglês vira "Cerca de 3 minutos" com o idioma em português
std::wstring TimeText(std::wstring s) {
    if (!idioma::Pt()) return s;
    static const std::pair<const wchar_t*, const wchar_t*> rep[] = {
        { L"About ", L"Cerca de " }, { L" and ", L" e " }, { L"seconds", L"segundos" }, { L"second", L"segundo" },
        { L"minutes", L"minutos" }, { L"minute", L"minuto" }, { L"hours", L"horas" }, { L"hour", L"hora" },
        { L"days", L"dias" }, { L"day", L"dia" } };
    for (auto& [a, b] : rep) {
        std::wstring A = a, B = b;
        for (size_t p = 0; (p = s.find(A, p)) != std::wstring::npos; p += B.size()) s.replace(p, A.size(), B);
    }
    return s;
}

Row MakeRow(const Tile& t) {
    Row r; r.win = t.win; r.id = t.id;
    r.kind = KindOf(t.start);
    std::wstring n = CountOf(t.start);
    bool one = n == L"1";
    std::wstring what = one && !t.item.empty() ? L"“" + t.item + L"”" : Fmt(Tr(L"%ls itens"), n);
    switch (r.kind) {
    case K_COPY: r.line1 = t.to.empty() ? Fmt(Tr(L"Copiando %ls"), what) : Fmt(Tr(L"Copiando %ls para “%ls”"), what, t.to); break;
    case K_MOVE: r.line1 = t.to.empty() ? Fmt(Tr(L"Movendo %ls"), what) : Fmt(Tr(L"Movendo %ls para “%ls”"), what, t.to); break;
    case K_DELETE: r.line1 = t.from.empty() ? Fmt(Tr(L"Apagando %ls"), what) : Fmt(Tr(L"Apagando %ls de “%ls”"), what, t.from); break;
    case K_EXTRACT: r.line1 = t.to.empty() ? Fmt(Tr(L"Expandindo “%ls”"), t.from) : Fmt(Tr(L"Expandindo “%ls” para “%ls”"), t.from, t.to); break;
    case K_COMPRESS: r.line1 = Fmt(Tr(L"Comprimindo %ls"), what); break;
    default: {  // operação que não conhecemos: o texto do Windows
        r.line1 = t.start + t.from;
        if (!t.to.empty()) r.line1 += L" → " + t.to;
        if (r.line1.empty()) r.line1 = t.group;
    }
    }
    // porcentagem: "44% complete"
    size_t pc = t.summary.find(L'%');
    if (pc != std::wstring::npos) {
        size_t b = pc; while (b > 0 && iswdigit(t.summary[b - 1])) b--;
        if (b < pc) r.pct = std::clamp(_wtoi(t.summary.c_str() + b) / 100.f, 0.f, 1.f);
    }
    r.canPause = !t.pauseName.empty();
    r.paused = Has(t.pauseName, L"Resume") || Has(t.pauseName, L"Retomar") || Has(t.pauseName, L"Continuar") || Has(t.group, L"Paused") || Has(t.group, L"Pausad");
    // "26,764 (509 MB)" -> "509 MB"
    std::wstring left = t.remaining;
    size_t a = left.find(L'('), z = left.rfind(L')');
    if (a != std::wstring::npos && z != std::wstring::npos && z > a) left = left.substr(a + 1, z - a - 1);
    bool calculating = r.pct < 0 || t.time.empty() || Has(t.time, L"Calculat") || Has(t.time, L"Calculand");
    if (r.paused) r.line2 = left.empty() ? std::wstring(Tr(L"Pausado")) : Fmt(Tr(L"Pausado — faltam %ls"), left);
    else if (calculating) {
        r.line2 = r.kind == K_COPY ? Tr(L"Preparando para copiar…") : r.kind == K_MOVE ? Tr(L"Preparando para mover…") :
                  r.kind == K_DELETE ? Tr(L"Preparando para apagar…") : Tr(L"Preparando…");
        if (r.pct > 0) r.line2 = left.empty() ? std::wstring(t.summary) : Fmt(Tr(L"Faltam %ls"), left);
    } else r.line2 = left.empty() ? TimeText(t.time) : Fmt(Tr(L"Faltam %ls — %ls"), left, TimeText(t.time));
    return r;
}

std::wstring Title() {
    if (g_rows.empty()) return Tr(L"Copiar");
    Kind k = g_rows[0].kind;
    for (auto& r : g_rows) if (r.kind != k) return Tr(L"Progresso");
    switch (k) {
    case K_COPY: return Tr(L"Copiar");
    case K_MOVE: return Tr(L"Mover");
    case K_DELETE: return Tr(L"Apagar");
    case K_EXTRACT: return Tr(L"Expandir");
    case K_COMPRESS: return Tr(L"Comprimir");
    default: return Tr(L"Progresso");
    }
}

// ---------------------------------------------------------------------------
// Desenho

D2D1_RECT_F CloseRect() { float cy = TitleH() / 2, d = Tahoe() ? 14.f : 12.f, x = liquid::Golden() ? 9.f : 13.f; return { x, cy - d / 2, x + d, cy + d / 2 }; }
D2D1_RECT_F StopRect(int i) {
    if (liquid::Golden()) { float y = TitleH() + i * ROW + 23.5f; return { W - 14 - BTN, y - BTN / 2, W - 14, y + BTN / 2 }; }
    float y = TitleH() + i * ROW + 31; return { W - PAD - BTN, y - BTN / 2, W - PAD, y + BTN / 2 }; }
D2D1_RECT_F PauseRect(int i) { auto r = StopRect(i); return { r.left - BTN - 8, r.top, r.left - 8, r.bottom }; }

bool In(const D2D1_RECT_F& r, D2D1_POINT_2F p) { return p.x >= r.left - 3 && p.x < r.right + 3 && p.y >= r.top - 3 && p.y < r.bottom + 3; }

int HitTest(D2D1_POINT_2F p) {
    if (In(CloseRect(), p)) return 0;
    for (int i = 0; i < (int)g_rows.size(); i++) {
        if (g_rows[i].canPause && In(PauseRect(i), p)) return 1 + 2 * i;
        if (In(StopRect(i), p)) return 2 + 2 * i;
    }
    return -1;
}

void DrawText(ID2D1DeviceContext* dc, const std::wstring& s, IDWriteTextFormat* tf, D2D1_RECT_F r, ID2D1Brush* br) {
    if (tf) dc->DrawTextW(s.c_str(), (UINT32)s.size(), tf, r, br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

IDWriteTextFormat* Trimmed(float size, DWRITE_FONT_WEIGHT w, DWRITE_TEXT_ALIGNMENT a = DWRITE_TEXT_ALIGNMENT_LEADING) {
    IDWriteTextFormat* tf = ui::Format(size, w, false, a);
    if (!tf) return nullptr;
    tf->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    IDWriteInlineObject* sign = nullptr;
    ui::dw->CreateEllipsisTrimmingSign(tf, &sign);
    DWRITE_TRIMMING tr{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
    tf->SetTrimming(&tr, sign);
    if (sign) sign->Release();
    return tf;
}

void Motion();

void Paint() {
    if (!g_visible) return;
    float sc = Sc();
    float H = PanelH();
    int wpx = (int)std::lround(W * sc), hpx = (int)std::lround(H * sc);
    bool dark = ui::Dark(), tahoe = Tahoe(), golden = liquid::Golden();
    const float R = Radius();
    if (g_glass) backdrop::SetPieces(g_back, (float)wpx, (float)hpx, { { D2D1::RectF(0, 0, (float)wpx, (float)hpx), R * sc } },
                                     golden ? (dark ? D2D1::ColorF(0.17f, 0.17f, 0.17f, 0.98f) : D2D1::ColorF(1, 1, 1, 0.98f)) :  // macOS 27: janela opaca
                                     dark ? D2D1::ColorF(0.15f, 0.15f, 0.16f, tahoe ? 0.62f : 0.84f) : D2D1::ColorF(0.95f, 0.95f, 0.96f, tahoe ? 0.55f : 0.80f), 30.f);
    ID2D1DeviceContext* dc = g_glass ? backdrop::ContentBegin(g_back, wpx, hpx, sc) : nullptr;
    g_inGlass = dc != nullptr;
    if (!dc) dc = ui::GpuBegin(g_s, wpx, hpx, sc);
    if (!dc) return;
    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &br);
    if (!br) { if (g_inGlass) backdrop::ContentEnd(g_back); else ui::GpuEnd(g_s); return; }
    if (!g_glass) {
        br->SetColor(dark ? D2D1::ColorF(0.17f, 0.17f, 0.18f, 0.97f) : D2D1::ColorF(0.95f, 0.95f, 0.96f, 0.97f));
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0, 0, W, H), R, R), br);
    }
    if (golden) {
        br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.12f) : D2D1::ColorF(0, 0, 0, 0.10f));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, W - 0.5f, H - 0.5f), R - 0.5f, R - 0.5f), br, 1);
    } else if (tahoe) liquid::Rim(dc, D2D1::RectF(0, 0, W, H), R, dark);
    else {
        br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.16f) : D2D1::ColorF(0, 0, 0, 0.14f));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(0.5f, 0.5f, W - 0.5f, H - 0.5f), R - 0.5f, R - 0.5f), br, 1);
    }
    D2D1_COLOR_F text = dark ? D2D1::ColorF(1, 1, 1, 0.90f) : D2D1::ColorF(0, 0, 0, 0.85f);
    D2D1_COLOR_F gray = dark ? D2D1::ColorF(1, 1, 1, 0.55f) : D2D1::ColorF(0, 0, 0, 0.50f);

    // barra de título: fechar (vermelho), os outros dois apagados, e o título no meio
    {
        D2D1_RECT_F c = CloseRect();
        float d = c.right - c.left, gap = tahoe ? 9.f : 8.f;
        for (int k = 0; k < 3; k++) {
            D2D1_ELLIPSE e = D2D1::Ellipse({ c.left + d / 2 + k * (d + gap), (c.top + c.bottom) / 2 }, d / 2, d / 2);
            br->SetColor(k == 0 ? semaforo::Close() : (dark ? D2D1::ColorF(1, 1, 1, 0.16f) : D2D1::ColorF(0, 0, 0, 0.13f)));
            if (golden && k == 1) br->SetColor(D2D1::ColorF(0.996f, 0.737f, 0.18f));  // macOS 27: minimizar aceso, zoom apagado
            dc->FillEllipse(e, br);
            semaforo::Gloss(dc, e.point, d / 2, br);
            if (k == 0) {
                br->SetColor(D2D1::ColorF(0.75f, 0.2f, 0.18f, 0.6f));
                dc->DrawEllipse(D2D1::Ellipse(e.point, d / 2 - 0.25f, d / 2 - 0.25f), br, 0.5f);
                if (g_hover == 0) {  // o "x" aparece com o mouse em cima, como no Mac
                    br->SetColor(D2D1::ColorF(0.30f, 0.0f, 0.0f, 0.8f));
                    float q = d * 0.2f;
                    dc->DrawLine({ e.point.x - q, e.point.y - q }, { e.point.x + q, e.point.y + q }, br, 1.2f);
                    dc->DrawLine({ e.point.x - q, e.point.y + q }, { e.point.x + q, e.point.y - q }, br, 1.2f);
                }
            }
        }
        IDWriteTextFormat* tt = Trimmed(13, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER);
        if (tt) tt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.80f) : D2D1::ColorF(0, 0, 0, 0.75f));
        if (!golden) DrawText(dc, Title(), tt, D2D1::RectF(90, 0, W - 90, TitleH()), br);  // macOS 27: sem título
        if (tt) tt->Release();
    }

    IDWriteTextFormat* t1 = Trimmed(12.5f, DWRITE_FONT_WEIGHT_NORMAL);
    IDWriteTextFormat* t2 = Trimmed(golden ? 13.f : 11.f, DWRITE_FONT_WEIGHT_NORMAL);
    const D2D1_COLOR_F accent = dark ? D2D1::ColorF(0.04f, 0.52f, 1.0f) : D2D1::ColorF(0.0f, 0.48f, 1.0f);
    float now = (float)(anim::Now() / 1000.0);
    for (int i = 0; i < (int)g_rows.size(); i++) {
        const Row& r = g_rows[i];
        float y = TitleH() + i * ROW;
        if (i > 0) {  // divisória entre as operações
            br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.10f) : D2D1::ColorF(0, 0, 0, 0.09f));
            dc->FillRectangle(D2D1::RectF(PAD + ICON + 12, y, W - PAD, y + 1 / std::max(1.f, Sc())), br);
        }
        D2D1_RECT_F ic = golden ? D2D1::RectF(PAD, y + 8, PAD + ICON, y + 8 + ICON) : D2D1::RectF(PAD, y + 14, PAD + ICON, y + 14 + ICON);
        if (r.kind == K_DELETE) ui::DrawTrash(dc, ic, true);
        else ui::DrawFolder(dc, ic, 0);
        float x0 = golden ? 62.f : PAD + ICON + 12, xr = (r.canPause ? PauseRect(i).left : StopRect(i).left) - 10;
        br->SetColor(text);
        DrawText(dc, r.line1, t1, golden ? D2D1::RectF(x0, y + 4, W - 14, y + 20) : D2D1::RectF(x0, y + 9, W - PAD, y + 26), br);
        // barra fina do Mac: trilho cinza e o azul crescendo (calculando: um pedaço indo e voltando)
        float bh = golden ? 8.f : tahoe ? 6.f : 5.f, by = golden ? y + 23.5f - bh / 2 : y + 31 - bh / 2;
        D2D1_RECT_F track = D2D1::RectF(x0, by, xr, by + bh);
        br->SetColor(dark ? D2D1::ColorF(1, 1, 1, 0.14f) : D2D1::ColorF(0, 0, 0, 0.10f));
        dc->FillRoundedRectangle(D2D1::RoundedRect(track, bh / 2, bh / 2), br);
        D2D1_COLOR_F fill = r.paused ? gray : accent;
        br->SetColor(fill);
        float tw = track.right - track.left;
        if (r.pct < 0 && !r.paused) {
            float seg = tw * 0.25f, u = (std::sin(now * 2.6f) + 1) / 2;
            float a = track.left + u * (tw - seg);
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(a, by, a + seg, by + bh), bh / 2, bh / 2), br);
        } else if (r.shown > 0) {
            float wfill = std::max(bh, tw * r.shown);
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(track.left, by, track.left + wfill, by + bh), bh / 2, bh / 2), br);
        }
        br->SetColor(gray);
        DrawText(dc, r.line2, t2, golden ? D2D1::RectF(x0, y + 33, W - 14, y + 51) : D2D1::RectF(x0, y + 40, W - PAD, y + 56), br);
        // pausar/continuar e parar: círculos cinza com o desenho branco (o "xmark.circle.fill" do Mac)
        for (int b = r.canPause ? 0 : 1; b < 2; b++) {
            D2D1_RECT_F rc = b == 0 ? PauseRect(i) : StopRect(i);
            int id = b == 0 ? 1 + 2 * i : 2 + 2 * i;
            D2D1_POINT_2F c{ (rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2 };
            float rad = BTN / 2;
            float k = g_press == id ? 0.62f : g_hover == id ? 0.52f : 0.40f;
            br->SetColor(dark ? D2D1::ColorF(1, 1, 1, k * 0.9f) : D2D1::ColorF(0, 0, 0, k * 0.85f));
            dc->FillEllipse(D2D1::Ellipse(c, rad, rad), br);
            br->SetColor(dark ? D2D1::ColorF(0.12f, 0.12f, 0.13f) : D2D1::ColorF(1, 1, 1));
            if (b == 1) {
                float q = rad * 0.38f;
                dc->DrawLine({ c.x - q, c.y - q }, { c.x + q, c.y + q }, br, 1.6f);
                dc->DrawLine({ c.x - q, c.y + q }, { c.x + q, c.y - q }, br, 1.6f);
            } else if (r.paused) {  // continuar: triângulo
                ID2D1Factory* f = nullptr; dc->GetFactory(&f);
                ID2D1PathGeometry* g = nullptr; if (f) f->CreatePathGeometry(&g);
                if (g) {
                    ID2D1GeometrySink* s = nullptr; g->Open(&s);
                    s->BeginFigure({ c.x - rad * 0.28f, c.y - rad * 0.42f }, D2D1_FIGURE_BEGIN_FILLED);
                    s->AddLine({ c.x + rad * 0.45f, c.y }); s->AddLine({ c.x - rad * 0.28f, c.y + rad * 0.42f });
                    s->EndFigure(D2D1_FIGURE_END_CLOSED); s->Close(); s->Release();
                    dc->FillGeometry(g, br); g->Release();
                }
                if (f) f->Release();
            } else {  // pausar: duas barrinhas
                float bw = rad * 0.22f, bhh = rad * 0.42f;
                dc->FillRectangle(D2D1::RectF(c.x - bw * 1.9f, c.y - bhh, c.x - bw * 0.6f, c.y + bhh), br);
                dc->FillRectangle(D2D1::RectF(c.x + bw * 0.6f, c.y - bhh, c.x + bw * 1.9f, c.y + bhh), br);
            }
        }
    }
    for (auto* f : { t1, t2 }) if (f) f->Release();
    br->Release();
    if (g_inGlass) backdrop::ContentEnd(g_back); else ui::GpuEnd(g_s);
    Motion();
}

void Motion() {
    float sc = Sc();
    int wpx = (int)std::lround(W * sc), hpx = (int)std::lround(PanelH() * sc);
    anim::Panel mo = g_closing ? anim::PanelOut(anim::Now() - g_closeAt) : anim::PanelIn(anim::Now() - g_openAt);
    if (g_inGlass) backdrop::Fade(g_back, mo.fade, mo.scale, wpx / 2.f, hpx / 2.f);
    else { ui::GpuMotion(g_s, mo.scale, wpx / 2.f, hpx / 2.f, mo.fade); if (g_glass) backdrop::Fade(g_back, mo.fade, mo.scale, wpx / 2.f, hpx / 2.f); }
}

bool Animating() {
    if (!g_visible) return false;
    if (g_closing) return true;
    if (!anim::PanelIn(anim::Now() - g_openAt).done) return true;
    for (auto& r : g_rows) {
        if (r.pct < 0 && !r.paused) return true;
        if (r.pct >= 0 && std::fabs(r.shown - r.pct) > 0.002f) return true;
    }
    return false;
}

void Tick() {
    for (auto& r : g_rows) if (r.pct >= 0) r.shown += (r.pct - r.shown) * 0.18f;  // a barra anda macio entre as leituras
    ui::FrameClock(g_wnd, WM_PG_FRAME, Animating());
}

// tamanho e lugar: a janela cresce para baixo, mantendo o topo
void Place(bool first) {
    float sc = Sc();
    int wpx = (int)std::lround(W * sc), hpx = (int)std::lround(PanelH() * sc);
    int x, y;
    if (first) {
        if (g_pos.x != INT_MIN) { x = g_pos.x; y = g_pos.y; }
        else {
            POINT c = g_anchor;
            MONITORINFO mi{ sizeof(mi) };
            if (c.x == INT_MIN) { GetCursorPos(&c); }
            GetMonitorInfoW(MonitorFromPoint(c, MONITOR_DEFAULTTOPRIMARY), &mi);
            if (g_anchor.x == INT_MIN) c = { (mi.rcWork.left + mi.rcWork.right) / 2, mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top) / 3 };
            x = c.x - wpx / 2; y = c.y - (int)std::lround(ROW * sc);
            x = std::clamp(x, (int)mi.rcWork.left, (int)mi.rcWork.right - wpx);
            y = std::clamp(y, (int)mi.rcWork.top, (int)mi.rcWork.bottom - hpx);
        }
        SetWindowPos(g_wnd, HWND_TOPMOST, x, y, wpx, hpx, SWP_NOACTIVATE);
        SetWindowPos(g_back, g_wnd, x, y, wpx, hpx, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        ShowWindow(g_wnd, SW_SHOWNA);
    } else {
        RECT r; GetWindowRect(g_wnd, &r);
        if (r.bottom - r.top == hpx && r.right - r.left == wpx) return;
        SetWindowPos(g_wnd, nullptr, 0, 0, wpx, hpx, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        SetWindowPos(g_back, nullptr, 0, 0, wpx, hpx, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}

void Open() {
    g_visible = true; g_closing = false;
    g_openAt = anim::Now();
    Place(true);
    Paint();
    Tick();
}

void Close(bool byUser) {
    if (!g_visible || g_closing) return;
    if (byUser) g_dismissed = true;
    g_closing = true;
    g_closeAt = anim::Now();
    ui::FrameClock(g_wnd, WM_PG_FRAME, true);
}

void HideNow() {
    g_visible = g_closing = false;
    ui::FrameClock(g_wnd, WM_PG_FRAME, false);
    ShowWindow(g_wnd, SW_HIDE);
    ShowWindow(g_back, SW_HIDE);
    g_hover = g_press = -1;
}

// leitura nova da thread: linhas, janelas do Windows para esconder ou devolver
void OnSnap(std::vector<WinSnap>* snap) {
    std::vector<Row> rows;
    bool fresh = false;
    {   // registro das mudanças (quantas janelas, blocos, perguntas): para entender quando a do Windows volta
        static std::wstring last;
        std::wstring now;
        for (auto& s : *snap) { RECT r{}; GetWindowRect(s.h, &r); now += L"[vis " + std::to_wstring(s.visible) + L" blocos " + std::to_wstring(s.tiles.size()) + L" pergunta " + std::to_wstring(s.interrupted) + L" fora " + std::to_wstring(OffScreen(r)) + L"] "; }
        if (now != last) { last = now; Log(L"progresso: leitura %ls", now.empty() ? L"(nenhuma janela)" : now.c_str()); }
    }
    for (auto& s : *snap) {
        if (!s.visible) continue;
        if (s.interrupted) ShowOriginal(s.h, true);  // pergunta: responde na janela do Windows
        else HideOriginal(s.h);
        for (auto& t : s.tiles) {
            if (s.interrupted) continue;
            Row r = MakeRow(t);
            for (auto& old : g_rows) if (old.win == r.win && old.id == r.id) r.shown = old.shown;
            std::wstring key = std::to_wstring((UINT_PTR)t.win) + L"|" + t.id + L"|" + t.start + t.from + t.to;
            if (!g_seen.count(key)) { g_seen.insert(key); fresh = true; }
            rows.push_back(std::move(r));
        }
    }
    std::set<DWORD> pids;  // processos que ainda têm janela de progresso: os outros perdem o gancho de posição
    {
        std::lock_guard<std::mutex> l(g_mx);
        for (HWND w : g_track) { DWORD pid = 0; if (IsWindow(w) && GetWindowThreadProcessId(w, &pid)) pids.insert(pid); }
    }
    for (auto it = g_locHooks.begin(); it != g_locHooks.end();) {
        if (pids.count(it->first)) { ++it; continue; }
        if (it->second) UnhookWinEvent(it->second);
        it = g_locHooks.erase(it);
    }
    delete snap;
    g_rows = std::move(rows);
    if (fresh) g_dismissed = false;  // operação nova: a janela volta, mesmo que tenha sido fechada
    if (g_rows.empty()) {
        if (g_visible) Close(false);
        return;
    }
    if (g_dismissed) return;
    if (!g_visible || g_closing) { Open(); return; }
    Place(false);
    Paint();
    Tick();
}

// criada (ainda escondida) ou mostrada: sai da tela antes de aparecer; e quem está na frente (para o foco voltar)
void CALLBACK OnEvent(HWINEVENTHOOK, DWORD ev, HWND h, LONG obj, LONG, DWORD, DWORD) {
    if (obj != OBJID_WINDOW || !h || GetAncestor(h, GA_ROOT) != h) return;
    wchar_t cls[32] = {}; GetClassNameW(h, cls, 32);
    bool ours = !wcscmp(cls, L"OperationStatusWindow");
    if (ev == EVENT_SYSTEM_FOREGROUND) { if (!ours) g_lastFg = h; return; }
    if (ours && ev != EVENT_OBJECT_DESTROY) PostMessageW(g_wnd, WM_PG_SHOWN, ev == EVENT_OBJECT_CREATE, (LPARAM)h);
}

// O diálogo do Windows se centraliza sozinho depois de aparecer (sem evento de mostrar): vigiamos as
// mudanças de posição só no processo dele e o mandamos de volta para fora na hora.
void CALLBACK OnLocation(HWINEVENTHOOK, DWORD, HWND h, LONG obj, LONG child, DWORD, DWORD) {
    if (obj != OBJID_WINDOW || child != CHILDID_SELF || !h) return;
    {
        std::lock_guard<std::mutex> l(g_mx);
        if (!g_hidden.count(h)) return;
    }
    RECT r;
    if (GetWindowRect(h, &r) && !OffScreen(r)) { static int n; if (n++ < 12) Log(L"progresso: a janela do Windows voltou para %ld,%ld; fora de novo", r.left, r.top); HideOriginal(h); }
}

void Track(HWND h) {
    { std::lock_guard<std::mutex> l(g_mx); g_track.insert(h); }
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid && !g_locHooks.count(pid))
        g_locHooks[pid] = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, OnLocation, pid, 0, WINEVENT_OUTOFCONTEXT);
    SetEvent(g_wake);
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PG_SHOWN: {
        HWND t = (HWND)l;
        if (!IsWindow(t)) return 0;
        Track(t);
        // sai da tela já (criada: ainda sem conteúdo; mostrada: se o título é de progresso). Pergunta fica:
        // a thread confirma e, se mesmo assim era pergunta, devolve logo em seguida
        bool hide = w || ProgressTitle(t);
        if (hide) HideOriginal(t);
        wchar_t title[96] = {}; GetWindowTextW(t, title, 96);
        RECT r{}; GetWindowRect(t, &r);
        Log(L"progresso: janela do Windows %ls (“%ls”, x %ld, visível %d): %ls", w ? L"criada" : L"mostrada", title, r.left, IsWindowVisible(t), hide ? L"fora da tela" : L"esperando a leitura");
        return 0;
    }
    case WM_PG_SNAP: OnSnap((std::vector<WinSnap>*)l); return 0;
    case WM_PG_FRAME:
        if (g_closing && anim::PanelOut(anim::Now() - g_closeAt).done) { HideNow(); return 0; }
        if (g_visible) Paint();
        Tick();
        return 0;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps); Paint(); return 0; }
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_MOUSEMOVE: case WM_LBUTTONDOWN: case WM_LBUTTONUP: {
        float sc = Sc();
        D2D1_POINT_2F p{ (short)LOWORD(l) / sc, (short)HIWORD(l) / sc };
        int hit = HitTest(p);
        if (m == WM_MOUSEMOVE) {
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 }; TrackMouseEvent(&tme);
            if (hit != g_hover) { g_hover = hit; Paint(); }
            return 0;
        }
        if (m == WM_LBUTTONDOWN) {
            SetWindowPos(h, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            SetWindowPos(g_back, h, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            if (hit < 0) { ReleaseCapture(); SendMessageW(h, WM_NCLBUTTONDOWN, HTCAPTION, 0); return 0; }  // arrasta pelo fundo
            g_press = hit; SetCapture(h); Paint();
            return 0;
        }
        int was = g_press; g_press = -1; ReleaseCapture();
        if (was >= 0 && was == hit) {
            if (hit == 0) Close(true);  // fechar não para nada (como no Mac): as operações continuam
            else {
                int i = (hit - 1) / 2;
                if (i < (int)g_rows.size()) {
                    bool stop = hit % 2 == 0;
                    Command(g_rows[i].win, g_rows[i].id, stop ? L"eltCancelButton" : L"eltPauseButton");
                    if (!stop) g_rows[i].paused = !g_rows[i].paused;  // resposta imediata; a próxima leitura confirma
                }
            }
        }
        Paint();
        return 0;
    }
    case WM_MOUSELEAVE: if (g_hover != -1) { g_hover = -1; Paint(); } return 0;
    case WM_MOVE: {
        RECT r; GetWindowRect(h, &r);
        if (g_visible) g_pos = { r.left, r.top };
        SetWindowPos(g_back, h, r.left, r.top, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
        return 0;
    }
    case WM_DPICHANGED: if (g_visible) { Place(false); Paint(); } return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

LRESULT CALLBACK BackProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

bool progresso::Owns(HWND h) {
    std::lock_guard<std::mutex> l(g_mx);
    return g_hidden.count(h) != 0;
}

bool progresso::Create(HINSTANCE hi) {
    if (shell::Setting(L"progresso_mac", L"1") == L"0") return false;
    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = hi; wc.lpszClassName = L"ShellProgresso"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    WNDCLASSW bc{}; bc.lpfnWndProc = BackProc; bc.hInstance = hi; bc.lpszClassName = L"ShellProgressoFundo";
    RegisterClassW(&bc);
    g_wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, L"Progresso", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
    g_back = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, bc.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
    if (!g_wnd || !g_back) return false;
    g_glass = backdrop::Attach(g_back);
    if (!g_glass) {
        DestroyWindow(g_back);
        g_back = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, bc.lpszClassName, L"", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr, hi, nullptr);
    }
    DWORD corner = 1; DwmSetWindowAttribute(g_back, 33, &corner, sizeof(corner));
    COLORREF none = 0xFFFFFFFE; DwmSetWindowAttribute(g_back, 34, &none, sizeof(none));
    apps::ours.push_back(g_wnd);
    apps::ours.push_back(g_back);
    g_s.hwnd = g_wnd;
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_quit = false;
    g_worker = std::thread(Worker);
    g_hook = SetWinEventHook(EVENT_OBJECT_CREATE, EVENT_OBJECT_SHOW, nullptr, OnEvent, 0, 0, WINEVENT_OUTOFCONTEXT | shell::SemProprio());
    g_hookFg = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnEvent, 0, 0, WINEVENT_OUTOFCONTEXT | shell::SemProprio());
    // as que já estavam abertas quando o Shell começou
    EnumWindows([](HWND h, LPARAM) -> BOOL {
        wchar_t cls[32] = {}; GetClassNameW(h, cls, 32);
        if (!wcscmp(cls, L"OperationStatusWindow") && IsWindowVisible(h)) PostMessageW(g_wnd, WM_PG_SHOWN, 0, (LPARAM)h);
        return TRUE;
    }, 0);
    return true;
}

void progresso::Stop() {
    for (auto* hk : { &g_hook, &g_hookFg }) if (*hk) { UnhookWinEvent(*hk); *hk = nullptr; }
    for (auto& [pid, hk] : g_locHooks) if (hk) UnhookWinEvent(hk);
    g_locHooks.clear();
    if (g_worker.joinable()) {
        g_quit = true;
        SetEvent(g_wake);
        // a thread pode estar presa numa chamada da UI Automation a um Explorer travado: não espera para sempre
        HANDLE th = (HANDLE)g_worker.native_handle();
        if (WaitForSingleObject(th, 1500) == WAIT_OBJECT_0) g_worker.join(); else g_worker.detach();
    }
    // as janelas do Windows voltam para onde estavam
    std::lock_guard<std::mutex> l(g_mx);
    for (auto& [h, r] : g_saved) {
        RECT now;
        if (IsWindow(h) && GetWindowRect(h, &now) && OffScreen(now))
            SetWindowPos(h, nullptr, r.left, r.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    }
    g_hidden.clear();
}
