// LiquinixTitleBars.exe: só a barra de título do Mac nas janelas dos outros programas, sem o resto do Liquinix (sem
// barra de menus, Dock, mesa; a barra de tarefas do Windows fica). Para quem tem o próprio visual e quer só as
// barras (Reddit, 20/11/2026). O trabalho é o mesmo do Shell: src\shell\janelas.cpp (gancho por thread com a
// LiquinixWindow.dll, os de 32 bits pelo rundll32 do Windows), compilado junto com este arquivo.
//   LiquinixTitleBars.exe          liga (uma instância só; com o Shell aberto não faz nada: ele já põe as barras)
//   LiquinixTitleBars.exe --sair   desliga a que estiver rodando (as janelas voltam ao visual do Windows)
// O Shell, ao abrir, fecha esta (janelas.cpp) e assume. Exclusões: %APPDATA%\Shell\janelas-excluidas.txt.
// Ajustes lidos: janelas_mac (0 desliga) e o tema (a DLL lê o ajustes.txt sozinha).
#include "../shell/shell.h"
#include <shlobj.h>
#include <cstdarg>

std::wstring shell::dir;
namespace {
FILE* g_log;
HWND g_wnd;
}

void Log(const wchar_t* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fwprintf(g_log, L"%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a; va_start(a, fmt); vfwprintf(g_log, fmt, a); va_end(a);
    fwprintf(g_log, L"\n");
    fflush(g_log);
}

// %APPDATA%\Shell\ajustes.txt (o mesmo do Shell), só leitura
std::wstring shell::Setting(const wchar_t* key, const wchar_t* def) {
    PWSTR p = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) return def;
    std::wstring f = std::wstring(p) + L"\\Shell\\ajustes.txt";
    CoTaskMemFree(p);
    FILE* fp = nullptr;
    if (_wfopen_s(&fp, f.c_str(), L"r, ccs=UTF-8") || !fp) return def;
    std::wstring want = std::wstring(key) + L"=", val = def;
    wchar_t line[1024];
    while (fgetws(line, 1024, fp)) {
        std::wstring s = line;
        while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
        if (s.rfind(want, 0) == 0) val = s.substr(want.size());
    }
    fclose(fp);
    return val;
}

namespace {

void CALLBACK OnForeground(HWINEVENTHOOK, DWORD, HWND h, LONG obj, LONG, DWORD, DWORD) {
    if (obj == OBJID_WINDOW && h) janelas::Foreground(h);
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_CLOSE) { DestroyWindow(h); return 0; }
    if (m == WM_ENDSESSION && w) { janelas::Stop(); return 0; }
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, PWSTR cmd, int) {
    if (cmd && wcsstr(cmd, L"--sair")) {
        if (HWND h = FindWindowW(L"LiquinixBarrasDeTitulo", nullptr)) PostMessageW(h, WM_CLOSE, 0, 0);
        return 0;
    }
    HANDLE one = CreateMutexW(nullptr, TRUE, L"LiquinixBarrasDeTituloUmaInstancia");
    if (GetLastError() == ERROR_ALREADY_EXISTS) return 0;
    if (FindWindowW(L"ShellMensagens", nullptr)) return 0;  // o Shell aberto já põe as barras
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    shell::dir = exe; shell::dir = shell::dir.substr(0, shell::dir.find_last_of(L'\\'));
    g_log = _wfsopen((shell::dir + L"\\barras-titulo.log").c_str(), L"w, ccs=UTF-8", _SH_DENYNO);
    Log(L"LiquinixTitleBars iniciando (só as barras de título, sem o Shell)");
    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = hi; wc.lpszClassName = L"LiquinixBarrasDeTitulo";
    RegisterClassW(&wc);
    // janela comum escondida (não "message-only"): o Shell e o --sair a acham pelo FindWindow
    g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"Liquinix Title Bars", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, hi, nullptr);
    if (!g_wnd) return 1;
    if (shell::Setting(L"janelas_mac", L"1") != L"1") Log(L"janelas_mac=0 no ajustes.txt: nada a fazer");
    janelas::Start();
    SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnForeground, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    janelas::Stop();  // as janelas voltam ao visual do Windows
    Log(L"LiquinixTitleBars saiu");
    if (g_log) fclose(g_log);
    ReleaseMutex(one);
    return 0;
}
