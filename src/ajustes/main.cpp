// Ajustes do Sistema: janela, barra lateral, barra de ferramentas, grupos e controles no visual do
// macOS Sequoia. As medidas vêm de capturas do Ajustes do Sequoia (janela de 715 pt de largura).
#include "ajustes.h"
#include "../menu/context_menu.h"
#include "../comum/simbolos.h"
#include "../comum/vidro.h"
#include <dwmapi.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wincodec.h>
#include <set>
#include <thread>
#include <condition_variable>
#include <mutex>
#include <cstdarg>
#include "../comum/ampliacao.h"  // botão verde com a animação do ampliar do Shell
#include "../comum/semaforo.h"
#include "../comum/leopard.h"  // tema Mac OS X Leopard: barra metálica de ponta a ponta

namespace {

// ---- Medidas (pt), tiradas da referência
// SIDE_W, SIDE_ROW e ROW_H mudam com o tema (ApplyWindowTheme). Tahoe: guia macOS26-Theme-Reference, seção 43
// (Ajustes do Sistema do macOS 26 medido com Acessibilidade): janela 801 × 625, barra lateral de 239 com 8 de
// recuo, linhas de 32, conteúdo com 20 de margem (grupos de 514) e linhas de 42 dentro dos grupos
float SIDE_W = 215;
float TOOL_H = 52;  // Leopard: 56 (a barra "unified" do 10.5, ApplyWindowTheme)
float SIDE_ROW = 28;
const float SIDE_GAP = 12;
// SEARCH_Y e ACCOUNT_Y mudam no Golden Gate (ApplyWindowTheme): busca com o centro a 75 e a conta 6 mais abaixo (captura)
float SEARCH_Y = 52, ACCOUNT_Y = 90;
const float SEARCH_H = 28, ACCOUNT_H = 50;
const float PAD = 20, GROUP_R = 8, GROUP_GAP = 11;
float ROW_H = 38;
const UINT WM_APP_REBUILD = WM_APP + 1, WM_APP_BITMAP = WM_APP + 2;
const UINT_PTR T_REFRESH = 1, T_ANIM = 2, T_CARET = 3, T_FRENTE = 4;  // T_FRENTE: vem para a frente depois de aparecer

HWND g_hwnd;
HINSTANCE g_inst;
FILE* g_log;
float g_dpi = 96, g_scale = 1;
bool g_dark, g_winActive = true, g_hoverLights;
bool g_tahoe;  // tema macOS Tahoe (liquid_glass=1): janela de cantos grandes, barra lateral flutuante, cápsulas
// macOS 27 Golden Gate (golden_gate=1 junto com o Tahoe), medido nas capturas do 512 Pixels (Ajustes do Sistema, 2x):
// barra lateral de 231 pt que vai até a borda (cor cheia 236,235,236; sem painel flutuante), linhas de 32 pt com a
// seleção na cor de destaque pura, 10 pt de recuo e raio 8; conteúdo branco com caixas (247) a ~20 pt das bordas
bool Golden() { return g_tahoe && tahoe::Golden(); }
bool g_leoAll = true;  // Leopard: "Mostrar Todos" (a grade de painéis); um painel aberto põe false
// Mac OS X Leopard (leopard=1 sem o Liquid Glass): os controles Aqua do 10.5 (caixas de seleção e pop-ups de gel azul,
// botões em pílula), o fundo cinza 232 das janelas e a seleção azul em degradê na barra lateral
bool Leo() { return !g_tahoe && tahoe::Leopard(); }
// degradê vertical com cantos (rad) — usado pelos controles Aqua
void LeoFill(ID2D1DeviceContext* dc, D2D1_RECT_F r, float rad, std::initializer_list<D2D1_GRADIENT_STOP> st) {
    ID2D1GradientStopCollection* gs = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
    if (SUCCEEDED(dc->CreateGradientStopCollection(st.begin(), (UINT32)st.size(), &gs)) &&
        SUCCEEDED(dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, r.top }, { 0, r.bottom }), gs, &lb)))
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), lb);
    if (lb) lb->Release();
    if (gs) gs->Release();
}
// gel do Aqua: claro em cima, a quebra do brilho no meio e a luz de baixo (azul) ou o branco perolado (pílula comum)
void LeoGel(ID2D1DeviceContext* dc, D2D1_RECT_F r, float rad, bool blue) {
    if (blue) LeoFill(dc, r, rad, { { 0, D2D1::ColorF(0.66f, 0.80f, 0.98f, 1) }, { 0.5f, D2D1::ColorF(0.38f, 0.60f, 0.93f, 1) },
                                    { 0.5f, D2D1::ColorF(0.24f, 0.47f, 0.87f, 1) }, { 1, D2D1::ColorF(0.58f, 0.82f, 1.0f, 1) } });
    else LeoFill(dc, r, rad, { { 0, D2D1::ColorF(1, 1, 1, 1) }, { 0.5f, D2D1::ColorF(0.95f, 0.95f, 0.95f, 1) },
                               { 0.5f, D2D1::ColorF(0.88f, 0.88f, 0.88f, 1) }, { 1, D2D1::ColorF(0.98f, 0.98f, 0.98f, 1) } });
}
float GroupR() { return g_tahoe ? 11.f : GROUP_R; }  // caixas mais redondas no Tahoe (guia: group_radius 11)
// Tahoe: estilo de ícones "transparente" (Aparência > Estilo de ícones): símbolos na cor do texto, sem o
// quadradinho colorido; na barra lateral, só o símbolo (como nas capturas do Ajustes do macOS 26)
bool ClearIcons() { return g_tahoe && aj::Setting(L"estilo_icones", L"padrao") == L"transparente"; }
bool g_tileBare;  // desenhando a barra lateral: no estilo transparente, sem fundo nenhum
float PanelR() { return std::max(10.f, tahoe::Radius(g_hwnd) - 7); }
D2D1_RECT_F PanelRect(float H) { return D2D1::RectF(7, 7, SIDE_W - 1, std::floor(H - 7)); }  // bordas no grid de pixels

IDWriteTextFormat *g_tfSmallBoldCenter, *g_tfBody, *g_tfSmall, *g_tfBold, *g_tfTitle, *g_tfGlyph, *g_tfGlyphSmall, *g_tfSide, *g_tfCenter, *g_tfSmallCenter,
    *g_tfName, *g_tfTileText;
ID2D1StrokeStyle* g_round;

// ---- Navegação
struct Entry {
    int side = 0;
    std::vector<std::pair<std::wstring, aj::Builder>> stack;  // subpáginas abertas a partir da página da barra lateral
};
std::vector<Entry> g_hist;
int g_histAt = -1;
aj::Page g_page;
float g_scroll = 0, g_scrollTarget = 0, g_contentH = 0;
float g_sideScroll = 0, g_sideH = 0;

// ---- Busca
TextField g_search;
bool g_searchFocus = false;
DWORD g_caretAt = 0;

// ---- Folha (pedido de senha do Wi-Fi e parecidos)
struct Sheet {
    bool open = false;
    std::wstring title, message, okLabel;
    bool password = true;
    TextField field;
    std::function<void(const std::wstring&)> ok;
} g_sheet;

// ---- Interação
enum HitKind { H_NONE, H_LIGHT, H_BACK, H_FWD, H_SEARCH, H_SIDE, H_ACCOUNT, H_ROW, H_TOGGLE, H_POPUP, H_SLIDER, H_LINK, H_BUTTON, H_CUSTOM,
               H_SHEET_OK, H_SHEET_CANCEL, H_SHEET_FIELD };
struct Hit {
    HitKind kind;
    D2D1_RECT_F r;
    int a = -1, b = -1;  // lado: índice; linha: grupo, linha
};
std::vector<Hit> g_hits;
int g_hoverSide = -1;
struct { int g = -1, r = -1; D2D1_RECT_F track{}; } g_drag;  // slider sendo arrastado
std::map<std::wstring, std::pair<DWORD, bool>> g_toggleAnim;  // chave -> (início, novo estado)

std::vector<int> g_sideVisible;  // índices de SidebarItems() mostrados (com a busca)

// ---- Imagens (miniaturas de fundo de tela, foto da conta)
std::mutex g_bmpMx;
std::map<std::wstring, ID2D1Bitmap*> g_bmps;
std::set<std::wstring> g_bmpPending;
struct BmpResult { std::wstring key; UINT w, h; std::vector<BYTE> px; };

std::wstring g_settingsPath;
std::map<std::wstring, std::wstring> g_settings;

void Invalidate() { if (g_hwnd) InvalidateRect(g_hwnd, nullptr, FALSE); }
double Qpf() { static LARGE_INTEGER f{}; if (!f.QuadPart) QueryPerformanceFrequency(&f); return (double)f.QuadPart; }
// medida da rolagem (log "rolagem: ..."): quadros, tempo e quanto custa desenhar um quadro
struct Rolagem { bool on = false; int n = 0; double soma = 0, pior = 0; LARGE_INTEGER ini{}, ult{}; } g_rol;
D2D1_POINT_2F ToDip(int x, int y) { return { x / g_scale, y / g_scale }; }
bool In(const D2D1_RECT_F& r, D2D1_POINT_2F p) { return p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom; }
D2D1_SIZE_F ClientDip() { RECT rc; GetClientRect(g_hwnd, &rc); return { rc.right / g_scale, rc.bottom / g_scale }; }
D2D1_COLOR_F Rgb(UINT32 c, float a = 1) { return D2D1::ColorF(((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f, a); }

bool ReadDark() {
    if (tahoe::Leopard()) return false;  // Leopard: sempre claro (o 10.5 não tinha modo escuro)
    std::wstring t = aj::Setting(L"tema", L"auto");
    if (t == L"claro") return false;
    if (t == L"escuro") return true;
    DWORD v = 1, cb = sizeof(v);
    RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &cb);
    return v == 0;
}

void LoadSettings() {
    g_settings.clear();
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, g_settingsPath.c_str(), L"r, ccs=UTF-8") || !fp) return;
    wchar_t line[512];
    while (fgetws(line, 512, fp)) {
        std::wstring s = line;
        while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ')) s.pop_back();
        size_t eq = s.find(L'=');
        if (eq != std::wstring::npos && !s.empty() && s[0] != L'#') g_settings[s.substr(0, eq)] = s.substr(eq + 1);
    }
    fclose(fp);
}

}  // namespace

void Log(const wchar_t* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fwprintf(g_log, L"%02d:%02d:%02d ", st.wHour, st.wMinute, st.wSecond);
    va_list a; va_start(a, fmt); vfwprintf(g_log, fmt, a); va_end(a);
    fputwc(L'\n', g_log); fflush(g_log);
}

// ================= API para as páginas
std::wstring aj::Setting(const wchar_t* key, const wchar_t* def) {
    auto it = g_settings.find(key);
    return it != g_settings.end() ? it->second : def;
}

void aj::SetSetting(const wchar_t* key, const std::wstring& v) {
    LoadSettings();  // o Shell também grava aqui: parte do arquivo atual
    g_settings[key] = v;
    FILE* fp = nullptr;
    if (!_wfopen_s(&fp, g_settingsPath.c_str(), L"w, ccs=UTF-8") && fp) {
        for (auto& [k, val] : g_settings) fwprintf(fp, L"%ls=%ls\n", k.c_str(), val.c_str());
        fclose(fp);
    }
    // avisa o Shell (Dock, barra de menus) para reler os ajustes
    static UINT msg = RegisterWindowMessageW(L"ShellAjustesMudaram");
    HWND sh = FindWindowExW(HWND_MESSAGE, nullptr, L"ShellMensagens", nullptr);
    if (!sh) sh = FindWindowW(L"ShellMensagens", nullptr);
    if (!sh) sh = FindWindowW(nullptr, L"ShellMensagens");
    if (sh) {
        // a mudança vai junto: o Shell grava no ajustes.txt dele (este app pode estar num contêiner
        // que desvia o %APPDATA% para outra cópia) e recarrega; sem resposta, só o aviso antigo
        std::wstring kv = std::wstring(key) + L"=" + v;
        COPYDATASTRUCT cd{ 0x534A5354, (DWORD)((kv.size() + 1) * sizeof(wchar_t)), (void*)kv.c_str() };
        DWORD_PTR r = 0;
        if (!SendMessageTimeoutW(sh, WM_COPYDATA, (WPARAM)g_hwnd, (LPARAM)&cd, SMTO_ABORTIFHUNG, 500, &r) || !r)
            PostMessageW(sh, msg, 0, 0);
    }
    if (wcscmp(key, L"tema") == 0 || wcscmp(key, L"destaque") == 0) { g_dark = ReadDark(); }
}

void aj::Rebuild() { if (g_hwnd) PostMessageW(g_hwnd, WM_APP_REBUILD, 0, 0); }
void aj::Redraw() { Invalidate(); }
HWND aj::Window() { return g_hwnd; }
bool aj::Dark() { return g_dark; }

D2D1_COLOR_F aj::Accent() {
    std::wstring d = Setting(L"destaque", L"azul");
    static const std::pair<const wchar_t*, UINT32> cols[] = {
        { L"azul", 0x007AFF }, { L"roxo", 0x953D96 }, { L"rosa", 0xF74F9E }, { L"vermelho", 0xE0383E },
        { L"laranja", 0xF7821B }, { L"amarelo", 0xFCB827 }, { L"verde", 0x62BA46 }, { L"grafite", 0x8C8C8C }, { L"lavanda", 0x98A8D9 } };  // lavanda: a 10ª cor do macOS 27
    for (auto& [n, c] : cols) if (d == n) return Rgb(c);
    return Rgb(g_dark ? 0x0A84FF : 0x007AFF);
}

void aj::OpenUri(const std::wstring& uri) {
    if (uri.rfind(L"ms-settings:", 0) == 0) {
        IApplicationActivationManager* am = nullptr;
        IShellItem* it = nullptr;
        IShellItemArray* arr = nullptr;
        DWORD pid = 0;
        bool ok = SUCCEEDED(CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&am))) &&
                  SUCCEEDED(SHCreateItemFromParsingName(uri.c_str(), nullptr, IID_PPV_ARGS(&it))) &&
                  SUCCEEDED(SHCreateShellItemArrayFromShellItem(it, IID_PPV_ARGS(&arr)));
        if (ok) {
            CoAllowSetForegroundWindow(am, nullptr);
            ok = SUCCEEDED(am->ActivateForProtocol(L"windows.immersivecontrolpanel_cw5n1h2txyewy!microsoft.windows.immersivecontrolpanel", arr, &pid));
        }
        {   // registro curto (para saber, na VM, por onde o link saiu)
            wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
            FILE* f = nullptr;
            if (!_wfopen_s(&f, (std::wstring(tmp) + L"ajustes-uri.log").c_str(), L"a, ccs=UTF-8") && f) {
                fwprintf(f, L"%ls: gerenciador %d, item %d, lista %d, ativou %d (pid %lu)\n", uri.c_str(), am != nullptr, it != nullptr, arr != nullptr, (int)ok, pid);
                fclose(f);
            }
        }
        if (arr) arr->Release();
        if (it) it->Release();
        if (am) am->Release();
        if (ok) return;
    }
    ShellExecuteW(nullptr, L"open", uri.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void aj::Push(const std::wstring& title, Builder b) {
    if (g_histAt < 0) return;
    Entry e = g_hist[g_histAt];
    e.stack.push_back({ title, b });
    g_hist.resize(g_histAt + 1);
    g_hist.push_back(e);
    g_histAt++;
    g_page = b();
    g_page.title = title;
    g_scroll = g_scrollTarget = 0;
    Invalidate();
}

void aj::Text(Ctx& c, const std::wstring& s, D2D1_RECT_F r, IDWriteTextFormat* tf, D2D1_COLOR_F col) {
    c.br->SetColor(col);
    std::wstring t = Tr(s);  // todo texto das páginas passa por aqui: o idioma (Geral > Idioma) se aplica no desenho
    c.dc->DrawTextW(t.c_str(), (UINT32)t.size(), tf, r, c.br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void aj::Tile(Ctx& c, D2D1_RECT_F r, wchar_t glyph, UINT32 color, const std::wstring& symbol) {
    float w = r.right - r.left;
    // Tahoe: os ícones seguem Aparência > Estilo de ícones e widgets, como no Ajustes do macOS 26 — Escuro (quadrado
    // grafite com o símbolo na cor dele), Transparente (vidro claro, símbolo claro) e Tingido (vidro na cor de destaque)
    std::wstring est = g_tahoe ? aj::Setting(L"estilo_icones", L"padrao") : L"padrao";
    if (est == L"escuro" && aj::Setting(L"estilo_escuro_auto", L"0") == L"1" && !g_dark) est = L"padrao";  // macOS 27: Escuro "Auto"
    if (est == L"escuro" || est == L"transparente" || est == L"tingido") {
        auto rr = D2D1::RoundedRect(r, w * 0.24f, w * 0.24f);
        D2D1_COLOR_F ink = D2D1::ColorF(1, 1, 1, 1);
        if (est == L"escuro") {
            c.br->SetColor(D2D1::ColorF(0.17f, 0.17f, 0.18f, 1));
            c.dc->FillRoundedRectangle(rr, c.br);
            ink = Rgb(color);
            ink.r = std::min(1.f, ink.r * 1.15f + 0.08f); ink.g = std::min(1.f, ink.g * 1.15f + 0.08f); ink.b = std::min(1.f, ink.b * 1.15f + 0.08f);
        } else {
            D2D1_COLOR_F acc = aj::Accent();
            if (est == L"tingido") c.br->SetColor(D2D1::ColorF(acc.r, acc.g, acc.b, c.dark ? 0.62f : 0.72f));
            else c.br->SetColor(c.dark ? D2D1::ColorF(1, 1, 1, 0.14f) : D2D1::ColorF(1, 1, 1, 0.55f));
            c.dc->FillRoundedRectangle(rr, c.br);
            vidro::Borda(c.dc, r, w * 0.24f, 1, c.dark, 1, true);  // o vidro: fio claro e brilho nos cantos
            if (est == L"transparente" && !c.dark) ink = D2D1::ColorF(0.30f, 0.30f, 0.33f, 1);
        }
        float m = w * 0.19f;
        c.br->SetColor(ink);
        if (!symbol.empty() && simbolo::Draw(c.dc, symbol, D2D1::RectF(r.left + m, r.top + m, r.right - m, r.bottom - m), c.br)) return;
        if (glyph == 1) {
            D2D1_POINT_2F mm{ (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
            float rad = w * 0.3f;
            c.dc->DrawEllipse(D2D1::Ellipse(mm, rad, rad), c.br, 1.2f);
            c.dc->PushAxisAlignedClip(D2D1::RectF(mm.x, mm.y - rad - 1, mm.x + rad + 1, mm.y + rad + 1), D2D1_ANTIALIAS_MODE_ALIASED);
            c.dc->FillEllipse(D2D1::Ellipse(mm, rad, rad), c.br);
            c.dc->PopAxisAlignedClip();
            return;
        }
        if (glyph) Text(c, std::wstring(1, glyph), r, glyph < 0xE000 ? g_tfTileText : (w > 22 ? c.glyph : c.glyphSmall), ink);
        return;
    }
    if (ClearIcons()) {
        if (!g_tileBare) {  // no conteúdo: quadradinho cinza translúcido com o símbolo claro
            c.br->SetColor(c.dark ? D2D1::ColorF(1, 1, 1, 0.10f) : D2D1::ColorF(0, 0, 0, 0.06f));
            c.dc->FillRoundedRectangle(D2D1::RoundedRect(r, w * 0.24f, w * 0.24f), c.br);
        }
        D2D1_COLOR_F ink = c.dark ? D2D1::ColorF(1, 1, 1, 0.85f) : D2D1::ColorF(0, 0, 0, 0.72f);
        float m = w * (g_tileBare ? 0.14f : 0.19f);
        if (!symbol.empty()) {
            c.br->SetColor(ink);
            if (simbolo::Draw(c.dc, symbol, D2D1::RectF(r.left + m, r.top + m, r.right - m, r.bottom - m), c.br)) return;
        }
        if (glyph == 1) {
            D2D1_POINT_2F mm{ (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
            float rad = w * 0.3f;
            c.br->SetColor(ink);
            c.dc->DrawEllipse(D2D1::Ellipse(mm, rad, rad), c.br, 1.2f);
            c.dc->PushAxisAlignedClip(D2D1::RectF(mm.x, mm.y - rad - 1, mm.x + rad + 1, mm.y + rad + 1), D2D1_ANTIALIAS_MODE_ALIASED);
            c.dc->FillEllipse(D2D1::Ellipse(mm, rad, rad), c.br);
            c.dc->PopAxisAlignedClip();
            return;
        }
        if (glyph) Text(c, std::wstring(1, glyph), r, glyph < 0xE000 ? g_tfTileText : (w > 22 ? c.glyph : c.glyphSmall), ink);
        return;
    }
    // quadradinho com gradiente leve (mais claro em cima), como os ícones do Ajustes do Mac
    ID2D1GradientStopCollection* stops = nullptr;
    D2D1_COLOR_F top = Rgb(color), bot = Rgb(color);
    top.r = std::min(1.f, top.r * 1.08f + 0.05f); top.g = std::min(1.f, top.g * 1.08f + 0.05f); top.b = std::min(1.f, top.b * 1.08f + 0.05f);
    D2D1_GRADIENT_STOP gs[2] = { { 0, top }, { 1, bot } };
    c.dc->CreateGradientStopCollection(gs, 2, &stops);
    ID2D1LinearGradientBrush* lg = nullptr;
    if (stops) c.dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ 0, r.top }, { 0, r.bottom }), stops, &lg);
    auto rr = D2D1::RoundedRect(r, w * 0.24f, w * 0.24f);
    if (lg) c.dc->FillRoundedRectangle(rr, lg); else { c.br->SetColor(Rgb(color)); c.dc->FillRoundedRectangle(rr, c.br); }
    if (lg) lg->Release();
    if (stops) stops->Release();
    if (!symbol.empty()) {  // SF Symbol branco, ~62% do quadradinho (como no Ajustes do Mac)
        float m = w * 0.19f;
        c.br->SetColor(D2D1::ColorF(1, 1, 1, 1));
        if (simbolo::Draw(c.dc, symbol, D2D1::RectF(r.left + m, r.top + m, r.right - m, r.bottom - m), c.br)) return;
    }
    if (glyph == 1) {  // Aparência: círculo metade preto, metade branco
        D2D1_POINT_2F m{ (r.left + r.right) / 2, (r.top + r.bottom) / 2 };
        float rad = w * 0.3f;
        c.br->SetColor(D2D1::ColorF(1, 1, 1, 1));
        c.dc->DrawEllipse(D2D1::Ellipse(m, rad, rad), c.br, 1.2f);
        c.dc->PushAxisAlignedClip(D2D1::RectF(m.x, m.y - rad - 1, m.x + rad + 1, m.y + rad + 1), D2D1_ANTIALIAS_MODE_ALIASED);
        c.dc->FillEllipse(D2D1::Ellipse(m, rad, rad), c.br);
        c.dc->PopAxisAlignedClip();
        return;
    }
    if (!glyph) return;
    bool text = glyph < 0xE000;  // letra comum (@): fonte do texto
    Text(c, std::wstring(1, glyph), r, text ? g_tfTileText : (w > 22 ? c.glyph : c.glyphSmall), D2D1::ColorF(1, 1, 1, 1));
}

ID2D1Bitmap* aj::BitmapFromFile(const std::wstring& path, UINT maxPx) {
    std::wstring key = path + L"|" + std::to_wstring(maxPx);
    {
        std::lock_guard<std::mutex> l(g_bmpMx);
        auto it = g_bmps.find(key);
        if (it != g_bmps.end()) return it->second;
        if (g_bmpPending.count(key)) return nullptr;
        g_bmpPending.insert(key);
    }
    HWND w = g_hwnd;
    std::thread([path, maxPx, key, w] {
        // no máximo 3 de cada vez: os papéis de parede do Mac têm até 8K, e decodificar dezenas ao mesmo
        // tempo comia gigabytes de memória
        static std::mutex semMx;
        static std::condition_variable semCv;
        static int running = 0;
        { std::unique_lock<std::mutex> l(semMx); semCv.wait(l, [] { return running < 3; }); running++; }
        struct Release { ~Release() { { std::lock_guard<std::mutex> l(semMx); running--; } semCv.notify_one(); } } rel;
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto* res = new BmpResult{ key, 0, 0, {} };
        // miniatura: primeiro a do cache do Windows (rápida, sem decodificar a imagem inteira)
        if (maxPx <= 512) {
            IShellItemImageFactory* sf = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&sf)))) {
                HBITMAP hb = nullptr;
                if (SUCCEEDED(sf->GetImage(SIZE{ (int)maxPx, (int)maxPx }, SIIGBF_RESIZETOFIT | SIIGBF_THUMBNAILONLY, &hb)) && hb) {
                    BITMAP bm{}; GetObjectW(hb, sizeof(bm), &bm);
                    UINT bw = bm.bmWidth, bh = std::abs(bm.bmHeight);
                    BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), (LONG)bw, -(LONG)bh, 1, 32, BI_RGB };
                    res->px.resize((size_t)bw * bh * 4);
                    HDC sdc = GetDC(nullptr);
                    if (GetDIBits(sdc, hb, 0, bh, res->px.data(), &bi, DIB_RGB_COLORS) == (int)bh) {
                        res->w = bw; res->h = bh;
                        bool anyA = false;
                        for (size_t i = 3; i < res->px.size(); i += 4) if (res->px[i]) { anyA = true; break; }
                        if (!anyA) for (size_t i = 3; i < res->px.size(); i += 4) res->px[i] = 255;  // foto: sem alfa
                    } else res->px.clear();
                    ReleaseDC(nullptr, sdc);
                    DeleteObject(hb);
                }
                sf->Release();
            }
        }
        IWICImagingFactory* f = nullptr;
        if (res->px.empty() && SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) {
            IWICBitmapDecoder* dec = nullptr;
            IWICBitmapFrameDecode* fr = nullptr;
            if (SUCCEEDED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr))) {
                UINT w0, h0; fr->GetSize(&w0, &h0);
                float k = std::min(1.f, (float)maxPx / std::max(w0, h0));
                UINT w1 = std::max(1u, (UINT)(w0 * k)), h1 = std::max(1u, (UINT)(h0 * k));
                IWICBitmapScaler* sc = nullptr;
                IWICFormatConverter* cv = nullptr;
                if (SUCCEEDED(f->CreateBitmapScaler(&sc)) && SUCCEEDED(sc->Initialize(fr, w1, h1, WICBitmapInterpolationModeFant)) &&
                    SUCCEEDED(f->CreateFormatConverter(&cv)) &&
                    SUCCEEDED(cv->Initialize(sc, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
                    res->w = w1; res->h = h1; res->px.resize((size_t)w1 * h1 * 4);
                    if (FAILED(cv->CopyPixels(nullptr, w1 * 4, (UINT)res->px.size(), res->px.data()))) res->px.clear();
                }
                if (cv) cv->Release();
                if (sc) sc->Release();
            }
            if (fr) fr->Release();
            if (dec) dec->Release();
            f->Release();
        }
        if (!PostMessageW(w, WM_APP_BITMAP, 0, (LPARAM)res)) delete res;
        CoUninitialize();
    }).detach();
    return nullptr;
}

namespace {

// ================= Fontes
IDWriteTextFormat* Format(const wchar_t* family, float size, DWRITE_FONT_WEIGHT weight, DWRITE_TEXT_ALIGNMENT a, bool wrap = false) {
    IDWriteTextFormat* tf = nullptr;
    gfx::dw->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"pt-br", &tf);
    if (!tf) return nullptr;
    tf->SetTextAlignment(a);
    tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    tf->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (!wrap) {
        IDWriteInlineObject* ell = nullptr;
        gfx::dw->CreateEllipsisTrimmingSign(tf, &ell);
        DWRITE_TRIMMING tr{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        tf->SetTrimming(&tr, ell);
        if (ell) ell->Release();
    }
    return tf;
}

std::wstring PickFamily(std::initializer_list<const wchar_t*> names) {
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

void CreateFormats() {
    // SF Pro só se estiver instalada neste PC (a licença não permite distribuir); senão a Inter.
    std::wstring ui = PickFamily({ L"SF Pro Text", L"Inter", L"Segoe UI Variable Text", L"Segoe UI" });
    std::wstring disp = PickFamily({ L"SF Pro Display", L"SF Pro Text", L"Inter", L"Segoe UI" });
    DWRITE_FONT_WEIGHT body = ui == L"SF Pro Text" ? DWRITE_FONT_WEIGHT_NORMAL : DWRITE_FONT_WEIGHT_MEDIUM;
    std::wstring icons = PickFamily({ L"Segoe Fluent Icons", L"Segoe MDL2 Assets" });
    const auto L = DWRITE_TEXT_ALIGNMENT_LEADING, C = DWRITE_TEXT_ALIGNMENT_CENTER;
    g_tfBody = Format(ui.c_str(), 13, body, L);
    g_tfSide = Format(ui.c_str(), 13, body, L);
    g_tfSmall = Format(ui.c_str(), 11, body, L, true);
    g_tfBold = Format(ui.c_str(), 13, DWRITE_FONT_WEIGHT_SEMI_BOLD, L);
    g_tfTitle = Format(disp.c_str(), 15, DWRITE_FONT_WEIGHT_BOLD, L);
    g_tfName = Format(disp.c_str(), 22, DWRITE_FONT_WEIGHT_BOLD, C);
    g_tfCenter = Format(ui.c_str(), 13, body, C);
    g_tfSmallCenter = Format(ui.c_str(), 11, body, C);
    g_tfSmallBoldCenter = Format(ui.c_str(), 11, DWRITE_FONT_WEIGHT_SEMI_BOLD, C);  // uma linha só, com "…" (nomes das imagens de fundo)
    g_tfGlyph = Format(icons.c_str(), 14, DWRITE_FONT_WEIGHT_NORMAL, C);
    g_tfGlyphSmall = Format(icons.c_str(), 11.5f, DWRITE_FONT_WEIGHT_NORMAL, C);
    g_tfTileText = Format(ui.c_str(), 13, DWRITE_FONT_WEIGHT_BOLD, C);
}

float TextWidth(const std::wstring& s0, IDWriteTextFormat* tf) {
    IDWriteTextLayout* tl = nullptr;
    float w = 0;
    std::wstring s = Tr(s0);  // mede o texto como aj::Text vai desenhá-lo
    if (SUCCEEDED(gfx::dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), tf, 4000, 100, &tl))) {
        DWRITE_TEXT_METRICS m{}; tl->GetMetrics(&m); w = m.widthIncludingTrailingWhitespace; tl->Release();
    }
    return w;
}

float TextHeight(const std::wstring& s0, IDWriteTextFormat* tf, float width) {
    IDWriteTextLayout* tl = nullptr;
    float h = 0;
    std::wstring s = Tr(s0);
    if (SUCCEEDED(gfx::dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), tf, std::max(width, 10.f), 4000, &tl))) {
        tl->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        DWRITE_TEXT_METRICS m{}; tl->GetMetrics(&m); h = m.height; tl->Release();
    }
    return h;
}

// ================= Cores
struct Pal {
    D2D1_COLOR_F side, content, text, text2, text3, line, group, groupLine, field, control, sideSel, sideSelText, divider;
};
Pal ColorsBase();
Pal Colors() {  // Tahoe: o fundo da janela leva 10 % da cor do papel de parede (tahoe::Tint)
    Pal p = ColorsBase();
    if (Golden()) {
        // medido: claro = barra (236,235,236), conteúdo branco, caixas 247; escuro = conteúdo (29,28,29), caixas (49,48,48),
        // barra lateral (43,41,40); seleção na cor de destaque pura (0,122,255 no azul)
        p.side = g_dark ? D2D1::ColorF(43 / 255.f, 41 / 255.f, 40 / 255.f, 1) : D2D1::ColorF(236 / 255.f, 235 / 255.f, 236 / 255.f, 1);
        p.content = g_dark ? D2D1::ColorF(29 / 255.f, 28 / 255.f, 29 / 255.f, 1) : D2D1::ColorF(1, 1, 1, 1);
        p.group = g_dark ? D2D1::ColorF(1, 1, 1, 0.088f) : D2D1::ColorF(0, 0, 0, 0.031f);
        p.divider = g_dark ? D2D1::ColorF(0, 0, 0, 0.5f) : D2D1::ColorF(0, 0, 0, 0.06f);
        if (g_winActive) p.sideSel = aj::Accent();
        return p;
    }
    if (g_tahoe) p.content = tahoe::Tint(p.content);
    if (!g_tahoe && tahoe::Leopard()) {  // Leopard: barra lateral azul-acinzentada, janela cinza 232, caixas gravadas
        p.side = leopard::Sidebar(g_winActive, g_dark);
        p.content = D2D1::ColorF(232 / 255.f, 232 / 255.f, 232 / 255.f, 1);
        p.group = D2D1::ColorF(0, 0, 0, 0.04f);
        p.groupLine = D2D1::ColorF(0, 0, 0, 0.14f);
        p.line = D2D1::ColorF(0, 0, 0, 0.10f);
        p.sideSelText = D2D1::ColorF(1, 1, 1, 1);
    }
    return p;
}
Pal ColorsBase() {
    D2D1_COLOR_F acc = aj::Accent();
    D2D1_COLOR_F sel = g_dark ? D2D1::ColorF(acc.r * 0.82f, acc.g * 0.82f, acc.b * 0.82f, 1) : D2D1::ColorF(acc.r * 0.9f, acc.g * 0.9f, acc.b * 0.9f, 1);
    if (!g_winActive) sel = g_dark ? D2D1::ColorF(1, 1, 1, 0.12f) : D2D1::ColorF(0, 0, 0, 0.09f);
    if (g_dark)
        return { D2D1::ColorF(0, 0, 0, 0.22f), D2D1::ColorF(0.122f, 0.125f, 0.137f, 1), D2D1::ColorF(1, 1, 1, 0.88f), D2D1::ColorF(1, 1, 1, 0.55f),
                 D2D1::ColorF(1, 1, 1, 0.28f), D2D1::ColorF(1, 1, 1, 0.08f), D2D1::ColorF(1, 1, 1, 0.035f), D2D1::ColorF(1, 1, 1, 0.07f),
                 D2D1::ColorF(1, 1, 1, 0.07f), D2D1::ColorF(1, 1, 1, 0.16f), sel, D2D1::ColorF(1, 1, 1, 1), D2D1::ColorF(0, 0, 0, 0.55f) };
    return { D2D1::ColorF(0.95f, 0.95f, 0.95f, 0.25f), D2D1::ColorF(0.925f, 0.925f, 0.925f, 1), D2D1::ColorF(0, 0, 0, 0.85f), D2D1::ColorF(0, 0, 0, 0.50f),
             D2D1::ColorF(0, 0, 0, 0.25f), D2D1::ColorF(0, 0, 0, 0.08f), D2D1::ColorF(0, 0, 0, 0.028f), D2D1::ColorF(0, 0, 0, 0.055f),
             D2D1::ColorF(0, 0, 0, 0.06f), D2D1::ColorF(1, 1, 1, 1), g_winActive ? sel : D2D1::ColorF(0, 0, 0, 0.09f), g_winActive ? D2D1::ColorF(1, 1, 1, 1) : D2D1::ColorF(0, 0, 0, 0.85f),
             D2D1::ColorF(0, 0, 0, 0.12f) };
}

void Line(ID2D1DeviceContext* dc, ID2D1Brush* b, float x1, float y1, float x2, float y2, float w = 1.5f) {
    dc->DrawLine({ x1, y1 }, { x2, y2 }, b, w, g_round);
}

// ================= Navegação
void ShowPage() {
    if (g_histAt < 0) return;
    Entry& e = g_hist[g_histAt];
    auto& items = SidebarItems();
    if (e.side < 0 || e.side >= (int)items.size()) e.side = 0;
    if (e.stack.empty()) { g_page = items[e.side].build(); g_page.title = items[e.side].name; }
    else { g_page = e.stack.back().second(); g_page.title = e.stack.back().first; }
}

void SelectSide(int i) {
    auto& items = SidebarItems();
    if (i < 0 || i >= (int)items.size()) return;
    if (g_leoAll) { g_leoAll = false; g_search.SetText(L"", false); g_searchFocus = false; Invalidate(); }  // Leopard: abriu um painel
    if (g_histAt >= 0 && g_hist[g_histAt].side == i && g_hist[g_histAt].stack.empty()) return;
    g_hist.resize(g_histAt + 1);
    g_hist.push_back({ i, {} });
    g_histAt++;
    ShowPage();
    g_scroll = g_scrollTarget = 0;
    aj::SetSetting(L"ajustes_pagina", items[i].id);
    Invalidate();
}

void Go(int delta) {
    int to = g_histAt + delta;
    if (to < 0 || to >= (int)g_hist.size()) return;
    g_histAt = to;
    ShowPage();
    g_scroll = g_scrollTarget = 0;
    Invalidate();
}

void UpdateSideVisible() {
    g_sideVisible.clear();
    auto& items = SidebarItems();
    std::wstring q = g_search.text;
    for (auto& ch : q) ch = towlower(ch);
    for (int i = 0; i < (int)items.size(); i++) {
        if (items[i].id == L"conta") continue;  // é o cabeçalho da conta, no topo
        if (!q.empty()) {
            std::wstring hay = items[i].name + L" " + Tr(items[i].name) + L" " + items[i].keywords;
            for (auto& ch : hay) ch = towlower(ch);
            if (hay.find(q) == std::wstring::npos) continue;
        }
        g_sideVisible.push_back(i);
    }
}

// ================= Layout do conteúdo
float RowHeight(const aj::Row& r, float w) {
    if (r.kind == aj::Kind::Custom || r.height > 0) return r.height;
    float h = r.icon ? (Golden() ? 49.f : 42.f) : ROW_H;
    if (r.kind == aj::Kind::Slider && r.hasDef) h += 14;  // espaço para a marca "Padrão" embaixo do trilho
    if (!r.detail.empty()) {
        float textW = w - 20 - (r.icon ? 34 : 0) - (r.kind == aj::Kind::Toggle ? 50 : r.kind == aj::Kind::Button ? 120 : 0);
        h = std::max(h, 12 + 17 + TextHeight(r.detail, g_tfSmall, textW) + 9);
    }
    return h;
}

struct Placed { int g, r; D2D1_RECT_F rect; };

float HeaderHeight(const aj::Group& g, float w) {
    if (g.title.empty() && g.subtitle.empty()) return 0;
    float h = 0;
    if (!g.title.empty()) h += 18;
    if (!g.subtitle.empty()) h += TextHeight(g.subtitle, g_tfSmall, w - 20) + 2;
    return h + 7;
}

// ================= Desenho dos controles
void DrawToggle(aj::Ctx& c, D2D1_RECT_F r, bool on, float t /*0..1 animação*/, bool enabled) {
    if (Leo()) {  // Leopard: caixa de seleção Aqua de 14 px (gel azul com o visto branco ligada; perolada desligada)
        (void)t;
        float x = r.right - 10 - 14, y = (r.top + r.bottom) / 2 - 7;
        D2D1_RECT_F b = D2D1::RectF(x, y, x + 14, y + 14);
        c.br->SetColor(D2D1::ColorF(1, 1, 1, 0.6f));
        c.dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(b.left, b.top + 1, b.right, b.bottom + 1), 3, 3), c.br);
        LeoGel(c.dc, b, 3, on);
        c.br->SetColor(on ? D2D1::ColorF(0.12f, 0.27f, 0.58f, enabled ? 0.9f : 0.4f) : D2D1::ColorF(0, 0, 0, enabled ? 0.42f : 0.2f));
        c.dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(b.left + 0.5f, b.top + 0.5f, b.right - 0.5f, b.bottom - 0.5f), 2.5f, 2.5f), c.br, 1);
        if (on) {
            c.br->SetColor(D2D1::ColorF(0.05f, 0.08f, 0.15f, enabled ? 0.95f : 0.4f));  // o visto preto do Aqua
            Line(c.dc, c.br, x + 3.2f, y + 7.2f, x + 6, y + 10.5f, 2.0f);
            Line(c.dc, c.br, x + 6, y + 10.5f, x + 11.5f, y + 2.8f, 2.0f);
        }
        return;
    }
    if (g_tahoe) {  // Tahoe (guia macOS26-Theme-Reference, seção 43): switch pequeno de 36 × 16, o pino uma pílula branca
        const float TW = 36, TH = 16;
        float x = r.right - 10 - TW, y = (r.top + r.bottom) / 2 - TH / 2;
        float pos = on ? t : 1 - t;                     // pode passar um pouco de 1 (a mola do switch passa 3 %)
        float pc = std::clamp(pos, 0.f, 1.f);
        D2D1_COLOR_F off = g_dark ? D2D1::ColorF(1, 1, 1, 0.18f) : D2D1::ColorF(0, 0, 0, 0.10f), onc = c.accent;
        D2D1_COLOR_F col{ off.r + (onc.r - off.r) * pc, off.g + (onc.g - off.g) * pc, off.b + (onc.b - off.b) * pc, off.a + (onc.a - off.a) * pc };
        if (!enabled) col.a *= 0.5f;
        c.br->SetColor(col);
        c.dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y, x + TW, y + TH), TH / 2, TH / 2), c.br);
        float kw = 20, kx = x + 2 + pos * (TW - 4 - kw);
        D2D1_RECT_F k = D2D1::RectF(kx, y + 2, kx + kw, y + TH - 2);
        float kr = (TH - 4) / 2;
        c.br->SetColor(D2D1::ColorF(0, 0, 0, 0.16f));
        c.dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(k.left, k.top + 0.6f, k.right, k.bottom + 0.6f), kr, kr), c.br);
        c.br->SetColor(D2D1::ColorF(1, 1, 1, enabled ? 1 : 0.7f));
        c.dc->FillRoundedRectangle(D2D1::RoundedRect(k, kr, kr), c.br);
        return;
    }
    // chave "mini" do Ajustes do Sequoia: 26 x 15
    float x = r.right - 10 - 26, y = (r.top + r.bottom) / 2 - 7.5f;
    float pos = on ? t : 1 - t;  // 0 = desligada, 1 = ligada
    D2D1_COLOR_F off = g_dark ? D2D1::ColorF(1, 1, 1, 0.16f) : D2D1::ColorF(0, 0, 0, 0.09f);
    D2D1_COLOR_F onc = c.accent;
    D2D1_COLOR_F col{ off.r + (onc.r - off.r) * pos, off.g + (onc.g - off.g) * pos, off.b + (onc.b - off.b) * pos, off.a + (onc.a - off.a) * pos };
    if (!enabled) col.a *= 0.5f;
    auto track = D2D1::RoundedRect(D2D1::RectF(x, y, x + 26, y + 15), 7.5f, 7.5f);
    c.br->SetColor(col);
    c.dc->FillRoundedRectangle(track, c.br);
    if (!g_dark && pos < 0.5f) { c.br->SetColor(D2D1::ColorF(0, 0, 0, 0.06f)); c.dc->DrawRoundedRectangle(track, c.br, 0.6f); }
    float kx = x + 7.5f + pos * 11;
    c.br->SetColor(D2D1::ColorF(0, 0, 0, 0.18f));
    c.dc->FillEllipse(D2D1::Ellipse({ kx, y + 7.9f }, 6.8f, 6.8f), c.br);
    c.br->SetColor(D2D1::ColorF(1, 1, 1, enabled ? 1 : 0.7f));
    c.dc->FillEllipse(D2D1::Ellipse({ kx, y + 7.5f }, 6.5f, 6.5f), c.br);
}

void DrawUpDown(aj::Ctx& c, float cx, float cy, D2D1_COLOR_F col) {
    c.br->SetColor(col);
    Line(c.dc, c.br, cx - 3, cy - 1.8f, cx, cy - 4.6f, 1.25f);
    Line(c.dc, c.br, cx, cy - 4.6f, cx + 3, cy - 1.8f, 1.25f);
    Line(c.dc, c.br, cx - 3, cy + 1.8f, cx, cy + 4.6f, 1.25f);
    Line(c.dc, c.br, cx, cy + 4.6f, cx + 3, cy + 1.8f, 1.25f);
}

void DrawChevronRight(aj::Ctx& c, float cx, float cy, D2D1_COLOR_F col) {
    c.br->SetColor(col);
    Line(c.dc, c.br, cx - 2, cy - 4.5f, cx + 2.5f, cy, 1.5f);
    Line(c.dc, c.br, cx + 2.5f, cy, cx - 2, cy + 4.5f, 1.5f);
}

D2D1_RECT_F ButtonRect(const std::wstring& label, float right, float cy) {
    // Tahoe (guia, seção 11): botão regular de 24 em cápsula (raio = altura / 2)
    float w = std::max(TextWidth(label, g_tfCenter) + (g_tahoe ? 28 : 22), 60.f), h = g_tahoe ? 12.f : 11.f;
    return D2D1::RectF(right - w, cy - h, right, cy + h);
}

void DrawButton(aj::Ctx& c, D2D1_RECT_F b, const std::wstring& label, bool enabled, bool primary = false) {
    if (Leo()) {  // Leopard: pílula Aqua perolada (o botão padrão em gel azul), contorno cinza e o relevo branco embaixo
        float rad = (b.bottom - b.top) / 2;
        c.br->SetColor(D2D1::ColorF(1, 1, 1, 0.6f));
        c.dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(b.left, b.top + 1, b.right, b.bottom + 1), rad, rad), c.br);
        LeoGel(c.dc, b, rad, primary);
        c.br->SetColor(primary ? D2D1::ColorF(0.12f, 0.27f, 0.58f, 0.9f) : D2D1::ColorF(0, 0, 0, 0.40f));
        c.dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(b.left + 0.5f, b.top + 0.5f, b.right - 0.5f, b.bottom - 0.5f), rad - 0.5f, rad - 0.5f), c.br, 1);
        aj::Text(c, label, b, g_tfCenter, D2D1::ColorF(0, 0, 0, enabled ? 0.9f : 0.35f));
        return;
    }
    if (g_tahoe && primary) {  // Tahoe: o botão padrão também é cápsula, na cor de destaque
        float rad = (b.bottom - b.top) / 2;
        c.br->SetColor(c.accent);
        c.dc->FillRoundedRectangle(D2D1::RoundedRect(b, rad, rad), c.br);
        aj::Text(c, label, b, g_tfCenter, D2D1::ColorF(1, 1, 1, enabled ? 1 : 0.5f));
        return;
    }
    if (g_tahoe && !primary) {  // Tahoe: cápsula cinza translúcida, sem sombra nem borda
        float rad = (b.bottom - b.top) / 2;
        c.br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.12f) : D2D1::ColorF(0, 0, 0, 0.07f));
        c.dc->FillRoundedRectangle(D2D1::RoundedRect(b, rad, rad), c.br);
        D2D1_COLOR_F t = c.text; if (!enabled) t.a *= 0.45f;
        aj::Text(c, label, b, g_tfCenter, t);
        return;
    }
    auto rr = D2D1::RoundedRect(b, 5.5f, 5.5f);
    if (primary) {
        c.br->SetColor(c.accent);
        c.dc->FillRoundedRectangle(rr, c.br);
        aj::Text(c, label, b, g_tfCenter, D2D1::ColorF(1, 1, 1, enabled ? 1 : 0.5f));
        return;
    }
    if (!g_dark) {  // botão branco com sombra fina embaixo
        c.br->SetColor(D2D1::ColorF(0, 0, 0, 0.10f));
        c.dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(b.left, b.top + 0.5f, b.right, b.bottom + 0.8f), 5.5f, 5.5f), c.br);
        c.br->SetColor(D2D1::ColorF(1, 1, 1, 1));
        c.dc->FillRoundedRectangle(rr, c.br);
        c.br->SetColor(D2D1::ColorF(0, 0, 0, 0.08f));
        c.dc->DrawRoundedRectangle(rr, c.br, 0.6f);
    } else {
        c.br->SetColor(D2D1::ColorF(1, 1, 1, 0.17f));
        c.dc->FillRoundedRectangle(rr, c.br);
        c.br->SetColor(D2D1::ColorF(1, 1, 1, 0.06f));
        c.dc->DrawLine({ b.left + 4, b.top + 0.5f }, { b.right - 4, b.top + 0.5f }, c.br, 0.8f);
    }
    D2D1_COLOR_F t = c.text; if (!enabled) t.a *= 0.45f;
    aj::Text(c, label, b, g_tfCenter, t);
}

float SliderValue(const aj::Row& r) { return r.getF ? std::clamp((r.getF() - r.min) / std::max(r.max - r.min, 0.0001f), 0.f, 1.f) : 0; }

// trilho do slider (à direita da linha, com espaço para os ícones/rótulos das pontas)
D2D1_RECT_F SliderTrack(const aj::Row& r, D2D1_RECT_F row) {
    // macOS 27 (Ajustes > Aparência, 2x): trilho de 5,75 de 213 da borda da caixa até 33,5 da outra, centro 15,75 abaixo
    // do topo da linha
    if (r.gg) return D2D1::RectF(row.left + 213, row.top + 15.75f - 2.875f, row.right - 33.5f, row.top + 15.75f + 2.875f);
    float maxW = r.maxGlyph.empty() ? 0 : (r.maxGlyph[0] >= 0xE000 ? 22 : TextWidth(r.maxGlyph, g_tfSmall) + 10);
    float minW = r.minGlyph.empty() ? 0 : (r.minGlyph[0] >= 0xE000 ? 22 : TextWidth(r.minGlyph, g_tfSmall) + 10);
    float right = row.right - 12 - maxW;
    float w = std::min(240.f, (row.right - row.left) * 0.5f);
    float left = right - w;
    float labelEnd = row.left + 10 + TextWidth(r.label, g_tfBody) + 16;  // o rótulo nunca fica embaixo do slider
    if (left - minW < labelEnd) left = labelEnd + minW;
    float cy = r.hasDef ? row.top + ROW_H / 2 : (row.top + row.bottom) / 2;  // com "Padrão", o trilho fica no alto da linha
    return D2D1::RectF(left, cy - 2, right, cy + 2);
}

// valor padrão no trilho (0..1)
float SliderDefault(const aj::Row& r) { return std::clamp((r.def - r.min) / std::max(r.max - r.min, 0.0001f), 0.f, 1.f); }

void DrawSlider(aj::Ctx& c, const aj::Row& r, D2D1_RECT_F row) {
    D2D1_RECT_F t = SliderTrack(r, row);
    float v = SliderValue(r);
    if (Leo()) {
        // Leopard: trilho fino afundado (cinza com sombra por dentro) e o botão redondo de gel perolado do Aqua
        float cy = (t.top + t.bottom) / 2;
        D2D1_RECT_F tr = D2D1::RectF(t.left, cy - 2.5f, t.right, cy + 2.5f);
        LeoFill(c.dc, tr, 2.5f, { { 0, D2D1::ColorF(0.55f, 0.55f, 0.55f, 1) }, { 1, D2D1::ColorF(0.80f, 0.80f, 0.80f, 1) } });
        c.br->SetColor(D2D1::ColorF(0, 0, 0, 0.30f));
        c.dc->DrawRoundedRectangle(D2D1::RoundedRect(tr, 2.5f, 2.5f), c.br, 0.8f);
        float kx = t.left + v * (t.right - t.left);
        D2D1_RECT_F kb = D2D1::RectF(kx - 8, cy - 8, kx + 8, cy + 8);
        c.br->SetColor(D2D1::ColorF(0, 0, 0, 0.22f));
        c.dc->FillEllipse(D2D1::Ellipse({ kx, cy + 1.2f }, 8.2f, 8.2f), c.br);
        LeoGel(c.dc, kb, 8, false);
        c.br->SetColor(D2D1::ColorF(0, 0, 0, 0.42f));
        c.dc->DrawEllipse(D2D1::Ellipse({ kx, cy }, 7.5f, 7.5f), c.br, 1);
        return;
    }
    if (r.gg) {
        // macOS 27: trilho arredondado (cinza claro / azul), botão em cápsula branca de 20 × 16 com sombra, os dois
        // pontinhos embaixo das pontas e, nas pontas, os ícones de "transparente" (contorno) e "opaco" (cheio)
        float cy = (t.top + t.bottom) / 2, rad = (t.bottom - t.top) / 2;
        c.br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.16f) : D2D1::ColorF(0, 0, 0, 0.09f));
        c.dc->FillRoundedRectangle(D2D1::RoundedRect(t, rad, rad), c.br);
        float kx = t.left + 10 + v * (t.right - t.left - 20);
        c.br->SetColor(c.accent);
        c.dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(t.left, t.top, kx, t.bottom), rad, rad), c.br);
        c.br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.3f) : D2D1::ColorF(0, 0, 0, 0.25f));
        c.dc->FillEllipse(D2D1::Ellipse({ t.left + 9.5f, cy + 8 }, 1.1f, 1.1f), c.br);
        c.dc->FillEllipse(D2D1::Ellipse({ t.right - 9.5f, cy + 8 }, 1.1f, 1.1f), c.br);
        D2D1_RECT_F kb = D2D1::RectF(kx - 10, cy - 8, kx + 10, cy + 8);
        for (int s = 3; s >= 1; s--) {
            c.br->SetColor(D2D1::ColorF(0, 0, 0, g_dark ? 0.10f : 0.045f));
            c.dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(kb.left - s * 0.5f, kb.top + s * 0.4f, kb.right + s * 0.5f, kb.bottom + s * 0.8f), 8 + s * 0.5f, 8 + s * 0.5f), c.br);
        }
        c.br->SetColor(D2D1::ColorF(1, 1, 1, 1));
        c.dc->FillRoundedRectangle(D2D1::RoundedRect(kb, 8, 8), c.br);
        auto twoBoxes = [&](float cx, bool filled) {
            D2D1_RECT_F back = D2D1::RectF(cx - 7.5f, cy - 6, cx + 3, cy + 2), front = D2D1::RectF(cx - 4.5f, cy - 3, cx + 7.5f, cy + 6);
            c.br->SetColor(c.text2);
            if (filled) {
                c.dc->FillRoundedRectangle(D2D1::RoundedRect(back, 2.5f, 2.5f), c.br);
                c.br->SetColor(g_dark ? D2D1::ColorF(0.19f, 0.19f, 0.19f, 1) : D2D1::ColorF(0.97f, 0.97f, 0.97f, 1));
                c.dc->DrawRoundedRectangle(D2D1::RoundedRect(front, 4, 4), c.br, 1.4f);
                c.br->SetColor(c.text2);
                c.dc->FillRoundedRectangle(D2D1::RoundedRect(front, 4, 4), c.br);
            } else {
                c.dc->DrawRoundedRectangle(D2D1::RoundedRect(back, 2.5f, 2.5f), c.br, 1.1f);
                c.br->SetColor(g_dark ? D2D1::ColorF(0.19f, 0.19f, 0.19f, 1) : D2D1::ColorF(0.97f, 0.97f, 0.97f, 1));
                c.dc->FillRoundedRectangle(D2D1::RoundedRect(front, 4, 4), c.br);
                c.br->SetColor(c.text2);
                c.dc->DrawRoundedRectangle(D2D1::RoundedRect(front, 4, 4), c.br, 1.1f);
            }
        };
        twoBoxes(row.left + 199, false);
        twoBoxes(row.right - 20, true);
        return;
    }
    float cy = (t.top + t.bottom) / 2;
    c.br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.14f) : D2D1::ColorF(0, 0, 0, 0.10f));
    c.dc->FillRoundedRectangle(D2D1::RoundedRect(t, 2, 2), c.br);
    float kx = t.left + v * (t.right - t.left);
    c.br->SetColor(c.accent);
    c.dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(t.left, t.top, kx, t.bottom), 2, 2), c.br);
    if (r.hasDef) {  // marca do padrão: tracinho embaixo do trilho e "Padrão" centralizado nele, como no Mac
        float dx = std::round(t.left + SliderDefault(r) * (t.right - t.left)) + 0.5f;
        c.br->SetColor(c.text3);
        c.dc->DrawLine({ dx, t.bottom + 5 }, { dx, t.bottom + 10 }, c.br, 1);
        g_tfSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        aj::Text(c, L"Padrão", D2D1::RectF(dx - 40, t.bottom + 10, dx + 40, t.bottom + 24), g_tfSmall, c.text2);
        g_tfSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    }
    // botão: círculo branco com sombra, como no Mac
    c.br->SetColor(D2D1::ColorF(0, 0, 0, g_dark ? 0.45f : 0.18f));
    c.dc->FillEllipse(D2D1::Ellipse({ kx, cy + 0.6f }, 9.6f, 9.6f), c.br);
    c.br->SetColor(g_dark ? D2D1::ColorF(0.85f, 0.85f, 0.86f, 1) : D2D1::ColorF(1, 1, 1, 1));
    c.dc->FillEllipse(D2D1::Ellipse({ kx, cy }, 9, 9), c.br);
    auto end = [&](const std::wstring& s, bool left) {
        if (s.empty()) return;
        bool glyph = s[0] >= 0xE000;
        float w = glyph ? 22 : TextWidth(s, g_tfSmall) + 10;
        float top = cy - 19, bottom = cy + 19;  // rótulos das pontas na altura do trilho
        D2D1_RECT_F rr = left ? D2D1::RectF(t.left - w - 2, top, t.left - 6, bottom) : D2D1::RectF(t.right + 8, top, t.right + w + 2, bottom);
        if (glyph) aj::Text(c, s, rr, g_tfGlyphSmall, c.text2);
        else {
            g_tfSmall->SetTextAlignment(left ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
            aj::Text(c, s, rr, g_tfSmall, c.text2);
            g_tfSmall->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }
    };
    end(r.minGlyph, true);
    end(r.maxGlyph, false);
}

std::wstring RowKey(int g, int r) { return g_page.title + L"#" + std::to_wstring(g) + L"#" + std::to_wstring(r); }

void DrawRow(aj::Ctx& c, const aj::Row& r, D2D1_RECT_F rr, int gi, int ri) {
    float x = rr.left + 10;
    D2D1_COLOR_F text = c.text, text2 = c.text2;
    if (!r.enabled) { text.a *= 0.45f; text2.a *= 0.45f; }
    if (r.kind == aj::Kind::Custom) { if (r.draw) r.draw(c, rr); g_hits.push_back({ H_CUSTOM, rr, gi, ri }); return; }
    if (r.icon) {
        float cy = (rr.top + rr.bottom) / 2;
        if (!r.detail.empty()) cy = rr.top + 21;
        aj::Tile(c, D2D1::RectF(x, cy - 11, x + 22, cy + 11), r.icon, r.iconColor, SymbolFor(r.label));
        x += 32;
    }
    // área à direita (controle), para o rótulo não passar por cima
    float right = rr.right - 10;
    float cy = (rr.top + rr.bottom) / 2;
    switch (r.kind) {
    case aj::Kind::Toggle: {
        bool on = r.get ? r.get() : false;
        float t = 1;
        auto it = g_toggleAnim.find(RowKey(gi, ri));
        if (it != g_toggleAnim.end()) {
            float ms = (float)(GetTickCount() - it->second.first);
            if (g_tahoe) {  // mola medida no switch do macOS 26 (guia, seção 30): resposta 0,44 s, amortecimento 0,74
                float s = ms / 1000.f, w0 = 2 * 3.14159265f / 0.44f, z = 0.74f, wd = w0 * std::sqrt(1 - z * z);
                t = 1 - std::exp(-z * w0 * s) * (std::cos(wd * s) + z * w0 / wd * std::sin(wd * s));
                if (ms >= 450) { t = 1; g_toggleAnim.erase(it); }
            } else {
                t = std::min(1.f, ms / 150.f);
                t = 1 - (1 - t) * (1 - t);
                if (t >= 1) g_toggleAnim.erase(it);
            }
        }
        DrawToggle(c, rr, on, t, r.enabled);
        g_hits.push_back({ H_TOGGLE, rr, gi, ri });
        right -= g_tahoe ? 50 : 40;
        break;
    }
    case aj::Kind::Popup: {
        std::wstring v = r.value ? r.value() : (r.getIdx && r.getIdx() >= 0 && r.getIdx() < (int)r.options.size() ? r.options[r.getIdx()] : L"");
        if (r.gg && !Leo()) {  // (Leopard: o pop-up Aqua, mais abaixo)
            // macOS 27 (Ajustes > Aparência, 2x): botão redondo de 19,5 a 13,5 da borda da caixa, o valor a 13 dele e a
            // bolinha de 16 a 8,5 do texto
            float sx = rr.right - 13.5f - 9.75f;
            c.br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.10f) : D2D1::ColorF(0, 0, 0, 0.075f));
            c.dc->FillEllipse(D2D1::Ellipse({ sx, cy }, 9.75f, 9.75f), c.br);
            DrawUpDown(c, sx, cy, text);
            float w = TextWidth(v, g_tfBody), tx = sx - 9.75f - 13 - w;
            aj::Text(c, v, D2D1::RectF(tx, rr.top, sx - 9.75f - 13 + 2, rr.bottom), g_tfBody, text);
            float dx = tx - 8.5f - 8;
            if (r.dot) {
                D2D1_COLOR_F col = r.dot(r.getIdx ? r.getIdx() : 0);
                if (col.a < 0) {
                    D2D1_GRADIENT_STOP st[2] = { { 0, D2D1::ColorF(0.62f, 0.55f, 0.85f, 1) }, { 1, D2D1::ColorF(1.0f, 0.60f, 0.60f, 1) } };
                    ID2D1GradientStopCollection* gs = nullptr; ID2D1LinearGradientBrush* lb = nullptr;
                    if (SUCCEEDED(c.dc->CreateGradientStopCollection(st, 2, &gs)) &&
                        SUCCEEDED(c.dc->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({ dx - 8, cy }, { dx + 8, cy }), gs, &lb)))
                        c.dc->FillEllipse(D2D1::Ellipse({ dx, cy }, 8, 8), lb);
                    if (lb) lb->Release();
                    if (gs) gs->Release();
                } else { c.br->SetColor(col); c.dc->FillEllipse(D2D1::Ellipse({ dx, cy }, 8, 8), c.br); }
            }
            g_hits.push_back({ H_POPUP, D2D1::RectF(dx - 10, rr.top, rr.right - 8, rr.bottom), gi, ri });
            right = dx - 16;
            break;
        }
        if (Leo()) {
            float w = std::min(TextWidth(v, g_tfBody), 260.f);
            D2D1_RECT_F b = D2D1::RectF(right - w - 40, cy - 10, right + 6, cy + 10);
            c.br->SetColor(D2D1::ColorF(1, 1, 1, 0.6f));
            c.dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(b.left, b.top + 1, b.right, b.bottom + 1), 4, 4), c.br);
            LeoGel(c.dc, b, 4, false);
            D2D1_RECT_F cap = D2D1::RectF(b.right - 19, b.top, b.right, b.bottom);
            c.dc->PushAxisAlignedClip(cap, D2D1_ANTIALIAS_MODE_ALIASED);
            LeoGel(c.dc, b, 4, true);
            c.dc->PopAxisAlignedClip();
            c.br->SetColor(D2D1::ColorF(0, 0, 0, 0.40f));
            c.dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(b.left + 0.5f, b.top + 0.5f, b.right - 0.5f, b.bottom - 0.5f), 3.5f, 3.5f), c.br, 1);
            c.br->SetColor(D2D1::ColorF(0.12f, 0.27f, 0.58f, 0.7f));
            c.dc->DrawLine({ cap.left + 0.5f, b.top + 1 }, { cap.left + 0.5f, b.bottom - 1 }, c.br, 1);
            DrawUpDown(c, (cap.left + cap.right) / 2, cy, D2D1::ColorF(1, 1, 1, 1));
            aj::Text(c, v, D2D1::RectF(b.left + 9, b.top, cap.left - 4, b.bottom), g_tfBody, D2D1::ColorF(0, 0, 0, 0.9f));
            g_hits.push_back({ H_POPUP, b, gi, ri });
            right = b.left - 4;
            break;
        }
        if (g_tahoe) {  // Tahoe: ⌃⌄ num círculo cinza
            c.br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.10f) : D2D1::ColorF(0, 0, 0, 0.06f));
            c.dc->FillEllipse(D2D1::Ellipse({ right - 4, cy }, 10, 10), c.br);
            DrawUpDown(c, right - 4, cy, text);
        } else DrawUpDown(c, right - 4, cy, text2);
        float w = TextWidth(v, g_tfBody);
        g_tfBody->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        float gap = g_tahoe ? 6 : 0;  // o círculo é mais largo que as setinhas
        aj::Text(c, v, D2D1::RectF(right - 16 - gap - std::min(w, 260.f) - 2, rr.top, right - 14 - gap, rr.bottom), g_tfBody, text);
        g_tfBody->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        g_hits.push_back({ H_POPUP, D2D1::RectF(right - 30 - w, rr.top, right + 6, rr.bottom), gi, ri });
        right -= 30 + w;
        break;
    }
    case aj::Kind::Slider: {
        DrawSlider(c, r, rr);
        D2D1_RECT_F t = SliderTrack(r, rr);
        g_hits.push_back({ H_SLIDER, D2D1::RectF(t.left - 10, rr.top, t.right + 10, rr.bottom), gi, ri });
        right = t.left - 30;
        break;
    }
    case aj::Kind::Link: {
        DrawChevronRight(c, right - 3, cy, c.text3);
        std::wstring v = r.value ? r.value() : L"";
        if (!v.empty()) {
            g_tfBody->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            aj::Text(c, v, D2D1::RectF(rr.left + 160, rr.top, right - 16, rr.bottom), g_tfBody, text2);
            g_tfBody->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            right -= TextWidth(v, g_tfBody) + 10;
        }
        g_hits.push_back({ H_LINK, rr, gi, ri });
        right -= 20;
        break;
    }
    case aj::Kind::Button: {
        D2D1_RECT_F b = ButtonRect(r.button, right, cy);
        DrawButton(c, b, r.button, r.enabled);
        g_hits.push_back({ H_BUTTON, b, gi, ri });
        right = b.left - 10;
        break;
    }
    case aj::Kind::Value: {
        std::wstring v = r.value ? r.value() : L"";
        if (!v.empty()) {
            g_tfBody->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
            float w = std::min(TextWidth(v, g_tfBody), (rr.right - rr.left) * 0.62f);
            aj::Text(c, v, D2D1::RectF(right - w - 1, rr.top, right, rr.bottom), g_tfBody, text2);
            g_tfBody->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            right -= w + 12;
        }
        if (r.click) g_hits.push_back({ H_ROW, rr, gi, ri });
        break;
    }
    default: break;
    }
    if (r.detail.empty()) aj::Text(c, r.label, D2D1::RectF(x, rr.top, std::max(right, x + 40), r.kind == aj::Kind::Slider && r.hasDef ? rr.top + ROW_H : rr.bottom), g_tfBody, text);
    else {
        aj::Text(c, r.label, D2D1::RectF(x, rr.top + 10, std::max(right, x + 40), rr.top + 28), g_tfBody, text);
        float h = TextHeight(r.detail, g_tfSmall, right - x);
        g_tfSmall->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        aj::Text(c, r.detail, D2D1::RectF(x, rr.top + 29, right, rr.top + 29 + h + 2), g_tfSmall, text2);
        g_tfSmall->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
}

// ================= Barra lateral
float SideRowsTop() { return ACCOUNT_Y + ACCOUNT_H + 10; }

void DrawLights(aj::Ctx& c) {
    static const D2D1_COLOR_F on_c[3] = { D2D1::ColorF(1.0f, 0.373f, 0.341f), D2D1::ColorF(0.996f, 0.737f, 0.18f), D2D1::ColorF(0.157f, 0.784f, 0.251f) }; const D2D1_COLOR_F* on = semaforo::Pick(on_c, semaforo::FILL);
    for (int k = 0; k < 3; k++) {
        D2D1_POINT_2F p{ 23 + k * 23.f, 25 };  // padrão do projeto: 14 pt, 23 entre centros
        bool lit = g_winActive || g_hoverLights;
        c.br->SetColor(lit ? on[k] : (g_dark ? D2D1::ColorF(1, 1, 1, 0.18f) : D2D1::ColorF(0, 0, 0, 0.16f)));
        c.dc->FillEllipse(D2D1::Ellipse(p, 7, 7), c.br);
        semaforo::Gloss(c.dc, p, 7, c.br);
        c.br->SetColor(D2D1::ColorF(0, 0, 0, 0.14f));
        c.dc->DrawEllipse(D2D1::Ellipse(p, 6.75f, 6.75f), c.br, 0.5f);
        g_hits.push_back({ H_LIGHT, D2D1::RectF(p.x - 9, p.y - 9, p.x + 9, p.y + 9), k });
        if (!g_hoverLights) continue;
        c.br->SetColor(D2D1::ColorF(0, 0, 0, 0.55f));
        float a = 2.6f;
        if (k == 0) { Line(c.dc, c.br, p.x - a, p.y - a, p.x + a, p.y + a, 1.2f); Line(c.dc, c.br, p.x - a, p.y + a, p.x + a, p.y - a, 1.2f); }
        else if (k == 1) Line(c.dc, c.br, p.x - 3.2f, p.y, p.x + 3.2f, p.y, 1.3f);
        else { Line(c.dc, c.br, p.x - 3.2f, p.y, p.x + 3.2f, p.y, 1.3f); Line(c.dc, c.br, p.x, p.y - 3.2f, p.x, p.y + 3.2f, 1.3f); }
    }
}

void DrawSearch(aj::Ctx& c, const Pal& pal) {
    D2D1_RECT_F sr = Golden() ? D2D1::RectF(10, SEARCH_Y - 1, SIDE_W - 10, SEARCH_Y + SEARCH_H) :
                     g_tahoe ? D2D1::RectF(15, SEARCH_Y - 2, SIDE_W - 9, SEARCH_Y + SEARCH_H + 2) : D2D1::RectF(10, SEARCH_Y, SIDE_W - 10, SEARCH_Y + SEARCH_H);
    float srR = g_tahoe ? (sr.bottom - sr.top) / 2 : 6;  // Tahoe: busca em cápsula
    if (g_tahoe) {
        // Tahoe: a busca flutua sobre a lista (que rola por baixo dela, desfocada): cápsula translúcida com
        // sombra fraca e contorno de luz, como os controles de vidro
        for (int k = 3; k >= 1; k--) {
            c.br->SetColor(D2D1::ColorF(0, 0, 0, g_dark ? 0.05f : 0.025f));
            c.dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(sr.left - k * 0.5f, sr.top + k * 0.5f, sr.right + k * 0.5f, sr.bottom + k), srR + k * 0.5f, srR + k * 0.5f), c.br);
        }
        c.br->SetColor(g_dark ? D2D1::ColorF(0.30f, 0.30f, 0.32f, 0.55f) : D2D1::ColorF(0.90f, 0.90f, 0.92f, 0.62f));
        c.dc->FillRoundedRectangle(D2D1::RoundedRect(sr, srR, srR), c.br);
        c.br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.12f) : D2D1::ColorF(1, 1, 1, 0.75f));
        c.dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(sr.left + 0.5f, sr.top + 0.5f, sr.right - 0.5f, sr.bottom - 0.5f), srR - 0.5f, srR - 0.5f), c.br, 1);
    } else {
    c.br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.07f) : D2D1::ColorF(0, 0, 0, 0.055f));
    c.dc->FillRoundedRectangle(D2D1::RoundedRect(sr, srR, srR), c.br);
    }
    if (g_searchFocus) {
        c.br->SetColor(D2D1::ColorF(c.accent.r, c.accent.g, c.accent.b, 0.55f));
        c.dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(sr.left - 1.5f, sr.top - 1.5f, sr.right + 1.5f, sr.bottom + 1.5f), srR + 1.5f, srR + 1.5f), c.br, 3);
    }
    c.br->SetColor(pal.text2);
    if (!simbolo::Draw(c.dc, L"magnifyingglass", D2D1::RectF(sr.left + 8, sr.top + 8, sr.left + 20, sr.bottom - 8), c.br))
        aj::Text(c, std::wstring(1, (wchar_t)0xE721), D2D1::RectF(sr.left + 4, sr.top, sr.left + 24, sr.bottom), g_tfGlyphSmall, pal.text2);
    D2D1_RECT_F tr = D2D1::RectF(sr.left + 26, sr.top, sr.right - 8, sr.bottom);
    if (g_search.text.empty() && !g_searchFocus) aj::Text(c, L"Buscar", tr, g_tfBody, pal.text3);
    else {
        ID2D1SolidColorBrush* sel = nullptr; c.dc->CreateSolidColorBrush(D2D1::ColorF(c.accent.r, c.accent.g, c.accent.b, 0.35f), &sel);
        c.br->SetColor(pal.text);
        bool caret = g_searchFocus && ((GetTickCount() - g_caretAt) / 530) % 2 == 0;
        g_search.Draw(c.dc, g_tfBody, tr, c.br, sel, c.br, caret, false);
        if (sel) sel->Release();
    }
    g_hits.push_back({ H_SEARCH, sr });
}

void DrawSidebar(aj::Ctx& c, const Pal& pal, float H) {
    // Tahoe: a lista rola por baixo dos botões da janela e da busca (borda de rolagem desfocada)
    ID2D1DeviceContext* mainDc = c.dc;
    ID2D1DeviceContext* sdc = g_tahoe ? tahoe::SceneBegin(mainDc, 1, ClientDip(), g_dpi) : nullptr;
    D2D1_RECT_F panel = Golden() ? D2D1::RectF(0, 0, SIDE_W - 0.5f, H) : PanelRect(H);
    { static int n; if (g_tahoe && !sdc && n++ < 3) Log(L"tahoe: cena da barra lateral indisponível (%ls)", tahoe::SceneError()); }
    if (!sdc) { DrawLights(c); DrawSearch(c, pal); }
    else c.dc = sdc;
    c.dc->PushAxisAlignedClip(D2D1::RectF(0, sdc ? panel.top : SEARCH_Y + SEARCH_H + 6, SIDE_W, H - (g_tahoe && !Golden() ? 9 : 0)), D2D1_ANTIALIAS_MODE_ALIASED);
    float y = ACCOUNT_Y - g_sideScroll;
    auto& items = SidebarItems();
    // conta (no topo, como "Iniciar sessão com sua Conta Apple")
    if (g_search.text.empty()) {
        const sys::Info& inf = sys::About();
        D2D1_RECT_F ar = D2D1::RectF(10, y, SIDE_W - 10, y + ACCOUNT_H);
        bool selAcc = g_histAt >= 0 && g_hist[g_histAt].side == -1;
        (void)selAcc;
        D2D1_POINT_2F m{ 30, y + ACCOUNT_H / 2 };
        if (ID2D1Bitmap* pic = sys::AccountPicture()) {
            ID2D1EllipseGeometry* eg = nullptr;
            ID2D1Factory* f = nullptr; c.dc->GetFactory(&f);
            f->CreateEllipseGeometry(D2D1::Ellipse(m, 17, 17), &eg);
            c.dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), eg), nullptr);
            c.dc->DrawBitmap(pic, D2D1::RectF(m.x - 17, m.y - 17, m.x + 17, m.y + 17), 1, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
            c.dc->PopLayer();
            eg->Release(); f->Release();
        } else {
            c.br->SetColor(g_dark ? D2D1::ColorF(0.55f, 0.55f, 0.58f, 1) : D2D1::ColorF(0.62f, 0.62f, 0.65f, 1));
            c.dc->FillEllipse(D2D1::Ellipse(m, 17, 17), c.br);
            aj::Text(c, std::wstring(1, (wchar_t)0xE77B), D2D1::RectF(m.x - 17, m.y - 17, m.x + 17, m.y + 17), g_tfGlyph, D2D1::ColorF(1, 1, 1, 1));
        }
        aj::Text(c, inf.fullName, D2D1::RectF(54, y + 8, SIDE_W - 12, y + 26), g_tfBold, pal.text);
        aj::Text(c, L"Conta do Windows", D2D1::RectF(54, y + 25, SIDE_W - 12, y + 41), g_tfBody, pal.text2);
        g_hits.push_back({ H_ACCOUNT, ar });
        y += ACCOUNT_H + 10;
    } else y = SEARCH_Y + SEARCH_H + 14 - g_sideScroll;

    int cur = g_histAt >= 0 ? g_hist[g_histAt].side : -1;
    int lastGroup = -100;
    for (int i : g_sideVisible) {
        auto& it = items[i];
        if (g_search.text.empty() && lastGroup != -100 && it.group != lastGroup) y += SIDE_GAP;
        lastGroup = it.group;
        D2D1_RECT_F rr = g_tahoe && !Golden() ? D2D1::RectF(13, y, SIDE_W - 8, y + SIDE_ROW) : D2D1::RectF(10, y, SIDE_W - 10, y + SIDE_ROW);
        bool sel = i == cur;
        float selR = Golden() ? 8.f : g_tahoe ? 9.f : 6.f;
        if (sel && Leo()) {
            D2D1_RECT_F full = D2D1::RectF(0, rr.top, SIDE_W, rr.bottom);
            LeoFill(c.dc, full, 0, { { 0, g_winActive ? D2D1::ColorF(0.43f, 0.60f, 0.85f, 1) : D2D1::ColorF(0.68f, 0.68f, 0.70f, 1) },
                                     { 1, g_winActive ? D2D1::ColorF(0.24f, 0.44f, 0.76f, 1) : D2D1::ColorF(0.53f, 0.53f, 0.55f, 1) } });
            c.br->SetColor(g_winActive ? D2D1::ColorF(0.33f, 0.52f, 0.80f, 1) : D2D1::ColorF(0.60f, 0.60f, 0.62f, 1));
            c.dc->FillRectangle(D2D1::RectF(0, full.top, SIDE_W, full.top + 1), c.br);
        } else
        if (sel) { c.br->SetColor(pal.sideSel); c.dc->FillRoundedRectangle(D2D1::RoundedRect(rr, selR, selR), c.br); }

        g_tileBare = true;
        aj::Tile(c, D2D1::RectF(14, y + 4, 34, y + 24), it.glyph, it.color, SymbolFor(it.name));
        g_tileBare = false;
        aj::Text(c, it.name, D2D1::RectF(42, y, SIDE_W - 14, y + SIDE_ROW), g_tfSide, sel ? pal.sideSelText : pal.text);
        g_hits.push_back({ H_SIDE, rr, i });
        y += SIDE_ROW;
    }
    g_sideH = y + g_sideScroll + 12 - ACCOUNT_Y;
    c.dc->PopAxisAlignedClip();
    if (sdc) {
        tahoe::SceneEnd(1);
        c.dc = mainDc;
        tahoe::SceneDraw(mainDc, 1, panel, SEARCH_Y + SEARCH_H + 12 - panel.top);
        DrawLights(c);
        DrawSearch(c, pal);
    }
}

// ================= Folha (sheet)
D2D1_RECT_F SheetRect() {
    auto s = ClientDip();
    float w = 300, h = g_sheet.message.empty() ? 150 : 180;
    float cx = SIDE_W + (s.width - SIDE_W) / 2;
    return D2D1::RectF(cx - w / 2, TOOL_H, cx + w / 2, TOOL_H + h);
}

void DrawSheet(aj::Ctx& c, const Pal& pal) {
    auto s = ClientDip();
    c.br->SetColor(D2D1::ColorF(0, 0, 0, 0.18f));
    c.dc->FillRectangle(D2D1::RectF(0, 0, s.width, s.height), c.br);
    D2D1_RECT_F r = SheetRect();
    for (int k = 6; k > 0; k--) {
        c.br->SetColor(D2D1::ColorF(0, 0, 0, 0.035f));
        c.dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left - k, r.top - k + 4, r.right + k, r.bottom + k + 4), 12.f + k, 12.f + k), c.br);
    }
    c.br->SetColor(g_dark ? D2D1::ColorF(0.17f, 0.17f, 0.18f, 1) : D2D1::ColorF(0.96f, 0.96f, 0.96f, 1));
    c.dc->FillRoundedRectangle(D2D1::RoundedRect(r, 12, 12), c.br);
    c.br->SetColor(pal.groupLine);
    c.dc->DrawRoundedRectangle(D2D1::RoundedRect(r, 12, 12), c.br, 1);
    float y = r.top + 18;
    aj::Text(c, g_sheet.title, D2D1::RectF(r.left + 20, y, r.right - 20, y + 18), g_tfBold, pal.text);
    y += 22;
    if (!g_sheet.message.empty()) {
        g_tfSmall->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        aj::Text(c, g_sheet.message, D2D1::RectF(r.left + 20, y, r.right - 20, y + 30), g_tfSmall, pal.text2);
        g_tfSmall->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        y += 30;
    }
    D2D1_RECT_F fr = D2D1::RectF(r.left + 20, y + 4, r.right - 20, y + 28);
    c.br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.08f) : D2D1::ColorF(1, 1, 1, 1));
    c.dc->FillRoundedRectangle(D2D1::RoundedRect(fr, 5, 5), c.br);
    c.br->SetColor(D2D1::ColorF(c.accent.r, c.accent.g, c.accent.b, 0.6f));
    c.dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(fr.left - 1, fr.top - 1, fr.right + 1, fr.bottom + 1), 6, 6), c.br, 2.5f);
    TextField shown = g_sheet.field;
    if (g_sheet.password) { shown.text = std::wstring(g_sheet.field.text.size(), L'•'); }
    ID2D1SolidColorBrush* sel = nullptr; c.dc->CreateSolidColorBrush(D2D1::ColorF(c.accent.r, c.accent.g, c.accent.b, 0.35f), &sel);
    c.br->SetColor(pal.text);
    bool caret = ((GetTickCount() - g_caretAt) / 530) % 2 == 0;
    shown.Draw(c.dc, g_tfBody, D2D1::RectF(fr.left + 6, fr.top, fr.right - 6, fr.bottom), c.br, sel, c.br, caret, false);
    if (sel) sel->Release();
    g_hits.push_back({ H_SHEET_FIELD, fr });
    float by = r.bottom - 20;
    D2D1_RECT_F ok = ButtonRect(g_sheet.okLabel, r.right - 20, by - 11);
    D2D1_RECT_F cancel = ButtonRect(L"Cancelar", ok.left - 10, by - 11);
    DrawButton(c, cancel, L"Cancelar", true);
    DrawButton(c, ok, g_sheet.okLabel, true, true);
    g_hits.push_back({ H_SHEET_CANCEL, cancel });
    g_hits.push_back({ H_SHEET_OK, ok });
}

// ================= Mac OS X Leopard: as Preferências do Sistema do 10.5
// Desenho próprio do tema (nada do Sequoia/Tahoe): janela cinza 232 sem barra lateral; barra "unified" de metal com
// o título na linha de cima e, embaixo, ◀ ▶ segmentados, "Mostrar Todos" e a busca em pílula. "Mostrar Todos" é a
// grade de painéis em faixas por categoria (Pessoal, Hardware, Internet e Rede, Sistema) com os ícones do 10.5
// (icones\leopard\pref <id>.png); cada painel é um formulário: rótulo alinhado à direita com dois-pontos, o controle à
// direita dele (pop-up Aqua, caixa de seleção à esquerda do texto, botão em pílula) e linhas gravadas entre os grupos.
// Os dados (páginas, linhas, o que cada controle faz) são os mesmos de sempre (paginas.cpp).
const int H_SHOWALL = 1000;  // acerto do botão "Mostrar Todos" (fora do enum para não mexer nos outros temas)
struct LeoCat { const wchar_t* name; std::vector<std::wstring> ids; };
const std::vector<LeoCat>& LeoCats() {
    static const std::vector<LeoCat> cats = {
        { L"Pessoal", { L"aparencia", L"fundo", L"protetor", L"dock", L"central", L"geral", L"spotlight" } },
        { L"Hardware", { L"telas", L"som", L"energia", L"teclado", L"mouse", L"trackpad", L"impressoras", L"bluetooth" } },
        { L"Internet e Rede", { L"rede", L"wifi", L"contas", L"nuvem", L"jogos" } },
        { L"Sistema", { L"usuarios", L"privacidade", L"senha", L"bloqueio", L"acessibilidade", L"inteligencia", L"notificacoes", L"foco", L"tempo" } },
    };
    return cats;
}
IDWriteTextFormat* LeoFmt(float size, bool bold, DWRITE_TEXT_ALIGNMENT a, bool wrap = false) {
    // Lucida Grande no Mac; no Windows a Lucida Sans Unicode (o mesmo desenho de origem)
    IDWriteTextFormat* f = Format(L"Lucida Sans Unicode", size, bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL, a);
    if (f && wrap) { f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP); f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR); }
    return f;
}
IDWriteTextFormat *g_lf, *g_lfR, *g_lfB, *g_lfS, *g_lfSC, *g_lfT;
void LeoFormats() {
    if (g_lf) return;
    g_lf = LeoFmt(13, false, DWRITE_TEXT_ALIGNMENT_LEADING);
    g_lfR = LeoFmt(13, false, DWRITE_TEXT_ALIGNMENT_TRAILING);
    g_lfB = LeoFmt(12, true, DWRITE_TEXT_ALIGNMENT_LEADING);
    g_lfS = LeoFmt(11, false, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_lfSC = LeoFmt(11, false, DWRITE_TEXT_ALIGNMENT_CENTER, true);
    g_lfT = LeoFmt(13, false, DWRITE_TEXT_ALIGNMENT_CENTER);
}
std::wstring LeoIconPath(const std::wstring& id) {
    static std::wstring dir;
    if (dir.empty()) {
        wchar_t exe[MAX_PATH] = {}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring p = exe; p = p.substr(0, p.find_last_of(L'\\')); p = p.substr(0, p.find_last_of(L'\\'));  // bin\ -> instalação
        dir = p + L"\\icones\\leopard\\pref ";
    }
    std::wstring f = dir + id + L".png";
    return GetFileAttributesW(f.c_str()) != INVALID_FILE_ATTRIBUTES ? f : dir + L"geral.png";
}
// texto com relevo branco 1 px abaixo (os textos sobre o metal e o cinza do 10.5)
void LeoText(aj::Ctx& c, const std::wstring& s, D2D1_RECT_F r, IDWriteTextFormat* f, D2D1_COLOR_F col, bool emboss = true) {
    if (emboss) aj::Text(c, s, D2D1::RectF(r.left, r.top + 1, r.right, r.bottom + 1), f, D2D1::ColorF(1, 1, 1, 0.55f));
    aj::Text(c, s, r, f, col);
}
// pílula Aqua (botão comum perolado; padrão/apertado em gel azul)
void LeoPill(aj::Ctx& c, D2D1_RECT_F b, const std::wstring& label, bool enabled, bool blue = false) {
    float rad = (b.bottom - b.top) / 2;
    c.br->SetColor(D2D1::ColorF(1, 1, 1, 0.6f));
    c.dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(b.left, b.top + 1, b.right, b.bottom + 1), rad, rad), c.br);
    LeoGel(c.dc, b, rad, blue);
    c.br->SetColor(blue ? D2D1::ColorF(0.12f, 0.27f, 0.58f, 0.9f) : D2D1::ColorF(0, 0, 0, 0.42f));
    c.dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(b.left + 0.5f, b.top + 0.5f, b.right - 0.5f, b.bottom - 0.5f), rad - 0.5f, rad - 0.5f), c.br, 1);
    g_lfT->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    aj::Text(c, label, b, g_lfT, D2D1::ColorF(0, 0, 0, enabled ? 0.9f : 0.35f));
}
float LeoDetailH(const aj::Row& r, float w) { return r.detail.empty() ? 0 : TextHeight(r.detail, g_lfS, w) + 6; }
float LeoRowH(const aj::Row& r, float colW) {
    if (r.kind == aj::Kind::Custom) return r.height > 0 ? r.height : 40;
    float base = r.kind == aj::Kind::Toggle ? 24.f : 30.f;
    return base + LeoDetailH(r, colW - (r.kind == aj::Kind::Toggle ? 20 : 0));
}
void LeoRow(aj::Ctx& c, const aj::Row& r, D2D1_RECT_F rr, float x0, float colX, int gi, int ri) {
    D2D1_COLOR_F ink = D2D1::ColorF(0, 0, 0, r.enabled ? 0.88f : 0.35f), gray = D2D1::ColorF(0, 0, 0, r.enabled ? 0.55f : 0.30f);
    if (r.kind == aj::Kind::Custom) { if (r.draw) r.draw(c, rr); g_hits.push_back({ H_CUSTOM, rr, gi, ri }); return; }
    float cy = rr.top + 13;
    auto rightLabel = [&] {
        if (r.label.empty()) return;
        LeoText(c, Tr(r.label) + L":", D2D1::RectF(x0, cy - 10, colX - 6, cy + 10), g_lfR, ink, false);
    };
    auto detail = [&](float dx) {
        if (r.detail.empty()) return;
        float h = LeoDetailH(r, rr.right - dx);
        aj::Text(c, r.detail, D2D1::RectF(dx, rr.top + 25, rr.right, rr.top + 25 + h), g_lfS, gray);
    };
    switch (r.kind) {
    case aj::Kind::Toggle: {
        bool on = r.get ? r.get() : false;
        D2D1_RECT_F b = D2D1::RectF(colX, cy - 7, colX + 14, cy + 7);
        c.br->SetColor(D2D1::ColorF(1, 1, 1, 0.6f));
        c.dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(b.left, b.top + 1, b.right, b.bottom + 1), 3, 3), c.br);
        LeoGel(c.dc, b, 3, on);
        c.br->SetColor(on ? D2D1::ColorF(0.12f, 0.27f, 0.58f, r.enabled ? 0.9f : 0.4f) : D2D1::ColorF(0, 0, 0, r.enabled ? 0.45f : 0.2f));
        c.dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(b.left + 0.5f, b.top + 0.5f, b.right - 0.5f, b.bottom - 0.5f), 2.5f, 2.5f), c.br, 1);
        if (on) {
            c.br->SetColor(D2D1::ColorF(0.05f, 0.08f, 0.15f, r.enabled ? 0.95f : 0.4f));
            Line(c.dc, c.br, colX + 3.2f, cy + 0.2f, colX + 6, cy + 3.5f, 2.0f);
            Line(c.dc, c.br, colX + 6, cy + 3.5f, colX + 11.5f, cy - 4.2f, 2.0f);
        }
        LeoText(c, Tr(r.label), D2D1::RectF(colX + 20, cy - 10, rr.right, cy + 10), g_lf, ink, false);
        detail(colX + 20);
        g_hits.push_back({ H_TOGGLE, D2D1::RectF(colX - 4, rr.top, std::min(rr.right, colX + 30 + TextWidth(Tr(r.label), g_lf)), rr.top + 26), gi, ri });
        return;
    }
    case aj::Kind::Popup: {
        rightLabel();
        std::wstring v = r.value ? r.value() : (r.getIdx && r.getIdx() >= 0 && r.getIdx() < (int)r.options.size() ? r.options[r.getIdx()] : L"");
        if (!v.empty() && v[0] == L'!') v = v.substr(1);
        v = Tr(v);
        float w = std::max(120.f, std::min(TextWidth(v, g_lf), 300.f) + 46);
        D2D1_RECT_F b = D2D1::RectF(colX, cy - 10, colX + w, cy + 10);
        c.br->SetColor(D2D1::ColorF(1, 1, 1, 0.6f));
        c.dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(b.left, b.top + 1, b.right, b.bottom + 1), 4, 4), c.br);
        LeoGel(c.dc, b, 4, false);
        D2D1_RECT_F cap = D2D1::RectF(b.right - 19, b.top, b.right, b.bottom);
        c.dc->PushAxisAlignedClip(cap, D2D1_ANTIALIAS_MODE_ALIASED);
        LeoGel(c.dc, b, 4, true);
        c.dc->PopAxisAlignedClip();
        c.br->SetColor(D2D1::ColorF(0, 0, 0, 0.42f));
        c.dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(b.left + 0.5f, b.top + 0.5f, b.right - 0.5f, b.bottom - 0.5f), 3.5f, 3.5f), c.br, 1);
        c.br->SetColor(D2D1::ColorF(0.12f, 0.27f, 0.58f, 0.7f));
        c.dc->DrawLine({ cap.left + 0.5f, b.top + 1 }, { cap.left + 0.5f, b.bottom - 1 }, c.br, 1);
        float ax = (cap.left + cap.right) / 2;  // as setinhas brancas ▲▼
        c.br->SetColor(D2D1::ColorF(1, 1, 1, 1));
        ID2D1Factory* fac = nullptr; c.dc->GetFactory(&fac);
        for (int k = 0; k < 2 && fac; k++) {
            ID2D1PathGeometry* g = nullptr;
            if (FAILED(fac->CreatePathGeometry(&g))) break;
            ID2D1GeometrySink* sk = nullptr;
            if (SUCCEEDED(g->Open(&sk))) {
                float sgn = k ? 1.f : -1.f, ty = cy + sgn * 1.5f;
                sk->BeginFigure({ ax - 3.5f, ty }, D2D1_FIGURE_BEGIN_FILLED);
                sk->AddLine({ ax + 3.5f, ty }); sk->AddLine({ ax, ty + sgn * 4 });
                sk->EndFigure(D2D1_FIGURE_END_CLOSED); sk->Close(); sk->Release();
                c.dc->FillGeometry(g, c.br);
            }
            g->Release();
        }
        if (fac) fac->Release();
        aj::Text(c, v, D2D1::RectF(b.left + 9, b.top, cap.left - 4, b.bottom), g_lf, ink);
        detail(colX);
        g_hits.push_back({ H_POPUP, b, gi, ri });
        return;
    }
    case aj::Kind::Button: {
        rightLabel();
        float w = std::max(80.f, TextWidth(Tr(r.button), g_lfT) + 30);
        D2D1_RECT_F b = D2D1::RectF(colX, cy - 10, colX + w, cy + 10);
        LeoPill(c, b, Tr(r.button), r.enabled);
        detail(colX);
        g_hits.push_back({ H_BUTTON, b, gi, ri });
        return;
    }
    case aj::Kind::Link: {  // abre um painel/folha: botão "Nome…"
        std::wstring t = Tr(r.label) + L"…";
        float w = std::max(80.f, TextWidth(t, g_lfT) + 30);
        D2D1_RECT_F b = D2D1::RectF(colX, cy - 10, colX + w, cy + 10);
        LeoPill(c, b, t, r.enabled);
        std::wstring v = r.value ? r.value() : L"";
        if (!v.empty()) aj::Text(c, v, D2D1::RectF(b.right + 10, cy - 10, rr.right, cy + 10), g_lf, gray);
        g_hits.push_back({ H_LINK, b, gi, ri });
        return;
    }
    case aj::Kind::Value: {
        rightLabel();
        std::wstring v = r.value ? r.value() : L"";
        aj::Text(c, v, D2D1::RectF(colX, cy - 10, rr.right, cy + 10), g_lf, ink);
        detail(colX);
        if (r.click) g_hits.push_back({ H_ROW, rr, gi, ri });
        return;
    }
    case aj::Kind::Slider: {
        rightLabel();
        float v = SliderValue(r);
        D2D1_RECT_F tr = D2D1::RectF(colX + 8, cy - 2.5f, std::min(rr.right - 20, colX + 268), cy + 2.5f);
        LeoFill(c.dc, tr, 2.5f, { { 0, D2D1::ColorF(0.55f, 0.55f, 0.55f, 1) }, { 1, D2D1::ColorF(0.82f, 0.82f, 0.82f, 1) } });
        c.br->SetColor(D2D1::ColorF(0, 0, 0, 0.30f));
        c.dc->DrawRoundedRectangle(D2D1::RoundedRect(tr, 2.5f, 2.5f), c.br, 0.8f);
        float kx = tr.left + v * (tr.right - tr.left);
        c.br->SetColor(D2D1::ColorF(0, 0, 0, 0.22f));
        c.dc->FillEllipse(D2D1::Ellipse({ kx, cy + 1.2f }, 8.2f, 8.2f), c.br);
        LeoGel(c.dc, D2D1::RectF(kx - 8, cy - 8, kx + 8, cy + 8), 8, false);
        c.br->SetColor(D2D1::ColorF(0, 0, 0, 0.42f));
        c.dc->DrawEllipse(D2D1::Ellipse({ kx, cy }, 7.5f, 7.5f), c.br, 1);
        if (!r.minGlyph.empty() && r.minGlyph[0] < 0xE000) aj::Text(c, r.minGlyph, D2D1::RectF(colX - 60, cy + 6, colX + 60, cy + 20), g_lfSC, gray);
        detail(colX);
        g_hits.push_back({ H_SLIDER, D2D1::RectF(tr.left - 10, cy - 10, tr.right + 10, cy + 10), gi, ri });  // SliderTo tira 10 de cada lado
        return;
    }
    default:
        rightLabel();
        return;
    }
}

void LeoShowAll(aj::Ctx& c, D2D1_SIZE_F s, float top) {
    auto& items = SidebarItems();
    std::wstring q = g_search.text;
    float y = top - g_scroll;
    int band = 0;
    for (auto& cat : LeoCats()) {
        std::vector<int> idx;
        for (auto& id : cat.ids) {
            int i = SidebarIndexFor(id);
            if (i < 0) continue;
            if (!q.empty() && std::find(g_sideVisible.begin(), g_sideVisible.end(), i) == g_sideVisible.end()) continue;
            idx.push_back(i);
        }
        if (idx.empty()) continue;
        const float PITCH = 84, ROWH = 82;
        int perRow = std::max(1, (int)((s.width - 24) / PITCH));
        int rows = ((int)idx.size() + perRow - 1) / perRow;
        float h = 24 + rows * ROWH + 6;
        // faixas alternadas (cinza claro / cinza do 10.5) com a linha fina entre elas
        c.br->SetColor(band % 2 == 0 ? D2D1::ColorF(0.93f, 0.93f, 0.93f, 1) : D2D1::ColorF(0.87f, 0.87f, 0.87f, 1));
        c.dc->FillRectangle(D2D1::RectF(0, y, s.width, y + h), c.br);
        c.br->SetColor(D2D1::ColorF(0, 0, 0, 0.12f));
        c.dc->FillRectangle(D2D1::RectF(0, y + h - 1, s.width, y + h), c.br);
        LeoText(c, Tr(cat.name), D2D1::RectF(12, y + 4, s.width, y + 22), g_lfB, D2D1::ColorF(0, 0, 0, 0.75f));
        for (int k = 0; k < (int)idx.size(); k++) {
            int i = idx[k];
            float x = 14 + (k % perRow) * PITCH, iy = y + 24 + (k / perRow) * ROWH;
            if (ID2D1Bitmap* b = aj::BitmapFromFile(LeoIconPath(items[i].id), 64))  // o PNG tem 10 % de margem: 40 para 32 de desenho
                c.dc->DrawBitmap(b, D2D1::RectF(x + PITCH / 2 - 20, iy, x + PITCH / 2 + 20, iy + 40), 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            aj::Text(c, Tr(items[i].name), D2D1::RectF(x + 2, iy + 42, x + PITCH - 2, iy + ROWH), g_lfSC, D2D1::ColorF(0, 0, 0, 0.85f));
            g_hits.push_back({ H_SIDE, D2D1::RectF(x, iy, x + PITCH, iy + ROWH), i });
        }
        y += h;
        band++;
    }
    g_contentH = y + g_scroll - top + 8;
}

void LeoPage(aj::Ctx& c, D2D1_SIZE_F s, float top) {
    float x0 = 20, x1 = s.width - 20, w = x1 - x0;
    float colX = x0 + std::max(170.f, w * 0.38f);
    float y = top + 18 - g_scroll;
    for (int gi = 0; gi < (int)g_page.groups.size(); gi++) {
        auto& g = g_page.groups[gi];
        if (g.rows.empty() && g.title.empty()) continue;
        if (gi > 0) {  // a linha gravada entre os grupos (escura e o fio branco embaixo)
            y += 8;
            c.br->SetColor(D2D1::ColorF(0, 0, 0, 0.14f));
            c.dc->FillRectangle(D2D1::RectF(x0, y, x1, y + 1), c.br);
            c.br->SetColor(D2D1::ColorF(1, 1, 1, 0.7f));
            c.dc->FillRectangle(D2D1::RectF(x0, y + 1, x1, y + 2), c.br);
            y += 12;
        }
        if (!g.title.empty()) { LeoText(c, Tr(g.title), D2D1::RectF(x0, y, x1, y + 18), g_lfB, D2D1::ColorF(0, 0, 0, 0.8f)); y += 22; }
        if (!g.subtitle.empty()) {
            float th = TextHeight(g.subtitle, g_lfS, w);
            aj::Text(c, g.subtitle, D2D1::RectF(x0, y, x1, y + th + 2), g_lfS, D2D1::ColorF(0, 0, 0, 0.55f));
            y += th + 8;
        }
        for (int ri = 0; ri < (int)g.rows.size(); ri++) {
            auto& r = g.rows[ri];
            float rh = LeoRowH(r, x1 - colX);
            D2D1_RECT_F rr = D2D1::RectF(r.kind == aj::Kind::Custom ? x0 : x0, y, x1, y + rh);
            if (rr.bottom > top - 60 && rr.top < s.height + 60) LeoRow(c, r, rr, x0, colX, gi, ri);
            y += rh;
        }
    }
    g_contentH = y + g_scroll - top + 20;
}

void RenderLeopard() {
    auto* dc = gfx::Begin(g_dpi);
    if (!dc) return;
    if (!g_round) {
        ID2D1Factory* f = nullptr; dc->GetFactory(&f);
        f->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND), nullptr, 0, &g_round);
        f->Release();
    }
    LeoFormats();
    dc->Clear(D2D1::ColorF(232 / 255.f, 232 / 255.f, 232 / 255.f, 1));
    Pal pal = Colors();
    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &br);
    aj::Ctx c{ dc, br, false, D2D1::ColorF(0, 0, 0, 0.88f), D2D1::ColorF(0, 0, 0, 0.55f), D2D1::ColorF(0, 0, 0, 0.30f), aj::Accent(),
               D2D1::ColorF(0, 0, 0, 0.12f), D2D1::ColorF(0, 0, 0, 0.04f), D2D1::ColorF(1, 1, 1, 1),
               g_lf, g_lfS, g_lfB, g_lfB, g_tfGlyph, g_tfGlyphSmall, g_lfT, g_lfSC, g_lfSC };
    g_hits.clear();
    auto s = ClientDip();
    const bool all = g_leoAll || !g_search.text.empty();
    dc->PushAxisAlignedClip(D2D1::RectF(0, TOOL_H, s.width, s.height), D2D1_ANTIALIAS_MODE_ALIASED);
    if (all) LeoShowAll(c, s, TOOL_H); else LeoPage(c, s, TOOL_H);
    dc->PopAxisAlignedClip();

    // barra "unified": metal, título, botões da janela e a linha de ferramentas
    leopard::Metal(dc, D2D1::RectF(0, 0, s.width, TOOL_H), g_winActive, false, true);
    std::wstring title = all ? Tr(L"Preferências do Sistema") : Tr(g_page.title);
    LeoText(c, title, D2D1::RectF(80, 0, s.width - 80, 22), g_lfT, D2D1::ColorF(0, 0, 0, g_winActive ? 0.85f : 0.45f));
    for (int k = 0; k < 3; k++) {
        static const D2D1_COLOR_F on_c[3] = { D2D1::ColorF(1.0f, 0.373f, 0.341f), D2D1::ColorF(0.996f, 0.737f, 0.18f), D2D1::ColorF(0.157f, 0.784f, 0.251f) };
        const D2D1_COLOR_F* on = semaforo::Pick(on_c, semaforo::FILL);
        D2D1_POINT_2F p{ 14.5f + k * 21.f, 11 };
        br->SetColor(g_winActive || g_hoverLights ? on[k] : D2D1::ColorF(0.75f, 0.75f, 0.75f, 1));
        dc->FillEllipse(D2D1::Ellipse(p, 7, 7), br);
        semaforo::Gloss(dc, p, 7, br);
        g_hits.push_back({ H_LIGHT, D2D1::RectF(p.x - 9, p.y - 9, p.x + 9, p.y + 9), k });
        if (!g_hoverLights) continue;
        br->SetColor(D2D1::ColorF(0, 0, 0, 0.6f));
        float a = 2.6f;
        if (k == 0) { Line(dc, br, p.x - a, p.y - a, p.x + a, p.y + a, 1.2f); Line(dc, br, p.x - a, p.y + a, p.x + a, p.y - a, 1.2f); }
        else if (k == 1) Line(dc, br, p.x - 3.2f, p.y, p.x + 3.2f, p.y, 1.3f);
        else { Line(dc, br, p.x - 3.2f, p.y, p.x + 3.2f, p.y, 1.3f); Line(dc, br, p.x, p.y - 3.2f, p.x, p.y + 3.2f, 1.3f); }
    }
    // ◀ ▶ segmentados (moldura de metal claro)
    bool canBack = !all || g_histAt > 0, canFwd = g_histAt + 1 < (int)g_hist.size();
    D2D1_RECT_F seg = D2D1::RectF(10, 30, 62, 52);
    br->SetColor(D2D1::ColorF(1, 1, 1, 0.45f));
    dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(seg.left, seg.top + 1, seg.right, seg.bottom + 1), 4, 4), br);
    LeoFill(dc, seg, 4, { { 0, D2D1::ColorF(0.99f, 0.99f, 0.99f, 1) }, { 1, D2D1::ColorF(0.84f, 0.84f, 0.84f, 1) } });
    br->SetColor(D2D1::ColorF(0, 0, 0, 0.42f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(seg.left + 0.5f, seg.top + 0.5f, seg.right - 0.5f, seg.bottom - 0.5f), 3.5f, 3.5f), br, 1);
    br->SetColor(D2D1::ColorF(0, 0, 0, 0.30f));
    dc->DrawLine({ 36.5f, seg.top + 1 }, { 36.5f, seg.bottom - 1 }, br, 1);
    ID2D1Factory* fac = nullptr; dc->GetFactory(&fac);
    auto tri = [&](float cx, bool left, bool on) {
        ID2D1PathGeometry* g = nullptr;
        if (!fac || FAILED(fac->CreatePathGeometry(&g))) return;
        ID2D1GeometrySink* sk = nullptr;
        if (SUCCEEDED(g->Open(&sk))) {
            float d = left ? -1.f : 1.f, cy = 41;
            sk->BeginFigure({ cx + d * 3.5f, cy }, D2D1_FIGURE_BEGIN_FILLED);
            sk->AddLine({ cx - d * 3, cy - 4.5f }); sk->AddLine({ cx - d * 3, cy + 4.5f });
            sk->EndFigure(D2D1_FIGURE_END_CLOSED); sk->Close(); sk->Release();
            br->SetColor(D2D1::ColorF(0.18f, 0.18f, 0.19f, on ? 0.9f : 0.3f));
            dc->FillGeometry(g, br);
        }
        g->Release();
    };
    tri(23, true, canBack); tri(49, false, canFwd);
    if (fac) fac->Release();
    g_hits.push_back({ H_BACK, D2D1::RectF(10, 30, 36, 52) });
    g_hits.push_back({ H_FWD, D2D1::RectF(36, 30, 62, 52) });
    std::wstring sa = Tr(L"Mostrar Todos");
    D2D1_RECT_F sb = D2D1::RectF(70, 30, 70 + std::max(84.f, TextWidth(sa, g_lfT) + 26), 52);
    LeoPill(c, sb, sa, true);
    g_hits.push_back({ (HitKind)H_SHOWALL, sb });
    // busca: pílula branca afundada com a lupa; com o foco, o anel azul
    D2D1_RECT_F sr = D2D1::RectF(s.width - 180, 31, s.width - 12, 51);
    float srR = (sr.bottom - sr.top) / 2;
    if (g_searchFocus) {
        br->SetColor(D2D1::ColorF(0.36f, 0.60f, 0.95f, 0.75f));
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(sr.left - 1.5f, sr.top - 1.5f, sr.right + 1.5f, sr.bottom + 1.5f), srR + 1.5f, srR + 1.5f), br, 3);
    }
    br->SetColor(D2D1::ColorF(1, 1, 1, 1));
    dc->FillRoundedRectangle(D2D1::RoundedRect(sr, srR, srR), br);
    LeoFill(dc, D2D1::RectF(sr.left + 1, sr.top + 1, sr.right - 1, sr.top + 5), 2, { { 0, D2D1::ColorF(0, 0, 0, 0.14f) }, { 1, D2D1::ColorF(0, 0, 0, 0) } });
    br->SetColor(D2D1::ColorF(0, 0, 0, 0.45f));
    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(sr.left + 0.5f, sr.top + 0.5f, sr.right - 0.5f, sr.bottom - 0.5f), srR - 0.5f, srR - 0.5f), br, 1);
    br->SetColor(D2D1::ColorF(0.25f, 0.25f, 0.27f, 1));
    dc->DrawEllipse(D2D1::Ellipse({ sr.left + 13, (sr.top + sr.bottom) / 2 - 1 }, 4, 4), br, 1.5f);
    Line(dc, br, sr.left + 16, (sr.top + sr.bottom) / 2 + 2, sr.left + 19, (sr.top + sr.bottom) / 2 + 5, 1.8f);
    D2D1_RECT_F tr = D2D1::RectF(sr.left + 24, sr.top, sr.right - 8, sr.bottom);
    if (!g_search.text.empty() || g_searchFocus) {
        ID2D1SolidColorBrush* sel = nullptr; dc->CreateSolidColorBrush(D2D1::ColorF(0.36f, 0.60f, 0.95f, 0.35f), &sel);
        br->SetColor(D2D1::ColorF(0, 0, 0, 0.9f));
        bool caret = g_searchFocus && ((GetTickCount() - g_caretAt) / 530) % 2 == 0;
        g_search.Draw(dc, g_lf, tr, br, sel, br, caret, false);
        if (sel) sel->Release();
    }
    g_hits.push_back({ H_SEARCH, sr });
    if (g_sheet.open) { g_hits.clear(); DrawSheet(c, pal); }
    br->Release();
    gfx::End();
}

// ================= Quadro
void Render() {
    if (Leo()) { RenderLeopard(); return; }  // Mac OS X Leopard: as Preferências do Sistema do 10.5 (desenho próprio)
    auto* dc = gfx::Begin(g_dpi);
    if (!dc) return;
    if (!g_round) {
        ID2D1Factory* f = nullptr; dc->GetFactory(&f);
        f->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND), nullptr, 0, &g_round);
        f->Release();
    }
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    Pal pal = Colors();
    ID2D1SolidColorBrush* br = nullptr;
    dc->CreateSolidColorBrush(pal.text, &br);
    aj::Ctx c{ dc, br, g_dark, pal.text, pal.text2, pal.text3, aj::Accent(), pal.line, pal.group, pal.control,
               g_tfBody, g_tfSmall, g_tfBold, g_tfTitle, g_tfGlyph, g_tfGlyphSmall, g_tfCenter, g_tfSmallCenter, g_tfSmallBoldCenter };
    g_hits.clear();
    auto s = ClientDip();

    // fundo: barra lateral translúcida (acrílico do sistema) e conteúdo opaco
    float winR = g_tahoe ? tahoe::Radius(g_hwnd) : 0;
    bool winClip = g_tahoe && tahoe::PushWindowClip(dc, s, winR);
    if (Golden()) {
        // Golden Gate: a barra lateral é uma coluna de cor cheia até a borda (sem o vidro flutuante do Tahoe)
        tahoe::SetPanel(g_hwnd, D2D1::RectF(0, 0, 0, 0), 0);
        br->SetColor(pal.side);
        dc->FillRectangle(D2D1::RectF(0, 0, SIDE_W, s.height), br);
        br->SetColor(pal.content);
        dc->FillRectangle(D2D1::RectF(SIDE_W, 0, s.width, s.height), br);
        br->SetColor(pal.divider);
        dc->FillRectangle(D2D1::RectF(SIDE_W - 0.5f, 0, SIDE_W, s.height), br);
    } else if (g_tahoe) {
        // Tahoe: janela inteira na cor do conteúdo; a barra lateral é um painel de vidro flutuante
        D2D1_RECT_F panel = PanelRect(s.height);
        tahoe::SetPanel(g_hwnd, panel, PanelR());
        ID2D1Factory* f = nullptr; dc->GetFactory(&f);

        ID2D1RectangleGeometry* all = nullptr; ID2D1PathGeometry* hole = nullptr; ID2D1PathGeometry* rest = nullptr;
        f->CreateRectangleGeometry(D2D1::RectF(0, 0, s.width, s.height), &all);
        hole = tahoe::Squircle(f, panel, PanelR());  // canto contínuo
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
        br->SetColor(g_dark ? D2D1::ColorF(0, 0, 0, 0.10f) : D2D1::ColorF(1, 1, 1, 0.18f));
        if (hole) { dc->FillGeometry(hole, br); hole->Release(); }
    } else {
    br->SetColor(pal.side);
    dc->FillRectangle(D2D1::RectF(0, 0, SIDE_W, s.height), br);
    br->SetColor(pal.content);
    dc->FillRectangle(D2D1::RectF(SIDE_W, 0, s.width, s.height), br);
    br->SetColor(pal.divider);
    dc->FillRectangle(D2D1::RectF(SIDE_W - 0.5f, 0, SIDE_W + 0.5f, s.height), br);
    }
    // Leopard: a barra de cima é o metal "unified" do 10.5, de ponta a ponta (os três botões e o título por cima)
    if (!g_tahoe && tahoe::Leopard()) leopard::Metal(dc, D2D1::RectF(0, 0, s.width, TOOL_H), g_winActive, g_dark, true);

    // conteúdo
    // Golden Gate (Aparência do macOS 27, 2x): caixas a 21 da barra lateral e 20,5 da direita, a primeira a 64,5 do alto
    float x0 = SIDE_W + (Golden() ? 21.f : PAD), x1 = s.width - (Golden() ? 20.5f : PAD), w = x1 - x0;
    // Tahoe: o conteúdo rola por baixo de Voltar/Avançar e do título (borda de rolagem desfocada), sem faixa opaca
    ID2D1DeviceContext* mainDc = dc;
    ID2D1DeviceContext* cdc = g_tahoe ? tahoe::SceneBegin(mainDc, 0, s, g_dpi) : nullptr;
    if (cdc) { dc = cdc; c.dc = cdc; }
    const float clipTop = cdc ? 0.f : TOOL_H;
    dc->PushAxisAlignedClip(D2D1::RectF(SIDE_W + 0.5f, clipTop, s.width, s.height), D2D1_ANTIALIAS_MODE_ALIASED);
    float y = (Golden() ? 64.5f : TOOL_H + 6) - g_scroll;
    for (int gi = 0; gi < (int)g_page.groups.size(); gi++) {
        auto& g = g_page.groups[gi];
        if (Golden() && !g.title.empty() && g.subtitle.empty()) {
            // macOS 27: 57,5 entre as caixas, com o título em negrito centrado a 39,5 da caixa de cima
            if (gi > 0) { aj::Text(c, g.title, D2D1::RectF(x0 + 10, y + 30.5f, x1 - 10, y + 48.5f), g_tfBold, pal.text); y += 57.5f; }
            else { aj::Text(c, g.title, D2D1::RectF(x0 + 10, y, x1 - 10, y + 18), g_tfBold, pal.text); y += 25; }
        } else {
        if (gi > 0) y += Golden() ? 10.5f : GROUP_GAP;
        float hh = HeaderHeight(g, w);
        if (hh > 0) {
            if (gi > 0) y += 10;
            float hy = y;
            if (!g.title.empty()) { aj::Text(c, g.title, D2D1::RectF(x0 + 10, hy, x1 - 10, hy + 18), g_tfBold, pal.text); hy += 18; }
            if (!g.subtitle.empty()) {
                float th = TextHeight(g.subtitle, g_tfSmall, w - 20);
                g_tfSmall->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
                aj::Text(c, g.subtitle, D2D1::RectF(x0 + 10, hy, x1 - 10, hy + th + 2), g_tfSmall, pal.text2);
                g_tfSmall->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
            y += hh;
        }
        }
        float gh = 0;
        for (auto& r : g.rows) gh += RowHeight(r, w);
        if (g.rows.empty()) continue;
        D2D1_RECT_F box = D2D1::RectF(x0, y, x1, y + gh);
        if (g.box && box.bottom > clipTop - 10 && box.top < s.height + 10) {
            br->SetColor(pal.group);
            dc->FillRoundedRectangle(D2D1::RoundedRect(box, GroupR(), GroupR()), br);
            if (!g_tahoe) {  // Tahoe: caixa só pelo tom, sem contorno
                br->SetColor(pal.groupLine);
                dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(box.left + 0.5f, box.top + 0.5f, box.right - 0.5f, box.bottom - 0.5f), GroupR(), GroupR()), br, 1);
            }
        }
        float ry = y;
        for (int ri = 0; ri < (int)g.rows.size(); ri++) {
            float rh = RowHeight(g.rows[ri], w);
            D2D1_RECT_F rr = D2D1::RectF(x0, ry, x1, ry + rh);
            if (rr.bottom > clipTop - 50 && rr.top < s.height + 50) {
                if (ri > 0 && g.box && !g.rows[ri].noLine) {
                    br->SetColor(pal.line);
                    dc->FillRectangle(D2D1::RectF(x0 + 10, ry - 0.5f, x1 - 10, ry + 0.5f), br);
                }
                DrawRow(c, g.rows[ri], rr, gi, ri);
            }
            ry += rh;
        }
        y += gh;
    }
    g_contentH = y + g_scroll + 20 - TOOL_H;
    dc->PopAxisAlignedClip();
    if (cdc) {
        tahoe::SceneEnd(0);
        dc = mainDc; c.dc = mainDc;
        tahoe::SceneDraw(dc, 0, D2D1::RectF(SIDE_W + 0.5f, 0, s.width, s.height), TOOL_H + 10, D2D1::ColorF(pal.content.r, pal.content.g, pal.content.b, 0.9f));
    } else {
    // barra de ferramentas: setas e título; linha embaixo quando há rolagem
    br->SetColor(pal.content);
    dc->FillRectangle(D2D1::RectF(SIDE_W + 0.5f, 0, s.width, TOOL_H), br);
    }
    if (!cdc && g_scroll > 1) { br->SetColor(g_dark ? D2D1::ColorF(0, 0, 0, 0.5f) : D2D1::ColorF(0, 0, 0, 0.1f)); dc->FillRectangle(D2D1::RectF(SIDE_W, TOOL_H - 1, s.width, TOOL_H), br); }
    bool canBack = g_histAt > 0, canFwd = g_histAt + 1 < (int)g_hist.size();
    if (g_tahoe) {
        tahoe::Capsule(dc, br, D2D1::RectF(SIDE_W + 10, 8, SIDE_W + 82, 42), -1, g_dark);  // Voltar/Avançar numa cápsula de vidro
        br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.16f) : D2D1::ColorF(0, 0, 0, 0.12f));
        dc->FillRectangle(D2D1::RectF(SIDE_W + 46 - 0.5f, 17, SIDE_W + 46 + 0.5f, 33), br);  // divisória entre as setas
    }
    auto chev = [&](float cx, bool left, bool on) {
        br->SetColor(on ? pal.text2 : pal.text3);
        float d = left ? 1 : -1;
        Line(dc, br, cx + 2.5f * d, 25 - 6, cx - 2.5f * d, 25, 1.7f);
        Line(dc, br, cx - 2.5f * d, 25, cx + 2.5f * d, 25 + 6, 1.7f);
    };
    chev(SIDE_W + 28, true, canBack);
    chev(SIDE_W + 64, false, canFwd);
    g_hits.push_back({ H_BACK, D2D1::RectF(SIDE_W + 14, 10, SIDE_W + 44, 40) });
    g_hits.push_back({ H_FWD, D2D1::RectF(SIDE_W + 50, 10, SIDE_W + 80, 40) });
    if (!(Golden() && g_page.title == L"Geral"))  // macOS 27: a Geral não tem título na barra (tem o cartão grande)
        aj::Text(c, g_page.title, D2D1::RectF(SIDE_W + (g_tahoe ? 94 : 88), 0, s.width - 20, 50), g_tfTitle, pal.text);

    if (g_tahoe && !Golden()) tahoe::Panel(dc, br, PanelRect(s.height), PanelR(), g_dark);  // Golden Gate: sem painel flutuante
    DrawSidebar(c, pal, s.height);
    if (g_sheet.open) { g_hits.clear(); DrawSheet(c, pal); }
    if (winClip) { dc->PopLayer(); tahoe::WindowRim(dc, br, s, winR, g_dark); }
    br->Release();
    gfx::End();
    if (!g_toggleAnim.empty()) SetTimer(g_hwnd, T_ANIM, 15, nullptr);
}

const Hit* HitAt(D2D1_POINT_2F p) {
    for (int i = (int)g_hits.size() - 1; i >= 0; i--) {  // de cima para baixo (o último desenhado primeiro)
        auto& h = g_hits[i];
        if (h.kind == H_ROW || h.kind == H_TOGGLE || h.kind == H_POPUP || h.kind == H_SLIDER || h.kind == H_LINK || h.kind == H_BUTTON || h.kind == H_CUSTOM)
            if (p.y < TOOL_H || p.x < SIDE_W) continue;  // linhas roladas para baixo da barra de ferramentas
        if (h.kind == H_SIDE && p.y < SEARCH_Y + SEARCH_H + 6) continue;
        if (In(h.r, p)) return &h;
    }
    return nullptr;
}

aj::Row* RowOf(const Hit& h) {
    if (h.a < 0 || h.a >= (int)g_page.groups.size()) return nullptr;
    auto& g = g_page.groups[h.a];
    if (h.b < 0 || h.b >= (int)g.rows.size()) return nullptr;
    return &g.rows[h.b];
}

void SliderTo(const Hit& h, float x) {
    aj::Row* r = RowOf(h);
    if (!r || !r->setF) return;
    D2D1_RECT_F t = SliderTrack(*r, h.r);
    // h.r é a área de toque (trilho ± 10); recalcula o trilho a partir da linha original
    float v = std::clamp((x - (h.r.left + 10)) / std::max(1.f, (h.r.right - 10) - (h.r.left + 10)), 0.f, 1.f);
    (void)t;
    // ímã no padrão: perto da marca (até 6 pt), o botão gruda nela
    if (r->hasDef) {
        float d = SliderDefault(*r), w = std::max(1.f, (h.r.right - 10) - (h.r.left + 10));
        if (std::fabs(v - d) * w <= 6) v = d;
    }
    r->setF(r->min + v * (r->max - r->min));
    Invalidate();
}

void OpenPopup(const Hit& h) {
    aj::Row* r = RowOf(h);
    if (!r || !r->enabled || r->options.empty()) return;
    int cur = r->getIdx ? r->getIdx() : -1;
    std::vector<ctxmenu::Item> items;
    for (int i = 0; i < (int)r->options.size(); i++) {
        ctxmenu::Item it;
        if (r->options[i] == L"-") { it.separator = true; items.push_back(it); continue; }
        it.label = r->options[i];
        it.id = i + 1;
        it.checked = i == cur;
        if (!it.label.empty() && it.label[0] == L'!') { it.label = it.label.substr(1); it.disabled = true; }
        it.label = Tr(it.label);
        items.push_back(it);
    }
    POINT pt{ (LONG)(h.r.left * g_scale), (LONG)(h.r.bottom * g_scale) };
    ClientToScreen(g_hwnd, &pt);
    auto set = r->setIdx;
    int id = ctxmenu::ShowItems(g_hwnd, pt, items);
    if (id > 0 && set) { set(id - 1); ShowPage(); }
    Invalidate();
}

LRESULT HitTestNC(HWND h, LPARAM l) {
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
    if (g_sheet.open) return HTCLIENT;
    if (Leo()) {  // Leopard: a barra inteira arrasta, menos os botões, a busca e os três botões da janela
        if (d.y >= TOOL_H) return HTCLIENT;
        for (auto& hh : g_hits) if (d.x >= hh.r.left && d.x < hh.r.right && d.y >= hh.r.top && d.y < hh.r.bottom) return HTCLIENT;
        return HTCAPTION;
    }
    bool lights = d.x >= 10 && d.x < 76 && d.y < 40;
    bool arrows = d.x >= SIDE_W + 10 && d.x < SIDE_W + 84 && d.y < TOOL_H;
    if (!lights && !arrows && (d.y < TOOL_H - 4 && (d.x >= SIDE_W || d.y < SEARCH_Y - 4))) return HTCAPTION;
    return HTCLIENT;
}

void ApplyWindowTheme() {
    SIDE_W = g_tahoe ? (Golden() ? 231.f : 247.f) : 215.f;
    TOOL_H = Leo() ? 56.f : 52.f;
    if (Leo()) SIDE_W = 0;  // Leopard: as Preferências do Sistema não têm barra lateral (Mostrar Todos)  // Tahoe: 8 de recuo + 239 do painel; Golden Gate: 231 até a borda
    SIDE_ROW = g_tahoe ? 32.f : 28.f;
    ROW_H = g_tahoe ? (Golden() ? 37.5f : 42.f) : 38.f;  // macOS 27: linhas de 37,5 (as com ícone, 49)
    SEARCH_Y = Golden() ? 62.f : 52.f;
    ACCOUNT_Y = Golden() ? 96.f : 90.f;
    BOOL dark = g_dark;
    DwmSetWindowAttribute(g_hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &dark, sizeof(dark));
    if (g_tahoe) {  // Tahoe: cantos de 26 pt com o vidro da Composição (finder/tahoe.cpp)
        tahoe::Frame(g_hwnd, true);
        RECT rc; GetClientRect(g_hwnd, &rc);
        tahoe::Size(g_hwnd, (float)rc.right, (float)rc.bottom, g_dark);
        shadow::SetRadius(tahoe::WindowR());
        return;
    }
    tahoe::Frame(g_hwnd, false);
    shadow::SetRadius(0);
    COLORREF border = g_dark ? RGB(88, 88, 92) : RGB(196, 196, 200);
    DwmSetWindowAttribute(g_hwnd, 34 /*DWMWA_BORDER_COLOR*/, &border, sizeof(border));
    int backdrop = 3;  // DWMSBT_TRANSIENTWINDOW: acrílico do sistema na barra lateral
    DwmSetWindowAttribute(g_hwnd, 38 /*DWMWA_SYSTEMBACKDROP_TYPE*/, &backdrop, sizeof(backdrop));
}

void CloseSheet() { g_sheet.open = false; g_sheet.field.SetText(L"", false); Invalidate(); }

void Click(D2D1_POINT_2F p, bool right) {
    const Hit* hp = HitAt(p);
    if (!hp) { if (g_searchFocus) { g_searchFocus = false; Invalidate(); } return; }
    Hit h = *hp;
    if (h.kind != H_SEARCH && g_searchFocus) { g_searchFocus = false; }
    if ((int)h.kind == H_SHOWALL) {  // Leopard: "Mostrar Todos" (a grade de painéis)
        g_leoAll = true; g_search.SetText(L"", false); UpdateSideVisible(); g_scroll = g_scrollTarget = 0; Invalidate(); return;
    }
    if (h.kind == H_BACK && Leo() && !g_leoAll && g_histAt <= 0) { g_leoAll = true; g_scroll = g_scrollTarget = 0; Invalidate(); return; }
    switch (h.kind) {
    case H_LIGHT:
        if (h.a == 0) PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
        else if (h.a == 1) ShowWindow(g_hwnd, SW_MINIMIZE);
        else ampliacao::Alternar(g_hwnd);
        break;
    case H_BACK: Go(-1); break;
    case H_FWD: Go(1); break;
    case H_SEARCH:
        g_searchFocus = true; g_caretAt = GetTickCount();
        g_search.Click(g_tfBody, p, false);
        SetTimer(g_hwnd, T_CARET, 530, nullptr);
        break;
    case H_SIDE: SelectSide(h.a); break;
    case H_ACCOUNT: {
        int i = SidebarIndexFor(L"conta");
        if (i >= 0) SelectSide(i);
        break;
    }
    case H_TOGGLE: {
        aj::Row* r = RowOf(h);
        if (!r || !r->enabled || !r->set) break;
        bool nv = !(r->get ? r->get() : false);
        g_toggleAnim[RowKey(h.a, h.b)] = { GetTickCount(), nv };
        r->set(nv);
        ShowPage();
        break;
    }
    case H_POPUP: OpenPopup(h); break;
    case H_SLIDER: {
        aj::Row* r = RowOf(h);
        if (!r || !r->enabled) break;
        g_drag.g = h.a; g_drag.r = h.b; g_drag.track = h.r;
        SetCapture(g_hwnd);
        SliderTo(h, p.x);
        break;
    }
    case H_LINK: case H_ROW: case H_BUTTON: {
        aj::Row* r = RowOf(h);
        if (r && r->enabled && r->click) { auto f = r->click; f(); }
        break;
    }
    case H_CUSTOM: {
        aj::Row* r = RowOf(h);
        if (r && r->clickAt) { auto f = r->clickAt; f(p, h.r); }
        break;
    }
    case H_SHEET_CANCEL: CloseSheet(); break;
    case H_SHEET_OK: {
        auto f = g_sheet.ok; std::wstring t = g_sheet.field.text;
        CloseSheet();
        if (f) f(t);
        break;
    }
    case H_SHEET_FIELD: g_sheet.field.Click(g_tfBody, p, false); g_caretAt = GetTickCount(); break;
    default: break;
    }
    (void)right;
    Invalidate();
}

float MaxScroll() { return std::max(0.f, g_contentH - (ClientDip().height - TOOL_H)); }
float SideMaxScroll() { return std::max(0.f, g_sideH - (ClientDip().height - ACCOUNT_Y)); }

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
    case WM_GETMINMAXINFO: {
        auto* mm = (MINMAXINFO*)l;
        mm->ptMinTrackSize = { (LONG)(680 * g_scale), (LONG)(420 * g_scale) };
        return 0;
    }
    case WM_SIZE:
        if (w != SIZE_MINIMIZED) { gfx::Resize(LOWORD(l), HIWORD(l)); if (g_tahoe) tahoe::Size(h, LOWORD(l), HIWORD(l), g_dark); g_scroll = g_scrollTarget = std::min(g_scroll, MaxScroll()); Render(); }
        return 0;
    case WM_DPICHANGED: {
        g_dpi = HIWORD(w); g_scale = g_dpi / 96.f;
        RECT* r = (RECT*)l;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_ACTIVATE:
        g_winActive = LOWORD(w) != WA_INACTIVE;
        shadow::SetActive(g_winActive);
        shadow::Update();
        Invalidate();
        break;
    case WM_WINDOWPOSCHANGED: shadow::Update(); break;
    case WM_SETTINGCHANGE:
        if (l && !lstrcmpiW((LPCWSTR)l, L"ImmersiveColorSet")) { g_dark = ReadDark(); ApplyWindowTheme(); Invalidate(); }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps; BeginPaint(h, &ps); EndPaint(h, &ps);
        LARGE_INTEGER a, b; QueryPerformanceCounter(&a);
        Render();
        QueryPerformanceCounter(&b);
        if (g_rol.on) { double ms = (b.QuadPart - a.QuadPart) * 1000.0 / Qpf(); g_rol.n++; g_rol.soma += ms; g_rol.pior = std::max(g_rol.pior, ms); }
        return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEMOVE: {
        D2D1_POINT_2F p = ToDip(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (g_drag.g >= 0 && (w & MK_LBUTTON)) { SliderTo({ H_SLIDER, g_drag.track, g_drag.g, g_drag.r }, p.x); return 0; }
        bool hl = p.x >= 10 && p.x < 84 && p.y < 40;
        ampliacao::Verde(h, hl && p.x >= 58 && p.x < 81);
        if (hl != g_hoverLights) { g_hoverLights = hl; Invalidate(); }
        TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
        TrackMouseEvent(&tme);
        const Hit* ht = HitAt(p);
        bool hand = ht && (ht->kind == H_SIDE || ht->kind == H_ACCOUNT);
        (void)hand;
        SetCursor(LoadCursorW(nullptr, ht && ht->kind == H_SEARCH ? IDC_IBEAM : IDC_ARROW));
        return 0;
    }
    case WM_MOUSELEAVE: ampliacao::Verde(h, false); if (g_hoverLights) { g_hoverLights = false; Invalidate(); } return 0;
    case WM_SETCURSOR: if (LOWORD(l) == HTCLIENT) return TRUE; break;
    case WM_LBUTTONDOWN: SetFocus(h); Click(ToDip(GET_X_LPARAM(l), GET_Y_LPARAM(l)), false); return 0;
    case WM_LBUTTONUP:
        if (g_drag.g >= 0) { g_drag.g = g_drag.r = -1; ReleaseCapture(); ShowPage(); Invalidate(); }
        return 0;
    case WM_MOUSEWHEEL: {
        POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        ScreenToClient(h, &pt);
        float d = -GET_WHEEL_DELTA_WPARAM(w) / 120.f * 48;
        if (pt.x / g_scale < SIDE_W) g_sideScroll = std::clamp(g_sideScroll + d, 0.f, SideMaxScroll());
        else {
            g_scrollTarget = std::clamp(g_scrollTarget + d, 0.f, MaxScroll());
            if (!g_rol.on) { g_rol = {}; g_rol.on = true; QueryPerformanceCounter(&g_rol.ini); g_rol.ult = g_rol.ini; }
            SetTimer(h, T_ANIM, 4, nullptr);
        }
        Invalidate();
        return 0;
    }
    case WM_TIMER:
        if (w == T_FRENTE) {  // já à mostra (o Shell terminou a animação de abrir): para a frente de verdade
            KillTimer(h, T_FRENTE);
            if (GetForegroundWindow() != h) { keybd_event(VK_MENU, 0, 0, 0); SetForegroundWindow(h); keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0); }
            BringWindowToTop(h);
            return 0;
        }
        if (w == T_REFRESH) {
            if (tahoe::Reload()) { g_tahoe = tahoe::On(); g_dark = ReadDark(); ApplyWindowTheme(); shadow::Update(); Invalidate(); }
            if (!g_sheet.open && g_drag.g < 0 && !ctxmenu::IsOpen()) {
                bool d = ReadDark();
                if (d != g_dark) { g_dark = d; ApplyWindowTheme(); }
                LoadSettings();
                ShowPage();
                Invalidate();
            }
        } else if (w == T_ANIM) {
            // passo pelo tempo real (antes 30% por tique de 15 ms: o SetTimer anda a ~64 Hz e a rolagem dava trancos)
            LARGE_INTEGER now; QueryPerformanceCounter(&now);
            double dt = g_rol.on ? (now.QuadPart - g_rol.ult.QuadPart) * 1000.0 / Qpf() : 15.0;
            if (g_rol.on) g_rol.ult = now;
            float diff = g_scrollTarget - g_scroll;
            if (std::fabs(diff) < 0.5f) g_scroll = g_scrollTarget; else g_scroll += diff * (float)(1.0 - std::pow(0.7, std::clamp(dt, 1.0, 60.0) / 15.0));
            if (g_scroll == g_scrollTarget && g_rol.on) {
                double tot = (now.QuadPart - g_rol.ini.QuadPart) * 1000.0 / Qpf();
                Log(L"rolagem: %d quadros em %.0f ms (%.0f quadros/s; desenho médio %.2f ms, pior %.2f ms)", g_rol.n, tot,
                    tot > 0 ? g_rol.n * 1000.0 / tot : 0.0, g_rol.n ? g_rol.soma / g_rol.n : 0.0, g_rol.pior);
                g_rol.on = false;
            }
            if (g_scroll == g_scrollTarget && g_toggleAnim.empty()) KillTimer(h, T_ANIM);
            Invalidate();
        } else if (w == T_CARET) {
            if (!g_searchFocus && !g_sheet.open) KillTimer(h, T_CARET);
            Invalidate();
        }
        return 0;
    case WM_KEYDOWN: {
        bool shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0;
        if (g_sheet.open) {
            if (w == VK_ESCAPE) { CloseSheet(); return 0; }
            if (w == VK_RETURN) { auto f = g_sheet.ok; std::wstring t = g_sheet.field.text; CloseSheet(); if (f) f(t); return 0; }
            if (g_sheet.field.OnKey((UINT)w, shift, ctrl)) { g_caretAt = GetTickCount(); Invalidate(); }
            return 0;
        }
        if (ctrl && w == 'F') { g_searchFocus = true; g_search.Select(0, (int)g_search.text.size()); SetTimer(h, T_CARET, 530, nullptr); Invalidate(); return 0; }
        if (ctrl && w == 'W') { PostMessageW(h, WM_CLOSE, 0, 0); return 0; }
        if (ctrl && w == VK_OEM_4) { Go(-1); return 0; }
        if (ctrl && w == VK_OEM_6) { Go(1); return 0; }
        if (g_searchFocus) {
            if (w == VK_ESCAPE) { g_search.SetText(L"", false); g_searchFocus = false; UpdateSideVisible(); Invalidate(); return 0; }
            if (w == VK_RETURN && !g_sideVisible.empty()) { SelectSide(g_sideVisible[0]); return 0; }
            if (g_search.OnKey((UINT)w, shift, ctrl)) { g_caretAt = GetTickCount(); UpdateSideVisible(); Invalidate(); }
            return 0;
        }
        if (w == VK_UP || w == VK_DOWN) {  // anda pela barra lateral
            int cur = g_histAt >= 0 ? g_hist[g_histAt].side : -1;
            auto it = std::find(g_sideVisible.begin(), g_sideVisible.end(), cur);
            int pos = it == g_sideVisible.end() ? 0 : (int)(it - g_sideVisible.begin()) + (w == VK_DOWN ? 1 : -1);
            if (pos >= 0 && pos < (int)g_sideVisible.size()) SelectSide(g_sideVisible[pos]);
            return 0;
        }
        return 0;
    }
    case WM_CHAR: {
        wchar_t ch = (wchar_t)w;
        if (ch < 32) return 0;
        if (g_sheet.open) { g_sheet.field.OnChar(ch); g_caretAt = GetTickCount(); Invalidate(); return 0; }
        if (!g_searchFocus) { g_searchFocus = true; SetTimer(h, T_CARET, 530, nullptr); }  // digitar vai para a busca, como no Mac
        g_search.OnChar(ch);
        g_caretAt = GetTickCount();
        UpdateSideVisible();
        if (!g_sideVisible.empty()) {}
        Invalidate();
        return 0;
    }
    case WM_COPYDATA: {  // outra instância pediu uma página
        auto* cd = (COPYDATASTRUCT*)l;
        if (cd->dwData == 0x414A5354 && cd->cbData) {
            std::wstring id((const wchar_t*)cd->lpData, cd->cbData / sizeof(wchar_t));
            int i = SidebarIndexFor(id);
            if (i >= 0) SelectSide(i);
            else if (id == L"atualizacao") { int g = SidebarIndexFor(L"geral"); if (g >= 0) SelectSide(g); OpenUpdatePage(); }
        }
        if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
        keybd_event(VK_MENU, 0, 0, 0); SetForegroundWindow(h); keybd_event(VK_MENU, 0, KEYEVENTF_KEYUP, 0);  // já aberta: para a frente mesmo atrás de outro app
        BringWindowToTop(h);
        return TRUE;
    }
    case WM_APP_REBUILD:
        if (tahoe::Reload()) { g_tahoe = tahoe::On(); g_dark = ReadDark(); ApplyWindowTheme(); shadow::Update(); }  // Tema trocado aqui mesmo
        if (g_drag.g < 0 && !ctxmenu::IsOpen()) ShowPage();
        Invalidate();
        return 0;
    case WM_APP_BITMAP: {
        auto* res = (BmpResult*)l;
        ID2D1Bitmap* b = res->px.empty() ? nullptr : gfx::CreateBitmapBGRA(res->w, res->h, res->px.data(), res->w * 4);
        {
            std::lock_guard<std::mutex> lk(g_bmpMx);
            g_bmps[res->key] = b;
            g_bmpPending.erase(res->key);
        }
        delete res;
        Invalidate();
        return 0;
    }
    case WM_CLOSE: DestroyWindow(h); return 0;
    case WM_DESTROY: shadow::Destroy(); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

std::wstring PageArg(int argc, wchar_t** argv) {
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a.rfind(L"--pagina=", 0) == 0) return a.substr(9);
        if (a.rfind(L"ms-settings:", 0) == 0) return a;
    }
    return L"";
}

}  // namespace

int aj::Menu(D2D1_RECT_F anchor, const std::vector<std::wstring>& opts, int cur) {
    std::vector<ctxmenu::Item> items;
    for (int i = 0; i < (int)opts.size(); i++) { ctxmenu::Item it; it.label = Tr(opts[i]); it.id = i + 1; it.checked = i == cur; items.push_back(it); }
    POINT pt{ (LONG)(anchor.left * g_scale), (LONG)(anchor.bottom * g_scale) };
    ClientToScreen(g_hwnd, &pt);
    int id = ctxmenu::ShowItems(g_hwnd, pt, items);
    Invalidate();
    return id > 0 ? id - 1 : -1;
}

void aj::PopupBadge(Ctx& c, float cx, float cy) {
    if (g_tahoe) {
        c.br->SetColor(g_dark ? D2D1::ColorF(1, 1, 1, 0.10f) : D2D1::ColorF(0, 0, 0, 0.06f));
        c.dc->FillEllipse(D2D1::Ellipse({ cx, cy }, 10, 10), c.br);
    }
    DrawUpDown(c, cx, cy, g_tahoe ? c.text : c.text2);
}

void aj::ButtonFace(Ctx& c, D2D1_RECT_F b, const std::wstring& label) { DrawButton(c, b, label, true); }

// Pedido de senha (Wi-Fi) e parecidos: folha por cima do conteúdo, como no Mac.
void AskText(const std::wstring& title, const std::wstring& message, const std::wstring& okLabel, bool password,
             std::function<void(const std::wstring&)> ok) {
    g_sheet.open = true;
    g_sheet.title = title; g_sheet.message = message; g_sheet.okLabel = okLabel; g_sheet.password = password;
    g_sheet.field.SetText(L"", false);
    g_sheet.ok = ok;
    g_caretAt = GetTickCount();
    SetTimer(g_hwnd, T_CARET, 530, nullptr);
    Invalidate();
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring page = PageArg(argc, argv);
    LocalFree(argv);

    // uma janela só: se já está aberta, só leva para a página pedida
    if (HWND other = FindWindowW(L"AjustesJanela", nullptr)) {
        COPYDATASTRUCT cd{ 0x414A5354, (DWORD)(page.size() * sizeof(wchar_t)), (void*)page.c_str() };
        AllowSetForegroundWindow(ASFW_ANY);
        SendMessageW(other, WM_COPYDATA, 0, (LPARAM)&cd);
        return 0;
    }

    g_inst = hi;
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) { dir = std::wstring(p) + L"\\Shell"; CoTaskMemFree(p); }
    CreateDirectoryW(dir.c_str(), nullptr);
    g_settingsPath = dir + L"\\ajustes.txt";
    {
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring bin = exe; bin = bin.substr(0, bin.find_last_of(L'\\'));
        simbolo::Dir() = bin.substr(0, bin.find_last_of(L'\\')) + L"\\icones\\simbolos";
    }
    _wfopen_s(&g_log, (dir + L"\\ajustes-app.log").c_str(), L"w, ccs=UTF-8");
    LoadSettings();
    OleInitialize(nullptr);
    ctxmenu::Init(hi, g_log);
    g_dark = ReadDark();

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hi;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));  // ícone do .exe (Settings.png)
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"AjustesJanela";
    RegisterClassExW(&wc);

    // 715 x 640 pt, centralizada na área de trabalho
    UINT dpi = GetDpiForSystem();
    g_dpi = (float)dpi; g_scale = g_dpi / 96.f;
    RECT wa; SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    const bool th0 = tahoe::On(), gg0 = th0 && tahoe::Golden();  // Tahoe: 801 × 625 (guia, seção 43); macOS 27: 740 × 690 (captura)
    int ww = (int)((gg0 ? 740 : th0 ? 801 : 715) * g_scale), wh = (int)(std::min(gg0 ? 690.f : th0 ? 625.f : 640.f, (wa.bottom - wa.top) / g_scale - 60) * g_scale);
    int wx = wa.left + (wa.right - wa.left - ww) / 2, wy = wa.top + (wa.bottom - wa.top - wh) / 2 - (int)(20 * g_scale);
    g_hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, wc.lpszClassName, TrPick(L"Ajustes do Sistema", L"System Settings"), WS_OVERLAPPEDWINDOW,
                             wx, std::max<int>(wy, wa.top), ww, wh, nullptr, nullptr, hi, nullptr);
    ampliacao::Janela(g_hwnd);  // abrir e fechar animados (o Shell anima)
    g_dpi = (float)GetDpiForWindow(g_hwnd); g_scale = g_dpi / 96.f;
    MARGINS margins{ -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(g_hwnd, &margins);
    g_tahoe = tahoe::On();
    ApplyWindowTheme();
    SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    if (!gfx::Init(g_hwnd)) { MessageBoxW(nullptr, L"Não foi possível iniciar o Direct2D.", L"Ajustes do Sistema", MB_ICONERROR); return 1; }
    CreateFormats();
    shadow::Create(g_hwnd);

    UpdateSideVisible();
    int start = SidebarIndexFor(page.empty() ? aj::Setting(L"ajustes_pagina", L"geral") : page);
    if (start < 0) start = SidebarIndexFor(L"geral");
    g_hist.push_back({ std::max(start, 0), {} });
    g_histAt = 0;
    ShowPage();
    g_leoAll = page.empty();  // Leopard: abre no Mostrar Todos, a não ser que tenham pedido um painel
    if (page == L"atualizacao") OpenUpdatePage();

    // escondida (cloak) até o primeiro quadro estar pronto: sem isso a janela aparecia com a área da
    // direita ainda vazia e transparente (o que estava atrás aparecia através dela por alguns quadros)
    BOOL cloak = TRUE;
    DwmSetWindowAttribute(g_hwnd, 13 /*DWMWA_CLOAK*/, &cloak, sizeof(cloak));
    ShowWindow(g_hwnd, SW_SHOW);
    SetForegroundWindow(g_hwnd);
    Render();
    DwmFlush();
    ampliacao::Revelar(g_hwnd);  // abrir animado (o Shell mostra no fim) ou já
    SetTimer(g_hwnd, T_REFRESH, 2500, nullptr);
    // aberta pelo Dock (o Shell não está na frente para passar a vez): o SetForegroundWindow lá em cima, com a
    // janela ainda escondida, não pegava e os Ajustes abriam atrás do app da frente
    SetTimer(g_hwnd, T_FRENTE, 350, nullptr);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    OleUninitialize();
    if (g_log) fclose(g_log);
    return 0;
}
