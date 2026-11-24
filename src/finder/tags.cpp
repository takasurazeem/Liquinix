// Etiquetas coloridas, como no Finder. Ficam gravadas no próprio arquivo, num fluxo
// alternativo do NTFS ("arquivo:Arquivos.Etiquetas"), o equivalente aos atributos estendidos
// do Mac: a etiqueta acompanha o arquivo se ele for movido ou renomeado no mesmo disco.
// Gravar o fluxo mudaria a data de modificação; as datas originais são restauradas.
// Um índice em %APPDATA%\Arquivos\etiquetas.txt guarda onde há etiquetas, para a barra lateral
// listar todos os itens de uma cor sem varrer o disco.
#include "finder.h"
#include <shlobj.h>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <map>

namespace tags {

const wchar_t* names[COUNT] = { L"Vermelho", L"Laranja", L"Amarelo", L"Verde", L"Azul", L"Roxo", L"Cinza" };
UINT32 colors[COUNT] = { 0xFF453A, 0xFF9F0A, 0xFFD60A, 0x30D158, 0x0A84FF, 0xBF5AF2, 0x98989D };
// Tahoe: as cores do sistema do macOS 26 são mais vivas (guia macOS26-Theme-Reference, seção 12, medido):
// vermelho FF383C/FF4245, laranja FF8D28/FF9230, amarelo FFCC00/FFD600, verde 34C759/30D158,
// azul 0088FF/0091FF, roxo CB30E0/DB34F2, cinza 8E8E93/98989D (claro/escuro)
void Tema(bool tahoe, bool dark) {
    static const UINT32 seq[COUNT] = { 0xFF453A, 0xFF9F0A, 0xFFD60A, 0x30D158, 0x0A84FF, 0xBF5AF2, 0x98989D };
    static const UINT32 th[2][COUNT] = { { 0xFF383C, 0xFF8D28, 0xFFCC00, 0x34C759, 0x0088FF, 0xCB30E0, 0x8E8E93 },
                                         { 0xFF4245, 0xFF9230, 0xFFD600, 0x30D158, 0x0091FF, 0xDB34F2, 0x98989D } };
    for (int i = 0; i < COUNT; i++) colors[i] = tahoe ? th[dark ? 1 : 0][i] : seq[i];
}

namespace {
std::map<std::wstring, std::pair<unsigned, std::wstring>> index;  // caminho em minúsculas -> (máscara, caminho original)
bool loaded = false;

std::wstring AppDataFile(const wchar_t* name) {
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) dir = std::wstring(p) + L"\\Arquivos";
    CoTaskMemFree(p);
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\" + name;
}

std::wstring Key(const std::wstring& path) {
    std::wstring k = path;
    for (auto& c : k) c = (wchar_t)towlower(c);
    return k;
}

bool Supported(const std::wstring& path) {  // só discos locais (fluxos em rede são lentos ou não existem)
    if (path.size() < 3 || path[1] != L':') return false;
    wchar_t root[4] = { path[0], L':', L'\\', 0 };
    UINT t = GetDriveTypeW(root);
    return t == DRIVE_FIXED || t == DRIVE_REMOVABLE;
}

void SaveIndex() {
    FILE* f = nullptr;
    if (_wfopen_s(&f, AppDataFile(L"etiquetas.txt").c_str(), L"w, ccs=UTF-8") || !f) return;
    for (auto& [key, v] : index) if (v.first) fwprintf(f, L"%u\t%ls\n", v.first, v.second.c_str());
    fclose(f);
}

void LoadIndex() {
    if (loaded) return;
    loaded = true;
    FILE* f = nullptr;
    if (_wfopen_s(&f, AppDataFile(L"etiquetas.txt").c_str(), L"r, ccs=UTF-8") || !f) return;
    wchar_t line[2048];
    while (fgetws(line, 2048, f)) {
        wchar_t* tab = wcschr(line, L'\t');
        if (!tab) continue;
        *tab = 0;
        std::wstring path = tab + 1;
        while (!path.empty() && (path.back() == L'\n' || path.back() == L'\r')) path.pop_back();
        index[Key(path)] = { (unsigned)_wtoi(line), path };
    }
    fclose(f);
}
}  // namespace

unsigned Read(const std::wstring& path) {
    if (!Supported(path)) return 0;
    HANDLE h = CreateFileW((path + L":Arquivos.Etiquetas").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    char buf[32] = {};
    DWORD n = 0;
    ReadFile(h, buf, sizeof(buf) - 1, &n, nullptr);
    CloseHandle(h);
    return (unsigned)strtoul(buf, nullptr, 10) & ((1u << COUNT) - 1);
}

bool Write(const std::wstring& path, unsigned mask) {
    if (!Supported(path)) return false;
    LoadIndex();
    // guarda as datas: escrever no fluxo mexeria na data de modificação do arquivo
    HANDLE main = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    FILETIME c{}, a{}, m{};
    bool haveTimes = main != INVALID_HANDLE_VALUE && GetFileTime(main, &c, &a, &m);
    std::wstring stream = path + L":Arquivos.Etiquetas";
    bool ok;
    if (!mask) {
        ok = DeleteFileW(stream.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
    } else {
        HANDLE h = CreateFileW(stream.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
        ok = h != INVALID_HANDLE_VALUE;
        if (ok) {
            char buf[16];
            int n = sprintf_s(buf, "%u", mask);
            DWORD w = 0;
            ok = WriteFile(h, buf, (DWORD)n, &w, nullptr) != 0;
            CloseHandle(h);
        }
    }
    if (haveTimes) SetFileTime(main, &c, &a, &m);
    if (main != INVALID_HANDLE_VALUE) CloseHandle(main);
    if (!ok) { Log(L"erro: etiqueta em %ls (%lu)", path.c_str(), GetLastError()); return false; }
    if (mask) index[Key(path)] = { mask, path }; else index.erase(Key(path));
    SaveIndex();
    return true;
}

std::vector<std::wstring> Tagged(int tag) {
    LoadIndex();
    std::vector<std::wstring> out;
    bool changed = false;
    for (auto it = index.begin(); it != index.end();) {
        // confere no próprio arquivo: pode ter sido apagado ou perdido a etiqueta fora daqui
        const std::wstring& path = it->second.second;
        unsigned real = GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES ? 0 : Read(path);
        if (real != it->second.first) { changed = true; if (!real) { it = index.erase(it); continue; } it->second.first = real; }
        if (real & (1u << tag)) out.push_back(path);
        ++it;
    }
    if (changed) SaveIndex();
    return out;
}

}  // namespace tags
