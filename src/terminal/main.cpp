// Terminal: o Terminal do macOS (perfil "Básico" escuro) rodando o PowerShell e o Prompt de Comando.
// O shell de verdade roda num pseudoconsole (ConPTY, o mesmo do Windows Terminal); a saída chega em
// sequências VT (cores, cursor, apagar, rolar), que interpretamos numa grade de células e desenhamos com
// Direct2D na fonte do Mac (SF Mono, se instalada; senão Menlo, Cascadia Mono ou Consolas).
// Barra de título do Mac: bolinhas e "Pasta — shell — 80×24" com a pastinha azul. A pasta atual vem de
// uma marca invisível no prompt (OSC 9;9, a mesma convenção do Windows Terminal), posta só nesta janela.
//   Terminal.exe              PowerShell
//   Terminal.exe --cmd        Prompt de Comando
//   Terminal.exe --pasta X    começa na pasta X
//   Terminal.exe --executar F roda o script F (.ps1) no PowerShell ao abrir ("Executar no Terminal" do Finder)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <shellscalingapi.h>
#include <wincodec.h>
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <thread>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <share.h>
#include "../menu/context_menu.h"
#include "../comum/ampliacao.h"  // botão verde com a animação do ampliar do Shell
#include "../comum/idioma.h"
#include "../comum/semaforo.h"

FILE* g_log;
void Log(const wchar_t* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fwprintf(g_log, L"%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
    va_list a; va_start(a, fmt); vfwprintf(g_log, fmt, a); va_end(a);
    fputwc(L'\n', g_log); fflush(g_log);
}

namespace {

// ---------------------------------------------------------------------------
// Medidas (pt): barra de título de 28 (referência: 57 px em 2x), margem do texto 5 × 4
const float TITLE_H = 28, PAD_X = 5, PAD_Y = 4, FONT_PT = 12;
const int SCROLLBACK = 10000;
const UINT WM_TERM_OUTPUT = WM_APP + 1, WM_TERM_EXIT = WM_APP + 2;

// Cores do perfil "Básico" escuro do Terminal (16 cores ANSI do macOS)
const UINT32 ANSI[16] = {
    0x000000, 0x990000, 0x00A600, 0x999900, 0x0000B2, 0xB200B2, 0x00A6B2, 0xBFBFBF,
    0x666666, 0xE50000, 0x00D900, 0xE5E500, 0x0000FF, 0xE500E5, 0x00E5E5, 0xE5E5E5 };
const UINT32 DEF_FG = 0xF2F2F2, DEF_BG = 0x1E1E1E;
const UINT32 F_DEFAULT = 0xFF000000;  // "cor padrão" (segue o tema)

struct Cell {
    char32_t ch = U' ';
    UINT32 fg = F_DEFAULT, bg = F_DEFAULT;
    BYTE attr = 0;  // 1 negrito, 2 sublinhado, 4 inverso, 8 itálico, 16 apagado (dim), 32 continuação de caractere largo
};
enum { A_BOLD = 1, A_UNDER = 2, A_INV = 4, A_ITAL = 8, A_DIM = 16, A_WIDE2 = 32 };
using Line = std::vector<Cell>;

// ---------------------------------------------------------------------------
// Tela (grade de células) e o intérprete VT
struct Screen {
    int cols = 80, rows = 24;
    std::vector<Line> main, alt;
    bool useAlt = false;
    std::deque<Line> history;  // linhas que saíram por cima (só da tela principal)
    int cx = 0, cy = 0, savedX = 0, savedY = 0;
    bool pendingWrap = false, cursorVisible = true, autoWrap = true, appCursor = false, bracketedPaste = false;
    int top = 0, bottom = 23;  // região de rolagem
    Cell pen;                  // atributos atuais
    std::wstring title, cwd;

    std::vector<Line>& buf() { return useAlt ? alt : main; }
    Line Blank() const { Line l(cols); for (auto& c : l) { c.bg = pen.bg; } return l; }
    Line BlankDefault() const { return Line(cols); }

    void Init(int c, int r) {
        cols = c; rows = r;
        main.assign(rows, Line(cols)); alt.assign(rows, Line(cols));
        top = 0; bottom = rows - 1; cx = cy = 0;
    }
    void Resize(int c, int r) {
        auto fix = [&](std::vector<Line>& b, bool keepHistory) {
            for (auto& l : b) l.resize(c);
            if (r < (int)b.size()) {  // encolheu: as linhas de cima vão para o histórico
                int drop = (int)b.size() - r;
                int below = std::max(0, (int)b.size() - 1 - cy);
                int fromBottom = std::min(drop, below);
                b.resize(b.size() - fromBottom);  // primeiro as vazias embaixo do cursor
                drop -= fromBottom;
                for (int i = 0; i < drop; i++) { if (keepHistory) Push(b.front()); b.erase(b.begin()); }
                if (keepHistory) cy = std::max(0, cy - drop);
            }
            while ((int)b.size() < r) b.push_back(Line(c));
        };
        for (auto& l : history) l.resize(c);
        fix(main, true); fix(alt, false);
        cols = c; rows = r; top = 0; bottom = rows - 1;
        cx = std::min(cx, cols - 1); cy = std::min(cy, rows - 1);
        pendingWrap = false;
    }
    void Push(const Line& l) { history.push_back(l); if ((int)history.size() > SCROLLBACK) history.pop_front(); }
    void ScrollUp(int n) {  // conteúdo da região sobe n linhas
        auto& b = buf();
        for (int k = 0; k < n; k++) {
            if (top == 0 && !useAlt) Push(b[top]);
            b.erase(b.begin() + top);
            b.insert(b.begin() + bottom, Blank());
        }
    }
    void ScrollDown(int n) {
        auto& b = buf();
        for (int k = 0; k < n; k++) { b.erase(b.begin() + bottom); b.insert(b.begin() + top, Blank()); }
    }
    void LineFeed() { if (cy == bottom) ScrollUp(1); else if (cy < rows - 1) cy++; }
    void Put(char32_t ch) {
        int w = CharWidth(ch);
        if (w == 0) return;  // marcas de combinação: ignoradas
        if (pendingWrap && autoWrap) { cx = 0; LineFeed(); pendingWrap = false; }
        if (w == 2 && cx == cols - 1) { if (autoWrap) { buf()[cy][cx] = pen; buf()[cy][cx].ch = U' '; cx = 0; LineFeed(); } }
        auto& l = buf()[cy];
        Cell c = pen; c.ch = ch;
        l[cx] = c;
        if (w == 2 && cx + 1 < cols) { Cell c2 = pen; c2.ch = U' '; c2.attr |= A_WIDE2; l[cx + 1] = c2; }
        cx += w;
        if (cx >= cols) { cx = cols - 1; pendingWrap = true; }
    }
    static int CharWidth(char32_t c) {
        if (c >= 0x300 && c <= 0x36F) return 0;
        if (c == 0x200B || c == 0x200D || (c >= 0xFE00 && c <= 0xFE0F)) return 0;
        if ((c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0xA4CF) || (c >= 0xAC00 && c <= 0xD7A3) || (c >= 0xF900 && c <= 0xFAFF) ||
            (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xFF00 && c <= 0xFF60) || (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1FAFF) ||
            (c >= 0x20000 && c <= 0x3FFFD)) return 2;
        return 1;
    }
    void EraseInLine(int mode) {
        auto& l = buf()[cy];
        int a = mode == 0 ? cx : 0, b = mode == 1 ? cx : cols - 1;
        for (int i = a; i <= b && i < cols; i++) { l[i] = Cell(); l[i].bg = pen.bg; }
    }
    void EraseInDisplay(int mode) {
        auto& b = buf();
        if (mode == 3) { history.clear(); return; }
        if (mode == 0) { EraseInLine(0); for (int y = cy + 1; y < rows; y++) b[y] = Blank(); }
        else if (mode == 1) { EraseInLine(1); for (int y = 0; y < cy; y++) b[y] = Blank(); }
        else for (int y = 0; y < rows; y++) b[y] = Blank();
    }
};

struct Parser {
    enum St { GROUND, ESC, CSI, OSC, OSC_ESC, CHARSET } st = GROUND;
    std::string params; char inter = 0, priv = 0;
    std::string osc;
    std::string utf8; int need = 0;
};

Screen g_scr;
Parser g_ps;
std::string g_reply;  // respostas ao shell (DSR/DA)

void Sgr(const std::vector<int>& p) {
    auto& pen = g_scr.pen;
    if (p.empty()) { pen = Cell(); return; }
    for (size_t i = 0; i < p.size(); i++) {
        int v = p[i];
        if (v == 0) { pen.fg = pen.bg = F_DEFAULT; pen.attr = 0; }
        else if (v == 1) pen.attr |= A_BOLD;
        else if (v == 2) pen.attr |= A_DIM;
        else if (v == 3) pen.attr |= A_ITAL;
        else if (v == 4) pen.attr |= A_UNDER;
        else if (v == 7) pen.attr |= A_INV;
        else if (v == 22) pen.attr &= ~(A_BOLD | A_DIM);
        else if (v == 23) pen.attr &= ~A_ITAL;
        else if (v == 24) pen.attr &= ~A_UNDER;
        else if (v == 27) pen.attr &= ~A_INV;
        else if (v >= 30 && v <= 37) pen.fg = v - 30;
        else if (v >= 90 && v <= 97) pen.fg = v - 90 + 8;
        else if (v == 39) pen.fg = F_DEFAULT;
        else if (v >= 40 && v <= 47) pen.bg = v - 40;
        else if (v >= 100 && v <= 107) pen.bg = v - 100 + 8;
        else if (v == 49) pen.bg = F_DEFAULT;
        else if ((v == 38 || v == 48) && i + 1 < p.size()) {
            UINT32 col = F_DEFAULT;
            if (p[i + 1] == 5 && i + 2 < p.size()) { col = 0x01000000 | (UINT32)p[i + 2]; i += 2; }  // paleta de 256 (marcada)
            else if (p[i + 1] == 2 && i + 4 < p.size()) { col = 0x02000000 | ((p[i + 2] & 255) << 16) | ((p[i + 3] & 255) << 8) | (p[i + 4] & 255); i += 4; }
            if (v == 38) pen.fg = col; else pen.bg = col;
        }
    }
}

void Csi(char fin) {
    std::vector<int> p;
    { int cur = -1; for (char c : g_ps.params) { if (c >= '0' && c <= '9') cur = (cur < 0 ? 0 : cur) * 10 + (c - '0'); else if (c == ';' || c == ':') { p.push_back(cur < 0 ? 0 : cur); cur = -1; } } if (cur >= 0 || !g_ps.params.empty()) p.push_back(cur < 0 ? 0 : cur); }
    auto P = [&](size_t i, int def) { return i < p.size() && p[i] > 0 ? p[i] : def; };
    auto& s = g_scr;
    s.pendingWrap = s.pendingWrap && (fin == 'm');
    auto clampXY = [&] { s.cx = std::clamp(s.cx, 0, s.cols - 1); s.cy = std::clamp(s.cy, 0, s.rows - 1); };
    if (g_ps.priv == '?') {
        bool on = fin == 'h';
        if (fin != 'h' && fin != 'l') return;
        for (int m : p) {
            if (m == 25) s.cursorVisible = on;
            else if (m == 7) s.autoWrap = on;
            else if (m == 1) s.appCursor = on;
            else if (m == 2004) s.bracketedPaste = on;
            else if (m == 1049 || m == 1047 || m == 47) {
                if (on && !s.useAlt) { if (m == 1049) { s.savedX = s.cx; s.savedY = s.cy; } s.useAlt = true; for (auto& l : s.alt) l = s.BlankDefault(); }
                else if (!on && s.useAlt) { s.useAlt = false; if (m == 1049) { s.cx = s.savedX; s.cy = s.savedY; } }
            }
        }
        return;
    }
    if (g_ps.priv == '>' || g_ps.priv == '=') { if (fin == 'c') g_reply += "\x1b[>0;10;1c"; return; }
    auto& b = s.buf();
    switch (fin) {
    case 'A': s.cy = std::max(s.cy == s.top ? s.top : 0, s.cy - P(0, 1)); break;
    case 'B': case 'e': s.cy = std::min(s.rows - 1, s.cy + P(0, 1)); break;
    case 'C': case 'a': s.cx = std::min(s.cols - 1, s.cx + P(0, 1)); break;
    case 'D': s.cx = std::max(0, s.cx - P(0, 1)); break;
    case 'E': s.cx = 0; s.cy = std::min(s.rows - 1, s.cy + P(0, 1)); break;
    case 'F': s.cx = 0; s.cy = std::max(0, s.cy - P(0, 1)); break;
    case 'G': case '`': s.cx = P(0, 1) - 1; clampXY(); break;
    case 'd': s.cy = P(0, 1) - 1; clampXY(); break;
    case 'H': case 'f': s.cy = P(0, 1) - 1; s.cx = P(1, 1) - 1; clampXY(); break;
    case 'J': s.EraseInDisplay(p.empty() ? 0 : p[0]); break;
    case 'K': s.EraseInLine(p.empty() ? 0 : p[0]); break;
    case 'X': { int n = P(0, 1); for (int i = s.cx; i < s.cx + n && i < s.cols; i++) { b[s.cy][i] = Cell(); b[s.cy][i].bg = s.pen.bg; } break; }
    case '@': { int n = std::min(P(0, 1), s.cols - s.cx); auto& l = b[s.cy]; l.insert(l.begin() + s.cx, n, Cell()); l.resize(s.cols); break; }
    case 'P': { int n = std::min(P(0, 1), s.cols - s.cx); auto& l = b[s.cy]; l.erase(l.begin() + s.cx, l.begin() + s.cx + n); l.resize(s.cols); break; }
    case 'L': if (s.cy >= s.top && s.cy <= s.bottom) { int n = P(0, 1); for (int k = 0; k < n; k++) { b.erase(b.begin() + s.bottom); b.insert(b.begin() + s.cy, s.Blank()); } } break;
    case 'M': if (s.cy >= s.top && s.cy <= s.bottom) { int n = P(0, 1); for (int k = 0; k < n; k++) { b.erase(b.begin() + s.cy); b.insert(b.begin() + s.bottom, s.Blank()); } } break;
    case 'S': s.ScrollUp(P(0, 1)); break;
    case 'T': s.ScrollDown(P(0, 1)); break;
    case 'm': Sgr(p); break;
    case 'r': s.top = P(0, 1) - 1; s.bottom = P(1, s.rows) - 1; if (s.top >= s.bottom || s.bottom >= s.rows) { s.top = 0; s.bottom = s.rows - 1; } s.cx = 0; s.cy = 0; break;
    case 's': s.savedX = s.cx; s.savedY = s.cy; break;
    case 'u': s.cx = s.savedX; s.cy = s.savedY; clampXY(); break;
    case 'n': if (P(0, 0) == 6) { char r[32]; sprintf_s(r, "\x1b[%d;%dR", s.cy + 1, s.cx + 1); g_reply += r; } else if (P(0, 0) == 5) g_reply += "\x1b[0n"; break;
    case 'c': g_reply += "\x1b[?1;0c"; break;
    default: break;  // t (janela), q (estilo do cursor) e o resto: ignorados
    }
}

void Osc() {
    std::string& o = g_ps.osc;
    size_t semi = o.find(';');
    std::string num = o.substr(0, semi);
    std::string arg = semi == std::string::npos ? "" : o.substr(semi + 1);
    auto wide = [](const std::string& u) { int n = MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), nullptr, 0); std::wstring w(n, 0); MultiByteToWideChar(CP_UTF8, 0, u.c_str(), (int)u.size(), w.data(), n); return w; };
    if (num == "0" || num == "2") g_scr.title = wide(arg);
    else if (num == "9" && arg.rfind("9;", 0) == 0) {  // pasta atual (marca do prompt)
        std::string path = arg.substr(2);
        if (path.size() >= 2 && path.front() == '"' && path.back() == '"') path = path.substr(1, path.size() - 2);
        g_scr.cwd = wide(path);
    }
}

void Feed(const char* data, size_t n) {
    auto& s = g_scr;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)data[i];
        switch (g_ps.st) {
        case Parser::GROUND:
            if (c == 0x1B) { g_ps.st = Parser::ESC; continue; }
            if (c < 0x20 || c == 0x7F) {
                if (c == '\r') { s.cx = 0; s.pendingWrap = false; }
                else if (c == '\n' || c == 0x0B || c == 0x0C) { s.LineFeed(); s.pendingWrap = false; }
                else if (c == '\b') { if (s.cx > 0) s.cx--; s.pendingWrap = false; }
                else if (c == '\t') { s.cx = std::min(s.cols - 1, (s.cx / 8 + 1) * 8); }
                else if (c == 0x07) { /* sino: nada */ }
                continue;
            }
            // UTF-8
            if (g_ps.need) {
                g_ps.utf8 += (char)c;
                if (--g_ps.need == 0) {
                    char32_t cp = 0; const std::string& u = g_ps.utf8;
                    if (u.size() == 2) cp = ((u[0] & 0x1F) << 6) | (u[1] & 0x3F);
                    else if (u.size() == 3) cp = ((u[0] & 0x0F) << 12) | ((u[1] & 0x3F) << 6) | (u[2] & 0x3F);
                    else cp = ((u[0] & 0x07) << 18) | ((u[1] & 0x3F) << 12) | ((u[2] & 0x3F) << 6) | (u[3] & 0x3F);
                    s.Put(cp);
                }
                continue;
            }
            if (c >= 0xC0) { g_ps.utf8 = (char)c; g_ps.need = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1; continue; }
            if (c >= 0x80) continue;
            s.Put(c);
            break;
        case Parser::ESC:
            g_ps.st = Parser::GROUND;
            if (c == '[') { g_ps.st = Parser::CSI; g_ps.params.clear(); g_ps.priv = 0; g_ps.inter = 0; }
            else if (c == ']') { g_ps.st = Parser::OSC; g_ps.osc.clear(); }
            else if (c == '(' || c == ')') g_ps.st = Parser::CHARSET;
            else if (c == '7') { s.savedX = s.cx; s.savedY = s.cy; }
            else if (c == '8') { s.cx = s.savedX; s.cy = s.savedY; }
            else if (c == 'M') { if (s.cy == s.top) s.ScrollDown(1); else if (s.cy > 0) s.cy--; }
            else if (c == 'D') s.LineFeed();
            else if (c == 'E') { s.cx = 0; s.LineFeed(); }
            else if (c == 'c') { s.Init(s.cols, s.rows); s.pen = Cell(); }
            break;
        case Parser::CHARSET: g_ps.st = Parser::GROUND; break;
        case Parser::CSI:
            if (c >= 0x40 && c <= 0x7E) { Csi((char)c); g_ps.st = Parser::GROUND; }
            else if (c == '?' || c == '>' || c == '=' || c == '<') g_ps.priv = (char)c;
            else if (c >= 0x20 && c <= 0x2F) g_ps.inter = (char)c;
            else g_ps.params += (char)c;
            break;
        case Parser::OSC:
            if (c == 0x07) { Osc(); g_ps.st = Parser::GROUND; }
            else if (c == 0x1B) g_ps.st = Parser::OSC_ESC;
            else if (g_ps.osc.size() < 4096) g_ps.osc += (char)c;
            break;
        case Parser::OSC_ESC: Osc(); g_ps.st = c == '\\' ? Parser::GROUND : Parser::ESC; if (g_ps.st == Parser::ESC) i--; break;
        }
    }
}

// ---------------------------------------------------------------------------
// Pseudoconsole (ConPTY) e o shell
HPCON g_pc;
HANDLE g_inWrite, g_outRead, g_proc;
std::mutex g_outMu;
std::string g_outBuf;
std::atomic<bool> g_outPosted;
bool g_isCmd;
std::wstring g_shellName = L"powershell";
std::wstring g_run;  // --executar: script para rodar assim que o PowerShell abrir
HWND g_hwnd;

void Send(const std::string& s) { if (g_inWrite && !s.empty()) { DWORD w = 0; WriteFile(g_inWrite, s.data(), (DWORD)s.size(), &w, nullptr); } }
std::string Utf8(const std::wstring& w) { int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr); std::string s(n, 0); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr); return s; }

bool StartShell(const std::wstring& startDir, int cols, int rows) {
    HANDLE inRead, outWrite;
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, FALSE };
    if (!CreatePipe(&inRead, &g_inWrite, &sa, 0) || !CreatePipe(&g_outRead, &outWrite, &sa, 0)) return false;
    if (FAILED(CreatePseudoConsole({ (SHORT)cols, (SHORT)rows }, inRead, outWrite, 0, &g_pc))) return false;
    CloseHandle(inRead); CloseHandle(outWrite);
    STARTUPINFOEXW si{}; si.StartupInfo.cb = sizeof(si);
    SIZE_T sz = 0; InitializeProcThreadAttributeList(nullptr, 1, 0, &sz);
    std::vector<BYTE> attrs(sz);
    si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)attrs.data();
    InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &sz);
    UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, g_pc, sizeof(g_pc), nullptr, nullptr);
    // a marca da pasta atual (OSC 9;9) entra antes do prompt de sempre; o prompt do usuário continua igual
    std::wstring cmd;
    std::vector<wchar_t> env;
    if (g_isCmd) {
        cmd = L"cmd.exe";
        g_shellName = L"cmd";
        // ambiente do pai + PROMPT com a marca (o $P mostra a pasta; o resto é o prompt padrão "C:\>")
        wchar_t* e = GetEnvironmentStringsW();
        for (wchar_t* p = e; *p; p += wcslen(p) + 1) { if (_wcsnicmp(p, L"PROMPT=", 7)) env.insert(env.end(), p, p + wcslen(p) + 1); }
        FreeEnvironmentStringsW(e);
        std::wstring pr = L"PROMPT=$E]9;9;$P$E\\$P$G";
        env.insert(env.end(), pr.begin(), pr.end()); env.push_back(0); env.push_back(0);
    } else {
        cmd = L"powershell.exe -NoLogo -NoExit -Command \"$__p = $function:prompt; function global:prompt { $l = $executionContext.SessionState.Path.CurrentLocation; "
              L"[char]27 + ']9;9;' + $l.ProviderPath + [char]27 + '\\' + (& $__p) }";
        if (!g_run.empty()) {  // roda o script como o "Executar com o PowerShell" do Windows, mas aqui dentro
            std::wstring q;
            for (wchar_t c : g_run) { if (c == L'\'') q += L"''"; else q += c; }
            cmd += L"; Set-ExecutionPolicy -Scope Process Bypass -Force; & '" + q + L"'";
        }
        cmd += L"\"";
        g_shellName = L"powershell";
    }
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, EXTENDED_STARTUPINFO_PRESENT | (env.empty() ? 0 : CREATE_UNICODE_ENVIRONMENT),
                             env.empty() ? nullptr : env.data(), startDir.empty() ? nullptr : startDir.c_str(), &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(si.lpAttributeList);
    if (!ok) { Log(L"não abriu o shell (%lu)", GetLastError()); return false; }
    g_proc = pi.hProcess; CloseHandle(pi.hThread);
    Log(L"shell: %ls (pid %lu) em %ls", g_shellName.c_str(), pi.dwProcessId, startDir.c_str());
    std::thread([] {  // leitura da saída: junta tudo num buffer e avisa a janela (uma mensagem por vez)
        char buf[16384]; DWORD n = 0;
        while (ReadFile(g_outRead, buf, sizeof(buf), &n, nullptr) && n) {
            { std::lock_guard<std::mutex> l(g_outMu); g_outBuf.append(buf, n); }
            if (!g_outPosted.exchange(true)) PostMessageW(g_hwnd, WM_TERM_OUTPUT, 0, 0);
        }
    }).detach();
    std::thread([] { WaitForSingleObject(g_proc, INFINITE); PostMessageW(g_hwnd, WM_TERM_EXIT, 0, 0); }).detach();
    return true;
}

// ---------------------------------------------------------------------------
// Janela e desenho
HINSTANCE g_inst;
ID2D1Factory* g_f;
IDWriteFactory* g_dw;
ID2D1HwndRenderTarget* g_rt;
IDWriteTextFormat *g_tfMono, *g_tfMonoB, *g_tfTitle;
std::wstring g_mono = L"Consolas", g_ui = L"Segoe UI";
float g_cw = 7, g_ch = 15, g_base = 12;  // tamanho da célula (pt)
bool g_active = true, g_hoverLights, g_shown;
int g_view = 0;  // linhas de histórico acima (0 = embaixo, acompanhando)
// seleção em coordenadas absolutas (linha do histórico + tela, coluna)
bool g_selecting, g_hasSel;
POINT g_selA{}, g_selB{};
bool g_cursorOn = true;

float Scale() { return GetDpiForWindow(g_hwnd) / 96.f; }
D2D1_SIZE_F Client() { RECT r; GetClientRect(g_hwnd, &r); float s = Scale(); return { r.right / s, r.bottom / s }; }

D2D1_COLOR_F Rgb(UINT32 v, float a = 1) { return D2D1::ColorF(((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f, a); }
UINT32 Palette256(int i) {
    if (i < 16) return ANSI[i];
    if (i < 232) { i -= 16; int r = i / 36, g = (i / 6) % 6, b = i % 6; auto f = [](int x) { return x ? 55 + x * 40 : 0; }; return (f(r) << 16) | (f(g) << 8) | f(b); }
    int v = 8 + (i - 232) * 10; return (v << 16) | (v << 8) | v;
}
UINT32 Resolve(UINT32 c, bool fg) {
    if (c == F_DEFAULT) return fg ? DEF_FG : DEF_BG;
    if ((c & 0xFF000000) == 0x02000000) return c & 0xFFFFFF;
    if ((c & 0xFF000000) == 0x01000000) return Palette256(c & 255);
    return ANSI[c & 15];
}

std::wstring PickFont(std::initializer_list<const wchar_t*> names, const wchar_t* def) {
    IDWriteFontCollection* fc = nullptr; g_dw->GetSystemFontCollection(&fc);
    std::wstring r = def;
    for (auto n : names) { UINT32 i; BOOL ok = FALSE; if (fc) fc->FindFamilyName(n, &i, &ok); if (ok) { r = n; break; } }
    if (fc) fc->Release();
    return r;
}

void MakeFonts() {
    g_mono = PickFont({ L"SF Mono", L"Menlo", L"Cascadia Mono", L"Consolas" }, L"Consolas");
    g_ui = PickFont({ L"SF Pro Text", L"Inter", L"Segoe UI Variable Text" }, L"Segoe UI");
    auto mk = [&](const std::wstring& fam, float sz, DWRITE_FONT_WEIGHT w, IDWriteTextFormat** out) {
        g_dw->CreateTextFormat(fam.c_str(), nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, sz, L"pt-br", out);
        (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    };
    mk(g_mono, FONT_PT * 96 / 72.f, DWRITE_FONT_WEIGHT_NORMAL, &g_tfMono);
    mk(g_mono, FONT_PT * 96 / 72.f, DWRITE_FONT_WEIGHT_BOLD, &g_tfMonoB);
    mk(g_ui, 13, DWRITE_FONT_WEIGHT_SEMI_BOLD, &g_tfTitle);
    g_tfTitle->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    // célula: largura do "M" e altura da linha da fonte
    IDWriteTextLayout* l = nullptr;
    if (SUCCEEDED(g_dw->CreateTextLayout(L"MMMMMMMMMM", 10, g_tfMono, 1000, 100, &l))) {
        DWRITE_TEXT_METRICS m{}; l->GetMetrics(&m); g_cw = m.widthIncludingTrailingWhitespace / 10; g_ch = std::ceil(m.height);
        DWRITE_LINE_METRICS lm{}; UINT32 cnt = 0; l->GetLineMetrics(&lm, 1, &cnt); g_base = lm.baseline;
        l->Release();
    }
    Log(L"fonte: %ls (célula %.2f × %.2f)", g_mono.c_str(), g_cw, g_ch);
}

int ColsFor(float w) { return std::max(20, (int)((w - 2 * PAD_X - 6) / g_cw)); }
int RowsFor(float h) { return std::max(5, (int)((h - TITLE_H - 2 * PAD_Y) / g_ch)); }

std::wstring TitleText() {
    std::wstring dir = g_scr.cwd;
    while (dir.size() > 3 && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    std::wstring name = dir.empty() ? L"" : (dir.size() <= 3 ? dir : dir.substr(dir.find_last_of(L"\\/") + 1));
    std::wstring t = (name.empty() ? L"" : name + L" — ") + g_shellName + L" — " + std::to_wstring(g_scr.cols) + L"×" + std::to_wstring(g_scr.rows);
    return t;
}

void Line2(ID2D1RenderTarget* rt, ID2D1Brush* b, float x1, float y1, float x2, float y2, float w) { rt->DrawLine({ x1, y1 }, { x2, y2 }, b, w); }

// linha absoluta -> célula (histórico + tela atual)
const Line* LineAt(int abs) {
    int h = g_scr.useAlt ? 0 : (int)g_scr.history.size();
    if (abs < h) return &g_scr.history[abs];
    abs -= h;
    if (abs >= 0 && abs < g_scr.rows) return &g_scr.buf()[abs];
    return nullptr;
}
int HistCount() { return g_scr.useAlt ? 0 : (int)g_scr.history.size(); }
bool Selected(int absRow, int col) {
    if (!g_hasSel) return false;
    POINT a = g_selA, b = g_selB;
    if (a.y > b.y || (a.y == b.y && a.x > b.x)) std::swap(a, b);
    if (absRow < a.y || absRow > b.y) return false;
    if (absRow == a.y && col < a.x) return false;
    if (absRow == b.y && col >= b.x) return false;
    return true;
}

void Render() {
    if (!g_rt) {
        RECT rc; GetClientRect(g_hwnd, &rc);
        float dpi = (float)GetDpiForWindow(g_hwnd);
        g_f->CreateHwndRenderTarget(D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), dpi, dpi),
                                    D2D1::HwndRenderTargetProperties(g_hwnd, D2D1::SizeU(rc.right, rc.bottom)), &g_rt);
        if (!g_rt) return;
        g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    }
    auto s = Client();
    g_rt->BeginDraw();
    g_rt->Clear(Rgb(DEF_BG));
    ID2D1SolidColorBrush* br = nullptr;
    g_rt->CreateSolidColorBrush(Rgb(DEF_FG), &br);

    // ---- texto
    int hist = HistCount();
    int firstAbs = hist - g_view;  // primeira linha visível
    float x0 = PAD_X, y0 = TITLE_H + PAD_Y;
    g_rt->PushAxisAlignedClip(D2D1::RectF(0, TITLE_H, s.width, s.height), D2D1_ANTIALIAS_MODE_ALIASED);
    for (int r = 0; r < g_scr.rows; r++) {
        const Line* ln = LineAt(firstAbs + r);
        if (!ln) continue;
        float y = y0 + r * g_ch;
        int n = std::min((int)ln->size(), g_scr.cols);
        // fundos (corridas da mesma cor) e seleção
        for (int c = 0; c < n;) {
            const Cell& cl = (*ln)[c];
            bool sel = Selected(firstAbs + r, c);
            UINT32 bg = (cl.attr & A_INV) ? Resolve(cl.fg, true) : Resolve(cl.bg, false);
            int e = c + 1;
            while (e < n) { const Cell& d = (*ln)[e]; UINT32 b2 = (d.attr & A_INV) ? Resolve(d.fg, true) : Resolve(d.bg, false); if (b2 != bg || Selected(firstAbs + r, e) != sel) break; e++; }
            if (sel) { br->SetColor(g_active ? D2D1::ColorF(0.30f, 0.45f, 0.75f, 0.85f) : D2D1::ColorF(1, 1, 1, 0.20f)); g_rt->FillRectangle(D2D1::RectF(x0 + c * g_cw, y, x0 + e * g_cw, y + g_ch), br); }
            else if (bg != DEF_BG) { br->SetColor(Rgb(bg)); g_rt->FillRectangle(D2D1::RectF(x0 + c * g_cw, y, x0 + e * g_cw, y + g_ch), br); }
            c = e;
        }
        // texto (corridas com os mesmos atributos)
        for (int c = 0; c < n;) {
            const Cell& cl = (*ln)[c];
            if (cl.attr & A_WIDE2) { c++; continue; }
            UINT32 fg = (cl.attr & A_INV) ? Resolve(cl.bg, false) : Resolve(cl.fg, true);
            if ((cl.attr & A_BOLD) && cl.fg != F_DEFAULT && (cl.fg & 0xFF000000) == 0 && cl.fg < 8) fg = ANSI[cl.fg + 8];  // negrito clareia, como no Mac
            BYTE at = cl.attr & (A_BOLD | A_UNDER | A_DIM | A_ITAL);
            std::wstring run;
            int e = c;
            while (e < n) {
                const Cell& d = (*ln)[e];
                if (d.attr & A_WIDE2) { e++; continue; }
                UINT32 f2 = (d.attr & A_INV) ? Resolve(d.bg, false) : Resolve(d.fg, true);
                if ((d.attr & A_BOLD) && d.fg != F_DEFAULT && (d.fg & 0xFF000000) == 0 && d.fg < 8) f2 = ANSI[d.fg + 8];
                if (f2 != fg || (d.attr & (A_BOLD | A_UNDER | A_DIM | A_ITAL)) != at) break;
                if (Screen::CharWidth(d.ch) == 2) { if (e > c) break; }  // largos sozinhos (posição exata)
                char32_t ch = d.ch;
                if (ch >= 0x10000) { ch -= 0x10000; run += (wchar_t)(0xD800 + (ch >> 10)); run += (wchar_t)(0xDC00 + (ch & 0x3FF)); }
                else run += (wchar_t)ch;
                e++;
                if (Screen::CharWidth(d.ch) == 2) break;
            }
            bool blank = run.find_first_not_of(L' ') == std::wstring::npos;
            if (!blank) {
                br->SetColor(Rgb(fg, (at & A_DIM) ? 0.6f : 1.f));
                g_rt->DrawTextW(run.c_str(), (UINT32)run.size(), (at & A_BOLD) ? g_tfMonoB : g_tfMono, D2D1::RectF(x0 + c * g_cw, y, x0 + (e + 2) * g_cw, y + g_ch), br, D2D1_DRAW_TEXT_OPTIONS_NONE);
            }
            if (at & A_UNDER) { br->SetColor(Rgb(fg)); Line2(g_rt, br, x0 + c * g_cw, y + g_base + 1.5f, x0 + e * g_cw, y + g_base + 1.5f, 1); }
            c = std::max(e, c + 1);
        }
    }
    // cursor: bloco cinza (vazado quando a janela não está ativa), só na tela de baixo
    if (g_scr.cursorVisible && g_view == 0) {
        float cx = x0 + g_scr.cx * g_cw, cy = y0 + g_scr.cy * g_ch;
        D2D1_RECT_F cr = D2D1::RectF(cx, cy, cx + g_cw, cy + g_ch);
        br->SetColor(D2D1::ColorF(0.62f, 0.62f, 0.62f));
        if (g_active) g_rt->FillRectangle(cr, br);
        else g_rt->DrawRectangle(D2D1::RectF(cr.left + 0.5f, cr.top + 0.5f, cr.right - 0.5f, cr.bottom - 0.5f), br, 1);
        if (g_active) {  // a letra embaixo do cursor fica escura
            const Line* ln = LineAt(hist + g_scr.cy);
            if (ln && g_scr.cx < (int)ln->size()) {
                char32_t ch = (*ln)[g_scr.cx].ch;
                if (ch != U' ' && ch < 0x10000) { wchar_t w = (wchar_t)ch; br->SetColor(Rgb(DEF_BG)); g_rt->DrawTextW(&w, 1, g_tfMono, D2D1::RectF(cx, cy, cx + g_cw * 2, cy + g_ch), br); }
            }
        }
    }
    // barra de rolagem fina enquanto olha o histórico
    if (g_view > 0 && hist > 0) {
        float h = s.height - TITLE_H, total = (float)(hist + g_scr.rows), th = std::max(24.f, h * g_scr.rows / total);
        float ty = TITLE_H + (h - th) * (1 - (float)g_view / hist);
        br->SetColor(D2D1::ColorF(1, 1, 1, 0.35f));
        g_rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(s.width - 9, ty + 2, s.width - 3, ty + th - 2), 3, 3), br);
    }
    g_rt->PopAxisAlignedClip();

    // ---- barra de título do Mac
    br->SetColor(g_active ? D2D1::ColorF(0.22f, 0.22f, 0.23f) : D2D1::ColorF(0.17f, 0.17f, 0.175f));
    g_rt->FillRectangle(D2D1::RectF(0, 0, s.width, TITLE_H), br);
    br->SetColor(D2D1::ColorF(0, 0, 0, 0.55f));
    Line2(g_rt, br, 0, TITLE_H - 0.5f, s.width, TITLE_H - 0.5f, 1);
    static const D2D1_COLOR_F fill_c[3] = { D2D1::ColorF(1.0f, 0.373f, 0.345f), D2D1::ColorF(0.996f, 0.737f, 0.180f), D2D1::ColorF(0.157f, 0.784f, 0.251f) }; const D2D1_COLOR_F* fill = semaforo::Pick(fill_c, semaforo::FILL);
    static const D2D1_COLOR_F ring_c[3] = { D2D1::ColorF(0.886f, 0.275f, 0.247f), D2D1::ColorF(0.882f, 0.631f, 0.086f), D2D1::ColorF(0.071f, 0.675f, 0.157f) }; const D2D1_COLOR_F* ring = semaforo::Pick(ring_c, semaforo::RING);
    static const D2D1_COLOR_F glyph_c[3] = { D2D1::ColorF(0.302f, 0, 0), D2D1::ColorF(0.6f, 0.341f, 0), D2D1::ColorF(0, 0.396f, 0) }; const D2D1_COLOR_F* glyph = semaforo::Pick(glyph_c, semaforo::GLYPH);
    bool lit = g_active || g_hoverLights;
    for (int k = 0; k < 3; k++) {  // padrão do projeto: 14 pt, 23 entre centros; barra fina: o primeiro em 19
        D2D1_POINT_2F c{ 19.f + k * 23, TITLE_H / 2 };
        br->SetColor(lit ? fill[k] : D2D1::ColorF(0.30f, 0.30f, 0.31f));
        g_rt->FillEllipse(D2D1::Ellipse(c, 7, 7), br);
        semaforo::Gloss(g_rt, c, 7, br);
        br->SetColor(lit ? ring[k] : D2D1::ColorF(0, 0, 0, 0.3f));
        g_rt->DrawEllipse(D2D1::Ellipse(c, 6.75f, 6.75f), br, 0.5f);
        if (!g_hoverLights) continue;
        br->SetColor(glyph[k]);
        if (k == 0) { Line2(g_rt, br, c.x - 2.5f, c.y - 2.5f, c.x + 2.5f, c.y + 2.5f, 1.1f); Line2(g_rt, br, c.x - 2.5f, c.y + 2.5f, c.x + 2.5f, c.y - 2.5f, 1.1f); }
        else if (k == 1) Line2(g_rt, br, c.x - 3.3f, c.y, c.x + 3.3f, c.y, 1.2f);
        else { Line2(g_rt, br, c.x - 3, c.y, c.x + 3, c.y, 1.2f); Line2(g_rt, br, c.x, c.y - 3, c.x, c.y + 3, 1.2f); }
    }
    // título centrado com a pastinha azul antes
    std::wstring t = TitleText();
    IDWriteTextLayout* tl = nullptr; float tw = 0;
    if (SUCCEEDED(g_dw->CreateTextLayout(t.c_str(), (UINT32)t.size(), g_tfTitle, 4000, TITLE_H, &tl))) { DWRITE_TEXT_METRICS m{}; tl->GetMetrics(&m); tw = m.widthIncludingTrailingWhitespace; tl->Release(); }
    bool folder = !g_scr.cwd.empty();
    float total = tw + (folder ? 22 : 0);
    float tx = std::max(84.f, (s.width - total) / 2);
    if (folder) {  // pasta do Finder em miniatura
        float fx = tx, fy = TITLE_H / 2 - 7;
        br->SetColor(D2D1::ColorF(0.20f, 0.56f, 0.90f));
        g_rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(fx, fy, fx + 7, fy + 4), 1.2f, 1.2f), br);
        br->SetColor(D2D1::ColorF(0.33f, 0.69f, 0.97f));
        g_rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(fx, fy + 2.2f, fx + 16, fy + 13.5f), 1.8f, 1.8f), br);
        br->SetColor(D2D1::ColorF(0.18f, 0.48f, 0.80f));
        g_rt->DrawEllipse(D2D1::Ellipse({ fx + 8, fy + 8.2f }, 2.4f, 2.4f), br, 1);
        tx += 22;
    }
    br->SetColor(g_active ? D2D1::ColorF(1, 1, 1, 0.72f) : D2D1::ColorF(1, 1, 1, 0.40f));
    g_rt->DrawTextW(t.c_str(), (UINT32)t.size(), g_tfTitle, D2D1::RectF(tx, 0, s.width - 10, TITLE_H), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);

    br->Release();
    if (g_rt->EndDraw() == D2DERR_RECREATE_TARGET) { g_rt->Release(); g_rt = nullptr; }
    if (!g_shown) { g_shown = true; ampliacao::Revelar(g_hwnd); }  // abrir animado (o Shell mostra no fim) ou já
}
void Invalidate() { InvalidateRect(g_hwnd, nullptr, FALSE); }

void ApplySize() {
    auto s = Client();
    int c = ColsFor(s.width), r = RowsFor(s.height);
    if (c == g_scr.cols && r == g_scr.rows) return;
    g_scr.Resize(c, r);
    if (g_pc) ResizePseudoConsole(g_pc, { (SHORT)c, (SHORT)r });
    SetWindowTextW(g_hwnd, TitleText().c_str());
}

// ---- seleção, copiar e colar
POINT CellAt(int px, int py) {
    float sc = Scale();
    float x = px / sc - PAD_X, y = py / sc - TITLE_H - PAD_Y;
    int col = std::clamp((int)std::lround(x / g_cw), 0, g_scr.cols);
    int row = (int)std::floor(y / g_ch);
    row = std::clamp(row, 0, g_scr.rows - 1);
    return { col, HistCount() - g_view + row };
}
std::wstring SelectionText() {
    POINT a = g_selA, b = g_selB;
    if (a.y > b.y || (a.y == b.y && a.x > b.x)) std::swap(a, b);
    std::wstring out;
    for (int r = a.y; r <= b.y; r++) {
        const Line* ln = LineAt(r);
        if (!ln) continue;
        int from = r == a.y ? a.x : 0, to = r == b.y ? b.x : (int)ln->size();
        std::wstring line;
        for (int c = from; c < to && c < (int)ln->size(); c++) {
            const Cell& cl = (*ln)[c];
            if (cl.attr & A_WIDE2) continue;
            char32_t ch = cl.ch;
            if (ch >= 0x10000) { ch -= 0x10000; line += (wchar_t)(0xD800 + (ch >> 10)); line += (wchar_t)(0xDC00 + (ch & 0x3FF)); } else line += (wchar_t)ch;
        }
        if (r != b.y) while (!line.empty() && line.back() == L' ') line.pop_back();
        out += line;
        if (r != b.y) out += L"\r\n";
    }
    return out;
}
void CopySelection() {
    if (!g_hasSel) return;
    std::wstring t = SelectionText();
    if (t.empty() || !OpenClipboard(g_hwnd)) return;
    EmptyClipboard();
    size_t n = (t.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, n)) { memcpy(GlobalLock(h), t.c_str(), n); GlobalUnlock(h); SetClipboardData(CF_UNICODETEXT, h); }
    CloseClipboard();
}
void Paste() {
    if (!OpenClipboard(g_hwnd)) return;
    std::wstring t;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) { if (auto* p = (const wchar_t*)GlobalLock(h)) { t = p; GlobalUnlock(h); } }
    CloseClipboard();
    if (t.empty()) return;
    std::wstring clean;  // \r\n -> \r (Enter), como num teclado
    for (size_t i = 0; i < t.size(); i++) { if (t[i] == L'\r' && i + 1 < t.size() && t[i + 1] == L'\n') continue; clean += t[i] == L'\n' ? L'\r' : t[i]; }
    std::string u = Utf8(clean);
    if (g_scr.bracketedPaste) u = "\x1b[200~" + u + "\x1b[201~";
    Send(u);
    g_view = 0;
}

void NewWindow(bool cmd) {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring args = cmd ? L"--cmd" : L"";
    if (!g_scr.cwd.empty()) args += L" --pasta \"" + g_scr.cwd + L"\"";
    ShellExecuteW(nullptr, L"open", exe, args.c_str(), nullptr, SW_SHOWNORMAL);
}

void ContextMenu(POINT screen) {
    using ctxmenu::Item;
    auto I = [](const wchar_t* t, int id, const wchar_t* sc = L"", bool dis = false) { Item i; i.label = t; i.id = id; i.shortcut = sc; i.disabled = dis; return i; };
    Item sep; sep.separator = true;
    int id = ctxmenu::ShowItems(g_hwnd, screen, { I(Tr(L"Copiar"), 1, L"Ctrl+Shift+C", !g_hasSel), I(Tr(L"Colar"), 2, L"Ctrl+Shift+V"), I(Tr(L"Selecionar Tudo"), 3, L"Ctrl+Shift+A"), sep,
                                                  I(Tr(L"Limpar Rolagem"), 4, L"Ctrl+K"), sep, I(Tr(L"Nova Janela com o PowerShell"), 5), I(Tr(L"Nova Janela com o Prompt de Comando"), 6), sep,
                                                  I(Tr(L"Abrir Pasta no Finder"), 7, L"", g_scr.cwd.empty()) });
    if (id == 1) CopySelection();
    else if (id == 2) Paste();
    else if (id == 3) { g_hasSel = true; g_selA = { 0, 0 }; g_selB = { g_scr.cols, HistCount() + g_scr.rows - 1 }; }
    else if (id == 4) { g_scr.history.clear(); g_view = 0; }
    else if (id == 5) NewWindow(false);
    else if (id == 6) NewWindow(true);
    else if (id == 7) {
        wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH); PathRemoveFileSpecW(exe);
        std::wstring finder = std::wstring(exe) + L"\\WacOSFinder.exe";
        ShellExecuteW(nullptr, L"open", GetFileAttributesW(finder.c_str()) != INVALID_FILE_ATTRIBUTES ? finder.c_str() : L"explorer.exe", (L"\"" + g_scr.cwd + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
    }
    Invalidate();
}

// ---- teclado: teclas especiais viram sequências VT; letras chegam pelo WM_CHAR
bool SpecialKey(WPARAM vk) {
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
    int mod = 1 + (shift ? 1 : 0) + (alt ? 2 : 0) + (ctrl ? 4 : 0);
    auto csi = [&](const char* base, char fin) {  // com modificador: ESC[1;<mod><fin>
        if (mod > 1) { char b[24]; sprintf_s(b, "\x1b[1;%d%c", mod, fin); Send(b); }
        else Send(base);
    };
    auto tilde = [&](int n) { char b[24]; if (mod > 1) sprintf_s(b, "\x1b[%d;%d~", n, mod); else sprintf_s(b, "\x1b[%d~", n); Send(b); };
    // atalhos do Terminal (Ctrl+Shift no lugar do Cmd)
    if (ctrl && shift) {
        if (vk == 'C') { CopySelection(); return true; }
        if (vk == 'V') { Paste(); return true; }
        if (vk == 'A') { g_hasSel = true; g_selA = { 0, 0 }; g_selB = { g_scr.cols, HistCount() + g_scr.rows - 1 }; Invalidate(); return true; }
        if (vk == 'N') { NewWindow(g_isCmd); return true; }
        if (vk == 'W') { PostMessageW(g_hwnd, WM_CLOSE, 0, 0); return true; }
    }
    if (ctrl && !shift && vk == 'V') { Paste(); return true; }  // Ctrl+V cola (como Cmd+V)
    if (ctrl && !shift && vk == 'C' && g_hasSel) { CopySelection(); g_hasSel = false; Invalidate(); return true; }  // com seleção: copia
    if (ctrl && vk == 'K' && !shift) { g_scr.history.clear(); g_view = 0; Send("\x0c"); return true; }  // Cmd+K: limpa a rolagem
    if (shift && (vk == VK_PRIOR || vk == VK_NEXT)) { g_view = std::clamp(g_view + (vk == VK_PRIOR ? g_scr.rows - 1 : -(g_scr.rows - 1)), 0, HistCount()); Invalidate(); return true; }
    bool ac = g_scr.appCursor && mod == 1;
    switch (vk) {
    case VK_UP: ac ? Send("\x1bOA") : csi("\x1b[A", 'A'); return true;
    case VK_DOWN: ac ? Send("\x1bOB") : csi("\x1b[B", 'B'); return true;
    case VK_RIGHT: ac ? Send("\x1bOC") : csi("\x1b[C", 'C'); return true;
    case VK_LEFT: ac ? Send("\x1bOD") : csi("\x1b[D", 'D'); return true;
    case VK_HOME: csi("\x1b[H", 'H'); return true;
    case VK_END: csi("\x1b[F", 'F'); return true;
    case VK_INSERT: tilde(2); return true;
    case VK_DELETE: tilde(3); return true;
    case VK_PRIOR: tilde(5); return true;
    case VK_NEXT: tilde(6); return true;
    case VK_F1: csi("\x1bOP", 'P'); return true;
    case VK_F2: csi("\x1bOQ", 'Q'); return true;
    case VK_F3: csi("\x1bOR", 'R'); return true;
    case VK_F4: csi("\x1bOS", 'S'); return true;
    case VK_F5: tilde(15); return true; case VK_F6: tilde(17); return true; case VK_F7: tilde(18); return true; case VK_F8: tilde(19); return true;
    case VK_F9: tilde(20); return true; case VK_F10: tilde(21); return true; case VK_F11: tilde(23); return true; case VK_F12: tilde(24); return true;
    case VK_TAB: if (shift) { Send("\x1b[Z"); return true; } return false;
    }
    if (ctrl && vk == VK_SPACE) { Send(std::string(1, '\0')); return true; }
    return false;
}

LRESULT HitTestNC(HWND h, LPARAM l) {
    POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ScreenToClient(h, &pt);
    RECT rc; GetClientRect(h, &rc);
    float sc = Scale();
    if (!IsZoomed(h)) {
        int b = (int)(6 * sc);
        bool L = pt.x < b, R = pt.x >= rc.right - b, T = pt.y < b, B = pt.y >= rc.bottom - b;
        if (T && L) return HTTOPLEFT; if (T && R) return HTTOPRIGHT; if (B && L) return HTBOTTOMLEFT; if (B && R) return HTBOTTOMRIGHT;
        if (L) return HTLEFT; if (R) return HTRIGHT; if (T) return HTTOP; if (B) return HTBOTTOM;
    }
    if (pt.y / sc < TITLE_H && pt.x / sc >= 84) return HTCAPTION;
    return HTCLIENT;
}

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_NCCALCSIZE:
        if (w) {
            if (IsZoomed(h)) {
                auto* p = (NCCALCSIZE_PARAMS*)l; UINT dpi = GetDpiForWindow(h);
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
    case WM_GETMINMAXINFO: { float sc = Scale(); ((MINMAXINFO*)l)->ptMinTrackSize = { (LONG)(320 * sc), (LONG)(160 * sc) }; return 0; }
    case WM_SIZE: if (g_rt) g_rt->Resize(D2D1::SizeU(LOWORD(l), HIWORD(l))); if (g_scr.cols) ApplySize(); Invalidate(); return 0;
    case WM_DPICHANGED: { RECT* r = (RECT*)l; if (g_rt) { g_rt->Release(); g_rt = nullptr; } SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); Render(); EndPaint(h, &ps); return 0; }
    case WM_ACTIVATE: g_active = LOWORD(w) != WA_INACTIVE; Invalidate(); return 0;
    case WM_TERM_OUTPUT: {
        std::string chunk;
        { std::lock_guard<std::mutex> lk(g_outMu); chunk.swap(g_outBuf); g_outPosted = false; }
        std::wstring before = g_scr.cwd;
        size_t histBefore = g_scr.history.size();
        Feed(chunk.data(), chunk.size());
        if (!g_reply.empty()) { Send(g_reply); g_reply.clear(); }
        if (g_view > 0) g_view = std::min(HistCount(), g_view + (int)(g_scr.history.size() - histBefore));  // olhando o histórico: fica parado
        if (g_scr.cwd != before) SetWindowTextW(h, TitleText().c_str());
        Invalidate();
        return 0;
    }
    case WM_TERM_EXIT: Log(L"shell terminou"); DestroyWindow(h); return 0;  // como no Mac: fecha se o shell saiu bem
    case WM_CHAR: {
        wchar_t c = (wchar_t)w;
        if (GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_SHIFT) < 0 && c < 32) return 0;  // atalhos já tratados
        g_hasSel = false; g_view = 0;
        static wchar_t high;
        if (c >= 0xD800 && c <= 0xDBFF) { high = c; return 0; }
        std::wstring s;
        if (c >= 0xDC00 && c <= 0xDFFF && high) { s += high; s += c; high = 0; }
        else if (c == 0x08) { Send("\x7f"); Invalidate(); return 0; }  // Backspace
        else s = c;
        Send(Utf8(s));
        Invalidate();
        return 0;
    }
    case WM_SYSCHAR: { std::wstring s(1, (wchar_t)w); Send("\x1b" + Utf8(s)); return 0; }  // Alt+tecla
    case WM_KEYDOWN: case WM_SYSKEYDOWN:
        if (w == VK_F4 && GetKeyState(VK_MENU) < 0) break;
        if (SpecialKey(w)) { if (w != VK_PRIOR && w != VK_NEXT) { g_view = 0; } Invalidate(); return 0; }
        break;
    case WM_MOUSEWHEEL: {
        int lines = GET_WHEEL_DELTA_WPARAM(w) / 120 * 3;
        if (g_scr.useAlt) { for (int i = 0; i < std::abs(lines); i++) Send(lines > 0 ? (g_scr.appCursor ? "\x1bOA" : "\x1b[A") : (g_scr.appCursor ? "\x1bOB" : "\x1b[B")); return 0; }
        g_view = std::clamp(g_view + lines, 0, HistCount());
        Invalidate();
        return 0;
    }
    case WM_LBUTTONDOWN: {
        float sc = Scale(); float x = GET_X_LPARAM(l) / sc, y = GET_Y_LPARAM(l) / sc;
        if (y < TITLE_H) {
            for (int k = 0; k < 3; k++) if (std::fabs(x - (19.f + k * 23)) <= 9 && std::fabs(y - TITLE_H / 2) <= 9) {
                if (k == 0) PostMessageW(h, WM_CLOSE, 0, 0); else if (k == 1) ShowWindow(h, SW_MINIMIZE); else ampliacao::Alternar(h);
                return 0;
            }
            return 0;
        }
        g_selA = g_selB = CellAt(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        g_selecting = true; g_hasSel = false;
        SetCapture(h);
        Invalidate();
        return 0;
    }
    case WM_LBUTTONDBLCLK: {  // duas vezes: seleciona a palavra
        POINT p = CellAt(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        const Line* ln = LineAt(p.y);
        if (!ln) return 0;
        auto word = [&](int c) { if (c < 0 || c >= (int)ln->size()) return false; char32_t ch = (*ln)[c].ch; return ch != U' ' && ch != U'"' && ch != U'\'' && ch != U'(' && ch != U')'; };
        int a = std::min((int)p.x, (int)ln->size() - 1), b = a;
        if (!word(a)) return 0;
        while (word(a - 1)) a--;
        while (word(b + 1)) b++;
        g_selA = { a, p.y }; g_selB = { b + 1, p.y }; g_hasSel = true;
        Invalidate();
        return 0;
    }
    case WM_MOUSEMOVE: {
        float sc = Scale(); float x = GET_X_LPARAM(l) / sc, y = GET_Y_LPARAM(l) / sc;
        bool hl = y < TITLE_H && x < 84;
        ampliacao::Verde(h, hl && x >= 58 && x < 81);
        if (hl != g_hoverLights) { g_hoverLights = hl; Invalidate(); TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t); }
        if (g_selecting) {
            g_selB = CellAt(GET_X_LPARAM(l), GET_Y_LPARAM(l));
            g_hasSel = g_selA.x != g_selB.x || g_selA.y != g_selB.y;
            Invalidate();
        }
        return 0;
    }
    case WM_MOUSELEAVE: ampliacao::Verde(h, false); if (g_hoverLights) { g_hoverLights = false; Invalidate(); } return 0;
    case WM_LBUTTONUP: if (g_selecting) { g_selecting = false; ReleaseCapture(); } return 0;
    case WM_RBUTTONUP: { POINT p{ GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ClientToScreen(h, &p); ContextMenu(p); return 0; }
    case WM_SETCURSOR:
        if (LOWORD(l) == HTCLIENT) {
            POINT p; GetCursorPos(&p); ScreenToClient(h, &p);
            SetCursor(LoadCursorW(nullptr, p.y / Scale() >= TITLE_H ? IDC_IBEAM : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_DESTROY:
        if (g_pc) { ClosePseudoConsole(g_pc); g_pc = nullptr; }
        if (g_proc) { TerminateProcess(g_proc, 0); }
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
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
            if (color && p) { memcpy(p, bits.data(), bits.size()); HBITMAP mask = CreateBitmap(px, px, 1, 1, nullptr); ICONINFO ii{ TRUE, 0, 0, mask, color }; ic = CreateIconIndirect(&ii); DeleteObject(mask); }
            if (color) DeleteObject(color);
        }
    }
    if (cv) cv->Release(); if (scl) scl->Release(); if (fr) fr->Release(); if (dec) dec->Release();
    wic->Release();
    return ic;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) {
    g_inst = hi;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH); PathRemoveFileSpecW(exe);
    std::wstring bin = exe;
    g_log = _wfsopen((bin + L"\\terminal.log").c_str(), L"a, ccs=UTF-8", _SH_DENYNO);
    ctxmenu::Init(hi, g_log);
    int argc; wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::wstring start;
    for (int a = 1; a < argc; a++) {
        if (!_wcsicmp(argv[a], L"--cmd")) g_isCmd = true;
        else if (!_wcsicmp(argv[a], L"--pasta") && a + 1 < argc) start = argv[++a];
        else if (!_wcsicmp(argv[a], L"--executar") && a + 1 < argc) g_run = argv[++a];
        else if (argv[a][0] != L'-') start = argv[a];
    }
    LocalFree(argv);
    while (!start.empty() && start.back() == L'"') start.pop_back();
    if (start.empty()) { PWSTR p = nullptr; if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &p))) { start = p; CoTaskMemFree(p); } }
    g_scr.cwd = start;

    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_f);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&g_dw);
    MakeFonts();

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.style = CS_DBLCLKS; wc.lpfnWndProc = WndProc; wc.hInstance = hi; wc.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
    wc.lpszClassName = L"TerminalMacJanela";
    std::wstring png = bin.substr(0, bin.find_last_of(L'\\')) + L"\\icones\\sequoia\\Terminal.png";
    wc.hIcon = PackIcon(png, 48); wc.hIconSm = PackIcon(png, 16);
    RegisterClassExW(&wc);
    // 80×24, como o Terminal abre, em cascata se já houver outra janela
    POINT cur; GetCursorPos(&cur);
    HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(mon, &mi);
    UINT dx = 96, dy = 96; GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    float sc = dx / 96.f;
    int w = (int)std::ceil((80 * g_cw + 2 * PAD_X + 6) * sc), hgt = (int)std::ceil((24 * g_ch + TITLE_H + 2 * PAD_Y) * sc);
    int x = (mi.rcWork.left + mi.rcWork.right - w) / 2, y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - hgt) / 3;
    int others = 0;
    for (HWND o = FindWindowW(L"TerminalMacJanela", nullptr); o; o = FindWindowExW(nullptr, o, L"TerminalMacJanela", nullptr)) others++;
    x += others * (int)(22 * sc); y += others * (int)(22 * sc);
    g_scr.Init(80, 24);
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"Terminal", WS_OVERLAPPEDWINDOW, x, y, w, hgt, nullptr, nullptr, hi, nullptr);
    ampliacao::Janela(g_hwnd);  // abrir e fechar animados (o Shell anima)
    BOOL cloak = TRUE; DwmSetWindowAttribute(g_hwnd, 13, &cloak, sizeof(cloak));
    BOOL dm = TRUE; DwmSetWindowAttribute(g_hwnd, 20, &dm, sizeof(dm));
    COLORREF border = RGB(88, 88, 92); DwmSetWindowAttribute(g_hwnd, 34, &border, sizeof(border));
    MARGINS mg{ 0, 0, 1, 0 }; DwmExtendFrameIntoClientArea(g_hwnd, &mg);
    SetWindowPos(g_hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER);
    ApplySize();
    if (!StartShell(start, g_scr.cols, g_scr.rows)) { MessageBoxW(g_hwnd, Tr(L"Não foi possível abrir o shell."), Tr(L"Terminal"), MB_ICONERROR); return 1; }
    SetWindowTextW(g_hwnd, TitleText().c_str());
    ShowWindow(g_hwnd, show == SW_SHOWMINIMIZED ? SW_SHOWMINIMIZED : SW_SHOWNORMAL);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (g_log) fclose(g_log);
    return 0;
}
