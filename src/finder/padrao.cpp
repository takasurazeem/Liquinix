// Finder no lugar do Explorador para o .zip (duplo clique descompacta) e o Win+E (só HKCU, reversível).
// Cria o verbo "arquivos" em CompressedFolder e o torna o verbo padrão; troca o comando "nova janela" do
// Explorador (Win+E). As pastas comuns vão para o Finder pelo Shell (shell\pastas.cpp). O que existia antes fica anotado em
// %APPDATA%\Arquivos\padrao-backup.txt para ser restaurado.
#include "finder.h"
#include <shlobj.h>
#include <cstdio>

namespace padrao {

// Só o .zip (duplo clique descompacta, como no Mac) e o Win+E. Pastas e discos NÃO passam mais por aqui:
// o verbo de pasta só recebe a pasta, sem o arquivo, e o "Mostrar na pasta" dos apps (Claude, Chrome:
// SHOpenFolderAndSelectItems) ficava esperando a janela do Explorador para marcar o arquivo, desistia depois
// de ~20 s e o Finder abria sem a seleção, às vezes atrás. Agora o Explorador abre a pasta já com a seleção
// e o Shell troca a janela pelo Finder (shell\pastas.cpp), que abre no mesmo lugar e com o mesmo arquivo marcado.
static const wchar_t* KEYS[] = { L"Software\\Classes\\CompressedFolder\\shell" };
static const wchar_t* LEGACY[] = { L"Software\\Classes\\Directory\\shell", L"Software\\Classes\\Drive\\shell" };  // versões antigas

// tira o verbo "arquivos" das versões antigas (pastas e discos) e o verbo padrão que apontava para ele
static void CleanLegacy(FILE* log) {
    for (auto* k : LEGACY) {
        std::wstring verb = std::wstring(k) + L"\\arquivos";
        HKEY h;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, verb.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) continue;
        RegCloseKey(h);
        RegDeleteTreeW(HKEY_CURRENT_USER, verb.c_str());
        wchar_t buf[64]; DWORD cb = sizeof(buf);
        if (RegGetValueW(HKEY_CURRENT_USER, k, nullptr, RRF_RT_REG_SZ, nullptr, buf, &cb) == ERROR_SUCCESS && !wcscmp(buf, L"arquivos") &&
            RegOpenKeyExW(HKEY_CURRENT_USER, k, 0, KEY_SET_VALUE, &h) == ERROR_SUCCESS) { RegDeleteValueW(h, nullptr); RegCloseKey(h); }
        RegDeleteKeyW(HKEY_CURRENT_USER, k);  // só some se ficou vazia
        if (log) fwprintf(log, L"registro: removido HKCU\\%ls (versão antiga: pastas pelo verbo)\n", verb.c_str());
    }
}
static const wchar_t* WINE_KEY = L"Software\\Classes\\CLSID\\{52205fd8-5dfb-447d-801a-d0b52f2e83e1}";  // Win+E
static const wchar_t* WINE_CMD = L"Software\\Classes\\CLSID\\{52205fd8-5dfb-447d-801a-d0b52f2e83e1}\\shell\\opennewwindow\\command";

static std::wstring BackupPath() {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) { out = std::wstring(p) + L"\\Arquivos"; CoTaskMemFree(p); }
    CreateDirectoryW(out.c_str(), nullptr);
    return out + L"\\padrao-backup.txt";
}

static bool ReadDefault(const wchar_t* key, std::wstring& v) {
    wchar_t buf[512]; DWORD cb = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, key, nullptr, RRF_RT_REG_SZ, nullptr, buf, &cb) != ERROR_SUCCESS) return false;
    v = buf;
    return true;
}

static bool KeyExists(const wchar_t* key) {
    HKEY h;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    RegCloseKey(h);
    return true;
}

static bool SetString(const std::wstring& key, const wchar_t* name, const std::wstring& val) {
    return RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), name, REG_SZ, val.c_str(), (DWORD)((val.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

bool Enabled() {
    std::wstring v;
    return ReadDefault(KEYS[0], v) && v == L"arquivos";
}

// Liga. Cada parte só é feita (e anotada no backup) se ainda não estiver feita.
bool Enable(FILE* log) {
    CleanLegacy(log);
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    // --abrir: um .zip (que o Windows abre como pasta) chega aqui; o Finder descompacta em vez de só selecionar
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\" --abrir \"%1\"";
    bool ok = true;
    FILE* f = nullptr;
    _wfopen_s(&f, BackupPath().c_str(), L"a, ccs=UTF-8");
    if (!Enabled()) {
        for (auto* k : KEYS) {
            std::wstring old;
            bool had = ReadDefault(k, old);
            if (f) fwprintf(f, L"%ls\t%ls\t%ls\n", k, had ? L"1" : L"0", old.c_str());
            std::wstring verb = std::wstring(k) + L"\\arquivos";
            ok &= SetString(verb, nullptr, L"Abrir no Finder");
            ok &= SetString(verb + L"\\command", nullptr, cmd);
            ok &= SetString(k, nullptr, L"arquivos");
            if (log) fwprintf(log, L"registro: HKCU\\%ls = arquivos; %ls\\command = %ls\n", k, verb.c_str(), cmd.c_str());
        }
    } else {  // já ligado: o programa pode ter mudado de nome/lugar (Arquivos.exe → WacOSFinder.exe): atualiza o caminho
        for (auto* k : KEYS) {
            std::wstring verb = std::wstring(k) + L"\\arquivos\\command", cur;
            if (ReadDefault(verb.c_str(), cur) && cur == cmd) continue;
            ok &= SetString(verb, nullptr, cmd);
            if (log) fwprintf(log, L"registro: HKCU\\%ls = %ls (caminho atualizado)\n", verb.c_str(), cmd.c_str());
        }
    }
    if (KeyExists(WINE_CMD)) {  // Win+E já é nosso: confere o caminho também
        std::wstring c = L"\"" + std::wstring(exe) + L"\"", cur;
        if (ReadDefault(WINE_CMD, cur) && cur != c && cur.find(L"shell-proprio") != std::wstring::npos) ok &= SetString(WINE_CMD, nullptr, c);
    }
    if (!KeyExists(WINE_CMD)) {  // Win+E: o comando "nova janela" do Explorador passa a abrir o Arquivos
        bool existed = KeyExists(WINE_KEY);
        if (f) fwprintf(f, L"%ls\t%ls\t\n", WINE_KEY, existed ? L"existia" : L"novo");
        std::wstring c = L"\"" + std::wstring(exe) + L"\"";
        ok &= SetString(WINE_CMD, nullptr, c);
        ok &= SetString(WINE_CMD, L"DelegateExecute", L"");
        if (log) fwprintf(log, L"registro: HKCU\\%ls = %ls (Win+E)\n", WINE_CMD, c.c_str());
    }
    if (f) fclose(f);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok;
}

// Desliga: apaga o que criamos e devolve o que havia antes.
bool Disable(FILE* log) {
    CleanLegacy(log);
    std::wstring bk = BackupPath();
    for (auto* k : KEYS) {
        RegDeleteTreeW(HKEY_CURRENT_USER, (std::wstring(k) + L"\\arquivos").c_str());
        HKEY h;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, k, 0, KEY_SET_VALUE, &h) == ERROR_SUCCESS) { RegDeleteValueW(h, nullptr); RegCloseKey(h); }
        if (log) fwprintf(log, L"registro: removido HKCU\\%ls\\arquivos e o verbo padrão\n", k);
    }
    bool wineOurs = false, wineExisted = false;
    FILE* f = nullptr;
    if (_wfopen_s(&f, bk.c_str(), L"r, ccs=UTF-8") == 0 && f) {
        wchar_t line[1024];
        while (fgetws(line, 1024, f)) {
            std::wstring s = line;
            while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r')) s.pop_back();
            size_t a = s.find(L'\t'), b = s.find(L'\t', a + 1);
            if (a == std::wstring::npos || b == std::wstring::npos) continue;
            std::wstring key = s.substr(0, a), flag = s.substr(a + 1, b - a - 1), val = s.substr(b + 1);
            if (key == WINE_KEY) { wineOurs |= flag == L"novo"; wineExisted |= flag == L"existia"; continue; }
            if (flag == L"1") SetString(key, nullptr, val);  // verbo padrão que havia antes
        }
        fclose(f);
    }
    if (wineOurs && !wineExisted) RegDeleteTreeW(HKEY_CURRENT_USER, WINE_KEY);  // a chave inteira era nossa
    else RegDeleteTreeW(HKEY_CURRENT_USER, (std::wstring(WINE_KEY) + L"\\shell\\opennewwindow").c_str());
    if (log) fwprintf(log, L"registro: removido o Win+E do Finder\n");
    for (auto* k : KEYS) RegDeleteKeyW(HKEY_CURRENT_USER, k);  // só some se ficou vazia
    DeleteFileW(bk.c_str());
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return !Enabled();
}

}  // namespace padrao
