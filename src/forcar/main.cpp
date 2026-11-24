// Forçar Encerrar Aplicativos (LiquinixForceQuit.exe), como o do Mac (Command+Option+Esc): a lista dos apps
// abertos, com "(Não Respondendo)" em vermelho nos travados; escolher um e clicar em Forçar Encerrar pede
// confirmação ("Você perderá as alterações não salvas") e encerra o processo.
// Ctrl+Alt+Esc e o menu da Apple > Forçar Encerrar… abrem esta janela. Com --memoria ela é a do Mac quando
// a memória acaba ("Seu sistema ficou sem memória para aplicativos."): a lista vem ordenada por quanto cada
// app gasta (o app e os processos filhos dele, como as abas do navegador). O Shell abre sozinho nesse modo
// quando a pressão de memória fica vermelha (shell\memoria.cpp); encerrar é sempre o usuário quem decide.
// Ficam fora da lista o Shell e o Explorador (são a área de trabalho e a barra). Apps da Store aparecem pelo
// processo deles (não o ApplicationFrameHost, que levaria todos juntos).
#include "../comum/app2d.h"
#include "../comum/nomes.h"
#include "../comum/pressao.h"
#include "../comum/semaforo.h"
#include <tlhelp32.h>
#include <shellapi.h>
#include <map>
#pragma comment(lib, "version.lib")

using namespace app2d;

namespace {

struct App {
    DWORD pid = 0;
    std::wstring name, path;
    bool hung = false;
    ULONGLONG mem = 0;
};

std::vector<App> g_apps;
DWORD g_sel;                  // pid escolhido (0 = nenhum)
bool g_memory;                // modo "ficou sem memória"
int g_sheet;                  // 0 sem confirmação, 1 confirmando, 2 não deu para encerrar
App g_target;
int g_hover = -1;             // linha sob o mouse; -2 botão principal; -3 Cancelar da confirmação; -4 confirmar
float g_scroll;
IDWriteTextFormat *g_tfBody, *g_tfBold, *g_tfRow, *g_tfSmall, *g_tfBtn, *g_tfMem, *g_tfSheetTitle, *g_tfSheet;
IWICImagingFactory* g_wic;
std::map<std::wstring, IWICBitmapSource*> g_wicIcons;
std::map<std::wstring, ID2D1Bitmap*> g_icons;
ID2D1RenderTarget* g_iconsRt;
const UINT_PTR T_REFRESH = 1;
const float PAD = 20;
// macOS 27 Golden Gate (Force-Quit.png, 2x): janela de 370 × 310, fundo branco, lista branca com borda, linhas de 22,
// seleção azul de borda a borda (sem canto) e o botão em cápsula de 28 (semaforo::Aqua = tema Golden Gate)
#define GG (semaforo::Aqua())
#define W (GG ? 370.f : 440.f)
#define ROW (GG ? 22.f : 24.f)
#define BTN_H (GG ? 28.f : 24.f)

std::wstring Lower(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }

// os nossos apps com o nome do Mac em português (o FileDescription deles é em inglês, para o Gerenciador de Tarefas)
std::wstring OurName(const std::wstring& exe) {
    static const struct { const wchar_t *exe, *pt, *en; } T[] = {
        { L"liquinixfinder.exe", L"Finder", L"Finder" }, { L"liquinixsettings.exe", L"Ajustes do Sistema", L"System Settings" },
        { L"liquinixactivitymonitor.exe", L"Monitor de Atividade", L"Activity Monitor" }, { L"liquinixterminal.exe", L"Terminal", L"Terminal" },
        { L"liquinixcalculator.exe", L"Calculadora", L"Calculator" }, { L"liquinixnotes.exe", L"Notas", L"Notes" },
        { L"liquinixscreenshot.exe", L"Captura de Tela", L"Screenshot" }, { L"liquinixpreview.exe", L"Pré-Visualização", L"Preview" },
        { L"liquinixreminders.exe", L"Lembretes", L"Reminders" }, { L"liquinixcalendar.exe", L"Calendário", L"Calendar" },
        { L"liquinixtextedit.exe", L"TextEdit", L"TextEdit" }, { L"liquinixclock.exe", L"Relógio", L"Clock" },
        { L"liquinixweather.exe", L"Clima", L"Weather" }, { L"liquinixcontacts.exe", L"Contatos", L"Contacts" },
        { L"liquinixphotos.exe", L"Fotos", L"Photos" }, { L"liquinixtimemachine.exe", L"Time Machine", L"Time Machine" },
        { L"liquinixmusic.exe", L"Música", L"Music" }, { L"liquinixquicktime.exe", L"QuickTime Player", L"QuickTime Player" },
    };
    for (auto& t : T) if (exe == t.exe) return TrPick(t.pt, t.en);
    return L"";
}

std::wstring Describe(const std::wstring& path) {
    DWORD dummy = 0, n = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (!n) return L"";
    std::vector<BYTE> buf(n);
    if (!GetFileVersionInfoW(path.c_str(), 0, n, buf.data())) return L"";
    struct LangCp { WORD lang, cp; }* lc = nullptr; UINT len = 0;
    if (!VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", (void**)&lc, &len) || len < sizeof(LangCp)) return L"";
    wchar_t key[80]; swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\FileDescription", lc[0].lang, lc[0].cp);
    wchar_t* v = nullptr;
    if (!VerQueryValueW(buf.data(), key, (void**)&v, &len) || !v || !*v) return L"";
    return v;
}

std::wstring PathOf(DWORD pid) {
    std::wstring r;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        wchar_t p[MAX_PATH]; DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(h, 0, p, &n)) r = p;
        CloseHandle(h);
    }
    return r;
}

ULONGLONG PrivateOf(DWORD pid) {
    ULONGLONG r = 0;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        PROCESS_MEMORY_COUNTERS_EX pm{ sizeof(pm) };
        if (GetProcessMemoryInfo(h, (PROCESS_MEMORY_COUNTERS*)&pm, sizeof(pm))) r = pm.PrivateUsage;
        CloseHandle(h);
    }
    return r;
}

struct Found { std::map<DWORD, App> apps; DWORD self, shell; };

BOOL CALLBACK ChildPid(HWND c, LPARAM l) {
    auto* p = (std::pair<DWORD, DWORD>*)l;
    DWORD pid = 0; GetWindowThreadProcessId(c, &pid);
    if (pid && pid != p->first) { p->second = pid; return FALSE; }
    return TRUE;
}

BOOL CALLBACK OnWin(HWND h, LPARAM l) {
    Found& f = *(Found*)l;
    if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    LONG st = GetWindowLongW(h, GWL_STYLE), ex = GetWindowLongW(h, GWL_EXSTYLE);
    if ((ex & WS_EX_TOOLWINDOW) || (st & WS_CAPTION) != WS_CAPTION) return TRUE;
    BOOL cloaked = FALSE; DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked && !IsIconic(h)) return TRUE;
    if (GetWindowTextLengthW(h) == 0) return TRUE;
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (!pid || pid == f.self) return TRUE;
    std::wstring path = PathOf(pid), exe = Lower(path.substr(path.find_last_of(L'\\') + 1));
    if (exe == L"applicationframehost.exe") {  // app da Store: o processo de verdade é o da janela filha
        std::pair<DWORD, DWORD> p{ pid, 0 };
        EnumChildWindows(h, ChildPid, (LPARAM)&p);
        if (!p.second) return TRUE;
        pid = p.second; path = PathOf(pid); exe = Lower(path.substr(path.find_last_of(L'\\') + 1));
    }
    if (path.empty() || exe == L"explorer.exe" || exe == L"liquinixshell.exe" || exe == L"liquinixwindow32host.exe" || exe == L"liquinixforcequit.exe") return TRUE;
    App& a = f.apps[pid];
    if (!a.pid) {
        a.pid = pid; a.path = path;
        a.name = OurName(exe);
        if (a.name.empty()) a.name = Describe(path);
        // descrição que parece um identificador ("WhatsApp.Root"): o título da janela diz melhor o nome
        if (a.name.find(L'.') != std::wstring::npos && a.name.find(L' ') == std::wstring::npos) {
            wchar_t t[128] = L""; GetWindowTextW(h, t, 128);
            if (t[0] && wcslen(t) < 40) a.name = t;
        }
        if (a.name.empty()) { a.name = path.substr(path.find_last_of(L'\\') + 1); size_t d = a.name.rfind(L'.'); if (d != std::wstring::npos) a.name.erase(d); }
    }
    if (IsHungAppWindow(h)) a.hung = true;
    return TRUE;
}

void Refresh() {
    Found f; f.self = GetCurrentProcessId();
    EnumWindows(OnWin, (LPARAM)&f);
    // memória: o app e os filhos dele (as abas e os ajudantes do navegador contam no navegador)
    std::map<DWORD, DWORD> parent;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{ sizeof(pe) };
        for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) parent[pe.th32ProcessID] = pe.th32ParentProcessID;
        CloseHandle(snap);
    }
    if (g_memory) {
        for (auto& [pid, ppid] : parent) {
            DWORD cur = pid;
            for (int depth = 0; depth < 6 && cur; depth++) {
                auto it = f.apps.find(cur);
                if (it != f.apps.end()) { it->second.mem += PrivateOf(pid); break; }
                auto p = parent.find(cur);
                if (p == parent.end() || p->second == cur) break;
                cur = p->second;
            }
        }
    }
    g_apps.clear();
    for (auto& [pid, a] : f.apps) g_apps.push_back(a);
    if (g_memory) std::sort(g_apps.begin(), g_apps.end(), [](const App& a, const App& b) { return a.mem > b.mem; });
    else std::sort(g_apps.begin(), g_apps.end(), [](const App& a, const App& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    bool still = false;
    for (auto& a : g_apps) if (a.pid == g_sel) still = true;
    if (!still) g_sel = 0;
    InvalidateRect(wnd, nullptr, FALSE);
}

ID2D1Bitmap* Icon(const std::wstring& path) {
    if (g_iconsRt != rt) { for (auto& [k, b] : g_icons) if (b) b->Release(); g_icons.clear(); g_iconsRt = rt; }
    auto it = g_icons.find(path);
    if (it != g_icons.end()) return it->second;
    ID2D1Bitmap* bmp = nullptr;
    IWICBitmapSource*& src = g_wicIcons[path];
    if (!src && g_wic) {
        SHFILEINFOW fi{};
        if (SHGetFileInfoW(path.c_str(), 0, &fi, sizeof(fi), SHGFI_ICON | SHGFI_LARGEICON) && fi.hIcon) {
            IWICBitmap* wb = nullptr;
            if (SUCCEEDED(g_wic->CreateBitmapFromHICON(fi.hIcon, &wb)) && wb) {
                IWICFormatConverter* cv = nullptr;
                if (SUCCEEDED(g_wic->CreateFormatConverter(&cv)) && SUCCEEDED(cv->Initialize(wb, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) src = cv;
                else if (cv) cv->Release();
                wb->Release();
            }
            DestroyIcon(fi.hIcon);
        }
    }
    if (src) rt->CreateBitmapFromWicBitmap(src, nullptr, &bmp);
    g_icons[path] = bmp;
    return bmp;
}

std::wstring Bytes(ULONGLONG b) {
    wchar_t s[32];
    if (b >= (1ull << 30)) swprintf_s(s, L"%.2f GB", b / 1073741824.0);
    else swprintf_s(s, L"%.0f MB", b / 1048576.0);
    std::wstring r = s;
    if (idioma::DecimalComma()) for (auto& c : r) if (c == L'.') c = L',';
    return r;
}

struct Pal { D2D1_COLOR_F bg, list, border, text, text2, sel, red, btn, btnText, dim; };
Pal P() {
    bool d = Dark();
    if (GG) return d ? Pal{ Rgb(30, 30, 30), Rgb(30, 30, 30), Rgb(255, 255, 255, 0.14f), Rgb(255, 255, 255, 0.88f), Rgb(255, 255, 255, 0.55f), Rgb(0, 100, 220), Rgb(255, 69, 58), Rgb(255, 255, 255, 0.14f), Rgb(255, 255, 255, 0.9f), Rgb(0, 0, 0, 0.35f) }
                     : Pal{ Rgb(255, 255, 255), Rgb(255, 255, 255), Rgb(0, 0, 0, 0.16f), Rgb(0, 0, 0, 0.85f), Rgb(0, 0, 0, 0.5f), Rgb(0, 100, 220), Rgb(255, 59, 48), Rgb(0, 0, 0, 0.06f), Rgb(0, 0, 0, 0.85f), Rgb(0, 0, 0, 0.18f) };
    return d ? Pal{ Rgb(44, 44, 46), Rgb(30, 30, 31), Rgb(255, 255, 255, 0.12f), Rgb(255, 255, 255, 0.88f), Rgb(255, 255, 255, 0.55f), Rgb(0, 88, 208), Rgb(255, 69, 58), Rgb(255, 255, 255, 0.16f), Rgb(255, 255, 255, 0.9f), Rgb(0, 0, 0, 0.35f) }
             : Pal{ Rgb(236, 236, 236), Rgb(255, 255, 255), Rgb(0, 0, 0, 0.14f), Rgb(0, 0, 0, 0.85f), Rgb(0, 0, 0, 0.5f), Rgb(0, 100, 225), Rgb(255, 59, 48), Rgb(255, 255, 255), Rgb(0, 0, 0, 0.85f), Rgb(0, 0, 0, 0.18f) };
}

// medidas da tela atual (em DIP)
struct Layout { D2D1_RECT_F head, list, note, btn, sheet, sCancel, sOk; };
Layout L() {
    D2D1_SIZE_F s = Size();
    Layout l;
    float headH = g_memory ? 74 : 54;
    l.head = D2D1::RectF(PAD, 14, s.width - PAD, 14 + headH);
    l.list = D2D1::RectF(PAD, l.head.bottom + 4, s.width - PAD, s.height - (GG ? 61 : 56));
    float bw = std::max(TextW(g_tfBtn, Tr(L"Forçar Encerrar")) + (GG ? 36 : 28), GG ? 84.f : 110.f);
    l.btn = GG ? D2D1::RectF(s.width - 16 - bw, s.height - 16 - BTN_H, s.width - 16, s.height - 16)  // macOS 27: a 16 das bordas
               : D2D1::RectF(s.width - PAD - bw, s.height - 40, s.width - PAD, s.height - 40 + BTN_H);
    l.note = D2D1::RectF(PAD, s.height - (GG ? 50 : 44), l.btn.left - 12, s.height - 12);
    float sw = std::min(s.width - 40, 320.f), sh = 196;
    l.sheet = D2D1::RectF((s.width - sw) / 2, 0, (s.width + sw) / 2, sh);
    float half = (sw - 48) / 2;
    l.sCancel = D2D1::RectF(l.sheet.left + 16, sh - 44, l.sheet.left + 16 + half, sh - 16);
    l.sOk = D2D1::RectF(l.sCancel.right + 16, sh - 44, l.sheet.right - 16, sh - 16);
    return l;
}

void Button(D2D1_RECT_F r, const std::wstring& label, bool primary, bool enabled, bool hover, const Pal& p) {
    float rad = GG ? (r.bottom - r.top) / 2 : (r.bottom - r.top) / 2 < 7 ? (r.bottom - r.top) / 2 : 6;  // macOS 27: cápsula
    if (primary && enabled) Fill(r, hover ? Rgb(20, 120, 245) : Rgb(0, 122, 255), rad);
    else {
        Fill(r, p.btn, rad);
        br->SetColor(p.border); rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + 0.5f, r.top + 0.5f, r.right - 0.5f, r.bottom - 0.5f), rad, rad), br, 1);
    }
    D2D1_COLOR_F c = primary && enabled ? Rgb(255, 255, 255) : enabled ? p.btnText : p.text2;
    Text(label, g_tfBtn, r, c);
}

int Visible() { Layout l = L(); return (int)((l.list.bottom - l.list.top - 2) / ROW); }

void Paint() {
    if (!BeginPaint()) return;
    Pal p = P();
    Layout l = L();
    rt->Clear(p.bg);
    if (g_memory) {
        Text(Tr(L"Seu sistema ficou sem memória para aplicativos."), g_tfBold, D2D1::RectF(l.head.left, l.head.top, l.head.right, l.head.top + 22), p.text);
        Text(Tr(L"Para evitar problemas com o computador, encerre os aplicativos que não estiver usando."), g_tfBody, D2D1::RectF(l.head.left, l.head.top + 24, l.head.right, l.head.bottom), p.text);
    } else {
        Text(Tr(L"Se um aplicativo não responder por algum tempo, selecione o nome dele e clique em Forçar Encerrar."), g_tfBody, l.head, p.text);
    }
    // lista
    Fill(l.list, p.list, GG ? 0.f : 5.f);
    rt->PushAxisAlignedClip(l.list, D2D1_ANTIALIAS_MODE_ALIASED);
    int first = (int)g_scroll;
    float y = l.list.top + 1;
    for (int i = first; i < (int)g_apps.size() && y < l.list.bottom; i++, y += ROW) {
        App& a = g_apps[i];
        D2D1_RECT_F rr = D2D1::RectF(l.list.left + 1, y, l.list.right - 1, y + ROW);
        bool sel = a.pid == g_sel;
        if (sel) { if (GG) Fill(rr, p.sel, 0); else Fill(D2D1::RectF(rr.left + 4, rr.top + 1, rr.right - 4, rr.bottom - 1), p.sel, 5); }
        float iy = (rr.top + rr.bottom) / 2;
        if (ID2D1Bitmap* b = Icon(a.path)) rt->DrawBitmap(b, GG ? D2D1::RectF(rr.left + 7, iy - 8.5f, rr.left + 24, iy + 8.5f) : D2D1::RectF(rr.left + 10, rr.top + 3, rr.left + 28, rr.top + 21), 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        float tx = rr.left + (GG ? 29 : 36), right = rr.right - 10;
        if (g_memory) {
            std::wstring m = Bytes(a.mem);
            float mw = TextW(g_tfMem, m);
            Text(m, g_tfMem, D2D1::RectF(right - mw, rr.top, right, rr.bottom), sel ? Rgb(255, 255, 255) : p.text2);
            right -= mw + 12;
        }
        std::wstring name = a.name;
        float nw = std::min(TextW(g_tfRow, name), right - tx);
        Text(name, g_tfRow, D2D1::RectF(tx, rr.top, tx + nw + 1, rr.bottom), sel ? Rgb(255, 255, 255) : p.text);
        if (a.hung) Text(Tr(L"(Não Respondendo)"), g_tfRow, D2D1::RectF(tx + nw + 5, rr.top, right, rr.bottom), sel ? Rgb(255, 210, 205) : p.red);
    }
    rt->PopAxisAlignedClip();
    br->SetColor(p.border);
    rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(l.list.left + 0.5f, l.list.top + 0.5f, l.list.right - 0.5f, l.list.bottom - 0.5f), GG ? 0.f : 5.f, GG ? 0.f : 5.f), br, 1);
    // rodapé
    Text(Tr(L"Você pode abrir esta janela pressionando Ctrl\u00A0+\u00A0Alt\u00A0+\u00A0Esc."), g_tfSmall, l.note, p.text2);
    Button(l.btn, Tr(L"Forçar Encerrar"), true, g_sel != 0 && !g_sheet, g_hover == -2, p);
    // confirmação (a folha que desce da barra de título, como no Mac)
    if (g_sheet) {
        D2D1_SIZE_F s = Size();
        Fill(D2D1::RectF(0, 0, s.width, s.height), p.dim);
        D2D1_RECT_F sh = l.sheet;
        Fill(D2D1::RectF(sh.left, sh.top - 10, sh.right, sh.bottom), Dark() ? Rgb(58, 58, 60) : Rgb(246, 246, 246), 10);
        br->SetColor(p.border); rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(sh.left + 0.5f, sh.top - 10, sh.right - 0.5f, sh.bottom - 0.5f), 10, 10), br, 1);
        if (ID2D1Bitmap* b = Icon(g_target.path)) { float cx = (sh.left + sh.right) / 2; rt->DrawBitmap(b, D2D1::RectF(cx - 24, sh.top + 14, cx + 24, sh.top + 62), 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR); }
        std::wstring title, info;
        if (g_sheet == 1) {
            title = std::wstring(Tr(L"Deseja forçar o encerramento de “")) + g_target.name + Tr(L"”?");
            info = Tr(L"Você perderá as alterações não salvas.");
        } else {
            title = std::wstring(Tr(L"Não foi possível forçar o encerramento de “")) + g_target.name + L"”.";
            info = Tr(L"O aplicativo pode estar rodando como administrador.");
        }
        Text(title, g_tfSheetTitle, D2D1::RectF(sh.left + 16, sh.top + 68, sh.right - 16, sh.top + 108), p.text);
        Text(info, g_tfSheet, D2D1::RectF(sh.left + 16, sh.top + 108, sh.right - 16, sh.top + 140), p.text);
        if (g_sheet == 1) {
            Button(l.sCancel, Tr(L"Cancelar"), false, true, g_hover == -3, p);
            Button(l.sOk, Tr(L"Forçar Encerrar"), true, true, g_hover == -4, p);
        } else {
            Button(D2D1::RectF(l.sheet.left + 16, l.sOk.top, l.sheet.right - 16, l.sOk.bottom), L"OK", true, true, g_hover == -4, p);
        }
    }
    EndPaint();
}

const App* Selected() { for (auto& a : g_apps) if (a.pid == g_sel) return &a; return nullptr; }

void Ask() {
    const App* a = Selected();
    if (!a) return;
    g_target = *a;
    g_sheet = 1;
    InvalidateRect(wnd, nullptr, FALSE);
}

void Kill() {
    bool ok = false;
    if (HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, g_target.pid)) { ok = TerminateProcess(h, 1) != FALSE; CloseHandle(h); }
    g_sheet = ok ? 0 : 2;
    if (ok) { g_sel = 0; Sleep(150); Refresh(); }
    InvalidateRect(wnd, nullptr, FALSE);
}

void Move(int d) {
    if (g_apps.empty()) return;
    int i = -1;
    for (int k = 0; k < (int)g_apps.size(); k++) if (g_apps[k].pid == g_sel) i = k;
    i = std::clamp(i < 0 ? 0 : i + d, 0, (int)g_apps.size() - 1);
    g_sel = g_apps[i].pid;
    int vis = Visible();
    if (i < (int)g_scroll) g_scroll = (float)i;
    if (i >= (int)g_scroll + vis) g_scroll = (float)(i - vis + 1);
    InvalidateRect(wnd, nullptr, FALSE);
}

int HitTest(float x, float y) {
    Layout l = L();
    if (g_sheet == 1) { if (In(l.sCancel, x, y)) return -3; if (In(l.sOk, x, y)) return -4; return -9; }
    if (g_sheet == 2) return In(D2D1::RectF(l.sheet.left + 16, l.sOk.top, l.sheet.right - 16, l.sOk.bottom), x, y) ? -4 : -9;
    if (In(l.btn, x, y)) return -2;
    if (In(l.list, x, y)) { int i = (int)g_scroll + (int)((y - l.list.top - 1) / ROW); if (i >= 0 && i < (int)g_apps.size()) return i; }
    return -1;
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    static UINT msgMode = RegisterWindowMessageW(L"SequoiaForcarEncerrarModo");
    if (m == msgMode) {  // outra chamada (Ctrl+Alt+Esc de novo, ou a memória acabou): troca o modo e vem para a frente
        g_memory = w != 0; g_sheet = 0; Refresh();
        if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
        SetForegroundWindow(h);
        return 0;
    }
    switch (m) {
    case WM_PAINT: { PAINTSTRUCT ps; ::BeginPaint(h, &ps); Paint(); ::EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: OnSize(l); return 0;
    case WM_DPICHANGED: OnDpi(w, l); return 0;
    case WM_SETTINGCHANGE: InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_TIMER: if (!g_sheet) Refresh(); return 0;
    case WM_MOUSEMOVE: {
        float s = Sc(); int k = HitTest(GET_X_LPARAM(l) / s, GET_Y_LPARAM(l) / s);
        int hv = k < -1 ? k : -1;
        if (hv != g_hover) { g_hover = hv; InvalidateRect(h, nullptr, FALSE); }
        TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h }; TrackMouseEvent(&t);
        return 0;
    }
    case WM_MOUSELEAVE: if (g_hover != -1) { g_hover = -1; InvalidateRect(h, nullptr, FALSE); } return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        float s = Sc(); int k = HitTest(GET_X_LPARAM(l) / s, GET_Y_LPARAM(l) / s);
        if (k >= 0) { g_sel = g_apps[k].pid; if (m == WM_LBUTTONDBLCLK) Ask(); }
        else if (k == -2) Ask();
        else if (k == -3) g_sheet = 0;
        else if (k == -4) { if (g_sheet == 1) Kill(); else g_sheet = 0; }
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        if (g_sheet) return 0;
        float maxS = std::max(0.f, (float)g_apps.size() - Visible());
        g_scroll = std::clamp(g_scroll - GET_WHEEL_DELTA_WPARAM(w) / 120.f * 3, 0.f, maxS);
        g_scroll = std::floor(g_scroll);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN: {
        bool ctrl = GetKeyState(VK_CONTROL) < 0;
        if (ctrl && w == 'W') { DestroyWindow(h); return 0; }
        if (g_sheet) {
            if (w == VK_ESCAPE) g_sheet = 0;
            else if (w == VK_RETURN) { if (g_sheet == 1) Kill(); else g_sheet = 0; }
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        }
        if (w == VK_ESCAPE) { DestroyWindow(h); return 0; }
        if (w == VK_UP) Move(-1);
        else if (w == VK_DOWN) Move(1);
        else if (w == VK_RETURN) Ask();
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR cmd, int show) {
    g_memory = cmd && wcsstr(cmd, L"--memoria");
    if (HWND other = FindWindowW(L"ForcarEncerrarJanela", nullptr)) {
        PostMessageW(other, RegisterWindowMessageW(L"SequoiaForcarEncerrarModo"), g_memory ? 1 : 0, 0);
        SetForegroundWindow(other);
        return 0;
    }
    if (!Init()) return 1;
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_wic));
    g_tfBody = Fmt(13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_tfBold = Fmt(13, DWRITE_FONT_WEIGHT_BOLD);
    g_tfRow = Fmt(13);
    g_tfMem = Fmt(12, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_TRAILING);
    g_tfSmall = Fmt(11, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    g_tfBtn = Fmt(13, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tfSheetTitle = Fmt(13, DWRITE_FONT_WEIGHT_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, true);
    g_tfSheet = Fmt(11.5f, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_TEXT_ALIGNMENT_CENTER, true);
    if (g_tfBody) g_tfBody->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    if (!CreateMain(hi, L"ForcarEncerrarJanela", Tr(L"Forçar Encerrar Aplicativos"), Proc, (int)W, GG ? 310 : 470)) return 1;
    // como o do Mac: tamanho fixo, sem maximizar
    SetWindowLongW(wnd, GWL_STYLE, GetWindowLongW(wnd, GWL_STYLE) & ~(WS_THICKFRAME | WS_MAXIMIZEBOX));
    SetWindowPos(wnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    Refresh();
    SetTimer(wnd, T_REFRESH, 2000, nullptr);
    ShowWindow(wnd, show);
    SetForegroundWindow(wnd);
    return Run();
}
