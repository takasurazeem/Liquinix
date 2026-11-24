// Troca de nome Sequoia → WacOS (16/11/2026): os programas passaram de SequoiaX.exe para WacOSX.exe e a pasta de
// instalação de %LOCALAPPDATA%\Programs\SequoiaShell para ...\Programs\WacOS. O que o Windows guardava com o nome
// velho (o Shell do Windows no Winlogon, o Win+E e o .zip do Finder, os apps Música e QuickTime, os diálogos de
// arquivo, a tarefa que abre o Shell no login) é corrigido aqui, na primeira vez que o WacOSShell abre: cada caminho
// só muda se o arquivo com o nome novo existir. Os nomes internos (ProgIDs SequoiaMusic.Audio, a chave
// Software\SequoiaShell, classes de janela) ficam como estão, para não desfazer as associações de arquivo.
#include "shell.h"
#include <regex>
#include <thread>

namespace {

std::wstring Novo(const std::wstring& v) {
    static const std::wregex dir(L"\\\\Programs\\\\SequoiaShell\\\\", std::regex::icase);
    static const std::wregex nome(L"\\\\Sequoia(Shell|Finder|Settings|ActivityMonitor|Terminal|Calculator|Notes|Screenshot|Preview|"
                                  L"Reminders|Calendar|TextEdit|Clock|Weather|Contacts|Photos|TimeMachine|Music|QuickTime|ForceQuit|"
                                  L"Window32Host|Window32|Window)\\.(exe|dll)", std::regex::icase);
    std::wstring s = std::regex_replace(v, dir, L"\\Programs\\WacOS\\");
    s = std::regex_replace(s, nome, L"\\WacOS$1.$2");
    if (s == v) return v;
    // o arquivo novo tem que existir: o caminho vai até o .exe/.dll (entre aspas ou não)
    std::wstring low = s;
    for (auto& c : low) c = towlower(c);
    size_t end = low.find(L".exe");
    size_t e2 = low.find(L".dll");
    if (end == std::wstring::npos || (e2 != std::wstring::npos && e2 < end)) end = e2;
    if (end == std::wstring::npos) return v;
    size_t start = s[0] == L'"' ? 1 : 0;
    std::wstring path = s.substr(start, end + 4 - start);
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return v;
    return s;
}

int Corrige(HKEY root, const std::wstring& key, const wchar_t* onlyValue, bool recurse, int depth = 0) {
    HKEY h;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_READ | KEY_SET_VALUE, &h) != ERROR_SUCCESS) return 0;
    int n = 0;
    for (DWORD i = 0;; i++) {
        wchar_t name[256]; DWORD nl = 256, type = 0, cb = 0;
        if (RegEnumValueW(h, i, name, &nl, nullptr, &type, nullptr, &cb) != ERROR_SUCCESS) break;
        if (type != REG_SZ && type != REG_EXPAND_SZ) continue;
        if (onlyValue && _wcsicmp(name, onlyValue)) continue;
        std::vector<wchar_t> buf(cb / 2 + 2, 0);
        DWORD cb2 = cb;
        if (RegQueryValueExW(h, name, nullptr, nullptr, (BYTE*)buf.data(), &cb2) != ERROR_SUCCESS) continue;
        std::wstring v = buf.data(), nv = Novo(v);
        if (nv == v) continue;
        if (RegSetValueExW(h, name, 0, type, (const BYTE*)nv.c_str(), (DWORD)((nv.size() + 1) * 2)) == ERROR_SUCCESS) {
            Log(L"wacos: registro HKCU\\%ls [%ls]: %ls -> %ls", key.c_str(), *name ? name : L"(padrão)", v.c_str(), nv.c_str());
            n++;
        }
    }
    if (recurse && depth < 6) {
        std::vector<std::wstring> subs;
        for (DWORD i = 0;; i++) {
            wchar_t sub[256]; DWORD sl = 256;
            if (RegEnumKeyExW(h, i, sub, &sl, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            subs.push_back(sub);
        }
        for (auto& s : subs) n += Corrige(root, key + L"\\" + s, nullptr, true, depth + 1);
    }
    RegCloseKey(h);
    return n;
}

DWORD RunHidden(std::wstring cmd, DWORD wait) {
    STARTUPINFOW si{ sizeof(si) }; si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return (DWORD)-1;
    WaitForSingleObject(pi.hProcess, wait);
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return code;
}

}  // namespace

// Rodando com o nome velho (o reinício ou o login abriram SequoiaShell.exe) e o WacOSShell.exe ao lado: abre o novo
// com os mesmos argumentos e sai. true = quem chamou deve sair já.
bool migrar::TrocarPeloNovo() {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring p = exe, base = p.substr(p.find_last_of(L'\\') + 1);
    if (_wcsicmp(base.c_str(), L"SequoiaShell.exe")) return false;
    std::wstring novo = p.substr(0, p.find_last_of(L'\\')) + L"\\WacOSShell.exe";
    if (GetFileAttributesW(novo.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    // os argumentos (sem o caminho do programa)
    const wchar_t* cl = GetCommandLineW();
    if (*cl == L'"') { cl = wcschr(cl + 1, L'"'); cl = cl ? cl + 1 : L""; }
    else { while (*cl && *cl != L' ') cl++; }
    std::wstring args = cl;
    if (args.find(L"--depois-de ") == std::wstring::npos) args += L" --depois-de " + std::to_wstring(GetCurrentProcessId());
    std::wstring cmd = L"\"" + novo + L"\"" + args;
    STARTUPINFOW si{ sizeof(si) }; PROCESS_INFORMATION pi{};
    std::wstring dir = p.substr(0, p.find_last_of(L'\\'));
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) return false;
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return true;
}

void migrar::Registro() {
    int n = 0;
    n += Corrige(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Winlogon", L"Shell", false);
    n += Corrige(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", nullptr, false);
    for (auto k : { L"Software\\Classes\\CompressedFolder\\shell", L"Software\\Classes\\CLSID\\{52205fd8-5dfb-447d-801a-d0b52f2e83e1}",
                    L"Software\\Classes\\CLSID\\{DC1C5A9C-E88A-4DDE-A5A1-60F82A20AEF7}", L"Software\\Classes\\CLSID\\{C0B4E2F3-BA21-4773-8DBA-335EC9468EB8}",
                    L"Software\\Classes\\SequoiaMusic.Audio", L"Software\\Classes\\SequoiaQuickTime.Video",
                    L"Software\\Classes\\Applications\\SequoiaMusic.exe", L"Software\\Classes\\Applications\\SequoiaQuickTime.exe",
                    L"Software\\SequoiaShell" })
        n += Corrige(HKEY_CURRENT_USER, k, nullptr, true);
    if (n) Log(L"wacos: %d caminho(s) do nome velho corrigido(s) no registro", n);
    // a tarefa que abre o Shell no login: "SequoiaShell" vira "WacOS" (uma vez; em segundo plano)
    if (shell::Setting(L"wacos_tarefa", L"0") == L"1") return;
    shell::SetSetting(L"wacos_tarefa", L"1");
    std::thread([] {
        std::wstring ps =
            L"$t = Get-ScheduledTask -TaskName SequoiaShell -ErrorAction SilentlyContinue; if (-not $t) { exit 2 }; "
            L"$x = $t.Actions[0].Execute -replace '\\\\Programs\\\\SequoiaShell\\\\', '\\Programs\\WacOS\\' -replace 'SequoiaShell\\.exe$', 'WacOSShell.exe'; "
            L"if (-not (Test-Path $x)) { exit 3 }; "
            L"$a = if ($t.Actions[0].Arguments) { New-ScheduledTaskAction -Execute $x -Argument $t.Actions[0].Arguments } else { New-ScheduledTaskAction -Execute $x }; "
            L"Register-ScheduledTask -TaskName WacOS -Action $a -Trigger $t.Triggers -Settings $t.Settings -Principal $t.Principal -Force | Out-Null; "
            L"Unregister-ScheduledTask -TaskName SequoiaShell -Confirm:$false; exit 0";
        DWORD r = RunHidden(L"powershell.exe -NoProfile -ExecutionPolicy Bypass -Command \"" + ps + L"\"", 60000);
        Log(L"wacos: tarefa do login %ls", r == 0 ? L"SequoiaShell trocada por WacOS" : r == 2 ? L"SequoiaShell não existe (nada a trocar)" : r == 3 ? L"não trocada: o WacOSShell.exe novo não está no lugar" : L"não trocada (erro)");
        if (r == 3) shell::SetSetting(L"wacos_tarefa", L"0");  // tenta de novo quando a instalação nova existir
    }).detach();
}
