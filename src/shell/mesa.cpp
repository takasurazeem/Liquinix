// Mesa própria (papel de parede + ícones da Área de Trabalho), para o Shell rodar sem o explorer.exe.
// Ver docs/sem-explorer.md.
//
// Ajuste `explorer` em %APPDATA%\Shell\ajustes.txt:
//   explorer=1 (padrão)  o Explorador continua sendo a mesa; a nossa só aparece se ele não estiver rodando
//                        (caiu, foi fechado), sem se registrar como shell, e some quando ele volta.
//   explorer=0           o Shell pede ao Explorador para sair (a mesma mensagem do "Sair do Explorador" do
//                        Ctrl+Shift+clique direito na barra de tarefas) e assume: janela de shell
//                        (SetShellWindow), Taskman, minimizadas escondidas (ARW_HIDE), Win+E/R/D.
// Ao sair do Shell o Explorador é aberto de novo; se o Shell cair, o vigia (outro LiquinixShell.exe,
// --mesa-vigia) abre o Explorador. Ninguém fica sem mesa.
//
// A mesa: uma janela por monitor, sempre no fundo, com o papel de parede do Windows (registro, com o modo
// de ajuste). No monitor principal, os ícones de Área de Trabalho (do usuário e pública) no jeito do Mac:
// em grade a partir do canto superior direito, nome branco com sombra, seleção azul. Clique, Ctrl+clique,
// retângulo de seleção, duplo clique abre (pastas no Finder), arrastar (OLE: para o Finder, para outra pasta
// da mesa ou para outra casa da grade), soltar arquivos de fora, F2 renomeia, Delete vai para a Lixeira,
// Ctrl+C/X/V, menu de contexto (o nosso, com as extensões do Windows) e o menu do fundo do Mac.
#include "shell.h"
#include <dwmapi.h>
#include "../menu/context_menu.h"
#include <shlobj.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <tlhelp32.h>
#include <algorithm>
#include <atomic>
#include <map>
#include <set>
#include <mutex>
#include <thread>
#include <cmath>
#include "../comum/idioma.h"

void BeginEditAt(int idx);  // renomear no lugar (F2), mais abaixo

namespace {

const wchar_t* DESK_CLASS = L"SequoiaMesa";
const wchar_t* CTL_CLASS = L"SequoiaMesaControle";
const UINT WM_MS_ICONS = WM_APP + 90;    // a thread de ícones tem imagens prontas
const UINT WM_MS_CHANGED = WM_APP + 91;  // a pasta da mesa mudou (vigia de pasta)
const UINT WM_MS_SHN = WM_APP + 92;      // SHChangeNotify
const UINT WM_MS_ENDEDIT = WM_APP + 93;  // a caixa de renomear perdeu o foco
const UINT WM_MS_REBUILD = WM_APP + 94;  // monitores/DPI mudaram
const UINT WM_MS_WALLNOW = WM_APP + 95;  // o Ajustes escolheu um papel de parede: mostra já (sem esperar o Explorador)
std::mutex g_wallMx;
std::wstring g_wallNow;                  // papel de parede escolhido agora (até o registro mostrar o mesmo)
const UINT EXPLORER_EXIT = WM_USER + 436;  // "Sair do Explorador" (Shell_TrayWnd)

// grade (pt). Tamanho do ícone, espaço da grade e tamanho do texto: Ajustes > Mesa e Dock > Personalizar a Mesa
// (mesa_icone_tamanho, mesa_espaco, mesa_texto; padrão do Mac 64 / 0 / 12, como no "Mostrar Opções de Visualização")
float ICON = 64, CELL_W = 106, CELL_H = 108, LABEL_H = 32, g_textoPt = 12;
const float MARGIN_R = 10, MARGIN_T = 8, LABEL_GAP = 5;
// relê as medidas; true se mudaram (os ícones precisam ser lidos de novo no tamanho novo)
bool LerMedidas() {
    auto num = [](const wchar_t* k, float d) { std::wstring v = shell::Setting(k, L""); return v.empty() ? d : (float)_wtof(v.c_str()); };
    float icon = std::clamp(num(L"mesa_icone_tamanho", 64), 16.f, 128.f);
    float esp = std::clamp(num(L"mesa_espaco", 0), -20.f, 80.f);
    float txt = std::clamp(num(L"mesa_texto", 12), 10.f, 16.f);
    bool mudou = icon != ICON || txt != g_textoPt || icon + 42 + esp != CELL_W;
    ICON = icon; g_textoPt = txt;
    LABEL_H = std::round(txt * 2.67f);         // duas linhas de nome
    CELL_W = std::max(icon + 42 + esp, icon + 8);  // 106 com 64: o nome de duas linhas cabe
    CELL_H = std::max(icon + 12 + LABEL_H + esp, icon + LABEL_H);
    return mudou;
}

std::atomic<bool> g_standalone;  // explorer fechado por nós, a mesa é o shell
// App da Loja antigo (UWP) pediu o Explorador (mesa::NeedExplorer): o modo sem Explorador fica suspenso
std::atomic<bool> g_uwpWant;
std::mutex g_uwpMx;
std::wstring g_uwpPending;   // app para abrir de novo quando o Explorador estiver pronto
DWORD g_uwpIdleSince;        // desde quando não há app da Loja aberto
// Ajuste explorer=fundo: o Explorador fica sempre rodando, escondido (sem barra de tarefas; a mesa dele fica
// atrás da nossa). Os apps antigos da Loja (Segurança do Windows...) só abrem com ele, e sem as idas e vindas
// do Explorador (que travaram o Shell em 09/11/2026).
std::atomic<bool> g_fundo;

// A janela da mesa do Explorador mais de cima (Progman ou WorkerW): a nossa fica logo acima dela
HWND TopExplorerDesk() {
    HWND top = nullptr;
    for (HWND h = GetTopWindow(nullptr); h; h = GetWindow(h, GW_HWNDNEXT)) {
        wchar_t c[16] = {}; GetClassNameW(h, c, 16);
        DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
        // só as visíveis: o Explorador deixa várias WorkerW escondidas no meio da pilha, e a nossa mesa subia
        // para cima dos apps (as janelas pareciam "ir para a área de trabalho", 09/11/2026)
        // nativo (Shell dentro do explorer): a mesa do Explorador é do nosso próprio processo
        if ((!wcscmp(c, L"Progman") || !wcscmp(c, L"WorkerW")) && (shell::nativo || pid != GetCurrentProcessId()) && IsWindowVisible(h)) { top = h; break; }
    }
    return top;
}
HINSTANCE g_hi;
HWND g_msgWnd, g_ctl;
bool g_weClosed;               // fomos nós que fechamos o Explorador nesta execução
bool g_explorerAtStart;        // o Explorador estava aberto quando o Shell abriu (já rodou os apps de inicialização)
DWORD g_closeAskedAt;          // pedimos para o Explorador sair (GetTickCount)
int g_closeTries;
DWORD g_explorerGoneAt;        // modo explorer=1: desde quando não há Explorador
MINIMIZEDMETRICS g_origMin{ sizeof(MINIMIZEDMETRICS) };
bool g_minChanged;
HANDLE g_endEvent;             // para o vigia: saída normal
std::wstring g_userDesk, g_publicDesk, g_posFile;

// ------------------------------------------------------------------ monitores
struct Mon {
    HWND h = nullptr;
    RECT r{};                  // tela
    bool primary = false;
    HDC wallDC = nullptr, bufDC = nullptr;
    HBITMAP wall = nullptr, buf = nullptr, wallOld = nullptr, bufOld = nullptr;
    void* bufBits = nullptr;   // pixels do buf (BGRA): no papel de parede animado a mesa é transparente
    ID2D1DCRenderTarget* rt = nullptr;
};
std::vector<Mon> g_mons;

// ------------------------------------------------------------------ ícones
struct Item {
    std::wstring path, name, key;  // key: caminho em minúsculas
    bool dir = false;
    FILETIME mtime{};
    int col = -1, row = -1;        // casa da grade (coluna contada da direita)
    float ox = 0, oy = 0;          // "Ordenar Por > Nenhum" (padrão do Mac): deslocamento livre da casa, em pt
    bool sel = false;
    std::vector<BYTE> px; UINT iw = 0, ih = 0;
    ID2D1Bitmap* bmp = nullptr;
    bool loading = false;
    // Pilhas (como o "Usar Pilhas" do Mac): stack = item de mentira que representa a pilha de um tipo; kind = o
    // tipo da pilha (ou do arquivo que está dentro dela). Arquivos de pilha fechada ficam fora da grade (col -1).
    bool stack = false;
    int kind = -1;
};
std::vector<Item> g_items;
bool g_stacks = false;               // mesa_pilhas=1
std::set<int> g_openStacks;          // pilhas abertas (o conteúdo aparece logo depois delas)
std::map<std::wstring, std::pair<int, int>> g_pos;  // posições gravadas (mesa.txt)
std::map<std::wstring, std::pair<float, float>> g_off;  // e o deslocamento livre de cada uma (mesa.txt, 3º e 4º números)
// Como no Mac (botão direito > Ordenar Por): "Nenhum" (padrão) deixa cada ícone onde foi solto; "Alinhar à Grade"
// encaixa nas casas. mesa_grade=1 liga a grade.
bool Livre() { return shell::Setting(L"mesa_grade", L"0") != L"1"; }
// "Ocultar Ícones da Mesa" (menu da mesa e Ajustes > Mesa e Dock): os arquivos continuam lá, só não aparecem
bool IconesOn() { return shell::Setting(L"mesa_icones", L"1") != L"0"; }
POINT g_grab{ -1, -1 };  // onde o ícone foi pego (px, a partir do canto do ícone), para soltar exatamente ali
IDWriteTextFormat* g_tf = nullptr;
float g_tfScale = 0;
ID2D1SolidColorBrush* g_br = nullptr;

// interação
int g_press = -1; POINT g_pressPt{}; bool g_pressCtrl = false;
bool g_band = false; POINT g_bandA{}, g_bandB{};
std::vector<bool> g_bandOrig;
bool g_dragSrc = false; int g_dragAnchor = -1;
int g_dropHi = -1;
POINT g_dropHint{ -1, -1 }; DWORD g_dropHintAt = 0;   // casa onde os próximos itens novos entram
std::wstring g_renameAfter;                             // nova pasta: renomeia quando aparecer
HWND g_edit = nullptr; WNDPROC g_editOld = nullptr; int g_editIdx = -1; HFONT g_editFont = nullptr; bool g_editEnding = false;
ULONG g_shnId = 0;
HANDLE g_watchStop = nullptr;
std::atomic<int> g_iconGen{ 0 };
std::mutex g_iconMx;
struct Loaded { int gen; std::wstring key; UINT w, h; std::vector<BYTE> px; };
std::vector<Loaded> g_loaded;

std::wstring Lower(std::wstring s) { for (auto& c : s) c = towlower(c); return s; }
std::wstring Known(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr; std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) { r = p; CoTaskMemFree(p); }
    return r;
}
std::wstring Parent(const std::wstring& p) { size_t s = p.find_last_of(L'\\'); return s == std::wstring::npos ? p : p.substr(0, s); }

Mon* Primary() { for (auto& m : g_mons) if (m.primary) return &m; return g_mons.empty() ? nullptr : &g_mons[0]; }
Mon* MonOf(HWND h) { for (auto& m : g_mons) if (m.h == h) return &m; return nullptr; }
void Invalidate(Mon& m);
float Sc() { Mon* p = Primary(); return p && p->h ? GetDpiForWindow(p->h) / 96.f : 1.f; }

// ------------------------------------------------------------------ Explorador
DWORD PidOf(HWND h) { DWORD p = 0; if (h) GetWindowThreadProcessId(h, &p); return p; }
HWND ExplorerProgman() {
    // o Progman do Windows 11 nem sempre aparece no FindWindow; a janela de shell é ele
    HWND sh = GetShellWindow();
    if (sh && (shell::nativo || PidOf(sh) != GetCurrentProcessId())) { wchar_t c[32] = {}; GetClassNameW(sh, c, 32); if (!wcscmp(c, L"Progman")) return sh; }
    HWND h = nullptr;
    while ((h = FindWindowExW(nullptr, h, L"Progman", nullptr))) if (shell::nativo || PidOf(h) != GetCurrentProcessId()) return h;
    return nullptr;
}
HWND ExplorerTrayOf(DWORD pid) {
    HWND h = nullptr;
    while ((h = FindWindowExW(nullptr, h, L"Shell_TrayWnd", nullptr))) if (PidOf(h) == pid) return h;
    return nullptr;
}
double ProcessAgeSec(DWORD pid) {
    HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!hp) return 1e9;
    FILETIME c, e, k, u, now; double age = 1e9;
    if (GetProcessTimes(hp, &c, &e, &k, &u)) {
        GetSystemTimeAsFileTime(&now);
        age = (((ULONGLONG)now.dwHighDateTime << 32 | now.dwLowDateTime) - ((ULONGLONG)c.dwHighDateTime << 32 | c.dwLowDateTime)) / 1e7;
    }
    CloseHandle(hp);
    return age;
}

// Shell do Windows (HKCU\...\Winlogon, valor Shell) é o LiquinixShell?
const wchar_t* WINLOGON = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon";
bool ReadWinlogonShell(std::wstring& out) {
    wchar_t buf[1024]; DWORD cb = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, WINLOGON, L"Shell", RRF_RT_REG_SZ, nullptr, buf, &cb) != ERROR_SUCCESS) return false;
    out = buf;
    return true;
}
bool WinlogonIsOurs() {
    std::wstring s;
    return ReadWinlogonShell(s) && (Lower(s).find(L"liquinixshell.exe") != std::wstring::npos || Lower(s).find(L"sequoiashell.exe") != std::wstring::npos);  // o nome velho também
}

// Explorador "fantasma": um explorer.exe que ficou com janelas de mesa (WorkerW) sem ser a mesa (não é o
// dono da janela de shell) e sem nenhuma janela visível. Nasce quando um Explorador que era o shell é
// encerrado à força: o Windows o reabre na hora, mas a mesa já é nossa e ele fica pela metade. Todo
// Explorador aberto depois disso travava ~3 s depois de nascer (visto em 08/11/2026). minAge: só os que já
// rodam há esse tempo (um Explorador abrindo agora ainda não teve tempo de virar a mesa).
void CloseGhostExplorers(double minAge, const wchar_t* why) {
    DWORD shellPid = PidOf(GetShellWindow());
    struct Info { bool desk = false, visible = false; };
    std::map<DWORD, Info> procs;
    EnumWindows([](HWND h, LPARAM l) -> BOOL {
        auto& m = *(std::map<DWORD, Info>*)l;
        DWORD pid = PidOf(h);
        wchar_t cls[32] = {}; GetClassNameW(h, cls, 32);
        if (!wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Progman")) m[pid].desk = true;
        else if (IsWindowVisible(h)) m[pid].visible = true;
        return TRUE;
    }, (LPARAM)&procs);
    for (auto& [pid, info] : procs) {
        if (!info.desk || info.visible || pid == shellPid || pid == GetCurrentProcessId()) continue;
        HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, pid);
        if (!hp) continue;
        wchar_t path[MAX_PATH] = {}; DWORD n = MAX_PATH;
        bool isExplorer = QueryFullProcessImageNameW(hp, 0, path, &n) && Lower(path).size() >= 12 && Lower(path).compare(Lower(path).size() - 12, 12, L"explorer.exe") == 0;
        if (isExplorer && ProcessAgeSec(pid) >= minAge) {
            BOOL ok = TerminateProcess(hp, 0);
            Log(L"mesa: Explorador fantasma encerrado (pid %lu, %.0f s, %ls): %d", pid, ProcessAgeSec(pid), why, ok);
        }
        CloseHandle(hp);
    }
}

void StartExplorer(const wchar_t* why) {
    if (ExplorerProgman()) return;
    CloseGhostExplorers(0, L"antes de abrir o Explorador");  // senão o Explorador novo trava
    // Com o LiquinixShell como shell do Windows, o explorer.exe se abre só como janela de pasta. Por alguns
    // segundos o valor volta a ser explorer.exe (ele confere ao abrir) e depois é devolvido.
    std::wstring saved;
    bool swap = ReadWinlogonShell(saved) && (Lower(saved).find(L"liquinixshell.exe") != std::wstring::npos || Lower(saved).find(L"sequoiashell.exe") != std::wstring::npos);
    if (swap) {
        const wchar_t* ex = L"explorer.exe";
        RegSetKeyValueW(HKEY_CURRENT_USER, WINLOGON, L"Shell", REG_SZ, ex, (DWORD)(wcslen(ex) + 1) * 2);
        Log(L"mesa: Winlogon Shell = explorer.exe por alguns segundos (era %ls)", saved.c_str());
    }
    wchar_t win[MAX_PATH]; GetWindowsDirectoryW(win, MAX_PATH);
    std::wstring cmd = std::wstring(L"\"") + win + L"\\explorer.exe\"";
    STARTUPINFOW si{ sizeof(si) }; PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, win, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        Log(L"mesa: Explorador aberto de novo (%ls)", why);
    } else Log(L"mesa: ERRO abrindo o Explorador (%lu)", GetLastError());
    if (swap) {
        for (int i = 0; i < 60 && !ExplorerProgman(); i++) Sleep(250);  // até 15 s pela mesa dele
        Sleep(1500);
        RegSetKeyValueW(HKEY_CURRENT_USER, WINLOGON, L"Shell", REG_SZ, saved.c_str(), (DWORD)(saved.size() + 1) * 2);
        Log(L"mesa: Winlogon Shell devolvido (%ls); Explorador %ls", saved.c_str(), ExplorerProgman() ? L"é a mesa" : L"NÃO virou a mesa");
    }
}

typedef BOOL(WINAPI* SetHwndFn)(HWND);
BOOL CallUser32(const char* fn, HWND h) {
    auto f = (SetHwndFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), fn);
    return f ? f(h) : FALSE;
}

// Pulso: a thread da interface soma 1 a cada segundo numa memória com nome que o vigia lê. Se ela ficar
// presa (esperando algo, mesmo respondendo a mensagens enviadas, como no travamento ao fechar o FL Studio),
// os temporizadores param, o pulso para e o vigia reinicia o Shell.
volatile LONG* g_pulse;
void CALLBACK PulseTimer(HWND, UINT, UINT_PTR, DWORD) { if (g_pulse) InterlockedIncrement(g_pulse); }

void StartWatchdog() {
    if (!g_pulse) {
        wchar_t pn[64]; swprintf_s(pn, L"SequoiaShellPulso-%lu", GetCurrentProcessId());
        if (HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(LONG), pn))
            g_pulse = (volatile LONG*)MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LONG));  // fica aberta até o Shell sair
        SetTimer(nullptr, 0, 1000, PulseTimer);
    }
    if (g_endEvent) { ResetEvent(g_endEvent); }
    else {
        wchar_t name[64]; swprintf_s(name, L"SequoiaMesaFim-%lu", GetCurrentProcessId());
        g_endEvent = CreateEventW(nullptr, TRUE, FALSE, name);
    }
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    wchar_t cmd[MAX_PATH + 64]; swprintf_s(cmd, L"\"%ls\" --mesa-vigia %lu", exe, GetCurrentProcessId());
    STARTUPINFOW si{ sizeof(si) }; PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
}

// ------------------------------------------------------------------ papel de parede
struct Wall { std::wstring path; int style = 10; bool tile = false; DWORD bg = 0; };
Wall ReadWall() {
    Wall w;
    wchar_t buf[1024]; DWORD cb = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"WallPaper", RRF_RT_REG_SZ, nullptr, buf, &cb) == ERROR_SUCCESS) w.path = buf;
    cb = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"WallpaperStyle", RRF_RT_REG_SZ, nullptr, buf, &cb) == ERROR_SUCCESS) w.style = _wtoi(buf);
    cb = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Desktop", L"TileWallpaper", RRF_RT_REG_SZ, nullptr, buf, &cb) == ERROR_SUCCESS) w.tile = _wtoi(buf) != 0;
    cb = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Colors", L"Background", RRF_RT_REG_SZ, nullptr, buf, &cb) == ERROR_SUCCESS) {
        int r = 0, g = 0, b = 0; swscanf_s(buf, L"%d %d %d", &r, &g, &b);
        w.bg = 0xFF000000 | (r << 16) | (g << 8) | b;  // BGRA na memória
    } else w.bg = 0xFF000000;
    {   // escolhido agora no Ajustes: vale até o Explorador gravar o mesmo no registro
        std::lock_guard<std::mutex> lk(g_wallMx);
        if (!g_wallNow.empty()) { if (!_wcsicmp(g_wallNow.c_str(), w.path.c_str())) g_wallNow.clear(); else w.path = g_wallNow; }
    }
    if (w.path.empty() || GetFileAttributesW(w.path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        std::wstring t = Known(FOLDERID_RoamingAppData) + L"\\Microsoft\\Windows\\Themes\\TranscodedWallpaper";
        if (!w.path.empty() && GetFileAttributesW(t.c_str()) != INVALID_FILE_ATTRIBUTES) w.path = t;
    }
    return w;
}

HBITMAP BuildWall(int W, int H, const Wall& wl, HDC dc) {
    BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB };
    void* bits = nullptr;
    HBITMAP hb = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!hb) return nullptr;
    auto* px = (DWORD*)bits;
    std::fill(px, px + (size_t)W * H, wl.bg);
    if (wl.path.empty()) return hb;
    IWICImagingFactory* wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return hb;
    IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* cv = nullptr; IWICBitmapScaler* sc = nullptr;
    if (SUCCEEDED(wic->CreateDecoderFromFilename(wl.path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) &&
        SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(wic->CreateFormatConverter(&cv)) &&
        SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
        UINT iw = 0, ih = 0; cv->GetSize(&iw, &ih);
        if (iw && ih) {
            int sw = iw, sh = ih;
            double kx = (double)W / iw, ky = (double)H / ih;
            bool tile = wl.style == 0 && wl.tile;
            switch (tile ? -1 : wl.style) {
            case 2: sw = W; sh = H; break;                                                             // esticar
            case 6: { double k = std::min(kx, ky); sw = (int)std::lround(iw * k); sh = (int)std::lround(ih * k); break; }  // ajustar
            case 0: case -1: break;                                                                    // centralizar / lado a lado
            default: { double k = std::max(kx, ky); sw = (int)std::lround(iw * k); sh = (int)std::lround(ih * k); break; }  // preencher, estender
            }
            IWICBitmapSource* src = cv;
            if ((sw != (int)iw || sh != (int)ih) && SUCCEEDED(wic->CreateBitmapScaler(&sc)) &&
                SUCCEEDED(sc->Initialize(cv, sw, sh, (UINT)sw * sh < iw * ih ? WICBitmapInterpolationModeFant : WICBitmapInterpolationModeHighQualityCubic))) src = sc;
            else { sw = iw; sh = ih; }
            auto blit = [&](int ox, int oy) {
                int x0 = std::max(0, ox), y0 = std::max(0, oy), x1 = std::min(W, ox + sw), y1 = std::min(H, oy + sh);
                if (x1 <= x0 || y1 <= y0) return;
                WICRect rc{ x0 - ox, y0 - oy, x1 - x0, y1 - y0 };
                BYTE* dst = (BYTE*)bits + ((size_t)y0 * W + x0) * 4;
                UINT size = (UINT)(((size_t)(y1 - y0 - 1) * W + (x1 - x0)) * 4);
                src->CopyPixels(&rc, W * 4, size, dst);
            };
            if (tile) { for (int y = 0; y < H; y += sh) for (int x = 0; x < W; x += sw) blit(x, y); }
            else blit((W - sw) / 2, (H - sh) / 2);
        }
    }
    if (sc) sc->Release(); if (cv) cv->Release(); if (fr) fr->Release(); if (dec) dec->Release();
    wic->Release();
    return hb;
}

void LoadWallpapers() {
    Wall wl = ReadWall();
    for (auto& m : g_mons) {
        if (!m.wallDC) continue;
        int W = m.r.right - m.r.left, H = m.r.bottom - m.r.top;
        HBITMAP nb = BuildWall(W, H, wl, m.wallDC);
        if (!nb) continue;
        SelectObject(m.wallDC, nb);
        if (m.wall) DeleteObject(m.wall);
        m.wall = nb;
        Invalidate(m);
    }
    Log(L"mesa: papel de parede %ls (estilo %d%ls)", wl.path.c_str(), wl.style, wl.tile ? L", lado a lado" : L"");
    menubar::ApplyTheme();  // a barra (transparente no Tahoe) refaz a cor do texto com o papel de parede novo
}

// ------------------------------------------------------------------ grade
RECT Work() {  // área de trabalho do monitor principal, em coordenadas da janela dele
    Mon* p = Primary();
    MONITORINFO mi{ sizeof(mi) };
    // a área que o Shell reserva, e não a do Windows: se outro programa a devolver para a tela inteira por um
    // instante, os ícones não sobem para baixo da barra de menus (issue #1)
    RECT r = shell::WorkAreaFor(MonitorFromPoint({ 0, 0 }, MONITOR_DEFAULTTOPRIMARY));
    (void)mi;
    if (p) OffsetRect(&r, -p->r.left, -p->r.top);
    return r;
}
int Cols() { RECT w = Work(); float s = Sc(); return std::max(1, (int)((w.right - w.left - 2 * MARGIN_R * s) / (CELL_W * s))); }
int Rows() { RECT w = Work(); float s = Sc(); return std::max(1, (int)((w.bottom - w.top - MARGIN_T * s) / (CELL_H * s))); }
D2D1_RECT_F CellRect(int col, int row) {
    RECT w = Work(); float s = Sc();
    float x = w.right - MARGIN_R * s - (col + 1) * CELL_W * s, y = w.top + MARGIN_T * s + row * CELL_H * s;
    return D2D1::RectF(x, y, x + CELL_W * s, y + CELL_H * s);
}
void CellAt(POINT client, int& col, int& row) {
    RECT w = Work(); float s = Sc();
    col = (int)std::floor((w.right - MARGIN_R * s - client.x) / (CELL_W * s));
    row = (int)std::floor((client.y - w.top - MARGIN_T * s) / (CELL_H * s));
    col = std::clamp(col, 0, Cols() - 1); row = std::clamp(row, 0, Rows() - 1);
}
std::vector<RECT> g_blocked;  // widgets (coordenadas da janela principal)
void ReadBlocked() {
    g_blocked.clear();
    Mon* p = Primary();
    for (RECT r : widgets::Areas()) { if (p) OffsetRect(&r, -p->r.left, -p->r.top); g_blocked.push_back(r); }
}
bool Blocked(int col, int row) {
    D2D1_RECT_F c = CellRect(col, row);
    RECT rc{ (LONG)c.left + 4, (LONG)c.top + 4, (LONG)c.right - 4, (LONG)c.bottom - 4 }, x;
    for (auto& b : g_blocked) if (IntersectRect(&x, &rc, &b)) return true;
    return false;
}

void SavePositions() {
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, g_posFile.c_str(), L"w, ccs=UTF-8") || !fp) return;
    for (auto& [k, v] : g_pos) {
        auto o = g_off.find(k);
        if (o != g_off.end() && (o->second.first != 0 || o->second.second != 0))
            fwprintf(fp, L"%d,%d,%.1f,%.1f|%ls\n", v.first, v.second, o->second.first, o->second.second, k.c_str());
        else fwprintf(fp, L"%d,%d|%ls\n", v.first, v.second, k.c_str());
    }
    fclose(fp);
}
void LoadPositions() {
    g_pos.clear(); g_off.clear();
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, g_posFile.c_str(), L"r, ccs=UTF-8") || !fp) return;
    wchar_t line[1200];
    while (fgetws(line, 1200, fp)) {
        int c = 0, r = 0; float ox = 0, oy = 0; wchar_t* bar = wcschr(line, L'|');
        if (!bar) continue;
        int n = swscanf_s(line, L"%d,%d,%f,%f", &c, &r, &ox, &oy);
        if (n < 2) continue;
        std::wstring k = bar + 1;
        while (!k.empty() && (k.back() == L'\n' || k.back() == L'\r')) k.pop_back();
        g_pos[k] = { c, r };
        if (n == 4) g_off[k] = { ox, oy };
    }
    fclose(fp);
}

// ---- Pilhas: o tipo de cada arquivo, pela extensão (pastas e atalhos ficam soltos, como os apps no Mac)
const wchar_t* KIND_NAMES[] = { L"Capturas de Tela", L"Documentos", L"Filmes", L"Imagens", L"Músicas", L"PDFs", L"Planilhas", L"Apresentações", L"Outros" };
const int KIND_COUNT = 9;
int KindOf(const Item& it) {
    if (it.dir || it.stack) return -1;
    std::wstring x = Lower(PathFindExtensionW(it.path.c_str()));
    if (x == L".lnk" || x == L".url" || x == L".appref-ms") return -1;
    auto in = [&](std::initializer_list<const wchar_t*> l) { for (auto e : l) if (x == e) return true; return false; };
    if (in({ L".png", L".jpg", L".jpeg", L".gif", L".bmp", L".webp", L".heic", L".tif", L".tiff", L".svg", L".ico", L".avif", L".raw", L".cr2", L".nef" })) {
        std::wstring n = Lower(it.name);
        return n.rfind(L"captura de tela", 0) == 0 || n.rfind(L"screenshot", 0) == 0 || n.rfind(L"captura", 0) == 0 ? 0 : 3;
    }
    if (in({ L".doc", L".docx", L".txt", L".rtf", L".odt", L".md", L".pages", L".log" })) return 1;
    if (in({ L".mp4", L".mov", L".mkv", L".avi", L".wmv", L".webm", L".m4v", L".flv" })) return 2;
    if (in({ L".mp3", L".wav", L".flac", L".aac", L".m4a", L".ogg", L".wma", L".aif", L".aiff", L".opus" })) return 4;
    if (x == L".pdf") return 5;
    if (in({ L".xls", L".xlsx", L".csv", L".ods", L".numbers" })) return 6;
    if (in({ L".ppt", L".pptx", L".odp", L".key" })) return 7;
    return 8;
}

// monta as pilhas (itens de mentira) a partir dos arquivos; sem Pilhas, tira as que houver
void BuildStacks() {
    g_items.erase(std::remove_if(g_items.begin(), g_items.end(), [](const Item& i) { if (i.stack && i.bmp) i.bmp->Release(); return i.stack; }), g_items.end());
    if (!g_stacks) { for (auto& it : g_items) it.kind = -1; return; }
    bool has[KIND_COUNT] = {};
    for (auto& it : g_items) { it.kind = KindOf(it); if (it.kind >= 0) has[it.kind] = true; }
    std::vector<Item> st;
    for (int k = 0; k < KIND_COUNT; k++) if (has[k]) {
        Item s; s.stack = true; s.kind = k; s.name = Tr(KIND_NAMES[k]); s.key = L"pilha:" + std::to_wstring(k);
        st.push_back(std::move(s));
    }
    g_items.insert(g_items.begin(), std::make_move_iterator(st.begin()), std::make_move_iterator(st.end()));
    for (auto it = g_openStacks.begin(); it != g_openStacks.end();) it = has[*it] ? std::next(it) : g_openStacks.erase(it);
}

// Pilhas ligadas: arrumação automática (como no Mac): as pilhas em ordem, cada aberta seguida do seu conteúdo,
// depois pastas e atalhos. Nada é gravado em mesa.txt (as posições livres voltam ao desligar).
void ArrangeStacks() {
    ReadBlocked();
    int C = Cols(), R = Rows(), c = 0, r = 0;
    auto next = [&](Item& it) {
        while (c < C && Blocked(c, r)) { if (++r >= R) { r = 0; c++; } }
        if (c >= C) { it.col = C - 1; it.row = R - 1; return; }
        it.col = c; it.row = r;
        if (++r >= R) { r = 0; c++; }
    };
    for (auto& it : g_items) it.col = it.row = -1;
    for (auto& s : g_items) {
        if (!s.stack) continue;
        next(s);
        if (!g_openStacks.count(s.kind)) continue;
        for (auto& it : g_items) if (!it.stack && it.kind == s.kind) next(it);
    }
    for (auto& it : g_items) if (!it.stack && it.kind < 0) next(it);
}

void Arrange(bool forgetSaved = false) {
    if (g_stacks) { ArrangeStacks(); return; }
    ReadBlocked();
    int C = Cols(), R = Rows();
    std::set<std::pair<int, int>> taken;
    auto ok = [&](int c, int r) { return c >= 0 && r >= 0 && c < C && r < R && !taken.count({ c, r }) && !Blocked(c, r); };
    std::vector<int> rest;
    for (int i = 0; i < (int)g_items.size(); i++) {
        auto& it = g_items[i];
        if (forgetSaved) { it.col = it.row = -1; it.ox = it.oy = 0; rest.push_back(i); continue; }
        if (it.col < 0) {
            auto f = g_pos.find(it.key);
            if (f != g_pos.end()) { it.col = f->second.first; it.row = f->second.second; }
            auto o = g_off.find(it.key);
            if (o != g_off.end() && Livre()) { it.ox = o->second.first; it.oy = o->second.second; } else { it.ox = it.oy = 0; }
        }
        bool livreOk = Livre() && (it.ox != 0 || it.oy != 0) && it.col >= 0 && it.row >= 0 && it.col < C && it.row < R;
        if (livreOk) taken.insert({ it.col, it.row });  // solto à mão: fica onde está, mesmo perto de outro
        else if (ok(it.col, it.row)) taken.insert({ it.col, it.row });
        else { it.col = it.row = -1; rest.push_back(i); }
    }
    bool hint = g_dropHint.x >= 0 && GetTickCount() - g_dropHintAt < 15000;
    for (int i : rest) {
        auto& it = g_items[i];
        int bc = -1, br = -1;
        if (hint) {  // perto de onde foi solto / onde a pasta foi criada
            double best = 1e18;
            for (int c = 0; c < C; c++) for (int r = 0; r < R; r++) {
                if (!ok(c, r)) continue;
                double d = (c - g_dropHint.x) * (c - g_dropHint.x) + (r - g_dropHint.y) * (r - g_dropHint.y);
                if (d < best) { best = d; bc = c; br = r; }
            }
        } else {
            for (int c = 0; c < C && bc < 0; c++) for (int r = 0; r < R; r++) if (ok(c, r)) { bc = c; br = r; break; }
        }
        if (bc < 0) { bc = C - 1; br = R - 1; }  // mesa cheia: empilha no último lugar
        it.col = bc; it.row = br; it.ox = it.oy = 0; taken.insert({ bc, br });
    }
    bool changed = forgetSaved;
    for (auto& it : g_items) {
        auto f = g_pos.find(it.key);
        if (f == g_pos.end() || f->second != std::make_pair(it.col, it.row)) { g_pos[it.key] = { it.col, it.row }; changed = true; }
        auto o = g_off.find(it.key);
        std::pair<float, float> off{ it.ox, it.oy };
        if (!g_stacks && (o == g_off.end() ? (it.ox != 0 || it.oy != 0) : o->second != off)) { g_off[it.key] = off; changed = true; }
    }
    if (changed) SavePositions();
}

// ------------------------------------------------------------------ itens
void QueueIcons();
void Reload() {
    if (!Primary()) return;
    LerMedidas();
    std::map<std::wstring, Item> old;
    for (auto& it : g_items) old[it.key] = std::move(it);
    g_items.clear();
    for (auto& dir : { g_userDesk, g_publicDesk }) {
        if (dir.empty()) continue;
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (f == INVALID_HANDLE_VALUE) continue;
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            if (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) continue;
            Item it;
            it.path = dir + L"\\" + fd.cFileName;
            it.key = Lower(it.path);
            it.dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            it.mtime = fd.ftLastWriteTime;
            SHFILEINFOW sfi{};
            it.name = SHGetFileInfoW(it.path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_DISPLAYNAME) ? sfi.szDisplayName : fd.cFileName;
            auto o = old.find(it.key);
            if (o != old.end()) {
                it.col = o->second.col; it.row = o->second.row; it.sel = o->second.sel; it.loading = o->second.loading;
                if (CompareFileTime(&o->second.mtime, &it.mtime) == 0) { it.px = std::move(o->second.px); it.iw = o->second.iw; it.ih = o->second.ih; it.bmp = o->second.bmp; o->second.bmp = nullptr; }
                else it.loading = false;
            }
            g_items.push_back(std::move(it));
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    for (auto& [k, it] : old) if (it.bmp) it.bmp->Release();
    std::sort(g_items.begin(), g_items.end(), [](const Item& a, const Item& b) { return StrCmpLogicalW(a.name.c_str(), b.name.c_str()) < 0; });
    g_stacks = shell::Setting(L"mesa_pilhas", L"0") == L"1";
    BuildStacks();
    Arrange();
    QueueIcons();
    if (Mon* p = Primary()) Invalidate(*p);
    if (!g_renameAfter.empty()) {
        for (int i = 0; i < (int)g_items.size(); i++) if (g_items[i].key == g_renameAfter) {
            g_renameAfter.clear();
            for (auto& x : g_items) x.sel = false;
            g_items[i].sel = true;
            BeginEditAt(i);
            break;
        }
    }
}

void QueueIcons() {
    std::vector<std::wstring> todo;
    for (auto& it : g_items) if (!it.stack && it.px.empty() && !it.loading) { it.loading = true; todo.push_back(it.path); }
    if (todo.empty()) return;
    int gen = g_iconGen.load();
    int px = (int)std::lround(ICON * Sc());
    HWND ctl = g_ctl;
    std::thread([todo, gen, px, ctl] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        int n = 0;
        for (auto& p : todo) {
            if (g_iconGen.load() != gen) break;
            Loaded l{ gen, Lower(p) };
            // nativo: pasta comum com a pasta do Mac (do pacote de ícones). Sem isto vinha o ícone de pasta do Windows,
            // que na VM (sem a troca de ícones do sistema do PC) é uma folha branca. Pasta com ícone próprio (desktop.ini) fica.
            bool got = false;
            DWORD at = shell::nativo ? GetFileAttributesW(p.c_str()) : INVALID_FILE_ATTRIBUTES;
            if (at != INVALID_FILE_ATTRIBUTES && (at & FILE_ATTRIBUTE_DIRECTORY) && GetFileAttributesW((p + L"\\desktop.ini").c_str()) == INVALID_FILE_ATTRIBUTES) {
                std::wstring png = ui::PackIcon(L"folder default");
                got = !png.empty() && ui::LoadPng(png, l.w, l.h, l.px);
            }
            if (!got && !ui::FileImage(p, px, l.w, l.h, l.px)) continue;
            { std::lock_guard<std::mutex> lk(g_iconMx); g_loaded.push_back(std::move(l)); }
            if (++n % 6 == 0) PostMessageW(ctl, WM_MS_ICONS, 0, 0);
        }
        PostMessageW(ctl, WM_MS_ICONS, 0, 0);
        CoUninitialize();
    }).detach();
}

void TakeIcons() {
    std::vector<Loaded> got;
    { std::lock_guard<std::mutex> lk(g_iconMx); got.swap(g_loaded); }
    bool any = false;
    for (auto& l : got) {
        if (l.gen != g_iconGen.load()) continue;
        for (auto& it : g_items) if (it.key == l.key) {
            if (it.bmp) { it.bmp->Release(); it.bmp = nullptr; }
            it.px = std::move(l.px); it.iw = l.w; it.ih = l.h; it.loading = false; any = true;
            break;
        }
    }
    if (any) if (Mon* p = Primary()) Invalidate(*p);
}

void ForgetIcons() {  // escala mudou / associações mudaram
    g_iconGen++;
    for (auto& it : g_items) { if (it.bmp) it.bmp->Release(); it.bmp = nullptr; it.px.clear(); it.loading = false; }
}

// ------------------------------------------------------------------ desenho
D2D1_RECT_F ItemCell(const Item& it) {
    D2D1_RECT_F c = CellRect(it.col, it.row);
    if (!g_stacks && (it.ox != 0 || it.oy != 0)) { float s = Sc(); c.left += it.ox * s; c.right += it.ox * s; c.top += it.oy * s; c.bottom += it.oy * s; }
    return c;
}
D2D1_RECT_F IconBox(const Item& it) {
    D2D1_RECT_F c = ItemCell(it); float s = Sc();
    float x = (c.left + c.right) / 2 - ICON * s / 2, y = c.top + 4 * s;
    return D2D1::RectF(x, y, x + ICON * s, y + ICON * s);
}
D2D1_RECT_F LabelBox(const Item& it) {
    D2D1_RECT_F c = ItemCell(it), ib = IconBox(it); float s = Sc();
    return D2D1::RectF(c.left + 3 * s, ib.bottom + LABEL_GAP * s, c.right - 3 * s, ib.bottom + (LABEL_GAP + LABEL_H) * s);
}
int HitClient(POINT p) {
    if (!IconesOn()) return -1;  // ícones ocultos: clique na mesa vazia
    for (int i = (int)g_items.size() - 1; i >= 0; i--) {
        auto& it = g_items[i];
        if (it.col < 0) continue;
        D2D1_RECT_F a = IconBox(it), b = LabelBox(it); float s = Sc();
        if ((p.x >= a.left - 4 * s && p.x < a.right + 4 * s && p.y >= a.top - 2 * s && p.y < a.bottom + 2 * s) ||
            (p.x >= b.left + 6 * s && p.x < b.right - 6 * s && p.y >= b.top && p.y < b.bottom)) return i;
    }
    return -1;
}
int HitScreen(POINT p) { Mon* m = Primary(); if (!m) return -1; p.x -= m->r.left; p.y -= m->r.top; return HitClient(p); }

void EnsureFormat() {
    float s = Sc();
    static float pt = 0;
    if (g_tf && g_tfScale == s && pt == g_textoPt) return;
    if (g_tf) g_tf->Release();
    g_tf = nullptr; g_tfScale = s; pt = g_textoPt;
    ui::dw->CreateTextFormat(ui::family.c_str(), nullptr, DWRITE_FONT_WEIGHT_MEDIUM, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                             g_textoPt * s, L"", &g_tf);
    if (!g_tf) return;
    g_tf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    g_tf->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
    DWRITE_TRIMMING tr{ DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
    IDWriteInlineObject* ell = nullptr;
    ui::dw->CreateEllipsisTrimmingSign(g_tf, &ell);
    g_tf->SetTrimming(&tr, ell);
    if (ell) ell->Release();
}

void DrawIcons(ID2D1RenderTarget* rt) {
    EnsureFormat();
    if (!g_br) rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), &g_br);
    if (!g_tf || !g_br) return;
    if (!IconesOn()) return;  // "Ocultar Ícones da Mesa"
    float s = Sc();
    for (int i = 0; i < (int)g_items.size(); i++) {
        auto& it = g_items[i];
        if (it.col < 0) continue;
        bool hi = it.sel || i == g_dropHi;
        D2D1_RECT_F ib = IconBox(it);
        if (hi) {
            g_br->SetColor(D2D1::ColorF(0, 0, 0, 0.28f));
            auto rr = D2D1::RoundedRect(D2D1::RectF(ib.left - 4 * s, ib.top - 4 * s, ib.right + 4 * s, ib.bottom + 4 * s), 6 * s, 6 * s);
            rt->FillRoundedRectangle(rr, g_br);
            g_br->SetColor(D2D1::ColorF(1, 1, 1, 0.22f));
            rt->DrawRoundedRectangle(rr, g_br, 1);
        }
        if (it.stack) {  // pilha: os três primeiros arquivos dela, um sobre o outro, os de trás um pouco girados
            std::vector<Item*> mem;
            for (auto& x : g_items) if (!x.stack && x.kind == it.kind && mem.size() < 3) mem.push_back(&x);
            D2D1_MATRIX_3X2_F old; rt->GetTransform(&old);
            static const float rot[3] = { 0.f, -8.f, 7.f };
            float cx = (ib.left + ib.right) / 2, cy = (ib.top + ib.bottom) / 2, side = (ib.right - ib.left) * 0.86f;
            for (int k = (int)mem.size() - 1; k >= 0; k--) {
                Item& x = *mem[k];
                if (!x.bmp && !x.px.empty()) x.bmp = ui::Bitmap(rt, x.iw, x.ih, x.px);
                if (!x.bmp || !x.iw || !x.ih) continue;
                float kk = std::min(side / x.iw, side / x.ih), w = x.iw * kk, h = x.ih * kk;
                rt->SetTransform(D2D1::Matrix3x2F::Rotation(rot[k], { cx, cy }) * old);
                rt->DrawBitmap(x.bmp, D2D1::RectF(cx - w / 2, cy - h / 2 - k * 2 * s, cx + w / 2, cy + h / 2 - k * 2 * s), 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
            }
            rt->SetTransform(old);
            if (g_openStacks.count(it.kind)) {  // aberta: um aro claro em volta, como a pilha "aberta" do Mac
                g_br->SetColor(D2D1::ColorF(1, 1, 1, 0.35f));
                rt->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(ib.left - 4 * s, ib.top - 4 * s, ib.right + 4 * s, ib.bottom + 4 * s), 8 * s, 8 * s), g_br, 1.5f * s);
            }
        }
        if (!it.stack && !it.bmp && !it.px.empty()) it.bmp = ui::Bitmap(rt, it.iw, it.ih, it.px);
        if (!it.stack && it.bmp && it.iw && it.ih) {
            float k = std::min((ib.right - ib.left) / it.iw, (ib.bottom - ib.top) / it.ih);
            float w = it.iw * k, h = it.ih * k;
            float x = (ib.left + ib.right - w) / 2, y = ib.bottom - h - (ib.bottom - ib.top - h) / 2;
            rt->DrawBitmap(it.bmp, D2D1::RectF(x, y, x + w, y + h), 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
        if (i == g_editIdx && g_edit) continue;
        D2D1_RECT_F lb = LabelBox(it);
        IDWriteTextLayout* lay = nullptr;
        ui::dw->CreateTextLayout(it.name.c_str(), (UINT32)it.name.size(), g_tf, lb.right - lb.left, lb.bottom - lb.top, &lay);
        if (!lay) continue;
        if (it.sel) {
            DWRITE_TEXT_METRICS tm{}; lay->GetMetrics(&tm);
            float h = std::min(tm.height, lb.bottom - lb.top);
            g_br->SetColor(D2D1::ColorF(0.04f, 0.38f, 0.88f, 1));
            rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(lb.left + tm.left - 4 * s, lb.top - 1 * s, lb.left + tm.left + tm.width + 4 * s, lb.top + h + 1 * s), 4 * s, 4 * s), g_br);
        } else {  // sombra do nome, como no Mac
            g_br->SetColor(D2D1::ColorF(0, 0, 0, 0.30f));
            rt->DrawTextLayout(D2D1::Point2F(lb.left, lb.top + 1.5f * s), lay, g_br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
            g_br->SetColor(D2D1::ColorF(0, 0, 0, 0.55f));
            rt->DrawTextLayout(D2D1::Point2F(lb.left, lb.top + 1 * s), lay, g_br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        g_br->SetColor(D2D1::ColorF(1, 1, 1, 1));
        rt->DrawTextLayout(D2D1::Point2F(lb.left, lb.top), lay, g_br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
        lay->Release();
    }
    if (g_band) {
        D2D1_RECT_F r = D2D1::RectF((float)std::min(g_bandA.x, g_bandB.x), (float)std::min(g_bandA.y, g_bandB.y),
                                    (float)std::max(g_bandA.x, g_bandB.x), (float)std::max(g_bandA.y, g_bandB.y));
        g_br->SetColor(D2D1::ColorF(1, 1, 1, 0.16f)); rt->FillRectangle(r, g_br);
        g_br->SetColor(D2D1::ColorF(1, 1, 1, 0.55f)); rt->DrawRectangle(r, g_br, 1);
    }
}

// Papel de parede animado (Lively Wallpaper e parecidos): o programa põe a janela dele dentro da mesa do Windows
// (Progman/WorkerW), atrás dos ícones, e a nossa mesa, opaca e por cima, escondia tudo (17/11/2026). Com uma janela
// de outro programa ali dentro, a mesa vira transparente (janela em camadas, alfa por pixel): só os ícones, sobre
// um fundo de alfa 1/255 (invisível, mas ainda recebe os cliques da mesa: seleção, botão direito, arrastar).
bool g_live;
bool LiveWallpaperNow() {
    HWND pm = FindWindowW(L"Progman", nullptr);
    DWORD exPid = 0; if (pm) GetWindowThreadProcessId(pm, &exPid);
    struct S { DWORD exPid; bool found; } st{ exPid, false };
    auto look = [](HWND parent, S* st) {
        EnumChildWindows(parent, [](HWND c, LPARAM l) -> BOOL {
            auto* st = (S*)l;
            DWORD pid = 0; GetWindowThreadProcessId(c, &pid);
            if (pid && pid != st->exPid && pid != GetCurrentProcessId() && IsWindowVisible(c)) { st->found = true; return FALSE; }
            return TRUE;
        }, (LPARAM)st);
    };
    if (pm) look(pm, &st);
    // Windows antigo: a WorkerW de trás (visível, do Explorador) é a mãe da janela do papel de parede
    for (HWND w = nullptr; !st.found && (w = FindWindowExW(nullptr, w, L"WorkerW", nullptr));) {
        DWORD pid = 0; GetWindowThreadProcessId(w, &pid);
        if (pid == exPid && IsWindowVisible(w)) look(w, &st);
    }
    return st.found;
}
void PaintLayered(Mon& m) {
    int W = m.r.right - m.r.left, H = m.r.bottom - m.r.top;
    if (!m.bufBits || !m.bufDC) return;
    std::fill((UINT32*)m.bufBits, (UINT32*)m.bufBits + (size_t)W * H, 0x01000000u);
    if (m.primary && m.rt) {
        RECT full{ 0, 0, W, H };
        if (SUCCEEDED(m.rt->BindDC(m.bufDC, &full))) {
            m.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);  // ClearType não tem alfa
            m.rt->BeginDraw();
            DrawIcons(m.rt);
            m.rt->EndDraw();
        }
    }
    POINT pos{ m.r.left, m.r.top }, zero{ 0, 0 };
    SIZE size{ W, H };
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    UpdateLayeredWindow(m.h, nullptr, &pos, &size, m.bufDC, &zero, 0, &bf, ULW_ALPHA);
}
// no transparente cada desenho é a tela inteira (CPU): no máximo ~60 por segundo (o retângulo de seleção pedia um
// por movimento do mouse); o que chega antes espera um temporizador curto
void Invalidate(Mon& m) {
    if (!g_live) { InvalidateRect(m.h, nullptr, FALSE); return; }
    static DWORD last;
    DWORD now = GetTickCount();
    if (now - last < 15) { SetTimer(m.h, 77, 16, nullptr); return; }
    last = now;
    PaintLayered(m);
}
void SetLive(bool on) {
    if (on == g_live) return;
    g_live = on;
    for (auto& m : g_mons) {
        LONG ex = GetWindowLongW(m.h, GWL_EXSTYLE);
        SetWindowLongW(m.h, GWL_EXSTYLE, on ? (ex | WS_EX_LAYERED) : (ex & ~WS_EX_LAYERED));
        if (on) PaintLayered(m);
        else { if (m.rt) m.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_DEFAULT); RedrawWindow(m.h, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN); }
    }
    Log(L"mesa: papel de parede animado %ls", on ? L"detectado: a mesa fica transparente (só os ícones)" : L"saiu: a mesa volta a desenhar o papel de parede");
}

void Paint(Mon& m) {
    if (g_live) { ValidateRect(m.h, nullptr); PaintLayered(m); return; }
    PAINTSTRUCT ps; HDC dc = BeginPaint(m.h, &ps);
    int W = m.r.right - m.r.left, H = m.r.bottom - m.r.top;
    if (m.bufDC && m.wallDC) {
        RECT u = ps.rcPaint;
        BitBlt(m.bufDC, u.left, u.top, u.right - u.left, u.bottom - u.top, m.wallDC, u.left, u.top, SRCCOPY);
        if (m.primary && m.rt) {
            RECT full{ 0, 0, W, H };
            if (SUCCEEDED(m.rt->BindDC(m.bufDC, &full))) {
                m.rt->BeginDraw();
                m.rt->PushAxisAlignedClip(D2D1::RectF((float)u.left, (float)u.top, (float)u.right, (float)u.bottom), D2D1_ANTIALIAS_MODE_ALIASED);
                DrawIcons(m.rt);
                m.rt->PopAxisAlignedClip();
                if (m.rt->EndDraw() == D2DERR_RECREATE_TARGET) {
                    for (auto& it : g_items) { if (it.bmp) it.bmp->Release(); it.bmp = nullptr; }
                    if (g_br) { g_br->Release(); g_br = nullptr; }
                    m.rt->Release(); m.rt = nullptr;
                    D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
                    ui::d2d->CreateDCRenderTarget(&p, &m.rt);
                    InvalidateRect(m.h, nullptr, FALSE);
                }
            }
        }
        BitBlt(dc, u.left, u.top, u.right - u.left, u.bottom - u.top, m.bufDC, u.left, u.top, SRCCOPY);
    } else {
        HBRUSH b = CreateSolidBrush(0); FillRect(dc, &ps.rcPaint, b); DeleteObject(b);
    }
    EndPaint(m.h, &ps);
}
void Redraw() { if (Mon* p = Primary()) Invalidate(*p); }

// ------------------------------------------------------------------ ações
// só arquivos de verdade e à vista (pilhas e o conteúdo de pilha fechada nunca entram em copiar/apagar/abrir)
std::vector<int> Selected() { std::vector<int> r; for (int i = 0; i < (int)g_items.size(); i++) if (g_items[i].sel && !g_items[i].stack && g_items[i].col >= 0) r.push_back(i); return r; }

void ToggleStack(int i) {
    int k = g_items[i].kind;
    if (!g_openStacks.erase(k)) g_openStacks.insert(k);
    for (auto& it : g_items) it.sel = false;
    Arrange();
}
void SetStacks(bool on) {
    shell::SetSetting(L"mesa_pilhas", on ? L"1" : L"0");
    g_openStacks.clear();
    for (auto& it : g_items) it.col = it.row = -1;  // desligando: cada um volta para a posição gravada
    Reload();
}
// itens selecionados da mesma pasta que `anchor` (o menu e o arrastar trabalham numa pasta só)
std::vector<std::wstring> SelPaths(int anchor, std::wstring* folder = nullptr) {
    std::vector<std::wstring> r;
    if (anchor < 0) { auto s = Selected(); if (s.empty()) return r; anchor = s[0]; }
    std::wstring f = Lower(Parent(g_items[anchor].path));
    r.push_back(g_items[anchor].path);
    for (int i : Selected()) if (i != anchor && Lower(Parent(g_items[i].path)) == f) r.push_back(g_items[i].path);
    if (folder) *folder = Parent(g_items[anchor].path);
    return r;
}

void OpenItem(const Item& it) {
    std::wstring target = it.path;
    bool dir = it.dir;
    if (!dir && Lower(PathFindExtensionW(target.c_str())) == L".lnk") {
        IShellLinkW* sl = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) {
            IPersistFile* pf = nullptr;
            if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) {
                wchar_t t[MAX_PATH] = {};
                if (SUCCEEDED(pf->Load(it.path.c_str(), STGM_READ)) && SUCCEEDED(sl->GetPath(t, MAX_PATH, nullptr, 0)) && t[0] && PathIsDirectoryW(t)) { target = t; dir = true; }
                pf->Release();
            }
            sl->Release();
        }
    }
    std::wstring finder = shell::dir + L"\\" EXE_FINDER;
    if (dir && GetFileAttributesW(finder.c_str()) != INVALID_FILE_ATTRIBUTES) {
        ShellExecuteW(nullptr, L"open", finder.c_str(), (L"\"" + target + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        return;
    }
    if (shell::OpenCalculator(it.path)) return;
    if (AbrirNoAppMac(it.path)) return;  // áudio: no Música
    std::wstring wd = Parent(it.path);
    SHELLEXECUTEINFOW sei{ sizeof(sei) };
    sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_LOG_USAGE;
    sei.lpFile = it.path.c_str(); sei.lpDirectory = wd.c_str(); sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) Log(L"mesa: não abriu %ls (%lu)", it.path.c_str(), GetLastError());
}
void OpenSelection() { int n = 0; for (int i : Selected()) { if (n++ >= 20) break; OpenItem(g_items[i]); } }

void DeleteSelection(bool permanent) {
    auto sel = Selected();
    if (sel.empty()) return;
    IFileOperation* op = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op)))) return;
    Mon* p = Primary();
    op->SetOwnerWindow(p ? p->h : nullptr);
    op->SetOperationFlags(permanent ? FOF_WANTNUKEWARNING : (FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE | FOF_NOCONFIRMATION));
    int n = 0;
    for (int i : sel) {
        IShellItem* si = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(g_items[i].path.c_str(), nullptr, IID_PPV_ARGS(&si)))) { if (SUCCEEDED(op->DeleteItem(si, nullptr))) n++; si->Release(); }
    }
    HRESULT hr = n ? op->PerformOperations() : E_FAIL;
    op->Release();
    if (SUCCEEDED(hr) && !permanent) shell::Sound(L"move_to_trash");
    Log(L"mesa: %d item(ns) %ls (0x%08X)", n, permanent ? L"apagados" : L"para a Lixeira", (unsigned)hr);
}

void NewFolder(POINT client) {
    std::wstring base = g_userDesk + L"\\" + Tr(L"pasta sem título"), p = base;
    for (int k = 2; GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES && k < 500; k++) p = base + L" " + std::to_wstring(k);
    if (!CreateDirectoryW(p.c_str(), nullptr)) { Log(L"mesa: ERRO criando pasta (%lu)", GetLastError()); return; }
    int c, r; CellAt(client, c, r);
    g_dropHint = { c, r }; g_dropHintAt = GetTickCount();
    g_renameAfter = Lower(p);
    Reload();
}

// ---- renomear no lugar (F2)
void EndEdit(bool commit) {
    if (!g_edit || g_editEnding) return;
    g_editEnding = true;
    int len = GetWindowTextLengthW(g_edit);
    std::wstring text(len, L'\0');
    GetWindowTextW(g_edit, text.data(), len + 1);
    HWND e = g_edit; g_edit = nullptr;
    int idx = g_editIdx; g_editIdx = -1;
    DestroyWindow(e);
    g_editEnding = false;
    while (!text.empty() && text.back() == L' ') text.pop_back();
    if (commit && idx >= 0 && idx < (int)g_items.size() && !text.empty() && text != g_items[idx].name) {
        IFileOperation* op = nullptr; IShellItem* si = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op))) &&
            SUCCEEDED(SHCreateItemFromParsingName(g_items[idx].path.c_str(), nullptr, IID_PPV_ARGS(&si)))) {
            Mon* p = Primary();
            op->SetOwnerWindow(p ? p->h : nullptr);
            op->SetOperationFlags(FOF_ALLOWUNDO);
            op->RenameItem(si, text.c_str(), nullptr);
            HRESULT hr = op->PerformOperations();
            BOOL aborted = FALSE; op->GetAnyOperationsAborted(&aborted);
            Log(L"mesa: renomeado para \"%ls\" (0x%08X)", text.c_str(), (unsigned)hr);
            if (SUCCEEDED(hr) && !aborted) {  // a posição acompanha o nome novo (o vigia de pasta relê)
                g_dropHint = { g_items[idx].col, g_items[idx].row }; g_dropHintAt = GetTickCount();
            }
        }
        if (si) si->Release(); if (op) op->Release();
    }
    Redraw();
}
LRESULT CALLBACK EditProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_GETDLGCODE) return DLGC_WANTALLKEYS;
    if (m == WM_KEYDOWN && w == VK_RETURN) { EndEdit(true); return 0; }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) { EndEdit(false); return 0; }
    if (m == WM_CHAR && (w == L'\r' || w == 27)) return 0;
    if (m == WM_KILLFOCUS) { if (Mon* p = Primary()) PostMessageW(p->h, WM_MS_ENDEDIT, 1, 0); }
    return CallWindowProcW(g_editOld, h, m, w, l);
}
}  // namespace

void BeginEditAt(int idx) {
    Mon* p = Primary();
    if (!p || idx < 0 || idx >= (int)g_items.size() || g_items[idx].col < 0 || g_items[idx].stack) return;
    EndEdit(true);
    float s = Sc();
    D2D1_RECT_F lb = LabelBox(g_items[idx]);
    if (g_editFont) DeleteObject(g_editFont);
    g_editFont = CreateFontW(-(int)std::lround(12 * s), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    int x = (int)lb.left - (int)(4 * s), w = (int)(lb.right - lb.left + 8 * s), hgt = (int)(20 * s);
    g_edit = CreateWindowExW(0, L"EDIT", g_items[idx].name.c_str(), WS_CHILD | WS_VISIBLE | WS_BORDER | ES_CENTER | ES_AUTOHSCROLL,
                             x, (int)lb.top - (int)(2 * s), w, hgt, p->h, nullptr, g_hi, nullptr);
    if (!g_edit) return;
    g_editIdx = idx;
    SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_editFont, TRUE);
    g_editOld = (WNDPROC)SetWindowLongPtrW(g_edit, GWLP_WNDPROC, (LONG_PTR)EditProc);
    const std::wstring& n = g_items[idx].name;
    size_t dot = g_items[idx].dir ? std::wstring::npos : n.find_last_of(L'.');
    SendMessageW(g_edit, EM_SETSEL, 0, dot == std::wstring::npos || dot == 0 ? (LPARAM)n.size() : (LPARAM)dot);
    SetForegroundWindow(p->h);
    SetFocus(g_edit);
    Redraw();
}

namespace {

void Reposition(POINT client) {
    if (g_stacks) return;  // com Pilhas a mesa se arruma sozinha (como no Mac)
    if (g_dragAnchor < 0 || g_dragAnchor >= (int)g_items.size()) return;
    if (Livre()) {  // "Ordenar Por > Nenhum": o ícone fica onde foi solto (a casa é só a mais perto, para gravar)
        float s = Sc();
        D2D1_RECT_F a = IconBox(g_items[g_dragAnchor]);
        POINT grab = g_grab.x >= 0 ? g_grab : POINT{ (LONG)(ICON * s / 2), (LONG)(ICON * s / 2) };
        float dx = (client.x - grab.x) - a.left, dy = (client.y - grab.y) - a.top;
        if (std::fabs(dx) < 1 && std::fabs(dy) < 1) return;
        RECT w = Work();
        for (int i : Selected()) {
            auto& it = g_items[i];
            D2D1_RECT_F ib = IconBox(it);
            float cx = (ib.left + ib.right) / 2 + dx, cy = (ib.top + ib.bottom) / 2 + dy;
            // dentro da área de trabalho (não some atrás da barra de menus nem do Dock)
            cx = std::clamp(cx, (float)w.left + ICON * s / 2, (float)w.right - ICON * s / 2);
            cy = std::clamp(cy, (float)w.top + ICON * s / 2, (float)w.bottom - (ICON + LABEL_H) * s);
            int c, r; CellAt(POINT{ (LONG)cx, (LONG)cy }, c, r);
            it.col = c; it.row = r; it.ox = it.oy = 0;
            D2D1_RECT_F base = IconBox(it);  // o ícone nessa casa sem deslocamento
            it.ox = (cx - (base.left + base.right) / 2) / s;
            it.oy = (cy - (base.top + base.bottom) / 2) / s;
            if (it.ox == 0 && it.oy == 0) it.ox = 0.01f;  // marca "solto à mão"
            g_pos[it.key] = { c, r };
            g_off[it.key] = { it.ox, it.oy };
        }
        SavePositions();
        Redraw();
        return;
    }
    int tc, tr; CellAt(client, tc, tr);
    int dc = tc - g_items[g_dragAnchor].col, dr = tr - g_items[g_dragAnchor].row;
    if (!dc && !dr) return;
    ReadBlocked();
    int C = Cols(), R = Rows();
    std::set<std::pair<int, int>> taken;
    for (auto& it : g_items) if (!it.sel && it.col >= 0) taken.insert({ it.col, it.row });
    std::vector<int> moving = Selected();
    std::sort(moving.begin(), moving.end(), [](int a, int b) { return a == g_dragAnchor ? true : b == g_dragAnchor ? false : a < b; });
    for (int i : moving) {
        auto& it = g_items[i];
        int c = std::clamp(it.col + dc, 0, C - 1), r = std::clamp(it.row + dr, 0, R - 1);
        if (taken.count({ c, r }) || Blocked(c, r)) {  // ocupada: a livre mais perto
            double best = 1e18; int bc = c, br = r;
            for (int x = 0; x < C; x++) for (int y = 0; y < R; y++) {
                if (taken.count({ x, y }) || Blocked(x, y)) continue;
                double d = (x - c) * (x - c) + (y - r) * (y - r);
                if (d < best) { best = d; bc = x; br = y; }
            }
            c = bc; r = br;
        }
        it.col = c; it.row = r; it.ox = it.oy = 0; taken.insert({ c, r });
        g_pos[it.key] = { c, r }; g_off.erase(it.key);
    }
    SavePositions();
    Redraw();
}

// ---- soltar: reposiciona (arrasto nosso), entrega à pasta embaixo do cursor ou à pasta da Área de Trabalho
struct DeskDrop : IDropTarget {
    LONG ref = 1;
    IDataObject* obj = nullptr;
    IDropTarget* fwd = nullptr;
    int fwdIdx = -2;              // -1 pasta da mesa, >= 0 pasta da mesa embaixo do cursor, -3 reposicionar
    IDropTargetHelper* helper = nullptr;
    HWND hwnd = nullptr;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pv) override {
        if (riid == IID_IUnknown || riid == IID_IDropTarget) { *pv = this; AddRef(); return S_OK; }
        *pv = nullptr; return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&ref); }
    ULONG STDMETHODCALLTYPE Release() override { LONG r = InterlockedDecrement(&ref); if (!r) delete this; return r; }
    // true se acabou de chamar DragEnter no alvo novo
    bool Retarget(DWORD keys, POINTL pt, DWORD* eff) {
        int idx = HitScreen({ pt.x, pt.y }), want;
        if (idx >= 0 && (g_items[idx].dir || Lower(PathFindExtensionW(g_items[idx].path.c_str())) == L".exe") && !(g_dragSrc && g_items[idx].sel)) want = idx;
        else if (g_dragSrc) want = -3;
        else want = -1;
        if (want == fwdIdx) return false;
        if (fwd) { fwd->DragLeave(); fwd->Release(); fwd = nullptr; }
        fwdIdx = want;
        g_dropHi = want >= 0 ? want : -1;
        Redraw();
        if (want == -3) return false;
        std::wstring path = want >= 0 ? g_items[want].path : g_userDesk;
        ctxmenu::GetUIObject(hwnd, { path }, IID_IDropTarget, (void**)&fwd);
        if (fwd) { fwd->DragEnter(obj, keys, pt, eff); return true; }
        *eff = DROPEFFECT_NONE;
        return true;
    }
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* o, DWORD keys, POINTL pt, DWORD* eff) override {
        if (obj) obj->Release();
        obj = o; obj->AddRef();
        fwdIdx = -2;
        DWORD allowed = *eff;
        if (!Retarget(keys, pt, eff) && fwdIdx == -3) *eff = allowed & DROPEFFECT_MOVE;
        POINT p{ pt.x, pt.y };
        if (helper) helper->DragEnter(hwnd, o, &p, *eff);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD keys, POINTL pt, DWORD* eff) override {
        DWORD allowed = *eff;
        if (!Retarget(keys, pt, eff)) {
            if (fwdIdx == -3) *eff = allowed & DROPEFFECT_MOVE;
            else if (fwd) fwd->DragOver(keys, pt, eff);
            else *eff = DROPEFFECT_NONE;
        }
        POINT p{ pt.x, pt.y };
        if (helper) helper->DragOver(&p, *eff);
        return S_OK;
    }
    void Clear() {
        if (fwd) { fwd->Release(); fwd = nullptr; }
        if (obj) { obj->Release(); obj = nullptr; }
        fwdIdx = -2; g_dropHi = -1; Redraw();
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        if (fwd) fwd->DragLeave();
        if (helper) helper->DragLeave();
        Clear();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* o, DWORD keys, POINTL pt, DWORD* eff) override {
        DWORD allowed = *eff;
        bool entered = Retarget(keys, pt, eff);
        POINT p{ pt.x, pt.y };
        if (fwdIdx == -3) {
            *eff = DROPEFFECT_NONE;  // o arquivo fica onde está; só a casa muda
            if (helper) helper->Drop(o, &p, allowed & DROPEFFECT_MOVE);
            Mon* m = Primary();
            if (m) { p.x -= m->r.left; p.y -= m->r.top; Reposition(p); }
        } else if (fwd) {
            if (!entered) fwd->DragOver(keys, pt, eff);
            if (helper) helper->Drop(o, &p, *eff);
            if (fwdIdx == -1) {  // arquivos novos entram onde foram soltos
                Mon* m = Primary();
                if (m) { POINT c{ p.x - m->r.left, p.y - m->r.top }; int col, row; CellAt(c, col, row); g_dropHint = { col, row }; g_dropHintAt = GetTickCount(); }
            }
            fwd->Drop(o, keys, pt, eff);
        } else { *eff = DROPEFFECT_NONE; if (helper) helper->DragLeave(); }
        Clear();
        return S_OK;
    }
};
DeskDrop* g_drop;

void StartDrag(int idx) {
    Mon* p = Primary();
    if (!p) return;
    auto paths = SelPaths(idx);
    IDataObject* obj = nullptr;
    if (FAILED(ctxmenu::GetUIObject(p->h, paths, IID_IDataObject, (void**)&obj)) || !obj) return;
    g_dragSrc = true; g_dragAnchor = idx;
    {   // o ponto do ícone que está sob o cursor: ao soltar, esse ponto fica sob o cursor (sem pulo)
        POINT c; GetCursorPos(&c);
        D2D1_RECT_F ib = IconBox(g_items[idx]);
        g_grab = { c.x - p->r.left - (LONG)ib.left, c.y - p->r.top - (LONG)ib.top };
    }
    DWORD eff = 0;
    SHDoDragDrop(p->h, obj, nullptr, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &eff);
    obj->Release();
    g_dragSrc = false; g_dragAnchor = -1; g_grab = { -1, -1 };
    Redraw();
}

// ---- menus
void ItemMenu(HWND h, POINT screen, int idx) {
    std::wstring folder;
    auto paths = SelPaths(idx, &folder);
    if (paths.empty()) return;
    auto r = ctxmenu::Show(h, screen, folder, paths, GetKeyState(VK_SHIFT) < 0);
    if (r.rename) { for (int i = 0; i < (int)g_items.size(); i++) if (g_items[i].path == paths[0]) { BeginEditAt(i); break; } }
}

void BackgroundMenu(HWND h, POINT screen) {
    enum { ID_NEW = 1, ID_INFO, ID_PASTE, ID_WALL, ID_CLEAN, ID_BY_NAME, ID_BY_KIND, ID_BY_DATE, ID_BY_SIZE, ID_MORE, ID_DISPLAY, ID_PERSONAL, ID_STACKS,
           ID_SORT_NONE, ID_SORT_GRID, ID_WIDGETS, ID_ICONS };
    bool canPaste = IsClipboardFormatAvailable(CF_HDROP) || IsClipboardFormatAvailable(RegisterClipboardFormatW(CFSTR_SHELLIDLIST));
    std::vector<ctxmenu::Item> items;
    auto add = [&](const wchar_t* t, int id, bool dis = false) { ctxmenu::Item i; i.label = t; i.id = id; i.disabled = dis; items.push_back(i); return &items.back(); };
    auto sep = [&] { ctxmenu::Item i; i.separator = true; items.push_back(i); };
    add(Tr(L"Nova Pasta"), ID_NEW)->shortcut = L"Ctrl+Shift+N";
    sep();
    add(Tr(L"Obter Informações"), ID_INFO);
    add(Tr(L"Colar"), ID_PASTE, !canPaste)->shortcut = L"Ctrl+V";
    sep();
    add(Tr(L"Alterar Fundo da Mesa…"), ID_WALL);
    add(Tr(L"Editar Widgets…"), ID_WIDGETS);  // como no macOS: traz de volta os widgets removidos
    sep();
    add(IconesOn() ? Tr(L"Ocultar Ícones da Mesa") : Tr(L"Mostrar Ícones na Mesa"), ID_ICONS);
    add(Tr(L"Usar Pilhas"), ID_STACKS)->checked = g_stacks;
    if (!g_stacks) {  // como no Mac: Ordenar Por > Nenhum (solto) / Alinhar à Grade
        ctxmenu::Item sb; sb.label = Tr(L"Ordenar Por");
        ctxmenu::Item n1; n1.label = Tr(L"Nenhum"); n1.id = ID_SORT_NONE; n1.checked = Livre(); sb.sub.push_back(n1);
        ctxmenu::Item n2; n2.label = Tr(L"Alinhar à Grade"); n2.id = ID_SORT_GRID; n2.checked = !Livre(); sb.sub.push_back(n2);
        items.push_back(sb);
    }
    if (!g_stacks) add(TrPick(L"Limpar", L"Clean Up"), ID_CLEAN);
    if (!g_stacks) {
        ctxmenu::Item by; by.label = Tr(L"Limpar Por");
        for (auto [t, id] : { std::pair{ Tr(L"Nome"), (int)ID_BY_NAME }, { Tr(L"Tipo"), (int)ID_BY_KIND }, { Tr(L"Data de Modificação"), (int)ID_BY_DATE }, { Tr(L"Tamanho"), (int)ID_BY_SIZE } }) {
            ctxmenu::Item s; s.label = t; s.id = id; by.sub.push_back(s);
        }
        items.push_back(by);
    }
    sep();
    // do menu da mesa do Explorador (sem ele, "Mais Opções" não traz estes dois)
    add(Tr(L"Configurações de Tela…"), ID_DISPLAY);
    add(Tr(L"Personalizar…"), ID_PERSONAL);
    sep();
    add(Tr(L"Mais Opções do Windows…"), ID_MORE);
    int id = ctxmenu::ShowItems(h, screen, items);
    Mon* p = Primary();
    POINT client{ screen.x - (p ? p->r.left : 0), screen.y - (p ? p->r.top : 0) };
    switch (id) {
    case ID_NEW: NewFolder(client); break;
    case ID_INFO: ctxmenu::InvokeVerb(h, Parent(g_userDesk), { g_userDesk }, "properties"); break;
    case ID_PASTE: {
        int c, r; CellAt(client, c, r); g_dropHint = { c, r }; g_dropHintAt = GetTickCount();
        ctxmenu::InvokeVerb(h, g_userDesk, {}, "paste");
        break;
    }
    case ID_WALL: shell::OpenSettings(L"fundo"); break;
    case ID_WIDGETS: widgets::EditMenu(screen); break;
    case ID_ICONS: shell::SetSetting(L"mesa_icones", IconesOn() ? L"0" : L"1"); for (auto& it : g_items) it.sel = false; Redraw(); break;
    case ID_STACKS: SetStacks(!g_stacks); break;
    case ID_SORT_NONE: shell::SetSetting(L"mesa_grade", L"0"); Redraw(); break;
    case ID_SORT_GRID:  // encaixa todos nas casas (a mais perto de onde cada um está)
        shell::SetSetting(L"mesa_grade", L"1");
        g_off.clear();
        for (auto& it : g_items) it.ox = it.oy = 0;
        Arrange(); SavePositions(); Redraw();
        break;
    case ID_CLEAN: Arrange(true); Redraw(); break;
    case ID_BY_NAME: case ID_BY_KIND: case ID_BY_DATE: case ID_BY_SIZE: {
        auto kind = [](const Item& i) { return i.dir ? std::wstring(L"") : Lower(PathFindExtensionW(i.path.c_str())); };
        auto size = [](const Item& i) -> ULONGLONG {
            WIN32_FILE_ATTRIBUTE_DATA d{}; if (!GetFileAttributesExW(i.path.c_str(), GetFileExInfoStandard, &d)) return 0;
            return (ULONGLONG)d.nFileSizeHigh << 32 | d.nFileSizeLow;
        };
        std::stable_sort(g_items.begin(), g_items.end(), [&](const Item& a, const Item& b) {
            if (id == ID_BY_KIND) { auto ka = kind(a), kb = kind(b); if (ka != kb) return ka < kb; }
            if (id == ID_BY_DATE) { int c = CompareFileTime(&a.mtime, &b.mtime); if (c) return c > 0; }
            if (id == ID_BY_SIZE) { auto sa = size(a), sb = size(b); if (sa != sb) return sa > sb; }
            return StrCmpLogicalW(a.name.c_str(), b.name.c_str()) < 0;
        });
        Arrange(true); Redraw();
        break;
    }
    case ID_MORE: ctxmenu::Show(h, screen, g_userDesk, {}, true); break;
    case ID_DISPLAY: shell::OpenSettings(L"telas"); break;  // o nosso Ajustes do Sistema, não o do Windows
    case ID_PERSONAL: shell::OpenSettings(L"fundo"); break;  // Papel de Parede
    }
}

// ------------------------------------------------------------------ janela da mesa
LRESULT CALLBACK DeskProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    LRESULT res;
    if ((m == WM_INITMENUPOPUP || m == WM_DRAWITEM || m == WM_MEASUREITEM || m == WM_MENUCHAR) && ctxmenu::ForwardOwnerMsg(m, w, l, &res)) return res;
    Mon* mon = MonOf(h);
    bool primary = mon && mon->primary;
    switch (m) {
    case WM_PAINT: if (mon) Paint(*mon); else ValidateRect(h, nullptr); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_TIMER: if (w == 77) { KillTimer(h, 77); if (mon && g_live) PaintLayered(*mon); return 0; } break;
    case WM_WINDOWPOSCHANGING: {  // sempre no fundo, como a mesa do Windows
        auto* wp = (WINDOWPOS*)l;
        if (!(wp->flags & SWP_NOZORDER)) {
            wp->hwndInsertAfter = HWND_BOTTOM;
            if (g_fundo)  // Explorador no fundo: logo acima da mesa dele (abaixo de todas as janelas)
                if (HWND ed = TopExplorerDesk()) {
                    HWND above = GetWindow(ed, GW_HWNDPREV);
                    if (above == h) wp->flags |= SWP_NOZORDER;
                    else wp->hwndInsertAfter = above ? above : HWND_TOP;
                }
        }
        return 0;
    }
    case WM_MOUSEACTIVATE: return MA_ACTIVATE;
    case WM_SETTINGCHANGE:
        if (w == SPI_SETDESKWALLPAPER && primary) LoadWallpapers();
        if (w == SPI_SETWORKAREA && primary) { Arrange(); Redraw(); }
        return 0;
    case WM_DISPLAYCHANGE: case WM_DPICHANGED:
        PostMessageW(g_ctl, WM_MS_REBUILD, 0, m == WM_DPICHANGED);  // DPI: refaz mesmo com os mesmos retângulos
        return 0;
    case WM_MS_ENDEDIT: if (g_edit && GetFocus() != g_edit) EndEdit(w != 0); return 0;
    case WM_LBUTTONDOWN: {
        POINT p{ (short)LOWORD(l), (short)HIWORD(l) };
        if (g_edit) EndEdit(true);
        SetFocus(h);
        if (!primary) return 0;
        bool ctrl = (w & MK_CONTROL) != 0;
        int i = HitClient(p);
        if (i >= 0 && g_items[i].stack) { ToggleStack(i); Redraw(); return 0; }  // pilha: um clique abre/fecha
        if (i >= 0) {
            if (ctrl) g_items[i].sel = !g_items[i].sel;
            else if (!g_items[i].sel) { for (auto& it : g_items) it.sel = false; g_items[i].sel = true; }
            g_press = i; g_pressPt = p; g_pressCtrl = ctrl;
        } else {
            g_bandOrig.clear();
            for (auto& it : g_items) { if (!ctrl) it.sel = false; g_bandOrig.push_back(it.sel); }
            g_band = true; g_bandA = g_bandB = p;
        }
        SetCapture(h);
        Redraw();
        return 0;
    }
    case WM_MOUSEMOVE: {
        POINT p{ (short)LOWORD(l), (short)HIWORD(l) };
        if (g_press >= 0 && (w & MK_LBUTTON) &&
            (std::abs(p.x - g_pressPt.x) > GetSystemMetrics(SM_CXDRAG) || std::abs(p.y - g_pressPt.y) > GetSystemMetrics(SM_CYDRAG))) {
            int i = g_press; g_press = -1;
            ReleaseCapture();
            if (!g_items[i].sel) g_items[i].sel = true;
            StartDrag(i);
        } else if (g_band) {
            g_bandB = p;
            RECT b{ std::min(g_bandA.x, p.x), std::min(g_bandA.y, p.y), std::max(g_bandA.x, p.x), std::max(g_bandA.y, p.y) };
            for (int i = 0; i < (int)g_items.size(); i++) {
                auto& it = g_items[i];
                if (it.col < 0) continue;
                D2D1_RECT_F a = IconBox(it), lb = LabelBox(it);
                RECT r{ (LONG)a.left, (LONG)a.top, (LONG)a.right, (LONG)lb.bottom }, x;
                bool in = IntersectRect(&x, &r, &b) != FALSE;
                bool orig = i < (int)g_bandOrig.size() && g_bandOrig[i];
                it.sel = orig != in;
            }
            Redraw();
        }
        return 0;
    }
    case WM_LBUTTONUP: {
        if (GetCapture() == h) ReleaseCapture();
        // papel de parede animado: clique num lugar vazio (sem arrastar seleção) devolve o primeiro plano à mesa do
        // Windows. Com a nossa mesa (do tamanho da tela) na frente, o Lively achava que um app em tela cheia estava
        // aberto: pausava o papel de parede e parava de repassar o mouse para ele (17/11/2026)
        if (g_live && g_band && std::abs(g_bandB.x - g_bandA.x) < 4 && std::abs(g_bandB.y - g_bandA.y) < 4)
            if (HWND pm = FindWindowW(L"Progman", nullptr)) SetForegroundWindow(pm);
        if (g_press >= 0 && !g_pressCtrl) { for (int i = 0; i < (int)g_items.size(); i++) g_items[i].sel = i == g_press; }
        g_press = -1;
        if (g_band) { g_band = false; Redraw(); }
        Redraw();
        return 0;
    }
    case WM_CAPTURECHANGED: if (g_band) { g_band = false; Redraw(); } return 0;
    case WM_LBUTTONDBLCLK: {
        if (!primary) return 0;
        POINT p{ (short)LOWORD(l), (short)HIWORD(l) };
        int i = HitClient(p);
        if (i >= 0 && g_items[i].stack) return 0;  // o segundo clique na pilha (o primeiro já abriu/fechou)
        if (i >= 0) { if (!g_items[i].sel) { for (auto& it : g_items) it.sel = false; g_items[i].sel = true; } OpenSelection(); }
        return 0;
    }
    case WM_RBUTTONDOWN: {
        if (g_edit) EndEdit(true);
        SetFocus(h);
        if (!primary) return 0;
        POINT p{ (short)LOWORD(l), (short)HIWORD(l) };
        int i = HitClient(p);
        if (i >= 0 && !g_items[i].sel) { for (auto& it : g_items) it.sel = false; g_items[i].sel = true; }
        if (i < 0) for (auto& it : g_items) it.sel = false;
        Redraw();
        return 0;
    }
    case WM_CONTEXTMENU: {
        POINT s{ (short)LOWORD(l), (short)HIWORD(l) };
        auto sel = Selected();
        if (s.x == -1 && s.y == -1) {  // Shift+F10 / tecla de menu
            if (primary && !sel.empty()) { D2D1_RECT_F b = IconBox(g_items[sel[0]]); s = { (LONG)b.right + mon->r.left, (LONG)b.top + mon->r.top }; }
            else GetCursorPos(&s);
        }
        int i = primary ? HitScreen(s) : -1;
        if (i < 0 && primary && !sel.empty() && (short)LOWORD(l) == -1) i = sel[0];
        if (i >= 0 && g_items[i].stack) i = -1;  // pilha: o menu da mesa (com "Usar Pilhas")
        if (i >= 0) ItemMenu(h, s, i); else BackgroundMenu(h, s);
        return 0;
    }
    case WM_KEYDOWN: {
        if (!primary) return 0;
        bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
        auto sel = Selected();
        switch (w) {
        case VK_RETURN: OpenSelection(); return 0;
        case VK_SPACE: if (!sel.empty()) quicklook::Toggle(g_items[sel[0]].path); return 0;  // Quick Look, como no Mac
        case VK_F2: if (!sel.empty()) BeginEditAt(sel[0]); return 0;
        case VK_DELETE: DeleteSelection(shift); return 0;
        case VK_F5: Reload(); return 0;
        case VK_ESCAPE: for (auto& it : g_items) it.sel = false; Redraw(); return 0;
        case 'A': if (ctrl) { for (auto& it : g_items) it.sel = true; Redraw(); } return 0;
        case 'C': case 'X': if (ctrl && !sel.empty()) { std::wstring f; auto ps = SelPaths(-1, &f); ctxmenu::InvokeVerb(h, f, ps, w == 'C' ? "copy" : "cut"); } return 0;
        case 'V': if (ctrl) ctxmenu::InvokeVerb(h, g_userDesk, {}, "paste"); return 0;
        case 'N': if (ctrl && shift) { POINT cl{ 0, 0 }; GetCursorPos(&cl); cl.x -= mon->r.left; cl.y -= mon->r.top; NewFolder(cl); } return 0;
        case 'I': if (ctrl && !sel.empty()) { std::wstring f; auto ps = SelPaths(-1, &f); ctxmenu::InvokeVerb(h, f, ps, "properties"); } return 0;
        }
        break;
    }
    case WM_DESTROY: if (primary) RevokeDragDrop(h); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void FreeMon(Mon& m) {
    if (m.rt) { m.rt->Release(); m.rt = nullptr; }
    if (m.wallDC) { SelectObject(m.wallDC, m.wallOld); DeleteDC(m.wallDC); m.wallDC = nullptr; }
    if (m.bufDC) { SelectObject(m.bufDC, m.bufOld); DeleteDC(m.bufDC); m.bufDC = nullptr; }
    if (m.wall) { DeleteObject(m.wall); m.wall = nullptr; }
    if (m.buf) { DeleteObject(m.buf); m.buf = nullptr; }
    if (m.h) { DestroyWindow(m.h); m.h = nullptr; }
}

void StopWatch() {
    if (g_watchStop) { SetEvent(g_watchStop); g_watchStop = nullptr; }  // a thread fecha o próprio evento
    if (g_shnId) { SHChangeNotifyDeregister(g_shnId); g_shnId = 0; }
}

void StartWatch() {
    StopWatch();
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_watchStop = stop;
    HWND ctl = g_ctl;
    std::vector<std::wstring> dirs{ g_userDesk, g_publicDesk };
    std::thread([stop, ctl, dirs] {
        std::vector<HANDLE> hs{ stop };
        for (auto& d : dirs) {
            if (d.empty()) continue;
            HANDLE c = FindFirstChangeNotificationW(d.c_str(), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_ATTRIBUTES);
            if (c != INVALID_HANDLE_VALUE) hs.push_back(c);
        }
        for (;;) {
            DWORD r = WaitForMultipleObjects((DWORD)hs.size(), hs.data(), FALSE, INFINITE);
            if (r == WAIT_OBJECT_0 || r == WAIT_FAILED) break;
            PostMessageW(ctl, WM_MS_CHANGED, 0, 0);
            FindNextChangeNotification(hs[r - WAIT_OBJECT_0]);
        }
        for (size_t i = 1; i < hs.size(); i++) FindCloseChangeNotification(hs[i]);
        CloseHandle(stop);
    }).detach();
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (SUCCEEDED(SHGetKnownFolderIDList(FOLDERID_Desktop, 0, nullptr, &pidl))) {
        SHChangeNotifyEntry e{ pidl, FALSE };
        g_shnId = SHChangeNotifyRegister(g_ctl, SHCNRF_ShellLevel | SHCNRF_InterruptLevel | SHCNRF_NewDelivery,
                                         SHCNE_ALLEVENTS, WM_MS_SHN, 1, &e);
        CoTaskMemFree(pidl);
    }
}

bool DeskExists() { return !g_mons.empty(); }

void DestroyDesk() {
    if (!DeskExists()) return;
    if (g_edit) EndEdit(false);
    StopWatch();
    g_iconGen++;
    for (auto& it : g_items) if (it.bmp) { it.bmp->Release(); it.bmp = nullptr; }
    g_items.clear();
    if (g_br) { g_br->Release(); g_br = nullptr; }
    for (auto& m : g_mons) FreeMon(m);
    g_mons.clear();
    if (g_drop) { g_drop->Release(); g_drop = nullptr; }
    Log(L"mesa: janelas da mesa fechadas");
    g_live = false;  // as janelas novas nascem opacas; o Tick confere de novo
}

// os monitores de agora são outros que os da mesa (número ou retângulos)?
bool g_forceRebuild = false, g_rebuildLater = false;
bool MonitorsChanged() {
    std::vector<RECT> now;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hm, HDC, LPRECT, LPARAM l) -> BOOL {
        MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(hm, &mi);
        ((std::vector<RECT>*)l)->push_back(mi.rcMonitor);
        return TRUE;
    }, (LPARAM)&now);
    if (now.size() != g_mons.size()) return true;
    for (auto& r : now) {
        bool found = false;
        for (auto& m : g_mons) if (EqualRect(&r, &m.r)) { found = true; break; }
        if (!found) return true;
    }
    return false;
}

void CreateDesk() {
    if (DeskExists()) return;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR hm, HDC, LPRECT, LPARAM) -> BOOL {
        MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(hm, &mi);
        Mon m; m.r = mi.rcMonitor; m.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
        g_mons.push_back(m);
        return TRUE;
    }, 0);
    HDC screen = GetDC(nullptr);
    for (auto& m : g_mons) {
        int W = m.r.right - m.r.left, H = m.r.bottom - m.r.top;
        m.h = CreateWindowExW(WS_EX_TOOLWINDOW, DESK_CLASS, L"Mesa", WS_POPUP | WS_CLIPCHILDREN, m.r.left, m.r.top, W, H, nullptr, nullptr, g_hi, nullptr);
        m.wallDC = CreateCompatibleDC(screen);
        m.bufDC = CreateCompatibleDC(screen);
        BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), W, -H, 1, 32, BI_RGB };
        void* bits = nullptr;
        m.buf = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        m.bufBits = bits;
        m.wall = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        m.bufOld = (HBITMAP)SelectObject(m.bufDC, m.buf);
        m.wallOld = (HBITMAP)SelectObject(m.wallDC, m.wall);
        if (m.primary) {
            D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
            ui::d2d->CreateDCRenderTarget(&p, &m.rt);
        }
    }
    ReleaseDC(nullptr, screen);
    LoadWallpapers();
    for (auto& m : g_mons) {
        SetWindowPos(m.h, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        UpdateWindow(m.h);
    }
    if (Mon* p = Primary()) {
        g_drop = new DeskDrop();
        g_drop->hwnd = p->h;
        CoCreateInstance(CLSID_DragDropHelper, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_drop->helper));
        RegisterDragDrop(p->h, g_drop);
    }
    LoadPositions();
    Reload();
    StartWatch();
    Log(L"mesa: janelas da mesa criadas (%zu monitor(es), %zu ícones)", g_mons.size(), g_items.size());
}

// ------------------------------------------------------------------ modo sem Explorador
void BecomeShell() {
    if (g_standalone) return;
    Mon* p = Primary();
    if (!p) return;
    BOOL sw = CallUser32("SetShellWindow", p->h);
    BOOL tm = CallUser32("SetTaskmanWindow", g_msgWnd);
    g_origMin.cbSize = sizeof(g_origMin);
    if (SystemParametersInfoW(SPI_GETMINIMIZEDMETRICS, sizeof(g_origMin), &g_origMin, 0) && !(g_origMin.iArrange & ARW_HIDE)) {
        MINIMIZEDMETRICS mm = g_origMin; mm.iArrange |= ARW_HIDE;  // minimizadas não viram barrinhas no canto da tela
        g_minChanged = SystemParametersInfoW(SPI_SETMINIMIZEDMETRICS, sizeof(mm), &mm, 0) != FALSE;  // só nesta sessão (sem gravar)
    }
    SendNotifyMessageW(HWND_BROADCAST, RegisterWindowMessageW(L"TaskbarCreated"), 0, 0);  // ícones da bandeja voltam para a nossa
    g_standalone = true;
    for (auto& m : g_mons) SetWindowPos(m.h, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    shell::ApplyWorkArea();
    Log(L"mesa: Shell sem o Explorador (SetShellWindow %d, SetTaskmanWindow %d, minimizadas escondidas %d)", sw, tm, g_minChanged);
}

void LeaveShell() {
    if (!g_standalone) return;
    CallUser32("SetTaskmanWindow", nullptr);
    if (g_minChanged) { SystemParametersInfoW(SPI_SETMINIMIZEDMETRICS, sizeof(g_origMin), &g_origMin, 0); g_minChanged = false; }
    g_standalone = false;
}

// Alguma janela de app da Loja antigo aberta (a moldura do ApplicationFrameHost ou a janela do app)
bool AnyUwpWindow() {
    bool any = false;
    EnumWindows([](HWND h, LPARAM l) -> BOOL {
        if (!IsWindowVisible(h)) return TRUE;
        wchar_t c[64] = {}; GetClassNameW(h, c, 64);
        if (wcscmp(c, L"ApplicationFrameWindow") && wcscmp(c, L"Windows.UI.Core.CoreWindow")) return TRUE;
        DWORD cloak = 0; DwmGetWindowAttribute(h, 14 /*DWMWA_CLOAKED*/, &cloak, sizeof(cloak));
        if (cloak) return TRUE;
        *(bool*)l = true;
        return FALSE;
    }, (LPARAM)&any);
    return any;
}

// máquina de estados, a cada 500 ms
void Tick() {
    {   // papel de parede animado entrou ou saiu (a cada ~2 s)
        static int n;
        if (++n % 4 == 0 && !g_mons.empty()) SetLive(LiveWallpaperNow());
    }
    // explorer=0 / explorer=1 / explorer=fundo mandam. Sem o ajuste e com o Shell como shell do Windows: fundo.
    // Sem nenhum Explorador na sessão o Windows não liga o desfoque (transparência): o vidro do Dock, da barra e
    // dos painéis saía preto (13/11/2026, PC ligado direto no Shell). Com o Explorador escondido no fundo, volta.
    std::wstring opt = shell::Setting(L"explorer", L"");
    bool registered = WinlogonIsOurs();
    bool want0 = opt == L"0";
    g_fundo = opt == L"fundo" || (opt.empty() && registered);
    // nativo: o Shell roda dentro do explorer.exe; a mesa do projeto fica por cima da dele (os ícones e o papel
    // de parede do Windows ficam atrás, sem uso) e nunca se pede para o Explorador sair
    if (shell::nativo) { g_fundo = true; g_uwpWant = false; }
    if (g_fundo) want0 = false;
    HWND ex = ExplorerProgman();
    DWORD now = GetTickCount();
    {   // Papel de parede trocado sem aviso: com o Explorador aberto, o Ajustes troca por ele (IDesktopWallpaper) e
        // ninguém manda o WM_SETTINGCHANGE para a nossa mesa (ela continuava com o antigo). Confere o registro.
        static std::wstring lastWall;
        static DWORD lastCheck;
        if (DeskExists() && now - lastCheck > 1000) {
            lastCheck = now;
            Wall wl = ReadWall();
            std::wstring key = wl.path + L"|" + std::to_wstring(wl.style) + L"|" + std::to_wstring(wl.tile);
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            if (!wl.path.empty() && GetFileAttributesExW(wl.path.c_str(), GetFileExInfoStandard, &fa)) key += L"|" + std::to_wstring(fa.ftLastWriteTime.dwLowDateTime);
            if (lastWall.empty()) lastWall = key;
            else if (key != lastWall) { lastWall = key; Log(L"mesa: papel de parede mudou; recarregando"); LoadWallpapers(); }
        }
    }
    if (g_uwpWant) {  // app da Loja antigo aberto: o Explorador fica (escondido; a barra dele o Shell esconde)
        want0 = false;
        if (ex && ProcessAgeSec(PidOf(ex)) > 3) {
            std::wstring p;
            { std::lock_guard<std::mutex> lk(g_uwpMx); p.swap(g_uwpPending); }
            if (!p.empty()) { Log(L"mesa: Explorador pronto; abrindo de novo %ls", p.c_str()); apps::ActivatePackaged(L"shell:AppsFolder\\" + p); }
            if (ProcessAgeSec(PidOf(ex)) > 20) {
                if (AnyUwpWindow()) g_uwpIdleSince = 0;
                else if (!g_uwpIdleSince) g_uwpIdleSince = now;
                else if (now - g_uwpIdleSince > 15000) {
                    g_uwpWant = false; g_uwpIdleSince = 0; g_closeTries = 0; g_closeAskedAt = 0;
                    Log(L"mesa: nenhum app da Loja aberto há 15 s; voltando ao modo sem Explorador");
                }
            }
        }
    }
    static int lastState = -1;
    int state = (want0 ? 1 : 0) | (ex ? 2 : 0) | (DeskExists() ? 4 : 0) | (g_standalone ? 8 : 0);
    if (state != lastState) {
        lastState = state;
        Log(L"mesa: estado explorer=%d, Explorador %ls (pid %lu, %.0f s), mesa própria %ls, sem Explorador %ls", want0 ? 0 : 1,
            ex ? L"aberto" : L"fechado", PidOf(ex), ex ? ProcessAgeSec(PidOf(ex)) : 0.0, DeskExists() ? L"sim" : L"não", g_standalone ? L"sim" : L"não");
    }
    if (want0 && g_closeTries < 3) {
        if (ex) {
            DWORD pid = PidOf(ex);
            if (!g_closeAskedAt) {
                // logo depois do login o Explorador ainda abre os apps de inicialização: deixa ele terminar
                if (ProcessAgeSec(pid) < 30) return;
                CreateDesk();  // a nossa já está embaixo quando a dele sumir
                HWND tray = ExplorerTrayOf(pid);
                if (tray) PostMessageW(tray, EXPLORER_EXIT, 0, 0);
                g_closeAskedAt = now; g_closeTries++;
                Log(L"mesa: pedindo para o Explorador sair (pid %lu, tentativa %d%ls)", pid, g_closeTries, tray ? L"" : L", sem barra de tarefas");
            } else if (now - g_closeAskedAt > 8000) {
                HANDLE hp = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
                BOOL ok = hp && TerminateProcess(hp, 0);
                if (hp) CloseHandle(hp);
                Log(L"mesa: o Explorador não saiu em 8 s: encerrado à força (%d)", ok);
                g_closeAskedAt = 0;
            }
            return;
        }
        g_closeAskedAt = 0;
        if (!DeskExists()) CreateDesk();
        if (!g_standalone) { g_weClosed = true; BecomeShell(); }
        static DWORD lastGhostCheck;  // a mesa é nossa: Explorador pela metade não fica (a cada 5 s)
        if (g_standalone && now - lastGhostCheck > 5000) { lastGhostCheck = now; CloseGhostExplorers(10, L"a mesa é do Shell"); }
        return;
    }
    if (want0 && g_closeTries >= 3 && ex && !g_standalone) {
        static bool told; if (!told) { told = true; Log(L"mesa: o Explorador voltou 3 vezes; desisti de fechar (fica com ele)"); }
    }
    // explorer=1 (ou desistimos): o Explorador é a mesa
    if (ex) {
        if (g_standalone) LeaveShell();
        if (g_fundo) {  // Explorador no fundo: a nossa mesa por cima da dele (a barra dele o Shell esconde)
            if (!DeskExists()) { CreateDesk(); Log(L"mesa: Explorador no fundo; a mesa do Shell fica por cima da dele"); }
            HWND ed = TopExplorerDesk();
            Mon* p = Primary();
            if (ed && p && GetWindow(ed, GW_HWNDPREV) != p->h)  // a dele passou para cima (Win+D...): a nossa volta
                for (auto& m : g_mons) SetWindowPos(m.h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        } else if (DeskExists()) DestroyDesk();
        g_explorerGoneAt = 0;
        return;
    }
    if (g_standalone) {  // estava sem o Explorador e o ajuste voltou para 1: devolve a mesa a ele
        LeaveShell();
        DestroyDesk();
        StartExplorer(L"ajuste explorer=1");
        g_weClosed = false;
        return;
    }
    // sem Explorador e sem termos fechado (ele caiu, ou foi fechado à mão): a nossa mesa cobre o buraco
    if (!g_explorerGoneAt) g_explorerGoneAt = now;
    // explorer=1 com o Shell registrado como shell do Windows (no login o Explorador não abre): abre ele
    static DWORD lastStart;
    if ((registered || g_fundo) && now - g_explorerGoneAt > 2000 && (!lastStart || now - lastStart > 30000)) {
        lastStart = now;
        if (DeskExists()) DestroyDesk();
        StartExplorer(L"explorer=1 com o Shell registrado");
        return;
    }
    if (!DeskExists() && now - g_explorerGoneAt > 3000) { Log(L"mesa: Explorador ausente; mostrando a mesa do Shell"); CreateDesk(); }
}

LRESULT CALLBACK CtlProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_TIMER:
        if (w == 1) Tick();
        if (w == 2) { KillTimer(h, 2); if (DeskExists()) Reload(); }
        if (w == 7) { KillTimer(h, 7); PostMessageW(h, WM_MS_REBUILD, 1, 0); }  // a tela assentou: confere e refaz
        if (w == 3) {  // widgets mudaram de lugar?
            if (!DeskExists()) return 0;
            static std::vector<RECT> last;
            auto now = widgets::Areas();
            now.push_back(Work());  // a área de trabalho também (Dock mudou de tamanho)
            bool same = now.size() == last.size();
            for (size_t i = 0; same && i < now.size(); i++) same = EqualRect(&now[i], &last[i]) != FALSE;
            if (!same) { last = now; Arrange(); Redraw(); }
        }
        return 0;
    case WM_MS_CHANGED: SetTimer(h, 2, 250, nullptr); return 0;
    case WM_MS_SHN: {
        PIDLIST_ABSOLUTE* pidls = nullptr; LONG ev = 0;
        HANDLE lock = SHChangeNotification_Lock((HANDLE)w, (DWORD)l, &pidls, &ev);
        if (lock) SHChangeNotification_Unlock(lock);
        if (ev & (SHCNE_ASSOCCHANGED | SHCNE_UPDATEIMAGE)) { ForgetIcons(); QueueIcons(); Redraw(); }
        SetTimer(h, 2, 250, nullptr);
        return 0;
    }
    case WM_MS_ICONS: TakeIcons(); return 0;
    case WM_MS_WALLNOW: if (DeskExists()) { Log(L"mesa: papel de parede escolhido no Ajustes; mostrando já"); LoadWallpapers(); } return 0;
    case WM_MS_REBUILD:
        // Cada aviso de tela (WM_DISPLAYCHANGE/DPI) refazia a mesa na hora e decodificava o papel de parede de novo: um
        // jogo entrando e saindo da tela cheia dava isso a cada segundo (Fortnite, 20/11/2026: 101 linhas da mesa no
        // log). Agora espera a tela assentar (1,2 s), só refaz se os monitores mudaram mesmo, e não no meio de um jogo
        // (fica para quando ele fechar: mesa::DepoisDoJogo)
        if (w != 1) { if (l) g_forceRebuild = true; SetTimer(h, 7, 1200, nullptr); return 0; }
        if (jogo::Ativo()) { g_rebuildLater = true; return 0; }
        if (!g_forceRebuild && !MonitorsChanged()) { Log(L"mesa: aviso de tela sem mudança nos monitores; mesa mantida"); return 0; }
        g_forceRebuild = false;
        if (DeskExists()) {
            bool shellWas = g_standalone;
            if (shellWas) CallUser32("SetShellWindow", nullptr);
            DestroyDesk(); ForgetIcons(); CreateDesk();
            if (shellWas) if (Mon* p = Primary()) CallUser32("SetShellWindow", p->h);
        }
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

// Win+D sem o Explorador: minimiza as janelas dos apps; de novo, devolve
std::vector<HWND> g_shown;
void ShowDesktopToggle() {
    // já voltaram por outro caminho (Dock, Alt+Tab)? Então este Win+D esconde de novo, em vez de "devolver" nada
    bool anyHidden = false;
    for (HWND h : g_shown) if (IsWindow(h) && IsIconic(h)) { anyHidden = true; break; }
    if (!anyHidden) g_shown.clear();
    if (!g_shown.empty()) {
        // devolve de trás para a frente: a que estava na frente termina na frente
        // (SW_RESTORE: a maximizada volta maximizada; a de cima é a última, e fica com o foco)
        for (auto it = g_shown.rbegin(); it != g_shown.rend(); ++it) if (IsWindow(*it) && IsIconic(*it)) ShowWindowAsync(*it, SW_RESTORE);
        g_shown.clear();
        return;
    }
    EnumWindows([](HWND h, LPARAM) -> BOOL {
        if (!IsWindowVisible(h) || IsIconic(h) || apps::IsShellWindow(h)) return TRUE;
        LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
        if ((ex & WS_EX_TOOLWINDOW) && !(ex & WS_EX_APPWINDOW)) return TRUE;
        if (GetWindow(h, GW_OWNER) && !(ex & WS_EX_APPWINDOW)) return TRUE;
        if (!(GetWindowLongW(h, GWL_STYLE) & WS_MINIMIZEBOX) || GetWindowTextLengthW(h) == 0) return TRUE;
        wchar_t c[64]; GetClassNameW(h, c, 64);
        if (mesa::IsDeskClass(c) || !wcscmp(c, L"Shell_TrayWnd")) return TRUE;
        g_shown.push_back(h);
        genio::Pause(1200);  // todas de uma vez, sem o gênio (várias animações juntas travavam com Win+D repetido)
        ShowWindowAsync(h, SW_MINIMIZE);
        return TRUE;
    }, 0);
}

}  // namespace

// ================================================================== API
bool mesa::IsDeskClass(const wchar_t* cls) {
    return cls && (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"SequoiaMesa"));
}
bool mesa::IsDeskWindow(HWND h) {
    if (!h) return false;
    wchar_t c[32] = {}; GetClassNameW(GetAncestor(h, GA_ROOT), c, 32);
    return IsDeskClass(c);
}
bool mesa::Standalone() { return g_standalone.load(); }

// a faixa do papel de parede atrás da barra de menus, como a mesa a desenhou (o mesmo corte e estilo da tela):
// a barra do Tahoe é transparente e a cor do texto segue o que está atrás dela
bool mesa::TopColor(int barPx, float& r, float& g, float& b) {
    Mon* p = Primary();
    if (!p || !p->wall || !p->wallDC) return false;
    static HBITMAP lastBmp; static int lastPx; static float cr, cg, cb;  // só relê quando o papel de parede muda
    if (p->wall == lastBmp && barPx == lastPx) { r = cr; g = cg; b = cb; return true; }
    BITMAP bm{};
    if (!GetObjectW(p->wall, sizeof(bm), &bm) || bm.bmWidth <= 0 || bm.bmHeight <= 0) return false;
    int rows = std::clamp(barPx, 1, (int)bm.bmHeight);
    BITMAPINFO bi{}; bi.bmiHeader = { sizeof(BITMAPINFOHEADER), bm.bmWidth, -rows, 1, 32, BI_RGB };
    std::vector<BYTE> px((size_t)bm.bmWidth * rows * 4);
    // o bitmap está selecionado no wallDC: GetDIBits lê o topo (de cima para baixo com a altura negativa)
    if (GetDIBits(p->wallDC, p->wall, 0, rows, px.data(), &bi, DIB_RGB_COLORS) <= 0) return false;
    double sr = 0, sg = 0, sb = 0; size_t n = 0;
    for (int y = 0; y < rows; y += 2)
        for (int x = 0; x < bm.bmWidth; x += 6) {
            const BYTE* q = &px[((size_t)y * bm.bmWidth + x) * 4];
            sb += q[0]; sg += q[1]; sr += q[2]; n++;
        }
    if (!n) return false;
    r = (float)(sr / n / 255); g = (float)(sg / n / 255); b = (float)(sb / n / 255);
    lastBmp = p->wall; lastPx = barPx; cr = r; cg = g; cb = b;
    return true;
}

void mesa::WallpaperNow(const std::wstring& path) {
    { std::lock_guard<std::mutex> lk(g_wallMx); g_wallNow = path; }
    if (g_ctl) PostMessageW(g_ctl, WM_MS_WALLNOW, 0, 0);
}

void mesa::NeedExplorer(const std::wstring& aumid) {
    { std::lock_guard<std::mutex> lk(g_uwpMx); g_uwpPending = aumid; }
    if (!g_uwpWant.exchange(true)) Log(L"mesa: %ls precisa do Explorador; abrindo ele escondido", aumid.c_str());
}
bool mesa::IconAt(POINT screen) {
    HWND w = WindowFromPoint(screen);
    wchar_t c[32] = {}; GetClassNameW(GetAncestor(w, GA_ROOT), c, 32);
    if (wcscmp(c, L"SequoiaMesa")) return false;
    return HitScreen(screen) >= 0;
}

void mesa::Start(HINSTANCE hi, HWND msgWnd) {
    g_hi = hi; g_msgWnd = msgWnd;
    g_explorerAtStart = ExplorerProgman() != nullptr;
    g_userDesk = Known(FOLDERID_Desktop);
    g_publicDesk = Known(FOLDERID_PublicDesktop);
    g_posFile = Known(FOLDERID_RoamingAppData) + L"\\Shell\\mesa.txt";
    WNDCLASSW dc{}; dc.lpfnWndProc = DeskProc; dc.hInstance = hi; dc.lpszClassName = DESK_CLASS;
    dc.style = CS_DBLCLKS; dc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&dc);
    WNDCLASSW cc{}; cc.lpfnWndProc = CtlProc; cc.hInstance = hi; cc.lpszClassName = CTL_CLASS;
    RegisterClassW(&cc);
    g_ctl = CreateWindowExW(0, CTL_CLASS, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, hi, nullptr);
    if (!g_ctl) Log(L"mesa: ERRO criando a janela de controle (%lu)", GetLastError());
    SetTimer(g_ctl, 1, 500, nullptr);
    SetTimer(g_ctl, 3, 1500, nullptr);
    Log(L"mesa: ajuste explorer=%ls, Shell registrado como shell do Windows: %ls", shell::Setting(L"explorer", L"(vazio)").c_str(), WinlogonIsOurs() ? L"sim" : L"não");
    if (!shell::nativo) StartWatchdog();  // sempre: Ctrl+Alt+Shift+F11 e Shell travado devolvem o Explorador; Shell caído também
    Tick();
}

// o jogo fechou: a troca de tela que ficou para depois (confere de novo se os monitores mudaram)
void mesa::DepoisDoJogo() {
    if (g_rebuildLater && g_ctl) { g_rebuildLater = false; PostMessageW(g_ctl, WM_MS_REBUILD, 1, 0); }
}

void mesa::SettingsChanged() {
    Log(L"mesa: ajustes mudaram (explorer=%ls)", shell::Setting(L"explorer", L"1").c_str());
    if (g_ctl) { g_closeTries = 0; Tick(); }
    { static int lastIcons = -1; int on = IconesOn() ? 1 : 0; if (lastIcons >= 0 && on != lastIcons && Primary()) Redraw(); lastIcons = on; }  // ícones ligados/desligados nos Ajustes
    if (LerMedidas() && Primary()) { ForgetIcons(); QueueIcons(); Redraw(); }  // tamanho do ícone, grade ou texto mudou
    if ((shell::Setting(L"mesa_pilhas", L"0") == L"1") != g_stacks && Primary()) {  // Pilhas ligadas/desligadas fora do menu da mesa
        g_openStacks.clear();
        for (auto& it : g_items) it.col = it.row = -1;
        Reload();
    }
}

static bool g_noExplorerOnExit;
void mesa::NoExplorerOnExit() { g_noExplorerOnExit = true; }

void mesa::Stop(bool restartExplorer) {
    bool was = g_standalone || (DeskExists() && !ExplorerProgman());
    if (g_ctl) KillTimer(g_ctl, 1);
    if (!restartExplorer) {  // caiu / fim da sessão: só o que é seguro aqui; o vigia abre o Explorador
        if (g_minChanged) { SystemParametersInfoW(SPI_SETMINIMIZEDMETRICS, sizeof(g_origMin), &g_origMin, 0); g_minChanged = false; }
        return;
    }
    if (g_endEvent) SetEvent(g_endEvent);  // saída normal: o vigia não faz nada
    LeaveShell();
    DestroyDesk();  // a janela de shell some com ela: o Explorador volta como mesa, não como janela de pasta
    // reiniciando: o Shell novo já vem; abrir o Explorador só para o Shell novo dispensar em seguida deixava
    // o Explorador travado no meio do caminho (e ele segurava o Shell novo)
    if (was && !g_noExplorerOnExit && !GetSystemMetrics(SM_SHUTTINGDOWN)) StartExplorer(L"Shell saindo");
}

void mesa::WinKey(UINT vk) {
    if (vk == 'E') {
        std::wstring finder = shell::dir + L"\\" EXE_FINDER;
        bool usaFinder = shell::Setting(L"finder_padrao", L"1") == L"1" && GetFileAttributesW(finder.c_str()) != INVALID_FILE_ATTRIBUTES;
        if (!usaFinder && !Standalone()) { ShellExecuteW(nullptr, L"open", L"explorer.exe", nullptr, nullptr, SW_SHOWNORMAL); return; }
        AllowSetForegroundWindow(ASFW_ANY);
        ShellExecuteW(nullptr, L"open", finder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (vk == 'R') {  // Executar (shell32 #61), no próprio Shell
        typedef void(WINAPI * RunFileDlgFn)(HWND, HICON, LPCWSTR, LPCWSTR, LPCWSTR, UINT);
        auto f = (RunFileDlgFn)GetProcAddress(GetModuleHandleW(L"shell32.dll"), MAKEINTRESOURCEA(61));
        if (f) { Mon* p = Primary(); f(p ? p->h : nullptr, nullptr, nullptr, nullptr, nullptr, 0); }
    } else if (vk == 'D') ShowDesktopToggle();
    else if (vk == 'S') {  // Win+Shift+S: a Ferramenta de Captura no modo recorte
        HINSTANCE r = ShellExecuteW(nullptr, L"open", L"ms-screenclip:", nullptr, nullptr, SW_SHOWNORMAL);
        if ((INT_PTR)r <= 32) Log(L"mesa: recorte de tela não abriu (%d)", (int)(INT_PTR)r);
    }
}

bool mesa::IsWindowsShell() { return WinlogonIsOurs(); }

// Apps de inicialização: com o LiquinixShell como shell do Windows, o Explorador não abre no login e quem
// roda Run (HKCU, HKLM, HKLM 32 bits) e as pastas Inicializar é o Shell. Uma vez por sessão (átomo global,
// some no logoff), respeitando o que foi desligado no Gerenciador de Tarefas (StartupApproved).
void mesa::RunStartupApps() {
    if (!WinlogonIsOurs()) return;
    if (shell::Setting(L"explorer", L"") == L"1") { Log(L"inicialização: explorer=1 (o Explorador abre os apps)"); return; }
    if (GlobalFindAtomW(L"SequoiaShellInicializacao")) { Log(L"inicialização: já rodou nesta sessão"); return; }
    GlobalAddAtomW(L"SequoiaShellInicializacao");
    if (g_explorerAtStart) { Log(L"inicialização: o Explorador já estava aberto (ele abriu os apps)"); return; }
    std::thread([] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        const wchar_t* APPROVED = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\";
        auto disabled = [&](const wchar_t* sub, const std::wstring& name) {
            for (HKEY root : { HKEY_CURRENT_USER, HKEY_LOCAL_MACHINE }) {
                BYTE d[16]; DWORD cb = sizeof(d);
                if (RegGetValueW(root, (std::wstring(APPROVED) + sub).c_str(), name.c_str(), RRF_RT_REG_BINARY, nullptr, d, &cb) == ERROR_SUCCESS && cb && (d[0] & 1))
                    return true;
            }
            return false;
        };
        int n = 0;
        struct Src { HKEY root; const wchar_t* key; REGSAM view; const wchar_t* approved; };
        for (auto s : { Src{ HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", KEY_WOW64_64KEY, L"Run" },
                        Src{ HKEY_LOCAL_MACHINE, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", KEY_WOW64_32KEY, L"Run32" },
                        Src{ HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, L"Run" } }) {
            HKEY k;
            if (RegOpenKeyExW(s.root, s.key, 0, KEY_READ | s.view, &k) != ERROR_SUCCESS) continue;
            wchar_t name[256], data[2048];
            for (DWORD i = 0;; i++) {
                DWORD nn = 256, cb = sizeof(data), type = 0;
                if (RegEnumValueW(k, i, name, &nn, nullptr, &type, (BYTE*)data, &cb) != ERROR_SUCCESS) break;
                if (type != REG_SZ && type != REG_EXPAND_SZ) continue;
                if (disabled(s.approved, name)) { Log(L"inicialização: %ls desligado", name); continue; }
                wchar_t cmd[4096]; ExpandEnvironmentStringsW(data, cmd, 4096);
                STARTUPINFOW si{ sizeof(si) }; PROCESS_INFORMATION pi{};
                if (CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); n++; }
                else Log(L"inicialização: ERRO %lu abrindo %ls", GetLastError(), name);
            }
            RegCloseKey(k);
        }
        for (auto id : { FOLDERID_Startup, FOLDERID_CommonStartup }) {
            std::wstring dir = Known(id);
            if (dir.empty()) continue;
            WIN32_FIND_DATAW fd;
            HANDLE f = FindFirstFileW((dir + L"\\*").c_str(), &fd);
            if (f == INVALID_HANDLE_VALUE) continue;
            do {
                if ((fd.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) || !_wcsicmp(fd.cFileName, L"desktop.ini")) continue;
                if (disabled(L"StartupFolder", fd.cFileName)) { Log(L"inicialização: %ls desligado", fd.cFileName); continue; }
                std::wstring p = dir + L"\\" + fd.cFileName;
                if ((INT_PTR)ShellExecuteW(nullptr, nullptr, p.c_str(), nullptr, dir.c_str(), SW_SHOWNORMAL) > 32) n++;
            } while (FindNextFileW(f, &fd));
            FindClose(f);
        }
        Log(L"inicialização: %d app(s) abertos pelo Shell", n);
        CoUninitialize();
    }).detach();
}

// LiquinixShell.exe --mesa-vigia <pid>: se o Shell sumir sem sair direito, abre o Explorador
bool mesa::Watchdog(const wchar_t* cmdLine) {
    const wchar_t* a = wcsstr(cmdLine, L"--mesa-vigia ");
    if (!a) return false;
    DWORD pid = wcstoul(a + 13, nullptr, 10);
    HANDLE proc = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, pid);
    if (!proc) proc = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    wchar_t name[64]; swprintf_s(name, L"SequoiaMesaFim-%lu", pid);
    HANDLE ev = OpenEventW(SYNCHRONIZE, FALSE, name);
    // o vigia escreve no mesmo shell.log (o Shell abre com compartilhamento)
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring logPath = Parent(exe) + L"\\shell.log";
    auto vlog = [&](const wchar_t* text) {
        FILE* f = _wfsopen(logPath.c_str(), L"a, ccs=UTF-8", _SH_DENYNO);
        if (!f) return;
        SYSTEMTIME st; GetLocalTime(&st);
        fwprintf(f, L"%02d:%02d:%02d vigia da mesa: %ls\n", st.wHour, st.wMinute, st.wSecond, text);
        fclose(f);
    };
    // Emergência: o Explorador volta de qualquer jeito. Shell respondendo: sai direito (ele mesmo devolve o
    // Explorador). Travado: é encerrado e o vigia abre o Explorador.
    auto shellWindow = [&]() -> HWND {
        HWND h = nullptr;
        while ((h = FindWindowExW(nullptr, h, L"ShellMensagens", nullptr))) if (PidOf(h) == pid) return h;
        return nullptr;
    };
    bool done = false;
    // Shell travado: encerra e abre ele de novo (no lugar de devolver o Explorador)
    auto restart = [&](const wchar_t* why) {
        vlog(why);
        BOOL killed = TerminateProcess(proc, 1);
        WaitForSingleObject(proc, 5000);
        if (!killed) { vlog(L"não consegui encerrar o Shell travado"); return; }
        SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0);  // o ponteiro pode ter ficado grande
        Sleep(500);
        std::wstring dir = Parent(exe), cmd = std::wstring(L"\"") + exe + L"\"";
        STARTUPINFOW si{ sizeof(si) }; PROCESS_INFORMATION pi{};
        if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            vlog(L"Shell aberto de novo");
        } else { vlog(L"não consegui abrir o Shell de novo; abrindo o Explorador"); StartExplorer(L"vigia: Shell não reabriu"); }
        done = true;
    };
    wchar_t pn[64]; swprintf_s(pn, L"SequoiaShellPulso-%lu", pid);
    HANDLE pulseMap = nullptr; const volatile LONG* pulse = nullptr;
    LONG lastPulse = -1; DWORD pulseSince = GetTickCount(), lastLoop = GetTickCount();
    auto emergency = [&](const wchar_t* why) {
        vlog(why);
        HWND sw = shellWindow();
        DWORD_PTR r = 0;
        if (sw && SendMessageTimeoutW(sw, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 1000, &r)) {
            PostMessageW(sw, WM_HOTKEY, 1, 0);  // o "sair do Shell" (Ctrl+Alt+Shift+F12)
            if (WaitForSingleObject(proc, 20000) == WAIT_OBJECT_0) { vlog(L"o Shell saiu e devolveu o Explorador"); done = true; return; }
        }
        BOOL killed = TerminateProcess(proc, 1);
        WaitForSingleObject(proc, 5000);
        vlog(killed ? L"o Shell travado foi encerrado; abrindo o Explorador" : L"não consegui encerrar o Shell; abrindo o Explorador mesmo assim");
        Sleep(500);
        StartExplorer(L"emergência");
        // Explorador que já estava aberto: a barra de tarefas dele estava escondida pelo Shell
        EnumWindows([](HWND h, LPARAM) -> BOOL {
            wchar_t c[64] = {}; GetClassNameW(h, c, 64);
            if (!wcscmp(c, L"Shell_TrayWnd") || !wcscmp(c, L"Shell_SecondaryTrayWnd")) ShowWindow(h, SW_SHOWNOACTIVATE);
            return TRUE;
        }, 0);
        done = true;
    };
    if (proc && ev) {
        RegisterHotKey(nullptr, 1, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, VK_F11);  // era E: AltGr+Shift+E (= Ctrl+Alt+Shift+E) apertava sem querer
        HANDLE hs[2] = { ev, proc };
        DWORD r = 0;
        DWORD hungSince = 0;
        for (;;) {
            r = MsgWaitForMultipleObjects(2, hs, FALSE, 1000, QS_ALLINPUT);
            if (r == WAIT_OBJECT_0 || r == WAIT_OBJECT_0 + 1) break;
            MSG m;
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE))
                if (m.message == WM_HOTKEY && m.wParam == 1) emergency(L"Ctrl+Alt+Shift+F11: devolvendo o Explorador");
            if (done) break;
            DWORD now = GetTickCount();
            if (now - lastLoop > 5000) { pulseSince = now; hungSince = 0; }  // o PC dormiu: não conta como travado
            lastLoop = now;
            // pulso parado há 10 s: a interface está presa, mesmo respondendo a mensagens
            if (!pulse) {
                if (!pulseMap) pulseMap = OpenFileMappingW(FILE_MAP_READ, FALSE, pn);
                if (pulseMap) pulse = (const volatile LONG*)MapViewOfFile(pulseMap, FILE_MAP_READ, 0, 0, sizeof(LONG));
            }
            if (pulse) {
                LONG v = *pulse;
                if (v != lastPulse) { lastPulse = v; pulseSince = now; }
                else if (now - pulseSince > 10000) restart(L"o Shell parou (sem pulso há 10 s); reiniciando");
                if (done) break;
            }
            // a janela do Shell não responde há 5 s: reinicia
            HWND sw = shellWindow();
            DWORD_PTR res = 0;
            bool hung = sw && !SendMessageTimeoutW(sw, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, 1000, &res);
            if (!hung) { hungSince = 0; continue; }
            if (!hungSince) hungSince = GetTickCount();
            if (GetTickCount() - hungSince > 5000) restart(L"o Shell não responde há 5 s; reiniciando");
            if (done) break;
        }
        UnregisterHotKey(nullptr, 1);
        // o Shell não saiu direito: o ponteiro pode ter ficado grande (sacudir para localizar); o esquema
        // de cursores é relido do registro
        if (WaitForSingleObject(ev, 0) != WAIT_OBJECT_0) SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, 0);
        if (!done && r == WAIT_OBJECT_0 + 1 && WaitForSingleObject(ev, 0) != WAIT_OBJECT_0) {
            vlog(L"o Shell sumiu sem sair direito");
            FILETIME c{}, e{}, k{}, u{};
            double lived = GetProcessTimes(proc, &c, &e, &k, &u)
                ? ((((ULONGLONG)e.dwHighDateTime << 32) | e.dwLowDateTime) - (((ULONGLONG)c.dwHighDateTime << 32) | c.dwLowDateTime)) / 1e7 : 0;
            Sleep(1500);
            if (!GetSystemMetrics(SM_SHUTTINGDOWN) && !GetShellWindow()) {
                if (WinlogonIsOurs() && lived > 30) {
                    // o Shell é o shell do Windows e caiu depois de rodar um tempo: abre ele de novo
                    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
                    std::wstring dir = Parent(exe), cmd = std::wstring(L"\"") + exe + L"\"";
                    STARTUPINFOW si{ sizeof(si) }; PROCESS_INFORMATION pi{};
                    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) { CloseHandle(pi.hThread); CloseHandle(pi.hProcess); }
                } else StartExplorer(L"vigia: o Shell caiu");  // caiu logo ao abrir (ou não é o shell): o Explorador volta
            }
        }
    }
    if (pulse) UnmapViewOfFile((LPCVOID)pulse);
    if (pulseMap) CloseHandle(pulseMap);
    if (proc) CloseHandle(proc);
    if (ev) CloseHandle(ev);
    return true;
}
