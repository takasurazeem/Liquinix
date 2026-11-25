// LiquinixSetup.exe: instalar, atualizar, modificar e desinstalar o Liquinix num programa só, no visual do Instalador do
// macOS (barra lateral com as etapas, painel de conteúdo, "Voltar" / "Continuar"). Tudo nativo: copia os arquivos,
// atalhos (IShellLink), tarefa de login (Agendador de Tarefas por COM), fontes, cursor, Música e QuickTime em
// "Abrir com", e se registra em Aplicativos instalados do Windows (HKCU, desinstalar por lá abre esta janela).
// Nenhum PowerShell escondido (é o padrão que os antivírus acusam): a única exceção é desfazer os ícones do sistema,
// que roda o tools\icones-sistema.ps1 numa janela visível, e só se eles estiverem aplicados.
//   LiquinixSetup.exe                 instalar (ou, já instalado: atualizar / modificar / desinstalar)
//   LiquinixSetup.exe --atualizar     procura a versão nova no GitHub (Ajustes > Geral > Atualização de Software)
//   LiquinixSetup.exe --desinstalar   desinstalar (Aplicativos instalados do Windows)
//   LiquinixSetup.exe --foto <pasta>  desenha cada tela num PNG, claro e escuro, sem instalar nada (conferir o visual)
//   LiquinixSetup.exe --diagnostico <arquivo>  consulta o GitHub, baixa, confere o SHA-256 e extrai num temporário
// Mesmas regras do instalador antigo (instalador\instalar.ps1 / desinstalar.ps1): instalacao.log com o que muda,
// modo.txt com o que foi instalado, ajustes do usuário (%APPDATA%\Shell) nunca trocados.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <taskschd.h>
#include <tlhelp32.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <shlwapi.h>
#include <shellscalingapi.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <functional>
#include <cmath>
#include <ctime>
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shcore.lib")

using std::wstring;

// ============================================================ textos (português ou inglês, pelo idioma do Windows)
static bool g_pt;
static const wchar_t* T(const wchar_t* pt, const wchar_t* en) { return g_pt ? pt : en; }

// ============================================================ caminhos e utilidades
static wstring g_self, g_selfDir, g_src, g_dest, g_log;
static wstring Env(const wchar_t* n) { wchar_t b[MAX_PATH * 2]; DWORD k = GetEnvironmentVariableW(n, b, MAX_PATH * 2); return k ? wstring(b, k) : L""; }
static bool Exists(const wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
static bool IsDir(const wstring& p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
static wstring Parent(const wstring& p) { size_t s = p.find_last_of(L'\\'); return s == wstring::npos ? p : p.substr(0, s); }
static wstring Leaf(const wstring& p) { size_t s = p.find_last_of(L'\\'); return s == wstring::npos ? p : p.substr(s + 1); }
static wstring KnownFolder(REFKNOWNFOLDERID id) { PWSTR p = nullptr; wstring r; if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) r = p; CoTaskMemFree(p); return r; }
static wstring Lower(wstring s) { for (auto& c : s) c = towlower(c); return s; }
static void MkDirs(const wstring& p) { SHCreateDirectoryExW(nullptr, p.c_str(), nullptr); }
static wstring ExtOf(const wstring& p) { size_t d = p.find_last_of(L'.'), s = p.find_last_of(L'\\'); return d == wstring::npos || (s != wstring::npos && d < s) ? L"" : p.substr(d); }

static std::string ToUtf8(const wstring& w) { if (w.empty()) return {}; int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr); std::string s(n, 0); WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr); return s; }
static wstring FromUtf8(const std::string& s) { if (s.empty()) return {}; int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0); wstring w(n, 0); MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n); return w; }

static wstring ReadText(const wstring& path) {  // UTF-8 (com ou sem BOM) ou UTF-16
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return L"";
    LARGE_INTEGER sz{}; GetFileSizeEx(f, &sz);
    std::string b((size_t)std::min<LONGLONG>(sz.QuadPart, 64ll << 20), 0); DWORD got = 0;
    if (!b.empty()) ReadFile(f, b.data(), (DWORD)b.size(), &got, nullptr);
    CloseHandle(f); b.resize(got);
    if (b.size() >= 2 && (unsigned char)b[0] == 0xFF && (unsigned char)b[1] == 0xFE) return wstring((const wchar_t*)(b.data() + 2), (b.size() - 2) / 2);
    if (b.size() >= 3 && (unsigned char)b[0] == 0xEF && (unsigned char)b[1] == 0xBB && (unsigned char)b[2] == 0xBF) b.erase(0, 3);
    return FromUtf8(b);
}
static bool WriteText(const wstring& path, const wstring& text) {  // UTF-8 com BOM (como o Set-Content -Encoding utf8)
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    std::string s = "\xEF\xBB\xBF" + ToUtf8(text); DWORD w = 0;
    WriteFile(f, s.data(), (DWORD)s.size(), &w, nullptr); CloseHandle(f); return w == s.size();
}
static std::vector<wstring> Lines(const wstring& t) {
    std::vector<wstring> r; wstring cur;
    for (wchar_t c : t) { if (c == L'\n') { if (!cur.empty() && cur.back() == L'\r') cur.pop_back(); r.push_back(cur); cur.clear(); } else cur += c; }
    if (!cur.empty()) r.push_back(cur);
    return r;
}
static wstring Trim(wstring s) { while (!s.empty() && iswspace(s.back())) s.pop_back(); size_t i = 0; while (i < s.size() && iswspace(s[i])) i++; return s.substr(i); }

static void Anota(const wstring& line) {  // instalacao.log, o mesmo formato do instalar.ps1 (o desinstalar lê)
    if (g_log.empty()) return;
    bool nova = !Exists(g_log);
    HANDLE f = CreateFileW(g_log.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t ts[32]; swprintf_s(ts, L"%04d-%02d-%02d %02d:%02d:%02d ", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    std::string s = (nova ? "\xEF\xBB\xBF" : "") + ToUtf8(ts + line + L"\r\n"); DWORD w;
    WriteFile(f, s.data(), (DWORD)s.size(), &w, nullptr); CloseHandle(f);
}

static wstring VersionOf(const wstring& root) {  // bin\versao.txt, linha 1 (build-N)
    auto l = Lines(ReadText(root + L"\\bin\\versao.txt"));
    return l.empty() ? L"" : Trim(l[0]);
}

// ============================================================ processos
static std::vector<DWORD> Pids(const wchar_t* exe) {
    std::vector<DWORD> r;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return r;
    PROCESSENTRY32W e{ sizeof(e) };
    for (BOOL ok = Process32FirstW(s, &e); ok; ok = Process32NextW(s, &e)) if (!_wcsicmp(e.szExeFile, exe)) r.push_back(e.th32ProcessID);
    CloseHandle(s); return r;
}
static bool Running(const wchar_t* exe) { return !Pids(exe).empty(); }
static void Kill(const wchar_t* exe) {
    for (DWORD pid : Pids(exe)) if (HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid)) { TerminateProcess(h, 1); WaitForSingleObject(h, 3000); CloseHandle(h); }
}
static DWORD RunWait(const wstring& exe, const wstring& args, DWORD ms = 60000, bool hide = true) {
    wstring cmd = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si{ sizeof(si) }; PROCESS_INFORMATION pi{};
    if (hide) { si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE; }
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, hide ? CREATE_NO_WINDOW : 0, nullptr, Parent(exe).c_str(), &si, &pi)) return (DWORD)-1;
    WaitForSingleObject(pi.hProcess, ms);
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess); return code;
}
static void Launch(const wstring& exe, const wstring& args = L"") {
    ShellExecuteW(nullptr, L"open", exe.c_str(), args.empty() ? nullptr : args.c_str(), Parent(exe).c_str(), SW_SHOWNORMAL);
}
static bool WaitGone(const wchar_t* exe, int ms) { for (int t = 0; t < ms; t += 250) { if (!Running(exe)) return true; Sleep(250); } return !Running(exe); }

// o LiquinixShell.exe que tem o modo ajudante (--sair, --aplicar-cursor...): os antigos abririam o Shell
static bool HasHelper(const wstring& exe) {
    HANDLE f = CreateFileW(exe.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{}; GetFileSizeEx(f, &sz);
    std::vector<char> b((size_t)std::min<LONGLONG>(sz.QuadPart, 64ll << 20)); DWORD got = 0;
    ReadFile(f, b.data(), (DWORD)b.size(), &got, nullptr); CloseHandle(f);
    static const wchar_t key[] = L"--desfazer-janelas";
    const char* k = (const char*)key; size_t kn = (wcslen(key)) * 2;
    return std::search(b.begin(), b.begin() + got, k, k + kn) != b.begin() + got;
}
static wstring Helper() {
    for (auto& e : { g_dest + L"\\bin\\LiquinixShell.exe", g_src + L"\\bin\\LiquinixShell.exe" })
        if (Exists(e) && HasHelper(e)) {
            if (e.rfind(g_dest, 0) == 0 && !g_dest.empty()) {  // o da instalação pode sair junto com a pasta: usa uma cópia
                wstring c = Env(L"TEMP") + L"\\LiquinixShell-ajudante.exe";
                if (CopyFileW(e.c_str(), c.c_str(), FALSE)) return c;
            }
            return e;
        }
    return L"";
}

// ============================================================ registro
static bool RegSetStr(HKEY root, const wstring& key, const wchar_t* name, const wstring& v) {
    HKEY k; if (RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr)) return false;
    LSTATUS r = RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * 2)); RegCloseKey(k); return r == 0;
}
static bool RegSetDword(HKEY root, const wstring& key, const wchar_t* name, DWORD v) {
    HKEY k; if (RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr)) return false;
    LSTATUS r = RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, 4); RegCloseKey(k); return r == 0;
}
static void RegSetNone(HKEY root, const wstring& key, const wchar_t* name) {
    HKEY k; if (RegCreateKeyExW(root, key.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &k, nullptr)) return;
    RegSetValueExW(k, name, 0, REG_NONE, nullptr, 0); RegCloseKey(k);
}
static wstring RegGetStr(HKEY root, const wstring& key, const wchar_t* name) {
    wchar_t b[2048]; DWORD n = sizeof(b);
    if (RegGetValueW(root, key.c_str(), name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, nullptr, b, &n)) return L"";
    return b;
}
static void RegDelValue(HKEY root, const wstring& key, const wchar_t* name) { HKEY k; if (!RegOpenKeyExW(root, key.c_str(), 0, KEY_SET_VALUE, &k)) { RegDeleteValueW(k, name); RegCloseKey(k); } }
static void RegDelTree(HKEY root, const wstring& key) { RegDeleteTreeW(root, key.c_str()); RegDeleteKeyW(root, key.c_str()); }
// "HKCU:\Software\X" (como o PowerShell escreve no log) -> HKEY_CURRENT_USER + "Software\X"
static bool PsKey(const wstring& ps, HKEY& root, wstring& sub) {
    if (_wcsnicmp(ps.c_str(), L"HKCU:\\", 6) == 0) { root = HKEY_CURRENT_USER; sub = ps.substr(6); return true; }
    if (_wcsnicmp(ps.c_str(), L"HKLM:\\", 6) == 0) { root = HKEY_LOCAL_MACHINE; sub = ps.substr(6); return true; }
    return false;
}

// ============================================================ atalhos e tarefa agendada
static bool Shortcut(const wstring& lnk, const wstring& target, const wstring& desc = L"") {
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return false;
    sl->SetPath(target.c_str()); sl->SetWorkingDirectory(Parent(target).c_str()); sl->SetIconLocation(target.c_str(), 0);
    if (!desc.empty()) sl->SetDescription(desc.c_str());
    IPersistFile* pf = nullptr; bool ok = false;
    if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) { ok = SUCCEEDED(pf->Save(lnk.c_str(), TRUE)); pf->Release(); }
    sl->Release(); return ok;
}

static ITaskFolder* TaskRoot(ITaskService** svcOut) {
    ITaskService* svc = nullptr;
    if (FAILED(CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&svc)))) return nullptr;
    VARIANT e{}; VariantInit(&e);
    if (FAILED(svc->Connect(e, e, e, e))) { svc->Release(); return nullptr; }
    ITaskFolder* root = nullptr; BSTR p = SysAllocString(L"\\");
    svc->GetFolder(p, &root); SysFreeString(p);
    *svcOut = svc; return root;
}
static bool TaskExists(const wchar_t* name) {
    ITaskService* svc = nullptr; ITaskFolder* root = TaskRoot(&svc); if (!root) return false;
    IRegisteredTask* t = nullptr; BSTR n = SysAllocString(name);
    bool ok = SUCCEEDED(root->GetTask(n, &t)); if (t) t->Release(); SysFreeString(n); root->Release(); svc->Release(); return ok;
}
static void TaskDelete(const wchar_t* name) {
    ITaskService* svc = nullptr; ITaskFolder* root = TaskRoot(&svc); if (!root) return;
    BSTR n = SysAllocString(name); root->DeleteTask(n, 0); SysFreeString(n); root->Release(); svc->Release();
}
// ao entrar no Windows, só este usuário, sem administrador, sem limite de tempo (como o Register-ScheduledTask do ps1)
static bool TaskLogon(const wchar_t* name, const wstring& exe, const wchar_t* desc) {
    ITaskService* svc = nullptr; ITaskFolder* root = TaskRoot(&svc); if (!root) return false;
    bool ok = false;
    ITaskDefinition* def = nullptr;
    if (SUCCEEDED(svc->NewTask(0, &def))) {
        wchar_t user[256]; DWORD un = 256; wstring who;
        if (GetUserNameW(user, &un)) who = Env(L"USERDOMAIN") + L"\\" + user;
        IRegistrationInfo* ri = nullptr; if (SUCCEEDED(def->get_RegistrationInfo(&ri))) { BSTR d = SysAllocString(desc); ri->put_Description(d); SysFreeString(d); ri->Release(); }
        IPrincipal* pr = nullptr; if (SUCCEEDED(def->get_Principal(&pr))) { pr->put_LogonType(TASK_LOGON_INTERACTIVE_TOKEN); pr->put_RunLevel(TASK_RUNLEVEL_LUA); pr->Release(); }
        ITaskSettings* st = nullptr;
        if (SUCCEEDED(def->get_Settings(&st))) {
            st->put_DisallowStartIfOnBatteries(VARIANT_FALSE); st->put_StopIfGoingOnBatteries(VARIANT_FALSE);
            BSTR z = SysAllocString(L"PT0S"); st->put_ExecutionTimeLimit(z); SysFreeString(z);
            st->put_MultipleInstances(TASK_INSTANCES_IGNORE_NEW); st->put_Priority(4); st->Release();
        }
        ITriggerCollection* tc = nullptr; ITrigger* tr = nullptr;
        if (SUCCEEDED(def->get_Triggers(&tc)) && SUCCEEDED(tc->Create(TASK_TRIGGER_LOGON, &tr))) {
            ILogonTrigger* lt = nullptr;
            if (SUCCEEDED(tr->QueryInterface(IID_PPV_ARGS(&lt)))) { BSTR u = SysAllocString(who.c_str()); lt->put_UserId(u); SysFreeString(u); lt->Release(); }
            tr->Release();
        }
        if (tc) tc->Release();
        IActionCollection* ac = nullptr; IAction* a = nullptr;
        if (SUCCEEDED(def->get_Actions(&ac)) && SUCCEEDED(ac->Create(TASK_ACTION_EXEC, &a))) {
            IExecAction* ea = nullptr;
            if (SUCCEEDED(a->QueryInterface(IID_PPV_ARGS(&ea)))) {
                BSTR p = SysAllocString(exe.c_str()), w = SysAllocString(Parent(exe).c_str());
                ea->put_Path(p); ea->put_WorkingDirectory(w); SysFreeString(p); SysFreeString(w); ea->Release();
            }
            a->Release();
        }
        if (ac) ac->Release();
        IRegisteredTask* rt = nullptr; BSTR n = SysAllocString(name);
        VARIANT empty{}; VariantInit(&empty);
        ok = SUCCEEDED(root->RegisterTaskDefinition(n, def, TASK_CREATE_OR_UPDATE, empty, empty, TASK_LOGON_INTERACTIVE_TOKEN, empty, &rt));
        if (rt) rt->Release(); SysFreeString(n); def->Release();
    }
    root->Release(); svc->Release(); return ok;
}

// ============================================================ o que dá para instalar
struct AppInfo { const wchar_t* exe; const wchar_t* name; bool on; };
static std::vector<AppInfo> g_apps = {
    { L"LiquinixFinder", L"Finder", true }, { L"LiquinixSettings", L"System Settings", true }, { L"LiquinixActivityMonitor", L"Activity Monitor", true },
    { L"LiquinixTerminal", L"Terminal", true }, { L"LiquinixCalculator", L"Calculator", true }, { L"LiquinixNotes", L"Notes", true },
    { L"LiquinixScreenshot", L"Screenshot", true }, { L"LiquinixPreview", L"Preview", true }, { L"LiquinixReminders", L"Reminders", true },
    { L"LiquinixCalendar", L"Calendar", true }, { L"LiquinixTextEdit", L"TextEdit", true }, { L"LiquinixClock", L"Clock", true },
    { L"LiquinixWeather", L"Weather", true }, { L"LiquinixContacts", L"Contacts", true }, { L"LiquinixPhotos", L"Photos", true },
    { L"LiquinixTimeMachine", L"Time Machine", true }, { L"LiquinixMusic", L"Music", true }, { L"LiquinixQuickTime", L"QuickTime Player", true },
    { L"LiquinixForceQuit", L"Force Quit Applications", true },
};
static const wchar_t* OUR_PROCS[] = { L"LiquinixFinder.exe", L"LiquinixSettings.exe", L"LiquinixActivityMonitor.exe", L"LiquinixTerminal.exe", L"LiquinixCalculator.exe",
    L"LiquinixNotes.exe", L"LiquinixScreenshot.exe", L"LiquinixWindow32Host.exe", L"LiquinixPreview.exe", L"LiquinixReminders.exe", L"LiquinixCalendar.exe",
    L"LiquinixTextEdit.exe", L"LiquinixClock.exe", L"LiquinixWeather.exe", L"LiquinixContacts.exe", L"LiquinixPhotos.exe", L"LiquinixTimeMachine.exe",
    L"LiquinixMusic.exe", L"LiquinixQuickTime.exe", L"LiquinixForceQuit.exe" };

enum Mode { M_FULL, M_APPS, M_BARS, M_APPSBARS };
static const wchar_t* MODE_ID[] = { L"completo", L"apps", L"barras", L"apps+barras" };
struct Choices {
    Mode mode = M_FULL;
    bool autostart = true, cursor = true, fonts = true, assoc = false, launch = true;
    bool delSettings = false, cleanCache = false;
} g_ch;

// ============================================================ estado compartilhado com a thread de trabalho
static std::mutex g_mx;
static wstring g_status, g_error;
static std::atomic<float> g_progress{ 0 };
static std::atomic<bool> g_busy{ false }, g_workDone{ false }, g_workOk{ false };
static HWND g_wnd;
static void Status(const wstring& s) { std::lock_guard<std::mutex> k(g_mx); g_status = s; }
static void Fail(const wstring& s) { std::lock_guard<std::mutex> k(g_mx); g_error = s; }

// ============================================================ copiar uma pasta (com progresso)
static void ListFiles(const wstring& dir, const wstring& rel, std::vector<wstring>& out) {
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        wstring r = rel.empty() ? wstring(fd.cFileName) : rel + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ListFiles(dir + L"\\" + fd.cFileName, r, out);
        else if (_wcsicmp(ExtOf(fd.cFileName).c_str(), L".log")) out.push_back(r);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
static wstring OldDir() { wstring d = Env(L"LOCALAPPDATA") + L"\\SequoiaBinariosAntigos"; MkDirs(d); return d; }
// copia; arquivo preso (exe aberto): renomeia o velho para a pasta de antigos e copia de novo
static bool CopyOne(const wstring& from, const wstring& to) {
    MkDirs(Parent(to));
    if (CopyFileW(from.c_str(), to.c_str(), FALSE)) { DeleteFileW((to + L":Zone.Identifier").c_str()); return true; }
    SYSTEMTIME st; GetLocalTime(&st); wchar_t tag[32]; swprintf_s(tag, L"inst-%02d%02d%02d-", st.wHour, st.wMinute, st.wSecond);
    wstring old = OldDir() + L"\\" + tag + Leaf(to);
    if (MoveFileExW(to.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) && CopyFileW(from.c_str(), to.c_str(), FALSE)) {
        DeleteFileW((to + L":Zone.Identifier").c_str()); return true;
    }
    return false;
}
static int CopyTrees(const std::vector<std::pair<wstring, wstring>>& trees, float p0, float p1) {
    std::vector<std::pair<wstring, wstring>> files;
    for (auto& [s, d] : trees) { std::vector<wstring> f; ListFiles(s, L"", f); for (auto& r : f) files.push_back({ s + L"\\" + r, d + L"\\" + r }); }
    int fails = 0;
    for (size_t i = 0; i < files.size(); i++) {
        if (!CopyOne(files[i].first, files[i].second)) fails++;
        if (i % 8 == 0) { Status(wstring(T(L"Copiando ", L"Copying ")) + Leaf(files[i].second)); g_progress = p0 + (p1 - p0) * (float)(i + 1) / files.size(); }
    }
    g_progress = p1; return fails;
}

// ============================================================ etapas de trabalho
static bool CloseEverything() {
    wstring help = Helper();
    if (Running(L"LiquinixShell.exe")) {
        Status(T(L"Fechando o Liquinix…", L"Quitting Liquinix…"));
        if (!help.empty()) RunWait(help, L"--sair", 20000);
        if (!WaitGone(L"LiquinixShell.exe", 20000)) { Fail(T(L"O Liquinix não fechou. Feche-o com Ctrl+Alt+Shift+F12 e tente de novo.", L"Liquinix didn't quit. Quit it with Ctrl+Alt+Shift+F12 and try again.")); return false; }
    }
    if (Running(L"LiquinixTitleBars.exe")) {
        wstring tb = g_dest + L"\\bin\\LiquinixTitleBars.exe";
        if (Exists(tb)) RunWait(tb, L"--sair", 10000);
        if (!WaitGone(L"LiquinixTitleBars.exe", 5000)) Kill(L"LiquinixTitleBars.exe");
    }
    for (auto p : OUR_PROCS) Kill(p);
    if (!Running(L"explorer.exe")) Launch(L"explorer.exe");
    return true;
}

static wstring Programs() { return KnownFolder(FOLDERID_Programs); }

struct Assoc { const wchar_t* exeName; const wchar_t* prog; const wchar_t* typeName; const wchar_t* app; const wchar_t* capKey; const wchar_t* regName; std::vector<const wchar_t*> exts; };
static const Assoc ASSOC[] = {
    // nomes em inglês (aparecem em Configurações > Aplicativos padrão e no "Abrir com"; issue #2 no GitHub)
    { L"LiquinixMusic.exe", L"SequoiaMusic.Audio", L"Audio File", L"Music", L"Software\\SequoiaShell\\Musica\\Capabilities", L"Liquinix Music",
      { L".mp3", L".m4a", L".flac", L".wav", L".aac", L".ogg", L".wma", L".aif", L".aiff", L".opus" } },
    { L"LiquinixQuickTime.exe", L"SequoiaQuickTime.Video", L"Movie", L"QuickTime Player", L"Software\\SequoiaShell\\QuickTime\\Capabilities", L"Liquinix QuickTime Player",
      { L".mp4", L".mov", L".m4v", L".mkv", L".avi", L".wmv", L".webm", L".mpg", L".mpeg", L".3gp" } },
};
// o mesmo que tools\musica-registrar.ps1 / quicktime-registrar.ps1 (HKCU; o padrão quem escolhe é o usuário)
static void AssocRegister(const Assoc& a) {
    wstring exe = g_dest + L"\\bin\\" + a.exeName, cmd = L"\"" + exe + L"\" \"%1\"", cls = L"Software\\Classes\\";
    RegSetStr(HKEY_CURRENT_USER, cls + a.prog, nullptr, a.typeName);
    RegSetStr(HKEY_CURRENT_USER, cls + a.prog, L"FriendlyTypeName", a.typeName);
    RegSetStr(HKEY_CURRENT_USER, cls + a.prog + L"\\DefaultIcon", nullptr, L"\"" + exe + L"\",0");
    RegSetStr(HKEY_CURRENT_USER, cls + a.prog + L"\\shell\\open\\command", nullptr, cmd);
    wstring ap = cls + L"Applications\\" + a.exeName;
    RegSetStr(HKEY_CURRENT_USER, ap, L"FriendlyAppName", a.app);
    RegSetStr(HKEY_CURRENT_USER, ap + L"\\shell\\open\\command", nullptr, cmd);
    RegSetStr(HKEY_CURRENT_USER, a.capKey, L"ApplicationName", a.app);
    RegSetStr(HKEY_CURRENT_USER, a.capKey, L"ApplicationDescription", wstring(a.app) + L" (Liquinix)");
    RegSetStr(HKEY_CURRENT_USER, a.capKey, L"ApplicationIcon", L"\"" + exe + L"\",0");
    for (auto e : a.exts) {
        RegSetNone(HKEY_CURRENT_USER, cls + e + L"\\OpenWithProgids", a.prog);
        RegSetStr(HKEY_CURRENT_USER, ap + L"\\SupportedTypes", e, L"");
        RegSetStr(HKEY_CURRENT_USER, wstring(a.capKey) + L"\\FileAssociations", e, a.prog);
    }
    for (auto antigo : { L"Música (Sequoia)", L"QuickTime Player (Sequoia)" }) RegDelValue(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", antigo);
    RegSetStr(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", a.regName, a.capKey);
    Anota(wstring(a.app) + L" registrado em Abrir com (HKCU\\Software\\Classes\\" + a.prog + L"; o desinstalar remove)");
}
static void AssocUndo(const Assoc& a) {
    wstring cls = L"Software\\Classes\\";
    for (auto e : a.exts) RegDelValue(HKEY_CURRENT_USER, cls + e + L"\\OpenWithProgids", a.prog);
    RegDelTree(HKEY_CURRENT_USER, cls + a.prog);
    RegDelTree(HKEY_CURRENT_USER, cls + L"Applications\\" + a.exeName);
    RegDelValue(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", a.regName);
    for (auto antigo : { L"Música (Sequoia)", L"QuickTime Player (Sequoia)" }) RegDelValue(HKEY_CURRENT_USER, L"Software\\RegisteredApplications", antigo);  // nomes de versões antigas
    RegDelTree(HKEY_CURRENT_USER, Parent(a.capKey));
}

static const wchar_t* UNINST_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Liquinix";
static void RegisterUninstall() {
    wstring setup = g_dest + L"\\LiquinixSetup.exe";
    RegSetStr(HKEY_CURRENT_USER, UNINST_KEY, L"DisplayName", L"Liquinix");
    RegSetStr(HKEY_CURRENT_USER, UNINST_KEY, L"DisplayVersion", VersionOf(g_dest));
    RegSetStr(HKEY_CURRENT_USER, UNINST_KEY, L"Publisher", L"Liquinix (open source)");
    RegSetStr(HKEY_CURRENT_USER, UNINST_KEY, L"DisplayIcon", setup + L",0");
    RegSetStr(HKEY_CURRENT_USER, UNINST_KEY, L"InstallLocation", g_dest);
    RegSetStr(HKEY_CURRENT_USER, UNINST_KEY, L"URLInfoAbout", L"https://github.com/takasurazeem/Liquinix");
    RegSetStr(HKEY_CURRENT_USER, UNINST_KEY, L"UninstallString", L"\"" + setup + L"\" --desinstalar");
    RegSetStr(HKEY_CURRENT_USER, UNINST_KEY, L"ModifyPath", L"\"" + setup + L"\"");
    RegSetDword(HKEY_CURRENT_USER, UNINST_KEY, L"NoRepair", 1);
    ULARGE_INTEGER total{}; std::vector<wstring> f; ListFiles(g_dest, L"", f);
    for (auto& r : f) { WIN32_FILE_ATTRIBUTE_DATA d; if (GetFileAttributesExW((g_dest + L"\\" + r).c_str(), GetFileExInfoStandard, &d)) total.QuadPart += ((ULONGLONG)d.nFileSizeHigh << 32) | d.nFileSizeLow; }
    RegSetDword(HKEY_CURRENT_USER, UNINST_KEY, L"EstimatedSize", (DWORD)(total.QuadPart / 1024));
}

// O mesmo que o instalar.ps1. update = só os arquivos (o resto fica como estava).
static void DoInstall(bool update) {
    g_progress = 0.02f;
    if (!CloseEverything()) { g_workOk = false; g_workDone = true; return; }
    bool shellWasOpen = false;  // (fechado acima; quem volta a abrir é o "Abrir o Liquinix" do fim)
    MkDirs(g_dest);
    Anota((update ? L"atualizando de " : L"instalando de ") + g_src + L" (LiquinixSetup)");
    // 1. arquivos (quando a fonte não é a própria instalação)
    if (_wcsicmp(g_src.c_str(), g_dest.c_str())) {
        std::vector<std::pair<wstring, wstring>> trees;
        for (auto d : { L"bin", L"icones", L"sons", L"tools" }) if (IsDir(g_src + L"\\" + d)) trees.push_back({ g_src + L"\\" + d, g_dest + L"\\" + d });
        if (IsDir(g_src + L"\\cursor-leopard\\Leopard")) trees.push_back({ g_src + L"\\cursor-leopard\\Leopard", g_dest + L"\\cursor\\Leopard" });
        int fails = CopyTrees(trees, 0.05f, 0.7f);
        // o próprio instalador vai junto (Aplicativos instalados chama ele para desinstalar)
        if (Exists(g_src + L"\\LiquinixSetup.exe")) CopyOne(g_src + L"\\LiquinixSetup.exe", g_dest + L"\\LiquinixSetup.exe");
        else if (_wcsicmp(g_self.c_str(), (g_dest + L"\\LiquinixSetup.exe").c_str())) CopyOne(g_self, g_dest + L"\\LiquinixSetup.exe");
        for (auto d : { L"instalar.cmd", L"atualizar.cmd", L"desinstalar.cmd", L"instalar.ps1", L"desinstalar.ps1" })
            if (Exists(g_src + L"\\" + d)) CopyOne(g_src + L"\\" + d, g_dest + L"\\tools\\" + d);
        DeleteFileW((g_dest + L"\\bin\\LiquinixWindow32Host.exe").c_str());
        Anota(L"copiado: bin, icones, sons, tools" + (fails ? L" (" + std::to_wstring(fails) + L" arquivo(s) não copiados)" : wstring()));
        if (fails) { Fail(std::to_wstring(fails) + T(L" arquivo(s) não puderam ser copiados. Feche os apps do Liquinix e tente de novo.", L" file(s) could not be copied. Close the Liquinix apps and try again.")); g_workOk = false; g_workDone = true; return; }
    }
    g_progress = 0.72f;
    // 2. o que instalar (modo.txt; a atualização mantém o que estava)
    wstring modoArq = g_dest + L"\\modo.txt";
    if (!update) {
        wstring t = MODE_ID[g_ch.mode]; t += L"\r\n";
        if (g_ch.mode == M_APPS || g_ch.mode == M_APPSBARS) for (auto& a : g_apps) if (a.on) { t += a.exe; t += L"\r\n"; }
        WriteText(modoArq, t);
        Anota(L"modo: " + wstring(MODE_ID[g_ch.mode]));
    }
    RegisterUninstall();
    if (update) {
        // Música/QuickTime já registrados por esta instalação: registra de novo (nomes em inglês desde a issue #2)
        for (auto& a : ASSOC)
            if (Lower(RegGetStr(HKEY_CURRENT_USER, wstring(L"Software\\Classes\\") + a.prog + L"\\shell\\open\\command", nullptr)).find(Lower(g_dest + L"\\bin\\")) != wstring::npos) AssocRegister(a);
        Status(T(L"Atualizado.", L"Updated.")); g_progress = 1; g_workOk = true; g_workDone = true; (void)shellWasOpen; return; }
    bool full = g_ch.mode == M_FULL, apps = g_ch.mode == M_APPS || g_ch.mode == M_APPSBARS, bars = g_ch.mode == M_BARS || g_ch.mode == M_APPSBARS;
    // 3. fontes SF (só para este usuário)
    if (g_ch.fonts && IsDir(g_src + L"\\fontes")) {
        Status(T(L"Instalando as fontes…", L"Installing fonts…"));
        wstring fdir = Env(L"LOCALAPPDATA") + L"\\Microsoft\\Windows\\Fonts"; MkDirs(fdir);
        std::vector<wstring> fs; ListFiles(g_src + L"\\fontes", L"", fs);
        int n = 0;
        for (auto& r : fs) {
            wstring ext = Lower(ExtOf(r)); if (ext != L".otf" && ext != L".ttf") continue;
            wstring alvo = fdir + L"\\" + Leaf(r);
            if (!Exists(alvo) && CopyFileW((g_src + L"\\fontes\\" + r).c_str(), alvo.c_str(), TRUE)) Anota(L"fonte: " + alvo);
            wstring base = Leaf(r); base = base.substr(0, base.find_last_of(L'.'));
            wstring nome = base + (ext == L".otf" ? L" (OpenType)" : L" (TrueType)");
            const wchar_t* fk = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Fonts";
            if (RegGetStr(HKEY_CURRENT_USER, fk, nome.c_str()) != alvo) { RegSetStr(HKEY_CURRENT_USER, fk, nome.c_str(), alvo); Anota(L"registro: HKCU:\\" + wstring(fk) + L" | " + nome + L" = " + alvo); }
            AddFontResourceW(alvo.c_str()); n++;
        }
        if (n) SendMessageTimeoutW(HWND_BROADCAST, WM_FONTCHANGE, 0, 0, SMTO_ABORTIFHUNG, 1000, nullptr);
    }
    g_progress = 0.8f;
    // 4. cursor do Mac (o anterior fica no log e o desinstalar devolve)
    if (full && g_ch.cursor && IsDir(g_src + L"\\cursor")) {
        WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((g_src + L"\\cursor\\*").c_str(), &fd); wstring curName;
        if (h != INVALID_HANDLE_VALUE) { do if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.') { curName = fd.cFileName; break; } while (FindNextFileW(h, &fd)); FindClose(h); }
        if (!curName.empty()) {
            Status(T(L"Aplicando o cursor do Mac…", L"Applying the Mac cursor…"));
            wstring cdir = g_dest + L"\\cursor\\" + curName;
            CopyTrees({ { g_src + L"\\cursor\\" + curName, cdir } }, 0.8f, 0.84f);
            const wchar_t* ck = L"Control Panel\\Cursors";
            for (auto& l : Lines(ReadText(g_src + L"\\cursor\\cursores.txt"))) {
                size_t eq = l.find(L'='); if (eq == wstring::npos) continue;
                wstring papel = Trim(l.substr(0, eq)), arq = cdir + L"\\" + Trim(l.substr(eq + 1));
                if (!Exists(arq)) continue;
                Anota(L"cursor-antes: " + papel + L"=" + RegGetStr(HKEY_CURRENT_USER, ck, papel.c_str()));
                RegSetStr(HKEY_CURRENT_USER, ck, papel.c_str(), arq);
            }
            Anota(L"cursor-antes: (default)=" + RegGetStr(HKEY_CURRENT_USER, ck, nullptr));
            RegSetStr(HKEY_CURRENT_USER, ck, nullptr, curName);
            RegSetDword(HKEY_CURRENT_USER, ck, L"Scheme Source", 1);
            Anota(L"registro: HKCU:\\Control Panel\\Cursors (cursor do Mac)");
            SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, SPIF_SENDCHANGE);
        }
    }
    g_progress = 0.86f;
    Status(T(L"Criando os atalhos…", L"Creating shortcuts…"));
    wstring progs = Programs();
    if (full) {
        // 5. ajustes de um PC novo: área de trabalho e pastas do Mac
        wstring cfg = Env(L"APPDATA") + L"\\Shell", aj = cfg + L"\\ajustes.txt";
        if (!Exists(aj)) { MkDirs(cfg); WriteText(aj, L"explorer=fundo\r\nfinder_padrao=1\r\n"); Anota(L"ajuste: " + aj + L" (padrões: explorer=fundo, finder_padrao=1)"); }
        if (ReadText(aj).find(L"finder_padrao=0") == wstring::npos && Exists(g_dest + L"\\bin\\LiquinixFinder.exe")) {
            RunWait(g_dest + L"\\bin\\LiquinixFinder.exe", L"--quieto --padrao on", 30000);
            Anota(L"pastas no Finder: LiquinixFinder.exe --padrao on");
        }
        wstring exe = g_dest + L"\\bin\\LiquinixShell.exe";
        if (Shortcut(progs + L"\\Liquinix.lnk", exe, L"Liquinix")) Anota(L"atalho: " + progs + L"\\Liquinix.lnk");
        RemoveDirectoryW((progs + L"\\Liquinix").c_str());
        if (g_ch.autostart) { if (TaskLogon(L"Liquinix", exe, L"Starts Liquinix at sign-in (LiquinixSetup removes it)")) Anota(L"tarefa agendada: Liquinix (ao entrar)"); }
        else TaskDelete(L"Liquinix");
        TaskDelete(L"Liquinix Title Bars");
    } else {
        // 6. só os apps e/ou as barras: atalhos na pasta "Liquinix" do Menu Iniciar, nada do Shell
        wstring menu = progs + L"\\Liquinix"; MkDirs(menu);
        { WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((menu + L"\\*.lnk").c_str(), &fd); if (h != INVALID_HANDLE_VALUE) { do DeleteFileW((menu + L"\\" + fd.cFileName).c_str()); while (FindNextFileW(h, &fd)); FindClose(h); } }
        if (TaskExists(L"Liquinix")) { TaskDelete(L"Liquinix"); Anota(L"tarefa agendada Liquinix removida (modo sem o Shell)"); }
        DeleteFileW((progs + L"\\Liquinix.lnk").c_str());
        if (apps) for (auto& a : g_apps) if (a.on) Shortcut(menu + L"\\" + a.name + L".lnk", g_dest + L"\\bin\\" + a.exe + L".exe");
        if (bars) {
            wstring tb = g_dest + L"\\bin\\LiquinixTitleBars.exe";
            Shortcut(menu + L"\\Liquinix Title Bars.lnk", tb);
            if (g_ch.autostart) { if (TaskLogon(L"Liquinix Title Bars", tb, L"Mac title bars on other apps, without the Liquinix Shell (LiquinixSetup removes it)")) Anota(L"tarefa agendada: Liquinix Title Bars (ao entrar)"); }
            else TaskDelete(L"Liquinix Title Bars");
        } else TaskDelete(L"Liquinix Title Bars");
        Anota(L"menu iniciar: " + menu);
    }
    g_progress = 0.94f;
    // 7. Música e QuickTime em "Abrir com"
    if (g_ch.assoc) {
        for (auto& a : ASSOC) {
            bool want = full || (apps && std::any_of(g_apps.begin(), g_apps.end(), [&](const AppInfo& x) { return x.on && !_wcsicmp((wstring(x.exe) + L".exe").c_str(), a.exeName); }));
            if (want && Exists(g_dest + L"\\bin\\" + a.exeName)) AssocRegister(a);
        }
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    }
    RegisterUninstall();
    Anota(L"instalação concluída (LiquinixSetup)");
    Status(T(L"Pronto.", L"Done.")); g_progress = 1; g_workOk = true; g_workDone = true;
}

// O mesmo que o desinstalar.ps1
static void DoUninstall() {
    g_progress = 0.03f;
    wstring help = Helper();
    if (!CloseEverything()) { g_workOk = false; g_workDone = true; return; }
    g_progress = 0.15f;
    if (!help.empty()) { Status(T(L"Devolvendo as janelas ao visual do Windows…", L"Restoring the Windows look of open windows…")); RunWait(help, L"--desfazer-janelas", 30000); }
    // barra do Windows nos outros monitores. Vazio = o Shell já devolveu ao sair (ele deixa a linha vazia); não é um
    // valor (o _wtoi dava 0 e desligava a barra nos outros monitores de quem nunca tinha mexido nisso)
    wstring aj = Env(L"APPDATA") + L"\\Shell\\ajustes.txt";
    {
        auto ls = Lines(ReadText(aj)); wstring keep; bool changed = false, wrote = false;
        for (auto& l : ls) {
            if (l.rfind(L"barra_windows_monitores_antes=", 0) == 0) {
                wstring v = Trim(l.substr(30)); const wchar_t* adv = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
                // só um número de verdade vai para o registro: texto estragado ou editado à mão não vira 0 (desligaria a barra)
                bool num = !v.empty() && v.size() <= 9 && v.find_first_not_of(L"0123456789") == wstring::npos;
                if (v == L"x") { RegDelValue(HKEY_CURRENT_USER, adv, L"MMTaskbarEnabled"); wrote = true; }
                else if (num) { RegSetDword(HKEY_CURRENT_USER, adv, L"MMTaskbarEnabled", (DWORD)_wtoi(v.c_str())); wrote = true; }
                changed = true; continue;
            }
            keep += l + L"\r\n";
        }
        if (changed) WriteText(aj, keep);
        // o Explorador só relê o ajuste quando avisado (como o Shell faz ao sair, TellExplorerTray); sem isto a barra nos
        // outros monitores só voltava depois de reiniciar o Explorador
        if (wrote)
            EnumWindows([](HWND h, LPARAM) -> BOOL {
                wchar_t c[64] = {}; GetClassNameW(h, c, 64);
                if (!wcscmp(c, L"Shell_TrayWnd")) SendMessageTimeoutW(h, WM_SETTINGCHANGE, 0, (LPARAM)L"TraySettings", SMTO_ABORTIFHUNG, 1000, nullptr);
                return TRUE;
            }, 0);
    }
    g_progress = 0.25f;
    Status(T(L"Removendo o início automático e os atalhos…", L"Removing autostart and shortcuts…"));
    for (auto t : { L"Liquinix", L"SequoiaShell", L"Liquinix Title Bars" }) TaskDelete(t);
    if (Exists(g_dest + L"\\bin\\LiquinixFinder.exe")) RunWait(g_dest + L"\\bin\\LiquinixFinder.exe", L"--quieto --padrao off", 30000);
    wstring progs = Programs();
    DeleteFileW((progs + L"\\Liquinix.lnk").c_str());
    SHFILEOPSTRUCTW op{}; wstring menu = progs + L"\\Liquinix"; menu.push_back(0);
    op.wFunc = FO_DELETE; op.pFrom = menu.c_str(); op.fFlags = FOF_NO_UI; SHFileOperationW(&op);
    // janela de Abrir/Salvar do Mac
    for (auto raiz : { L"Software\\Classes\\CLSID\\", L"Software\\Classes\\WOW6432Node\\CLSID\\" })
        for (auto clsid : { L"{DC1C5A9C-E88A-4dde-A5A1-60F82A20AEF7}", L"{C0B4E2F3-BA21-4773-8DBA-335EC946EB8B}" }) {
            wstring k = wstring(raiz) + clsid;
            // só a desta instalação (num PC de desenvolvimento ela aponta para outra pasta e fica)
            if (Lower(RegGetStr(HKEY_CURRENT_USER, k + L"\\InprocServer32", nullptr)).rfind(Lower(g_dest + L"\\"), 0) == 0) RegDelTree(HKEY_CURRENT_USER, k);
        }
    g_progress = 0.4f;
    // ícones do sistema (raro): o script de desfazer, numa janela visível
    wstring docs = g_dest + L"\\docs";
    if (Exists(docs + L"\\backup-icones-sistema.json") || Exists(docs + L"\\backup-desktopini") || Exists(docs + L"\\backup-icones-maquina.json")) {
        wstring ps1 = g_dest + L"\\tools\\icones-sistema.ps1";
        if (Exists(ps1)) {
            Status(T(L"Devolvendo os ícones do Windows…", L"Restoring the Windows icons…"));
            wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
            RunWait(wstring(sys) + L"\\WindowsPowerShell\\v1.0\\powershell.exe", L"-NoProfile -File \"" + ps1 + L"\" -Desfazer", 300000, false);
        }
        if (Exists(docs + L"\\backup-icones-sistema.json") || Exists(docs + L"\\backup-desktopini") || Exists(docs + L"\\backup-icones-maquina.json")) {
            Fail(T(L"Parte dos ícones do Windows não voltou. A pasta da instalação fica (os backups estão nela); tente desinstalar de novo.", L"Some Windows icons were not restored. The install folder stays (the backups are in it); try uninstalling again."));
            g_workOk = false; g_workDone = true; return;
        }
    }
    g_progress = 0.5f;
    Status(T(L"Devolvendo o cursor e as fontes…", L"Restoring the cursor and fonts…"));
    const wchar_t* ck = L"Control Panel\\Cursors"; bool curChanged = false;
    wstring curAntes = Env(L"APPDATA") + L"\\Shell\\cursor-antes.txt";
    if (Exists(curAntes) && Lower(RegGetStr(HKEY_CURRENT_USER, ck, L"Arrow")).rfind(Lower(g_dest + L"\\cursor\\Leopard\\"), 0) == 0) {
        for (auto& l : Lines(ReadText(curAntes))) { size_t eq = l.find(L'='); if (eq != wstring::npos) RegSetStr(HKEY_CURRENT_USER, ck, l.substr(0, eq) == L"(default)" ? nullptr : l.substr(0, eq).c_str(), l.substr(eq + 1)); }
        DeleteFileW(curAntes.c_str()); curChanged = true;
    }
    auto log = Lines(ReadText(g_log));
    for (auto& l : log) {
        size_t p = l.find(L"cursor-antes: ");
        if (p != wstring::npos) { wstring r = l.substr(p + 14); size_t eq = r.find(L'='); if (eq != wstring::npos) { wstring n = r.substr(0, eq); RegSetStr(HKEY_CURRENT_USER, ck, n == L"(default)" ? nullptr : n.c_str(), r.substr(eq + 1)); curChanged = true; } }
    }
    if (curChanged) SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, SPIF_SENDCHANGE);
    for (auto& l : log) {
        size_t p = l.find(L"registro: "), f = l.find(L"fonte: ");
        if (p != wstring::npos) {
            wstring r = l.substr(p + 10); size_t bar = r.find(L" | "), eq = r.find(L" = ");
            HKEY root; wstring sub;
            if (bar != wstring::npos && eq != wstring::npos && eq > bar && PsKey(r.substr(0, bar), root, sub)) RegDelValue(root, sub, r.substr(bar + 3, eq - bar - 3).c_str());
        } else if (f != wstring::npos) { wstring path = l.substr(f + 7); RemoveFontResourceW(path.c_str()); DeleteFileW(path.c_str()); }
    }
    RegDelTree(HKEY_CURRENT_USER, L"Software\\ShellProprio");
    RegDelTree(HKEY_CURRENT_USER, L"Software\\Classes\\AppUserModelId\\Liquinix.Atualizacao");
    for (auto& a : ASSOC) AssocUndo(a);
    SHDeleteEmptyKeyW(HKEY_CURRENT_USER, L"Software\\SequoiaShell");  // sobrava vazia (Musica e QuickTime saem acima)
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    RegDelTree(HKEY_CURRENT_USER, UNINST_KEY);
    g_progress = 0.7f;
    Status(T(L"Apagando os arquivos…", L"Deleting files…"));
    {   // arquivo por arquivo: as cópias da DLL das janelas (bin\obj\mj) ficam presas nos apps abertos; essas vão para
        // uma pasta no TEMP (mover um arquivo em uso funciona, apagar não) e saem ao reiniciar o PC
        std::vector<wstring> fs; ListFiles(g_dest, L"", fs);
        { WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((g_dest + L"\\*.log").c_str(), &fd); if (h != INVALID_HANDLE_VALUE) { do fs.push_back(fd.cFileName); while (FindNextFileW(h, &fd)); FindClose(h); } }
        { WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((g_dest + L"\\bin\\*.log").c_str(), &fd); if (h != INVALID_HANDLE_VALUE) { do fs.push_back(wstring(L"bin\\") + fd.cFileName); while (FindNextFileW(h, &fd)); FindClose(h); } }
        wstring restos = Env(L"TEMP") + L"\\Liquinix-restos";
        for (auto& r : fs) {
            wstring p = g_dest + L"\\" + r;
            SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (DeleteFileW(p.c_str()) || !Exists(p)) continue;
            MkDirs(restos); wstring alvo = restos + L"\\" + std::to_wstring(GetTickCount64()) + L"-" + Leaf(p);
            if (MoveFileExW(p.c_str(), alvo.c_str(), MOVEFILE_REPLACE_EXISTING)) MoveFileExW(alvo.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
            else MoveFileExW(p.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
        }
        wstring from = g_dest; from.push_back(0);  // as pastas (vazias agora) e o que sobrou
        SHFILEOPSTRUCTW o{}; o.wFunc = FO_DELETE; o.pFrom = from.c_str(); o.fFlags = FOF_NO_UI;
        SHFileOperationW(&o);
    }
    if (g_ch.delSettings) {
        wstring cfg = Env(L"APPDATA") + L"\\Shell"; cfg.push_back(0);
        SHFILEOPSTRUCTW o{}; o.wFunc = FO_DELETE; o.pFrom = cfg.c_str(); o.fFlags = FOF_NO_UI; SHFileOperationW(&o);
    }
    g_progress = 0.85f;
    if (g_ch.cleanCache) {
        Status(T(L"Limpando o cache de ícones…", L"Clearing the icon cache…"));
        Kill(L"explorer.exe"); Sleep(800);
        wstring exp = Env(L"LOCALAPPDATA") + L"\\Microsoft\\Windows\\Explorer";
        DeleteFileW((Env(L"LOCALAPPDATA") + L"\\IconCache.db").c_str());
        for (auto pat : { L"\\iconcache_*.db", L"\\thumbcache_*.db" }) {
            WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((exp + pat).c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) { do DeleteFileW((exp + L"\\" + fd.cFileName).c_str()); while (FindNextFileW(h, &fd)); FindClose(h); }
        }
        Launch(L"explorer.exe");
        wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH); Launch(wstring(sys) + L"\\ie4uinit.exe", L"-show");
        for (auto k : { L"Software\\SequoiaShell", L"Software\\Classes\\SequoiaMusic.Audio", L"Software\\Classes\\SequoiaQuickTime.Video" }) RegDelTree(HKEY_CURRENT_USER, k);
    }
    if (!Running(L"explorer.exe")) Launch(L"explorer.exe");
    if (!help.empty() && help.find(Env(L"TEMP")) == 0) MoveFileExW(help.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    Status(Exists(g_dest) ? T(L"Alguns arquivos estavam em uso e saem depois de reiniciar o PC.", L"Some files were in use and go away after a restart.") : T(L"O Liquinix foi removido.", L"Liquinix was removed."));
    if (Exists(g_dest)) MoveFileExW(g_dest.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
    g_progress = 1; g_workOk = true; g_workDone = true;
}

// ============================================================ GitHub (versão nova)
struct Release { wstring tag, date, url, sha; std::vector<wstring> notes; ULONGLONG size = 0; };
static Release g_rel; static wstring g_relErr;
static std::string HttpGet(const wstring& host, const wstring& path, const wchar_t* accept, const wstring& outFile = L"", std::function<void(ULONGLONG, ULONGLONG)> prog = {}) {
    std::string body;
    HINTERNET s = WinHttpOpen(L"Liquinix-Setup/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) return body;
    WinHttpSetTimeouts(s, 10000, 10000, 30000, 60000);
    HINTERNET c = WinHttpConnect(s, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    wstring hdr = wstring(L"Accept: ") + accept;
    HANDLE f = INVALID_HANDLE_VALUE;
    if (r && WinHttpSendRequest(r, hdr.c_str(), (DWORD)-1, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
        DWORD code = 0, sz = 4; WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &sz, nullptr);
        wchar_t len[32] = {}; DWORD ln = sizeof(len); ULONGLONG total = 0;
        if (WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH, nullptr, len, &ln, nullptr)) total = _wcstoui64(len, nullptr, 10);
        if (code == 200) {
            if (!outFile.empty()) f = CreateFileW(outFile.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            std::vector<char> buf(1 << 16); ULONGLONG got = 0; DWORD n = 0;
            while (WinHttpReadData(r, buf.data(), (DWORD)buf.size(), &n) && n) {
                if (f != INVALID_HANDLE_VALUE) { DWORD w; WriteFile(f, buf.data(), n, &w, nullptr); } else body.append(buf.data(), n);
                got += n; if (prog) prog(got, total);
            }
            if (f != INVALID_HANDLE_VALUE) { CloseHandle(f); body = "ok"; }
        }
    }
    if (r) WinHttpCloseHandle(r); if (c) WinHttpCloseHandle(c); WinHttpCloseHandle(s);
    return body;
}
static wstring JsonStr(const std::string& j, const std::string& key, size_t from = 0, size_t* at = nullptr) {
    size_t p = j.find("\"" + key + "\"", from); if (p == std::string::npos) return L"";
    p = j.find(':', p); p = j.find('"', p); if (p == std::string::npos) return L"";
    wstring w; std::string run;
    auto flush = [&] { w += FromUtf8(run); run.clear(); };
    for (size_t i = p + 1; i < j.size() && j[i] != '"'; i++) {
        if (j[i] != '\\' || i + 1 >= j.size()) { run += j[i]; continue; }
        char e = j[++i];
        if (e == 'n') run += '\n'; else if (e == 'r') run += '\r'; else if (e == 't') run += '\t';
        else if (e == 'u' && i + 4 < j.size()) { flush(); wchar_t u = (wchar_t)strtoul(j.substr(i + 1, 4).c_str(), nullptr, 16); if (u != 0xFEFF) w += u; i += 4; }
        else run += e;
    }
    flush();
    if (!w.empty() && w[0] == 0xFEFF) w.erase(0, 1);
    if (at) *at = p; return w;
}
static void CheckGitHub() {
    g_rel = {}; g_relErr.clear();
    std::string j = HttpGet(L"api.github.com", L"/repos/takasurazeem/Liquinix/releases/latest", L"application/vnd.github+json");
    if (j.empty()) { g_relErr = T(L"Não deu para falar com o GitHub. Confira a internet e tente de novo.", L"Couldn't reach GitHub. Check your connection and try again."); return; }
    g_rel.tag = JsonStr(j, "tag_name");
    wstring d = JsonStr(j, "published_at"); if (d.size() >= 10) g_rel.date = d.substr(8, 2) + L"/" + d.substr(5, 2) + L"/" + d.substr(0, 4);
    for (auto& l : Lines(JsonStr(j, "body"))) { wstring t = Trim(l); while (!t.empty() && (t[0] == L'-' || t[0] == L'*' || t[0] == L' ')) t.erase(0, 1); if (!t.empty() && g_rel.notes.size() < 8) g_rel.notes.push_back(t); }
    size_t a = j.find("\"assets\"");
    for (size_t p = a; p != std::string::npos; ) {
        size_t at = 0; wstring url = JsonStr(j, "browser_download_url", p, &at); if (url.empty()) break;
        if (Lower(url).size() > 4 && Lower(url).substr(url.size() - 4) == L".zip") {
            g_rel.url = url;
            size_t objStart = j.rfind("{", at);
            wstring dg = JsonStr(j, "digest", objStart); if (dg.rfind(L"sha256:", 0) == 0) g_rel.sha = Lower(dg.substr(7));
            size_t sp = j.find("\"size\"", objStart); if (sp != std::string::npos) g_rel.size = _strtoui64(j.c_str() + j.find(':', sp) + 1, nullptr, 10);
            break;
        }
        p = at + 1;
    }
    if (g_rel.tag.empty() || g_rel.url.empty()) g_relErr = T(L"A versão mais nova no GitHub não tem o pacote .zip.", L"The latest release on GitHub has no .zip package.");
}
static wstring Sha256File(const wstring& path) {
    BCRYPT_ALG_HANDLE alg; BCRYPT_HASH_HANDLE h; wstring out;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) return out;
    if (!BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0)) {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            std::vector<BYTE> b(1 << 20); DWORD n;
            while (ReadFile(f, b.data(), (DWORD)b.size(), &n, nullptr) && n) BCryptHashData(h, b.data(), n, 0);
            CloseHandle(f);
            BYTE d[32]; BCryptFinishHash(h, d, 32, 0);
            wchar_t hx[3]; for (BYTE x : d) { swprintf_s(hx, L"%02x", x); out += hx; }
        }
        BCryptDestroyHash(h);
    }
    BCryptCloseAlgorithmProvider(alg, 0); return out;
}
static void DoUpdate() {
    g_progress = 0.01f;
    wstring tmp = Env(L"TEMP") + L"\\liquinix-setup-" + std::to_wstring(GetTickCount()); MkDirs(tmp);
    wstring zip = tmp + L"\\Liquinix.zip";
    Status(T(L"Baixando ", L"Downloading ") + g_rel.tag + L"…");
    size_t hs = g_rel.url.find(L"://") + 3, ps = g_rel.url.find(L'/', hs);
    std::string ok = HttpGet(g_rel.url.substr(hs, ps - hs), g_rel.url.substr(ps), L"application/octet-stream", zip, [](ULONGLONG got, ULONGLONG total) {
        if (total) g_progress = 0.02f + 0.5f * (float)got / total;
        wchar_t b[96]; swprintf_s(b, T(L"Baixando… %.0f de %.0f MB", L"Downloading… %.0f of %.0f MB"), got / 1048576.0, total / 1048576.0); Status(b);
    });
    if (ok.empty()) { Fail(T(L"O download falhou. Tente de novo.", L"The download failed. Try again.")); g_workOk = false; g_workDone = true; return; }
    Status(T(L"Conferindo o pacote (SHA-256)…", L"Checking the package (SHA-256)…")); g_progress = 0.54f;
    if (!g_rel.sha.empty() && Sha256File(zip) != g_rel.sha) { Fail(T(L"O arquivo baixado não bate com o publicado (SHA-256). Nada foi instalado.", L"The download doesn't match the published package (SHA-256). Nothing was installed.")); g_workOk = false; g_workDone = true; return; }
    Status(T(L"Extraindo…", L"Extracting…")); g_progress = 0.57f;
    wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
    wstring out = tmp + L"\\pacote"; MkDirs(out);
    RunWait(wstring(sys) + L"\\tar.exe", L"-xf \"" + zip + L"\" -C \"" + out + L"\"", 600000);
    wstring root;
    if (Exists(out + L"\\bin\\LiquinixShell.exe")) root = out;
    else { WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((out + L"\\*").c_str(), &fd); if (h != INVALID_HANDLE_VALUE) { do if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && Exists(out + L"\\" + fd.cFileName + L"\\bin\\LiquinixShell.exe")) root = out + L"\\" + fd.cFileName; while (root.empty() && FindNextFileW(h, &fd)); FindClose(h); } }
    if (root.empty()) { Fail(T(L"O pacote baixado não tem os programas do Liquinix.", L"The downloaded package has no Liquinix programs.")); g_workOk = false; g_workDone = true; return; }
    g_src = root;
    // a atualização usa o mesmo caminho da instalação, só com os arquivos
    float base = g_progress;
    DoInstall(true);
    (void)base;
    SHFILEOPSTRUCTW o{}; wstring t2 = tmp; t2.push_back(0); o.wFunc = FO_DELETE; o.pFrom = t2.c_str(); o.fFlags = FOF_NO_UI; SHFileOperationW(&o);
}

// ============================================================ interface (Direct2D, no visual do Instalador do macOS)
static ID2D1Factory* g_d2;
static IDWriteFactory* g_dw;
static ID2D1RenderTarget* g_rt;      // a janela (ID2D1HwndRenderTarget) ou, no --foto, um bitmap WIC
static ID2D1HwndRenderTarget* g_hrt;
static ID2D1Bitmap* g_logo;
static IWICBitmapSource* g_logoSrc;
static float g_k = 1;   // escala do DPI
static bool g_dark;
static wstring g_fontUI = L"Segoe UI Variable Text", g_fontTitle = L"Segoe UI Variable Display";

enum Page { P_INTRO, P_HOME, P_TYPE, P_APPS, P_WORK, P_DONE, P_UPDCHECK, P_UPDATE, P_UNINST, P_ERROR };
enum Flow { F_INSTALL, F_MODIFY, F_UPDATE, F_UNINSTALL };
static Page g_page = P_INTRO; static Flow g_flow = F_INSTALL;
static int g_homeSel = 0;  // 0 atualizar, 1 modificar, 2 desinstalar
static bool g_checking = false;
static float g_spin = 0;
static wstring g_instVer, g_pkgVer;
static bool g_installed = false, g_fromPkg = false;
static bool g_forceUninstallOnly;  // aberto por "Aplicativos instalados > Desinstalar"

struct Btn { D2D1_RECT_F r; int id; };
static std::vector<Btn> g_hits;
static int g_hover = -1, g_press = -1;
enum { B_BACK = 1, B_NEXT, B_CLOSE, B_MIN, B_ALL, B_NONE, B_MODE0 = 100, B_HOME0 = 200, B_APP0 = 300, B_CHK0 = 400 };

static D2D1_COLOR_F C(UINT32 rgb, float a = 1) { return D2D1::ColorF(((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, a); }
static D2D1_RECT_F R(float l, float t, float r, float b) { return D2D1::RectF(l, t, r, b); }
static D2D1_COLOR_F Accent() { return C(0x0A84FF); }
static D2D1_COLOR_F Fg() { return g_dark ? C(0xFFFFFF, 0.92f) : C(0x1D1D1F); }
static D2D1_COLOR_F Fg2() { return g_dark ? C(0xFFFFFF, 0.55f) : C(0x6E6E73); }

static IDWriteTextFormat* Fmt(float size, DWRITE_FONT_WEIGHT w, bool title = false, DWRITE_TEXT_ALIGNMENT al = DWRITE_TEXT_ALIGNMENT_LEADING) {
    IDWriteTextFormat* f = nullptr;
    g_dw->CreateTextFormat((title ? g_fontTitle : g_fontUI).c_str(), nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, g_pt ? L"pt-BR" : L"en-US", &f);
    if (f) { f->SetTextAlignment(al); f->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP); }
    return f;
}
static void Text(const wstring& s, D2D1_RECT_F r, float size, D2D1_COLOR_F col, DWRITE_FONT_WEIGHT w = DWRITE_FONT_WEIGHT_REGULAR, bool title = false, DWRITE_TEXT_ALIGNMENT al = DWRITE_TEXT_ALIGNMENT_LEADING, bool vcenter = false) {
    IDWriteTextFormat* f = Fmt(size, w, title, al); if (!f) return;
    if (vcenter) f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    ID2D1SolidColorBrush* b; g_rt->CreateSolidColorBrush(col, &b);
    g_rt->DrawTextW(s.c_str(), (UINT32)s.size(), f, r, b);
    b->Release(); f->Release();
}
static float TextH(const wstring& s, float w, float size, DWRITE_FONT_WEIGHT wt = DWRITE_FONT_WEIGHT_REGULAR) {
    IDWriteTextFormat* f = Fmt(size, wt); IDWriteTextLayout* l = nullptr; float h = size * 1.3f;
    if (f && SUCCEEDED(g_dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), f, w, 10000, &l))) { DWRITE_TEXT_METRICS m; l->GetMetrics(&m); h = m.height; l->Release(); }
    if (f) f->Release(); return h;
}
static void Fill(D2D1_RECT_F r, float rad, D2D1_COLOR_F c) { ID2D1SolidColorBrush* b; g_rt->CreateSolidColorBrush(c, &b); g_rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), b); b->Release(); }
static void Stroke(D2D1_RECT_F r, float rad, D2D1_COLOR_F c, float w = 1) { ID2D1SolidColorBrush* b; g_rt->CreateSolidColorBrush(c, &b); g_rt->DrawRoundedRectangle(D2D1::RoundedRect(r, rad, rad), b, w); b->Release(); }
static void Circle(float x, float y, float r, D2D1_COLOR_F c, bool fill = true, float w = 1.5f) { ID2D1SolidColorBrush* b; g_rt->CreateSolidColorBrush(c, &b); if (fill) g_rt->FillEllipse(D2D1::Ellipse({ x, y }, r, r), b); else g_rt->DrawEllipse(D2D1::Ellipse({ x, y }, r, r), b, w); b->Release(); }
static void Line(float x0, float y0, float x1, float y1, D2D1_COLOR_F c, float w = 1) { ID2D1SolidColorBrush* b; g_rt->CreateSolidColorBrush(c, &b); g_rt->DrawLine({ x0, y0 }, { x1, y1 }, b, w); b->Release(); }
static void Check(float x, float y, float s, D2D1_COLOR_F c, float w = 1.8f) {
    ID2D1PathGeometry* g; g_d2->CreatePathGeometry(&g); ID2D1GeometrySink* k; g->Open(&k);
    k->BeginFigure({ x - s * 0.45f, y }, D2D1_FIGURE_BEGIN_HOLLOW); k->AddLine({ x - s * 0.12f, y + s * 0.32f }); k->AddLine({ x + s * 0.48f, y - s * 0.38f });
    k->EndFigure(D2D1_FIGURE_END_OPEN); k->Close(); k->Release();
    ID2D1SolidColorBrush* b; g_rt->CreateSolidColorBrush(c, &b);
    ID2D1StrokeStyle* ss; D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
    g_d2->CreateStrokeStyle(sp, nullptr, 0, &ss);
    g_rt->DrawGeometry(g, b, w, ss); ss->Release(); b->Release(); g->Release();
}
static void Hit(D2D1_RECT_F r, int id) { g_hits.push_back({ r, id }); }

static void Button(D2D1_RECT_F r, const wstring& s, int id, bool primary, bool enabled = true) {
    bool hov = g_hover == id && enabled, pr = g_press == id && enabled;
    if (primary) {
        D2D1_COLOR_F c = enabled ? Accent() : C(0x8E8E93, 0.5f);
        if (pr) c = C(0x0060DF); else if (hov) c = C(0x2B95FF);
        Fill(r, 6, c);
        Text(s, r, 13, C(0xFFFFFF), DWRITE_FONT_WEIGHT_MEDIUM, false, DWRITE_TEXT_ALIGNMENT_CENTER, true);
    } else {
        Fill(r, 6, g_dark ? C(0xFFFFFF, pr ? 0.22f : hov ? 0.17f : 0.12f) : C(pr ? 0xD4D4D8 : hov ? 0xE6E6EA : 0xFFFFFF));
        if (!g_dark) Stroke(r, 6, C(0x000000, 0.12f), 0.8f);
        Text(s, r, 13, enabled ? Fg() : Fg2(), DWRITE_FONT_WEIGHT_REGULAR, false, DWRITE_TEXT_ALIGNMENT_CENTER, true);
    }
    if (enabled) Hit(r, id);
}
static void CheckBox(float x, float y, const wstring& s, bool on, int id, float w = 400) {
    D2D1_RECT_F box = R(x, y + 1, x + 16, y + 17);
    if (on) { Fill(box, 4, Accent()); Check(x + 8, y + 9, 10, C(0xFFFFFF), 1.8f); }
    else { Fill(box, 4, g_dark ? C(0xFFFFFF, 0.08f) : C(0xFFFFFF)); Stroke(box, 4, g_dark ? C(0xFFFFFF, 0.3f) : C(0x000000, 0.25f), 1); }
    Text(s, R(x + 26, y, x + w, y + 20), 13, Fg(), DWRITE_FONT_WEIGHT_REGULAR, false, DWRITE_TEXT_ALIGNMENT_LEADING, true);
    Hit(R(x - 4, y - 3, x + w, y + 22), id);
}
static void RadioCard(D2D1_RECT_F r, const wstring& title, const wstring& desc, bool on, int id) {
    bool hov = g_hover == id;
    Fill(r, 10, on ? (g_dark ? C(0x0A84FF, 0.18f) : C(0x0A84FF, 0.08f)) : (g_dark ? C(0xFFFFFF, hov ? 0.08f : 0.04f) : C(hov ? 0xF5F5F7 : 0xFFFFFF)));
    Stroke(r, 10, on ? Accent() : (g_dark ? C(0xFFFFFF, 0.14f) : C(0x000000, 0.1f)), on ? 2 : 1);
    float cx = r.left + 22, cy = r.top + 22;
    if (on) { Circle(cx, cy, 8, Accent()); Circle(cx, cy, 3.2f, C(0xFFFFFF)); }
    else Circle(cx, cy, 8, g_dark ? C(0xFFFFFF, 0.35f) : C(0x000000, 0.28f), false, 1.2f);
    Text(title, R(r.left + 40, r.top + 11, r.right - 12, r.top + 32), 14, Fg(), DWRITE_FONT_WEIGHT_SEMI_BOLD);
    Text(desc, R(r.left + 40, r.top + 32, r.right - 14, r.bottom - 6), 12, Fg2());
    Hit(r, id);
}

// etapas da barra lateral, conforme o caminho
static std::vector<wstring> Steps(int& cur) {
    std::vector<wstring> s; cur = 0;
    if (g_flow == F_UNINSTALL) {
        s = { T(L"Desinstalar", L"Uninstall"), T(L"Remoção", L"Removal"), T(L"Resumo", L"Summary") };
        cur = g_page == P_UNINST ? 0 : g_page == P_WORK ? 1 : 2;
    } else if (g_flow == F_UPDATE) {
        s = { T(L"Atualização", L"Software Update"), T(L"Instalação", L"Installation"), T(L"Resumo", L"Summary") };
        cur = g_page == P_UPDCHECK ? 0 : g_page == P_WORK ? 1 : 2;
    } else {
        s = { T(L"Introdução", L"Introduction"), T(L"Tipo de Instalação", L"Installation Type") };
        bool apps = g_ch.mode == M_APPS || g_ch.mode == M_APPSBARS;
        if (apps) s.push_back(T(L"Apps", L"Apps"));
        s.push_back(T(L"Instalação", L"Installation")); s.push_back(T(L"Resumo", L"Summary"));
        switch (g_page) {
        case P_INTRO: case P_HOME: cur = 0; break;
        case P_TYPE: cur = 1; break;
        case P_APPS: cur = 2; break;
        case P_WORK: cur = apps ? 3 : 2; break;
        default: cur = (int)s.size() - 1; break;
        }
    }
    return s;
}

static const float W = 800, H = 560, TB = 40, SIDE = 230, BOTTOM = 64;
static D2D1_RECT_F ContentR() { return R(SIDE + 6, TB + 8, W - 22, H - BOTTOM); }

static void PaintLogo(float x, float y, float s) {
    if (!g_logo && g_logoSrc) g_rt->CreateBitmapFromWicBitmap(g_logoSrc, nullptr, &g_logo);
    if (g_logo) g_rt->DrawBitmap(g_logo, R(x, y, x + s, y + s), 1, D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
}

static void Paint() {
    if (!g_rt) return;
    g_hits.clear();
    g_rt->BeginDraw();
    g_rt->SetTransform(D2D1::Matrix3x2F::Scale(g_k, g_k));
    g_rt->Clear(g_dark ? C(0x232325) : C(0xECECEE));
    // barra de título com os semáforos (fechar e minimizar; o maximizar fica apagado, como no Instalador)
    bool overLights = g_hover == B_CLOSE || g_hover == B_MIN;
    D2D1_COLOR_F off = g_dark ? C(0xFFFFFF, 0.18f) : C(0x000000, 0.14f);
    Circle(22, TB / 2, 6.5f, C(0xFF5F57)); Hit(R(14, 10, 30, 30), B_CLOSE);
    Circle(42, TB / 2, 6.5f, C(0xFEBC2E)); Hit(R(34, 10, 50, 30), B_MIN);
    Circle(62, TB / 2, 6.5f, off);
    if (overLights) { Line(19.5f, 17.5f, 24.5f, 22.5f, C(0x4D0000, 0.7f), 1.2f); Line(24.5f, 17.5f, 19.5f, 22.5f, C(0x4D0000, 0.7f), 1.2f); Line(39, 20, 45, 20, C(0x5A3A00, 0.7f), 1.2f); }
    wstring title = g_flow == F_UNINSTALL ? T(L"Desinstalar o Liquinix", L"Uninstall Liquinix") : g_flow == F_UPDATE ? T(L"Atualizar o Liquinix", L"Update Liquinix") : T(L"Instalar o Liquinix", L"Install Liquinix");
    Text(title, R(0, 0, W, TB), 13, Fg(), DWRITE_FONT_WEIGHT_SEMI_BOLD, false, DWRITE_TEXT_ALIGNMENT_CENTER, true);
    // barra lateral: logo e etapas
    PaintLogo(SIDE / 2 - 40, TB + 22, 80);
    int cur; auto steps = Steps(cur);
    float y = TB + 130;
    for (int i = 0; i < (int)steps.size(); i++) {
        bool done = i < cur || (g_page == P_DONE && i == cur && g_workOk), now = i == cur;
        if (now && !done) Circle(36, y + 10, 5, Accent());
        else if (done) Circle(36, y + 10, 5, g_dark ? C(0xFFFFFF, 0.45f) : C(0x8E8E93));
        else Circle(36, y + 10, 4.5f, g_dark ? C(0xFFFFFF, 0.3f) : C(0x000000, 0.25f), false, 1.2f);
        Text(steps[i], R(52, y, SIDE - 8, y + 20), 13, now ? Fg() : Fg2(), now ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_REGULAR, false, DWRITE_TEXT_ALIGNMENT_LEADING, true);
        y += 30;
    }
    // painel de conteúdo
    D2D1_RECT_F c = ContentR();
    Fill(c, 10, g_dark ? C(0x1C1C1E) : C(0xFFFFFF));
    Stroke(c, 10, g_dark ? C(0xFFFFFF, 0.08f) : C(0x000000, 0.08f), 1);
    float L = c.left + 30, Rr = c.right - 30, Y = c.top + 26;
    auto Heading = [&](const wstring& s) { Text(s, R(L, Y, Rr, Y + 34), 22, Fg(), DWRITE_FONT_WEIGHT_SEMI_BOLD, true); Y += 44; };
    auto Para = [&](const wstring& s, float size = 13.5f, D2D1_COLOR_F col = {}) { if (col.a == 0) col = Fg(); float h = TextH(s, Rr - L, size); Text(s, R(L, Y, Rr, Y + h + 4), size, col); Y += h + 12; };
    bool nextOn = true; wstring next = T(L"Continuar", L"Continue"), back = T(L"Voltar", L"Go Back"); bool showBack = true;

    switch (g_page) {
    case P_INTRO:
        Heading(T(L"Bem-vindo ao instalador do Liquinix", L"Welcome to the Liquinix Installer"));
        Para(T(L"O Liquinix traz o macOS para o Windows 11: barra de menus, Dock, Finder, Spotlight, Central de Controle e Liquid Glass, em C++ nativo.",
               L"Liquinix brings macOS to Windows 11: menu bar, Dock, Finder, Spotlight, Control Center and Liquid Glass, in native C++."));
        Para(T(L"Você vai escolher se instala o Liquinix completo, só os apps ou só as barras de título do Mac. Tudo fica só para o seu usuário, sem administrador, e dá para desfazer depois em Aplicativos instalados.",
               L"You'll choose the full Liquinix, only the apps, or only the Mac title bars. Everything is installed for your user only, without administrator rights, and can be undone later in Installed apps."));
        if (!g_pkgVer.empty()) Para(wstring(T(L"Versão: ", L"Version: ")) + g_pkgVer, 12, Fg2());
        Para(T(L"O Liquinix é gratuito e de código aberto (GPL-3.0). Apple, macOS e os nomes e ícones relacionados são da Apple Inc.; este projeto não é afiliado à Apple.",
               L"Liquinix is free and open source (GPL-3.0). Apple, macOS and related names and icons belong to Apple Inc.; this project is not affiliated with Apple."), 11.5f, Fg2());
        showBack = false;
        break;
    case P_HOME: {
        Heading(L"Liquinix " + g_instVer);
        Para(T(L"O Liquinix já está instalado neste PC. O que você quer fazer?", L"Liquinix is already installed on this PC. What would you like to do?"));
        Y += 4;
        wstring upd = g_fromPkg && g_pkgVer != g_instVer && !g_pkgVer.empty() ? wstring(T(L"Atualizar para ", L"Update to ")) + g_pkgVer : wstring(T(L"Procurar atualizações", L"Check for updates"));
        wstring updD = g_fromPkg && g_pkgVer != g_instVer && !g_pkgVer.empty() ? wstring(T(L"Troca os programas por estes, com os seus ajustes e escolhas mantidos.", L"Replaces the programs with these ones; your settings and choices stay."))
                                                                                : wstring(T(L"Procura a versão mais nova no GitHub e instala, com os seus ajustes mantidos.", L"Looks for the latest version on GitHub and installs it; your settings stay."));
        RadioCard(R(L, Y, Rr, Y + 64), upd, updD, g_homeSel == 0, B_HOME0); Y += 76;
        RadioCard(R(L, Y, Rr, Y + 64), T(L"Mudar o que está instalado", L"Change what's installed"), T(L"Liquinix completo, só os apps ou só as barras de título.", L"Full Liquinix, only the apps, or only the title bars."), g_homeSel == 1, B_HOME0 + 1); Y += 76;
        RadioCard(R(L, Y, Rr, Y + 64), T(L"Desinstalar o Liquinix", L"Uninstall Liquinix"), T(L"Remove tudo e devolve o visual do Windows.", L"Removes everything and brings back the Windows look."), g_homeSel == 2, B_HOME0 + 2);
        showBack = false;
        break;
    }
    case P_TYPE: {
        Heading(T(L"Tipo de instalação", L"Installation Type"));
        struct M { const wchar_t* t; const wchar_t* d; };
        M ms[4] = {
            { T(L"Liquinix completo", L"Full Liquinix"), T(L"Barra de menus, Dock, Finder, Spotlight e todos os apps, no lugar da barra de tarefas.", L"Menu bar, Dock, Finder, Spotlight and all apps, replacing the taskbar.") },
            { T(L"Só os apps", L"Only the apps"), T(L"Os apps que você escolher, no Menu Iniciar. A sua barra de tarefas fica como está.", L"The apps you pick, in the Start menu. Your taskbar stays as it is.") },
            { T(L"Só as barras de título do Mac", L"Only the Mac title bars"), T(L"Os botões e a barra do Mac nas janelas dos outros programas.", L"The Mac buttons and title bar on other programs' windows.") },
            { T(L"Apps e barras de título", L"Apps and title bars"), T(L"Os dois juntos, sem trocar a barra de tarefas.", L"Both, without replacing the taskbar.") },
        };
        float cw = (Rr - L - 12) / 2;
        for (int i = 0; i < 4; i++) {
            float x = L + (i % 2) * (cw + 12), yy = Y + (i / 2) * 96;
            RadioCard(R(x, yy, x + cw, yy + 86), ms[i].t, ms[i].d, g_ch.mode == i, B_MODE0 + i);
        }
        Y += 202;
        bool full = g_ch.mode == M_FULL, bars = g_ch.mode == M_BARS || g_ch.mode == M_APPSBARS;
        if (full || bars) { CheckBox(L, Y, full ? T(L"Abrir o Liquinix ao entrar no Windows", L"Open Liquinix when you sign in") : T(L"Ligar as barras de título ao entrar no Windows", L"Turn on the title bars when you sign in"), g_ch.autostart, B_CHK0); Y += 28; }
        if (full && IsDir(g_src + L"\\cursor")) { CheckBox(L, Y, T(L"Usar o cursor do Mac", L"Use the Mac cursor"), g_ch.cursor, B_CHK0 + 1); Y += 28; }
        if (IsDir(g_src + L"\\fontes")) { CheckBox(L, Y, T(L"Instalar as fontes SF Pro (o visual fica igual ao do Mac)", L"Install the SF Pro fonts (for the exact Mac look)"), g_ch.fonts, B_CHK0 + 2); Y += 28; }
        if (g_ch.mode != M_BARS) { CheckBox(L, Y, T(L"Pôr o Música e o QuickTime em \"Abrir com\"", L"Add Music and QuickTime to \"Open with\""), g_ch.assoc, B_CHK0 + 3); Y += 28; }
        if (std::any_of(std::begin(OUR_PROCS), std::end(OUR_PROCS), [](const wchar_t* p) { return Running(p); }) || Running(L"LiquinixShell.exe"))
            Para(T(L"O Liquinix e os apps dele que estiverem abertos vão ser fechados durante a instalação.", L"Liquinix and its apps will be closed during installation."), 11.5f, Fg2());
        back = T(L"Voltar", L"Go Back");
        break;
    }
    case P_APPS: {
        Heading(T(L"Escolha os apps", L"Choose the apps"));
        float cw = (Rr - L) / 2; int n = 0;
        for (int i = 0; i < (int)g_apps.size(); i++) {
            if (!Exists(g_src + L"\\bin\\" + g_apps[i].exe + L".exe")) continue;
            float x = L + (n % 2) * cw, yy = Y + (n / 2) * 27;
            CheckBox(x, yy, g_apps[i].name, g_apps[i].on, B_APP0 + i, cw - 10); n++;
        }
        Y += ((n + 1) / 2) * 27 + 8;
        Button(R(L, Y, L + 140, Y + 26), T(L"Marcar todos", L"Select All"), B_ALL, false);
        Button(R(L + 150, Y, L + 290, Y + 26), T(L"Desmarcar todos", L"Select None"), B_NONE, false);
        nextOn = std::any_of(g_apps.begin(), g_apps.end(), [](const AppInfo& a) { return a.on; });
        next = T(L"Instalar", L"Install");
        break;
    }
    case P_UPDCHECK: {
        Heading(T(L"Atualização de Software", L"Software Update"));
        if (g_checking) {
            for (int i = 0; i < 12; i++) {
                float a = g_spin + i * 3.14159f / 6, al = 0.15f + 0.85f * ((11 - i) / 11.f);
                float cx = (L + Rr) / 2, cy = Y + 50;
                Line(cx + cosf(a) * 9, cy + sinf(a) * 9, cx + cosf(a) * 17, cy + sinf(a) * 17, g_dark ? C(0xFFFFFF, al) : C(0x000000, al * 0.6f), 2.6f);
            }
            Y += 86; Text(T(L"Procurando atualizações…", L"Checking for updates…"), R(L, Y, Rr, Y + 20), 13, Fg2(), DWRITE_FONT_WEIGHT_REGULAR, false, DWRITE_TEXT_ALIGNMENT_CENTER);
            nextOn = false;
        } else if (!g_relErr.empty()) { Para(g_relErr); next = T(L"Tentar de novo", L"Try Again"); }
        else if (g_rel.tag == g_instVer) {
            Para(wstring(T(L"O Liquinix está atualizado (", L"Liquinix is up to date (")) + g_instVer + L").");
            next = T(L"Fechar", L"Close");
        } else {
            Para(wstring(T(L"Uma versão nova está disponível: ", L"A new version is available: ")) + g_rel.tag + (g_rel.date.empty() ? L"" : L" (" + g_rel.date + L")"), 14, Fg());
            Para(wstring(T(L"Instalada: ", L"Installed: ")) + (g_instVer.empty() ? T(L"desconhecida", L"unknown") : g_instVer.c_str()) +
                 (g_rel.size ? L" · " + std::to_wstring(g_rel.size / 1048576) + L" MB" : L""), 12, Fg2());
            for (auto& n : g_rel.notes) { float h = TextH(n, Rr - L - 16, 12.5f); Circle(L + 4, Y + 9, 2.2f, Fg2()); Text(n, R(L + 14, Y, Rr, Y + h + 2), 12.5f, Fg()); Y += h + 6; if (Y > c.bottom - 30) break; }
            next = T(L"Atualizar", L"Update");
        }
        showBack = g_installed;
        break;
    }
    case P_UNINST:
        Heading(T(L"Desinstalar o Liquinix", L"Uninstall Liquinix"));
        Para(T(L"O Liquinix vai ser fechado e removido: programas, atalhos, início automático, fontes e cursor que ele colocou. A barra de tarefas e as janelas voltam ao visual do Windows.",
               L"Liquinix will be closed and removed: programs, shortcuts, autostart, and the fonts and cursor it installed. The taskbar and windows go back to the Windows look."));
        Y += 4;
        CheckBox(L, Y, T(L"Apagar também os meus ajustes do Liquinix", L"Also delete my Liquinix settings"), g_ch.delSettings, B_CHK0 + 4); Y += 28;
        CheckBox(L, Y, T(L"Limpar o cache de ícones do Windows (a área de trabalho pisca)", L"Clear the Windows icon cache (the desktop flickers)"), g_ch.cleanCache, B_CHK0 + 5); Y += 28;
        next = T(L"Desinstalar", L"Uninstall");
        showBack = g_installed && g_flow == F_UNINSTALL && !g_forceUninstallOnly;
        break;
    case P_WORK: {
        Heading(g_flow == F_UNINSTALL ? T(L"Desinstalando…", L"Uninstalling…") : g_flow == F_UPDATE ? T(L"Atualizando…", L"Updating…") : T(L"Instalando…", L"Installing…"));
        Y += 40;
        D2D1_RECT_F bar = R(L, Y, Rr, Y + 8);
        Fill(bar, 4, g_dark ? C(0xFFFFFF, 0.1f) : C(0x000000, 0.08f));
        float p = std::clamp(g_progress.load(), 0.f, 1.f);
        if (p > 0) Fill(R(L, Y, L + (Rr - L) * p, Y + 8), 4, Accent());
        Y += 20;
        wstring st; { std::lock_guard<std::mutex> k(g_mx); st = g_status; }
        Text(st, R(L, Y, Rr, Y + 20), 12.5f, Fg2());
        nextOn = false; showBack = false; next = T(L"Continuar", L"Continue");
        break;
    }
    case P_DONE: {
        float cx = (L + Rr) / 2;
        Y += 20;
        Circle(cx, Y + 34, 34, g_workOk ? C(0x30D158) : C(0xFF453A));
        if (g_workOk) Check(cx, Y + 34, 34, C(0xFFFFFF), 4.5f);
        else { Line(cx - 10, Y + 24, cx + 10, Y + 44, C(0xFFFFFF), 4.5f); Line(cx + 10, Y + 24, cx - 10, Y + 44, C(0xFFFFFF), 4.5f); }
        Y += 90;
        wstring h = !g_workOk ? T(L"Não deu certo", L"Something went wrong")
                  : g_flow == F_UNINSTALL ? T(L"O Liquinix foi desinstalado.", L"Liquinix was uninstalled.")
                  : g_flow == F_UPDATE ? T(L"O Liquinix foi atualizado.", L"Liquinix was updated.")
                  : T(L"A instalação foi concluída.", L"The installation was completed successfully.");
        Text(h, R(L, Y, Rr, Y + 30), 20, Fg(), DWRITE_FONT_WEIGHT_SEMI_BOLD, true, DWRITE_TEXT_ALIGNMENT_CENTER); Y += 40;
        wstring sub;
        if (!g_workOk) { std::lock_guard<std::mutex> k(g_mx); sub = g_error.empty() ? g_status : g_error; }
        else if (g_flow == F_UNINSTALL) { std::lock_guard<std::mutex> k(g_mx); sub = g_status; }
        else if (g_ch.mode == M_FULL || g_flow == F_UPDATE) sub = T(L"Para sair do Liquinix: Ctrl+Alt+Shift+F12\nEmergência (devolve o Explorador): Ctrl+Alt+Shift+F11", L"To quit Liquinix: Ctrl+Alt+Shift+F12\nEmergency exit (brings Explorer back): Ctrl+Alt+Shift+F11");
        else sub = T(L"Os apps estão no Menu Iniciar, na pasta Liquinix.", L"The apps are in the Start menu, in the Liquinix folder.");
        float sh = TextH(sub, Rr - L, 13);
        Text(sub, R(L, Y, Rr, Y + sh + 4), 13, Fg2(), DWRITE_FONT_WEIGHT_REGULAR, false, DWRITE_TEXT_ALIGNMENT_CENTER); Y += sh + 16;
        if (g_workOk && g_flow != F_UNINSTALL && (g_ch.mode == M_FULL || g_ch.mode == M_BARS || g_ch.mode == M_APPSBARS)) {
            CheckBox(cx - 110, Y, g_ch.mode == M_FULL ? T(L"Abrir o Liquinix agora", L"Open Liquinix now") : T(L"Ligar as barras de título agora", L"Turn on the title bars now"), g_ch.launch, B_CHK0 + 6, 260);
        }
        next = T(L"Fechar", L"Close"); showBack = false;
        break;
    }
    case P_ERROR: {
        Heading(T(L"Não dá para instalar daqui", L"Can't install from here"));
        wstring e; { std::lock_guard<std::mutex> k(g_mx); e = g_error; }
        Para(e);
        next = T(L"Abrir a página de downloads", L"Open the downloads page"); showBack = false;
        break;
    }
    }
    // barra de baixo
    float by = H - BOTTOM + 18;
    Button(R(W - 22 - 120, by, W - 22, by + 30), next, B_NEXT, true, nextOn && !g_busy);
    if (showBack && !g_busy) Button(R(W - 22 - 250, by, W - 22 - 130, by + 30), back, B_BACK, false);
    g_rt->EndDraw();
}

static void StartWork(std::function<void()> fn) {
    g_busy = true; g_workDone = false; g_workOk = false; g_progress = 0;
    { std::lock_guard<std::mutex> k(g_mx); g_error.clear(); g_status = T(L"Preparando…", L"Preparing…"); }
    g_page = P_WORK;
    std::thread([fn] { CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); fn(); CoUninitialize(); }).detach();
    SetTimer(g_wnd, 1, 50, nullptr);
}
static void StartCheck() {
    g_checking = true; g_page = P_UPDCHECK; g_flow = F_UPDATE;
    std::thread([] { CheckGitHub(); g_checking = false; }).detach();
    SetTimer(g_wnd, 1, 40, nullptr);
}

static void Next() {
    switch (g_page) {
    case P_INTRO: g_flow = F_INSTALL; g_page = P_TYPE; break;
    case P_HOME:
        if (g_homeSel == 0) {
            if (g_fromPkg && g_pkgVer != g_instVer && !g_pkgVer.empty()) { g_flow = F_UPDATE; StartWork([] { DoInstall(true); }); }
            else StartCheck();
        } else if (g_homeSel == 1) {
            g_flow = F_MODIFY; g_page = P_TYPE;
            auto l = Lines(ReadText(g_dest + L"\\modo.txt"));  // o que já estava
            if (!l.empty()) { wstring m = Trim(l[0]); for (int i = 0; i < 4; i++) if (m == MODE_ID[i]) g_ch.mode = (Mode)i; }
            if (l.size() > 1) for (auto& a : g_apps) a.on = std::any_of(l.begin() + 1, l.end(), [&](const wstring& x) { return Trim(x) == a.exe; });
            g_ch.autostart = TaskExists(g_ch.mode == M_FULL ? L"Liquinix" : L"Liquinix Title Bars");
        } else { g_flow = F_UNINSTALL; g_page = P_UNINST; }
        break;
    case P_TYPE:
        if (g_ch.mode == M_APPS || g_ch.mode == M_APPSBARS) g_page = P_APPS;
        else StartWork([] { DoInstall(false); });
        break;
    case P_APPS: StartWork([] { DoInstall(false); }); break;
    case P_UPDCHECK:
        if (!g_relErr.empty()) StartCheck();
        else if (g_rel.tag == g_instVer) PostMessageW(g_wnd, WM_CLOSE, 0, 0);
        else StartWork([] { DoUpdate(); });
        break;
    case P_UNINST: StartWork([] { DoUninstall(); }); break;
    case P_DONE:
        if (g_workOk && g_flow != F_UNINSTALL && g_ch.launch) {
            if (g_ch.mode == M_FULL || g_flow == F_UPDATE) { if (Exists(g_dest + L"\\bin\\LiquinixShell.exe") && (g_flow != F_UPDATE || Lines(ReadText(g_dest + L"\\modo.txt")).empty() || Trim(Lines(ReadText(g_dest + L"\\modo.txt"))[0]) == L"completo")) Launch(g_dest + L"\\bin\\LiquinixShell.exe"); }
            else if (g_ch.mode == M_BARS || g_ch.mode == M_APPSBARS) Launch(g_dest + L"\\bin\\LiquinixTitleBars.exe");
        }
        PostMessageW(g_wnd, WM_CLOSE, 0, 0);
        return;
    case P_ERROR: ShellExecuteW(nullptr, L"open", L"https://github.com/takasurazeem/Liquinix/releases/latest", nullptr, nullptr, SW_SHOWNORMAL); PostMessageW(g_wnd, WM_CLOSE, 0, 0); return;
    default: break;
    }
    InvalidateRect(g_wnd, nullptr, FALSE);
}
static void Back() {
    switch (g_page) {
    case P_TYPE: g_page = g_installed ? P_HOME : P_INTRO; break;
    case P_APPS: g_page = P_TYPE; break;
    case P_UPDCHECK: case P_UNINST: if (g_installed) { g_page = P_HOME; g_flow = F_INSTALL; } break;
    default: break;
    }
    InvalidateRect(g_wnd, nullptr, FALSE);
}
static void Click(int id) {
    if (id == B_CLOSE) { if (!g_busy) PostMessageW(g_wnd, WM_CLOSE, 0, 0); return; }
    if (id == B_MIN) { ShowWindow(g_wnd, SW_MINIMIZE); return; }
    if (id == B_NEXT) { Next(); return; }
    if (id == B_BACK) { Back(); return; }
    if (id == B_ALL || id == B_NONE) { for (auto& a : g_apps) a.on = id == B_ALL; }
    else if (id >= B_MODE0 && id < B_MODE0 + 4) {
        Mode before = g_ch.mode; g_ch.mode = (Mode)(id - B_MODE0);
        if (before != g_ch.mode) g_ch.autostart = g_ch.mode == M_FULL || g_ch.mode == M_BARS || g_ch.mode == M_APPSBARS;
    }
    else if (id >= B_HOME0 && id < B_HOME0 + 3) g_homeSel = id - B_HOME0;
    else if (id >= B_APP0 && id < B_APP0 + 100) { auto& a = g_apps[id - B_APP0]; a.on = !a.on; }
    else if (id == B_CHK0) g_ch.autostart = !g_ch.autostart;
    else if (id == B_CHK0 + 1) g_ch.cursor = !g_ch.cursor;
    else if (id == B_CHK0 + 2) g_ch.fonts = !g_ch.fonts;
    else if (id == B_CHK0 + 3) g_ch.assoc = !g_ch.assoc;
    else if (id == B_CHK0 + 4) g_ch.delSettings = !g_ch.delSettings;
    else if (id == B_CHK0 + 5) g_ch.cleanCache = !g_ch.cleanCache;
    else if (id == B_CHK0 + 6) g_ch.launch = !g_ch.launch;
    InvalidateRect(g_wnd, nullptr, FALSE);
}
static int HitAt(int x, int y) {
    float fx = x / g_k, fy = y / g_k; int r = -1;
    for (auto& h : g_hits) if (fx >= h.r.left && fx < h.r.right && fy >= h.r.top && fy < h.r.bottom) r = h.id;
    return r;
}

static void MakeTarget() {
    if (g_rt) return;
    RECT rc; GetClientRect(g_wnd, &rc);
    D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(); p.dpiX = p.dpiY = 96;
    g_d2->CreateHwndRenderTarget(p, D2D1::HwndRenderTargetProperties(g_wnd, D2D1::SizeU(rc.right, rc.bottom)), &g_hrt);
    g_rt = g_hrt;
    if (g_rt) g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
}

static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_NCCALCSIZE: if (w) return 0; break;  // sem a moldura do Windows: a nossa barra de título
    case WM_NCHITTEST: {
        POINT pt{ GET_X_LPARAM(l), GET_Y_LPARAM(l) }; ScreenToClient(h, &pt);
        if (pt.y < TB * g_k && HitAt(pt.x, pt.y) < 0) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); MakeTarget(); Paint(); EndPaint(h, &ps); if (g_rt) {} return 0; }
    case WM_SIZE: if (g_hrt) g_hrt->Resize(D2D1::SizeU(LOWORD(l), HIWORD(l))); return 0;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, h, 0 }; TrackMouseEvent(&t);
        int hv = HitAt(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        if (hv != g_hover) { g_hover = hv; SetCursor(LoadCursorW(nullptr, hv >= 0 ? IDC_HAND : IDC_ARROW)); InvalidateRect(h, nullptr, FALSE); }
        return 0;
    }
    case WM_SETCURSOR: if (LOWORD(l) == HTCLIENT) { SetCursor(LoadCursorW(nullptr, g_hover >= 0 ? IDC_HAND : IDC_ARROW)); return TRUE; } break;
    case WM_MOUSELEAVE: if (g_hover >= 0) { g_hover = -1; InvalidateRect(h, nullptr, FALSE); } return 0;
    case WM_LBUTTONDOWN: g_press = HitAt(GET_X_LPARAM(l), GET_Y_LPARAM(l)); SetCapture(h); InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_LBUTTONUP: { int id = HitAt(GET_X_LPARAM(l), GET_Y_LPARAM(l)); ReleaseCapture(); int p = g_press; g_press = -1; if (id >= 0 && id == p) Click(id); InvalidateRect(h, nullptr, FALSE); return 0; }
    case WM_KEYDOWN:
        if (w == VK_RETURN && !g_busy) Next();
        else if (w == VK_ESCAPE && !g_busy) PostMessageW(h, WM_CLOSE, 0, 0);
        return 0;
    case WM_TIMER:
        g_spin += 0.16f;
        if (g_page == P_WORK && g_workDone) { g_busy = false; g_page = P_DONE; KillTimer(h, 1); g_instVer = VersionOf(g_dest); }
        if (g_page == P_UPDCHECK && !g_checking) KillTimer(h, 1);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_SETTINGCHANGE: {
        DWORD v = 1, n = 4; RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &n);
        g_dark = v == 0; BOOL d = g_dark; DwmSetWindowAttribute(h, 20, &d, sizeof(d)); InvalidateRect(h, nullptr, FALSE); return 0;
    }
    case WM_DPICHANGED: {
        g_k = HIWORD(w) / 96.f; RECT* r = (RECT*)l;
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE); return 0;
    }
    case WM_CLOSE: if (g_busy) return 0; DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}


// --foto <pasta>: desenha cada tela num PNG (claro e escuro), sem instalar nada; para conferir o visual
static void Foto(const wstring& dir) {
    MkDirs(dir);
    IWICImagingFactory* wic = nullptr; CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    g_k = 1.5f;
    UINT pw = (UINT)(W * g_k), ph = (UINT)(H * g_k);
    g_instVer = L"build-13"; g_pkgVer = L"build-14"; g_installed = true; g_fromPkg = true;
    g_rel.tag = L"build-14"; g_rel.date = L"20/11/2026"; g_rel.size = 172ull << 20;
    g_rel.notes = { L"Dynamic Island shows on the desktop again", L"License: GPL-3.0 instead of MIT", L"Mac OS X Leopard theme, Game Mode, apps and title bars without the shell" };
    struct Shot { const wchar_t* name; Page page; Flow flow; Mode mode; float prog; bool ok; };
    Shot shots[] = {
        { L"1-intro", P_INTRO, F_INSTALL, M_FULL, 0, true }, { L"2-home", P_HOME, F_INSTALL, M_FULL, 0, true },
        { L"3-tipo", P_TYPE, F_INSTALL, M_FULL, 0, true }, { L"4-tipo-apps", P_TYPE, F_INSTALL, M_APPSBARS, 0, true },
        { L"5-apps", P_APPS, F_INSTALL, M_APPS, 0, true }, { L"6-instalando", P_WORK, F_INSTALL, M_FULL, 0.62f, true },
        { L"7-concluido", P_DONE, F_INSTALL, M_FULL, 1, true }, { L"8-atualizacao", P_UPDCHECK, F_UPDATE, M_FULL, 0, true },
        { L"9-desinstalar", P_UNINST, F_UNINSTALL, M_FULL, 0, true }, { L"10-erro", P_DONE, F_INSTALL, M_FULL, 1, false },
    };
    for (int dark = 0; dark < 2; dark++) for (auto& s : shots) {
        g_dark = dark; g_page = s.page; g_flow = s.flow; g_ch.mode = s.mode; g_progress = s.prog; g_workOk = s.ok;
        { std::lock_guard<std::mutex> k(g_mx); g_status = T(L"Copiando LiquinixFinder.exe", L"Copying LiquinixFinder.exe"); g_error = T(L"O Liquinix não fechou. Feche-o com Ctrl+Alt+Shift+F12 e tente de novo.", L"Liquinix didn't quit."); }
        IWICBitmap* bmp = nullptr; wic->CreateBitmap(pw, ph, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp);
        D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        g_rt = nullptr; g_d2->CreateWicBitmapRenderTarget(bmp, p, &g_rt);
        if (g_logo) { g_logo->Release(); g_logo = nullptr; }
        Paint();
        if (g_logo) { g_logo->Release(); g_logo = nullptr; }
        g_rt->Release(); g_rt = nullptr;
        wstring file = dir + L"\\" + s.name + (dark ? L"-escuro.png" : L"-claro.png");
        IWICStream* st = nullptr; IWICBitmapEncoder* enc = nullptr; IWICBitmapFrameEncode* fr = nullptr;
        wic->CreateStream(&st); st->InitializeFromFilename(file.c_str(), GENERIC_WRITE);
        wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc); enc->Initialize(st, WICBitmapEncoderNoCache);
        enc->CreateNewFrame(&fr, nullptr); fr->Initialize(nullptr); fr->SetSize(pw, ph);
        WICPixelFormatGUID f = GUID_WICPixelFormat32bppPBGRA; fr->SetPixelFormat(&f); fr->WriteSource(bmp, nullptr);
        fr->Commit(); enc->Commit(); fr->Release(); enc->Release(); st->Release(); bmp->Release();
    }
    wic->Release();
}

static void LoadLogo(HINSTANCE hi) {  // o logo, se o build embutir um (recurso 2); sem ele, a barra lateral fica sem imagem
    HRSRC r = FindResourceW(hi, MAKEINTRESOURCEW(2), RT_RCDATA); if (!r) return;
    HGLOBAL g = LoadResource(hi, r); DWORD n = SizeofResource(hi, r); void* p = LockResource(g);
    IWICImagingFactory* wic = nullptr; if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return;
    IWICStream* s = nullptr; IWICBitmapDecoder* d = nullptr; IWICBitmapFrameDecode* f = nullptr; IWICFormatConverter* c = nullptr;
    if (SUCCEEDED(wic->CreateStream(&s)) && SUCCEEDED(s->InitializeFromMemory((BYTE*)p, n)) &&
        SUCCEEDED(wic->CreateDecoderFromStream(s, nullptr, WICDecodeMetadataCacheOnLoad, &d)) && SUCCEEDED(d->GetFrame(0, &f)) &&
        SUCCEEDED(wic->CreateFormatConverter(&c)) && SUCCEEDED(c->Initialize(f, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
        g_logoSrc = c; c = nullptr;
    }
    if (c) c->Release(); if (f) f->Release(); if (d) d->Release(); if (s) s->Release(); wic->Release();
}

int WINAPI wWinMain(HINSTANCE hi, HINSTANCE, LPWSTR cmdLine, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    g_pt = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_PORTUGUESE;
    wchar_t me[MAX_PATH]; GetModuleFileNameW(nullptr, me, MAX_PATH); g_self = me; g_selfDir = Parent(g_self);
    g_dest = Env(L"LOCALAPPDATA") + L"\\Programs\\Liquinix";
    g_log = g_dest + L"\\instalacao.log";
    wstring args = cmdLine ? cmdLine : L"";
    bool wantUninst = args.find(L"--desinstalar") != wstring::npos, wantUpdate = args.find(L"--atualizar") != wstring::npos;

    // desinstalar rodando de dentro da pasta que vai ser apagada: uma cópia no TEMP faz o trabalho
    if (wantUninst && Lower(g_self).rfind(Lower(g_dest + L"\\"), 0) == 0) {
        wstring c = Env(L"TEMP") + L"\\LiquinixSetup-desinstalar.exe";
        if (CopyFileW(g_self.c_str(), c.c_str(), FALSE)) { ShellExecuteW(nullptr, L"open", c.c_str(), L"--desinstalar", nullptr, SW_SHOWNORMAL); return 0; }
    }
    if (Lower(g_self).find(Lower(Env(L"TEMP"))) == 0) MoveFileExW(g_self.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);

    // de onde vêm os programas: o pacote ao lado deste exe, ou a própria instalação
    g_fromPkg = Exists(g_selfDir + L"\\bin\\LiquinixShell.exe") && _wcsicmp(g_selfDir.c_str(), g_dest.c_str());
    g_src = g_fromPkg ? g_selfDir : g_dest;
    g_installed = Exists(g_dest + L"\\bin\\LiquinixShell.exe") || Exists(g_dest + L"\\bin\\LiquinixFinder.exe");
    g_instVer = VersionOf(g_dest); g_pkgVer = g_fromPkg ? VersionOf(g_src) : L"";

    if (wantUninst) { g_flow = F_UNINSTALL; g_page = P_UNINST; g_forceUninstallOnly = true; }
    else if (wantUpdate) StartCheck(), g_flow = F_UPDATE;
    else if (g_installed) g_page = P_HOME;
    else if (!g_fromPkg) {
        g_page = P_ERROR;
        Fail(T(L"Esta pasta não tem os programas do Liquinix. Baixe o pacote pronto (Liquinix-<data>.zip) na página de downloads, extraia e abra o LiquinixSetup.exe de dentro dele.",
               L"This folder doesn't have the Liquinix programs. Download the ready-made package (Liquinix-<date>.zip) from the downloads page, extract it and open LiquinixSetup.exe from inside it."));
    }

    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&g_dw);
    {   // SF Pro se estiver instalada
        IDWriteFontCollection* fc = nullptr; UINT32 idx; BOOL ex = FALSE;
        if (SUCCEEDED(g_dw->GetSystemFontCollection(&fc, TRUE))) {
            if (SUCCEEDED(fc->FindFamilyName(L"SF Pro Text", &idx, &ex)) && ex) g_fontUI = L"SF Pro Text";
            if (SUCCEEDED(fc->FindFamilyName(L"SF Pro Display", &idx, &ex)) && ex) g_fontTitle = L"SF Pro Display";
            fc->Release();
        }
    }
    LoadLogo(hi);
    // --diagnostico <arquivo>: consulta o GitHub, baixa o pacote, confere o SHA-256 e extrai numa pasta temporária,
    // sem instalar nada (para testar a atualização sem mexer na instalação)
    if (size_t dp = args.find(L"--diagnostico"); dp != wstring::npos) {
        wstring out = Trim(args.substr(dp + 13)); if (!out.empty() && out[0] == L'"') out = out.substr(1, out.find(L'"', 1) - 1);
        wstring r; CheckGitHub();
        r += L"tag=" + g_rel.tag + L"\r\ndata=" + g_rel.date + L"\r\nurl=" + g_rel.url + L"\r\nsha=" + g_rel.sha + L"\r\nmb=" + std::to_wstring(g_rel.size >> 20) + L"\r\nerro=" + g_relErr + L"\r\n";
        for (auto& n : g_rel.notes) r += L"nota=" + n + L"\r\n";
        if (g_relErr.empty()) {
            wstring tmp = Env(L"TEMP") + L"\\liquinix-diag"; MkDirs(tmp); wstring zip = tmp + L"\\p.zip";
            size_t hs = g_rel.url.find(L"://") + 3, ps = g_rel.url.find(L'/', hs);
            bool ok = !HttpGet(g_rel.url.substr(hs, ps - hs), g_rel.url.substr(ps), L"application/octet-stream", zip).empty();
            wstring sha = ok ? Sha256File(zip) : L"";
            r += L"baixou=" + std::to_wstring(ok) + L"\r\nsha_confere=" + std::to_wstring(!sha.empty() && sha == g_rel.sha) + L"\r\n";
            wchar_t sys[MAX_PATH]; GetSystemDirectoryW(sys, MAX_PATH);
            DWORD ec = RunWait(wstring(sys) + L"\\tar.exe", L"-xf \"" + zip + L"\" -C \"" + tmp + L"\"", 600000);
            r += L"tar=" + std::to_wstring(ec) + L"\r\nbin_shell=" + std::to_wstring(Exists(tmp + L"\\bin\\LiquinixShell.exe")) + L"\r\nversao=" + VersionOf(tmp) + L"\r\n";
            wstring t2 = tmp; t2.push_back(0); SHFILEOPSTRUCTW o{}; o.wFunc = FO_DELETE; o.pFrom = t2.c_str(); o.fFlags = FOF_NO_UI; SHFileOperationW(&o);
        }
        WriteText(out, r); return 0;
    }
    // --teste <instalar|atualizar|desinstalar> <arquivo>: faz o trabalho sem a janela, com as escolhas tiradas do
    // que já está no PC (início automático, Abrir com, modo.txt), e grava o resultado no arquivo
    if (size_t tp = args.find(L"--teste"); tp != wstring::npos) {
        wstring rest = Trim(args.substr(tp + 7)); size_t sp = rest.find(L' ');
        wstring acao = rest.substr(0, sp), out = sp == wstring::npos ? L"" : Trim(rest.substr(sp + 1));
        if (!out.empty() && out[0] == L'"') out = out.substr(1, out.find(L'"', 1) - 1);
        g_ch.autostart = TaskExists(L"Liquinix") || TaskExists(L"Liquinix Title Bars") || !g_installed;
        HKEY k; g_ch.assoc = !RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Classes\\SequoiaMusic.Audio", 0, KEY_READ, &k); if (g_ch.assoc) RegCloseKey(k);
        // cursor: só se o atual for o do Windows (um cursor do Mac já em uso fica como está)
        g_ch.cursor = Lower(RegGetStr(HKEY_CURRENT_USER, L"Control Panel\\Cursors", L"Arrow")).find(L"\\windows\\cursors\\") != wstring::npos ||
                      RegGetStr(HKEY_CURRENT_USER, L"Control Panel\\Cursors", L"Arrow").empty();
        auto modo = Lines(ReadText(g_dest + L"\\modo.txt"));
        if (!modo.empty()) for (int m = 0; m < 4; m++) if (Trim(modo[0]) == MODE_ID[m]) g_ch.mode = (Mode)m;
        wstring r = L"acao=" + acao + L"\r\norigem=" + g_src + L"\r\nautostart=" + std::to_wstring(g_ch.autostart) + L"\r\nassoc=" + std::to_wstring(g_ch.assoc) +
                    L"\r\ncursor=" + std::to_wstring(g_ch.cursor) + L"\r\nmodo=" + MODE_ID[g_ch.mode] + L"\r\n";
        DWORD t0 = GetTickCount();
        if (acao == L"atualizar") { CheckGitHub(); r += L"tag=" + g_rel.tag + L"\r\nerro_github=" + g_relErr + L"\r\n"; if (g_relErr.empty()) DoUpdate(); }
        else if (acao == L"desinstalar") DoUninstall();
        else if (acao == L"instalar") { if (g_fromPkg) DoInstall(false); else Fail(L"sem pacote ao lado do exe"); }
        r += L"ok=" + std::to_wstring((bool)g_workOk) + L"\r\nstatus=" + g_status + L"\r\nerro=" + g_error + L"\r\nsegundos=" + std::to_wstring((GetTickCount() - t0) / 1000) + L"\r\n";
        if (!out.empty()) WriteText(out, r);
        return g_workOk ? 0 : 1;
    }
    if (size_t fp = args.find(L"--foto"); fp != wstring::npos) { wstring d = Trim(args.substr(fp + 6)); if (!d.empty() && d[0] == L'"') d = d.substr(1, d.find(L'"', 1) - 1); Foto(d); return 0; }
    DWORD v = 1, n = 4; RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize", L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &n);
    g_dark = v == 0;

    WNDCLASSW wc{}; wc.lpfnWndProc = Proc; wc.hInstance = hi; wc.lpszClassName = L"LiquinixSetup"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hi, MAKEINTRESOURCEW(1));
    RegisterClassW(&wc);
    POINT c; GetCursorPos(&c); HMONITOR mon = MonitorFromPoint(c, MONITOR_DEFAULTTOPRIMARY);
    UINT dx = 96, dy = 96; GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy); g_k = dx / 96.f;
    MONITORINFO mi{ sizeof(mi) }; GetMonitorInfoW(mon, &mi);
    int ww = (int)(W * g_k), wh = (int)(H * g_k);
    int x = (mi.rcWork.left + mi.rcWork.right - ww) / 2, y = (mi.rcWork.top + mi.rcWork.bottom - wh) / 2;
    g_wnd = CreateWindowExW(0, wc.lpszClassName, T(L"Instalar o Liquinix", L"Install Liquinix"), WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                            x, y, ww, wh, nullptr, nullptr, hi, nullptr);
    MARGINS mg{ 0, 0, 1, 0 }; DwmExtendFrameIntoClientArea(g_wnd, &mg);
    DWORD corner = 2; DwmSetWindowAttribute(g_wnd, 33, &corner, sizeof(corner));
    BOOL d = g_dark; DwmSetWindowAttribute(g_wnd, 20, &d, sizeof(d));
    SetWindowPos(g_wnd, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    ShowWindow(g_wnd, SW_SHOW);
    if (g_checking) SetTimer(g_wnd, 1, 40, nullptr);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    CoUninitialize();
    return 0;
}
