// MenuTeste — bancada do menu de contexto próprio (src/menu). Roda ao lado do Explorer.
//
//   MenuTeste.exe [pasta]                 abre a bancada de teste
//   MenuTeste.exe --dump <caminho> [--bg] grava a árvore do menu em menu-teste.log e sai
//   MenuTeste.exe --demo <pasta>          abre o menu sozinho (captura de tela) e sai

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cstdarg>
#include <share.h>
#include "../menu/context_menu.h"

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

static HINSTANCE g_inst;
static FILE* g_log;
static bool g_demo;

static void Log(const wchar_t* fmt, ...) {
    if (!g_log) return;
    va_list a; va_start(a, fmt); vfwprintf(g_log, fmt, a); va_end(a);
    fputwc(L'\n', g_log); fflush(g_log);
}

struct Entry { std::wstring name, path; bool dir; HICON icon; };

static HWND g_main;
static std::wstring g_folder, g_status = L"Pronto.";
static std::vector<Entry> g_entries;
static int g_scroll = 0, g_sel = -1;
static HFONT g_fMain, g_fBold;
static float g_ms = 1;
static HWND g_edit; static int g_editRow = -1; static WNDPROC g_editOld;

static int S(float v) { return (int)(v * g_ms + 0.5f); }
static int HeaderH() { return S(62); }
static int RowH() { return S(26); }
static int StatusH() { return S(26); }

static void MakeFonts(HWND h) {
    if (g_fMain) DeleteObject(g_fMain);
    if (g_fBold) DeleteObject(g_fBold);
    UINT dpi = GetDpiForWindow(h);
    g_ms = dpi / 96.f;
    g_fMain = CreateFontW(-MulDiv(9, dpi, 72), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_fBold = CreateFontW(-MulDiv(10, dpi, 72), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
}

static void LoadFolder(const std::wstring& folder, const std::wstring& selectName = L"") {
    for (auto& e : g_entries) if (e.icon) DestroyIcon(e.icon);
    g_entries.clear();
    g_folder = folder;
    while (g_folder.size() > 3 && g_folder.back() == L'\\') g_folder.pop_back();
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((g_folder + L"\\*").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) continue;
            std::wstring p = g_folder + (g_folder.back() == L'\\' ? L"" : L"\\") + fd.cFileName;
            g_entries.push_back({ fd.cFileName, p, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, nullptr });
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    std::sort(g_entries.begin(), g_entries.end(), [](const Entry& a, const Entry& b) {
        if (a.dir != b.dir) return a.dir;
        return StrCmpLogicalW(a.name.c_str(), b.name.c_str()) < 0;
    });
    for (size_t i = 0; i < g_entries.size() && i < 500; i++) {
        SHFILEINFOW sfi{};
        if (SHGetFileInfoW(g_entries[i].path.c_str(), 0, &sfi, sizeof(sfi), SHGFI_ICON | SHGFI_SMALLICON)) g_entries[i].icon = sfi.hIcon;
    }
    g_sel = -1; g_scroll = 0;
    for (size_t i = 0; i < g_entries.size(); i++) if (!selectName.empty() && !_wcsicmp(g_entries[i].name.c_str(), selectName.c_str())) g_sel = (int)i;
    if (g_main) InvalidateRect(g_main, nullptr, FALSE);
}

static void Reload() {
    std::wstring keep = g_sel >= 0 && g_sel < (int)g_entries.size() ? g_entries[g_sel].name : L"";
    int sc = g_scroll;
    LoadFolder(g_folder, keep);
    g_scroll = std::min(sc, std::max(0, (int)g_entries.size() - 1));
}

static int VisibleRows(HWND h) { RECT rc; GetClientRect(h, &rc); return std::max<int>(1, (rc.bottom - HeaderH() - StatusH()) / RowH()); }

static int RowAt(HWND h, int y) {
    RECT rc; GetClientRect(h, &rc);
    if (y < HeaderH() || y >= rc.bottom - StatusH()) return -1;
    int i = (y - HeaderH()) / RowH() + g_scroll;
    return i < (int)g_entries.size() ? i : -1;
}

static void EnsureVisible(HWND h) {
    if (g_sel < 0) return;
    int vis = VisibleRows(h);
    if (g_sel < g_scroll) g_scroll = g_sel;
    if (g_sel >= g_scroll + vis) g_scroll = g_sel - vis + 1;
}

static void SetStatus(const std::wstring& s) { g_status = s; Log(L"%ls", s.c_str()); InvalidateRect(g_main, nullptr, FALSE); }

static void PaintMain(HWND h) {
    PAINTSTRUCT ps; HDC hdc = BeginPaint(h, &ps);
    RECT rc; GetClientRect(h, &rc);
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    FillRect(mem, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
    SetBkMode(mem, TRANSPARENT);

    HGDIOBJ oldFont = SelectObject(mem, g_fBold);
    SetTextColor(mem, RGB(30, 30, 30));
    RECT r{ S(14), S(8), rc.right - S(14), S(32) };
    DrawTextW(mem, g_folder.c_str(), -1, &r, DT_SINGLELINE | DT_VCENTER | DT_PATH_ELLIPSIS | DT_NOPREFIX);
    SelectObject(mem, g_fMain);
    SetTextColor(mem, RGB(120, 120, 120));
    r = { S(14), S(32), rc.right - S(14), S(54) };
    DrawTextW(mem, L"Clique direito: menu novo  ·  Alt+clique direito: com itens extras  ·  Shift+clique direito: todas as opções  ·  Duplo clique: abrir  ·  Backspace: voltar  ·  Arraste uma pasta para cá",
              -1, &r, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    RECT line{ 0, HeaderH() - 1, rc.right, HeaderH() };
    HBRUSH lb = CreateSolidBrush(RGB(228, 228, 228)); FillRect(mem, &line, lb);

    int rowH = RowH(), y = HeaderH();
    HBRUSH selB = CreateSolidBrush(RGB(214, 232, 255));
    for (int i = g_scroll; i < (int)g_entries.size() && y < rc.bottom - StatusH(); i++, y += rowH) {
        const Entry& e = g_entries[i];
        if (i == g_sel) { RECT sr{ S(6), y + 1, rc.right - S(6), y + rowH - 1 }; FillRect(mem, &sr, selB); }
        if (e.icon) DrawIconEx(mem, S(14), y + (rowH - S(16)) / 2, e.icon, S(16), S(16), 0, nullptr, DI_NORMAL);
        if (g_edit && i == g_editRow) continue;
        SetTextColor(mem, RGB(30, 30, 30));
        RECT tr{ S(40), y, rc.right - S(14), y + rowH };
        DrawTextW(mem, e.name.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    if (g_entries.empty()) {
        SetTextColor(mem, RGB(150, 150, 150));
        RECT er{ S(14), HeaderH() + S(10), rc.right, HeaderH() + S(40) };
        DrawTextW(mem, L"(pasta vazia — clique direito aqui testa o menu do fundo da pasta)", -1, &er, DT_SINGLELINE | DT_NOPREFIX);
    }
    RECT st{ 0, rc.bottom - StatusH(), rc.right, rc.bottom };
    HBRUSH sb = CreateSolidBrush(RGB(245, 245, 245)); FillRect(mem, &st, sb);
    SetTextColor(mem, RGB(90, 90, 90));
    st.left += S(14); st.right -= S(14);
    DrawTextW(mem, g_status.c_str(), -1, &st, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

    BitBlt(hdc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldFont); SelectObject(mem, oldBmp);
    DeleteObject(bmp); DeleteObject(lb); DeleteObject(selB); DeleteObject(sb); DeleteDC(mem);
    EndPaint(h, &ps);
}

// Renomear exige a "view" do Explorer; aqui fazemos a edição no lugar nós mesmos.
static LRESULT CALLBACK EditProc(HWND e, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_KEYDOWN && (w == VK_RETURN || w == VK_ESCAPE)) { PostMessageW(GetParent(e), WM_APP + 2, w == VK_RETURN, 0); return 0; }
    if (m == WM_CHAR && (w == L'\r' || w == 27)) return 0;
    if (m == WM_KILLFOCUS) PostMessageW(GetParent(e), WM_APP + 2, 1, 0);
    return CallWindowProcW(g_editOld, e, m, w, l);
}

static void StartRename(HWND h, int row) {
    if (row < 0) return;
    g_sel = row; EnsureVisible(h);
    RECT rc; GetClientRect(h, &rc);
    int y = HeaderH() + (row - g_scroll) * RowH();
    g_editRow = row;
    g_edit = CreateWindowExW(0, L"EDIT", g_entries[row].name.c_str(), WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                             S(36), y + S(2), rc.right - S(50), RowH() - S(4), h, nullptr, g_inst, nullptr);
    SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_fMain, TRUE);
    g_editOld = (WNDPROC)SetWindowLongPtrW(g_edit, GWLP_WNDPROC, (LONG_PTR)EditProc);
    const std::wstring& n = g_entries[row].name;
    size_t dot = n.rfind(L'.');
    SendMessageW(g_edit, EM_SETSEL, 0, (!g_entries[row].dir && dot != std::wstring::npos && dot > 0) ? (LPARAM)dot : -1);
    SetFocus(g_edit);
    InvalidateRect(h, nullptr, FALSE);
}

static void EndRename(bool ok) {
    if (!g_edit) return;
    HWND e = g_edit; g_edit = nullptr;
    wchar_t buf[MAX_PATH] = {}; GetWindowTextW(e, buf, MAX_PATH);
    SetFocus(g_main);
    DestroyWindow(e);
    int row = g_editRow; g_editRow = -1;
    if (ok && row >= 0 && row < (int)g_entries.size() && buf[0] && g_entries[row].name != buf) {
        std::wstring to = g_folder + L"\\" + buf;
        if (MoveFileW(g_entries[row].path.c_str(), to.c_str())) { SetStatus(L"Renomeado para " + std::wstring(buf)); LoadFolder(g_folder, buf); return; }
        SetStatus(L"Falha ao renomear (erro " + std::to_wstring(GetLastError()) + L")");
    }
    InvalidateRect(g_main, nullptr, FALSE);
}

// full = "Mostrar todas as opções" (Shift+clique direito): tudo, inclusive os itens estendidos.
static void ShowContext(HWND h, POINT pt, int row, bool full) {
    std::vector<std::wstring> paths;
    if (row >= 0) paths.push_back(g_entries[row].path);
    auto r = ctxmenu::Show(h, pt, g_folder, paths, full);
    if (FAILED(r.hr) && !r.chosen) { SetStatus(L"Não consegui obter o menu"); return; }
    if (!r.chosen) return;
    if (r.rename) { StartRename(h, row); SetStatus(L"Executado: rename (edição no lugar)"); return; }
    wchar_t msg[256];
    swprintf_s(msg, L"Executado: %ls → %ls 0x%08X", r.verb.empty() ? L"(sem verbo)" : r.verb.c_str(),
               SUCCEEDED(r.hr) ? L"OK" : L"erro", (unsigned)r.hr);
    SetStatus(msg);
    SetTimer(h, 7, 700, nullptr);   // atualiza a lista depois de copiar/colar/excluir
    SetTimer(h, 8, 2500, nullptr);
}

static void OpenEntry(int i) {
    if (i < 0 || i >= (int)g_entries.size()) return;
    if (g_entries[i].dir) { LoadFolder(g_entries[i].path); SetStatus(L"Pasta: " + g_folder); }
    else ShellExecuteW(g_main, nullptr, g_entries[i].path.c_str(), nullptr, g_folder.c_str(), SW_SHOWNORMAL);
}

static LRESULT CALLBACK MainProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_INITMENUPOPUP: case WM_DRAWITEM: case WM_MEASUREITEM: case WM_MENUCHAR: {
        LRESULT r = 0;
        if (ctxmenu::ForwardOwnerMsg(m, w, l, &r)) return r;
        break;
    }
    case WM_ACTIVATEAPP:
        if (!w) ctxmenu::Close();
        if (w && !ctxmenu::IsOpen() && !g_edit) Reload();
        break;
    case WM_PAINT: PaintMain(h); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_DPICHANGED: {
        MakeFonts(h);
        RECT* r = (RECT*)l;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        SetFocus(h);
        int i = RowAt(h, GET_Y_LPARAM(l));
        if (i != g_sel) { g_sel = i; InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_LBUTTONDBLCLK: OpenEntry(RowAt(h, GET_Y_LPARAM(l))); return 0;
    case WM_CONTEXTMENU: {
        POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
        int row;
        if (pt.x == -1 && pt.y == -1) {  // tecla de menu / Shift+F10
            row = g_sel;
            RECT rc; GetClientRect(h, &rc);
            pt = { S(60), row >= 0 ? HeaderH() + (row - g_scroll) * RowH() + RowH() : HeaderH() + S(10) };
            ClientToScreen(h, &pt);
        } else {
            POINT c = pt; ScreenToClient(h, &c);
            row = RowAt(h, c.y);
        }
        g_sel = row; InvalidateRect(h, nullptr, FALSE); UpdateWindow(h);
        ShowContext(h, pt, row, GetKeyState(VK_SHIFT) < 0);
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int vis = VisibleRows(h);
        g_scroll -= GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA * 3;
        g_scroll = std::clamp(g_scroll, 0, std::max(0, (int)g_entries.size() - vis));
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
        switch (w) {
        case VK_BACK: {
            std::wstring p = g_folder, child;
            size_t k = p.find_last_of(L'\\');
            if (k != std::wstring::npos && p.size() > 3) { child = p.substr(k + 1); p = p.substr(0, k); if (p.size() == 2) p += L"\\"; LoadFolder(p, child); }
            return 0;
        }
        case VK_F5: Reload(); return 0;
        case VK_F2: StartRename(h, g_sel); return 0;
        case VK_RETURN: OpenEntry(g_sel); return 0;
        case VK_DOWN: if (g_sel + 1 < (int)g_entries.size()) g_sel++; EnsureVisible(h); InvalidateRect(h, nullptr, FALSE); return 0;
        case VK_UP: if (g_sel > 0) g_sel--; EnsureVisible(h); InvalidateRect(h, nullptr, FALSE); return 0;
        }
        break;
    case WM_DROPFILES: {
        wchar_t p[MAX_PATH] = {};
        DragQueryFileW((HDROP)w, 0, p, MAX_PATH);
        DragFinish((HDROP)w);
        DWORD a = GetFileAttributesW(p);
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) LoadFolder(p);
        else { std::wstring s = p; size_t k = s.find_last_of(L'\\'); if (k != std::wstring::npos) LoadFolder(s.substr(0, k), s.substr(k + 1)); }
        return 0;
    }
    case WM_TIMER:
        if (w == 7 || w == 8) { KillTimer(h, w); if (!g_edit) Reload(); }
        if (w == 9) {  // --demo: abre o menu da primeira linha, fecha sozinho e encerra
            KillTimer(h, 9);
            POINT pt{ S(120), HeaderH() + RowH() / 2 }; ClientToScreen(h, &pt);
            g_sel = g_entries.empty() ? -1 : 0;
            SetTimer(h, 10, 1500, nullptr);
            ShowContext(h, pt, g_sel, false);
            DestroyWindow(h);
        }
        if (w == 10) { KillTimer(h, 10); ctxmenu::Close(); }
        return 0;
    case WM_APP + 2: EndRename(w != 0); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR, int show) {
    g_inst = hi;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    OleInitialize(nullptr);

    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    PathRemoveFileSpecW(exe);
    std::wstring logPath = std::wstring(exe) + L"\\menu-teste.log";
    g_log = _wfsopen(logPath.c_str(), L"a, ccs=UTF-8", _SH_DENYNO);  // legível com o app aberto

    int argc; wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool dump = argc >= 3 && !wcscmp(argv[1], L"--dump");
    bool dumpBg = dump && argc >= 4 && !wcscmp(argv[3], L"--bg");
    g_demo = argc >= 3 && !wcscmp(argv[1], L"--demo");
    ctxmenu::Options opt;
    opt.demo = g_demo;
    if (g_demo) opt.fadeMs = 1500;  // fade lento para dar tempo de capturar a tela
    ctxmenu::Init(hi, g_log, opt);
    std::wstring start;
    if (dump || g_demo) start = argv[2];
    else if (argc >= 2) start = argv[1];
    else { PWSTR p; if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &p))) { start = p; CoTaskMemFree(p); } }

    SYSTEMTIME t; GetLocalTime(&t);
    Log(L"\n===== %04d-%02d-%02d %02d:%02d:%02d  %ls", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
        dump ? L"(dump)" : L"");

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.hInstance = hi; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpfnWndProc = MainProc; wc.lpszClassName = L"MenuTesteMain"; wc.style = CS_DBLCLKS;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    RegisterClassExW(&wc);

    g_main = CreateWindowExW(WS_EX_ACCEPTFILES, L"MenuTesteMain", L"MenuTeste — bancada do menu de contexto",
                             WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 820, 560, nullptr, nullptr, hi, nullptr);
    MakeFonts(g_main);
    {   // tamanho inicial proporcional ao DPI
        RECT r; GetWindowRect(g_main, &r);
        SetWindowPos(g_main, nullptr, 0, 0, S(820), S(560), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    if (dump) {
        int rc = ctxmenu::DumpToLog(g_main, start, dumpBg) ? 0 : 1;
        if (g_log) fclose(g_log);
        return rc;
    }

    LoadFolder(start);
    ShowWindow(g_main, show);
    UpdateWindow(g_main);
    if (g_demo) {
        SetForegroundWindow(g_main);
        // --quente: pré-carrega as extensões como o Finder faz ao abrir, para medir o menu "de verdade"
        for (int a = 3; a < argc; a++)
            if (!wcscmp(argv[a], L"--quente") && !g_entries.empty()) ctxmenu::Warmup(g_main, g_folder, g_entries[0].path);
        SetTimer(g_main, 9, 800, nullptr);
    }
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    if (g_log) fclose(g_log);
    OleUninitialize();
    return 0;
}
